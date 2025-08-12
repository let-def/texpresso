# Building and installing TeXpresso

If all dependencies are installed and out-of-the-box configuration works, `make all` should be sufficient to build both `texpresso` and `texpresso-xetex` in `build/` directory.

## Supported Systems

TeXpresso is in an early stage of development and its configuration logic is a rough hand-made script.
So far it has only been tested the following systems, where we expect it to work:

- macOS Sonoma (14.0)
- Fedora 40
- Arch Linux: [a PKGBUILD is available in the AUR](https://aur.archlinux.org/packages/texpresso-git) that builds from the latest Git HEAD on installation.
- Ubuntu 24.04
- Debian 12 (not tested, for now)

On other systems you may observe build failures that require modifying the Makefile. Let us know if it works on a system not listed above, or if you can tweak the configuration/build code to support your system without breaking others.

**Rerun `make config` when you change the build environment**, otherwise freshly installed libraries might not be considered by the build system.

### Ubuntu

(Tested with Ubuntu 24.04 ARM64/x86_64)

Install all needed dependencies with:
```sh
sudo apt update && sudo apt install build-essential libsdl2-dev libmupdf-dev libmujs-dev libfreetype-dev libgumbo-dev libjbig2dec0-dev libjpeg-dev libopenjp2-7-dev libssl-dev libfontconfig-dev libleptonica-dev libharfbuzz-dev mupdf snapd texlive-full texlive-xetex fonts-lmodern fonts-dejavu fonts-freefont-ttf fontconfig
sudo snap install tectonic
```

Details:
- `build-essential` install the compiler (GCC) and basic build tools (GNU Make)
- `libsdl2-dev`: SDL2 library
- `libmupdf-dev libmujs-dev libfreetype-dev  libgumbo-dev libjbig2dec0-dev libjpeg-dev libopenjp2-7-dev`: libmupdf and its dependencies
- `snapd` is another package manager, required for tectonic
- `tectonic` is a new `TeX` render engine, required for running `texpresso`
- `texlive-*` and `font*` are required for alternative `texpresso` mode

### Debian (Currently untested)

Debian 12 is quite similar to Ubuntu with the added difficulty that the rust version is too old for TeXpresso to build out of the box.

You can install the other dependencies:

```sh
sudo apt install build-essential libsdl2-dev libmupdf-dev libfreetype-dev libjpeg-dev libjbig2dec0-dev libharfbuzz-dev libopenjp2-7-dev libgumbo-dev libmujs-dev libssl-dev libfontconfig-dev
```

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
pacman -Sy --noconfirm texlive-core texlive-bin texlive-latexextra texlive-fontsextra texlive-fontsrecommended texlive-pictures texlive-science texlive-publishers texlive-music texlive-games texlive-humanities texlive-langcyrillic texlive-langextra texlive-langgreek texlive-langjapanese texlive-langchinese texlive-langkorean texlive-bibtexextra tectonic base-devel fontconfig freetype2 gcc-libs glibc graphite gumbo-parser harfbuzz icu jbig2dec libjpeg-turbo libmupdf libpng openjpeg2 openssl sdl2 zlib git libmupdf
```

### Fedora

(Tested on Fedora 40 x86_64)

Install all dependencies:

```sh
sudo dnf install rust cargo texlive-scheme-full make gcc gcc-c++ mupdf-devel SDL2-devel freetype-devel harfbuzz-devel libjpeg-turbo-devel jbig2dec-devel openjpeg2-devel gumbo-parser-devel tesseract-devel leptonica-devel openssl-devel fontconfig-devel graphite2-devel libicu-devel zlib-devel
```

Install `tectonic` with official script:

```sh
curl --proto '=https' --tlsv1.2 -fsSL https://drop-sh.fullyjustified.net |sh
mv tectonic /usr/local/bin # to make tectonic available in the $PATH
```

### macOS

(Tested Sonoma (14.0) Apple Silicon)

Install the following dependencies with homebrew:

```sh
brew install gcc mupdf-tools SDL2 tectonic texlive
```

> [!Note]
> `mupdf-tools` can be replaced by `mupdf`, either is fine.

> [!Note]
> Also, for macOS Sequoia (15.0) you may need to reinstall `gcc`, see [this](https://discussions.apple.com/thread/256033797?sortBy=rank) issue.

## Download

Simply clone the git repository using one of the following commands:

> [!Note]
> As of now the working branch is `detectonic`, which will be merge with `main`
> soon, so you need to use only `detectonic` branch.

```sh
# cloning by HTTP
git clone --single-branch --branch detectonic https://github.com/let-def/texpresso.git
# cloning by SSH
git clone --single-branch --branch detectonic git@github.com:let-def/texpresso.git
```

## Build TeXpresso

Make sure you have all the dependencies installed, see Supported Systems above.

To build the whole project use the following `make` targets:

```sh
make all
make fill-tectonic-cache # for faster file render
```

That's all! Now you can try render the test file:

```sh
build/texpresso test/simple.tex
```

This should open window with rendered `tex` file. If TeXpresso window does not display the document, please report an issue.

> [!Note]
> Expect first run to be quite slow, due to initial packages compilation

Other targets are:
- `config` to generate configuration in `Makefile.config` (automatically called during first build)
- `dev` produces `build/texpresso-dev` which supports hot-reloading to ease development
- `debug` produces debugging tools in `build/`
- `clean` to remove intermediate build files
- `distclean` to remove all build files (`build/` and `Makefile.config`)
- `test[-texlive | -tectonic]` test TeXpresso engine (aka headless mode).

If the build fails, try tweaking the configuration flags in `Makefile.config`.

## Using TeXpresso

[README.md](./README.md) has information on supported editors and how to control the TeXpresso viewer.
