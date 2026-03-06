#!/usr/bin/env sh

FLAGS=$@

mkdir -p build

cat >build/mupdf_test.c <<END
int main(int argc, char **argv)
{
    return 0;
}
END

LINKING=""
SKIPPING=""

link_if_possible()
{
  for i in "$@"; do
    if $CC "$i" $FLAGS -o build/mupdf_test build/mupdf_test.c 2> /dev/null; then
      LINKING="$LINKING $i"
      printf " %s"  "$i"
    else
      SKIPPING="$SKIPPING $i"
    fi
  done
}

link_if_possible -lmupdf-third -lleptonica -ltesseract -lmujs -lgumbo -ljbig2dec -lopenjp2

if [ -n "$LINKING" ]; then
  echo >&2 "Linking$LINKING"
fi
if [ -n "$SKIPPING" ]; then
  echo >&2 "Skipping$SKIPPING"
fi

rm -f build/mupdf_test*

exit 0
