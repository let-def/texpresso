/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * In-process wasm engine backend for TeXpresso.
 *
 * Where engine_tex.c drives a forked, patched TeX process over a socket, this
 * backend runs a stock TeX engine compiled to wasm2c *in-process* on a
 * coroutine (see src/engine-wasm/wasm_host.c). The engine's file syscalls are
 * routed here through the wasm_io_ops seam and answered from texpresso's VFS
 * (state.c fileentry buffers) — no native filesystem, no IPC, no fork.
 *
 * Editing is incremental: the engine is fenced (snapshotted) every ~stride bytes
 * of the document via the coroutine + layered-COW machinery in wasm_host.c. On
 * an edit we restore the deepest fence taken before it and replay only from
 * there — skipping module init, format undump and the unedited prefix. A full
 * re-instantiate (do_run) is the recovery path. Engine-specific configuration
 * lives in wasm_engine_profile (engine_wasm_<name>.c); this file is
 * engine-agnostic.
 */

#include <mupdf/fitz/buffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "engine.h"
#include "incdvi.h"
#include "mydvi.h"
#include "state.h"
#include "synctex.h"
#include "editor.h"
#include "engine_wasm.h"
#include "../engine-wasm/wasm_host.h"

/* Fence stack: at most this many checkpoints (must fit the host's COW layer
 * cap), spaced by roughly this many bytes of main-document progress. */
#define WASM_MAX_FENCES 32
#define FENCE_STRIDE (32 * 1024)

struct wasm_engine
{
  struct txp_engine_class *_class;

  char *name;
  char *prog;
  char *fmtarg; /* "-fmt=<format>" (owned) or NULL */
  int argc;
  char *argv[8];

  fz_context *ctx; /* valid only during a step()/run — io_ops need it */

  filesystem_t *fs;
  state_t st;
  log_t *log;
  mark_t restart;

  incdvi_t *dvi;
  synctex_t *stex;

  /* our fd table: openat() returns an index here (fd >= 3) */
  struct { fileentry_t *entry; int pos; } fds[MAX_FILES];

  bool ran;   /* has completed at least one run */
  bool dirty; /* a watched input changed -> replay needed */
  bool aux_dirty;
  bool finishing;

  /* Fence stack: a checkpoint every ~FENCE_STRIDE bytes of the main document,
   * layered over the host's COW snapshot stack. fences[i] uses COW layer i. On
   * an edit we restore the deepest fence taken before the edit and replay only
   * from there. */
  struct wasm_fence {
    mark_t mark;    /* VFS log mark at this fence */
    int doc_pos;    /* main-document bytes consumed at this fence */
    struct { fileentry_t *entry; int pos; } fds[MAX_FILES];
  } fences[WASM_MAX_FENCES];
  int n_fences;
  bool want_fence;        /* push a fence at the next read (armed in wio_*) */
  int last_fence_pos;     /* doc_pos of the most recent fence (for stride) */
  fileentry_t *doc_entry; /* the main .tex entry, once opened */
  int edit_pos;           /* earliest edited offset in the main doc (-1 = none) */
};

TXP_ENGINE_DEF_CLASS;
#define SELF struct wasm_engine *self = (struct wasm_engine *)_self

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
  return (e && e->saved.data) ? e->saved.data : NULL;
}

static char *last_ext(const char *path)
{
  const char *dot = strrchr(path, '.');
  return (char *)(dot ? dot : "");
}

/* ---------------- io_ops: the VFS seam, backed by state.c ---------------- */

