#!/bin/bash
# wasm2c the pdftex wasm and link a native (JS-free) engine.
#
# Generated with `-n engine` so the module symbols are w2c_engine_* — the same
# host (src/engine-wasm/wasm_host.c) drives pdftex and xetex unchanged.
#
# Prerequisite: scripts/build-wasm-pdftex.sh.
# Output: engines/build-wasm2c-pdftex/pdftex-native
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WASM="$ROOT/engines/build-wasm-pdftex/texk/web2c/pdftex.wasm"
OUT="$ROOT/engines/build-wasm2c-pdftex"
HOST="$ROOT/src/engine-wasm/wasm_host.c"

[ -f "$WASM" ] || { echo "missing $WASM — run build-wasm-pdftex.sh first" >&2; exit 1; }
command -v wasm2c >/dev/null || { echo "wasm2c not on PATH (install wabt)" >&2; exit 1; }

WABT_PREFIX="$(brew --prefix wabt)"
RT_SRC="$WABT_PREFIX/share/wabt/wasm2c"
RT_INC="$WABT_PREFIX/include"

mkdir -p "$OUT"; cd "$OUT"

# --enable-exceptions: the wasm uses the EH tag section (SUPPORT_LONGJMP=wasm).
# -n engine: normalize module symbols so the host is engine-agnostic.
echo "wasm2c -> engine.c/.h"
wasm2c --enable-exceptions -n engine "$WASM" -o engine.c

echo "compiling generated module + runtime + host"
cc -O2 -I"$RT_INC" -I"$OUT" -I"$ROOT/src/engine-wasm" -c engine.c -o engine.o
cc -O2 -I"$RT_INC" -c "$RT_SRC/wasm-rt-impl.c" -o wasm-rt-impl.o
cc -O2 -I"$RT_INC" -c "$RT_SRC/wasm-rt-mem-impl.c" -o wasm-rt-mem-impl.o
cc -O2 -I"$RT_INC" -c "$RT_SRC/wasm-rt-exceptions-impl.c" -o wasm-rt-exceptions-impl.o
cc -O2 -I"$RT_INC" -I"$OUT" -c "$HOST" -o wasm_host.o

cc -O2 -o pdftex-native \
   wasm_host.o engine.o wasm-rt-impl.o wasm-rt-mem-impl.o wasm-rt-exceptions-impl.o \
   -lm

echo "built: $OUT/pdftex-native"
