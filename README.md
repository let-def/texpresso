<br />
<div align="center">
  <a href="https://github.com/let-def/texpresso">
    <img width=256 height=256 src="doc/texpresso_logo_v3.svg", alt="texpresso">
  </a>
  <h1>
    <strong>
      TeXpresso: live rendering and error reporting for LaTeX
    </strong>
  </h1>
</div>

> [!Note]
> TeXpresso is still in an early development phase.
> Changes and bug fixes are happening frequently, check the [CHANGELOG.md](CHANGELOG.md).

[![CI C Build & Test](https://github.com/merv1n34k/texpresso/actions/workflows/ci.yml/badge.svg)](https://github.com/merv1n34k/texpresso/actions/workflows/ci.yml)

## About

TeXpresso provides a *"live rendering"* experience when editing LaTeX documents in a supported editor: change something in the `.tex` file, the render window will update almost immediately with your change. Write something invalid, you get an error message immediately.

This can radically improve the LaTeX editing experience compared to the usual rebuild-and-wait-for-viewer-to-update experience, especially for large documents.

See the [screencasts](#Screencasts) at the end of this file for a visual demo of TeXpresso capabilities.

### Install

TeXpresso has been tested on Linux and macOS and should work with both AMD64 and Apple Silicon architectures. See [INSTALL.md](./INSTALL.md) for dependency and build instructions.

### Design

The TeXpresso system is built of the following parts:

1. A TeX engine that renders LaTeX documents into PDF;
   we use a modified version of the `XeTeX` engine, modified to interact with the TeXpresso driver.

2. A PDF renderer that renders PDF documents into images.
   We use [MuPDF](https://mupdf.com/).

3. A viewer that shows the rendered images and allows simple user commands (see [Viewer controls](#Viewer_controls) below), built with [libSDL](https://www.libsdl.org/).

4. A driver program that talks to the editor to be notified of changes to the LaTeX document, maintains an incremental view of the document and the rendering process (supporting incrementality, rollback, error recovery, etc.), talks to the LaTeX engine to re-render the modified portions of the document, and synchronizes with the viewer.

   The driver is where the *"live"* magic lives. It is the `texpresso` binary, whose sources are in this repository.

The driver sends information between the editor and the renderer in both directions. In particular, it is possible to ask the editor to jump to a specific place in the LaTeX document by clicking on the viewer window or, conversely, to refresh the viewer window to display the document at the editor position.

## Viewer controls

Keyboard controls:
- `←`, `→`: change page
- `↑`, `↓`: move within the page
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

## Supported editors

### Emacs

TeXpresso comes with an Emacs mode. The source can be found in
[emacs/texpresso.el](emacs/texpresso.el).  Load this file in Emacs (using `M-X load-file`; it is also compatible with `require`).

Start TeXpresso with `M-x texpresso`. The prompt will let you select the master/root TeX file.
It will try to start the `texpresso` command. If it is not possible, it will open
`(customize-variable 'texpresso-binary)` to let you set the path to texpresso
binary (`<where you cloned the repository>/build/texpresso`).

To work correctly, `texpresso` needs `texpresso-xetex` helper; when copying them, make sure they are both in the same directory.

`M-x texpresso-display-output` will open a small window listing TeX warnings and errors on the current page.
Use `M-x texpresso-next-page` and `M-x texpresso-previous-page` to move between pages without leaving Emacs.

### Neovim

A Neovim mode is provided in a separate repository [texpresso.vim](https://github.com/let-def/texpresso.vim). It is not yet compatible with vanilla Vim, patches are welcome :bow:.

### Visual Studio Code

A vscode mode is being developed in [texpresso-vscode](https://github.com/DominikPeters/texpresso-vscode), thanks to @DominikPeters.
Look for [TeXpresso](https://marketplace.visualstudio.com/items?itemName=DominikPeters.texpresso-basic) in the marketplace.

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

## Credits

Thanks to [@DominikPeters](https://github.com/DominikPeters) for contributing the VS Code extension.
Thanks to [@merv1n34k](https://github.com/merv1n34k) for contributing the logo.

Thanks our many contributors, [@gasche](https://github.com/gasche), [@haselwarter](https://github.com/haselwarter), [@alerque](https://github.com/alerque), [@sandersantema](https://github.com/sandersantema), [@t4ccer](https://github.com/t4ccer), [@schnell18](https://github.com/schnell18), and [@bowentan](https://github.com/bowentan) for improving features, portability and compatibility with many platforms.
