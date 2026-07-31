/* xetex profile for the in-process wasm backend. Validated. */
#include "engine_wasm.h"

const wasm_engine_profile txp_wasm_profile_xetex = {
    .name = "xetex",
    .format = "xelatex",
    .extra_argv = {"-no-pdf", NULL}, /* emit XDV for incdvi, not PDF */
    .needs_icu = true,               /* ICU data (icudt*.dat) in the format dir */
};
