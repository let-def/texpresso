#!/bin/bash
# Fetch pristine TeX Live engine sources (pdftex, xetex, luatex) into engines/.
#
# Full 1:1 checkout of the upstream TeX Live source tree at a pinned release.
# Shallow (--depth 1) so we get the complete source without the full history.
# No third-party build harness, no patches — raw upstream. The engines/
# directory is gitignored; nothing here is vendored into texpresso.
set -euo pipefail

REPO="https://github.com/TeX-Live/texlive-source"
TAG="tags/texlive-2026.1"                    # pinned upstream release (r78399)
DEST="$(cd "$(dirname "$0")/.." && pwd)/engines/texlive-source"

if [ -d "$DEST/.git" ]; then
  echo "engines already present at $DEST"
else
  echo "cloning $REPO @ $TAG (full tree, shallow)"
  git clone --depth 1 --branch "$TAG" "$REPO" "$DEST"
fi

echo "done: $(du -sh "$DEST" | cut -f1) at $(git -C "$DEST" describe --all)"
