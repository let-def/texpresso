#!/usr/bin/env sh

FLAGS=$@

mkdir -p build

cat >build/mupdf_test.c <<END
int main(int argc, char **argv)
{
    return 0;
}
END

link_if_possible()
{
  if $CC "$1" $FLAGS -o build/mupdf_test build/mupdf_test.c > /dev/null; then
    printf " %s"  "$1"
  fi
}

link_if_possible -lmupdf-third
link_if_possible -lleptonica
link_if_possible -ltesseract

rm -f build/mupdf_test*

exit 0