static int wio_openat(void *vctx, int dirfd, const char *path, int flags,
                      mode_t mode)
{
  struct wasm_engine *self = vctx;
  fz_context *ctx = self->ctx;
  (void)dirfd; (void)mode;
  int write = (flags & (O_WRONLY | O_RDWR | O_CREAT)) != 0;

  fileentry_t *e = filesystem_lookup_or_create(ctx, self->fs, path);
  log_fileentry(ctx, self->log, e);

  if (!write)
  {
    if (e->saved.level < FILE_READ)
    {
      if (!entry_data(e))
      {
        /* not in the VFS: read it from disk once */
        struct stat st;
        if (stat(path, &st) != 0) { errno = ENOENT; return -1; }
        fz_try(ctx) { e->fs_data = fz_read_file(ctx, path); }
        fz_catch(ctx) { errno = EIO; return -1; }
        e->fs_stat = st;
      }
      e->saved.level = FILE_READ;
    }
    if (e->seen < 0) e->seen = 0;
  }
  else
  {
    e->saved.data = fz_new_buffer(ctx, 1024);
    e->saved.level = FILE_WRITE;
    const char *ext = last_ext(path);
    if (strcmp(path, "stdout") == 0)
    {
      log_filecell(ctx, self->log, &self->st.stdout);
      self->st.stdout.entry = e;
    }
    else if (!strcmp(ext, ".xdv") || !strcmp(ext, ".dvi") || !strcmp(ext, ".pdf"))
    {
      log_filecell(ctx, self->log, &self->st.document);
      self->st.document.entry = e;
      incdvi_reset(self->dvi);
    }
    else if (!strcmp(ext, ".synctex"))
    {
      log_filecell(ctx, self->log, &self->st.synctex);
      self->st.synctex.entry = e;
      synctex_rollback(ctx, self->stex, 0);
    }
    else if (!strcmp(ext, ".log"))
    {
      log_filecell(ctx, self->log, &self->st.log);
      self->st.log.entry = e;
    }
  }

  /* Base fence (0): the main document has just been opened. Arm a yield on the
   * next read, which fires *before* that read returns data (see fd_read) — so the
   * fence precedes any of the document's bytes entering TeX's line buffer. */
  if (!write && self->n_fences == 0 && !self->want_fence &&
      strcmp(path, self->name) == 0)
  {
    self->doc_entry = e;
    self->want_fence = true;
    wasm_engine_request_yield();
  }

  for (int fd = 3; fd < MAX_FILES; fd++)
    if (!self->fds[fd].entry)
    {
      self->fds[fd].entry = e;
      self->fds[fd].pos = 0;
      return fd;
    }
  errno = EMFILE;
  return -1;
}

static ssize_t wio_read(void *vctx, int fd, void *buf, size_t n)
{
  struct wasm_engine *self = vctx;
  if (fd < 3) return 0; /* stdin: EOF (engine runs -interaction=nonstopmode) */
  if (fd >= MAX_FILES || !self->fds[fd].entry) { errno = EBADF; return -1; }
  fileentry_t *e = self->fds[fd].entry;
  fz_buffer *data = entry_data(e);
  int pos = self->fds[fd].pos;
  if (pos > (int)data->len) pos = data->len;
  size_t k = (size_t)data->len - pos;
  if (k > n) k = n;
  memcpy(buf, data->data + pos, k);
  self->fds[fd].pos = pos + (int)k;
  if (self->fds[fd].pos > e->seen) e->seen = self->fds[fd].pos;

  /* Arm the next fence once the main document has advanced by a stride. The push
   * happens at the following read (see do_run), while the engine is suspended. */
  if (e == self->doc_entry && !self->want_fence &&
      self->n_fences > 0 && self->n_fences < WASM_MAX_FENCES &&
      e->seen - self->last_fence_pos >= FENCE_STRIDE)
  {
    self->want_fence = true;
    wasm_engine_request_yield();
  }
  return (ssize_t)k;
}

static ssize_t wio_write(void *vctx, int fd, const void *buf, size_t n)
{
  struct wasm_engine *self = vctx;
  fz_context *ctx = self->ctx;
  if (fd == 2) { return (ssize_t)fwrite(buf, 1, n, stderr); } /* engine stderr */

  fileentry_t *e;
  if (fd == 1) /* stdout */
  {
    e = self->st.stdout.entry;
    if (!e)
    {
      e = filesystem_lookup_or_create(ctx, self->fs, "stdout");
      log_fileentry(ctx, self->log, e);
      log_filecell(ctx, self->log, &self->st.stdout);
      e->saved.data = fz_new_buffer(ctx, 1024);
      e->saved.level = FILE_WRITE;
      self->st.stdout.entry = e;
    }
  }
  else
  {
    if (fd < 3 || fd >= MAX_FILES) { errno = EBADF; return -1; }
    e = self->fds[fd].entry;
  }
  if (!e || e->saved.level != FILE_WRITE) { errno = EBADF; return -1; }

  log_fileentry(ctx, self->log, e);
  int pos = e->saved.data->len; /* append (TeX output is sequential) */
  fz_append_data(ctx, e->saved.data, buf, n);

  if (self->st.document.entry == e)
    incdvi_update(ctx, self->dvi, e->saved.data);
  else if (self->st.synctex.entry == e)
    synctex_update(ctx, self->stex, e->saved.data);
  else if (self->st.log.entry == e)
    editor_append(BUF_LOG, output_data(e), pos);
  else if (self->st.stdout.entry == e)
    editor_append(BUF_OUT, output_data(e), pos);
  else
    self->aux_dirty = true;
  return (ssize_t)n;
}

