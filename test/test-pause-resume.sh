#!/bin/sh
# Test pause/resume: editor pauses the engine at startup, primes the VFS
# with register + open, then resumes. Engine should produce at least one
# page without any restarts caused by missing files.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEX_FILE="$SCRIPT_DIR/simple.tex"

if [ ! -f "$TEX_FILE" ]; then
  echo "FAIL: $TEX_FILE not found" >&2
  exit 1
fi

FIFO=$(mktemp -u /tmp/texpresso-fifo-XXXXXX)
OUTFILE=$(mktemp /tmp/texpresso-out-XXXXXX)
mkfifo "$FIFO"
trap 'rm -f "$FIFO" "$OUTFILE"; kill "$PID" 2>/dev/null || true' EXIT

# Escape content for sexp string: \ -> \\, " -> \", newline -> \n, tab -> \t
CONTENT=$(sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/	/\\t/g' "$TEX_FILE" | \
  awk '{ if (NR > 1) printf "\\n"; printf "%s", $0 }')

SDL_VIDEODRIVER=dummy build/texpresso -stream -test-initialize test/simple.tex \
  < "$FIFO" > "$OUTFILE" 2>/dev/null &
PID=$!

exec 3>"$FIFO"

# Pause before engine does any work, register + open the root file,
# then resume. Atomic batch on the stdin pipe.
printf '(pause)\n' >&3
printf '(register "%s")\n' "$TEX_FILE" >&3
printf '(open "%s" "%s")\n' "$TEX_FILE" "$CONTENT" >&3
printf '(resume)\n' >&3

if wait "$PID"; then
  echo "PASS: pause-resume test"
else
  echo "FAIL: texpresso exited with error"
  echo "stdout contents:"
  cat "$OUTFILE"
  exit 1
fi
exec 3>&-
