/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef MYDVI_H
#define MYDVI_H

#include <stdint.h>
#include <stdbool.h>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include "fixed.h"
#include "../mupdf_compat.h"

// Forward declaration of resource manager
typedef struct dvi_resmanager dvi_resmanager;

/********************************/
/* Definition of TeX structures */
/********************************/

// TeX Font Metrics

typedef struct tex_tfm tex_tfm;

fixed_t tex_tfm_char_width(const tex_tfm *tfm, int c);
fixed_t tex_tfm_char_height(const tex_tfm *tfm, int c);
fixed_t tex_tfm_char_depth(const tex_tfm *tfm, int c);
fixed_t tex_tfm_italic_corr(const tex_tfm *tfm, int c);
fixed_t tex_tfm_design_size(const tex_tfm *tfm);
fixed_t tex_tfm_space(const tex_tfm *tfm);
fixed_t tex_tfm_space_stretch(const tex_tfm *tfm);
fixed_t tex_tfm_space_shrink(const tex_tfm *tfm);
fixed_t tex_tfm_quad(const tex_tfm *tfm);
fixed_t tex_tfm_ascent(const tex_tfm *tfm);
fixed_t tex_tfm_descent(const tex_tfm *tfm);
uint32_t tex_tfm_checksum(const tex_tfm *tfm);
uint16_t tex_tfm_first_char(const tex_tfm *tfm);
uint16_t tex_tfm_last_char(const tex_tfm *tfm);

tex_tfm *tex_tfm_load(fz_context *ctx, fz_stream *stm);
void tex_tfm_free(fz_context *ctx, tex_tfm *tfm);

// TeX Virtual Font Character

typedef struct
{
  const uint8_t *dvi;
  uint32_t dvi_length;
  fixed_t width;
} tex_vf_char;

// TeX Virtual Font

typedef struct tex_vf tex_vf;

tex_vf *tex_vf_load(fz_context *ctx, dvi_resmanager *manager, fz_stream *stm);
void tex_vf_free(tex_vf *vf);
tex_vf_char *tex_vf_get(tex_vf *vf, int code);
int tex_vf_default_font(tex_vf *vf);

// TeX FontMap Entry

typedef struct
{
  unsigned long hash;
  const char *pk_font_name;
  const char *ps_font_name;
  const char *ps_snippet;
  const char *enc_file_name;
  const char *font_file_name;
} tex_fontmap_entry;

// TeX FontMap

typedef struct tex_fontmap tex_fontmap;

tex_fontmap *tex_fontmap_load(fz_context *ctx, fz_stream **stm, int count);
void tex_fontmap_free(fz_context *ctx, tex_fontmap *fm);
tex_fontmap_entry *tex_fontmap_lookup(tex_fontmap *fm, const char *name);
tex_fontmap_entry *tex_fontmap_iter(tex_fontmap *fm, unsigned *index);

// TeX Encoding

typedef struct tex_enc tex_enc;

tex_enc *tex_enc_load(fz_context *ctx, fz_stream *stream);
void tex_enc_free(fz_context *ctx, tex_enc *fm);
const char *tex_enc_get(tex_enc *fm, uint8_t code);

/***************************/
/* Definition of DVI fonts */
/***************************/

// Runtime representation of a font

typedef struct dvi_font dvi_font;
struct dvi_font
{
  const char *name;
  tex_tfm *tfm;
  tex_enc *enc;
  tex_vf  *vf;
  int *glyph_map;
  fz_font *fz;
};

// Definition of an extended font from a XDV file

typedef struct
{
  fixed_t size;
  uint16_t flags;
  uint32_t rgba;
  int32_t extend;
  int32_t slant;
  int32_t bold;
} dvi_xdvfontspec;

typedef struct
{
  int32_t checksum;
  fixed_t scale_factor;
  fixed_t design_size;
} dvi_fontspec;

// Definition of a font from a DVI file

typedef enum {
  TEX_FONT,
  XDV_FONT
} dvi_fontkind;

typedef struct
{
  dvi_fontkind kind;
  union {
    struct {
      dvi_font *font;
      dvi_fontspec spec;
    } tex_font;
    struct {
      fz_font *font;
      dvi_xdvfontspec spec;
    } xdv_font;
  };
} dvi_fontdef;

// Font Table, local to a DVI or VF file, to index font definitions

typedef struct dvi_fonttable dvi_fonttable;

dvi_fonttable *dvi_fonttable_new(fz_context *ctx);
void dvi_fonttable_free(fz_context *ctx, dvi_fonttable *table);
dvi_fontdef *dvi_fonttable_get(fz_context *ctx, dvi_fonttable *table, int index);

dvi_fonttable *tex_vf_fonttable(tex_vf *vf);

/********************/
/* Resource manager */
/********************/

// Loading and caching:
// - font metrics (TFM)
// - fonts (VF, TTF, OTF)
// - encodings
// - graphics (PDFs & images)

typedef enum {
  RES_PDF,
  RES_ENC,
  RES_MAP,
  RES_TFM,
  RES_VF,
  RES_FONT, /*TTF, OTF or PFB?*/
} dvi_reskind;

typedef struct {
  void *env;
  fz_stream *(*open_file)(fz_context *ctx, void *env, dvi_reskind kind, const char *name);
  void (*free_env)(fz_context *ctx, void *env);
} dvi_reshooks;

dvi_reshooks dvi_tectonic_hooks(fz_context *ctx, const char *document_directory);

typedef struct bundle_server bundle_server;

bundle_server *
bundle_server_start(fz_context *ctx,
                    const char *tectonic_path,
                    const char *document_directory);