static off_t wio_lseek(void *vctx, int fd, off_t off, int whence)
{
  struct wasm_engine *self = vctx;
  if (fd < 3 || fd >= MAX_FILES || !self->fds[fd].entry) { errno = EBADF; return -1; }
  fileentry_t *e = self->fds[fd].entry;
  off_t len = (off_t)entry_data(e)->len;
  off_t base = (whence == SEEK_SET) ? 0 : (whence == SEEK_CUR) ? self->fds[fd].pos : len;
  off_t np = base + off;
  if (np < 0) { errno = EINVAL; return -1; }
  self->fds[fd].pos = (int)np;
  return np;
}

static int wio_close(void *vctx, int fd)
{
  struct wasm_engine *self = vctx;
  if (fd >= 3 && fd < MAX_FILES) self->fds[fd].entry = NULL;
  return 0;
}

static void fill_estat(fileentry_t *e, struct stat *s)
{
  /* Base on the real cached disk stat when we have one: the engine's kpathsea
   * keys directory identity on st_ino/st_dev, and the first-run disk stat and
   * the replay's cached stat must agree or kpathsea drops "." from its search
   * (then a written-then-read file like .aux is not found). fs_stat carries the
   * real ino/dev/mode/mtime from the first disk read. VFS-only files (never on
   * disk) get a stable synthetic inode from the entry pointer. Size is always
   * the current VFS length. */
  if (e->fs_stat.st_ino != 0)
    *s = e->fs_stat;
  else
  {
    memset(s, 0, sizeof *s);
    s->st_mode = S_IFREG | 0644;
    s->st_nlink = 1;
    s->st_blksize = 4096;
    s->st_dev = 1; /* synthetic device for in-memory-only files */
    s->st_ino = (ino_t)(uintptr_t)e; /* stable across runs (entries persist) */
  }
  s->st_size = entry_data(e) ? (off_t)entry_data(e)->len : 0;
}

static int wio_fstat(void *vctx, int fd, struct stat *s)
{
  struct wasm_engine *self = vctx;
  if (fd < 3 || fd >= MAX_FILES || !self->fds[fd].entry) { errno = EBADF; return -1; }
  fill_estat(self->fds[fd].entry, s);
  return 0;
}

static int wio_statat(void *vctx, int dirfd, const char *path, struct stat *s,
                      int atflags)
{
  struct wasm_engine *self = vctx;
  (void)dirfd; (void)atflags;
  fileentry_t *e = filesystem_lookup(self->fs, path);
  if (e && entry_data(e)) { fill_estat(e, s); return 0; }
  return stat(path, s); /* existence probe hits disk (read-only) */
}

static int wio_accessat(void *vctx, int dirfd, const char *path, int amode,
                        int atflags)
{
  struct wasm_engine *self = vctx;
  (void)dirfd; (void)atflags;
  fileentry_t *e = filesystem_lookup(self->fs, path);
  if (e && entry_data(e)) return 0;
  return access(path, amode);
}

static const wasm_io_ops wasm_state_io_ops = {
    wio_openat, wio_read,   wio_write,  wio_lseek,
    wio_close,  wio_fstat,  wio_statat, wio_accessat};

/* ---------------- run / replay ---------------- */

static void reset_for_run(fz_context *ctx, struct wasm_engine *self)
{
  memset(self->fds, 0, sizeof self->fds);
  log_rollback(ctx, self->log, self->restart);
  incdvi_reset(self->dvi);
  synctex_rollback(ctx, self->stex, 0);
  editor_truncate(BUF_OUT, output_data(self->st.stdout.entry));
  editor_truncate(BUF_LOG, output_data(self->st.log.entry));
  self->aux_dirty = false;
  self->n_fences = 0;
  self->want_fence = false;
  self->last_fence_pos = 0;
  self->doc_entry = NULL;
  self->edit_pos = -1;
  fileentry_t *e;
  for (int i = 0; (e = filesystem_scan(self->fs, &i));)
    e->seen = -1;
}

