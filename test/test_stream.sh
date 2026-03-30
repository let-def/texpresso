#!/bin/sh
# Test stream mode by piping an open command via stdin.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEX_FILE="$SCRIPT_DIR/simple.tex"

if [ ! -f "$TEX_FILE" ]; then
  echo "FAIL: $TEX_FILE not found" >&2
  exit 1
fi

# Escape content for sexp string: \ → \\, " → \", newline → \n, tab → \t
CONTENT=$(sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/	/\\t/g' "$TEX_FILE" | \
  awk '{ if (NR > 1) printf "\\n"; printf "%s", $0 }')

SEXP="(open \"$TEX_FILE\" \"$CONTENT\")"

echo "$SEXP" | SDL_VIDEODRIVER=dummy build/texpresso -stream -test-initialize test/simple.tex
