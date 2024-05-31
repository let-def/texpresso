/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <signal.h>
#include "engine.h"
#include "incdvi.h"
#include "state.h"
#include "synctex.h"
#include "editor.h"
#include "mupdf_compat.h"

typedef struct
{
  fileentry_t *entry;
  int position;
} fence_t;

typedef struct
{
  fileentry_t *entry;
  int seen, time;
} trace_entry_t;

typedef struct
{
  int pid, fd;
  int trace_len;
  mark_t snap;
} process_t;

struct tex_engine
{
  struct txp_engine_class *_class;

  char *name;
  char *tectonic_path;
  char *inclusion_path;
  filesystem_t *fs;
  state_t st;
  log_t *log;

  channel_t *c;
  process_t processes[32];
  int process_count;

  trace_entry_t *trace;
  int trace_cap;
  fence_t fences[16];
  int fence_pos;
  mark_t restart;

  bundle_server *bundle;
  incdvi_t *dvi;
  synctex_t *stex;

  struct {
    int trace_len, offset, flush;
  } rollback;
};

// Backtrackable process state & VFS representation 
//
//
//
//
//
//
//
//
//
//
//
//

static process_t *get_process(struct tex_engine *t)
{
  if (t->process_count == 0)
    mabort();
  return &t->processes[t->process_count-1];
}

// Useful routines

static char *last_index(char *path, char needle)
{
  char *result = path;
  while (*path)
  {
    if (*path == needle)
      result = path + 1;
    path += 1;
  }
  return result;
}

// tex_engine implementation

TXP_ENGINE_DEF_CLASS;
#define SELF struct tex_engine *self = (struct tex_engine*)_self

static void answer_query(fz_context *ctx, struct tex_engine *self, query_t *q);

// Launching processes

static pid_t exec_xelatex_generic(char **args, int *fd)
{
  int sockets[2];
  if (socketpair(PF_UNIX, SOCK_STREAM, 0, sockets) != 0)
  {
    perror("exec_xelatex socketpair");
    mabort();
  }

  char buf[30];
  snprintf(buf, 30, "%d", sockets[1]);
  setenv("TEXPRESSO_FD", buf, 1);

#ifdef __APPLE__
  static int env_init = 0;
  if (!env_init)
  {
    env_init = 1;
    setenv("OBJC_DISABLE_INITIALIZE_FORK_SAFETY", "YES", 1);
  }

  pid_t pid = fork();
#else
  pid_t pid = vfork();
#endif

  if (pid == -1)
  {
    perror("exec_xelatex vfork");
    mabort();
  }

  if (pid == 0)
  {
    /* CHILD */
    if (close(sockets[0]) != 0)
      mabort();
    // Redirect stdout to stderr
    dup2(STDERR_FILENO, STDOUT_FILENO);
    execvp(args[0], args);
    _exit(2);
  }

  /* PARENT */
  if (close(sockets[1]) != 0)
    mabort();
  *fd = sockets[0];
  return pid;
}

static pid_t exec_xelatex(char *tectonic_path, const char *filename,
                          int bundle_input, int bundle_output, int bundle_lock,
                          int *fd)
{
  char bundle_url[256];
  sprintf(bundle_url, "texpresso-bundle://%d,%d,%d",
          bundle_input, bundle_output, bundle_lock);
  char *args[] = {
    tectonic_path,
    "-X",
    "texpresso",
    "--bundle",
    bundle_url,
    "--untrusted",
    "--synctex",
    "--outfmt",
    "xdv",
    "-Z",
    "continue-on-errors",
    (char*)filename,
    NULL
  };

  pid_t pid = exec_xelatex_generic(args, fd);
  fprintf(stderr, "[process] launched pid %d (using %s)\n", pid, tectonic_path);
  return pid;
}

static void prepare_process(fz_context *ctx, struct tex_engine *self)
{
  if (self->process_count == 0)
  {
    log_rollback(ctx, self->log, self->restart);
    self->process_count = 1;
    process_t *p = get_process(self);
    p->pid = exec_xelatex(self->tectonic_path, self->name,
                          bundle_server_input(self->bundle),
                          bundle_server_output(self->bundle),
                          bundle_server_lock(self->bundle),
                          &p->fd);
    p->trace_len = 0;
    if (!channel_handshake(self->c, p->fd))
      mabort();
  }
}

// Terminating processes

static void close_process(process_t *p)
{
  if (p->fd != -1)
  {
    kill(p->pid, SIGTERM);
    close(p->fd);
    p->fd = -1;
  }
}

static void pop_process(fz_context *ctx, struct tex_engine *self)
{
  process_t *p = get_process(self);
  close_process(p);
  channel_reset(self->c);
  self->process_count -= 1;
  mark_t mark =
    self->process_count > 0 ? get_process(self)->snap : self->restart;
  log_rollback(ctx, self->log, mark);
}

