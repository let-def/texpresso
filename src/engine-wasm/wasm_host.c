/*
 * Native host for the wasm2c-compiled pdftex engine.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "wasm-rt.h"
#include "pdftex.h" /* wasm2c-generated header */

/* Import module structs: hold a back-pointer to the instance so imports can
 * reach its linear memory. */
struct w2c_env {
  w2c_pdftex *mod;
};
struct w2c_wasi__snapshot__preview1 {
  w2c_pdftex *mod;
};

/* argv the wasm engine sees via WASI args_get. Set in main(). */
static int g_argc;
static char **g_argv;

/* ---- coroutine: run the engine on a dedicated fixed-address stack ---- */
#define ENGINE_STACK_SIZE (32u * 1024u * 1024u)
static ucontext_t g_host_ctx;   /* where the engine yields back to */
static ucontext_t g_engine_ctx; /* the engine's suspended state */
static void *g_engine_stack;
static w2c_pdftex *g_mod;
static uint32_t g_entry_argv;
static int g_entry_argc;
static int g_engine_done;   /* engine returned/exited */
static int g_yield_next_read; /* request: suspend on the next fd_read */
static int g_suspended;     /* engine is currently suspended in fd_read */

/* Switch from engine back to host (called from imports on the engine stack). */
static void engine_yield(void) { swapcontext(&g_engine_ctx, &g_host_ctx); }

static void engine_trampoline(void) {
  w2c_pdftex_0x5F_main_argc_argv(g_mod, (u32)g_entry_argc, g_entry_argv);
  g_engine_done = 1;
  /* uc_link returns us to the host */
}

/* ---- linear-memory access (wasm is little-endian; host arm64/x86 too) ---- */

static uint8_t *mem_base(w2c_pdftex *m) { return m->w2c_memory.data; }
static uint64_t mem_size(w2c_pdftex *m) { return m->w2c_memory.size; }

