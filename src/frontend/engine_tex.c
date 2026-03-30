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

/**
 * @file engine_tex.c
 * @brief TeX incremental compilation engine implementation
 *
 * This module implements a TeX compiler engine that supports incremental
 * compilation with checkpointing and process forking. It uses a custom
 * protocol to communicate with a TeX compiler process (either TeXLive or
 * tectonic) and maintains a multi-branch history for efficient rollback.
 *
 * The engine implements a sophisticated rollback mechanism that allows it to
 * revert to any previous checkpoint and restart compilation from that point.
 * This is essential for handling source file changes during interactive
 * editing.
 *
 * @note The engine maintains up to 32 concurrent processes to support forking
 *       behavior, which enablesefficient incremental recompilation without
 *       starting from scratch after each file change.
 */

#include <fcntl.h>
#include <limits.h>
#include <mupdf/fitz/buffer.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "editor.h"
#include "engine.h"
#include "incdvi.h"
#include "mydvi.h"
#include "state.h"
#include "synctex.h"

/**
 * @brief Track a file position for fence placement
 *
 * Fences are markers that indicate safe points for rollback. When the engine
 * needs to rollback due to a source file change, it can restart from the
 * position indicated by a fence rather than from the beginning.
 */
typedef struct
{
  FileEntry *entry; /**< File being tracked */
  int position;       /**< Byte offset position */
} Fence;

/**
 * @brief Record a file access in the trace
 *
 * The trace is a log of all file accesses (reads, writes, opens) performed
 * by the TeX process. It's used for rollback and for detecting which files
 * were affected by changes.
 */
typedef struct
{
  FileEntry *entry; /**< File that was accessed */
  int seen;           /**< Last "seen" position (where TeX stopped reading) */
  int time;           /**< Timestamp in milliseconds when access occurred */
} TraceEntry;

/**
 * @brief Represents a running TeX compilation process
 *
 * The engine maintains multiple processes (up to 32) to support forking
 * behavior. Each process maintains its own view of the filesystem and
 * can be rolled back independently.
 */
typedef struct
{
  int pid;       /**< Process ID */
  int fd;        /**< File descriptor for communication with process */
  int trace_len; /**< Number of trace entries from this process */
  LogMark snap;   /**< Log snapshot position for rollback */
} TexProcess;

/**
 * @brief TeX engine instance structure
 *
 * This structure contains all state needed for the incremental TeX compilation
 * engine, including the filesystem state, process management, and output data.
 */
struct TexEngine
{
  struct EngineClass *_class; /**< Virtual method table */

  char *name;           /**< Main .tex file name */
  char *engine_path;    /**< Path to xetex/platex executable */
  char *inclusion_path; /**< Additional search paths for includes */
  bool use_texlive;
  bool stream_mode;

  FileSystem *fs; /**< Filesystem state tracking */
  TexState st;    /**< File open state (file handles) */
  Log *log;       /**< Rollback log for state changes */

  IOChannel *c;             /**< Communication channel with child */
  TexProcess processes[32]; /**< Active processes (forked branches) */
  int process_count;        /**< Number of active processes */

  /**
   * @brief Trace of all file accesses during compilation
   *
   * This array records every file access (read, write, open) in the order
   * they occurred. It enables precise rollback to any previous state.
   */
  TraceEntry *trace;
  int trace_cap; /**< Allocated capacity of trace array */

  /**
   * @brief Fences for incremental recompilation
   *
   * Fences mark positions in the trace where it's safe to restart
   * compilation after a file change. They enable incremental recompilation
   * by avoiding re-reading files that haven't changed.
   */
  Fence fences[16];
  int fence_pos; /**< Current fence index (-1 = no fences) */

  /**
   * @brief Starting point for new processes
   *
   * When a new process is spawned (through forking), it starts from this
   * checkpoint. The process is initialized to match the state at this mark.
   */
  LogMark restart;

  IncDVI *dvi;      /**< DVI rendering state */
  TexSynctex *stex; /**< Synctex data parser */

  /**
   * @brief Current rollback transaction
   *
   * During begin_changes/end_changes, this tracks the rollback position.
   * Uses NOT_IN_TRANSACTION when no transaction is active.
   */
  struct
  {
    int trace_len; /**< Trace position to rollback to */
    int offset;    /**< Byte offset of file change */
    int flush;     /**< If set, flush child without rolling back */
  } rollback;
};

// Backtrackable process state & VFS representation

