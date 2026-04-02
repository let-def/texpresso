#!/bin/bash
# Test register + deferred Q_OPRD: editor pre-registers a file,
# engine defers the query, editor provides content, engine resumes.
set -e

FIFO=$(mktemp -u /tmp/texpresso-fifo-XXXXXX)
OUTFILE=$(mktemp /tmp/texpresso-out-XXXXXX)
mkfifo "$FIFO"
trap 'rm -f "$FIFO" "$OUTFILE"; kill "$PID" 2>/dev/null || true' EXIT

TARGET="texpresso_ci_missing_file.tex"

SDL_VIDEODRIVER=dummy build/texpresso -test-initialize test/missing-input.tex \
  < "$FIFO" > "$OUTFILE" 2>/dev/null &
PID=$!

exec 3>"$FIFO"

# Pre-register the file before the engine asks for it
printf '(register "%s")\n' "$TARGET" >&3

# Wait for request-file (engine deferred Q_OPRD for the promised file)
while ! grep -q "request-file \"$TARGET\"" "$OUTFILE" 2>/dev/null; do
  sleep 0.5
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "FAIL: texpresso exited before emitting request-file for $TARGET"
    echo "stdout contents:"
    cat "$OUTFILE"
    exit 1
  fi
done

echo "Got request-file for: $TARGET"

# Provide the missing file content (resolves deferred query)
printf '(open "%s" "Included content.\\n")\n' "$TARGET" >&3
exec 3>&-

if wait "$PID"; then
  echo "PASS: register test"
else
  echo "FAIL: texpresso exited with error"
  exit 1
fi