static int mem_ok(w2c_pdftex *m, uint32_t addr, uint32_t len) {
  return (uint64_t)addr + len <= mem_size(m);
}
static uint32_t rd_u32(w2c_pdftex *m, uint32_t addr) {
  uint32_t v = 0;
  memcpy(&v, mem_base(m) + addr, 4);
  return v;
}
static void wr_u32(w2c_pdftex *m, uint32_t addr, uint32_t v) {
  memcpy(mem_base(m) + addr, &v, 4);
}
static void wr_u64(w2c_pdftex *m, uint32_t addr, uint64_t v) {
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

/* ------------------------- WASI imports ------------------------- */

/* iovec in wasm memory: { u32 buf; u32 buf_len } */
u32 w2c_wasi__snapshot__preview1_fd_read(struct w2c_wasi__snapshot__preview1 *w,
                                         u32 fd, u32 iovs, u32 iovs_len,
                                         u32 nread_ptr) {
  w2c_pdftex *m = w->mod;
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
    ssize_t n = read((int)fd, mem_base(m) + base, len);
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
  w2c_pdftex *m = w->mod;
  uint32_t total = 0;
  for (u32 i = 0; i < iovs_len; i++) {
    uint32_t base = rd_u32(m, iovs + i * 8);
    uint32_t len = rd_u32(m, iovs + i * 8 + 4);
    if (!mem_ok(m, base, len)) return WASI_EINVAL;
    ssize_t n = write((int)fd, mem_base(m) + base, len);
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
  w2c_pdftex *m = w->mod;
  /* WASI whence: 0=SET,1=CUR,2=END — matches POSIX SEEK_* values. */
  off_t r = lseek((int)fd, (off_t)offset, (int)whence);
  if (r < 0) return WASI_EBADF;
  wr_u64(m, newoff_ptr, (uint64_t)r);
  return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1 *w,
                                          u32 fd) {
  (void)w;
  return close((int)fd) < 0 ? WASI_EBADF : WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_args_sizes_get(
    struct w2c_wasi__snapshot__preview1 *w, u32 argc_ptr, u32 bufsize_ptr) {
  w2c_pdftex *m = w->mod;
  uint32_t bufsize = 0;
  for (int i = 0; i < g_argc; i++) bufsize += (uint32_t)strlen(g_argv[i]) + 1;
  wr_u32(m, argc_ptr, (uint32_t)g_argc);
  wr_u32(m, bufsize_ptr, bufsize);
  return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_args_get(struct w2c_wasi__snapshot__preview1 *w,
                                          u32 argv_ptr, u32 argbuf_ptr) {
  w2c_pdftex *m = w->mod;
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
  w2c_pdftex *m = w->mod;
  wr_u32(m, count_ptr, 0);
  wr_u32(m, bufsize_ptr, 0);
  return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_environ_get(
    struct w2c_wasi__snapshot__preview1 *w, u32 environ_ptr, u32 buf_ptr) {
  (void)w;
  (void)environ_ptr;
  (void)buf_ptr;
  return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_clock_time_get(
    struct w2c_wasi__snapshot__preview1 *w, u32 clock_id, u64 precision,
    u32 time_ptr) {
  w2c_pdftex *m = w->mod;
  (void)precision;
  struct timespec ts;
  clockid_t c = (clock_id == WASI_CLOCK_MONOTONIC) ? CLOCK_MONOTONIC
                                                   : CLOCK_REALTIME;
  if (clock_gettime(c, &ts) != 0) return WASI_EINVAL;
  uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  wr_u64(m, time_ptr, ns);
  return WASI_ESUCCESS;
}

void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1 *w,
                                            u32 code) {
  (void)w;
  exit((int)code);
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
  w2c_pdftex *m = e->mod;
  if (!mem_ok(m, buf, size)) return neg_errno(EFAULT);
  if (getcwd((char *)mem_base(m) + buf, size) == NULL) return neg_errno(errno);
  return (uint32_t)strlen((char *)mem_base(m) + buf) + 1;
}

u32 w2c_env_0x5F_syscall_getuid32(struct w2c_env *e) {
  (void)e;
  return (uint32_t)getuid();
}

u32 w2c_env_0x5F_syscall_faccessat(struct w2c_env *e, u32 dirfd, u32 path,
                                   u32 amode, u32 flags) {
  w2c_pdftex *m = e->mod;
  const char *p = (const char *)mem_base(m) + path;
  int r = faccessat(xlate_dirfd(dirfd), p, (int)amode, (int)flags);
  return r < 0 ? neg_errno(errno) : 0;
}

u32 w2c_env_0x5F_syscall_readlinkat(struct w2c_env *e, u32 dirfd, u32 path,
                                    u32 buf, u32 bufsize) {
  w2c_pdftex *m = e->mod;
  const char *p = (const char *)mem_base(m) + path;
  ssize_t r = readlinkat(xlate_dirfd(dirfd), p, (char *)mem_base(m) + buf, bufsize);
  return r < 0 ? neg_errno(errno) : (uint32_t)r;
}

u32 w2c_env_0x5F_syscall_unlinkat(struct w2c_env *e, u32 dirfd, u32 path,
                                  u32 flags) {
  w2c_pdftex *m = e->mod;
  const char *p = (const char *)mem_base(m) + path;
  int r = unlinkat(xlate_dirfd(dirfd), p, (int)flags);
  return r < 0 ? neg_errno(errno) : 0;
}

u32 w2c_env_0x5F_syscall_rmdir(struct w2c_env *e, u32 path) {
  w2c_pdftex *m = e->mod;
  int r = rmdir((const char *)mem_base(m) + path);
  return r < 0 ? neg_errno(errno) : 0;
}

u32 w2c_env_0x5F_syscall_renameat(struct w2c_env *e, u32 od, u32 op, u32 nd,
                                  u32 np) {
  w2c_pdftex *m = e->mod;
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
static void write_estat(w2c_pdftex *m, uint32_t p, const struct stat *s) {
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
                                u32 flags, u32 mode) {
  w2c_pdftex *m = e->mod;
  const char *p = (const char *)mem_base(m) + path;
  int r = openat(xlate_dirfd(dirfd), p, xlate_open_flags(flags), (mode_t)mode);
  return r < 0 ? neg_errno(errno) : (uint32_t)r;
}

u32 w2c_env_0x5F_syscall_stat64(struct w2c_env *e, u32 path, u32 buf) {
  w2c_pdftex *m = e->mod;
  struct stat s;
  if (stat((const char *)mem_base(m) + path, &s) < 0) return neg_errno(errno);
  write_estat(m, buf, &s);
  return 0;
}

u32 w2c_env_0x5F_syscall_lstat64(struct w2c_env *e, u32 path, u32 buf) {
  w2c_pdftex *m = e->mod;
  struct stat s;
  if (lstat((const char *)mem_base(m) + path, &s) < 0) return neg_errno(errno);
  write_estat(m, buf, &s);
  return 0;
}

u32 w2c_env_0x5F_syscall_newfstatat(struct w2c_env *e, u32 dirfd, u32 path,
                                    u32 buf, u32 flags) {
  w2c_pdftex *m = e->mod;
  int hf = 0;
  if (flags & 0x100 /* AT_SYMLINK_NOFOLLOW (Linux) */) hf |= AT_SYMLINK_NOFOLLOW;
  if (flags & 0x1000 /* AT_EMPTY_PATH */) hf |= AT_SYMLINK_NOFOLLOW; /* best effort */
  struct stat s;
  if (fstatat(xlate_dirfd(dirfd), (const char *)mem_base(m) + path, &s, hf) < 0)
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
static void write_tm(w2c_pdftex *m, uint32_t p, const struct tm *t) {
  uint8_t *b = mem_base(m) + p;
  int32_t v[9] = {t->tm_sec,  t->tm_min,  t->tm_hour,
                  t->tm_mday, t->tm_mon,  t->tm_year,
                  t->tm_wday, t->tm_yday, t->tm_isdst};
  memcpy(b, v, sizeof(v));
}

u32 w2c_env_0x5Fgmtime_js(struct w2c_env *e, u64 t, u32 tmptr) {
  w2c_pdftex *m = e->mod;
  time_t tt = (time_t)t;
  struct tm r;
  gmtime_r(&tt, &r);
  write_tm(m, tmptr, &r);
  return 0;
}

u32 w2c_env_0x5Flocaltime_js(struct w2c_env *e, u64 t, u32 tmptr) {
  w2c_pdftex *m = e->mod;
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
  exit((int)code);
}

void w2c_env_0x5F_assert_fail(struct w2c_env *e, u32 cond, u32 file, u32 line,
                              u32 func) {
  w2c_pdftex *m = e->mod;
  fprintf(stderr, "assertion failed: %s (%s:%u, %s)\n",
          (const char *)mem_base(m) + cond, (const char *)mem_base(m) + file,
          line, (const char *)mem_base(m) + func);
  abort();
}

f64 w2c_env_emscripten_date_now(struct w2c_env *e) {
  (void)e;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

f64 w2c_env_emscripten_get_now(struct w2c_env *e) {
  (void)e;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Grow the linear memory to at least `requested` bytes. */
u32 w2c_env_emscripten_resize_heap(struct w2c_env *e, u32 requested) {
  w2c_pdftex *m = e->mod;
  wasm_rt_memory_t *mem = &m->w2c_memory;
  if ((uint64_t)requested <= mem->size) return 1;
  uint64_t need_pages = ((uint64_t)requested - mem->size + 65535) / 65536;
  uint64_t r = wasm_rt_grow_memory(mem, need_pages);
  return (r == (uint64_t)-1) ? 0 : 1;
}

/* ------------------------------ main ------------------------------ */

int main(int argc, char **argv) {
  g_argc = argc;
  g_argv = argv;

  wasm_rt_init();

  w2c_pdftex mod;
  struct w2c_env env;
  struct w2c_wasi__snapshot__preview1 wasi;
  env.mod = &mod;
  wasi.mod = &mod;

  wasm2c_pdftex_instantiate(&mod, &env, &wasi);
  w2c_pdftex_0x5F_wasm_call_ctors(&mod); /* run static constructors first */

  /* Build argv[] inside wasm memory and call __main_argc_argv.
   * Layout: argc+1 pointers (NULL-terminated) followed by the strings. */
  uint32_t ptrbytes = (uint32_t)(argc + 1) * 4;
  uint32_t strbytes = 0;
  for (int i = 0; i < argc; i++) strbytes += (uint32_t)strlen(argv[i]) + 1;
  uint32_t total = (ptrbytes + strbytes + 15u) & ~15u;
  uint32_t base = w2c_pdftex_0x5Femscripten_stack_alloc(&mod, total);
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

  /* Self-test: suspend once on the first read, then resume — proves the
   * coroutine suspend/resume works before we add COW snapshotting. */
  if (getenv("TEXPRESSO_SUSPEND_TEST")) g_yield_next_read = 1;

  swapcontext(&g_host_ctx, &g_engine_ctx); /* run until yield or done */
  if (g_suspended) {
    fprintf(stderr, "[host] engine suspended in fd_read; resuming\n");
    swapcontext(&g_host_ctx, &g_engine_ctx); /* resume to completion */
  }
  fprintf(stderr, "[host] engine done=%d\n", g_engine_done);

  wasm2c_pdftex_free(&mod);
  wasm_rt_free();
  return 0;
}
