/* luatex profile for the in-process wasm backend. Validated: renders, replays
 * byte-identically after edits, and converges TOC/refs on rerun (luatex needs
 * one more pass than xetex/pdftex to stabilise the aux).
 * DVI output is requested so incdvi can parse it. */
#include "engine_tex.h"

const tex_engine_profile txp_tex_profile_luatex = {
    .name = "luatex",
    .format = "lualatex",
    .extra_argv = {"--output-format=dvi", NULL}, /* DVI (not PDF) for incdvi */
    .needs_icu = false,
};
