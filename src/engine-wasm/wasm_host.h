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

#endif /* TEXPRESSO_WASM_HOST_H */
