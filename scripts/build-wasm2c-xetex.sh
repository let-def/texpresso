#!/bin/bash
# wasm2c the xetex wasm and link a native (JS-free) engine.
#
# Same recipe as build-wasm2c-pdftex.sh with `-n engine`, so the identical host
# (src/engine-wasm/wasm_host.c) drives xetex. The generated engine.c is large
# (~180 MB) and slow to compile.
#
# Prerequisite: scripts/build-wasm-xetex.sh.
# Output: engines/build-wasm2c-xetex/xetex-native
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WASM="$ROOT/engines/build-wasm-xetex/texk/web2c/xetex.wasm"
OUT="$ROOT/engines/build-wasm2c-xetex"
HOST="$ROOT/src/engine-wasm/wasm_host.c"

[ -f "$WASM" ] || { echo "missing $WASM — run build-wasm-xetex.sh first" >&2; exit 1; }
command -v wasm2c >/dev/null || { echo "wasm2c not on PATH (install wabt)" >&2; exit 1; }

WABT_PREFIX="$(brew --prefix wabt)"
RT_SRC="$WABT_PREFIX/share/wabt/wasm2c"
RT_INC="$WABT_PREFIX/include"

mkdir -p "$OUT"; cd "$OUT"

echo "wasm2c -> engine.c/.h (large; ~a few seconds)"
wasm2c --enable-exceptions -n engine "$WASM" -o engine.c

echo "compiling generated module (large; slow) + runtime + host"
# -O1 for the 180 MB generated module: -O2 is far slower here for little gain.
cc -O1 -I"$RT_INC" -I"$OUT" -I"$ROOT/src/engine-wasm" -c engine.c -o engine.o
cc -O2 -I"$RT_INC" -c "$RT_SRC/wasm-rt-impl.c" -o wasm-rt-impl.o
cc -O2 -I"$RT_INC" -c "$RT_SRC/wasm-rt-mem-impl.c" -o wasm-rt-mem-impl.o
cc -O2 -I"$RT_INC" -c "$RT_SRC/wasm-rt-exceptions-impl.c" -o wasm-rt-exceptions-impl.o
cc -O2 -I"$RT_INC" -I"$OUT" -c "$HOST" -o wasm_host.o

cc -O2 -o xetex-native \
   wasm_host.o engine.o wasm-rt-impl.o wasm-rt-mem-impl.o wasm-rt-exceptions-impl.o \
   -lm

# ICU data belongs with the engine (it is built from the ICU inside this wasm),
# so keep it beside engine.c rather than with the formats.
DAT="$ROOT/engines/build-wasm-xetex/libs/icu/icu-build/data/out/icudt78l.dat"
[ -f "$DAT" ] && cp -f "$DAT" "$OUT/" && echo "icu data: $OUT/$(basename "$DAT")"

echo "built: $OUT/xetex-native"
