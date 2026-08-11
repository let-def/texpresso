#!/bin/bash
# Package a self-contained wasm engine bundle for release.
#
# You run this LOCALLY (it needs the outputs of build-wasm2c-<eng>.sh +
# build-wasm-fmt.sh, i.e. emcc/wabt already used). It produces one portable
# tarball that CI and any contributor can compile with just a C compiler — no
# emscripten, no wabt. Upload the tarball to a GitHub release; fetch-wasm-engine.sh
# pulls it.
#
# Contents (everything needed to build + run texpresso except the frontend
# source and system libs):
#   engine.c engine.h            the wasm2c-compiled engine (portable C)
#   wasm-rt*.{c,h,inc}           the wabt runtime (vendored, so no wabt needed)
#   icudt*.dat                   ICU data (xetex)
#   MANIFEST                     engine name, sizes, provenance
#
# Usage: scripts/package-wasm-engine.sh [xetex|pdftex|luatex]
set -euo pipefail

ENG="${1:-xetex}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W2C="$ROOT/engines/build-wasm2c-$ENG"
FMTDIR="$ROOT/engines/wasm-fmt"
WABT_INC="$(brew --prefix wabt 2>/dev/null)/include"
WABT_SHARE="$(brew --prefix wabt 2>/dev/null)/share/wabt/wasm2c"
OUT="$ROOT/engines/dist"
STAGE="$OUT/engine-$ENG"

[ -f "$W2C/engine.c" ] || { echo "missing $W2C/engine.c — run build-wasm2c-$ENG.sh first" >&2; exit 1; }
[ -d "$WABT_SHARE" ]   || { echo "wabt runtime not found ($WABT_SHARE)" >&2; exit 1; }

rm -rf "$STAGE"; mkdir -p "$STAGE"

# the engine (portable C) + the vendored wabt runtime
cp -f "$W2C/engine.c" "$W2C/engine.h" "$STAGE/"
cp -f "$WABT_INC/wasm-rt.h" "$WABT_INC/wasm-rt-exceptions.h" "$STAGE/"
cp -f "$WABT_SHARE/wasm-rt-impl.h" \
      "$WABT_SHARE/wasm-rt-impl.c" \
      "$WABT_SHARE/wasm-rt-mem-impl.c" \
      "$WABT_SHARE/wasm-rt-exceptions-impl.c" \
      "$WABT_SHARE/wasm-rt-impl-tableops.inc" \
      "$WABT_SHARE/wasm-rt-mem-impl-helper.inc" "$STAGE/"

# Runtime data: ICU only. ICU data is compiled from OUR wasm build, so it must
# match this engine and has to ship. Formats are deliberately NOT shipped: a
# .fmt bakes in the LaTeX kernel of whichever TeX Live built it, which would
# silently override the user's own. texpresso generates it on first run instead.
for f in "$W2C"/*.dat; do
  [ -f "$f" ] && cp -f "$f" "$STAGE/" || true
done

WASM2C_VER="$(wasm2c --version 2>/dev/null || echo unknown)"
TL_TAG="$(sed -n 's/^TAG="tags\/\(.*\)".*/\1/p' "$ROOT/scripts/fetch-engines.sh" | head -1)"
SRC_REV="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
{
  echo "engine: $ENG"
  echo "wasm2c: $WASM2C_VER"
  echo "texlive: ${TL_TAG:-unknown}"
  echo "texpresso: $SRC_REV"   # host ABI this engine.c was generated against
  echo "built:  $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "files:"
  (cd "$STAGE" && ls -la | awk 'NR>1{print "  "$5"\t"$9}')
} > "$STAGE/MANIFEST"

mkdir -p "$OUT"
TARBALL="$OUT/engine-$ENG.tar.gz"
tar -C "$OUT" -czf "$TARBALL" "engine-$ENG"
echo "packaged: $TARBALL ($(du -h "$TARBALL" | cut -f1))"
echo "upload with e.g.: gh release upload engine-$ENG-<ver> $TARBALL"
