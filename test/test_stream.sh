#!/bin/sh
# Test stream mode by piping an open command via stdin.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEX_FILE="$SCRIPT_DIR/simple.tex"

if [ ! -f "$TEX_FILE" ]; then
  echo "FAIL: $TEX_FILE not found" >&2
  exit 1
fi

FIFO=$(mktemp -u /tmp/texpresso-fifo-XXXXXX)
mkfifo "$FIFO"
trap 'rm -f "$FIFO"; kill "$PID" 2>/dev/null || true' EXIT

# Escape content for sexp string: \ → \\, " → \", newline → \n, tab → \t
CONTENT=$(sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/	/\\t/g' "$TEX_FILE" | \
  awk '{ if (NR > 1) printf "\\n"; printf "%s", $0 }')

SEXP="(open \"$TEX_FILE\" \"$CONTENT\")"

SDL_VIDEODRIVER=dummy build/texpresso -stream -test-initialize test/simple.tex \
  < "$FIFO" 2>/dev/null &
PID=$!

# Keep FIFO open until texpresso processes the command
exec 3>"$FIFO"
printf '%s\n' "$SEXP" >&3

# Wait for texpresso to finish, then close
if wait "$PID"; then
  echo "PASS: stream-pipe test"
else
  echo "FAIL: texpresso exited with error"
  exit 1
fi
exec 3>&-
