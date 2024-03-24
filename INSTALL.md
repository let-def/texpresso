# Building and installing TeXpresso

If all dependencies are installed and out-of-the-box configuration works, `make all` should be sufficient to build both `texpresso` and `texpresso-tonic` in `build/` directory.

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

### Ubuntu

(Tested with Ubuntu 22.04 ARM64)

Install all needed dependencies with:
```sh
apt install build-essential libsdl2-dev re2c libmupdf-dev libmujs-dev libfreetype-dev  libgumbo-dev libjbig2dec0-dev libjpeg-dev libopenjp2-7-dev cargo libssl-dev libfontconfig-dev
```

Details:
- `build-essential` install the compiler (GCC) and basic build tools (GNU Make)
- `libsdl2-dev`: SDL2 library
- `re2c`: re2c preprocessor, necessary for generating lexing code
- `libmupdf-dev libmujs-dev libfreetype-dev  libgumbo-dev libjbig2dec0-dev libjpeg-dev libopenjp2-7-dev`: libmupdf and its dependencies
- `cargo libssl-dev libfontconfig-dev`: rust package manager, and dependencies needed by texpresso-tonic rust code

### Debian 12

Debian 12 is quite similar to Ubuntu with the added difficulty that the rust version is too old for TeXpresso to build out of the box.

You can install the other dependencies:

```sh
sudo apt install build-essential libsdl2-dev re2c libmupdf-dev libfreetype-dev libjpeg-dev libjbig2dec0-dev libharfbuzz-dev libopenjp2-7-dev libgumbo-dev libmujs-dev libssl-dev libfontconfig-dev
```

A workaround for rust is to install [rustup](https://rustup.rs). Make sure that curl is installed and setup rustup:

```sh
sudo apt install curl
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

After rustup is installed, source the environment before building. E.g:
```sh
source $HOME/.cargo/env
```

### Arch Linux (and Manjaro)

Dependencies are listed in the PKGBUILD, but if you need to install them manually:

```sh
pacman -S base-devel fontconfig freetype2 gcc-libs glibc graphite gumbo-parser harfbuzz icu jbig2dec libjpeg-turbo libmupdf libpng openjpeg2 openssl sdl2 zlib cargo git libmupdf re2c
```

### Fedora

(Tested on Fedora 38 ARM64)

Install all dependencies:

```sh
sudo dnf install make gcc mupdf-devel SDL2-devel re2c g++ freetype2-devel libjpeg-turbo-devel jbig2dec-devel openjpeg2-devel gumbo-parser-devel tesseract-devel leptonica-devel cargo openssl-devel fontconfig-devel
```

## Download

Simply clone the git repository (and its submodules) using one of the following commands:

```
git clone --recurse-submodules https://github.com/let-def/texpresso.git   # cloning by HTTP
git clone --recurse-submodules git@github.com:let-def/texpresso.git       # cloning by SSH
```

(You may want to adjust the URL if you are looking at a different fork.)

Note that while TeXpresso itself (the driver/viewer program) is small (less than 2MiB of sources, about 40MiB once built), the `tectonic` LaTeX engine that we include as a submodule is large -- 120MiB of sources, most of it from its `harfbuzz` dependency, and about 1.2GiB once built.

## Build TeXpresso

First make sure the dependencies are available: `pkg-config`, `re2c`, `SDL2`, `mupdf` (and its own dependencies: `libjpeg`, `libpng`, `freetype2`, `gumbo`, `jbig2dec`... and possibly `leptonica`, `tesseract` and `mujs` depending on the mupdf version).
Under macOS, `brew` is also used to find local files.

If it succeeds, `make texpresso` produces `build/texpresso`.

Other targets are:
- `config` to generate configuration in `Makefile.config` (automatically called during first build)
- `dev` produces `build/texpresso-dev` which supports hot-reloading to ease development
- `debug` produces debugging tools in `build/`
- `clean` to remove intermediate build files
- `distclean` to remove all build files (`build/` and `Makefile.config`)

If the build fails, try tweaking the configuration flags in `Makefile.config`.

## Build TeXpresso-tonic (Tectonic)

First you need an environment that is able to build Tectonic: a functional rust
and cargo installation, etc. Check tectonic documentation.

Then make sure that the git submodule has been initialized (in the `tectonic` directory):

```sh
git submodule update --init --recursive
```

Then `make texpresso-tonic` should work.

## Testing TeXpresso

If both commands built successfully, you can try TeXpresso using:

```sh
build/texpresso test/simple.tex
```

This is just a minimal test to make sure that TeXpresso is installed correctly.
If TeXpresso window does not display the document, please report an issue.

## Using TeXpresso

[README.md](./README.md) has information on supported editors and how to control the TeXpresso viewer.