/**
 * @brief Get the currently active process
 * @param t Engine instance
 * @return Pointer to current process structure
 *
 * This function always returns the most recently added process.
 * It's a convenience accessor that abstracts the process array indexing.
 * @pre process_count > 0
 */
static TexProcess *get_process(struct TexEngine *t)
{
  if (t->process_count == 0)
    mabort();
  return &t->processes[t->process_count - 1];
}

// Useful routines

/**
 * @brief Extract directory path from a full path
 * @param path Null-terminated path string
 * @param needle Character to search for (typically '/')
 * @return Pointer to first character after the last occurrence of needle
 *
 * This is a simple implementation of basename functionality.
 * It scans from the beginning, updating the result pointer each time
 * the needle character is found, so the final result points to the
 * last component of the path.
 */
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

// TexEngine implementation

TXP_ENGINE_DEF_CLASS;
#define SELF struct TexEngine *self = (struct TexEngine *)_self

/**
 * @brief Process a query from the TeX child process
 * @param ctx MuPDF context
 * @param self Engine instance
 * @param q parsed query structure
 *
 * This is the main query handler for the TeX protocol. It processes
 * various query types (OPEN, READ, WRITE, SEEN, etc.) and sends back
 * appropriate answers. It also manages the filesystem state and
 * tracks file accesses in the trace.
 */
static void answer_query(fz_context *ctx, struct TexEngine *self, ProtocolQuery *q);

// Launching processes

/**
 * @brief Fork and exec a TeX engine process
 * @param args Array of command-line arguments (must be NULL-terminated)
 * @param fd Pointer to store the communication file descriptor
 * @return Child process PID
 *
 * This function creates a Unix domain socket pair for communication with
 * the child process. It handles the differences between fork() on macOS
 * (which needs special initialization) and vfork() on other platforms.
 * @note The child process sets TEXPRESSO_FD environment variable to the
 *       socket file descriptor before exec'ing the TeX engine.
 */
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

/**
 * @brief Launch a TeX engine with appropriate arguments
 * @param engine_path Path to xetex or platex executable
 * @param use_texlive If true, use teTeX command-line interface
 * @param filename Input TeX file to compile
 * @param fd Pointer to store communication file descriptor
 * @return Child process PID
 *
 * This constructs the appropriate argument list based on whether we're
 * using TeXLive or tectonic, then calls exec_xelatex_generic().
 */
static pid_t exec_xelatex(char *engine_path,
                          bool use_texlive,
                          const char *filename,
                          int *fd)
{
  char *args[] = {engine_path, (use_texlive ? "-texlive" : "-tectonic"),
                  "-texpresso", (char *)filename, NULL};

  pid_t pid = exec_xelatex_generic(args, fd);
  fprintf(stderr, "[process] launched pid %d (using %s)\n", pid, engine_path);
  return pid;
}

/**
 * @brief Ensure a TeX process is running
 * @param ctx MuPDF context
 * @param self Engine instance
 *
 * This function is called at the start of each step() iteration when
 * restart_if_needed is true. If no processes are running, it launches
 * a new xetex process and performs the protocol handshake.
 */
static void prepare_process(fz_context *ctx, struct TexEngine *self)
{
  if (self->process_count == 0)
  {
    log_rollback(ctx, self->log, self->restart);
    self->process_count = 1;
    TexProcess *p = get_process(self);
    p->pid =
        exec_xelatex(self->engine_path, self->use_texlive, self->name, &p->fd);
    p->trace_len = 0;
    if (!channel_handshake(self->c, p->fd))
      mabort();
  }
}

// Terminating processes

/**
 * @brief Terminate a process and close its file descriptor
 * @param p Process to terminate
 *
 * This kills the process with SIGTERM and closes the communication
 * file descriptor. It's called during rollback and engine shutdown.
 */
static void close_process(TexProcess *p)
{
  if (p->fd != -1)
  {
    kill(p->pid, SIGTERM);
    close(p->fd);
    p->fd = -1;
  }
}

/**
 * @brief Remove the most recent process from the list
 * @param ctx MuPDF context
 * @param self Engine instance
 *
 * This rolls back the log to a previous checkpoint and decrements
 * process_count. The next step() will restart a process from that
 * checkpoint.
 */
static void pop_process(fz_context *ctx, struct TexEngine *self)
{
  TexProcess *p = get_process(self);
  close_process(p);
  channel_reset(self->c);
  self->process_count -= 1;
  LogMark mark =
      self->process_count > 0 ? get_process(self)->snap : self->restart;
  log_rollback(ctx, self->log, mark);
}

