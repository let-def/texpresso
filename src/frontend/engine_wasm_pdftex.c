/* pdftex profile for the in-process wasm backend.
 * NOTE: not yet validated through the full txp_engine path (no pdflatex.fmt is
 * built on this branch). DVI output is requested so incdvi can parse it. */
#include "engine_wasm.h"

const wasm_engine_profile txp_wasm_profile_pdftex = {
    .name = "pdftex",
    .format = "pdflatex",
    .extra_argv = {"-output-format=dvi", NULL}, /* DVI (not PDF) for incdvi */
    .needs_icu = false,
};