/* Record a fence over the current engine state: a COW layer + the VFS log mark,
 * the document read position, and the fd table. */
static void push_fence(fz_context *ctx, struct wasm_engine *self)
{
  if (self->n_fences >= WASM_MAX_FENCES) return;
  wasm_engine_snapshot_push(); /* COW layer index == fence index */
  struct wasm_fence *f = &self->fences[self->n_fences++];
  f->mark = log_snapshot(ctx, self->log);
  f->doc_pos = self->doc_entry ? self->doc_entry->seen : 0;
  memcpy(f->fds, self->fds, sizeof f->fds);
  self->last_fence_pos = f->doc_pos;
}

/* Resume to completion, pushing a fence whenever one was armed (wio_openat for
 * the base fence, wio_read for the stride fences). */
static void run_to_end(fz_context *ctx, struct wasm_engine *self)
{
  while (wasm_engine_run())
    if (self->want_fence)
    {
      self->want_fence = false;
      push_fence(ctx, self);
    }
}

/* First run of the session: fresh module instance, run to completion, laying
 * down fences as the document is consumed. */
static void do_run(fz_context *ctx, struct wasm_engine *self)
{
  self->ctx = ctx;
  wasm_host_set_io(&wasm_state_io_ops, self);
  reset_for_run(ctx, self);
  if (wasm_engine_init(self->argc, self->argv) != 0)
  {
    fprintf(stderr, "[wasm] engine init failed\n");
    return;
  }
  run_to_end(ctx, self);
  self->ran = true;
  self->dirty = false;
  fprintf(stderr, "[wasm] run complete: %d pages (%d fences)\n",
          incdvi_page_count(self->dvi), self->n_fences);
}

/* Edit: restore the deepest fence taken before the edit, roll the VFS back to
 * it, and replay only from there — re-reading the edited document while the
 * format stays loaded. */
static void do_replay(fz_context *ctx, struct wasm_engine *self)
{
  if (self->n_fences == 0) /* nothing to restore: full re-instantiate */
  {
    do_run(ctx, self);
    return;
  }
  self->ctx = ctx;
  wasm_host_set_io(&wasm_state_io_ops, self);

  int k = 0; /* deepest fence taken at or before the edit (fence 0 = whole body) */
  for (int i = self->n_fences - 1; i >= 0; i--)
    if (self->fences[i].doc_pos <= self->edit_pos) { k = i; break; }
  struct wasm_fence *f = &self->fences[k];

  wasm_engine_restore_to(k);
  log_rollback(ctx, self->log, f->mark);
  if (self->st.document.entry && self->st.document.entry->saved.data)
    incdvi_update(ctx, self->dvi, self->st.document.entry->saved.data);
  else
    incdvi_reset(self->dvi);
  if (self->st.synctex.entry && self->st.synctex.entry->saved.data)
    synctex_update(ctx, self->stex, self->st.synctex.entry->saved.data);
  else
    synctex_rollback(ctx, self->stex, 0);
  editor_truncate(BUF_OUT, output_data(self->st.stdout.entry));
  editor_truncate(BUF_LOG, output_data(self->st.log.entry));
  self->aux_dirty = false;
  memcpy(self->fds, f->fds, sizeof self->fds); /* authoritative fd state */
  self->n_fences = k + 1;                       /* deeper fences popped by restore_to */
  self->last_fence_pos = f->doc_pos;
  self->want_fence = false;
  self->edit_pos = -1;

  run_to_end(ctx, self); /* re-read from the fence, re-establishing fences forward */
  self->dirty = false;
  fprintf(stderr, "[wasm] replay from fence %d: %d pages (%d fences)\n",
          k, incdvi_page_count(self->dvi), self->n_fences);
}

/* ---------------- vtable ---------------- */

static bool engine_step(txp_engine *_self, fz_context *ctx, bool restart_if_needed)
{
  SELF;
  if (!self->ran)
  {
    if (!restart_if_needed) return false;
    do_run(ctx, self);
    return true;
  }
  if (self->dirty)
  {
    do_replay(ctx, self); /* incremental: restore the fence before the edit */
    return true;
  }
  return false; /* nothing pending: the run is atomic */
}

static void engine_begin_changes(txp_engine *_self, fz_context *ctx)
{
  (void)_self; (void)ctx;
}