/**
 * @brief Read a query from the child process
 * @param self Engine instance
 * @param t Communication channel
 * @param q Output query structure
 * @return true if query was read successfully, false on EOF
 *
 * This wraps channel_read_query() with error handling. On EOF, it
 * closes the process and returns false.
 */
static bool read_query(struct TexEngine *self, IOChannel *t, ProtocolQuery *q)
{
  TexProcess *p = get_process(self);
  bool result = channel_read_query(t, p->fd, q);
  if (!result)
  {
    fprintf(stderr, "[process] terminating process\n");
    close_process(p);
  }
  return result;
}

/**
 * @brief Reduce the number of processes to a manageable count
 *
 * To prevent memory exhaustion from fork proliferation, this function
 * halves the number of processes (keeping every other one) when there
 * are more than 16. It closes the file descriptors of terminated processes
 * but keeps their trace entries for potential future rollback.
 *
 * @note This is called implicitly when adding a new process would exceed
 *       the 32-process limit in answer_query() for Q_CHLD.
 */
static void decimate_processes(struct TexEngine *self)
{
  fprintf(stderr, "before process decimation:\n");
  for (int i = 0; i < self->process_count; ++i)
  {
    TexProcess *p = &self->processes[i];
    fprintf(stderr, "- position %d, time %dms [pid %d]\n",
            p->trace_len,
            p->trace_len == 0 ? 0 : self->trace[p->trace_len - 1].time,
            p->pid);
  }

  int i = 0, bound = (self->process_count - 8) / 2;
  while (i < bound)
  {
    close_process(&self->processes[2 * i]);
    self->processes[i] = self->processes[2 * i + 1];
    i++;
  }
  for (int j = bound * 2; j < self->process_count; ++j)
  {
    self->processes[i] = self->processes[j];
    i++;
  }
  self->process_count = i;

  fprintf(stderr, "after process decimation:\n");
  for (int i = 0; i < self->process_count; ++i)
  {
    TexProcess *p = &self->processes[i];
    fprintf(stderr, "- position %d, time %dms [pid %d]\n",
            p->trace_len,
            p->trace_len == 0 ? 0 : self->trace[p->trace_len - 1].time,
            p->pid);
  }
}

// Engine class implementation

static void engine_destroy(Engine *_self, fz_context *ctx)
{
  SELF;
  while (self->process_count > 0)
    pop_process(ctx, self);
  incdvi_free(ctx, self->dvi);
  synctex_free(ctx, self->stex);
  fz_free(ctx, self->name);
  fz_free(ctx, self->engine_path);
  fz_free(ctx, self->inclusion_path);
  fz_free(ctx, self);
}

/**
 * @brief Expand a file path using inclusion directories
 * @param inclusion_path Path list (colon-separated on Unix)
 * @param name File name to resolve
 * @param buffer Working buffer (must be at least 1024 bytes)
 * @return Expanded path, or NULL if not found
 *
 * This implements TeX's input file search algorithm. It prepends each
 * directory from inclusion_path to the file name (if not absolute or
 * relative) and checks if the file exists.
 */
static const char *expand_path(const char **inclusion_path,
                               const char *name,
                               char buffer[1024])
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
    if (p > buffer + 1024)
      mabort();
    *p = *i;
    p += 1;
    i += 1;
  }
  *inclusion_path = i + 1;

  if (p[-1] != '/')
  {
    if (p > buffer + 1024)
      mabort();
    p[0] = '/';
    p += 1;
  }

  while (*name)
  {
    if (p > buffer + 1024)
      mabort();
    *p = *name;
    p += 1;
    name += 1;
  }

  if (p > buffer + 1024)
    mabort();
  *p = '\0';

  return buffer;
}

/**
 * @brief Validate a file ID
 * @param fid File ID to validate
 *
 * File IDs are indices into the state.table array. This ensures they
 * are within the valid range (0 to MAX_FILES-1).
 */
static void check_fid(file_id fid)
{
  if (fid < 0 || fid >= MAX_FILES)
    mabort();
}

/**
 * @brief Record a file access in the trace
 * @param self Engine instance
 * @param entry File entry that was accessed
 * @param seen New "seen" position (where TeX stopped reading)
 * @param time Timestamp when access occurred
 *
 * This adds a trace entry if the file access is new, or updates the
 * timestamp if the same file is accessed multiple times without
 * intermediate accesses to other files.
 */
