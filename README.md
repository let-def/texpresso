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

For Arch Linux users, [a PKGBUILD is available in the AUR](https://aur.archlinux.org/packages/texpresso-git) that builds from the latest Git HEAD on installation.

Otherwise, read below.

## Build TeXpresso

First make sure the dependencies are available: `pkg-config`, `re2c`, `SDL2`, `mupdf` (and its own dependencies: `libjpeg`, `libpng`, `freetype2`, `gumbo`, `jbig2dec`... and possibly `leptonica` and `tesseract` depending on the mupdf version).
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

Then make sure that the git submodules were initialized:

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
