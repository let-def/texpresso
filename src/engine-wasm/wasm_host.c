/*
 * Native host for a wasm2c-compiled TeX engine (pdftex or xetex).
 *
 * Engine-agnostic: the wasm2c module is generated with `-n engine`, so the
 * exported symbols are w2c_engine_* and this one host drives any engine. Import
 * symbols (w2c_env_*, w2c_wasi_*) are keyed on the import module, not the engine.
 *
 * Implements the wasm imports (WASI subset + the Linux syscalls emscripten
 * leaves as env.__syscall_*) against native POSIX, and runs the engine on a
 * dedicated fixed-address stack (ucontext coroutine) so its full execution
 * state can be snapshotted by copy-on-write of the linear memory + that stack —
 * no JS, no node, no fork, no asyncify.
 *
 * File operations (openat/stat/lstat/newfstatat) are imports here — that is the
 * hook texpresso's VFS will replace. For now they map to the native FS. Two
 * cross-ABI translations are required: the engine passes Linux O_* flag values
 * (openat), and stat results are written in emscripten's struct-stat layout.
 */
#define _XOPEN_SOURCE 700 /* ucontext on macOS/glibc */
#define _DARWIN_C_SOURCE 1 /* re-expose MAP_ANON etc. under _XOPEN_SOURCE */
#include <ucontext.h>
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "wasm-rt.h"
#include "engine.h" /* wasm2c-generated header */
#include "wasm_host.h"

/* Import module structs: hold a back-pointer to the instance so imports can
 * reach its linear memory. */
struct w2c_env {
  w2c_engine *mod;
};
struct w2c_wasi__snapshot__preview1 {
  w2c_engine *mod;
};

/* argv the wasm engine sees via WASI args_get. Set in main(). */
static int g_argc;
static char **g_argv;

/* ---- coroutine: run the engine on a dedicated fixed-address stack ---- */
#define ENGINE_STACK_SIZE (32u * 1024u * 1024u)
static ucontext_t g_host_ctx;   /* where the engine yields back to */
static ucontext_t g_engine_ctx; /* the engine's suspended state */
static void *g_engine_stack;
static w2c_engine *g_mod;
static uint32_t g_entry_argv;
static int g_entry_argc;
static int g_engine_done;   /* engine returned/exited */
static int g_yield_next_read; /* request: suspend on the next fd_read */
static int g_suspended;     /* engine is currently suspended in fd_read */
static unsigned long long g_run_hash = 1469598103934665603ull; /* FNV-1a */
static int g_trace; /* TEXPRESSO_TRACE: log openat/mmap for debugging */
/* Virtualized clock: a fixed epoch (seconds), constant for the whole process so
 * snapshot/replay is deterministic. From $SOURCE_DATE_EPOCH, else read once at
 * startup. All time sources below derive from it; the engine reads it once and
 * the value is captured in snapshots. */
static long long g_epoch;
static int g_defer_close; /* snapshot test: keep fds open so rollback can reset
                             their positions instead of them being closed */

/* Switch from engine back to host (called from imports on the engine stack). */
static void engine_yield(void) { swapcontext(&g_engine_ctx, &g_host_ctx); }

static void engine_trampoline(void) {
  w2c_engine_0x5F_main_argc_argv(g_mod, (u32)g_entry_argc, g_entry_argv);
  g_engine_done = 1;
  /* uc_link returns us to the host */
}

/* ---- linear-memory access (wasm is little-endian; host arm64/x86 too) ---- */

static uint8_t *mem_base(w2c_engine *m) { return m->w2c_memory.data; }
static uint64_t mem_size(w2c_engine *m) { return m->w2c_memory.size; }

static int mem_ok(w2c_engine *m, uint32_t addr, uint32_t len) {
  return (uint64_t)addr + len <= mem_size(m);
}
static uint32_t rd_u32(w2c_engine *m, uint32_t addr) {
  uint32_t v = 0;
  memcpy(&v, mem_base(m) + addr, 4);
  return v;
}
static void wr_u32(w2c_engine *m, uint32_t addr, uint32_t v) {
  memcpy(mem_base(m) + addr, &v, 4);
}
static void wr_u64(w2c_engine *m, uint32_t addr, uint64_t v) {
  memcpy(mem_base(m) + addr, &v, 8);
}

/* WASI errno subset */
#define WASI_ESUCCESS 0
#define WASI_EBADF 8
#define WASI_EINVAL 28
#define WASI_EIO 29
#define WASI_ENOSYS 52

/* WASI clock ids */
#define WASI_CLOCK_REALTIME 0
#define WASI_CLOCK_MONOTONIC 1

/* ---- I/O backend (see wasm_host.h) ---- */
static int n_openat(void *c, int d, const char *p, int fl, mode_t m) {
  (void)c; return openat(d, p, fl, m);
}
static ssize_t n_read(void *c, int fd, void *b, size_t n) { (void)c; return read(fd, b, n); }
static ssize_t n_write(void *c, int fd, const void *b, size_t n) { (void)c; return write(fd, b, n); }
static off_t n_lseek(void *c, int fd, off_t o, int w) { (void)c; return lseek(fd, o, w); }
static int n_close(void *c, int fd) { (void)c; return close(fd); }
static int n_fstat(void *c, int fd, struct stat *s) { (void)c; return fstat(fd, s); }
static int n_statat(void *c, int d, const char *p, struct stat *s, int fl) {
  (void)c; return fstatat(d, p, s, fl);
}
static int n_accessat(void *c, int d, const char *p, int a, int fl) {
  (void)c; return faccessat(d, p, a, fl);
}
static const wasm_io_ops native_io_ops = {
    n_openat, n_read, n_write, n_lseek, n_close, n_fstat, n_statat, n_accessat};

static const wasm_io_ops *g_io = &native_io_ops;
static void *g_io_ctx;
void wasm_host_set_io(const wasm_io_ops *ops, void *ctx) {
  g_io = ops;
  g_io_ctx = ctx;
}

/* ---- in-memory io backend (TEXPRESSO_MEMFS test) ------------------------
 * Proves the io_ops seam fully replaces the native FS — exactly what the
 * texpresso state.c backend will do. Opened files (fd>=3) are served from
 * memory: input files are lazily read from disk once, then all reads come from
 * the buffer; writes are captured to memory and NEVER hit disk. fds 0/1/2 pass
 * through to native so engine messages still appear. */