static void record_seen(struct TexEngine *self,
                        FileEntry *entry,
                        int seen,
                        int time)
{
  TexProcess *p = get_process(self);

  // If same file and no forks between accesses, just update timestamp
  if (p->trace_len > 0 && self->trace[p->trace_len - 1].entry == entry &&
      (self->process_count <= 1 ||
       self->processes[self->process_count - 2].trace_len != p->trace_len))
  {
    self->trace[p->trace_len - 1].time = time;
    entry->seen = seen;
    return;
  }

  // Grow trace array if needed
  if (p->trace_len == self->trace_cap)
  {
    int new_cap = self->trace_cap == 0 ? 8 : self->trace_cap * 2;
    fprintf(stderr, "[info] trace has %d entries, growing to %d\n",
            self->trace_cap, new_cap);
    TraceEntry *newtr = calloc(sizeof(TraceEntry), new_cap);
    if (newtr == NULL)
      abort();
    if (self->trace)
    {
      memcpy(newtr, self->trace, self->trace_cap * sizeof(TraceEntry));
      free(self->trace);
    }
    self->trace = newtr;
    self->trace_cap = new_cap;
  }

  self->trace[p->trace_len] = (TraceEntry){
      .entry = entry,
      .seen = entry->seen,
      .time = time,
  };
  entry->seen = seen;
  p->trace_len += 1;
}

/**
 * @brief Get file data from the appropriate source
 * @param e File entry
 * @return Buffer containing file data
 *
 * File data can come from three sources in priority order:
 * 1. saved.data - If file was written by TeX (output file)
 * 2. edit_data  - If file is being edited and has unsaved changes
 * 3. fs_data    - Data read from the filesystem
 */
static fz_buffer *entry_data(FileEntry *e)
{
  if (e->saved.data)
    return e->saved.data;
  if (e->edit_data)
    return e->edit_data;
  return e->fs_data;
}

/**
 * @brief Get saved output data for a file
 * @param e File entry
 * @return Buffer containing saved data, or NULL if none
 *
 * This specifically returns only the "saved" output data, not the
 * current edit buffer or filesystem data. Used for output files.
 */
static fz_buffer *output_data(FileEntry *e)
{
  if (!e)
    return NULL;
  return e->saved.data;
}

/**
 * @brief Find a file's path in the filesystem
 * @param self Engine instance
 * @param path File path as referenced in TeX source
 * @param buf Temporary buffer for expanded paths
 * @param st Output stat buffer (can be NULL)
 * @return Actual filesystem path, or NULL if not found
 *
 * This searches for the file using both the inclusion_path and
 * direct path lookup. It populates the stat buffer if provided.
 */
static const char *lookup_path(struct TexEngine *self,
                               const char *path,
                               char buf[1024],
                               struct stat *st)
{
  struct stat st1;
  if (st == NULL)
    st = &st1;

  const char *fs_path = path;
  const char *inclusion_path = self->inclusion_path;

  do
  {
    if (stat(fs_path, st) != -1)
      break;
  } while ((fs_path = expand_path(&inclusion_path, path, buf)));

  return fs_path;
}

/**
 * @brief Get the time since last event for snapshot decision
 * @param ctx MuPDF context
 * @param self Engine instance
 * @param time Current timestamp from TeX process
 * @return true if a snapshot should be taken now
 *
 * This function decides whether to fork and create a checkpoint based on:
 * - No pending fences (we're at a safe point)
 * - Time elapsed since last snapshot (>500ms)
 * - New events have occurred in the current process
 *
 * On macOS, it delays initial forking until after output has started
 * (to work around font loading restrictions after fork).
 */
static bool need_snapshot(fz_context *ctx, struct TexEngine *self, int time)
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

