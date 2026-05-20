#!/bin/bash
# Test register + deferred Q_OPRD: editor pre-registers a file, engine defers
# the query when it tries to read it, editor provides content, engine resumes
# without restarting.
#
# Inherently async: the test must observe the engine reaching the deferred
# state before sending the file. Bounded poll + background watchdog as the
# final kill-switch.
set -e

WATCHDOG_TIMEOUT=${WATCHDOG_TIMEOUT:-60}
POLL_INTERVAL=0.5
MAX_POLLS=$((WATCHDOG_TIMEOUT * 2))

FIFO=$(mktemp -u /tmp/texpresso-fifo-XXXXXX)
OUTFILE=$(mktemp /tmp/texpresso-out-XXXXXX)
mkfifo "$FIFO"

cleanup() {
  rm -f "$FIFO" "$OUTFILE"
  kill "$PID" 2>/dev/null || true
  kill "$WATCHER" 2>/dev/null || true
}
trap cleanup EXIT

TARGET="texpresso_ci_missing_file.tex"

SDL_VIDEODRIVER=dummy build/texpresso -test-initialize test/missing-input.tex \
  < "$FIFO" > "$OUTFILE" 2>/dev/null &
PID=$!

# Background watchdog: hard kill if the test runs past the wall budget
( sleep "$WATCHDOG_TIMEOUT" && kill -TERM "$PID" 2>/dev/null ) &
WATCHER=$!

exec 3>"$FIFO"

# Pre-register the file before the engine asks for it
printf '(register "%s")\n' "$TARGET" >&3

# Bounded poll for the expected protocol message
for _ in $(seq 1 $MAX_POLLS); do
  grep -q "lookup-file read promised \"$TARGET\"" "$OUTFILE" 2>/dev/null && break
  sleep $POLL_INTERVAL
  kill -0 "$PID" 2>/dev/null || {
    echo "FAIL: engine died before emitting lookup-file promised for $TARGET"
    cat "$OUTFILE"
    exit 1
  }
done
grep -q "lookup-file read promised \"$TARGET\"" "$OUTFILE" 2>/dev/null || {
  echo "FAIL: timed out waiting for lookup-file promised for $TARGET (${WATCHDOG_TIMEOUT}s)"
  cat "$OUTFILE"
  exit 1
}

echo "Got lookup-file promised for: $TARGET"

# Provide the missing file content — resolves the deferred query
printf '(open "%s" "Included content.\\n")\n' "$TARGET" >&3
exec 3>&-

if wait "$PID"; then
  echo "PASS: register test"
else
  echo "FAIL: texpresso exited with error (possibly killed by watchdog)"
  exit 1
fi
