#!/bin/bash
# Cross-compile pristine xetex to WebAssembly via Emscripten for wasm2c.
#
# Same non-standalone recipe as pdftex, plus xetex's font stack: freetype2,
# harfbuzz, graphite2, teckit, icu (+ libpng, zlib, pplib). Several emscripten
# cross-build quirks are worked around (each documented inline below):
#   - harfbuzz promotes warnings to errors via in-source pragmas
#   - ICU builds "native" data tools; emmake would build them as wasm
#   - ICU has no platform config for the emscripten host
#   - freetype's own build system drops our wasm flags
#   - xetex needs fontconfig; we link a FreeType-backed shim
#
# Prerequisites: scripts/fetch-engines.sh, emcc on PATH.
# Output: engines/build-wasm-xetex/texk/web2c/xetex.wasm
set -euo pipefail

export EMCC_SKIP_SANITY_CHECK=1

WASMFLAGS="-O2 -sSUPPORT_LONGJMP=wasm -fwasm-exceptions"
# ALLOW_MEMORY_GROWTH: xetex xmallocs ~72 MB at startup; the default 16 MB
# non-growable heap exhausts. wasm2c (mmap-reserved) grows fine.
LINKFLAGS="-sSUPPORT_LONGJMP=wasm -fwasm-exceptions -sALLOW_MEMORY_GROWTH=1"
# harfbuzz promotes warnings to errors via in-source #pragma GCC diagnostic;
# emscripten's newer clang fires some (e.g. -Wunused-template). Its own opt-out:
CXXWASMFLAGS="$WASMFLAGS -DHB_NO_PRAGMA_GCC_DIAGNOSTIC_ERROR"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/engines/texlive-source"
BUILD="$ROOT/engines/build-wasm-xetex"
SHIM="$ROOT/src/engine-wasm/fontconfig-shim"

[ -d "$SRC" ] || { echo "run scripts/fetch-engines.sh first" >&2; exit 1; }
command -v emcc >/dev/null || { echo "emcc not on PATH" >&2; exit 1; }

BUILD_TRIPLE="$("$SRC/build-aux/config.guess")"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
echo "native build triple: $BUILD_TRIPLE"

# ICU has no config/mh-* for the emscripten host and falls back to mh-unknown,
# an error stub. Emscripten is Linux-like (we build static), so use mh-linux.
# This edits the (gitignored) source tree; harmless + what ICU's note advises.
/bin/cp -f "$SRC/libs/icu/icu-src/source/config/mh-linux" \
           "$SRC/libs/icu/icu-src/source/config/mh-unknown"

rm -rf "$BUILD"; mkdir -p "$BUILD"; cd "$BUILD"

# Point configure's fontconfig probe at our shim (cache vars skip the
# -lfontconfig link test, which fails under emscripten). Exported so the
# web2c subdir configure (run during make) sees them too.
export kpse_cv_have_fontconfig=yes
export kpse_cv_fontconfig_includes="-I$SHIM"
export kpse_cv_fontconfig_libs="$BUILD/libfcshim.a"

emconfigure "$SRC/configure" \
  --build="$BUILD_TRIPLE" --host=wasm32-unknown-emscripten \
  --without-x --disable-shared --disable-all-pkgs \
  --enable-web2c --enable-xetex --disable-xetex-synctex \
  --disable-tex --disable-etex --disable-pdftex --disable-ptex --disable-eptex \
  --disable-uptex --disable-euptex --disable-aleph \
  --disable-luatex --disable-luajittex --disable-mf --disable-mf-nowin \
  --disable-synctex --enable-missing \
  CFLAGS="$WASMFLAGS" CXXFLAGS="$CXXWASMFLAGS" LDFLAGS="$LINKFLAGS"

# xetex links only these; drop the rest (esp. lua53/luajit — luatex deps that
# would otherwise be recursed into but were never configured). Both the configure
# list (CONF_SUBDIRS) and the build-recursion list (MAKE_SUBDIRS) must be trimmed.
LIBS_XETEX="zlib libpng freetype2 graphite2 teckit icu harfbuzz pplib"
sed -i.bak "s|^CONF_SUBDIRS = .*|CONF_SUBDIRS = $LIBS_XETEX|" libs/Makefile
sed -i.bak2 "s|^MAKE_SUBDIRS = .*|MAKE_SUBDIRS = $LIBS_XETEX|" libs/Makefile

# --- support libs, pass 1: generates the per-lib Makefiles, then fails/builds
# wrong for icu (native tools compiled as wasm) and freetype (drops wasm flags). ---
echo "=== support libs (pass 1) ==="
emmake make -C libs -j"$JOBS" || true

# Fix icu: emmake exports CC=emcc, which leaks into icu-native (the native
# data/tool build). Force the real host compiler as explicit configure args.
sed -i.bak 's|^icu_native_args = .*|& CC=cc CXX=c++ CFLAGS=-O2 CXXFLAGS=-O2 LDFLAGS=|' \
  libs/icu/Makefile
rm -rf libs/icu/icu-native libs/icu/icu-build

# Fix freetype: its native build system ignores our CFLAGS, so setjmp/longjmp
# compiles to the JS (emscripten_longjmp) path, undefined under wasm SjLj. Pass
# our flags into freetype's own configure and force a clean rebuild.
sed -i.bak "s|CC='\$(CC)' CONFIG_SITE|CC='\$(CC)' CFLAGS='\$(CFLAGS)' CXXFLAGS='\$(CFLAGS)' CONFIG_SITE|" \
  libs/freetype2/Makefile
rm -rf libs/freetype2/ft-build libs/freetype2/ft-config libs/freetype2/ft-install \
       libs/freetype2/libfreetype.* libs/freetype2/freetype2 libs/freetype2/ft2build.h

echo "=== support libs (pass 2) ==="
emmake make -C libs -j"$JOBS"

# --- fontconfig shim (needs freetype headers configure resolved) ---
echo "=== fontconfig shim ==="
FT_INC="-I$BUILD/libs/freetype2/ft-build -I$SRC/libs/freetype2/freetype-src/include"
emcc $WASMFLAGS $FT_INC -I"$SHIM" -c "$SHIM/fcshim.c" -o "$BUILD/fcshim.o"
emar rcs "$BUILD/libfcshim.a" "$BUILD/fcshim.o"

# --- kpathsea, then configure web2c and link only the xetex target (building
# "all" would pull lua53, a luatex dep). ---
echo "=== kpathsea ==="
emmake make -C texk/kpathsea -j"$JOBS"
echo "=== configure texk/web2c (build fails at lua53; we only need xetex) ==="
emmake make -C texk -j"$JOBS" || true
# force xetex's fontconfig objects to compile against our shim header
sed -i.bak "s|^FONTCONFIG_INCLUDES = .*|FONTCONFIG_INCLUDES = -I$SHIM|" texk/web2c/Makefile
sed -i.bak2 "s|^FONTCONFIG_LIBS = .*|FONTCONFIG_LIBS = $BUILD/libfcshim.a|" texk/web2c/Makefile
rm -f texk/web2c/xetexdir/libxetex_a-XeTeXFontMgr_FC.o \
      texk/web2c/xetexdir/libxetex_a-XeTeX_ext.o \
      texk/web2c/xetexdir/libxetex_a-XeTeXLayoutInterface.o
echo "=== link xetex ==="
emmake make -C texk/web2c -j"$JOBS" xetex

echo "built: $BUILD/texk/web2c/xetex.wasm"
ls -la texk/web2c/xetex.wasm 2>/dev/null || { echo "xetex.wasm not produced" >&2; exit 1; }
