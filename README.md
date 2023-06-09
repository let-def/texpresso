# TeXpresso: live rendering and error reporting for LaTeX

_Note: this is an experimental tool._

**Important: this repository uses submodules. Clone using `git clone --recurse-submodules`.**

TeXpresso has been tested on Linux and macOS and should work with both AMD64 and Apple Silicon architectures.
Here is a sample recording of me editing and browsing @fabiensanglard [Game Engine Black Book: Doom](https://github.com/fabiensanglard/gebbdoom) in TeXpresso (using my emacs theme):

https://user-images.githubusercontent.com/1048096/235424858-a5a2900b-fb48-40b7-a167-d0b71af39034.mp4


It is made of two components:
- this repository which implements the `texpresso` binary
- the [tectonic/](tectonic/) git-submodule which implements a patched version of [Tectonic](https://github.com/tectonic-typesetting/tectonic) that produces the `texpresso-tonic` helper binary

At the moment, it requires a functional installation of [Tectonic](https://github.com/tectonic-typesetting/tectonic). Install it first and make it compile simple TeX file to generate initial LaTeX format. For instance, run "tectonic test/simple.tex" at the root of this repository.

# Building

If all dependencies are installed and out-of-the-box configuration works, `make all` should be sufficient to build both `texpresso` and `texpresso-tonic` in `build/` directory.

For Arch Linux users, [a PKGBUILD is available in the AUR](https://aur.archlinux.org/packages/texpresso-git) that builds from the latest Git HEAD on installation.

Otherwise, read below.

## Build TeXpresso

First make sure the dependencies are available: `pkg-config`, `re2c`, `SDL2`, `mupdf` (and its own dependencies: `libjpeg`, `libpng`, `freetype2`, `gumbo`, ...).
Under macOS, `brew` is also used to find local files.

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

# Navigating TeXpresso window

Keyboard controls: 
- `←`, `→`: change page
- `p` (for "page"): switch between "fit-to-page" and "fit-to-width" zoom modes
- `c` ("crop"): crop borders
- `q` ("quit"): quit
- `i` ("invert"): dark mode
- `I` : use Emacs theme
- `t` ("top"): toggle stay-on-top (above Emacs window)
- `b` ("border"): toggle window borders
- `F5`: start fullscreen presentation (leave with `ESC`)

Mouse controls:

- click: select text in window (TODO: move Emacs buffer with SyncTeX)
- control+click: pan page
- wheel: scroll page
- control+wheel: zoom