static void answer_query(fz_context *ctx, struct TexEngine *self, ProtocolQuery *q)
{
  TexProcess *p = get_process(self);
  ProtocolAnswer a;
  switch (q->tag)
  {
    case Q_OPRD:
    case Q_OPWR:
    {
      check_fid(q->open.fid);
      FileCell *cell = &self->st.table[q->open.fid];
      if (cell->entry != NULL) mabort();

      FileEntry *e = NULL;

      char fs_path_buffer[1024];
      const char *fs_path = NULL;

      if (q->tag == Q_OPRD)
      {
        e = filesystem_lookup(self->fs, q->open.path);
        if (self->stream_mode && e && entry_data(e))
        {
          // Stream mode: VFS data available, skip filesystem lookup
        }
        else if (!e || !entry_data(e))
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
        (q->tag == Q_OPRD) ? FILE_READ : FILE_WRITE;

      if (level == FILE_READ)
      {
        if (e->saved.level < FILE_READ)
        {
          if (!fs_path && !(self->stream_mode && e->edit_data))
            fs_path = lookup_path(self, q->open.path, fs_path_buffer, NULL);
          if (!fs_path)
          {
            if (!e->edit_data)
            {
              if (self->stream_mode)
              {
                log_fileentry(ctx, self->log, e);
                record_seen(self, e, INT_MAX, q->time);
                a.tag = A_PASS;
                channel_write_answer(self->c, p->fd, &a);
                break;
              }
              mabort("path: %s\nmode:%c\n", q->open.path, (q->tag == Q_OPRD) ? 'r' : 'w');
            }
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
      a.open.path_len = n;
      a.tag = A_OPEN;
      memmove(channel_get_buffer(self->c, n), q->open.path, n);
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_READ:
    {
      check_fid(q->read.fid);
      FileEntry *e = self->st.table[q->read.fid].entry;
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
    case Q_APND:
    {
      FileEntry *e = NULL;

      if (q->apnd.fid == -1)
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
      }
      else
      {
        check_fid(q->apnd.fid);
        e = self->st.table[q->apnd.fid].entry;
      }

      if (e == NULL || e->saved.level != FILE_WRITE) mabort();
      log_fileentry(ctx, self->log, e);

      int pos = e->saved.data->len;
      fz_append_data(ctx, e->saved.data, q->apnd.buf, q->apnd.size);

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
        editor_append(BUF_LOG, output_data(e), pos);
      else if (self->st.stdout.entry == e)
        editor_append(BUF_OUT, output_data(e), pos);
      a.tag = A_DONE;
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_CLOS:
    {
      check_fid(q->clos.fid);

      FileCell *cell = &self->st.table[q->clos.fid];
      FileEntry *e = cell->entry;
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
      FileEntry *e = self->st.table[q->clos.fid].entry;
      if (e == NULL || e->saved.level < FILE_READ) mabort();
      a.tag = A_SIZE;
      a.size.size = entry_data(e)->len;
      if (LOG)
        fprintf(stderr, "SIZE = %d (seen = %d)\n", a.size.size, e->seen);
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_MTIM:
    {
      check_fid(q->clos.fid);
      FileEntry *e = self->st.table[q->clos.fid].entry;
      if (e == NULL || e->saved.level < FILE_READ) mabort();
      a.tag = A_MTIM;
      a.mtim.mtime = e->fs_stat.st_mtime;
      if (LOG)
        fprintf(stderr, "MTIM = %d\n", a.mtim.mtime);
      channel_write_answer(self->c, p->fd, &a);
      break;
    }
    case Q_SEEN:
    {
      check_fid(q->seen.fid);
      FileEntry *e = self->st.table[q->seen.fid].entry;
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
      FileEntry *e = filesystem_lookup(self->fs, q->gpic.path);
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
      FileEntry *e = filesystem_lookup(self->fs, q->spic.path);
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
      TexProcess *p2 = get_process(self);
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

static int output_length(FileEntry *entry)
{
  if (!entry || !entry->saved.data)
    return 0;
  else
    return entry->saved.data->len;
}

static void revert_trace(TraceEntry *te)
{
  te->entry->seen = te->seen;
}

static void rollback_processes(fz_context *ctx, struct TexEngine *self, int reverted, int trace)
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
    TexProcess *p = &self->processes[i];
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

static bool possible_fence(TraceEntry *te)
{
  if (te->seen == INT_MAX || te->seen == -1)
    return 0;
  if (te->entry->saved.level > FILE_READ)
    return 0;
  return 1;
}

static int compute_fences(fz_context *ctx, struct TexEngine *self, int trace, int offset)
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

static int engine_page_count(Engine *_self)
{
  SELF;
  return incdvi_page_count(self->dvi);
}

static fz_display_list *engine_render_page(Engine *_self, fz_context *ctx, int page)
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

static bool engine_step(Engine *_self, fz_context *ctx, bool restart_if_needed)
{
  SELF;
  if (restart_if_needed)
    prepare_process(ctx, self);

  if (engine_get_status(_self) == DOC_RUNNING)
  {
    ProtocolQuery q;
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

static int scan_entry(fz_context *ctx, struct TexEngine *self, FileEntry *e)
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

static void rollback_begin(fz_context *ctx, struct TexEngine *self)
{
  // Check if already in a transaction
  if (self->rollback.trace_len != NOT_IN_TRANSACTION)
    abort();

  self->rollback.trace_len = get_process(self)->trace_len;
  self->rollback.offset = -1;
  self->rollback.flush = 0;
}

static bool rollback_end(fz_context *ctx, struct TexEngine *self, int *tracep, int *offsetp)
{
  int trace_len = self->rollback.trace_len;
  self->rollback.trace_len = NOT_IN_TRANSACTION;

  // Assert we are in a transaction
  if (trace_len == NOT_IN_TRANSACTION)
    abort();

  TexProcess *p = get_process(self);

  // Check if nothing changed
  if (trace_len == p->trace_len)
  {
    if (!self->rollback.flush)
      return false;
    if (p->fd > -1)
    {
      ProtocolAsk a;
      a.tag = C_FLSH;
      channel_write_ask(self->c, p->fd, &a);
      channel_flush(self->c, p->fd);
      return false;
    }
    trace_len -= 1;
    revert_trace(&self->trace[trace_len]);
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
static bool process_pending_messages(fz_context *ctx, struct TexEngine *self)
{
  // If the process is marked ready to flush, seen messages have already been
  // consumed
  if (self->rollback.flush)
    return 1;

  TexProcess *p = get_process(self);

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
          ProtocolQuery q;
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

static void rollback_add_change(fz_context *ctx, struct TexEngine *self, FileEntry *e, int changed)
{
  int trace_len = self->rollback.trace_len;
  // if (changed > 0) changed--;

  // Assert we are in a transaction
  if (trace_len == NOT_IN_TRANSACTION)
    mabort();

  if (e->seen < changed && trace_len == get_process(self)->trace_len)
  {
    // A pending message might update e->seen
    if (process_pending_messages(ctx, self))
      return;
    trace_len = self->rollback.trace_len = get_process(self)->trace_len;
  }
  if (e->seen < changed)
    return;

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

static void engine_notify_file_changes(Engine *_self,
                                       fz_context *ctx,
                                       FileEntry *entry,
                                       int offset)
{
  SELF;
  rollback_add_change(ctx, self, entry, offset);
}

static void engine_begin_changes(Engine *_self, fz_context *ctx)
{
  SELF;
  rollback_begin(ctx, self);
}

static void engine_detect_changes(Engine *_self, fz_context *ctx)
{
  SELF;

  FileEntry *e;
  for (int index = 0; (e = filesystem_scan(self->fs, &index));)
  {
    int changed = scan_entry(ctx, self, e);
    if (changed > -1)
      rollback_add_change(ctx, self, e, changed);
  }
}

static bool engine_end_changes(Engine *_self, fz_context *ctx)
{
  SELF;
  int reverted, trace, offset;

  if (!rollback_end(ctx, self, &reverted, &offset))
    return false;

  trace = reverted >= 0 ? compute_fences(ctx, self, reverted, offset) : 0;
  rollback_processes(ctx, self, reverted, trace);

  return true;
}

static EngineStatus engine_get_status(Engine *_self)
{
  SELF;
  if (self->process_count == 0)
    return DOC_TERMINATED;
  return get_process(self)->fd > -1 ? DOC_RUNNING : DOC_TERMINATED;
}

static float engine_scale_factor(Engine *_self)
{
  SELF;
  return incdvi_tex_scale_factor(self->dvi);
}

static TexSynctex *engine_synctex(Engine *_self, fz_buffer **buf)
{
  SELF;
  if (buf)
    *buf = self->st.synctex.entry ? entry_data(self->st.synctex.entry) : NULL;
  return self->stex;
}

static FileEntry *engine_find_file(Engine *_self, fz_context *ctx, const char *path)
{
  SELF;
  return filesystem_lookup_or_create(ctx, self->fs, path);
}

Engine *create_tex_engine(fz_context *ctx,
                          const char *engine_path,
                          bool use_texlive,
                          bool stream_mode,
                          const char *inclusion_path,
                          const char *tex_name,
                          dvi_reshooks hooks)
{
  struct TexEngine *self = fz_malloc_struct(ctx, struct TexEngine);
  self->_class = &_class;

  self->name = fz_strdup(ctx, tex_name);
  self->engine_path = fz_strdup(ctx, engine_path);
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

  self->dvi = incdvi_new(ctx, hooks);
  self->use_texlive = use_texlive;
  self->stream_mode = stream_mode;

  self->stex = synctex_new(ctx);
  self->rollback.trace_len = NOT_IN_TRANSACTION;

  return (Engine*)self;
}