int bundle_server_input(bundle_server *server);
int bundle_server_output(bundle_server *server);
int bundle_server_lock(bundle_server *server);
dvi_reshooks bundle_server_hooks(bundle_server *server);

void dvi_free_hooks(fz_context *ctx, const dvi_reshooks *hooks);

dvi_resmanager *dvi_resmanager_new(fz_context *ctx, dvi_reshooks hooks);
void dvi_resmanager_free(fz_context *ctx, dvi_resmanager *rm);
dvi_font *dvi_resmanager_get_tex_font(fz_context *ctx, dvi_resmanager *rm, const char *name, int namelen);
fz_font *dvi_resmanager_get_xdv_font(fz_context *ctx, dvi_resmanager *rm, const char *name, int namelen, int index);
pdf_document *dvi_resmanager_get_pdf(fz_context *ctx, dvi_resmanager *rm, const char *filename);
fz_image *dvi_resmanager_get_img(fz_context *ctx, dvi_resmanager *rm, const char *filename);
void dvi_resmanager_invalidate(fz_context *ctx, dvi_resmanager *rm, dvi_reskind kind, const char *name);

/****************************************/
/* Definition of DVI runtime structures */
/****************************************/

// DVI file format versions (+ VF fonts)

enum dvi_version
{
  DVI_NONE     = 0,
  DVI_STANDARD = 2,
  DVI_PTEX     = 3,
  DVI_XDV5     = 5,
  DVI_XDV6     = 6,
  DVI_XDV7     = 7,
  DVI_VF       = 202,
};

// Registers of the DVI VM

typedef struct
{
  int32_t h, v, w, x, y, z;
} dvi_registers;

// A simple arena allocator from page-scoped allocation

typedef struct
{
  struct dvi_scratch_buf *buf;
} dvi_scratch;

void dvi_scratch_init(dvi_scratch *sc);
void dvi_scratch_release(fz_context *ctx, dvi_scratch *sc);
void *dvi_scratch_alloc(fz_context *ctx, dvi_scratch *sc, size_t len);
void dvi_scratch_clear(fz_context *ctx, dvi_scratch *sc);

// The graphic state.
// Mostly based on PDF graphic state, with extensions to handle DVI/pdfTeX
// specials.

typedef struct
{
  float line[3], fill[3];
} dvi_colorstate;

typedef enum
{
  PDF_MITERED_JOIN = 0,
  PDF_ROUNDED_JOIN = 1,
  PDF_BEVEL_JOIN = 2,
} pdf_line_join;

typedef enum
{
  PDF_BUTT_CAPS = 0,
  PDF_ROUND_CAPS = 1,
  PDF_SQUARE_CAPS = 2,
} pdf_line_caps;

typedef struct
{
  fz_matrix ctm;
  dvi_colorstate colors;
  float line_width, miter_limit;
  pdf_line_join line_join;
  pdf_line_caps line_caps;
  int clip_depth;
  float dash[4];
  int dash_len;
  float dash_phase;
  int h, v;
} dvi_graphicstate;

typedef struct
{
  dvi_colorstate origin;
  dvi_colorstate *base;
  int depth, limit;
} dvi_colorstack;

typedef struct
{
  dvi_colorstack *stacks;
  int capacity;
} dvi_colorstacks;

// State of a DVI interpreter

typedef struct
{
  enum dvi_version version;
  uint32_t f;
  dvi_graphicstate gs;
  dvi_registers registers;
  struct {
    dvi_registers *base;
    int depth, limit;
  } registers_stack;
  struct {
    dvi_graphicstate *base;
    int depth, limit;
  } gs_stack;
  dvi_fonttable *fonts;
} dvi_state;

// Shared data common to DVI interpreter and renderer
typedef struct
{
  fz_device *dev;
  fz_text *text;
  fz_path *path;
  dvi_scratch scratch;
  dvi_resmanager *resmanager;
  dvi_state root;
  dvi_registers registers_stack[256];
  dvi_graphicstate gs_stack[256];

  // Default color stack (used by dvipdfmx)
  dvi_colorstack colorstack;
  // Pdf color stacks (introduced by pdftex)
  dvi_colorstacks pdfcolorstacks;
  float scale;
} dvi_context;

#define DC_ALLOC(ctx, dc, type, count) ((type*)dvi_scratch_alloc(ctx, &(dc)->scratch, sizeof(type) * (count)))

dvi_context *dvi_context_new(fz_context *ctx, dvi_reshooks hooks);
void dvi_context_free(fz_context *ctx, dvi_context *dc);
dvi_state *dvi_context_state(dvi_context *dc);
bool dvi_state_enter_vf(dvi_context *dc, dvi_state *vfst, const dvi_state *st, dvi_fonttable *fonts, int font, fixed_t scale);
void dvi_context_flush_text(fz_context *ctx, dvi_context *dc, dvi_state *st);
void dvi_context_begin_frame(fz_context *ctx, dvi_context *dc, fz_device *dev);
void dvi_context_end_frame(fz_context *ctx, dvi_context *dc);

#define inlined static inline __attribute__((unused))

inlined fz_matrix dvi_get_ctm(const dvi_context *dc, const dvi_state *st)
{
  float s = dc->scale;
  int32_t h = st->registers.h - st->gs.h;
  int32_t v = st->registers.v - st->gs.v;
  return fz_pre_translate(st->gs.ctm, h * s, - v * s);
}

inlined void dvi_set_ctm(dvi_state *st, fz_matrix ctm)
{
  st->gs.ctm = ctm;
  st->gs.h = st->registers.h;
  st->gs.v = st->registers.v;
}

#undef inlined

#endif /*!MYDVI_H*/