static bool read_query(struct tex_engine *self, channel_t *t, query_t *q)
{
  process_t *p = get_process(self);
  bool result = channel_read_query(t, p->fd, q);
  if (!result)
  {
    fprintf(stderr, "[process] terminating process\n");
    close_process(p);
  }
  return result;
}

static void decimate_processes(struct tex_engine *self)
{
  fprintf(stderr, "before process decimation:\n");
  for  (int i = 0; i < self->process_count; ++i)
  {
    process_t *p = &self->processes[i];
    fprintf(stderr, "- position %d, time %dms [pid %d]\n",
            p->trace_len,
            p->trace_len == 0 ? 0 : self->trace[p->trace_len - 1].time,
            p->pid);
  }

  int i = 0, bound = (self->process_count - 8) / 2;
  while (i < bound)
  {
    close_process(&self->processes[2*i]);
    self->processes[i] = self->processes[2*i+1];
    i++;
  }
  for (int j = bound * 2; j < self->process_count; ++j)
  {
    self->processes[i] = self->processes[j];
    i++;
  }
  self->process_count = i;

  fprintf(stderr, "after process decimation:\n");
  for  (int i = 0; i < self->process_count; ++i)
  {
    process_t *p = &self->processes[i];
    fprintf(stderr, "- position %d, time %dms [pid %d]\n",
            p->trace_len,
            p->trace_len == 0 ? 0 : self->trace[p->trace_len - 1].time,
            p->pid);
  }
}

// Engine class implementation

static void engine_destroy(txp_engine *_self, fz_context *ctx)
{
  SELF;
  while (self->process_count > 0)
    pop_process(ctx, self);
  incdvi_free(ctx, self->dvi);
  synctex_free(ctx, self->stex);
  fz_free(ctx, self->name);
  fz_free(ctx, self->tectonic_path);
  fz_free(ctx, self->inclusion_path);
  fz_free(ctx, self);
}

static const char *expand_path(const char **inclusion_path, const char *name, char buffer[1024])
{
  if (!*inclusion_path || !(*inclusion_path)[0])
    return NULL;

  if (name[0] == '/')
    return NULL;

  if (name[0] == '.' && name[1] == '/')
  {
    name += 2;
    while (*name == '/')
      name += 1;
  }

  char *p = buffer;
  const char *i = *inclusion_path;

  while (*i)
  {
    if (p > buffer + 1024) mabort();
    *p = *i;
    p += 1;
    i += 1;
  }
  *inclusion_path = i+1;

  if (p[-1] != '/')
  {
    if (p > buffer + 1024) mabort();
    p[0] = '/';
    p += 1;
  }

  while (*name)
  {
    if (p > buffer + 1024) mabort();
    *p = *name;
    p += 1;
    name += 1;
  }

  if (p > buffer + 1024) mabort();
  *p = '\0';

  return buffer;
}

static void check_fid(file_id fid)
{
  if (fid < 0 || fid >= MAX_FILES)
    mabort();
}

static void record_seen(struct tex_engine *self, fileentry_t *entry, int seen, int time)
{
  process_t *p = get_process(self);

  if (p->trace_len > 0 && self->trace[p->trace_len-1].entry == entry &&
      (self->process_count <= 1 ||
      self->processes[self->process_count - 2].trace_len != p->trace_len))
  {
    self->trace[p->trace_len-1].time = time;
    entry->seen = seen;
    return;
  }

  if (p->trace_len == self->trace_cap)
  {
    int new_cap = self->trace_cap == 0 ? 8 : self->trace_cap * 2;
    fprintf(stderr, "[info] trace has %d entries, growing to %d\n", self->trace_cap, new_cap);
    trace_entry_t *newtr = calloc(sizeof(trace_entry_t), new_cap);
    if (newtr == NULL) abort();
    if (self->trace)
    {
      memcpy(newtr, self->trace, self->trace_cap * sizeof(trace_entry_t));
      free(self->trace);
    }
    self->trace = newtr;
    self->trace_cap = new_cap;
  }

  self->trace[p->trace_len] = (trace_entry_t){
    .entry = entry,
    .seen = entry->seen,
    .time = time,
  };
  entry->seen = seen;
  p->trace_len += 1;
}

static fz_buffer *entry_data(fileentry_t *e)
{
  if (e->saved.data)
    return e->saved.data;
  if (e->edit_data)
    return e->edit_data;
  return e->fs_data;
}

static fz_buffer *output_data(fileentry_t *e)
{
  if (!e)
    return NULL;
  return e->saved.data;
}

static const char *
lookup_path(struct tex_engine *self, const char *path, char buf[1024], struct stat *st)
{
  struct stat st1;
  if (st == NULL)
    st = &st1;

  const char *fs_path = path;
  const char *inclusion_path = self->inclusion_path;

  do {
    if (stat(fs_path, st) != -1)
      break;
  }
  while ((fs_path = expand_path(&inclusion_path, path, buf)));

  return fs_path;
}