typedef struct {
  char *path;
  uint8_t *data;
  size_t len, cap;
  int output; /* was written to (captured, not on disk) */
} memfile;
typedef struct {
  memfile f[512];
  int nf;
  struct { int fi, open; size_t pos; } fd[512];
} memfs_t;

static memfile *memfs_find(memfs_t *fs, const char *path) {
  for (int i = 0; i < fs->nf; i++)
    if (strcmp(fs->f[i].path, path) == 0) return &fs->f[i];
  return NULL;
}
static memfile *memfs_load(memfs_t *fs, const char *path, int for_write) {
  memfile *mf = memfs_find(fs, path);
  if (mf) return mf;
  if (fs->nf >= 512) return NULL;
  mf = &fs->f[fs->nf];
  memset(mf, 0, sizeof *mf);
  mf->path = strdup(path);
  if (!for_write) { /* lazily read the disk file into memory */
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    mf->cap = n > 0 ? (size_t)n : 1;
    mf->data = malloc(mf->cap);
    mf->len = fread(mf->data, 1, n > 0 ? (size_t)n : 0, fp);
    fclose(fp);
  } else {
    mf->output = 1;
  }
  fs->nf++;
  return mf;
}
static int mem_openat(void *c, int d, const char *p, int fl, mode_t m) {
  (void)d; (void)m;
  memfs_t *fs = c;
  int wr = (fl & (O_WRONLY | O_RDWR | O_CREAT)) != 0;
  memfile *mf = memfs_find(fs, p);
  if (!mf) mf = memfs_load(fs, p, wr);
  if (!mf) { errno = ENOENT; return -1; }
  if (fl & O_TRUNC) { mf->len = 0; mf->output = 1; }
  for (int fd = 3; fd < 512; fd++)
    if (!fs->fd[fd].open) {
      fs->fd[fd].open = 1;
      fs->fd[fd].fi = (int)(mf - fs->f);
      fs->fd[fd].pos = 0;
      return fd;
    }
  errno = EMFILE;
  return -1;
}
static ssize_t mem_read(void *c, int fd, void *buf, size_t n) {
  memfs_t *fs = c;
  if (fd < 3) return read(fd, buf, n);
  if (fd >= 512 || !fs->fd[fd].open) { errno = EBADF; return -1; }
  memfile *mf = &fs->f[fs->fd[fd].fi];
  size_t pos = fs->fd[fd].pos;
  if (pos > mf->len) pos = mf->len;
  size_t k = mf->len - pos;
  if (k > n) k = n;
  memcpy(buf, mf->data + pos, k);
  fs->fd[fd].pos = pos + k;
  return (ssize_t)k;
}
static ssize_t mem_write(void *c, int fd, const void *buf, size_t n) {
  memfs_t *fs = c;
  if (fd < 3) return write(fd, buf, n);
  if (fd >= 512 || !fs->fd[fd].open) { errno = EBADF; return -1; }
  memfile *mf = &fs->f[fs->fd[fd].fi];
  size_t pos = fs->fd[fd].pos;
  if (pos + n > mf->cap) {
    size_t nc = (pos + n) * 2 + 64;
    mf->data = realloc(mf->data, nc);
    mf->cap = nc;
  }
  memcpy(mf->data + pos, buf, n);
  if (pos + n > mf->len) mf->len = pos + n;
  fs->fd[fd].pos = pos + n;
  mf->output = 1;
  return (ssize_t)n;
}
static off_t mem_lseek(void *c, int fd, off_t off, int w) {
  memfs_t *fs = c;
  if (fd < 3) return lseek(fd, off, w);
  if (fd >= 512 || !fs->fd[fd].open) { errno = EBADF; return -1; }
  memfile *mf = &fs->f[fs->fd[fd].fi];
  off_t base = (w == SEEK_SET) ? 0 : (w == SEEK_CUR) ? (off_t)fs->fd[fd].pos
                                                     : (off_t)mf->len;
  off_t np = base + off;
  if (np < 0) { errno = EINVAL; return -1; }
  fs->fd[fd].pos = (size_t)np;
  return np;
}
static int mem_close(void *c, int fd) {
  memfs_t *fs = c;
  if (fd < 3) return close(fd);
  if (fd < 512) fs->fd[fd].open = 0;
  return 0;
}
static void memfs_fill_stat(memfile *mf, struct stat *s) {
  memset(s, 0, sizeof *s);
  s->st_mode = S_IFREG | 0644;
  s->st_nlink = 1;
  s->st_size = (off_t)mf->len;
  s->st_blksize = 4096;
  s->st_blocks = (mf->len + 511) / 512;
}
static int mem_fstat(void *c, int fd, struct stat *s) {
  memfs_t *fs = c;
  if (fd < 3) return fstat(fd, s);
  if (fd >= 512 || !fs->fd[fd].open) { errno = EBADF; return -1; }
  memfs_fill_stat(&fs->f[fs->fd[fd].fi], s);
  return 0;
}
static int mem_statat(void *c, int d, const char *p, struct stat *s, int fl) {
  memfs_t *fs = c;
  memfile *mf = memfs_find(fs, p);
  if (mf) { memfs_fill_stat(mf, s); return 0; }
  return fstatat(d, p, s, fl); /* existence probe (read-only) hits disk */
}
static int mem_accessat(void *c, int d, const char *p, int a, int fl) {
  memfs_t *fs = c;
  if (memfs_find(fs, p)) return 0;
  return faccessat(d, p, a, fl);
}
static const wasm_io_ops mem_io_ops = {mem_openat, mem_read,   mem_write,
                                       mem_lseek,  mem_close,  mem_fstat,
                                       mem_statat, mem_accessat};

/* Write captured (in-memory) outputs to <path>.memout for verification. */
static void memfs_dump(memfs_t *fs) {
  for (int i = 0; i < fs->nf; i++) {
    if (!fs->f[i].output) continue;
    char out[2048];
    snprintf(out, sizeof out, "%s.memout", fs->f[i].path);
    FILE *fp = fopen(out, "wb");
    if (fp) {
      fwrite(fs->f[i].data, 1, fs->f[i].len, fp);
      fclose(fp);
    }
    fprintf(stderr, "[memfs] captured %s (%zu bytes) -> %s\n", fs->f[i].path,
            fs->f[i].len, out);
  }
}

/* ------------------------- WASI imports ------------------------- */

