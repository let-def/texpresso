#!/usr/bin/env sh

mkdir -p build

cat >build/mupdf_test.c <<END
int main(int argc, char **argv)
{
    return 0;
}
END

if gcc -lmupdf-third -o build/mupdf_test build/mupdf_test.c &> /dev/null; then
    echo -lmupdf-third
else
    echo
fi
rm -f build/mupdf_test*
exit 0
