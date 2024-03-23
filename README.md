# TeXpresso: live rendering and error reporting for LaTeX

_Note: TeXpresso is still in an early development phase._

**Important: this repository uses submodules. Clone using `git clone --recurse-submodules`.**

TeXpresso has been tested on Linux and macOS and should work with both AMD64 and Apple Silicon architectures. See the [screencasts](#Screencasts) at the end of this file.

It is made of two components:
- this repository which implements the `texpresso` binary
- the [tectonic/](tectonic/) git-submodule which implements a patched version of [Tectonic](https://github.com/tectonic-typesetting/tectonic) that produces the `texpresso-tonic` helper binary

TeXpressos uses the same data store as [Tectonic](https://github.com/tectonic-typesetting/tectonic) and both should cohabit sanely. (Data are stored in `tectonic -X show user-cache-dir` / `texpresso-tonic -X show user-cache-dir`).

# Building

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
git submodule update --init
```

Then `make texpresso-tonic` should work.

## Testing TeXpresso

If both commands built successfully, you can try TeXpresso using:

```sh
build/texpresso test/simple.tex
```

This is just a minimal test to make sure that TeXpresso is installed correctly.
If TeXpresso window does not display the document, please report an issue.
Recommended use is with Emacs, see below. Vim support will come later.

# Emacs mode

TeXpresso comes with an Emacs mode. The source can be found in
[emacs/texpresso.el](emacs/texpresso.el).  Load this file in Emacs (using `M-X load-file`; it is also compatible with `require`).

Start TeXpresso with `M-x texpresso`. The prompt will let you select the master/root TeX file.
It will try to start `texpresso` command. If it is not possible, it will open
`(customize-variable 'texpresso-binary)` to let you set the path to texpresso
binary.

To work correctly, `texpresso` needs `texpresso-tonic` helper; when copying them, make sure they are both in the same directory.

`M-x texpresso-display-output` will open a small window listing TeX warnings and errors on the current page.
Use `M-x texpresso-next-page` and `M-x texpresso-previous-page` to move between pages without leaving Emacs.

# Neovim mode

A Neovim mode is provided in separate repository [texpresso.vim](https://github.com/let-def/texpresso.vim). It is not yet compatible with vanilla Vim, patches are welcome :bow:.​

# Navigating TeXpresso window

Keyboard controls: 
- `←`, `→`: change page
- `p` (for "page"): switch between "fit-to-page" and "fit-to-width" zoom modes
- `c` ("crop"): crop borders
- `q` ("quit"): quit
- `i` ("invert"): dark mode
- `I` : toggle theming
- `t` ("top"): toggle stay-on-top (keeping TeXpresso above the editor window)
- `b` ("border"): toggle window borders
- `F5`: start fullscreen presentation (leave with `ESC`)

Mouse controls:

- click: select text in window (TODO: move Emacs buffer with SyncTeX)
- control+click: pan page
- wheel: scroll page
- control+wheel: zoom

## Screencasts

**Neovim integration.**
Launching TeXpresso in vim:

https://github.com/let-def/texpresso.vim/assets/1048096/b6a1966a-52ca-4e2e-bf33-e83b6af851d8

Live update during edition:

https://github.com/let-def/texpresso.vim/assets/1048096/cfdff380-992f-4732-a1fa-f05584930610

Using Quickfix window to fix errors and warnings interactively:

https://github.com/let-def/texpresso.vim/assets/1048096/e07221a9-85b1-44f3-a904-b4f7d6bcdb9b

Synchronization from Document to Editor (SyncTeX backward):

https://github.com/let-def/texpresso.vim/assets/1048096/f69b1508-a069-4003-9578-662d9e790ff9

Synchronization from Editor to Document (SyncTeX forward):

https://github.com/let-def/texpresso.vim/assets/1048096/78560d20-391e-490e-ad76-c8cce1004ce5

Theming, Light/Dark modes: 😎

https://github.com/let-def/texpresso.vim/assets/1048096/a072181b-82d3-42df-9683-7285ed1b32fc

**Emacs integration.** Here is a sample recording of me editing and browsing @fabiensanglard [Game Engine Black Book: Doom](https://github.com/fabiensanglard/gebbdoom) in TeXpresso (using my emacs theme):

https://user-images.githubusercontent.com/1048096/235424858-a5a2900b-fb48-40b7-a167-d0b71af39034.mp4
