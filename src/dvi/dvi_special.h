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

#ifndef DVI_SPECIAL_H
#define DVI_SPECIAL_H

// Private interface between the parts of the interpreter of specials:
// - dvi_special.re2c.c: parsing and dispatching of specials (re2c)
// - dvi_color.c: colors and color stacks
// - dvi_gstate.c: graphic state and transformations
// - dvi_xobject.c: object store, images and form XObjects
// - dvi_paint.c: painting, color spaces, patterns and graphic state parameters
// - dvi_pdfcode.c: interpretation of PDF content streams

#include <math.h>
#include <string.h>
#include <mupdf/pdf.h>
#include "mydvi.h"

#define device_cs fz_device_rgb
#define color_params fz_default_color_params

typedef const char *cursor_t;

static inline bool
unhandled(const char *kind, cursor_t cur, cursor_t lim, int ignored)
{
  if (0 && !ignored)
    fprintf(stderr, "unhandled %s: \"%.*s\"\n", kind, (int)(lim - cur), cur);
  return 0;
}

/*********************/
/* Parsing utilities */
/*********************/

static inline int pnat(cursor_t ptr, cursor_t lim)
{
  int result = 0;
  for (; ptr < lim && *ptr >= '0' && *ptr <= '9'; ptr += 1)
    result = result * 10 + (*ptr - '0');
  return result;
}

static inline int pint(cursor_t ptr, cursor_t lim)
{
  if (ptr < lim && *ptr == '-')
    return -pnat(ptr + 1, lim);
  else
    return pnat(ptr, lim);
}

// Conversion factor from a unit to TeX points
static inline double
punit(cursor_t cur, cursor_t lim)
{
  while (cur < lim && (*cur == ' ' || *cur == '\t'))
    cur++;

  // "true" units ignore magnification, which is not supported anyway
  if (lim - cur >= 4 && memcmp(cur, "true", 4) == 0)
    cur += 4;

  if (lim - cur < 2)
    return 1.0;

  char c0 = cur[0], c1 = cur[1];

  if (c0 == 'p' && c1 == 't') return 1.0;
  if (c0 == 'b' && c1 == 'p') return 72.27 / 72.0;
  if (c0 == 'm' && c1 == 'm') return 72.27 / 25.4;
  if (c0 == 'c' && c1 == 'm') return 72.27 / 2.54;
  if (c0 == 'i' && c1 == 'n') return 72.27;
  if (c0 == 'p' && c1 == 'c') return 12.0;
  if (c0 == 'd' && c1 == 'd') return 1238.0 / 1157.0;
  if (c0 == 'c' && c1 == 'c') return 12.0 * 1238.0 / 1157.0;
  if (c0 == 's' && c1 == 'p') return 1.0 / 65536.0;

  return 1.0;
}

static inline float
pfloat_or_dim(cursor_t ptr, cursor_t lim, bool is_dim)
{
  int neg = 0;
  double result = 0;
  if (ptr < lim && (*ptr == '-' || *ptr == '+'))
  {
    neg = (*ptr == '-');
    ptr += 1;
  }
  for (; ptr < lim && *ptr >= '0' && *ptr <= '9'; ptr += 1)
    result = result * 10 + (*ptr - '0');
  if (ptr < lim  && *ptr == '.')
  {
    ptr += 1;
    double pos = 0.1;
    for (; ptr < lim && *ptr >= '0' && *ptr <= '9'; ptr += 1)
    {
      result += pos * (*ptr - '0');
      pos /= 10;
    }
  }

  // Dimensions are converted to PDF units (big points)
  if (is_dim)
    result *= punit(ptr, lim) * 72.0 / 72.27;
  return neg ? -result : result;
}

static inline float pfloat(cursor_t ptr, cursor_t lim)
{
  return pfloat_or_dim(ptr, lim, 0);
}

static inline float pdim(cursor_t ptr, cursor_t lim)
{
  return pfloat_or_dim(ptr, lim, 1);
}

static inline cursor_t skip_ws(cursor_t cur, cursor_t lim)
{
  while (cur < lim && (*cur == ' ' || *cur == '\t' || *cur == '\r' || *cur == '\n'))
    cur++;
  return cur;
}

/***********************************/
/* Transformations and dimensions  */
/***********************************/

enum pagebox
{
  BOX_UNDEFINED,
  BOX_MEDIABOX,
  BOX_CROPBOX,
  BOX_ARTBOX,
  BOX_BLEEDBOX,
  BOX_TRIMBOX,
};

struct xform_spec
{
  fz_matrix ctm;
  float width, height, depth;
  fz_rect bbox;
  int page, clip;
  bool hide;
  enum pagebox pagebox;
};

static inline struct xform_spec xform_spec(void)
{
  return (struct xform_spec){
      .ctm = fz_identity,
      .width = NAN,
      .height = NAN,
      .depth = NAN,
      .bbox = fz_infinite_rect,
      .page = -1,
      .clip = 0,
      .hide = 0,
      .pagebox = BOX_UNDEFINED,
  };
}

enum xobject_mode
{
  XOBJ_DRAW,
  // Draw the content of a soft mask (alpha is applied by the mask)
  XOBJ_MASK,
};

// The path being constructed is saved when interpreting a sub-content
// (form, pattern cell, soft mask), which can be painted in the middle of the
// construction of a path.
typedef struct
{
  fz_path *path;
  int path_clip;
} saved_path;

