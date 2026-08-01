/* xetex profile for the in-process wasm backend. Validated. */
#include "engine_tex.h"

const tex_engine_profile txp_tex_profile_xetex = {
    .name = "xetex",
    .format = "xelatex",
    .extra_argv = {"-no-pdf", NULL}, /* emit XDV for incdvi, not PDF */
    .needs_icu = true,               /* ICU data (icudt*.dat) in the format dir */
};
