#!/bin/bash
# Build a LaTeX format (xelatex.fmt) with the wasm xetex engine, using the
# system TeX Live tree, into engines/wasm-fmt/. This is a one-time artifact
# (like the wasm binaries): the in-process engine loads it to run real LaTeX
# documents instead of re-parsing the kernel every run.
#
# Needs: engines/build-wasm2c-xetex/xetex-native (scripts/build-wasm2c-xetex.sh)
#        a system TeX Live (found via kpsewhich).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/engines/build-wasm2c-xetex/xetex-native"
DAT="$ROOT/engines/build-wasm-xetex/libs/icu/icu-build/data/out/icudt78l.dat"
FMTDIR="$ROOT/engines/wasm-fmt"

[ -x "$BIN" ] || { echo "missing $BIN — run build-wasm2c-xetex.sh first" >&2; exit 1; }
command -v kpsewhich >/dev/null || { echo "kpsewhich not found (need a TeX Live install)" >&2; exit 1; }

# Locate the system TeX Live tree. The wasm engine's kpathsea derives the tree
# from SELFAUTO*/TEXMFROOT, which point at our binary — so override explicitly.
R="$(kpsewhich -var-value=TEXMFROOT)"
export TEXMFROOT="$R"
export TEXMFDIST="$(kpsewhich -var-value=TEXMFDIST)"
export TEXMFLOCAL="$(kpsewhich -var-value=TEXMFLOCAL)"
export TEXMFSYSVAR="$(kpsewhich -var-value=TEXMFSYSVAR)"
export TEXMFSYSCONFIG="$(kpsewhich -var-value=TEXMFSYSCONFIG)"
export TEXMFVAR="$TEXMFSYSVAR"
export TEXMFCNF="$R:$TEXMFDIST/web2c"
echo "system TeX Live: $R"

mkdir -p "$FMTDIR"
cp -f "$DAT" "$FMTDIR/"
cd "$FMTDIR"
export ICU_DATA="$FMTDIR"

echo "=== building xelatex.fmt (loads the LaTeX kernel; ~1 min) ==="
"$BIN" -ini -etex -no-pdf xelatex.ini >/dev/null 2>&1 || true

[ -f "$FMTDIR/xelatex.fmt" ] || { echo "xelatex.fmt NOT produced" >&2; exit 1; }
echo "built: $FMTDIR/xelatex.fmt ($(wc -c < "$FMTDIR/xelatex.fmt") bytes)"
