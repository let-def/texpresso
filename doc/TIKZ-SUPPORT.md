# TeXpresso TikZ Support — PR Summary

This PR adds TikZ/pgf rendering support to TeXpresso.  ~70 commits on branch `tikz` implement a PostScript interpreter and PDF content-stream handler that process the DVI specials emitted by PGF's drivers (dvips and dvipdfmx).

## What Works

### Path & Shape Rendering
`moveto`, `lineto`, `curveto`, `closepath`, `newpath`, rectangles (`pgfe`).  All basic TikZ paths render: lines, curves (bézier, parabola), circles, ellipses, rectangles, arcs, grids, plot coordinates.

### Fill & Stroke
`pgffill`, `pgfstr`, `pgfs`/`pgfS`, `pgfr`/`pgfR`, `pgfeofill`.  `\fill`, `\draw`, `\filldraw` all work.  Stroke keeps the path after fill so `\filldraw` can issue both.

### Color
`setrgbcolor`, `setgray`, `setcmykcolor`, plus PGF's `pgffc`/`pgfsc` color-function scheme.  Gray, RGB, and CMYK color spaces.  Named xcolor colours resolve correctly.

### Graphics State
`gsave`/`grestore` (`q`/`Q`), line width (`pgfw`, `w`), line join, line cap, miter limit.  Dash patterns including long arrays (>4 elements) via `setdash`.

### Transforms
PS `concat` (including `cm` in PDF content streams), `scale`, `translate`, `rotate`, `currentpoint`.  CTM is built on the TeX page-position so that PGF coordinate arithmetic is correct.  `\resizebox` and `\scalebox` from graphicx are supported.

### Clipping
`W`/`W*` and PS `clip`/`eoclip`.  Stacks correctly with `gsave`/`grestore`.

### Text (Basic)
DVI text operators render node labels.  Standard PDF 14 fonts are resolved lazily.  The TRM matrix is concatenated correctly so that scoped/transformed node text positions are accurate.

### Shadings
Axial and radial gradients (`\shade`).  RGB, CMYK, and gray colour models.  Rendered natively via MuPDF's `fz_fill_shade`.  FunctionType 3 stitching functions are evaluated analytically.  Shadings with alpha < 1 use a transparency group.

### Transparency
`fill opacity` and `draw opacity`.  Transparency groups (`transparency group` in `\begin{scope}`) render correctly for overlapping semi-transparent objects.

### Decorations
`zigzag`, `snake`, `brace`, `text along path`.  Markings with arrow tips at arbitrary positions.

### Node Positioning
`above`, `below`, `left`, `right` with offsets.  Named anchors (`.north`, `.south`, `.east`, `.west`, `.north east`, etc.).  `fit` library.

### Loops & Maths
`\foreach` with `evaluate`, `count`, `remember`.  `calc` library: `($(A)!0.5!(B)$)`, `($(A)+(1,2)$)`, `($(A)!(C)!(B)$)`.

### Matrix (Manual)
A grid of named nodes replaces the `\matrix` command (which requires `&` that conflicts with TeXpresso's frontend).  Equivalent output.

### Patterns (Partial)
**Colour extraction works** — pattern-filled shapes show the correct colour.  **Texture tiling is not implemented.**  See *Known Issues* below.

## Architecture

### Two Special Formats
PGF emits content via two drivers, both are handled:

1. **dvips** — `ps:` and `ps::` DVI specials.  Parsed by `ps_code()` in `dvi_special.c`.  Tokens are interpreted by a simple stack machine.

2. **dvipdfmx** — `pdf:` DVI specials.  Content streams (`pdf:code` / `pdf:literal`) are parsed by `pdf_code()` which uses a vstack-based PDF operator interpreter.  Graphics-state specials (`pdf:btrans`/`pdf:etrans`, `pdf:bc`/`pdf:ec`, `pdf:q`/`pdf:Q`) are handled directly.

### PS Function Table
PGF defines ~96 internal functions via `!` specials.  TeXpresso stores the 16 most recent in `ps_funcs[]` (PS_FUNC_MAX=16).  This limit is a careful trade-off: expanding it lets internal functions leak into the token-dispatch fallback path and corrupt rendering state.

### Shading Pipeline
`try_parse_ps_shading()` intercepts `<<` dictionaries inside `ps:` specials.  When it detects a valid shading dictionary (`/ShadingType`, `/Coords`, `/Function`), it parses the parameters and calls `render_axial_shade()` or `render_radial_shade()`, which use MuPDF's `fz_fill_shade` to draw the gradient directly onto the device.

### Colour Management
PGF sets colours by defining a function (`/pgffc{...}def`) and later looking it up (`ps_lookup_func("pgffc")`).  TeXpresso executes colour-function bodies at definition time so that inline `setrgbcolor`/`setgray`/`setcmykcolor` commands update `st->gs.colors.fill` and `st->gs.colors.line` immediately.  This avoids a round-trip through the PS interpreter when the fill/stroke path is drawn.

## Known Issues

### Pattern Textures (Phase 13)
Pattern fills show the correct colour but no tiled texture.  PGF emits patterns as PostScript `makepattern`/`setcolor` operators (dvips driver) or as `pdf:stream`/`pdf:put` specials (dvipdfmx driver).  Neither path is handled by the current PS interpreter or PDF special handler.  The dvips path requires implementing PatternType 1 tiling with per-tile PaintProc execution and clip management; the dvipdfmx path requires adding `stream`/`obj`/`put` rules in the PDF special dispatcher and modifying the `scn` operator to accept 4-operand pattern colours.  Both approaches were prototyped but introduced crashes or incorrect output.

### Single-Page Hang with Direct XDV Rendering
When invoked as `texpresso file.xdv`, only the first page renders before the process hangs.  The VSCode extension pipeline does not have this issue.  Likely a teardown-path bug in the standalone DVI engine, not related to TikZ support.
