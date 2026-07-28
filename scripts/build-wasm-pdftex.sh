#!/bin/bash
# Cross-compile pristine pdftex to WebAssembly via Emscripten.
#
# Prerequisites: scripts/fetch-engines.sh (source), and emcc on PATH.
# Output: engines/build-wasm/texk/web2c/pdftex.wasm (+ node launcher).
#
# NOTE: this produces the emscripten/node-targeted wasm (JS glue, syscalls via
# node). The texpresso integration needs a standalone wasm for wasm2c — that is
# a separate build step (see WASM-ENGINE.md, Phase 1).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/engines/texlive-source"
BUILD="$ROOT/engines/build-wasm"

[ -d "$SRC" ] || { echo "run scripts/fetch-engines.sh first" >&2; exit 1; }
command -v emcc >/dev/null || { echo "emcc not on PATH (install emscripten)" >&2; exit 1; }

BUILD_TRIPLE="$("$SRC/build-aux/config.guess")"
echo "native build triple: $BUILD_TRIPLE"

rm -rf "$BUILD"; mkdir -p "$BUILD"; cd "$BUILD"

# Cross-configure. --enable-tex is enough; `make pdftex` builds pdftex too.
# luajit/luatex/mf disabled: luajit's 32-bit native buildvm cannot cross-build
# on arm64. --enable-missing skips absent optional system deps.
emconfigure "$SRC/configure" \
  --build="$BUILD_TRIPLE" --host=wasm32-unknown-emscripten \
  --without-x --disable-shared --disable-all-pkgs \
  --enable-tex --disable-synctex --disable-xetex \
  --disable-luatex --disable-luajittex --disable-mf --disable-mf-nowin \
  --enable-missing \
  CFLAGS="-O2" CXXFLAGS="-O2"

# The libs package configures EVERY lib in CONF_SUBDIRS even though pdftex only
# links zlib/libpng/xpdf. Trim it so the unneeded libs (esp. luajit) are never
# configured. This edits the generated build Makefile, not the source.
sed -i.bak 's/^CONF_SUBDIRS = .*/CONF_SUBDIRS = zlib libpng xpdf/' libs/Makefile

# First pass: recursive configure + build the needed libs + kpathsea.
emmake make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# Build the pdftex engine (WEB->C bootstrap uses the native/system otangle).
cd texk/web2c
emmake make pdftex

echo "built: $BUILD/texk/web2c/pdftex.wasm"
ls -la pdftex.wasm
