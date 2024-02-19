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

typedef struct
{
  fileentry_t *entry;
  int position;
} fence_t;

typedef struct
{
  fileentry_t *entry;
  int seen_before, seen_after, time;
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

  incdvi_t *dvi;
  synctex_t *stex;

  struct {
    fileentry_t *changed;
    int trace;
    int offset; // Last valid offset in trace entry
    int flush;
  } rollback;
};

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

static int answer_query(fz_context *ctx, struct tex_engine *self, query_t *q);

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
    /* Redirect stdout and stderr to null, output will be handled by intexcept */
    // int fd;
    // fd = open("/dev/null", O_WRONLY);
    // dup2(fd, STDOUT_FILENO);
    // close(fd);
    // fd = open("/dev/null", O_WRONLY);
    // dup2(fd, STDERR_FILENO);
    // close(fd);
    execvp(args[0], args);
    _exit(2);
  }

  /* PARENT */
  if (close(sockets[1]) != 0)
    mabort();
  *fd = sockets[0];
  return pid;
}

static pid_t exec_xelatex(char *tectonic_path, const char *filename, int *fd)
{
  char *args[] = {
    tectonic_path,
    "-X",
    "texpresso",
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
    p->pid = exec_xelatex(self->tectonic_path, self->name, &p->fd);
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
  close_process(get_process(self));
  channel_reset(self->c);
  self->process_count -= 1;
  if (self->process_count > 0)
    log_rollback(ctx, self->log, get_process(self)->snap);
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

static void record_trace(struct tex_engine *self, fileentry_t *entry, int seen, int time)
{
  process_t *p = get_process(self);

  if (p->trace_len > 0 && self->trace[p->trace_len-1].entry == entry &&
      (self->process_count <= 1 ||
      self->processes[self->process_count - 2].trace_len != p->trace_len))
  {
    self->trace[p->trace_len-1].seen_after = seen;
    self->trace[p->trace_len-1].time = time;
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
    .seen_before = entry->saved.seen,
    .seen_after = seen,
    .time = time,
  };
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

static int answer_query(fz_context *ctx, struct tex_engine *self, query_t *q)
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

          if (q->open.mode[1] == '?' && !fs_path)
          {
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

      enum accesslevel level =
        (q->open.mode[0] == 'w') ? FILE_WRITE : FILE_READ;

      if (level == FILE_READ)
      {
        if (e->saved.level < FILE_READ)
        {
          if (!fs_path)
            fs_path = lookup_path(self, q->open.path, fs_path_buffer, NULL);
          if (!fs_path)
            mabort("path: %s\nmode:%s\n", q->open.path, q->open.mode);
          if (fs_path == q->open.path)
            fs_path = e->path;
          e->fs_data = fz_read_file(ctx, fs_path);
          e->saved.level = FILE_READ;
          stat(fs_path, &e->fs_stat);
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
      if (e->rollback.invalidated > -1)
      {
        if (q->read.pos > e->rollback.invalidated)
          mabort();
        e->rollback.invalidated = -1;
      }
      if (q->read.pos > data->len)
      {
        fprintf(stderr, "read:%d\ndata->len:%d\n", q->read.pos, (int)data->len);
        mabort();
      }
      size_t n = q->read.size;
      if (n > data->len - q->read.pos)
        n = data->len - q->read.pos;

      int fork = 0;
      if (self->fence_pos >= 0 &&
          self->fences[self->fence_pos].entry == e &&
          self->fences[self->fence_pos].position < q->read.pos + n)
      {
        if (n < 0)
          abort();
        n = self->fences[self->fence_pos].position - q->read.pos;
        fork = (n == 0);
      }
      if (fork)
      {
        a.tag = A_FORK;
        self->fence_pos -= 1;
      }
      else if (self->fence_pos == -1 && q->time > 
          500 + (self->process_count <= 1 ? 0
                  : self->trace[self->processes[self->process_count - 2].trace_len - 1].time))
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
        fprintf(stderr, "SIZE = %d (seen = %d)\n", a.size.size, e->saved.seen);
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_SEEN:
    {
      check_fid(q->seen.fid);
      fileentry_t *e = self->st.table[q->seen.fid].entry;
      if (e == NULL) mabort();
      if (LOG)
        fprintf(stderr, "[info] file %s seen: %d -> %d\n", e->path, e->saved.seen, q->seen.pos);
      if (e->saved.level < FILE_READ) mabort();
      if (self->fence_pos >= 0 &&
          self->fences[self->fence_pos].entry == e &&
          self->fences[self->fence_pos].position < q->seen.pos)
      {
        fprintf(stderr,
                "Seen position invalid wrt fence:\n"
                "  file %s, seen: %d -> %d\n"
                "  fence #%d position: %d\n",
                e->path, e->saved.seen, q->seen.pos,
                self->fence_pos,
                self->fences[self->fence_pos].position);
        mabort();
      }
      if (e->rollback.invalidated != -1 && q->seen.pos >= e->rollback.invalidated)
          return -1;
      if (q->seen.pos <= e->saved.seen)
      {
        // if the same file is reopened, the "new seen position" can be lower
        // if (q->seen.pos < e->seen)
        //   mabort();
      }
      else
      {
        log_fileentry(ctx, self->log, e);
        record_trace(self, e, q->seen.pos, q->time);
        e->saved.seen = q->seen.pos;
      }
      break;
    }
    case Q_ACCS:
    {
      fileentry_t *e = filesystem_lookup(self->fs, q->accs.path);
      enum accs_answer f;
      if (e && e->saved.level == FILE_WRITE)
        f = ACCS_OK;
      else
      {
        int mode = 0;
        if (0)
          fprintf(stderr, "[info] access %s\n", q->accs.path);
        if (q->accs.flags & ACCS_R) mode |= R_OK;
        if (q->accs.flags & ACCS_W) mode |= W_OK;
        if (q->accs.flags & ACCS_X) mode |= X_OK;
        if (q->accs.flags & ACCS_F) mode |= F_OK;

        int r = access(q->accs.path, mode);
        if (r == 0)
          f = ACCS_OK;
        else if (errno == ENOENT)
          f = ACCS_ENOENT;
        else if (errno == EACCES)
          f = ACCS_EACCES;
        else
        {
          perror("Q_ACCS access");
          f = ACCS_EACCES;
        }
      }
      a.tag = A_ACCS;
      a.accs.flag = f;
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_STAT:
    {
      fileentry_t *e = filesystem_lookup(self->fs, q->accs.path);
      enum accs_answer f;
      if (e && e->saved.level == FILE_WRITE)
      {
        f = ACCS_OK;
        struct stat_answer *sa = &a.stat.stat;
        sa->dev = 0;
        sa->ino = 0;
        sa->mode = 0644 | S_IFREG;
        sa->nlink = 1;
        sa->uid = 1000;
        sa->gid = 0;
        sa->rdev = 0;
        sa->size = e->saved.data->len;
        sa->blksize = 4096;
        sa->blocks = (e->saved.data->len + 4095) / 4096;
        sa->atime.sec  = 0;
        sa->atime.nsec = 0;
        sa->ctime.sec  = 0;
        sa->ctime.nsec = 0;
        sa->mtime.sec  = 0;
        sa->mtime.nsec = 0;
      }
      else
        f = ACCS_PASS;
      a.tag = A_STAT;
      if (0)
        fprintf(stderr, "[info] stat %s: %s\n", q->stat.path,
            (f == ACCS_OK) ? "ACCS_OK" :
            (f == ACCS_ENOENT) ? "ACCS_ENOENT" :
            (f == ACCS_EACCES) ? "ACCS_EACCES" :
            "ACCS_PASS"
            );
      a.stat.flag = f;
      channel_write_answer(self->c, p->fd, &a);
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
  return 1;
}

static int output_length(fileentry_t *entry)
{
  if (!entry || !entry->saved.data)
    return 0;
  else
    return entry->saved.data->len;
}

static void rollback(fz_context *ctx, struct tex_engine *self, int trace)
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
    if (self->process_count > 0)
      mabort();
  }

  fprintf(stderr, "Last trace entries:\n");
  for (int i = get_process(self)->trace_len - 1, j = 10; i > 0 && j > 0; i--, j--)
  {
    fprintf(stderr, "- %d->%d, %s, %dms\n",
            self->trace[i].seen_before,
            self->trace[i].seen_after,
            self->trace[i].entry->path,
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

  if (self->process_count == 0)
    log_rollback(ctx, self->log, self->restart);

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

static int compute_fences(fz_context *ctx, struct tex_engine *self, int trace, int offset)
{
  self->fence_pos = -1;

  if (trace < 0)
    return trace;

  if (get_process(self)->trace_len <= trace)
    mabort();

  self->fence_pos = 0;

  offset = (offset - 64) & ~(64 - 1);
  if (offset < self->trace[trace].seen_before)
    offset = self->trace[trace].seen_before;

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
    if (self->trace[trace].time <= time)
    {
      self->fence_pos += 1;
      self->fences[self->fence_pos].entry = self->trace[trace].entry;
      self->fences[self->fence_pos].position = self->trace[trace].seen_before;
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
    if (!channel_has_pending_query(self->c, fd, 10))
      return 1;
    if (!read_query(self, self->c, &q))
      return 0;
    int result = answer_query(ctx, self, &q);
    channel_flush(self->c, fd);
    if (result == -1)
    {
      pop_process(ctx, self);
      return 1;
    }
    return result;
  }
  return 0;
}

static int scan_entry(fz_context *ctx, struct tex_engine *self, fileentry_t *e)
{
  if (e->saved.level != FILE_READ || e->edit_data)
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
  if (self->rollback.trace != NOT_IN_TRANSACTION)
    abort();

  // Invariant: nothing changed outside of a transaction
  if (self->rollback.changed != NULL)
    abort();

  self->rollback.trace = get_process(self)->trace_len - 1;
  self->rollback.flush = 0;
}

static bool rollback_end(fz_context *ctx, struct tex_engine *self, int *tracep, int *offsetp)
{
  int trace = self->rollback.trace;
  int need_flush = self->rollback.flush;
  self->rollback.flush = 0;

  // Assert we are in a transaction
  if (trace == NOT_IN_TRANSACTION)
    abort();

  // Check if nothing changed
  if (self->rollback.changed == NULL)
  {
    process_t *p = get_process(self);
    // Invariant: if nothing changed, the entire trace should be valid
    if (trace != p->trace_len - 1)
      abort();
    self->rollback.trace = NOT_IN_TRANSACTION;
    self->rollback.offset = -1;

    if (need_flush)
    {
      ask_t a;
      a.tag = C_FLSH;
      channel_write_ask(self->c, p->fd, &a);
      channel_flush(self->c, p->fd);
    }

    return false;
  }

  fprintf(stderr, "[change] rewinded trace from %d to %d entries\n",
          get_process(self)->trace_len, trace + 1);

  if (0)
    for (int i = trace; i >= 0; i--)
    {
      fprintf(stderr, "%s %s @ %d -> %d, %d ms\n", i == trace ? "=>" : "  ",
              self->trace[i].entry->path, self->trace[i].seen_before,
              self->trace[i].seen_after, self->trace[i].time);
    }

  if (tracep)
    *tracep = trace;
  if (offsetp)
    *offsetp = self->rollback.offset;

  self->rollback.trace = NOT_IN_TRANSACTION;
  self->rollback.offset = -1;

  fileentry_t *changed = self->rollback.changed;
  self->rollback.changed = NULL;

  while (changed)
  {
    fileentry_t *next = changed->rollback.next;
    changed->rollback.next = NULL;
    changed->rollback.cursor = -1;
    changed->rollback.invalidated = -1;
    changed = next;
  }

  return true;
}

static void rollback_add_change(fz_context *ctx, struct tex_engine *self, fileentry_t *e, int changed)
{
  int trace = self->rollback.trace;

  // Assert we are in a transaction
  if (trace == NOT_IN_TRANSACTION)
    abort();

  if ((e->rollback.cursor == -1) ? (e->saved.seen < changed)
                                 : (e->rollback.cursor < changed))
  {
    self->rollback.flush = 1;
    if (e->rollback.invalidated == -1 ||
        e->rollback.invalidated > changed)
      e->rollback.invalidated = changed;
    return;
  }

  while (trace >= 0)
  {
    trace_entry_t *te = &self->trace[trace];
    if (te->entry->rollback.cursor == -1)
    {
      if (te->entry->rollback.next != NULL)
        abort();
      if (te->entry->saved.seen != te->seen_after)
        abort();
      te->entry->rollback.next = self->rollback.changed;
      self->rollback.changed = te->entry;
    }
    te->entry->rollback.cursor = te->seen_before;
    if (te->entry == e && te->seen_before <= changed)
    {
      self->rollback.trace = trace;
      self->rollback.offset = changed;
      return;
    }
    trace -= 1;
  }

  self->rollback.trace = -1;
  self->rollback.offset = -1;
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
  int trace, offset;

  if (!rollback_end(ctx, self, &trace, &offset))
    return false;

  if (trace >= 0)
    trace = compute_fences(ctx, self, trace, offset);
  rollback(ctx, self, trace);

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

  self->dvi = incdvi_new(ctx, tectonic_path, tex_dir);

  self->stex = synctex_new(ctx);
  self->rollback.changed = NULL;
  self->rollback.trace = NOT_IN_TRANSACTION;
  self->rollback.offset = -1;

  signal(SIGCHLD, SIG_IGN);
  return (txp_engine*)self;
}
