# Building and installing TeXpresso

If all dependencies are installed and out-of-the-box configuration works, `make all` should be sufficient to build both `texpresso` and `texpresso-xetex` in `build/` directory.

## Supported systems

TeXpresso is in an early stage of development and its configuration logic is a rough hand-made script.
So far it has only been tested the following systems, where we expect it to work:

- OSX
- Fedora 39
- Arch Linux: [a PKGBUILD is available in the AUR](https://aur.archlinux.org/packages/texpresso-git) that builds from the latest Git HEAD on installation.
- Debian 12
- Ubuntu 22.04

On other systems you may observe build failures that require modifying the Makefile. Let us know if it works on a system not listed above, or if you can tweak the configuration/build code to support your system without breaking others.

**Rerun `make config` when you change the build environment**, otherwise freshly installed libraries might not be considered by the build system.

### Ubuntu and Debian

(Tested with Ubuntu 22.04 ARM64 and Ubuntu 20.04)

Install all needed dependencies with:
```sh
apt install build-essential libsdl2-dev libmupdf-dev libmujs-dev libfreetype-dev  libgumbo-dev libjbig2dec0-dev libjpeg-dev libopenjp2-7-dev cargo libssl-dev libfontconfig-dev libleptonica-dev libharfbuzz-dev
```

Details:
- `build-essential` install the compiler (GCC) and basic build tools (GNU Make)
- `libsdl2-dev`: SDL2 library
- `libmupdf-dev libmujs-dev libfreetype-dev libgumbo-dev libjbig2dec0-dev libjpeg-dev libopenjp2-7-dev`: libmupdf and its dependencies

### Arch Linux (and Manjaro)

Dependencies are listed in the PKGBUILD, but if you need to install them manually:

```sh
pacman -S base-devel fontconfig freetype2 gcc-libs glibc graphite gumbo-parser harfbuzz icu jbig2dec libjpeg-turbo libmupdf libpng openjpeg2 openssl sdl2 zlib git libmupdf
```

### Fedora

(Tested on Fedora 38 ARM64)

Install all dependencies:

```sh
sudo dnf install make gcc mupdf-devel SDL2-devel  g++ freetype2-devel libjpeg-turbo-devel jbig2dec-devel openjpeg2-devel gumbo-parser-devel tesseract-devel leptonica-devel cargo openssl-devel fontconfig-devel
```

### OSX

(Tested on Ventura Intel and Sonoma Apple Sillicon)

Install the following dependencies with homebrew:

```sh
brew install mupdf-tools SDL2
```

>[!Note]
>`mupdf-tools` can be replaced by `mupdf`, either is fine.

> [!Note]
> Also, for macOS Sequoia (15.0) you may need to reinstall `gcc`, see [this](https://discussions.apple.com/thread/256033797?sortBy=rank) issue.

## Download

Simply clone the git repository using one of the following commands:

```
git clone https://github.com/let-def/texpresso.git   # cloning by HTTP
git clone git@github.com:let-def/texpresso.git       # cloning by SSH
```

(You may want to adjust the URL if you are looking at a different fork.)

## Build TeXpresso and TeXpresso-XeTeX

First make sure the dependencies are available: `pkg-config`, `SDL2`, `mupdf` (and its own dependencies: `libjpeg`, `libpng`, `freetype2`, `gumbo`, `jbig2dec`... and possibly `leptonica`, `tesseract` and `mujs` depending on the mupdf version).
Under macOS, `brew` is also used to find local files.

If it succeeds, `make texpresso` produces `build/texpresso` and `make texpresso-xetex` produces `build/texpresso-xetex`.

Other targets are:
- `config` to generate configuration in `Makefile.config` (automatically called during first build)
- `dev` produces `build/texpresso-dev` which supports hot-reloading to ease development
- `debug` produces debugging tools in `build/`
- `clean` to remove intermediate build files
- `distclean` to remove all build files (`build/` and `Makefile.config`)

If the build fails, try tweaking the configuration flags in `Makefile.config`.

## Package providers

A LaTeX distribution comes with many packages and resource files.
TeXpresso uses an existing installation:
- it defaults to TeXlive if the `kpsewhich` command is available.
- it falls back to Tectonic if the `tectonic` command is available (in `PATH`)

It is possible to force selecting a specific distribution by passing the
`-tectonic` or `-texlive` flags.

## Testing TeXpresso

If both commands built successfully, you can try TeXpresso using:

```sh
build/texpresso test/simple.tex
```

Or:
```sh
build/texpresso -texlive test/simple.tex
build/texpresso -tectonic test/simple.tex
```
to select a specific distribution.

This is just a minimal test to make sure that TeXpresso is installed correctly.
If TeXpresso window does not display the document, please report an issue.

> [!Note]
> Expect first run to be quite slow, due to initial packages compilation

### Tectonic initialization

Using Tectonic can be quite slow when a file is needed for the first time as it needs to be downloaded.
To speed up the process, you can run:
```sh
make fill-tectonic-cache
```

## Using TeXpresso

[README.md](./README.md) has information on supported editors and how to control the TeXpresso viewer.
