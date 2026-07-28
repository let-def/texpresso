#!/bin/bash
# Cross-compile pristine pdftex to standalone WASI WebAssembly via Emscripten.
#
# Standalone WASM, no JS runtime, no JS compat. setjmp/longjmp uses the wasm
# exception-handling feature (no JS invoke_* trampolines). Output feeds wasm2c.
#
# Speed: configure with light flags (feature detection doesn't need -O2 /
# exceptions), apply real flags only at build time; skip emcc sanity check.
#
# Prerequisites: scripts/fetch-engines.sh (source), and emcc on PATH.
# Output: engines/build-wasm/texk/web2c/pdftex.wasm
set -euo pipefail

export EMCC_SKIP_SANITY_CHECK=1

# Real build flags (applied at make time, not during configure).
WASMFLAGS="-O2 -sSUPPORT_LONGJMP=wasm -fwasm-exceptions"
LINKFLAGS="-sSTANDALONE_WASM -sSUPPORT_LONGJMP=wasm -fwasm-exceptions"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/engines/texlive-source"
BUILD="$ROOT/engines/build-wasm"

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

echo "built: $BUILD/texk/web2c/pdftex.wasm"
ls -la pdftex.wasm
