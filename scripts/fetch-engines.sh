#!/bin/bash
# Fetch pristine TeX Live engine sources (pdftex, xetex, luatex) into engines/.
#
# Uses a blobless + sparse + shallow clone so only the web2c engines, kpathsea,
# and the C libraries those engines need are materialized (~330 MB vs ~1 GB for
# the full tree). No third-party build harness, no patches — raw upstream.
#
# The engines/ directory is gitignored; nothing here is vendored into texpresso.
set -euo pipefail

REPO="https://github.com/TeX-Live/texlive-source"
TAG="tags/texlive-2026.1"                    # pinned upstream release (r78399)
DEST="$(cd "$(dirname "$0")/.." && pwd)/engines/texlive-source"

# Only the subtrees the three engines actually need.
PATHS=(
  build-aux m4 am auxdir              # top-level build infrastructure
  texk/kpathsea                       # file lookup (all engines)
  texk/web2c                          # engines + web2c core + tangle
  libs/zlib libs/libpng              # pdftex, luatex
  libs/xpdf libs/pplib libs/zziplib  # PDF inclusion (pdftex, luatex)
  libs/freetype2 libs/harfbuzz       # xetex, luatex
  libs/graphite2 libs/icu libs/teckit # xetex
  libs/lua53                         # luatex (PUC Lua; NOT luajit)
)

if [ -d "$DEST/.git" ]; then
  echo "engines already present at $DEST — updating sparse set"
else
  echo "cloning $REPO @ $TAG (blobless, no-checkout)"
  git clone --filter=blob:none --no-checkout --depth 1 --branch "$TAG" "$REPO" "$DEST"
fi

git -C "$DEST" sparse-checkout set --cone "${PATHS[@]}"
git -C "$DEST" checkout

echo "done: $(du -sh "$DEST" | cut -f1) at $(git -C "$DEST" describe --all)"
