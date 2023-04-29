# texpresso

A wrapper to speed-up latex

Run `make` to build the tool:
```
$ make
```

If build succeeds, artifacts are put in the `output` directory:
```
$ ls output/
libintexcept.so  proxy.exe
```

You can test the tool by running pdflatex or latex like that:

```
output/proxy.exe pdflatex -synctex=-1 -interaction nonstopmode input.tex
```

The proxy will run the build and wait for changes.