// Color stack specials
enum colorstack_op
{
  // Main stack ("color ...", "pdf:bcolor", ...)
  COLOR_PUSH,
  COLOR_POP,
  COLOR_SET,
  PDF_BCOLOR,
  PDF_SCOLOR,
  // pdfcolorstack
  PDFCOLORSTACK_PUSH,
  PDFCOLORSTACK_POP,
  PDFCOLORSTACK_SET,
  PDFCOLORSTACK_CURRENT,
  PDFCOLORSTACK_INIT,
};

enum paint_op
{
  PAINT_NONE = 0,
  PAINT_CLOSE = 1,
  PAINT_FILL = 2,
  PAINT_EVEN_ODD = 4,
  PAINT_STROKE = 8,
};

/* PROTOTYPES */

// dvi_special.re2c.c
cursor_t parse_colorspec(fz_context *ctx, cursor_t cur, cursor_t lim, float rgb[3], uint8_t *space);
cursor_t parse_name_ref(fz_context *ctx, dvi_context *dc, cursor_t cur, cursor_t lim, int *num, bool create);
cursor_t parse_xform_or_dim(struct xform_spec *xf, cursor_t cur, cursor_t lim);

// dvi_color.c
void color_set_cmyk(fz_context *ctx, float color[3], float c, float m, float y, float k);
bool color_set_components(fz_context *ctx, float color[3], const float *v, int n);
void color_set_gray(float color[3], float gray);
void color_set_hsb(float color[3], float h, float s, float b);
void color_set_rgb(float color[3], float r, float g, float b);
uint8_t colorspace_of_components(int n);
cursor_t colorspec_numbers(fz_context *ctx, cursor_t cur, cursor_t lim, float rgb[3], uint8_t *space, cursor_t f0, cursor_t f1, cursor_t f2, cursor_t f3);
void colorstate_set_fill(dvi_colorstate *cs, uint8_t space, const float rgb[3]);
void colorstate_set_line(dvi_colorstate *cs, uint8_t space, const float rgb[3]);
bool parse_color(fz_context *ctx, dvi_colorstate *state, cursor_t cur, cursor_t lim);
bool parse_pdfcolor(fz_context *ctx, dvi_colorstate *state, cursor_t cur, cursor_t lim);
cursor_t pdfstring_end(cursor_t cur, cursor_t lim);
const dvi_colorstate *colorstack_top(const dvi_colorstack *stack);

bool special_colorstack(fz_context *ctx, dvi_colorstacks *cs, dvi_colorstate *current,
                        enum colorstack_op op, int index, bool page,
                        cursor_t cur, cursor_t lim);

// dvi_gstate.c
void concat_around_current_point(dvi_context *dc, dvi_state *st, fz_matrix m);
void enter_substate(dvi_state *sub, const dvi_state *st, fz_matrix ctm, pdf_obj *resources);
bool gs_pop(fz_context *ctx, dvi_context *dc, dvi_state *st);
bool gs_push(dvi_state *st);
bool special_bcontent(fz_context *ctx, dvi_context *dc, dvi_state *st);
bool special_btrans(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim);
bool special_content(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim);
bool special_econtent(fz_context *ctx, dvi_context *dc, dvi_state *st);
bool special_literal(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim);
void pop_clips(fz_context *ctx, dvi_context *dc, int from, int to);
bool xetex_scale(dvi_context *dc, dvi_state *st, float sx, float sy);
bool xetex_bscale(dvi_context *dc, dvi_state *st, float sx, float sy);
bool xetex_escale(dvi_context *dc, dvi_state *st);
bool xetex_rotate(dvi_context *dc, dvi_state *st, float angle);

// dvi_xobject.c
void draw_form(fz_context *ctx, dvi_context *dc, dvi_state *st, pdf_obj *form, fz_matrix ctm, enum xobject_mode mode);
void draw_xobject(fz_context *ctx, dvi_context *dc, dvi_state *st, pdf_obj *xobj, fz_matrix ctm, struct xform_spec *xf);
bool form_begin(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim, bool render);
bool form_end(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t spc, cursor_t cur, cursor_t lim);
pdf_obj *lookup_resource(fz_context *ctx, dvi_context *dc, dvi_state *st, pdf_obj *category, const char *name);
void restore_path(fz_context *ctx, dvi_context *dc, saved_path saved);
void run_pdf_form(fz_context *ctx, dvi_context *dc, dvi_state *sub, pdf_obj *form);
saved_path save_path(dvi_context *dc);
bool special_image(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim, bool render);
void special_obj(fz_context *ctx, dvi_context *dc, cursor_t cur, cursor_t lim);
void special_put(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim);
void special_stream(fz_context *ctx, dvi_context *dc, cursor_t cur, cursor_t lim);
bool special_uxobj(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim);
pdf_document *store_doc(dvi_context *dc);

// dvi_paint.c
void apply_extgstate(fz_context *ctx, dvi_context *dc, dvi_state *st, const char *name);
void clip_rect(fz_context *ctx, dvi_context *dc, fz_rect r, fz_matrix ctm);
void colorspace_initial_color(fz_context *ctx, dvi_context *dc, uint8_t cs, float rgb[3]);
void colorspace_set_color(fz_context *ctx, dvi_context *dc, uint8_t cs, const float *v, int n, float rgb[3]);
bool fill_background(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim);
fz_path *get_path(fz_context *ctx, dvi_context *dc);
uint8_t load_colorspace(fz_context *ctx, dvi_context *dc, dvi_state *st, const char *name);
int lookup_pattern(fz_context *ctx, dvi_context *dc, dvi_state *st, const char *name);
void paint(fz_context *ctx, dvi_context *dc, dvi_state *st, int op);

// dvi_pdfcode.c
bool run_pdf_code(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim);

#endif /*!DVI_SPECIAL_H*/
