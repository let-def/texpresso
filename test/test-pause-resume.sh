#!/bin/sh
# Deterministic pause/resume test: editor pauses the engine at startup,
# primes the VFS with the root file via register + open, then resumes.
# Engine has everything it needs before it steps — no race conditions.
# Exit status is the test result.
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

CONTENT=$(sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/	/\\t/g' "$TEX_FILE" | \
  awk '{ if (NR > 1) printf "\\n"; printf "%s", $0 }')

SDL_VIDEODRIVER=dummy build/texpresso -stream -test-initialize test/simple.tex \
  < "$FIFO" 2>/dev/null &
PID=$!

exec 3>"$FIFO"
printf '(pause)\n' >&3
printf '(register "%s")\n' "$TEX_FILE" >&3
printf '(open "%s" "%s")\n' "$TEX_FILE" "$CONTENT" >&3
printf '(resume)\n' >&3
exec 3>&-

if wait "$PID"; then
  echo "PASS: pause-resume test"
else
  echo "FAIL: texpresso exited with error"
  exit 1
fi
