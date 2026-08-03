#!/bin/bash
# Build a LaTeX format (<format>.fmt) with a wasm TeX engine, using the system
# TeX Live tree, into engines/wasm-fmt/. This is a one-time artifact (like the
# wasm binaries): the in-process engine loads it per session to run real LaTeX
# documents instead of re-parsing the kernel every run.
#
# The format is NOT baked into any binary — texpresso locates engines/wasm-fmt/
# at startup and passes -fmt=<format> (see locate_wasm_fmt in engine_tex.c).
#
# Needs: engines/build-wasm2c-<eng>/<eng>-native (scripts/build-wasm2c-<eng>.sh)
#        a system TeX Live (found via kpsewhich).
#
# Usage: scripts/build-wasm-fmt.sh [xetex|pdftex|luatex]   (default: xetex)
set -euo pipefail

ENG="${1:-xetex}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/engines/build-wasm2c-$ENG/$ENG-native"
FMTDIR="$ROOT/engines/wasm-fmt"

# Per-engine: the format to dump and the engine flags INITEX needs. Keep the
# format names in sync with the profiles in src/frontend/engine_tex_<eng>.c.
case "$ENG" in
  xetex)  FMT=xelatex;  INIFLAGS=(-no-pdf) ;;
  pdftex) FMT=pdflatex; INIFLAGS=() ;;
  luatex) FMT=lualatex; INIFLAGS=() ;;
  *) echo "unknown engine: $ENG (expected xetex|pdftex|luatex)" >&2; exit 1 ;;
esac

[ -x "$BIN" ] || { echo "missing $BIN — run build-wasm2c-$ENG.sh first" >&2; exit 1; }
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

# xetex needs its ICU data next to the format (ICU_DATA); the others don't.
if [ "$ENG" = xetex ]; then
  DAT="$ROOT/engines/build-wasm-xetex/libs/icu/icu-build/data/out/icudt78l.dat"
  [ -f "$DAT" ] || { echo "missing ICU data $DAT" >&2; exit 1; }
  cp -f "$DAT" "$FMTDIR/"
fi

cd "$FMTDIR"
export ICU_DATA="$FMTDIR"

echo "=== building $FMT.fmt with $ENG (loads the LaTeX kernel; ~1 min) ==="
# -etex is required: latex.ltx aborts with "LaTeX requires e-TeX" without it.
"$BIN" -ini -etex "${INIFLAGS[@]}" -jobname="$FMT" -progname="$FMT" "$FMT.ini" \
  >/dev/null 2>&1 || true

[ -f "$FMTDIR/$FMT.fmt" ] || { echo "$FMT.fmt NOT produced" >&2; exit 1; }
echo "built: $FMTDIR/$FMT.fmt ($(wc -c < "$FMTDIR/$FMT.fmt") bytes)"
