# Building and installing TeXpresso

Building TeXpresso takes two steps: obtain a TeX engine, then build the
frontend.

```sh
make fetch-engine     # download a prebuilt engine (needs only a C compiler)
make                  # build build/texpresso
```

The engine is compiled into `build/texpresso`, so that single binary is the
whole program. See `WASM-ENGINE.md` for what the engine is and how it is built.

TeXpresso also needs an installed LaTeX distribution (TeX Live or Tectonic) at
runtime for packages and fonts.

## Supported systems

Linux and macOS. There is no Windows support: the engine host uses `ucontext`
and POSIX signal handling.

Tested on:

- macOS
- Fedora
- Arch Linux
- Debian 12
- Ubuntu 22.04 and 24.04

On other systems you may hit build failures that require modifying the Makefile.
Let us know if it works elsewhere, or if you can support your system without
breaking others.

**Rerun `make config` when you change the build environment**, otherwise freshly
installed libraries might not be picked up.

Note: the [AUR PKGBUILD](https://aur.archlinux.org/packages/texpresso-git) has
not been updated for the in-process engine and still builds the retired model.

### Ubuntu and Debian

```sh
apt install build-essential libsdl2-dev libmupdf-dev libmujs-dev \
  libfreetype-dev libgumbo-dev libjbig2dec0-dev libjpeg-dev \
  libopenjp2-7-dev libssl-dev libfontconfig-dev libleptonica-dev \
  libharfbuzz-dev mupdf \
  texlive-xetex texlive-latex-recommended texlive-extra-utils
```

Details:

- `build-essential`: the compiler (GCC) and basic build tools (GNU Make)
- `libsdl2-dev`: SDL2
- `libmupdf-dev libmujs-dev libfreetype-dev libgumbo-dev libjbig2dec0-dev
  libjpeg-dev libopenjp2-7-dev`: libmupdf and its dependencies
- `texlive-*`: the LaTeX distribution TeXpresso reads packages and fonts from

### Arch Linux (and Manjaro)

```sh
pacman -S base-devel gcc git curl freetype2 harfbuzz icu libjpeg-turbo \
  openjpeg2 libpng gumbo-parser tesseract leptonica openssl zlib sdl2 mupdf \
  texlive-bin texlive-core texlive-latexrecommended texlive-xetex \
  texlive-fontsrecommended
```

### Fedora

```sh
dnf install gcc gcc-c++ make clang curl tar mupdf-devel SDL2-devel \
  freetype-devel harfbuzz-devel libjpeg-turbo-devel jbig2dec-devel \
  openjpeg2-devel gumbo-parser-devel tesseract-devel leptonica-devel \
  openssl-devel fontconfig-devel graphite2-devel libicu-devel zlib-devel \
  texlive-scheme-basic texlive-collection-latexrecommended texlive-xetex
```

### macOS

```sh
brew install mupdf-tools SDL2
```

You also need a LaTeX distribution, e.g. MacTeX, or Tectonic.

> [!Note]
> `mupdf-tools` can be replaced by `mupdf`, either is fine.

> [!Note]
> For macOS Sequoia (15.0) you may need to reinstall `gcc`, see
> [this](https://discussions.apple.com/thread/256033797?sortBy=rank) issue.

## Download

```sh
git clone https://github.com/let-def/texpresso.git   # cloning by HTTP
git clone git@github.com:let-def/texpresso.git       # cloning by SSH
```

(Adjust the URL if you are looking at a different fork.)

## Get an engine

The engine is not built by `make`, and a build never reaches the network on its
own. Choose one:

```sh
make fetch-engine                        # prebuilt bundle; needs only a C compiler
make engine-source                       # build from pinned TeX Live sources
```

`engine-source` needs emscripten and wabt, and takes considerably longer.

Both honour `TEXPRESSO_ENGINE`, which selects the engine: `xetex` (default),
`pdftex` or `luatex`. The same variable selects the engine compiled into the
frontend, so use it consistently:

```sh
TEXPRESSO_ENGINE=luatex make fetch-engine
TEXPRESSO_ENGINE=luatex make
```

No format file is shipped. If the format your engine needs is not present, it is
generated on the first run and cached, which makes that run slow.

## Build

Make sure the dependencies are available: `pkg-config`, `SDL2`, `mupdf` (and its
own dependencies: `libjpeg`, `libpng`, `freetype2`, `gumbo`, `jbig2dec`, and
possibly `leptonica`, `tesseract` and `mujs` depending on the mupdf version).
Under macOS, `brew` is also used to find local files.

`make` (or `make texpresso`) produces `build/texpresso`.

Other targets:

- `config` generates `Makefile.config` (called automatically on first build)
- `engines` builds one frontend per engine: `build/texpresso-{xetex,pdftex,luatex}`,
  each needing that engine fetched or built first
- `engine-native` builds the standalone engine binary used by `make test-fence`
- `debug-proxy` builds `texpresso-debug-proxy`
- `macos-app` (macOS only) bundles into `build/TeXpresso.app`; requires
  `rsvg-convert` and `iconutil` in `PATH`
- `test-report` runs the test suite and reports PASS/FAIL per target
- `clean` removes intermediate build files
- `distclean` removes all build files (`build/` and `Makefile.config`)

If the build fails, try tweaking the configuration flags in `Makefile.config`.

## Package providers

A LaTeX distribution comes with many packages and resource files. TeXpresso uses
an existing installation:

- it defaults to TeXlive if the `kpsewhich` command is available.
- it falls back to Tectonic if the `tectonic` command is available (in `PATH`)

Force a specific distribution with the `-texlive` or `-tectonic` flags.

## Testing TeXpresso

```sh
build/texpresso test/simple.tex
```

Or select a distribution:

```sh
build/texpresso -texlive test/simple.tex
build/texpresso -tectonic test/simple.tex
```

This is a minimal check that TeXpresso is installed correctly. If the window
does not display the document, please report an issue.

> [!Note]
> Expect the first run to be slow: the format is generated if absent, and
> packages are compiled for the first time.

Run the full suite with `make test-report`.

### Tectonic initialization

Tectonic can be slow the first time a file is needed, since it downloads it. To
prime the cache:

```sh
make fill-tectonic-cache
```

## Using TeXpresso

[README.md](./README.md) has information on supported editors and how to control
the TeXpresso viewer.
