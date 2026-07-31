/*
 * Host <-> filesystem seam for the wasm engine.
 *
 * The engine's file syscalls (openat/read/write/seek/close/stat/access) are
 * routed through this vtable instead of calling native POSIX directly. Two
 * backends:
 *   - native (default): thin POSIX passthrough — used by the standalone
 *     ENGINE-native test binaries.
 *   - texpresso: a state.c-backed impl that serves file *content* from
 *     fileentry buffers (edited buffers / captured output), so there is no
 *     native filesystem and snapshots capture all I/O state.
 *
 * Semantics are POSIX: return -1 and set errno on failure. fd values are
 * whatever the backend chooses (real fds for native; small handles for the VFS).
 * stat is returned in the *native* struct stat; the host marshals it into the
 * engine's (emscripten) layout.
 */
#ifndef TEXPRESSO_WASM_HOST_H
#define TEXPRESSO_WASM_HOST_H

#include <sys/stat.h>
#include <sys/types.h>

typedef struct wasm_io_ops {
  int (*openat)(void *ctx, int dirfd, const char *path, int flags, mode_t mode);
  ssize_t (*read)(void *ctx, int fd, void *buf, size_t len);
  ssize_t (*write)(void *ctx, int fd, const void *buf, size_t len);
  off_t (*lseek)(void *ctx, int fd, off_t off, int whence);
  int (*close)(void *ctx, int fd);
  int (*fstat)(void *ctx, int fd, struct stat *st);
  /* stat/lstat/newfstatat: atflags carries AT_SYMLINK_NOFOLLOW etc. */
  int (*statat)(void *ctx, int dirfd, const char *path, struct stat *st,
                int atflags);
  int (*accessat)(void *ctx, int dirfd, const char *path, int amode,
                  int atflags);
} wasm_io_ops;

/* Install the I/O backend. If never called, the host uses native POSIX. */
void wasm_host_set_io(const wasm_io_ops *ops, void *ctx);

/* ---- engine lifecycle (single in-process instance) ----
 * Used by the standalone binaries and by texpresso's engine_wasm backend.
 * The engine runs on a dedicated ucontext stack so its execution state can be
 * snapshotted (linear memory via mprotect-COW + the stack + registers). */

/* Instantiate the module, run ctors, place argv in wasm memory, set up the
 * coroutine. Returns 0 on success. Call wasm_host_set_io() first if you want a
 * non-native filesystem. */
int wasm_engine_init(int argc, char **argv);
void wasm_engine_shutdown(void);

/* Resume the engine until it suspends (yield point) or exits.
 * Returns 1 if it suspended and can be resumed, 0 if it exited. */
int wasm_engine_run(void);
int wasm_engine_exited(void); /* nonzero once the engine has returned/exited */

/* Request that the engine suspend on its next input read (a snapshot point). */
void wasm_engine_request_yield(void);

/* Copy-on-write snapshot of the whole execution state.
 * Snapshots form a stack (fences): snapshot()/restore() are the single-layer
 * shortcut (reset to one base fence and restore to it). For the fence stack,
 * snapshot_push() adds a fence and returns its index; restore_to(k) rewinds to
 * fence k, discarding deeper ones; snapshot_count() is the number of fences. */
void wasm_engine_snapshot(void);
void wasm_engine_restore(void);
int wasm_engine_snapshot_push(void);
void wasm_engine_restore_to(int fence);
int wasm_engine_snapshot_count(void);

/* Keep fds open across close() until the next restore (so a rollback can reset
 * their positions rather than them being gone). */
void wasm_engine_defer_close(int on);

#endif /* TEXPRESSO_WASM_HOST_H */
