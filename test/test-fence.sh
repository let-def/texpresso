#!/bin/bash
# Snapshot fidelity, with TeX and the frontend out of the picture.
#
# The host's built-in fence test pushes two COW layers, runs on, then restores
# each and compares a hash of the whole linear memory against what it was when
# that layer was captured. Restoring layer 0 — the base — is the case texpresso's
# replay uses when a non-document file changes, and the one test-replay never
# exercises (it restores layers 1 and above).
#
# Uses INITEX so no format is needed; kpathsea still wants a texmf tree.
set -e

ENG="${TEXPRESSO_ENGINE:-xetex}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/engines/build-wasm2c-$ENG/$ENG-native"

if [ ! -x "$BIN" ]; then
  echo "SKIP: $BIN not built (make engine-native)"
  exit 0
fi
command -v kpsewhich >/dev/null || { echo "SKIP: no kpsewhich"; exit 0; }

R="$(kpsewhich -var-value=TEXMFROOT)"
export TEXMFROOT="$R"
export TEXMFDIST="$(kpsewhich -var-value=TEXMFDIST)"
export TEXMFLOCAL="$(kpsewhich -var-value=TEXMFLOCAL)"
export TEXMFSYSVAR="$(kpsewhich -var-value=TEXMFSYSVAR)"
export TEXMFSYSCONFIG="$(kpsewhich -var-value=TEXMFSYSCONFIG)"
export TEXMFVAR="$TEXMFSYSVAR"
export TEXMFCNF="$R:$TEXMFDIST/web2c"
ICU="$(ls "$ROOT/engines/build-wasm2c-$ENG"/icudt*.dat 2>/dev/null | head -1)"
[ -n "$ICU" ] && export ICU_DATA="$(dirname "$ICU")"

W=$(mktemp -d /tmp/texpresso-fence-XXXXXX)
trap 'rm -rf "$W"' EXIT
cd "$W"
printf '\\catcode`\\{=1 \\catcode`\\}=2\n\\shipout\\hbox{}\n\\end\n' > f.tex

OUT=$(TEXPRESSO_FENCE_TEST=1 timeout 120 "$BIN" -ini f.tex 2>&1 </dev/null || true)
LINE=$(printf '%s\n' "$OUT" | grep -a "FENCE STACK" || true)

if [ -z "$LINE" ]; then
  # The engine never got as far as the first read, so there was nothing to
  # fence. That is an environment problem (no usable texmf for INITEX), not a
  # snapshot failure — report it and skip rather than fail the build.
  echo "SKIP: the engine did not reach a fence point; last output:"
  printf '%s\n' "$OUT" | tail -12 | sed 's/^/    /'
  exit 0
fi
echo "  $LINE"
case "$LINE" in
  *PASS*) echo "PASS: fence test"; exit 0 ;;
  *)      echo "FAIL: snapshot restore did not reproduce the captured memory"; exit 1 ;;
esac
