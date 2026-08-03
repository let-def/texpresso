#!/bin/bash
# Cross-compile pristine pdftex to WebAssembly via Emscripten for wasm2c.
#
# NON-standalone so filesystem syscalls (openat/stat/lstat/...) are emitted as
# imports our C host implements — that is the VFS hook. NOT standalone, because
# standalone inlines those to empty-FS ENOSYS stubs the host never sees.
# setjmp/longjmp uses wasm exception handling (SUPPORT_LONGJMP=wasm), so there
# are no JS invoke_* trampolines despite non-standalone. No JS runtime is used;
# the ~4 "_js" imports (time/abort) are implemented in C by the host.
#
# Prerequisites: scripts/fetch-engines.sh (source), and emcc on PATH.
# Output: engines/build-wasm-pdftex/texk/web2c/pdftex.wasm
set -euo pipefail

export EMCC_SKIP_SANITY_CHECK=1

WASMFLAGS="-O2 -sSUPPORT_LONGJMP=wasm -fwasm-exceptions"
# ALLOW_MEMORY_GROWTH: pdftex xmallocs ~40 MB while dumping a format; the
# default fixed 16 MB heap aborts with "fatal: memory exhausted".
LINKFLAGS="-sSUPPORT_LONGJMP=wasm -fwasm-exceptions -sALLOW_MEMORY_GROWTH=1"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/engines/texlive-source"
BUILD="$ROOT/engines/build-wasm-pdftex"

[ -d "$SRC" ] || { echo "run scripts/fetch-engines.sh first" >&2; exit 1; }
command -v emcc >/dev/null || { echo "emcc not on PATH (install emscripten)" >&2; exit 1; }

BUILD_TRIPLE="$("$SRC/build-aux/config.guess")"
echo "native build triple: $BUILD_TRIPLE"

rm -rf "$BUILD"; mkdir -p "$BUILD"; cd "$BUILD"

# Flags go at configure time: configure routes CFLAGS to the host compiler
# (emcc) and uses a separate native CC_FOR_BUILD for build tools. Overriding
# CFLAGS at make time would wrongly pass emcc -s flags to native clang.
# luajit/luatex/mf disabled (luajit's 32-bit native buildvm can't cross-build).
emconfigure "$SRC/configure" \
  --build="$BUILD_TRIPLE" --host=wasm32-unknown-emscripten \
  --without-x --disable-shared --disable-all-pkgs \
  --enable-tex --disable-synctex --disable-xetex \
  --disable-luatex --disable-luajittex --disable-mf --disable-mf-nowin \
  --enable-missing \
  CFLAGS="$WASMFLAGS" CXXFLAGS="$WASMFLAGS" LDFLAGS="$LINKFLAGS"

# pdftex only links zlib/libpng/xpdf; trim so other libs (esp. luajit) are
# never configured. Edits the generated build Makefile, not the source.
sed -i.bak 's/^CONF_SUBDIRS = .*/CONF_SUBDIRS = zlib libpng xpdf/' libs/Makefile

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
emmake make -j"$JOBS"

cd texk/web2c
emmake make pdftex

# No asyncify: the host snapshots execution state directly by running the
# engine on a dedicated fixed-address stack (ucontext coroutine) and
# copy-on-write-ing both the linear memory and that stack. See wasm_host.c.

echo "built: $BUILD/texk/web2c/pdftex.wasm"
ls -la pdftex.wasm
