#!/bin/bash
# Test lookup-file (non-blocking missing-file notification): engine emits
# (lookup-file read failed "...") for an unregistered missing file, editor
# provides it via (open ...), engine restarts via rollback and processes it
# successfully.
#
# Inherently async: the test must observe the failed lookup before sending
# the file. Bounded poll + background watchdog as the final kill-switch.
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

# Background watchdog
( sleep "$WATCHDOG_TIMEOUT" && kill -TERM "$PID" 2>/dev/null ) &
WATCHER=$!

exec 3>"$FIFO"

# Bounded poll for the expected protocol message (ignore .aux etc.)
for _ in $(seq 1 $MAX_POLLS); do
  grep -q "lookup-file read failed \"$TARGET\"" "$OUTFILE" 2>/dev/null && break
  sleep $POLL_INTERVAL
  kill -0 "$PID" 2>/dev/null || {
    echo "FAIL: engine died before emitting lookup-file failed for $TARGET"
    cat "$OUTFILE"
    exit 1
  }
done
grep -q "lookup-file read failed \"$TARGET\"" "$OUTFILE" 2>/dev/null || {
  echo "FAIL: timed out waiting for lookup-file failed for $TARGET (${WATCHDOG_TIMEOUT}s)"
  cat "$OUTFILE"
  exit 1
}

echo "Got lookup-file failed for: $TARGET"

# Provide the missing file content — engine will restart via rollback
printf '(open "%s" "Included content.\\n")\n' "$TARGET" >&3
exec 3>&-

if wait "$PID"; then
  echo "PASS: lookup-file test"
else
  echo "FAIL: texpresso exited with error (possibly killed by watchdog)"
  exit 1
fi
