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
  for i in "$@"; do
    if $CC "$i" $FLAGS -o build/mupdf_test build/mupdf_test.c > /dev/null; then
      printf " %s"  "$i"
    fi
  done
}

link_if_possible -lmupdf-third -lleptonica -ltesseract -lmujs

rm -f build/mupdf_test*

exit 0
