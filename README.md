# TeXpresso: live rendering and error reporting for LaTeX 

_Note: this is an experimental tool._

**Important: this repository uses submodules. Clone using `git clone --recurse-submodules`.**

TeXpresso has been tested on Linux and macOS and should work with both AMD64 and Apple Silicon architectures.

It is made of two components:
- this repository which implements the `texpresso` binary
- the [tectonic/](tectonic/) git-submodule which implements a patched version of [Tectonic](https://github.com/tectonic-typesetting/tectonic) that produces the `texpresso-tonic` helper binary

# Building

If all dependencies are installed and out-of-the-box configuration works, `make all` should be sufficient to build both `texpresso` and `texpresso-tonic` in `build/` directory.

Otherwise, read below.

## Build TeXpresso

For, make sure the dependencies are available: `pkg-config`, `re2c`, `SDL2`, `mupdf` and its dependencies (`libjpeg`, `libpng`, `freetype2`, `gumbo`, ...). 
Under macOS, `brew` is also used to find local files 

If it succeeds, `make texpresso` produces `build/texpresso`.

Other targets are:
- `config` to generate configuration in `Makefile.config` (automatically called during building)
- `dev` produces `build/texpresso-dev` which supports hot-reloading to ease development 
- `debug` produces debugging tools in `build/` 
- `clean` to remove intermediate build files
- `distclean` to remove all build files (`build/` and `Makefile.config`)

If build fails, try tweaking the configuration flags in `Makefile.config`.

## Build TeXpresso-tonic (Tectonic)

First you need an environment that is able to build Tectonic: a functional rust
and cargo installation, etc. Check tectonic documentation.

Then make sure that the git submodules were initialized:

```sh
git submodule update --init --recursive
```

Then `make texpresso-tonic` should work.

## Testing TeXpresso

If both commands built succesfully, you can try TeXpresso using:

```sh
build/texpresso test/simple.tex
```

# Emacs mode

TeXpresso comes with an Emacs mode. The source can be found in
[emacs/texpresso.el](emacs/texpresso.el).  Load this file in Emacs (using `M-X load-file`; it is also compatible with `require`).

Start TeXpresso with `M-x texpresso`. The prompt will let you select the master/root TeX file. 
It will try to start `texpresso` command. If it is not possible, it will open
`(customize-variable 'texpresso-binary)` to let you set the path to texpresso
binary.

To work correctly, `texpresso` needs `texpresso-tonic` helper; when copying them, make sure they are both in the same directory. 
