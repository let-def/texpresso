/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Per-engine contract for the in-process wasm backend.
 *
 * engine_tex.c is the engine-agnostic driver (VFS seam, coroutine snapshot,
 * replay). Everything that differs between TeX engines is captured in a
 * tex_engine_profile, one per engine in engine_tex_<name>.c. Adding an engine
 * is a new profile + linking that engine's wasm2c object; the driver is unchanged.
 */

#ifndef ENGINE_TEX_H
#define ENGINE_TEX_H

#include <stdbool.h>
#include "engine.h"

typedef struct tex_engine_profile
{
  const char *name;          /* engine name, matched against the engine path */
  const char *format;        /* LaTeX format for -fmt= (e.g. "xelatex"); NULL => -ini */
  const char *extra_argv[3]; /* engine flags before the doc, NULL-terminated */
  bool needs_icu;            /* point ICU_DATA at the format dir (xetex) */
} tex_engine_profile;

/* Generic constructor: drive a wasm2c engine described by `prof`. */
txp_engine *txp_tex_engine_create(fz_context *ctx,
                                   const tex_engine_profile *prof,
                                   const char *engine_path,
                                   const char *tex_name, dvi_reshooks hooks);

/* One profile per engine (engine_tex_<name>.c). */
extern const tex_engine_profile txp_tex_profile_xetex;
extern const tex_engine_profile txp_tex_profile_pdftex;
extern const tex_engine_profile txp_tex_profile_luatex;

#endif /* ENGINE_TEX_H */