static bool need_snapshot(fz_context *ctx, struct tex_engine *self, int time)
{
  // Fences are pending: don't snapshot now
  if (self->fence_pos != -1)
    return 0;

  int process = self->process_count - 1;

  int last_time;

  if (process > 0) 
  {
    // There is already some snapshot, stop if no new event has been traced
    if (self->processes[process].trace_len == self->processes[process-1].trace_len)
      return 0;
      
    last_time = self->trace[self->processes[process-1].trace_len - 1].time;

    // TODO Alternative
    // Checking that some new event happened avoid entering an infinite fork
    // loop when last event is old, but nothing new is registered, therefore
    // causing an infinite loop.
    // A better solution might be to record a fork as an event.
    // This could be done for instance in READ, when trying to fork again.
  }
  else
  {
    #ifdef __APPLE__
    // Workaround for macOS
    // Due to limitations in the implementation of fork on macOS, it is not
    // possible to load system fonts after fork (without exec). This breaks
    // TeXpresso sooner or later, and there is no obvious solution besides
    // implementing XeTeX snapshotting without fork.
    // The second best thing is to hopefully load all system fonts before the
    // first fork.
    // Therefore we delay forking until output started, hopping that all fonts
    // have been specified at this point.
    if (!incdvi_output_started(self->dvi))
      return 0;
    #endif

    // No snapshot, measure time since root process started
    last_time = 0;
  }

  return time > 500 + last_time;
}

