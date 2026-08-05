#!/bin/bash
# Fetch a prebuilt wasm engine bundle (tier 2) and unpack it ready to build.
#
# The bundle is portable C — engine.c/.h from wasm2c plus the vendored wabt
# runtime and, for xetex, its ICU data. Building from it needs only a C
# compiler: no emscripten, no wabt, no TeX Live source. That is the difference
# from tier 1 (scripts/fetch-engines.sh + build-wasm*.sh, which builds the wasm
# from pinned upstream sources and needs the full toolchain).
#
# Formats are deliberately not shipped. A .fmt bakes in the LaTeX kernel of
# whichever TeX Live produced it and would silently override the user's own;
# texpresso generates one on first run from the local installation instead.
#
# Usage: scripts/fetch-wasm-engine.sh [xetex|pdftex|luatex]   (default: xetex)
set -euo pipefail

ENG="${1:-xetex}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/engines/build-wasm2c-$ENG"

# Where releases live. Until this branch lands upstream the assets are published
# from the fork; point TEXPRESSO_ENGINE_REPO elsewhere to override.
REPO="${TEXPRESSO_ENGINE_REPO:-merv1n34k/texpresso}"

# Pinned per engine, so one engine can be re-cut without disturbing the others.
# engine.c and src/engine-wasm/wasm_host.c share an ABI (the w2c_engine struct
# and the syscall import set), so a mismatched pair fails to compile or, worse,
# misbehaves — keep these in step with the host.
case "$ENG" in
  xetex)  TAG="engine-xetex-2026.1-1";  SHA256="" ;;
  pdftex) TAG="engine-pdftex-2026.1-1"; SHA256="" ;;
  luatex) TAG="engine-luatex-2026.1-1"; SHA256="" ;;
  *) echo "unknown engine: $ENG (expected xetex|pdftex|luatex)" >&2; exit 1 ;;
esac

TARBALL="engine-$ENG.tar.gz"
URL="https://github.com/$REPO/releases/download/$TAG/$TARBALL"

if [ -f "$DEST/engine.c" ] && [ "${FORCE:-0}" != 1 ]; then
  echo "already present: $DEST/engine.c (FORCE=1 to refetch)"
  exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "fetching $URL"
if ! curl -fL --progress-bar -o "$TMP/$TARBALL" "$URL"; then
  echo "" >&2
  echo "could not download $TARBALL from $TAG." >&2
  echo "Either the release is not published yet, or this checkout expects a" >&2
  echo "version that no longer exists. Build it yourself instead (tier 1):" >&2
  echo "  make engine-source ENGINE=$ENG" >&2
  exit 1
fi

if [ -n "$SHA256" ]; then
  have="$(shasum -a 256 "$TMP/$TARBALL" | cut -d' ' -f1)"
  [ "$have" = "$SHA256" ] || {
    echo "checksum mismatch for $TARBALL" >&2
    echo "  expected $SHA256" >&2
    echo "  got      $have" >&2
    exit 1
  }
  echo "checksum ok"
else
  echo "warning: no checksum pinned for $ENG — skipping verification" >&2
fi

tar -xzf "$TMP/$TARBALL" -C "$TMP"
[ -f "$TMP/engine-$ENG/engine.c" ] || { echo "bundle is missing engine.c" >&2; exit 1; }

mkdir -p "$DEST"
cp -f "$TMP/engine-$ENG/"* "$DEST/"

echo "unpacked into $DEST"
[ -f "$DEST/MANIFEST" ] && sed -n '1,5p' "$DEST/MANIFEST" | sed 's/^/  /'
echo ""
echo "now: make texpresso WASM_ENGINE_DIR=engines/build-wasm2c-$ENG"
