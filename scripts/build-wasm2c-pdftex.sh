#!/bin/bash
# wasm2c the standalone pdftex wasm and link a native (JS-free) pdftex.
#
# Prerequisite: scripts/build-wasm-pdftex.sh (produces the standalone wasm).
# Output: engines/build-wasm2c/pdftex-native
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WASM="$ROOT/engines/build-wasm/texk/web2c/pdftex.wasm"
OUT="$ROOT/engines/build-wasm2c"
HOST="$ROOT/src/engine-wasm/wasm_host.c"

[ -f "$WASM" ] || { echo "missing $WASM — run build-wasm-pdftex.sh first" >&2; exit 1; }
command -v wasm2c >/dev/null || { echo "wasm2c not on PATH (install wabt)" >&2; exit 1; }

# wabt's wasm2c runtime sources/headers
WABT_PREFIX="$(brew --prefix wabt)"
RT_SRC="$WABT_PREFIX/share/wabt/wasm2c"
RT_INC="$WABT_PREFIX/include"

mkdir -p "$OUT"; cd "$OUT"

# --enable-exceptions: the wasm uses the EH tag section (SUPPORT_LONGJMP=wasm).
echo "wasm2c -> pdftex.c/.h"
wasm2c --enable-exceptions "$WASM" -o pdftex.c

echo "compiling generated module + runtime + host"
cc -O2 -I"$RT_INC" -I"$OUT" -I"$ROOT/src/engine-wasm" \
   -c pdftex.c -o pdftex.o
cc -O2 -I"$RT_INC" -c "$RT_SRC/wasm-rt-impl.c" -o wasm-rt-impl.o
cc -O2 -I"$RT_INC" -c "$RT_SRC/wasm-rt-mem-impl.c" -o wasm-rt-mem-impl.o
cc -O2 -I"$RT_INC" -c "$RT_SRC/wasm-rt-exceptions-impl.c" -o wasm-rt-exceptions-impl.o
cc -O2 -I"$RT_INC" -I"$OUT" -c "$HOST" -o wasm_host.o

cc -O2 -o pdftex-native \
   wasm_host.o pdftex.o wasm-rt-impl.o wasm-rt-mem-impl.o wasm-rt-exceptions-impl.o \
   -lm

echo "built: $OUT/pdftex-native"