static int scan_entry(fz_context *ctx, struct wasm_engine *self, fileentry_t *e)
{
  if (e->saved.level < FILE_READ || e->fs_stat.st_ino == 0 || e->edit_data)
    return -1;
  struct stat st;
  if (stat(e->path, &st) != 0) return -1;
  if (stat_same(&st, &e->fs_stat)) return -1;
  fz_buffer *buf;
  fz_var(buf);
  fz_try(ctx) { buf = fz_read_file(ctx, e->path); }
  fz_catch(ctx) { return -1; }
  e->fs_stat = st;
  fz_drop_buffer(ctx, e->fs_data);
  e->fs_data = buf;
  return 0;
}

static void engine_detect_changes(txp_engine *_self, fz_context *ctx)
{
  SELF;
  fileentry_t *e;
  for (int i = 0; (e = filesystem_scan(self->fs, &i));)
    if (scan_entry(ctx, self, e) == 0)
      self->dirty = true;
}

static void engine_notify_file_changes(txp_engine *_self, fz_context *ctx,
                                       fileentry_t *entry, int offset)
{
  SELF;
  (void)ctx;
  self->dirty = true;
  /* Track the earliest edited byte of the main document so do_replay can pick
   * the fence just before it. An edit to any other file (e.g. an \input) can't
   * be located on the main-document axis, so fall back to the base fence. */
  if (entry == self->doc_entry)
  {
    if (self->edit_pos < 0 || offset < self->edit_pos)
      self->edit_pos = offset;
  }
  else
    self->edit_pos = 0;
}

static bool engine_end_changes(txp_engine *_self, fz_context *ctx)
{
  SELF;
  (void)ctx;
  return self->dirty; /* true -> the main loop triggers a step() replay */
}

static int engine_page_count(txp_engine *_self)
{
  SELF;
  return incdvi_page_count(self->dvi);
}

static fz_display_list *engine_render_page(txp_engine *_self, fz_context *ctx,
                                           int page)
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

