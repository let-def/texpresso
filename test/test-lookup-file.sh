#!/bin/bash
# Test lookup-file (non-blocking missing-file notification): engine emits
# (lookup-file read failed "...") for an unregistered missing file, editor
# provides it via (open ...), engine restarts and processes it successfully.
set -e

FIFO=$(mktemp -u /tmp/texpresso-fifo-XXXXXX)
OUTFILE=$(mktemp /tmp/texpresso-out-XXXXXX)
mkfifo "$FIFO"
trap 'rm -f "$FIFO" "$OUTFILE"; kill "$PID" 2>/dev/null || true' EXIT

TARGET="texpresso_ci_missing_file.tex"

# Start texpresso in background, reading stdin from FIFO, stdout to file
SDL_VIDEODRIVER=dummy build/texpresso -test-initialize test/missing-input.tex \
  < "$FIFO" > "$OUTFILE" 2>/dev/null &
PID=$!

# Open FIFO for writing (unblocks texpresso's stdin)
exec 3>"$FIFO"

# Wait for (lookup-file read failed "...") for the target file (ignore .aux etc.)
while ! grep -q "lookup-file read failed \"$TARGET\"" "$OUTFILE" 2>/dev/null; do
  sleep 0.5
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "FAIL: texpresso exited before emitting lookup-file failed for $TARGET"
    echo "stdout contents:"
    cat "$OUTFILE"
    exit 1
  fi
done

echo "Got lookup-file failed for: $TARGET"

# Provide the missing file content
printf '(open "%s" "Included content.\\n")\n' "$TARGET" >&3
exec 3>&-

# Wait for texpresso to finish (it exits after page_count > 0 in -test-initialize mode)
if wait "$PID"; then
  echo "PASS: lookup-file test"
else
  echo "FAIL: texpresso exited with error"
  exit 1
fi
