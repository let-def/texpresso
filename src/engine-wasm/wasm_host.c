/*
 * Native host for the wasm2c-compiled pdftex engine.
 *
 * Implements the 22 wasm imports (WASI subset + a few Linux syscalls emscripten
 * leaves as env.__syscall_*) against native POSIX, and runs the engine's WASI
 * `_start` entry point in-process — no JS, no node.
 *
 * This is the Phase 1 bring-up host: enough to run `pdftex --version` natively.
 * File I/O for real typesetting (openat/stat) will be routed through texpresso's
 * VFS later; standalone emscripten currently resolves those internally.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
  int r = faccessat((int)dirfd, p, (int)amode, (int)flags);
  return r < 0 ? neg_errno(errno) : 0;
}

u32 w2c_env_0x5F_syscall_readlinkat(struct w2c_env *e, u32 dirfd, u32 path,
                                    u32 buf, u32 bufsize) {
  w2c_pdftex *m = e->mod;
  const char *p = (const char *)mem_base(m) + path;
  ssize_t r = readlinkat((int)dirfd, p, (char *)mem_base(m) + buf, bufsize);
  return r < 0 ? neg_errno(errno) : (uint32_t)r;
}

u32 w2c_env_0x5F_syscall_unlinkat(struct w2c_env *e, u32 dirfd, u32 path,
                                  u32 flags) {
  w2c_pdftex *m = e->mod;
  const char *p = (const char *)mem_base(m) + path;
  int r = unlinkat((int)dirfd, p, (int)flags);
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
  int r = renameat((int)od, (const char *)mem_base(m) + op, (int)nd,
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

/* ------------------------------ main ------------------------------ */

int main(int argc, char **argv) {
  /* Present args to the engine as: pdftex <user args...> */
  g_argc = argc;
  g_argv = argv;

  wasm_rt_init();

  w2c_pdftex mod;
  struct w2c_env env;
  struct w2c_wasi__snapshot__preview1 wasi;
  env.mod = &mod;
  wasi.mod = &mod;

  wasm2c_pdftex_instantiate(&mod, &env, &wasi);
  w2c_pdftex_0x5Fstart(&mod); /* WASI entry; calls proc_exit -> exit() */

  wasm2c_pdftex_free(&mod);
  wasm_rt_free();
  return 0;
}