static txp_engine_status engine_get_status(txp_engine *_self)
{
  SELF;
  /* atomic run model: RUNNING only until the first (or a replay) run finishes */
  return (!self->ran || self->dirty) ? DOC_RUNNING : DOC_TERMINATED;
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

static fileentry_t *engine_find_file(txp_engine *_self, fz_context *ctx,
                                     const char *path)
{
  SELF;
  return filesystem_lookup_or_create(ctx, self->fs, path);
}

static bool engine_aux_dirty(txp_engine *_self) { SELF; return self->aux_dirty; }
static bool engine_is_finishing(txp_engine *_self) { SELF; return self->finishing; }
static void engine_start_finishing(txp_engine *_self) { SELF; self->finishing = true; }

static void engine_finish_convergence(txp_engine *_self, fz_context *ctx)
{
  SELF;
  (void)ctx;
  /* Reruns/convergence: Phase 3 (needs the aux-stash logic from engine_tex.c). */
  self->finishing = false;
}

static void engine_destroy(txp_engine *_self, fz_context *ctx)
{
  SELF;
  wasm_engine_shutdown();
  incdvi_free(ctx, self->dvi);
  synctex_free(ctx, self->stex);
  filesystem_free(ctx, self->fs);
  log_free(ctx, self->log);
  fz_free(ctx, self->name);
  fz_free(ctx, self->prog);
  fz_free(ctx, self->fmtarg);
  fz_free(ctx, self);
}

/* Point the in-process engine's kpathsea at the system TeX Live. Its SELFAUTO*
 * derivation lands on our binary, not the tree, so override the tree vars from
 * kpsewhich (env vars beat texmf.cnf definitions). Done once; the engine reads
 * them via the host's environ passthrough. */
static void setenv_kpse(const char *var, const char *kpsevar)
{
  if (getenv(var)) return;
  char cmd[256];
  snprintf(cmd, sizeof cmd, "kpsewhich -var-value=%s 2>/dev/null", kpsevar);
  FILE *p = popen(cmd, "r");
  if (!p) return;
  char buf[2048];
  if (fgets(buf, sizeof buf, p))
  {
    buf[strcspn(buf, "\n")] = 0;
    if (buf[0]) setenv(var, buf, 1);
  }
  pclose(p);
}

static void wasm_setup_texmf(bool needs_icu)
{
  setenv_kpse("TEXMFROOT", "TEXMFROOT");
  setenv_kpse("TEXMFDIST", "TEXMFDIST");
  setenv_kpse("TEXMFLOCAL", "TEXMFLOCAL");
  setenv_kpse("TEXMFSYSVAR", "TEXMFSYSVAR");
  setenv_kpse("TEXMFSYSCONFIG", "TEXMFSYSCONFIG");
  const char *v = getenv("TEXMFSYSVAR");
  if (v && !getenv("TEXMFVAR")) setenv("TEXMFVAR", v, 1);
  const char *r = getenv("TEXMFROOT"), *d = getenv("TEXMFDIST");
  if (r && d && !getenv("TEXMFCNF"))
  {
    char c[4096];
    snprintf(c, sizeof c, "%s:%s/web2c", r, d);
    setenv("TEXMFCNF", c, 1);
  }
  /* Where our built <fmt>.fmt lives (see scripts/build-wasm-fmt.sh). */
  const char *fmt = getenv("TEXPRESSO_WASM_FMT");
  if (fmt && !getenv("TEXFORMATS")) setenv("TEXFORMATS", fmt, 1);
  /* xetex reads its ICU data (icudt*.dat) from the same directory. */
  if (needs_icu && fmt && !getenv("ICU_DATA")) setenv("ICU_DATA", fmt, 1);
}

txp_engine *txp_wasm_engine_create(fz_context *ctx,
                                   const wasm_engine_profile *prof,
                                   const char *engine_path,
                                   const char *tex_name, dvi_reshooks hooks)
{
  wasm_setup_texmf(prof->needs_icu);
  struct wasm_engine *self = fz_malloc_struct(ctx, struct wasm_engine);
  self->_class = &_class;
  /* Give the main file an explicit "./" so the engine's kpathsea opens it
   * directly (a bare relative name isn't found without a texmf.cnf "." path). */
  if (tex_name[0] != '/' && !(tex_name[0] == '.' && tex_name[1] == '/'))
  {
    self->name = fz_malloc(ctx, strlen(tex_name) + 3);
    self->name[0] = '.'; self->name[1] = '/'; strcpy(self->name + 2, tex_name);
  }
  else
    self->name = fz_strdup(ctx, tex_name);
  /* argv[0] must be a real, existing path: kpathsea lstat()s it to find its
   * SELFAUTOLOC (the engine has no real binary, so borrow the host's path). */
  self->prog = fz_strdup(ctx, engine_path);

  int a = 0;
  self->argv[a++] = self->prog;
  for (int i = 0; i < 3 && prof->extra_argv[i]; i++)
    self->argv[a++] = (char *)prof->extra_argv[i];
  if (prof->format && getenv("TEXFORMATS")) /* our <format>.fmt -> real LaTeX */
  {
    self->fmtarg = fz_malloc(ctx, strlen(prof->format) + 6);
    sprintf(self->fmtarg, "-fmt=%s", prof->format);
    self->argv[a++] = self->fmtarg;
  }
  else /* no format: raw primitives only */
    self->argv[a++] = "-ini";
  self->argv[a++] = "-interaction=nonstopmode";
  self->argv[a++] = self->name;
  self->argv[a] = NULL;
  self->argc = a;

  state_init(&self->st);
  self->fs = filesystem_new(ctx);
  self->log = log_new(ctx);
  self->restart = log_snapshot(ctx, self->log);
  self->dvi = incdvi_new(ctx, hooks);
  self->stex = synctex_new(ctx);
  return (txp_engine *)self;
}

/* Public entry (see engine.h): pick the profile from the engine path. */
txp_engine *txp_create_wasm_engine(fz_context *ctx, const char *engine_path,
                                   const char *tex_name, dvi_reshooks hooks)
{
  static const wasm_engine_profile *const profiles[] = {
      &txp_wasm_profile_xetex,
      &txp_wasm_profile_pdftex,
      &txp_wasm_profile_luatex,
  };
  const wasm_engine_profile *prof = &txp_wasm_profile_xetex; /* default */
  for (size_t i = 0; i < sizeof profiles / sizeof *profiles; i++)
    if (strstr(engine_path, profiles[i]->name)) { prof = profiles[i]; break; }
  return txp_wasm_engine_create(ctx, prof, engine_path, tex_name, hooks);
}
