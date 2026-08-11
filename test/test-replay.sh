#!/bin/bash
# Incremental replay must be byte-identical to a full compile.
#
# For an edit partway through the body, compare:
#   A  full compile of the EDITED document          (ground truth)
#   B  compile the ORIGINAL, then apply (change)    (incremental replay)
# The engine's "Output written on X (N pages, M bytes)" line is the comparison
# point — the output file itself stays in the in-memory VFS.
#
# Reads texpresso's output through a fifo and blocks on it, so each step happens
# as soon as the engine reports it rather than after a fixed sleep.
set -e

WATCHDOG_TIMEOUT=${WATCHDOG_TIMEOUT:-90}
PARAGRAPHS=400
TEXP="$PWD/build/texpresso"   # make runs from the repo root
SUMMARY='Output written on [A-Za-z0-9_.-]+ \([0-9]+ pages?, [0-9]+ bytes\)'

mkbody() { # $1 = marker word, $2 = paragraph to mark
  printf '\\documentclass{article}\n\\begin{document}\n'
  for i in $(seq 1 $PARAGRAPHS); do
    if [ "$i" = "$2" ]; then
      printf 'Paragraph %d %s filler words here.\n\n' "$i" "$1"
    else
      printf 'Paragraph %d with some filler words here.\n\n' "$i"
    fi
  done
  printf '\\end{document}\n'
}
esc() { sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' "$1" | awk '{if(NR>1)printf"\\n";printf"%s",$0}'; }

status=0
# 1% lands in the first few paragraphs, which selects the base checkpoint; the
# other three select deeper ones. Sampling only the middle of the document left
# the base-checkpoint path untested, and it was broken on Linux while every
# sampled position passed.
for PCT in 1 10 50 90; do
  EPAR=$((PARAGRAPHS * PCT / 100)); [ "$EPAR" -lt 1 ] && EPAR=1
  SRC=$(mktemp -d /tmp/txp-replay-src-XXXXXX)
  mkbody ORIGINALWORD   "$EPAR" >"$SRC/orig.tex"
  mkbody CHANGEDWORDXYZ "$EPAR" >"$SRC/edited.tex"
  OFF=$(grep -abo ORIGINALWORD "$SRC/orig.tex" | head -1 | cut -d: -f1)

  # A: ground truth — compile the edited document outright.
  A_DIR=$(mktemp -d /tmp/txp-replay-a-XXXXXX)
  cp "$SRC/edited.tex" "$A_DIR/doc.tex"
  A=$(cd "$A_DIR" && SDL_VIDEODRIVER=dummy "$TEXP" \
        -test-initialize ./doc.tex 2>/dev/null | grep -aoE "$SUMMARY" | tail -1)

  # B: compile the original, then edit it and let the engine replay.
  B_DIR=$(mktemp -d /tmp/txp-replay-b-XXXXXX); B_DIR=$(cd "$B_DIR" && pwd -P)
  cp "$SRC/orig.tex" "$B_DIR/doc.tex"
  CMD=$(mktemp -u /tmp/txp-replay-cmd-XXXXXX)
  OUTPIPE=$(mktemp -u /tmp/txp-replay-out-XXXXXX)
  mkfifo "$CMD" "$OUTPIPE"

  SDL_VIDEODRIVER=dummy "$TEXP" -stream "$B_DIR/doc.tex" \
    <"$CMD" >"$OUTPIPE" 2>/dev/null &
  PID=$!
  ( sleep "$WATCHDOG_TIMEOUT" && kill -TERM "$PID" 2>/dev/null ) >/dev/null 2>&1 &
  WATCHER=$!

  # Open both fifos before sending anything. The document is ~20 KB, so writing
  # it first would fill the command pipe and block us, while the engine is still
  # blocked opening its output pipe for want of a reader — each waiting on the
  # other. The small payloads in the other tests hide this; here it deadlocks.
  exec 3>"$CMD"
  exec 4<"$OUTPIPE"
  printf '(open "%s/doc.tex" "%s")\n(resume)\n' "$B_DIR" "$(esc "$SRC/orig.tex")" >&3

  first= ; B=
  while IFS= read -r line; do
    case "$line" in
      *"Output written on"*)
        s=$(printf '%s\n' "$line" | grep -aoE "$SUMMARY" | tail -1)
        [ -n "$s" ] || continue
        if [ -z "$first" ]; then
          # Original compiled; apply the edit and wait for the replay's output.
          first=$s
          printf '(change "%s/doc.tex" %d 12 "CHANGEDWORDXYZ")\n(resume)\n' "$B_DIR" "$OFF" >&3
        elif [ "$s" != "$first" ]; then
          B=$s
          break
        fi ;;
    esac
  done <&4

  exec 3>&- 4<&-
  kill "$PID" "$WATCHER" 2>/dev/null || true
  wait "$PID" 2>/dev/null || true
  rm -rf "$CMD" "$OUTPIPE" "$SRC" "$A_DIR" "$B_DIR"

  if [ -n "$A" ] && [ "$A" = "$B" ]; then
    echo "  edit at ${PCT}%: MATCH  ($A)"
  else
    echo "  edit at ${PCT}%: DIFF"
    echo "    full compile: ${A:-<none>}"
    echo "    replay:       ${B:-<none>}"
    status=1
  fi
done

[ $status -eq 0 ] && echo "PASS: replay test" || echo "FAIL: replay diverged from a full compile"
exit $status