static void answer_query(fz_context *ctx, struct tex_engine *self, query_t *q)
{
  process_t *p = get_process(self);
  answer_t a;
  switch (q->tag)
  {
    case Q_OPEN:
    {
      check_fid(q->open.fid);
      filecell_t *cell = &self->st.table[q->open.fid];
      if (cell->entry != NULL) mabort();

      fileentry_t *e = NULL;

      char fs_path_buffer[1024];
      const char *fs_path = NULL;

      if (q->open.mode[0] == 'r')
      {
        e = filesystem_lookup(self->fs, q->open.path);
        if (!e || !entry_data(e))
        {
          fs_path = lookup_path(self, q->open.path, fs_path_buffer, NULL);
          if (!fs_path)
          {
            e = filesystem_lookup_or_create(ctx, self->fs, q->open.path);
            log_fileentry(ctx, self->log, e);
            record_seen(self, e, INT_MAX, q->time);
            a.tag = A_PASS;
            channel_write_answer(self->c, p->fd, &a);
            break;
          }
        }
      }

      if (!e)
        e = filesystem_lookup_or_create(ctx, self->fs, q->open.path);

      log_filecell(ctx, self->log, cell);
      log_fileentry(ctx, self->log, e);
      cell->entry = e;
      if (e->seen < 0)
        record_seen(self, e, 0, q->time);

      enum accesslevel level =
        (q->open.mode[0] == 'w') ? FILE_WRITE : FILE_READ;

      if (level == FILE_READ)
      {
        if (e->saved.level < FILE_READ)
        {
          if (!fs_path)
            fs_path = lookup_path(self, q->open.path, fs_path_buffer, NULL);
          if (!fs_path)
          {
            if (!e->edit_data)
              mabort("path: %s\nmode:%s\n", q->open.path, q->open.mode);
            e->saved.level = FILE_READ;
            memset(&e->fs_stat, 0, sizeof(e->fs_stat));
          }
          else
          {
            if (fs_path == q->open.path)
              fs_path = e->path;
            e->fs_data = fz_read_file(ctx, fs_path);
            e->saved.level = FILE_READ;
            stat(fs_path, &e->fs_stat);
          }
        }
      }
      else
      {
        e->saved.data = fz_new_buffer(ctx, 1024);
        e->saved.level = level;
      }

      if (level == FILE_READ)
      {
        if (0)
          fprintf(stderr, "[info] opening %s\n", q->open.path);
      }
      else
      {
        fprintf(stderr, "[info] writing %s\n", q->open.path);
        if (strcmp(q->open.path, "stdout") == 0)
        {
          if (self->st.stdout.entry != NULL)
          {
            fprintf(stderr, "[error] two stdouts!\n");
            mabort();
          }
          log_filecell(ctx, self->log, &self->st.stdout);
          self->st.stdout.entry = e;
        }
        else
        {
          char *ext = last_index(q->open.path, '.');
          if (0)
            fprintf(stderr, "extension is %s\n", ext);
          if (!ext);
          else if ((strcmp(ext, "xdv") == 0 ||
                    strcmp(ext, "dvi") == 0 ||
                    strcmp(ext, "pdf") == 0))
          {
            if (self->st.document.entry != NULL)
            {
              fprintf(stderr, "[error] two outputs!\n");
              mabort();
            }
            log_filecell(ctx, self->log, &self->st.document);
            self->st.document.entry = e;
            incdvi_reset(self->dvi);
            fprintf(stderr, "[info] this is the output document\n");
          }
          else if ((strcmp(ext, "synctex") == 0))
          {
            if (self->st.synctex.entry != NULL)
            {
              fprintf(stderr, "[error] two synctex!\n");
              mabort();
            }
            log_filecell(ctx, self->log, &self->st.synctex);
            self->st.synctex.entry = e;
            synctex_rollback(ctx, self->stex, 0);
            fprintf(stderr, "[info] this is the synctex\n");
          }
          else if ((strcmp(ext, "log") == 0))
          {
            if (self->st.log.entry != NULL)
            {
              fprintf(stderr, "[error] two log files!\n");
              mabort();
            }
            log_filecell(ctx, self->log, &self->st.log);
            self->st.log.entry = e;
            fprintf(stderr, "[info] this is the log file\n");
          }
        }
      }

      int n = strlen(q->open.path);
      a.open.size = n;
      a.tag = A_OPEN;
      memmove(channel_get_buffer(self->c, n), q->open.path, n);
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_READ:
    {
      check_fid(q->read.fid);
      fileentry_t *e = self->st.table[q->read.fid].entry;
      if (e == NULL) mabort();
      if (e->saved.level < FILE_READ) mabort();
      fz_buffer *data = entry_data(e);
      if (e->debug_rollback_invalidation > -1)
      {
        if (q->read.pos > e->debug_rollback_invalidation)
          mabort();
        e->debug_rollback_invalidation = -1;
      }
      if (q->read.pos > data->len)
      {
        fprintf(stderr, "read:%d\ndata->len:%d\n", q->read.pos, (int)data->len);
        mabort();
      }
      ssize_t n = q->read.size;
      if (n > data->len - q->read.pos)
        n = data->len - q->read.pos;

      int fork = 0;
      if (self->fence_pos >= 0 &&
          self->fences[self->fence_pos].entry == e &&
          self->fences[self->fence_pos].position < q->read.pos + n)
      {
        if (n < 0)
          mabort();
        n = self->fences[self->fence_pos].position - q->read.pos;
        // Weird that n can be negative at this point?!
        fork = (n == 0);
        if (n < 0)
          mabort("n:%d fence_pos:%d read_pos:%d\n", (int)n, self->fences[self->fence_pos].position, q->read.pos);
      }
      if (fork)
      {
        a.tag = A_FORK;
        self->fence_pos -= 1;
      }
      else if (need_snapshot(ctx, self, q->time))
      {
        a.tag = A_FORK;
      }
      else
      {
        memmove(channel_get_buffer(self->c, n), data->data + q->read.pos, n);
        a.tag = A_READ;
        a.read.size = n;
      }
      if (0)
      {
        if (fork)
          fprintf(stderr, "read = fork\n");
        else
          fprintf(stderr, "read = %d\n", (int)n);
      }
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_WRIT:
    {
      fileentry_t *e = NULL;

      if (q->writ.fid == -1)
      {
        e = self->st.stdout.entry;
        if (e == NULL)
        {
          e = filesystem_lookup_or_create(ctx, self->fs, "stdout");
          log_fileentry(ctx, self->log, e);
          log_filecell(ctx, self->log, &self->st.stdout);
          self->st.stdout.entry = e;
          if (e->saved.data == NULL)
          {
            e->saved.data = fz_new_buffer(ctx, 1024);
            e->saved.level = FILE_WRITE;
          }
        }
        if (q->writ.pos != 0)
          mabort();
        q->writ.pos = e->saved.data->len;
      }
      else
      {
        check_fid(q->writ.fid);
        e = self->st.table[q->writ.fid].entry;
      }

      if (e == NULL || e->saved.level != FILE_WRITE) mabort();
      log_fileentry(ctx, self->log, e);

      if (q->writ.pos + q->writ.size > e->saved.data->len)
      {
        e->saved.data->len = q->writ.pos;
        fz_append_data(ctx, e->saved.data, q->writ.buf, q->writ.size);
      }
      else
        memmove(e->saved.data->data + q->writ.pos, q->writ.buf, q->writ.size);

      if (self->st.document.entry == e)
      {
        int opage = incdvi_page_count(self->dvi);
        incdvi_update(ctx, self->dvi, e->saved.data);
        int npage = incdvi_page_count(self->dvi);
        if (opage != npage)
          fprintf(stderr, "[info] output %d pages long\n", npage);
      }
      else if (self->st.synctex.entry == e)
      {
        int opage = synctex_page_count(self->stex);
        int oinput = synctex_input_count(self->stex);
        synctex_update(ctx, self->stex, e->saved.data);
        int npage = synctex_page_count(self->stex);
        int ninput = synctex_input_count(self->stex);
        if (opage != npage || oinput != ninput)
          fprintf(stderr, "[info] synctex used %d input files, is %d pages long\n", ninput, npage);
      }
      else if (self->st.log.entry == e)
        editor_append(BUF_LOG, output_data(e), q->writ.pos);
      else if (self->st.stdout.entry == e)
        editor_append(BUF_OUT, output_data(e), q->writ.pos);
      a.tag = A_DONE;
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_CLOS:
    {
      check_fid(q->clos.fid);

      filecell_t *cell = &self->st.table[q->clos.fid];
      fileentry_t *e = cell->entry;
      if (e == NULL) mabort();
      log_filecell(ctx, self->log, cell);
      cell->entry = NULL;

      if (0)
        fprintf(stderr, "[info] closing %s\n", e->path);

      if (self->st.stdout.entry == e)
      {
        log_filecell(ctx, self->log, &self->st.stdout);
        self->st.stdout.entry = NULL;
      }

      if (self->st.document.entry == e)
      {
        fprintf(stderr, "[info] finished output\n");
        // log_filecell(ctx, log, &st->document);
        // st->document.entry = NULL;
      }

      if (self->st.log.entry == e)
      {
        log_filecell(ctx, self->log, &self->st.log);
        self->st.log.entry = NULL;
      }

      a.tag = A_DONE;
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_SIZE:
    {
      check_fid(q->clos.fid);
      fileentry_t *e = self->st.table[q->clos.fid].entry;
      if (e == NULL || e->saved.level < FILE_READ) mabort();
      a.tag = A_SIZE;
      a.size.size = entry_data(e)->len;
      if (LOG)
        fprintf(stderr, "SIZE = %d (seen = %d)\n", a.size.size, e->seen);
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_SEEN:
    {
      check_fid(q->seen.fid);
      fileentry_t *e = self->st.table[q->seen.fid].entry;
      if (e == NULL) mabort();
      if (LOG)
        fprintf(stderr, "[info] file %s seen: %d -> %d\n", e->path, e->seen, q->seen.pos);
      if (e->saved.level < FILE_READ) mabort();
      if (self->fence_pos >= 0 &&
          self->fences[self->fence_pos].entry == e &&
          self->fences[self->fence_pos].position < q->seen.pos)
      {
        fprintf(stderr,
                "Seen position invalid wrt fence:\n"
                "  file %s, seen: %d -> %d\n"
                "  fence #%d position: %d\n",
                e->path, e->seen, q->seen.pos,
                self->fence_pos,
                self->fences[self->fence_pos].position);
        mabort();
      }
      if (e->debug_rollback_invalidation != -1 &&
          q->seen.pos >= e->debug_rollback_invalidation)
        mabort();
      if (q->seen.pos <= e->seen)
      {
        // if the same file is reopened, the "new seen position" can be lower
        // if (q->seen.pos < e->seen)
        //   mabort();
      }
      else
      {
        log_fileentry(ctx, self->log, e);
        record_seen(self, e, q->seen.pos, q->time);
      }
      break;
    }
    case Q_GPIC:
    {
      fileentry_t *e = filesystem_lookup(self->fs, q->gpic.path);
      if (e && e->saved.level == FILE_READ && 
          e->pic_cache.type == q->gpic.type &&
          e->pic_cache.page == q->gpic.page)
      {
        a.gpic.bounds[0] = e->pic_cache.bounds[0];
        a.gpic.bounds[1] = e->pic_cache.bounds[1];
        a.gpic.bounds[2] = e->pic_cache.bounds[2];
        a.gpic.bounds[3] = e->pic_cache.bounds[3];
        a.tag = A_GPIC;
      }
      else
        a.tag = A_PASS;
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_SPIC:
    {
      fileentry_t *e = filesystem_lookup(self->fs, q->spic.path);
      if (e && e->saved.level == FILE_READ)
          e->pic_cache = q->spic.cache;
      a.tag = A_DONE;
      channel_write_answer(self->c, p->fd, &a);
      break;
    }

    case Q_CHLD:
    {
      if (self->process_count == 32)
      {
        decimate_processes(self);
        p = get_process(self);
      }
      channel_reset(self->c);
      self->process_count += 1;
      process_t *p2 = get_process(self);
      p->snap = log_snapshot(ctx, self->log);
      p2->fd = q->chld.fd;
      p2->pid = q->chld.pid;
      p2->trace_len = p->trace_len;
      a.tag = A_DONE;
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
  }
}

static int output_length(fileentry_t *entry)
{
  if (!entry || !entry->saved.data)
    return 0;
  else
    return entry->saved.data->len;
}

static void revert_trace(trace_entry_t *te)
{
  te->entry->seen = te->seen;
}

static void rollback_processes(fz_context *ctx, struct tex_engine *self, int reverted, int trace)
{
  fprintf(
    stderr,
    "rolling back to position %d\nbefore rollback: %d bytes of output\n",
    trace,
    output_length(self->st.document.entry)
  );
  if (self->fence_pos < 0)
  {
    fprintf(stderr, "No fences, assuming process finished\n");
    // if (self->process_count > 0)
    //   mabort();
  }

  fprintf(stderr, "Last trace entries:\n");
  for (int i = get_process(self)->trace_len - 1, j = fz_maxi(i - 10, 0); i > j; i--)
  {
    fprintf(stderr, "- %s@%d, %dms\n",
            self->trace[i].entry->path,
            self->trace[i].seen,
            self->trace[i].time);
  }

  fprintf(stderr, "Snapshots:\n");
  for  (int i = 0; i < self->process_count; ++i)
  {
    process_t *p = &self->processes[i];
    fprintf(stderr, "- position %d, time %dms\n", p->trace_len,
            p->trace_len == 0 ? 0 : self->trace[p->trace_len - 1].time);
  }

  while (self->process_count > 0 && get_process(self)->trace_len > trace)
    pop_process(ctx, self);

  int trace_len = self->process_count == 0 ? 0 : get_process(self)->trace_len;
  while (reverted > trace_len)
  {
    reverted--;
    revert_trace(&self->trace[reverted]);
  }

  fprintf(stderr, "after rollback: %d bytes of output\n",
    self->st.document.entry
    ? (int)self->st.document.entry->saved.data->len
    : 0
  );

  if (self->st.document.entry)
  {
    fprintf(stderr, "[info] before rollback: %d pages\n", incdvi_page_count(self->dvi));
    incdvi_update(ctx, self->dvi, self->st.document.entry->saved.data);
    fprintf(stderr, "[info] after  rollback: %d pages\n", incdvi_page_count(self->dvi));
  }
  else
    incdvi_reset(self->dvi);
  if (self->st.synctex.entry)
  {
    fprintf(stderr, "[info] before rollback: %d pages in synctex\n", synctex_page_count(self->stex));
    synctex_update(ctx, self->stex, self->st.synctex.entry->saved.data);
    fprintf(stderr, "[info] after  rollback: %d pages in synctex\n", synctex_page_count(self->stex));
  }
  else
    synctex_rollback(ctx, self->stex, 0);
  editor_truncate(BUF_OUT, output_data(self->st.stdout.entry));
  editor_truncate(BUF_LOG, output_data(self->st.log.entry));
}

static bool possible_fence(trace_entry_t *te)
{
  if (te->seen == INT_MAX || te->seen == -1)
    return 0;
  if (te->entry->saved.level > FILE_READ)
    return 0;
  return 1;
}

static int compute_fences(fz_context *ctx, struct tex_engine *self, int trace, int offset)
{
  self->fence_pos = -1;

  if (trace <= 0)
    return trace;

  if (get_process(self)->trace_len <= trace)
    mabort();

  self->fence_pos = 0;

  offset = (offset - 64) & ~(64 - 1);
  if (offset < self->trace[trace].seen)
    offset = self->trace[trace].seen;
  if (offset == -1)
    offset = 0;

  self->fences[0].entry = self->trace[trace].entry;
  self->fences[0].position = offset;

  int process = self->process_count - 1;
  int delta = 50;
  int time = self->trace[trace].time - 10;

  fprintf(stderr,
          "[fence] placing fence %d at trace position %d, file %s, offset %d\n",
          self->fence_pos, trace, self->fences[self->fence_pos].entry->path,
          self->fences[self->fence_pos].position);

  int target_process = self->process_count - 1;
  while (target_process >= 0 && self->processes[target_process].trace_len > trace)
    target_process -= 1;
  int target_trace = target_process >= 0 ? self->processes[target_process].trace_len : -1;
  while (trace > target_trace && self->fence_pos < 15)
  {
    if (self->trace[trace].time <= time && possible_fence(&self->trace[trace]))
    {
      self->fence_pos += 1;
      self->fences[self->fence_pos].entry = self->trace[trace].entry;
      self->fences[self->fence_pos].position = self->trace[trace].seen;
      if (self->fences[self->fence_pos].position == -1)
        self->fences[self->fence_pos].position = 0;
      time -= delta;
      delta *= 2;
      fprintf(stderr, "[fence] placing fence %d at trace position %d, file %s, offset %d\n",
              self->fence_pos, trace,
              self->fences[self->fence_pos].entry->path,
              self->fences[self->fence_pos].position);
    }
    trace -= 1;
  }

  return trace;
}

static int engine_page_count(txp_engine *_self)
{
  SELF;
  return incdvi_page_count(self->dvi);
}

static fz_display_list *engine_render_page(txp_engine *_self, fz_context *ctx, int page)
{
  SELF;

  float pw, ph;
  bool landscape;
  fz_buffer *data = self->st.document.entry->saved.data;
  incdvi_page_dim(self->dvi, data, page, &pw, &ph, &landscape);

  fz_rect box = fz_make_rect(0, 0, pw, ph);
  fz_display_list *dl = fz_new_display_list(ctx, box);
  fz_device *dev = fz_new_list_device(ctx, dl);
  incdvi_render_page(ctx, self->dvi, data, page, dev);
  fz_close_device(ctx, dev);
  fz_drop_device(ctx, dev);
  return dl;
}

static bool engine_step(txp_engine *_self, fz_context *ctx, bool restart_if_needed)
{
  SELF;
  if (restart_if_needed)
    prepare_process(ctx, self);

  if (engine_get_status(_self) == DOC_RUNNING)
  {
    query_t q;
    int fd = get_process(self)->fd;
    if (fd == -1)
      return 0;
    if (!channel_has_pending_query(self->c, fd, 10))
      return 0;
    if (!read_query(self, self->c, &q))
    {
      close(fd);
      get_process(self)->fd = -1;
      return 0;
    }
    answer_query(ctx, self, &q);
    channel_flush(self->c, fd);
    return 1;
  }

  return 0;
}

static int scan_entry(fz_context *ctx, struct tex_engine *self, fileentry_t *e)
{
  if (e->saved.level < FILE_READ || e->fs_stat.st_ino == 0 || e->edit_data)
    return -1;

  struct stat st;

  fprintf(stderr, "[scan] scanning %s\n", e->path);

  const char *inclusion_path = self->inclusion_path;
  char fs_path_buffer[1024];
  const char *fs_path = lookup_path(self, e->path, fs_path_buffer, &st);

  if (!fs_path)
  {
      fprintf(stderr, "[scan] file removed\n");
      return -1;
  }

  if (stat_same(&st, &e->fs_stat))
    return -1;

  e->fs_stat = st;
  fprintf(stderr, "[scan] file %s has changed\n", e->path);

  fz_buffer *buf;
  fz_var(buf);

  fz_try(ctx)
  {
    buf = fz_read_file(ctx, fs_path);
  }
  fz_catch(ctx)
  {
    return -1;
  }

  e->pic_cache.type = -1;

  int olen = e->fs_data->len, nlen = buf->len;
  int len = olen < nlen ? olen : nlen;

  int i = 0;
  while (i < len && e->fs_data->data[i] == buf->data[i])
    i += 1;

  if (i != len)
    fprintf(stderr, "[scan] first changed byte is %d\n", i);
  else if (olen == nlen)
  {
    fprintf(stderr, "[scan] but content has not changed\n");
    fz_drop_buffer(ctx, buf);
    return -1;
  }
  else if (olen < nlen)
    fprintf(stderr, "[scan] content has grown from %d to %d bytes\n", olen, nlen);
  else
    fprintf(stderr, "[scan] content was shrinked from %d to %d bytes\n", olen, nlen);

  fz_drop_buffer(ctx, e->fs_data);
  e->fs_data = buf;

  return i;
}

#define NOT_IN_TRANSACTION (-2)

static void rollback_begin(fz_context *ctx, struct tex_engine *self)
{
  // Check if already in a transaction
  if (self->rollback.trace_len != NOT_IN_TRANSACTION)
    abort();

  self->rollback.trace_len = get_process(self)->trace_len;
  self->rollback.offset = -1;
  self->rollback.flush = 0;
}

static bool rollback_end(fz_context *ctx, struct tex_engine *self, int *tracep, int *offsetp)
{
  int trace_len = self->rollback.trace_len;
  self->rollback.trace_len = NOT_IN_TRANSACTION;

  // Assert we are in a transaction
  if (trace_len == NOT_IN_TRANSACTION)
    abort();

  process_t *p = get_process(self);

  // Check if nothing changed
  if (trace_len == p->trace_len)
  {
    if (!self->rollback.flush)
      return false;
    if (p->fd > -1)
    {
      ask_t a;
      a.tag = C_FLSH;
      channel_write_ask(self->c, p->fd, &a);
      channel_flush(self->c, p->fd);
      return false;
    }
    trace_len -= 1;
    if (trace_len > 0)
      self->rollback.offset = self->trace[trace_len].seen;
  }

  fprintf(stderr, "[change] rewinded trace from %d to %d entries\n",
          get_process(self)->trace_len, trace_len);

  if (tracep)
    *tracep = trace_len;
  if (offsetp)
    *offsetp = self->rollback.offset;

  return true;
}

// Return false if some contents had not been observed: caller should recheck
// for changed contents.
// Return true otherwise (process is ready to be flushed).
static bool process_pending_messages(fz_context *ctx, struct tex_engine *self)
{
  // If the process is marked ready to flush, seen messages have already been
  // consumed
  if (self->rollback.flush)
    return 1;

  process_t *p = get_process(self);

  // If process is dead, nothing has been missed
  if (p->fd == -1)
    return 1;

  // Synchronize with the child process:
  // - kill if stuck
  // - check pending SEEN messages to update vision of the process
  int nothing_seen = 1;
  do {
    if (!channel_has_pending_query(self->c, p->fd, 10))
    {
      fprintf(stderr, "[kill] worker might be stuck, killing\n");
      // The process hasn't answered in 10ms
      // It might be stuck in long computation or a loop, kill it to start from the previous one.
      close_process(p);
      break;
    }
    // Process only pending SEEN to have an updated view on process state
    switch (channel_peek_query(self->c, p->fd))
    {
      case Q_SEEN:
        {
          query_t q;
          if (!read_query(self, self->c, &q))
          {
            close(p->fd);
            p->fd = -1;
            break;
          }
          answer_query(ctx, self, &q);
          nothing_seen = 0;
          continue;
        }
      default:
        break;
    }
  } while(0);

  self->rollback.flush = 1;
  return nothing_seen;
}

static void rollback_add_change(fz_context *ctx, struct tex_engine *self, fileentry_t *e, int changed)
{
  int trace_len = self->rollback.trace_len;
  // if (changed > 0) changed--;

  // Assert we are in a transaction
  if (trace_len == NOT_IN_TRANSACTION)
    mabort();

  if (e->seen < changed)
  {
    if (process_pending_messages(ctx, self))
      return;

    // A pending message might have updated e->seen
    if (e->seen < changed)
      return;
  }

  while (e->seen >= changed)
  {
    trace_len--;
    revert_trace(&self->trace[trace_len]);
  }

  if (self->trace[trace_len].entry != e)
  {
    fprintf(stderr, "Rollback position: %d. Entries: %d. Seen: %d. Changed: %d. Last trace entries:\n", trace_len, get_process(self)->trace_len, e->seen, changed);
    for (int i = get_process(self)->trace_len - 1, j = fz_maxi(i - 10, 0); i > j; i--)
    {
      fprintf(stderr, "- %s@%d, %dms\n",
              self->trace[i].entry->path,
              self->trace[i].seen,
              self->trace[i].time);
    }
    mabort();
  }

  self->rollback.trace_len = trace_len;
  self->rollback.offset = changed;
}

static void engine_notify_file_changes(txp_engine *_self,
                                       fz_context *ctx,
                                       fileentry_t *entry,
                                       int offset)
{
  SELF;
  rollback_add_change(ctx, self, entry, offset);
}

static void engine_begin_changes(txp_engine *_self, fz_context *ctx)
{
  SELF;
  rollback_begin(ctx, self);
}

static void engine_detect_changes(txp_engine *_self, fz_context *ctx)
{
  SELF;

  fileentry_t *e;
  for (int index = 0; (e = filesystem_scan(self->fs, &index));)
  {
    int changed = scan_entry(ctx, self, e);
    if (changed > -1)
      rollback_add_change(ctx, self, e, changed);
  }
}

static bool engine_end_changes(txp_engine *_self, fz_context *ctx)
{
  SELF;
  int reverted, trace, offset;

  if (!rollback_end(ctx, self, &reverted, &offset))
    return false;

  trace = reverted >= 0 ? compute_fences(ctx, self, reverted, offset) : 0;
  rollback_processes(ctx, self, reverted, trace);

  return true;
}

static txp_engine_status engine_get_status(txp_engine *_self)
{
  SELF;
  if (self->process_count == 0)
    return DOC_TERMINATED;
  return get_process(self)->fd > -1 ? DOC_RUNNING : DOC_TERMINATED;
}

static float engine_scale_factor(txp_engine *_self)
{
  SELF;
  return incdvi_tex_scale_factor(self->dvi);
}

static synctex_t *engine_synctex(txp_engine *_self, fz_buffer **buf)
{
  SELF;
  if (buf)
    *buf = self->st.synctex.entry ? entry_data(self->st.synctex.entry) : NULL;
  return self->stex;
}

static fileentry_t *engine_find_file(txp_engine *_self, fz_context *ctx, const char *path)
{
  SELF;
  return filesystem_lookup_or_create(ctx, self->fs, path);
}

txp_engine *txp_create_tex_engine(fz_context *ctx,
                                  const char *tectonic_path,
                                  const char *inclusion_path,
                                  const char *tex_dir,
                                  const char *tex_name)
{
  struct tex_engine *self = fz_malloc_struct(ctx, struct tex_engine);
  self->_class = &_class;

  self->name = fz_strdup(ctx, tex_name);
  self->tectonic_path = fz_strdup(ctx, tectonic_path);
  self->inclusion_path = fz_strdup(ctx, inclusion_path ? inclusion_path : "");
  state_init(&self->st);
  self->fs = filesystem_new(ctx);
  self->log = log_new(ctx);
  self->trace = NULL;
  self->trace_cap = 0;
  self->fence_pos = -1;
  self->restart = log_snapshot(ctx, self->log);
  self->c = channel_new();
  self->process_count = 0;

  self->bundle = bundle_server_start(ctx, tectonic_path, tex_dir);
  self->dvi = incdvi_new(ctx, bundle_server_hooks(self->bundle));

  self->stex = synctex_new(ctx);
  self->rollback.trace_len = NOT_IN_TRANSACTION;

  signal(SIGCHLD, SIG_IGN);
  return (txp_engine*)self;
}
