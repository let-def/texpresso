/* luatex profile for the in-process wasm backend.
 * NOTE: not yet validated through the full txp_engine path (no lualatex.fmt is
 * built on this branch). DVI output is requested so incdvi can parse it. */
#include "engine_tex.h"

const tex_engine_profile txp_tex_profile_luatex = {
    .name = "luatex",
    .format = "lualatex",
    .extra_argv = {"--output-format=dvi", NULL}, /* DVI (not PDF) for incdvi */
    .needs_icu = false,
};
