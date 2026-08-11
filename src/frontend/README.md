# TeXpresso frontend

Build with `make texpresso` from the repo root. The result is a single
standalone binary; the TeX engine is linked into it (see `WASM-ENGINE.md`), so
there is no second process to launch.

## Important source files

### Entrypoints

[driver.c](driver.c) implements the entrypoint (the `main` function).
[driver.h](driver.h) defines the interface it expects. `main` initializes the
shared state and resources (mupdf and SDL2) and passes control to
`texpresso_main`.

[main.c](main.c) implements the main TeXpresso interface.

### Engine

[engine.h](engine.h) defines the common interface between engines. An engine
takes the input file given to `texpresso` and turns it into something that can
be displayed:

- [engine_dvi.c](engine_dvi.c) renders DVI and XDV files.
- [engine_pdf.c](engine_pdf.c) renders a PDF file (using MuPDF).
- [engine_tex.c](engine_tex.c) renders a .tex file by running a TeX engine
  in-process and turning its output into an XDV stream. It owns the checkpoint
  stack and incremental replay, and answers the engine's file syscalls from the
  VFS. It is engine-agnostic.

[engine_tex_xetex.c](engine_tex_xetex.c),
[engine_tex_pdftex.c](engine_tex_pdftex.c),
[engine_tex_luatex.c](engine_tex_luatex.c) are the per-engine profiles: program
name, format, and the arguments each engine needs. One is compiled in, selected
by `TEXPRESSO_ENGINE` at build time.

The host that implements the wasm imports and the engine lifecycle lives outside
this directory, in [../engine-wasm/wasm_host.c](../engine-wasm/wasm_host.c).

[dvi/](dvi/) is a generic interpreter for the DVI format, with support for TeX
TFM, VF, enc, PDF graphic streams, etc.

[fs.c](fs.c) fakes a minimalist filesystem. It keeps an in-memory copy of files
read from the real filesystem by LaTeX (to detect changes or to patch them) and
stores files written by LaTeX.

[state.c](state.c), [state.h](state.h) track the state of the running engine
(somewhat like the unix "U structure", keeping the list of opened file
descriptors) while supporting backtracking. This is what `engine_tex.c` rolls
back when it restores a checkpoint.

[incdvi.c](incdvi.c), [incdvi.h](incdvi.h) is an incremental viewer for DVI
files, implemented on top of the <dvi/> library.

[renderer.c](renderer.c), [renderer.h](renderer.h) renders the contents of the
TeXpresso window, with support for scrolling, cropping, remapping colors, etc.

### Misc files

[sexp_parser.c](sexp_parser.c), [sexp_parser.h](sexp_parser.h) is a simple
S-expression parser, compatible enough with Emacs.

[synctex.c](synctex.c), [synctex.h](synctex.h) is a quick'n'dirty SyncTeX
parser, rolled back alongside the rest of the state on a replay.

[myabort.c](myabort.c), [myabort.h](myabort.h) is a helper to print backtraces
before aborting.

[proxy.c](proxy.c) is a small C tool (compiled using `make
texpresso-debug-proxy`) to proxy TeXpresso communication from the editor to an
instance running through a debugger (launched using
<../../scripts/texpresso-debug>).

[logo.c](logo.c), [logo.h](logo.h) is the TeXpresso logo, represented as a
[qoi.h](qoi.h) image and serialized as a C string.
