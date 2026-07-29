#!/bin/bash
# Cross-compile pristine luatex to WebAssembly via Emscripten for wasm2c.
#
# luatex uses PUC Lua (lua53), NOT luajit (which can't target wasm). Unlike
# xetex it needs neither ICU nor fontconfig (fonts are handled in Lua), so the
# build is simpler: zlib libpng freetype2 graphite2 harfbuzz pplib zziplib lua53.
# Reuses the harfbuzz -Werror-pragma and freetype wasm-flags fixes from the
# xetex build.
#
# Prerequisites: scripts/fetch-engines.sh, emcc on PATH.
# Output: engines/build-wasm-luatex/texk/web2c/luatex.wasm
set -euo pipefail

export EMCC_SKIP_SANITY_CHECK=1

WASMFLAGS="-O2 -sSUPPORT_LONGJMP=wasm -fwasm-exceptions"
LINKFLAGS="-sSUPPORT_LONGJMP=wasm -fwasm-exceptions -sALLOW_MEMORY_GROWTH=1"
CXXWASMFLAGS="$WASMFLAGS -DHB_NO_PRAGMA_GCC_DIAGNOSTIC_ERROR"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/engines/texlive-source"
BUILD="$ROOT/engines/build-wasm-luatex"

[ -d "$SRC" ] || { echo "run scripts/fetch-engines.sh first" >&2; exit 1; }
command -v emcc >/dev/null || { echo "emcc not on PATH" >&2; exit 1; }

BUILD_TRIPLE="$("$SRC/build-aux/config.guess")"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
echo "native build triple: $BUILD_TRIPLE"

rm -rf "$BUILD"; mkdir -p "$BUILD"; cd "$BUILD"

emconfigure "$SRC/configure" \
  --build="$BUILD_TRIPLE" --host=wasm32-unknown-emscripten \
  --without-x --disable-shared --disable-all-pkgs \
  --enable-web2c --enable-luatex --disable-luajittex \
  --disable-tex --disable-etex --disable-pdftex --disable-ptex --disable-eptex \
  --disable-uptex --disable-euptex --disable-aleph --disable-xetex \
  --disable-mf --disable-mf-nowin --disable-synctex --enable-missing \
  CFLAGS="$WASMFLAGS" CXXFLAGS="$CXXWASMFLAGS" LDFLAGS="$LINKFLAGS"

LIBS_LUATEX="zlib libpng freetype2 graphite2 harfbuzz pplib zziplib lua53"
sed -i.bak "s|^CONF_SUBDIRS = .*|CONF_SUBDIRS = $LIBS_LUATEX|" libs/Makefile
sed -i.bak2 "s|^MAKE_SUBDIRS = .*|MAKE_SUBDIRS = $LIBS_LUATEX|" libs/Makefile

echo "=== support libs (pass 1: freetype builds without our wasm flags) ==="
emmake make -C libs -j"$JOBS" || true

# freetype's own build ignores our CFLAGS -> setjmp compiles to emscripten_longjmp
# (undefined under wasm SjLj). Pass our flags into its configure, force rebuild.
sed -i.bak "s|CC='\$(CC)' CONFIG_SITE|CC='\$(CC)' CFLAGS='\$(CFLAGS)' CXXFLAGS='\$(CFLAGS)' CONFIG_SITE|" \
  libs/freetype2/Makefile
rm -rf libs/freetype2/ft-build libs/freetype2/ft-config libs/freetype2/ft-install \
       libs/freetype2/libfreetype.* libs/freetype2/freetype2 libs/freetype2/ft2build.h

echo "=== support libs (pass 2) ==="
emmake make -C libs -j"$JOBS"

# Build texk: configures its subdirs (kpathsea, web2c) and builds kpathsea; the
# "all" target may fail later, but that configures web2c which is what we need.
echo "=== build texk (kpathsea + configure web2c; 'all' may fail) ==="
emmake make -C texk -j"$JOBS" || true
echo "=== link luatex ==="
emmake make -C texk/web2c -j"$JOBS" luatex

echo "built: $BUILD/texk/web2c/luatex.wasm"
ls -la texk/web2c/luatex.wasm 2>/dev/null || { echo "luatex.wasm not produced" >&2; exit 1; }