/* iovec in wasm memory: { u32 buf; u32 buf_len } */
u32 w2c_wasi__snapshot__preview1_fd_read(struct w2c_wasi__snapshot__preview1 *w,
                                         u32 fd, u32 iovs, u32 iovs_len,
                                         u32 nread_ptr) {
  w2c_engine *m = w->mod;
  /* Snapshot point: suspend the engine back to the host before this read. */
  if (g_yield_next_read) {
    g_yield_next_read = 0;
    g_suspended = 1;
    engine_yield();      /* -> host; resumes here after the host swaps back */
    g_suspended = 0;
  }
  uint32_t total = 0;
  for (u32 i = 0; i < iovs_len; i++) {
    uint32_t base = rd_u32(m, iovs + i * 8);
    uint32_t len = rd_u32(m, iovs + i * 8 + 4);
    if (!mem_ok(m, base, len)) return WASI_EINVAL;
    ssize_t n = g_io->read(g_io_ctx, (int)fd, mem_base(m) + base, len);
    if (n < 0) return WASI_EIO;
    total += (uint32_t)n;
    if ((uint32_t)n < len) break; /* short read: stop */
  }
  wr_u32(m, nread_ptr, total);
  return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_write(struct w2c_wasi__snapshot__preview1 *w,
                                          u32 fd, u32 iovs, u32 iovs_len,
                                          u32 nwritten_ptr) {
  w2c_engine *m = w->mod;
  uint32_t total = 0;
  for (u32 i = 0; i < iovs_len; i++) {
    uint32_t base = rd_u32(m, iovs + i * 8);
    uint32_t len = rd_u32(m, iovs + i * 8 + 4);
    if (!mem_ok(m, base, len)) return WASI_EINVAL;
    /* Hash the output stream so the snapshot self-test can compare runs. */
    const uint8_t *p = mem_base(m) + base;
    for (uint32_t k = 0; k < len; k++) {
      g_run_hash ^= p[k];
      g_run_hash *= 0x100000001b3ull;
    }
    ssize_t n = g_io->write(g_io_ctx, (int)fd, p, len);
    if (n < 0) return WASI_EIO;
    total += (uint32_t)n;
    if ((uint32_t)n < len) break;
  }
  wr_u32(m, nwritten_ptr, total);
  return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1 *w,
                                         u32 fd, u64 offset, u32 whence,
                                         u32 newoff_ptr) {
  w2c_engine *m = w->mod;
  /* WASI whence: 0=SET,1=CUR,2=END — matches POSIX SEEK_* values. */
  off_t r = g_io->lseek(g_io_ctx, (int)fd, (off_t)offset, (int)whence);
  if (r < 0) return WASI_EBADF;
  wr_u64(m, newoff_ptr, (uint64_t)r);
  return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1 *w,
                                          u32 fd) {
  (void)w;
  if (g_defer_close) return WASI_ESUCCESS; /* rollback needs the fd alive */
  return g_io->close(g_io_ctx, (int)fd) < 0 ? WASI_EBADF : WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_args_sizes_get(
    struct w2c_wasi__snapshot__preview1 *w, u32 argc_ptr, u32 bufsize_ptr) {
  w2c_engine *m = w->mod;
  uint32_t bufsize = 0;
  for (int i = 0; i < g_argc; i++) bufsize += (uint32_t)strlen(g_argv[i]) + 1;
  wr_u32(m, argc_ptr, (uint32_t)g_argc);
  wr_u32(m, bufsize_ptr, bufsize);
  return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_args_get(struct w2c_wasi__snapshot__preview1 *w,
                                          u32 argv_ptr, u32 argbuf_ptr) {
  w2c_engine *m = w->mod;
  uint32_t buf = argbuf_ptr;
  for (int i = 0; i < g_argc; i++) {
    uint32_t len = (uint32_t)strlen(g_argv[i]) + 1;
    if (!mem_ok(m, buf, len)) return WASI_EINVAL;
    memcpy(mem_base(m) + buf, g_argv[i], len);
    wr_u32(m, argv_ptr + (uint32_t)i * 4, buf);
    buf += len;
  }
  return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_environ_sizes_get(
    struct w2c_wasi__snapshot__preview1 *w, u32 count_ptr, u32 bufsize_ptr) {
  w2c_engine *m = w->mod;
  extern char **environ;
  uint32_t count = 0, bufsize = 0;
  for (char **e = environ; *e; e++) {
    count++;
    bufsize += (uint32_t)strlen(*e) + 1;
  }
  wr_u32(m, count_ptr, count);
  wr_u32(m, bufsize_ptr, bufsize);
  return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_environ_get(
    struct w2c_wasi__snapshot__preview1 *w, u32 environ_ptr, u32 buf_ptr) {
  w2c_engine *m = w->mod;
  extern char **environ;
  /* Pass the host environment through (the engine reads ICU_DATA, TEXMF*, ...). */
  uint32_t buf = buf_ptr, i = 0;
  for (char **e = environ; *e; e++, i++) {
    uint32_t len = (uint32_t)strlen(*e) + 1;
    if (!mem_ok(m, buf, len)) return WASI_EINVAL;
    memcpy(mem_base(m) + buf, *e, len);
    wr_u32(m, environ_ptr + i * 4, buf);
    buf += len;
  }
  return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_clock_time_get(
    struct w2c_wasi__snapshot__preview1 *w, u32 clock_id, u64 precision,
    u32 time_ptr) {
  w2c_engine *m = w->mod;
  (void)precision;
  /* Virtualized: realtime -> fixed epoch; monotonic -> 0 (deterministic). */
  uint64_t ns = (clock_id == WASI_CLOCK_MONOTONIC)
                    ? 0ull
                    : (uint64_t)g_epoch * 1000000000ull;
  wr_u64(m, time_ptr, ns);
  return WASI_ESUCCESS;
}

/* When running under the coroutine, "exit" yields back to the host with a done
 * flag instead of terminating the process, so the host can snapshot/rollback. */
static int g_exit_code;
static void engine_exit(int code) {
  g_exit_code = code;
  g_engine_done = 1;
  if (g_engine_stack) { engine_yield(); return; } /* -> host, never resumes */
  exit(code);
}

void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1 *w,
                                            u32 code) {
  (void)w;
  engine_exit((int)code);
}

/* ------------------- env.__syscall_* (Linux ABI) -------------------
 * Return the syscall result; negative -errno on failure (emscripten's musl
 * translates that into errno). Stubs return -ENOSYS. */

static uint32_t neg_errno(int e) { return (uint32_t)(-e); }

/* Linux AT_FDCWD is -100; the host's differs (macOS -2). Translate. */
#define L_AT_FDCWD ((uint32_t)-100)
static int xlate_dirfd(uint32_t d) {
  return d == L_AT_FDCWD ? AT_FDCWD : (int)d;
}

u32 w2c_env_0x5F_syscall_getcwd(struct w2c_env *e, u32 buf, u32 size) {
  w2c_engine *m = e->mod;
  if (!mem_ok(m, buf, size)) return neg_errno(EFAULT);
  if (getcwd((char *)mem_base(m) + buf, size) == NULL) return neg_errno(errno);
  return (uint32_t)strlen((char *)mem_base(m) + buf) + 1;
}

u32 w2c_env_0x5F_syscall_getuid32(struct w2c_env *e) {
  (void)e;
  return (uint32_t)getuid();
}

/* defined further below; used by fstat64/mktime_js here */
static void write_estat(w2c_engine *m, uint32_t p, const struct stat *s);
static void write_tm(w2c_engine *m, uint32_t p, const struct tm *t);

/* ---- extra file/misc syscalls (luatex) ---- */
u32 w2c_env_0x5F_syscall_chdir(struct w2c_env *e, u32 path) {
  w2c_engine *m = e->mod;
  int r = chdir((const char *)mem_base(m) + path);
  return r < 0 ? neg_errno(errno) : 0;
}
u32 w2c_env_0x5F_syscall_dup(struct w2c_env *e, u32 fd) {
  (void)e;
  int r = dup((int)fd);
  return r < 0 ? neg_errno(errno) : (uint32_t)r;
}
u32 w2c_env_0x5F_syscall_dup3(struct w2c_env *e, u32 oldfd, u32 newfd, u32 flags) {
  (void)e; (void)flags; /* macOS lacks dup3; O_CLOEXEC flag ignored */
  int r = dup2((int)oldfd, (int)newfd);
  return r < 0 ? neg_errno(errno) : (uint32_t)r;
}
u32 w2c_env_0x5F_syscall_fstat64(struct w2c_env *e, u32 fd, u32 buf) {
  w2c_engine *m = e->mod;
  struct stat st;
  if (g_io->fstat(g_io_ctx, (int)fd, &st) < 0) return neg_errno(errno);
  write_estat(m, buf, &st);
  return 0;
}
u32 w2c_env_0x5F_syscall_mkdirat(struct w2c_env *e, u32 dirfd, u32 path, u32 mode) {
  w2c_engine *m = e->mod;
  int r = mkdirat(xlate_dirfd(dirfd), (const char *)mem_base(m) + path, (mode_t)mode);
  return r < 0 ? neg_errno(errno) : 0;
}
u32 w2c_env_0x5F_syscall_linkat(struct w2c_env *e, u32 od, u32 op, u32 nd, u32 np,
                                u32 flags) {
  w2c_engine *m = e->mod;
  int r = linkat(xlate_dirfd(od), (const char *)mem_base(m) + op, xlate_dirfd(nd),
                 (const char *)mem_base(m) + np, (int)flags);
  return r < 0 ? neg_errno(errno) : 0;
}
u32 w2c_env_0x5F_syscall_symlinkat(struct w2c_env *e, u32 target, u32 nd, u32 lp) {
  w2c_engine *m = e->mod;
  int r = symlinkat((const char *)mem_base(m) + target, xlate_dirfd(nd),
                    (const char *)mem_base(m) + lp);
  return r < 0 ? neg_errno(errno) : 0;
}
u32 w2c_env_0x5F_syscall_utimensat(struct w2c_env *e, u32 dirfd, u32 path,
                                   u32 times, u32 flags) {
  (void)e; (void)dirfd; (void)path; (void)times; (void)flags;
  return 0; /* accept + ignore: file mtimes don't affect typesetting */
}

/* Time: fill/normalize a wasm struct tm and return time_t (see write_tm). */
u64 w2c_env_0x5Fmktime_js(struct w2c_env *e, u32 tm_ptr) {
  w2c_engine *m = e->mod;
  int32_t v[9];
  memcpy(v, mem_base(m) + tm_ptr, sizeof v);
  struct tm t;
  memset(&t, 0, sizeof t);
  t.tm_sec = v[0]; t.tm_min = v[1]; t.tm_hour = v[2]; t.tm_mday = v[3];
  t.tm_mon = v[4]; t.tm_year = v[5]; t.tm_wday = v[6]; t.tm_yday = v[7];
  t.tm_isdst = v[8];
  time_t r = mktime(&t);
  write_tm(m, tm_ptr, &t); /* mktime normalizes the fields */
  return (u64)(int64_t)r;
}

u32 w2c_env_emscripten_get_heap_max(struct w2c_env *e) {
  (void)e;
  return 0x80000000u; /* 2 GiB ceiling */
}
void w2c_env_0x5F_call_sighandler(struct w2c_env *e, u32 handler, u32 sig) {
  (void)e; (void)handler; (void)sig;
}
void w2c_env_0x5Femscripten_runtime_keepalive_clear(struct w2c_env *e) { (void)e; }

/* Networking (luasocket): unsupported — typesetting never needs a socket. */
u32 w2c_env_0x5Femscripten_lookup_name(struct w2c_env *e, u32 name) {
  (void)e; (void)name; return 0;
}
u32 w2c_env_getaddrinfo(struct w2c_env *e, u32 a, u32 b, u32 c, u32 d) {
  (void)e; (void)a; (void)b; (void)c; (void)d; return (uint32_t)(-1);
}
u32 w2c_env_getnameinfo(struct w2c_env *e, u32 a, u32 b, u32 c, u32 d, u32 f,
                        u32 g, u32 h) {
  (void)e; (void)a; (void)b; (void)c; (void)d; (void)f; (void)g; (void)h;
  return (uint32_t)(-1);
}
#define NET_STUB6(nm)                                                          \
  u32 w2c_env_0x5F_syscall_##nm(struct w2c_env *e, u32 a, u32 b, u32 c, u32 d, \
                                u32 f, u32 g) {                                \
    (void)e; (void)a; (void)b; (void)c; (void)d; (void)f; (void)g;            \
    return neg_errno(ENOSYS);                                                  \
  }
NET_STUB6(bind) NET_STUB6(accept4) NET_STUB6(listen) NET_STUB6(recvfrom)
NET_STUB6(sendto) NET_STUB6(getpeername) NET_STUB6(getsockname)
NET_STUB6(getsockopt) NET_STUB6(setsockopt) NET_STUB6(shutdown)
#undef NET_STUB6
u32 w2c_env_0x5F_syscall_poll(struct w2c_env *e, u32 fds, u32 nfds, u32 timeout) {
  (void)e; (void)fds; (void)nfds; (void)timeout; return 0; /* no fds ready */
}
u32 w2c_env_0x5F_syscall_poll_nonblocking(struct w2c_env *e, u32 a, u32 b) {
  (void)e; (void)a; (void)b; return 0;
}

/* fd_sync -> fsync */
u32 w2c_wasi__snapshot__preview1_fd_sync(struct w2c_wasi__snapshot__preview1 *w,
                                         u32 fd) {
  (void)w;
  fsync((int)fd);
  return WASI_ESUCCESS;
}

u32 w2c_env_0x5F_syscall_faccessat(struct w2c_env *e, u32 dirfd, u32 path,
                                   u32 amode, u32 flags) {
  w2c_engine *m = e->mod;
  const char *p = (const char *)mem_base(m) + path;
  int r = g_io->accessat(g_io_ctx, xlate_dirfd(dirfd), p, (int)amode, (int)flags);
  return r < 0 ? neg_errno(errno) : 0;
}

u32 w2c_env_0x5F_syscall_readlinkat(struct w2c_env *e, u32 dirfd, u32 path,
                                    u32 buf, u32 bufsize) {
  w2c_engine *m = e->mod;
  const char *p = (const char *)mem_base(m) + path;
  ssize_t r = readlinkat(xlate_dirfd(dirfd), p, (char *)mem_base(m) + buf, bufsize);
  return r < 0 ? neg_errno(errno) : (uint32_t)r;
}

u32 w2c_env_0x5F_syscall_unlinkat(struct w2c_env *e, u32 dirfd, u32 path,
                                  u32 flags) {
  w2c_engine *m = e->mod;
  const char *p = (const char *)mem_base(m) + path;
  int r = unlinkat(xlate_dirfd(dirfd), p, (int)flags);
  return r < 0 ? neg_errno(errno) : 0;
}

u32 w2c_env_0x5F_syscall_rmdir(struct w2c_env *e, u32 path) {
  w2c_engine *m = e->mod;
  int r = rmdir((const char *)mem_base(m) + path);
  return r < 0 ? neg_errno(errno) : 0;
}

u32 w2c_env_0x5F_syscall_renameat(struct w2c_env *e, u32 od, u32 op, u32 nd,
                                  u32 np) {
  w2c_engine *m = e->mod;
  int r = renameat(xlate_dirfd(od), (const char *)mem_base(m) + op, xlate_dirfd(nd),
                   (const char *)mem_base(m) + np);
  return r < 0 ? neg_errno(errno) : 0;
}

/* getdents64 / pipe2 / socket / connect / system: not needed to run pdftex;
 * shell escape and networking are intentionally unsupported. */
u32 w2c_env_0x5F_syscall_getdents64(struct w2c_env *e, u32 a, u32 b, u32 c) {
  (void)e; (void)a; (void)b; (void)c;
  return neg_errno(ENOSYS);
}
u32 w2c_env_0x5F_syscall_pipe2(struct w2c_env *e, u32 a, u32 b) {
  (void)e; (void)a; (void)b;
  return neg_errno(ENOSYS);
}
u32 w2c_env_0x5F_syscall_socket(struct w2c_env *e, u32 a, u32 b, u32 c, u32 d,
                                u32 f, u32 g) {
  (void)e; (void)a; (void)b; (void)c; (void)d; (void)f; (void)g;
  return neg_errno(ENOSYS);
}
u32 w2c_env_0x5F_syscall_connect(struct w2c_env *e, u32 a, u32 b, u32 c, u32 d,
                                 u32 f, u32 g) {
  (void)e; (void)a; (void)b; (void)c; (void)d; (void)f; (void)g;
  return neg_errno(ENOSYS);
}
u32 w2c_env_0x5Femscripten_system(struct w2c_env *e, u32 cmd) {
  (void)e; (void)cmd;
  return (uint32_t)-1; /* shell escape disabled */
}

/* ---------------- file operations (the VFS hook) ----------------
 * The engine uses Linux O_* flag values and expects stat results in
 * emscripten's struct-stat layout. Translate both. */

/* Linux/i386 O_* values as seen from the wasm side. */
#define L_O_ACCMODE 03
#define L_O_CREAT 0100
#define L_O_EXCL 0200
#define L_O_NOCTTY 0400
#define L_O_TRUNC 01000
#define L_O_APPEND 02000
#define L_O_NONBLOCK 04000
#define L_O_DIRECTORY 0200000
#define L_O_CLOEXEC 02000000

static int xlate_open_flags(uint32_t lf) {
  int hf = (int)(lf & L_O_ACCMODE); /* RDONLY/WRONLY/RDWR share 0/1/2 */
  if (lf & L_O_CREAT) hf |= O_CREAT;
  if (lf & L_O_EXCL) hf |= O_EXCL;
  if (lf & L_O_NOCTTY) hf |= O_NOCTTY;
  if (lf & L_O_TRUNC) hf |= O_TRUNC;
  if (lf & L_O_APPEND) hf |= O_APPEND;
  if (lf & L_O_NONBLOCK) hf |= O_NONBLOCK;
  if (lf & L_O_DIRECTORY) hf |= O_DIRECTORY;
  if (lf & L_O_CLOEXEC) hf |= O_CLOEXEC;
  return hf;
}

/* emscripten struct stat offsets (extracted from the toolchain). */
static void write_estat(w2c_engine *m, uint32_t p, const struct stat *s) {
  uint8_t *b = mem_base(m) + p;
  memset(b, 0, 96);
#define PUT32(off, v) do { uint32_t _v = (uint32_t)(v); memcpy(b + (off), &_v, 4); } while (0)
#define PUT64(off, v) do { uint64_t _v = (uint64_t)(v); memcpy(b + (off), &_v, 8); } while (0)
  PUT32(0, s->st_dev);
  PUT32(4, s->st_mode);
  PUT32(8, s->st_nlink);
  PUT32(12, s->st_uid);
  PUT32(16, s->st_gid);
  PUT32(20, s->st_rdev);
  PUT64(24, s->st_size);
  PUT32(32, s->st_blksize);
  PUT32(36, s->st_blocks);
  PUT64(40, s->st_atime);       /* atim.tv_sec */
  PUT64(56, s->st_mtime);       /* mtim.tv_sec */
  PUT64(72, s->st_ctime);       /* ctim.tv_sec */
  PUT64(88, s->st_ino);
#undef PUT32
#undef PUT64
}

u32 w2c_env_0x5F_syscall_openat(struct w2c_env *e, u32 dirfd, u32 path,
                                u32 flags, u32 varargs) {
  /* Emscripten packs variadic syscall args into memory and passes a pointer:
   * __syscall_openat(dirfd, path, flags, varargs) reads mode from *varargs. */
  w2c_engine *m = e->mod;
  const char *p = (const char *)mem_base(m) + path;
  mode_t cmode = 0;
  if ((flags & L_O_CREAT) && mem_ok(m, varargs, 4)) cmode = (mode_t)rd_u32(m, varargs);
  int r = g_io->openat(g_io_ctx, xlate_dirfd(dirfd), p, xlate_open_flags(flags), cmode);
  if (g_trace) fprintf(stderr, "[openat] '%s' flags=%#x -> %d (%s)\n", p, flags, r,
                       r < 0 ? strerror(errno) : "ok");
  return r < 0 ? neg_errno(errno) : (uint32_t)r;
}

u32 w2c_env_0x5F_syscall_stat64(struct w2c_env *e, u32 path, u32 buf) {
  w2c_engine *m = e->mod;
  struct stat s;
  if (g_io->statat(g_io_ctx, AT_FDCWD, (const char *)mem_base(m) + path, &s, 0) < 0)
    return neg_errno(errno);
  write_estat(m, buf, &s);
  return 0;
}

u32 w2c_env_0x5F_syscall_lstat64(struct w2c_env *e, u32 path, u32 buf) {
  w2c_engine *m = e->mod;
  struct stat s;
  if (g_io->statat(g_io_ctx, AT_FDCWD, (const char *)mem_base(m) + path, &s,
                   AT_SYMLINK_NOFOLLOW) < 0)
    return neg_errno(errno);
  write_estat(m, buf, &s);
  return 0;
}

u32 w2c_env_0x5F_syscall_newfstatat(struct w2c_env *e, u32 dirfd, u32 path,
                                    u32 buf, u32 flags) {
  w2c_engine *m = e->mod;
  int hf = 0;
  if (flags & 0x100 /* AT_SYMLINK_NOFOLLOW (Linux) */) hf |= AT_SYMLINK_NOFOLLOW;
  if (flags & 0x1000 /* AT_EMPTY_PATH */) hf |= AT_SYMLINK_NOFOLLOW; /* best effort */
  struct stat s;
  if (g_io->statat(g_io_ctx, xlate_dirfd(dirfd), (const char *)mem_base(m) + path,
                   &s, hf) < 0)
    return neg_errno(errno);
  write_estat(m, buf, &s);
  return 0;
}

u32 w2c_env_0x5F_syscall_fcntl64(struct w2c_env *e, u32 fd, u32 cmd, u32 arg) {
  (void)e;
  /* TeX mainly probes F_GETFL/F_SETFD; passthrough the common ones. */
  int r = fcntl((int)fd, (int)cmd, (long)arg);
  return r < 0 ? neg_errno(errno) : (uint32_t)r;
}

u32 w2c_env_0x5F_syscall_ioctl(struct w2c_env *e, u32 fd, u32 req, u32 arg) {
  (void)e; (void)fd; (void)req; (void)arg;
  return neg_errno(ENOTTY); /* not a tty; TeX handles this gracefully */
}

/* ---------------- time / abort (emscripten "_js" imports) ---------------- */

/* struct tm in wasm: 9 ints then tm_gmtoff(long) + tm_zone(ptr). */
static void write_tm(w2c_engine *m, uint32_t p, const struct tm *t) {
  uint8_t *b = mem_base(m) + p;
  int32_t v[9] = {t->tm_sec,  t->tm_min,  t->tm_hour,
                  t->tm_mday, t->tm_mon,  t->tm_year,
                  t->tm_wday, t->tm_yday, t->tm_isdst};
  memcpy(b, v, sizeof(v));
}

u32 w2c_env_0x5Fgmtime_js(struct w2c_env *e, u64 t, u32 tmptr) {
  w2c_engine *m = e->mod;
  time_t tt = (time_t)t;
  struct tm r;
  gmtime_r(&tt, &r);
  write_tm(m, tmptr, &r);
  return 0;
}

u32 w2c_env_0x5Flocaltime_js(struct w2c_env *e, u64 t, u32 tmptr) {
  w2c_engine *m = e->mod;
  time_t tt = (time_t)t;
  struct tm r;
  localtime_r(&tt, &r);
  write_tm(m, tmptr, &r);
  return 0;
}

void w2c_env_0x5Ftzset_js(struct w2c_env *e, u32 tz, u32 dst, u32 tzname1,
                          u32 tzname2) {
  (void)e; (void)tz; (void)dst; (void)tzname1; (void)tzname2;
  tzset();
}

void w2c_env_0x5Fabort_js(struct w2c_env *e) {
  (void)e;
  abort();
}

/* ---------------- process / heap / assert ---------------- */

void w2c_env_exit(struct w2c_env *e, u32 code) {
  (void)e;
  engine_exit((int)code);
}

void w2c_env_0x5F_assert_fail(struct w2c_env *e, u32 cond, u32 file, u32 line,
                              u32 func) {
  w2c_engine *m = e->mod;
  fprintf(stderr, "assertion failed: %s (%s:%u, %s)\n",
          (const char *)mem_base(m) + cond, (const char *)mem_base(m) + file,
          line, (const char *)mem_base(m) + func);
  abort();
}

f64 w2c_env_emscripten_date_now(struct w2c_env *e) {
  (void)e;
  return (double)g_epoch * 1000.0; /* virtualized wall clock (fixed) */
}

f64 w2c_env_emscripten_get_now(struct w2c_env *e) {
  (void)e;
  return 0.0; /* virtualized monotonic clock (deterministic) */
}

/* Grow the linear memory to at least `requested` bytes. */
u32 w2c_env_emscripten_resize_heap(struct w2c_env *e, u32 requested) {
  w2c_engine *m = e->mod;
  wasm_rt_memory_t *mem = &m->w2c_memory;
  if ((uint64_t)requested <= mem->size) return 1;
  uint64_t need_pages = ((uint64_t)requested - mem->size + 65535) / 65536;
  uint64_t r = wasm_rt_grow_memory(mem, need_pages);
  return (r == (uint64_t)-1) ? 0 : 1;
}

/* Engines that never mmap (pdftex) don't export memalign. Provide a weak
 * fallback so the shared host links; the engine's strong definition (xetex)
 * overrides it, and pdftex never reaches mmap_js anyway. */
u32 __attribute__((weak))
w2c_engine_emscripten_builtin_memalign(w2c_engine *m, u32 align, u32 size) {
  (void)m; (void)align; (void)size;
  return 0;
}

/* File-backed mmap (xetex needs it for ICU data + fonts; ICU has no fallback).
 * Allocate inside wasm memory via the engine's allocator and read the file
 * region into it. Anonymous mmap (fd < 0) is handled inside musl — decline it. */
u32 w2c_env_0x5Fmmap_js(struct w2c_env *e, u32 len, u32 prot, u32 flags, u32 fd,
                        u64 offset, u32 allocated_ptr, u32 addr_ptr) {
  w2c_engine *m = e->mod;
  (void)prot; (void)flags;
  if (g_trace) fprintf(stderr, "[mmap_js] fd=%d len=%u off=%llu\n", (int)fd, len,
                       (unsigned long long)offset);
  if ((int)fd < 0) return (uint32_t)(-38); /* -ENOSYS: let musl do anon */
  uint32_t ptr = w2c_engine_emscripten_builtin_memalign(m, 65536, len);
  if (!ptr) return (uint32_t)(-12); /* -ENOMEM */
  /* pread via the io backend (fd may be a VFS handle, not a native fd). */
  off_t save = g_io->lseek(g_io_ctx, (int)fd, 0, SEEK_CUR);
  g_io->lseek(g_io_ctx, (int)fd, (off_t)offset, SEEK_SET);
  ssize_t n = g_io->read(g_io_ctx, (int)fd, mem_base(m) + ptr, len);
  if (save >= 0) g_io->lseek(g_io_ctx, (int)fd, save, SEEK_SET);
  if (n < 0) return (uint32_t)(-5); /* -EIO */
  if ((uint32_t)n < len) memset(mem_base(m) + ptr + (uint32_t)n, 0, len - (uint32_t)n);
  wr_u32(m, addr_ptr, ptr);
  wr_u32(m, allocated_ptr, 1); /* we allocated -> munmap should free (we leak) */
  return 0;
}

u32 w2c_env_0x5Fmunmap_js(struct w2c_env *e, u32 addr, u32 len, u32 prot,
                          u32 flags, u32 fd, u64 offset) {
  (void)e; (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)offset;
  return 0; /* free not exported by the module; mappings are few and long-lived */
}

/* ---------------- snapshot / rollback (userland COW) ----------------
 * The linear memory is snapshotted by mprotect dirty-tracking: at snapshot the
 * region is marked read-only; a SIGSEGV/SIGBUS handler saves each page's
 * original bytes on first write and re-marks it writable. Rollback copies the
 * saved (dirty) pages back — cost is O(pages written), not O(heap size). The
 * engine stack (small) + register context + open-fd positions are full-copy.
 * The memory base is fixed (WASM_RT_USE_MMAP), so the protected region is stable
 * across grows. */
#define FNV_INIT 1469598103934665603ull
static wasm_rt_memory_t g_snap_meminfo;
static uint8_t *g_snap_stack;
static ucontext_t g_snap_ctx;
static off_t g_snap_fdpos[64];

/* mprotect COW state for the linear memory */
static long g_pg;              /* system page size */
static uint8_t *g_cow_base;    /* = w2c_memory.data (fixed) */
static uint64_t g_cow_size;    /* protected region size (snapshot size) */
static uint8_t *g_cow_shadow;  /* saved original pages (size g_cow_size) */
static uint8_t *g_cow_saved;   /* bitmap: page saved this snapshot */
static volatile sig_atomic_t g_cow_active;
static volatile sig_atomic_t g_cow_dirty; /* pages copied this snapshot */

static void cow_fault(int sig, siginfo_t *si, void *uctx) {
  (void)uctx;
  uintptr_t a = (uintptr_t)si->si_addr;
  uintptr_t b = (uintptr_t)g_cow_base;
  if (g_cow_active && a >= b && a < b + g_cow_size) {
    uintptr_t off = (a - b) & ~(uintptr_t)(g_pg - 1);
    uint64_t pi = off / (uint64_t)g_pg;
    if (!(g_cow_saved[pi >> 3] & (1u << (pi & 7)))) {
      memcpy(g_cow_shadow + off, g_cow_base + off, (size_t)g_pg); /* save */
      g_cow_saved[pi >> 3] |= (uint8_t)(1u << (pi & 7));
      g_cow_dirty++;
    }
    mprotect(g_cow_base + off, (size_t)g_pg, PROT_READ | PROT_WRITE);
    return; /* retry the faulting write */
  }
  signal(sig, SIG_DFL); /* not ours: crash as usual */
  raise(sig);
}

static void cow_install_handler(void) {
  static int done;
  if (done) return;
  done = 1;
  g_pg = sysconf(_SC_PAGESIZE);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = cow_fault;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL); /* macOS raises SIGBUS on protected writes */
}

static void snapshot_take(w2c_engine *m) {
  cow_install_handler();
  g_snap_meminfo = m->w2c_memory;
  g_snap_stack = malloc(ENGINE_STACK_SIZE);
  memcpy(g_snap_stack, g_engine_stack, ENGINE_STACK_SIZE);
  g_snap_ctx = g_engine_ctx;
  for (int fd = 0; fd < 64; fd++) g_snap_fdpos[fd] = lseek(fd, 0, SEEK_CUR);

  g_cow_base = m->w2c_memory.data;
  g_cow_size = m->w2c_memory.size; /* multiple of the 64K wasm page */
  g_cow_shadow = malloc(g_cow_size);
  g_cow_saved = calloc((g_cow_size / (uint64_t)g_pg + 7) / 8, 1);
  g_cow_dirty = 0;
  mprotect(g_cow_base, g_cow_size, PROT_READ);
  g_cow_active = 1;
}

static void snapshot_restore(w2c_engine *m) {
  g_cow_active = 0;
  mprotect(g_cow_base, g_cow_size, PROT_READ | PROT_WRITE);
  uint64_t npages = g_cow_size / (uint64_t)g_pg;
  for (uint64_t pi = 0; pi < npages; pi++) /* restore dirtied pages */
    if (g_cow_saved[pi >> 3] & (1u << (pi & 7)))
      memcpy(g_cow_base + pi * (uint64_t)g_pg, g_cow_shadow + pi * (uint64_t)g_pg,
             (size_t)g_pg);
  uint64_t cur = m->w2c_memory.size;
  if (cur > g_cow_size) /* zero pages the run grew into */
    memset(g_cow_base + g_cow_size, 0, cur - g_cow_size);
  m->w2c_memory = g_snap_meminfo; /* data base is fixed (mmap) */

  memcpy(g_engine_stack, g_snap_stack, ENGINE_STACK_SIZE);
  g_engine_ctx = g_snap_ctx;
  for (int fd = 0; fd < 64; fd++)
    if (g_snap_fdpos[fd] != (off_t)-1) lseek(fd, g_snap_fdpos[fd], SEEK_SET);

  free(g_cow_shadow); g_cow_shadow = NULL;
  free(g_cow_saved); g_cow_saved = NULL;
  free(g_snap_stack); g_snap_stack = NULL;
}

/* ------------------------------ main ------------------------------ */

int main(int argc, char **argv) {
  g_argc = argc;
  g_argv = argv;
  g_trace = getenv("TEXPRESSO_TRACE") != NULL;
  const char *sde = getenv("SOURCE_DATE_EPOCH");
  g_epoch = sde ? atoll(sde) : (long long)time(NULL); /* fixed for the process */
  memfs_t *memfs = NULL;
  if (getenv("TEXPRESSO_MEMFS")) {
    memfs = calloc(1, sizeof *memfs);
    wasm_host_set_io(&mem_io_ops, memfs); /* serve everything from memory */
  }

  wasm_rt_init();

  w2c_engine mod;
  struct w2c_env env;
  struct w2c_wasi__snapshot__preview1 wasi;
  env.mod = &mod;
  wasi.mod = &mod;

  wasm2c_engine_instantiate(&mod, &env, &wasi);
  w2c_engine_0x5F_wasm_call_ctors(&mod); /* run static constructors first */

  /* Build argv[] inside wasm memory and call __main_argc_argv.
   * Layout: argc+1 pointers (NULL-terminated) followed by the strings. */
  uint32_t ptrbytes = (uint32_t)(argc + 1) * 4;
  uint32_t strbytes = 0;
  for (int i = 0; i < argc; i++) strbytes += (uint32_t)strlen(argv[i]) + 1;
  uint32_t total = (ptrbytes + strbytes + 15u) & ~15u;
  uint32_t base = w2c_engine_0x5Femscripten_stack_alloc(&mod, total);
  uint32_t sp = base + ptrbytes;
  for (int i = 0; i < argc; i++) {
    uint32_t len = (uint32_t)strlen(argv[i]) + 1;
    memcpy(mem_base(&mod) + sp, argv[i], len);
    wr_u32(&mod, base + (uint32_t)i * 4, sp);
    sp += len;
  }
  wr_u32(&mod, base + (uint32_t)argc * 4, 0); /* argv[argc] = NULL */

  /* Run the engine on a dedicated fixed-address stack so its execution state
   * (call stack) lives in a region we can snapshot. */
  g_mod = &mod;
  g_entry_argc = argc;
  g_entry_argv = base;
  g_engine_stack = mmap(NULL, ENGINE_STACK_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (g_engine_stack == MAP_FAILED) { perror("mmap engine stack"); return 1; }

  getcontext(&g_engine_ctx);
  g_engine_ctx.uc_stack.ss_sp = g_engine_stack;
  g_engine_ctx.uc_stack.ss_size = ENGINE_STACK_SIZE;
  g_engine_ctx.uc_link = &g_host_ctx;
  makecontext(&g_engine_ctx, engine_trampoline, 0);

  int snap_test = getenv("TEXPRESSO_SNAPSHOT_TEST") != NULL;
  /* Both tests suspend on the first read; the snapshot test rolls back there. */
  if (snap_test || getenv("TEXPRESSO_SUSPEND_TEST")) g_yield_next_read = 1;

  swapcontext(&g_host_ctx, &g_engine_ctx); /* run until first yield or done */

  if (g_suspended && snap_test) {
    g_defer_close = 1;
    snapshot_take(&mod);
    g_run_hash = FNV_INIT;
    swapcontext(&g_host_ctx, &g_engine_ctx); /* run A -> completion */
    unsigned long long hashA = g_run_hash;
    fprintf(stderr,
            "[host] run A done=%d out-hash=%016llx (COW: %d/%llu pages dirtied)\n",
            g_engine_done, hashA, (int)g_cow_dirty,
            (unsigned long long)(g_cow_size / (uint64_t)g_pg));

    snapshot_restore(&mod);
    g_run_hash = FNV_INIT;
    g_engine_done = 0;
    swapcontext(&g_host_ctx, &g_engine_ctx); /* run B from the snapshot */
    unsigned long long hashB = g_run_hash;
    fprintf(stderr, "[host] run B done=%d out-hash=%016llx\n", g_engine_done,
            hashB);

    fprintf(stderr, "[host] SNAPSHOT ROLLBACK %s (A=%016llx B=%016llx)\n",
            hashA == hashB ? "PASS" : "FAIL", hashA, hashB);
  } else if (g_suspended) {
    fprintf(stderr, "[host] engine suspended in fd_read; resuming\n");
    swapcontext(&g_host_ctx, &g_engine_ctx); /* resume to completion */
    fprintf(stderr, "[host] engine done=%d\n", g_engine_done);
  } else {
    fprintf(stderr, "[host] engine done=%d\n", g_engine_done);
  }

  if (memfs) memfs_dump(memfs); /* prove output was captured in memory */

  wasm2c_engine_free(&mod);
  wasm_rt_free();
  return 0;
}
