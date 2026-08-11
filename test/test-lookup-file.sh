#!/bin/bash
# Test lookup-file (non-blocking missing-file notification): the engine reports
# an unregistered missing file, the editor supplies it, and the engine restarts
# and processes it.
#
# texpresso's output is read through a fifo and the loop blocks on it, so the
# reply is sent the moment the notification appears and the test ends when the
# engine closes stdout. Polling a regular file cannot block, which is why this
# used to sleep in a loop and react up to an interval late.
set -e

WATCHDOG_TIMEOUT=${WATCHDOG_TIMEOUT:-60}

CMD=$(mktemp -u /tmp/texpresso-cmd-XXXXXX)
OUTPIPE=$(mktemp -u /tmp/texpresso-outpipe-XXXXXX)
OUTFILE=$(mktemp /tmp/texpresso-out-XXXXXX)
mkfifo "$CMD" "$OUTPIPE"

cleanup() {
  rm -f "$CMD" "$OUTPIPE" "$OUTFILE"
  kill "$PID" 2>/dev/null || true
  kill "$WATCHER" 2>/dev/null || true
}
trap cleanup EXIT

TARGET="texpresso_ci_missing_file.tex"

SDL_VIDEODRIVER=dummy build/texpresso -test-initialize test/missing-input.tex \
  < "$CMD" > "$OUTPIPE" 2>/dev/null &
PID=$!

# Safety net only: the loop below ends on its own when the engine exits.
( sleep "$WATCHDOG_TIMEOUT" && kill -TERM "$PID" 2>/dev/null ) >/dev/null 2>&1 &
WATCHER=$!

# Order matters: the engine opens CMD for reading before OUTPIPE for writing,
# so this must be opened before we start reading OUTPIPE or both sides block.
exec 3>"$CMD"

sent=
while IFS= read -r line; do
  printf '%s\n' "$line" >>"$OUTFILE"
  if [ -z "$sent" ] && [[ "$line" == *"lookup-file read failed \"$TARGET\""* ]]; then
    printf '(open "%s" "Included content.\\n")\n' "$TARGET" >&3
    sent=1
  fi
done <"$OUTPIPE"
# Kept open until now on purpose: closing it as soon as the file was sent made
# stdin_eof true mid-recompile, and -test-initialize quits on
# "terminated && stdin_eof" — so the engine could exit before finishing the run
# it had just been given the file for. An editor would not close it either.
exec 3>&-

if [ -z "$sent" ]; then
  echo "FAIL: never saw lookup-file failed for $TARGET"
  cat "$OUTFILE"
  exit 1
fi
echo "Got lookup-file failed for: $TARGET"

if ! wait "$PID"; then
  echo "FAIL: texpresso exited with error (possibly killed by watchdog)"
  exit 1
fi

# The notification alone proves nothing — require that the supplied file was
# actually used and the document typeset.
if ! grep -q "Output written on" "$OUTFILE"; then
  echo "FAIL: no output produced after supplying $TARGET"
  echo "--- last 40 lines of engine output ---"
  tail -40 "$OUTFILE"
  exit 1
fi

echo "PASS: lookup-file test"
