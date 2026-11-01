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

// Interpretation of DVI specials.
//
// The semantics follow (x)dvipdfmx, which XeTeX documents target:
// - the graphic state holds the PDF CTM (gs.ctm): "pdf:code" is interpreted
//   in this coordinate system;
// - DVI positions are relative to an origin (st->origin), which is moved by
//   "pdf:bcontent" and restored by "pdf:econtent" independently of the PDF
//   graphic state;
// - "pdf:literal" and "pdf:content" are interpreted relative to the current
//   DVI position;
// - PDF objects (pdf:obj, pdf:put, pdf:stream, pdf:bxobj, ...) are kept in a
//   document-level store (see dvi_pdfstore.h), and are visible from all pages
//   following their definition.

#include <mupdf/pdf.h>
#include <string.h>
#include <ctype.h>
#include "fz_util.h"
#include "mydvi.h"
#include "mydvi_interp.h"
#include "pdf_lexer.h"
#include "vstack.h"
#include "dvi_pdfstore.h"
#include <math.h>

#define device_cs fz_device_rgb
#define color_params fz_default_color_params

typedef const char *cursor_t;

/*!re2c

re2c:yyfill:enable = 0;
re2c:eof = -1;
re2c:flags:input = custom;
re2c:api:style = free-form;
re2c:define:YYCURSOR    = cur;
re2c:define:YYCTYPE     = int;
re2c:define:YYLESSTHAN  = "cur >= lim";
re2c:define:YYPEEK      = "cur < lim ? *cur : -1";
re2c:define:YYSKIP      = "++cur;";
re2c:define:YYBACKUP    = "mar = cur;";
re2c:define:YYRESTORE   = "cur = mar;";
re2c:define:YYSTAGP     = "@@{tag} = cur;";
re2c:define:YYSTAGN     = "@@{tag} = NULL;";
re2c:define:YYSHIFT     = "cur += @@{shift};";
re2c:define:YYSHIFTSTAG = "@@{tag} += @@{shift};";

nat     = [0-9]+;
int     = "-"? nat;
float   = [-+]? (nat ("." nat*)? | "." nat);
ws      = [ \t\r\n];
rawunit = ("pt" | "mm" | "cm" | "in" | "bp" | "pc" | "dd" | "cc" | "sp");
unit    = "true"? rawunit;
dim     = float ws* unit;
ident   = [!"#$&'*+,\-.0-9:;=?@A-Z\\^_`a-z|~]+;

*/

/*!stags:re2c format = 'static cursor_t @@;\n'; */

static bool
unhandled(const char *kind, cursor_t cur, cursor_t lim, int ignored)
{
  if (0 && !ignored)
    fprintf(stderr, "unhandled %s: \"%.*s\"\n", kind, (int)(lim - cur), cur);
  return 0;
}

/*********************/
/* Parsing utilities */
/*********************/

static int pnat(cursor_t ptr, cursor_t lim)
{
  int result = 0;
  for (; ptr < lim && *ptr >= '0' && *ptr <= '9'; ptr += 1)
    result = result * 10 + (*ptr - '0');
  return result;
}

static int pint(cursor_t ptr, cursor_t lim)
{
  if (ptr < lim && *ptr == '-')
    return -pnat(ptr + 1, lim);
  else
    return pnat(ptr, lim);
}

// Conversion factor from a unit to TeX points
static double
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

static float
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

static float pfloat(cursor_t ptr, cursor_t lim)
{
  return pfloat_or_dim(ptr, lim, 0);
}

static float pdim(cursor_t ptr, cursor_t lim)
{
  return pfloat_or_dim(ptr, lim, 1);
}

static cursor_t skip_ws(cursor_t cur, cursor_t lim)
{
  while (cur < lim && (*cur == ' ' || *cur == '\t' || *cur == '\r' || *cur == '\n'))
    cur++;
  return cur;
}

/**********/
/* Colors */
/**********/

static void color_set_rgb(float color[3], float r, float g, float b)
{
  color[0] = r;
  color[1] = g;
  color[2] = b;
}

static void color_set_gray(float color[3], float gray)
{
  color_set_rgb(color, gray, gray, gray);
}

static void color_set_cmyk(fz_context *ctx, float color[3], float c, float m, float y, float k)
{
  float cmyk[4] = {c, m, y, k};
  fz_convert_color(ctx, fz_device_cmyk(ctx), cmyk, fz_device_rgb(ctx), color,
                   NULL, color_params);
}

static void color_set_hsb(float color[3], float h, float s, float b)
{
  h = (h - floorf(h)) * 6;
  int i = (int)h;
  float f = h - i, p = b * (1 - s), q = b * (1 - s * f), t = b * (1 - s * (1 - f));
  switch (i)
  {
    case 0:  color_set_rgb(color, b, t, p); break;
    case 1:  color_set_rgb(color, q, b, p); break;
    case 2:  color_set_rgb(color, p, b, t); break;
    case 3:  color_set_rgb(color, p, q, b); break;
    case 4:  color_set_rgb(color, t, p, b); break;
    default: color_set_rgb(color, b, p, q); break;
  }
}

// Convert components to RGB, with n = 1 (gray), 3 (rgb) or 4 (cmyk)
static bool color_set_components(fz_context *ctx, float color[3], const float *v, int n)
{
  switch (n)
  {
    case 1: color_set_gray(color, v[0]); return 1;
    case 3: color_set_rgb(color, v[0], v[1], v[2]); return 1;
    case 4: color_set_cmyk(ctx, color, v[0], v[1], v[2], v[3]); return 1;
    default: return 0;
  }
}

static uint8_t colorspace_of_components(int n)
{
  return n == 4 ? DVI_CS_CMYK : n == 3 ? DVI_CS_RGB : DVI_CS_GRAY;
}

static void colorstate_set_fill(dvi_colorstate *cs, uint8_t space, const float rgb[3])
{
  memcpy(cs->fill, rgb, sizeof(float) * 3);
  cs->fill_cs = space;
  cs->fill_pattern = 0;
}

static void colorstate_set_line(dvi_colorstate *cs, uint8_t space, const float rgb[3])
{
  memcpy(cs->line, rgb, sizeof(float) * 3);
  cs->line_cs = space;
  cs->line_pattern = 0;
}

static cursor_t
colorspec_numbers(fz_context *ctx, cursor_t cur, cursor_t lim, float rgb[3], uint8_t *space,
                  cursor_t f0, cursor_t f1, cursor_t f2, cursor_t f3)
{
  if (f3)
  {
    color_set_cmyk(ctx, rgb, pfloat(f0, lim), pfloat(f1, lim), pfloat(f2, lim), pfloat(f3, lim));
    *space = DVI_CS_CMYK;
  }
  else if (f1)
  {
    color_set_rgb(rgb, pfloat(f0, lim), pfloat(f1, lim), pfloat(f2, lim));
    *space = DVI_CS_RGB;
  }
  else
  {
    color_set_gray(rgb, pfloat(f0, lim));
    *space = DVI_CS_GRAY;
  }
  return cur;
}

// Parse a color specification as accepted by dvipdfmx:
//   rgb R G B | cmyk C M Y K | gray G | hsb H S B
//   [N] | [R G B] | [C M Y K] | N | R G B | C M Y K
// Returns the cursor after the color, or NULL if no color could be parsed.
static cursor_t
parse_colorspec(fz_context *ctx, cursor_t cur, cursor_t lim, float rgb[3], uint8_t *space)
{
  cursor_t mar, f0, f1, f2, f3;

  cur = skip_ws(cur, lim);

  /*!re2c

  "gray" ws+ @f0 float
  {
    color_set_gray(rgb, pfloat(f0, lim));
    *space = DVI_CS_GRAY;
    return cur;
  }

  "rgb" ws+ @f0 float ws+ @f1 float ws+ @f2 float
  {
    color_set_rgb(rgb, pfloat(f0, lim), pfloat(f1, lim), pfloat(f2, lim));
    *space = DVI_CS_RGB;
    return cur;
  }

  "hsb" ws+ @f0 float ws+ @f1 float ws+ @f2 float
  {
    color_set_hsb(rgb, pfloat(f0, lim), pfloat(f1, lim), pfloat(f2, lim));
    *space = DVI_CS_RGB;
    return cur;
  }

  "cmyk" ws+ @f0 float ws+ @f1 float ws+ @f2 float ws+ @f3 float
  {
    color_set_cmyk(ctx, rgb, pfloat(f0, lim), pfloat(f1, lim), pfloat(f2, lim), pfloat(f3, lim));
    *space = DVI_CS_CMYK;
    return cur;
  }

  "[" ws* @f0 float (ws+ @f1 float ws+ @f2 float (ws+ @f3 float)?)? ws* "]"
  { return colorspec_numbers(ctx, cur, lim, rgb, space, f0, f1, f2, f3); }

  @f0 float (ws+ @f1 float ws+ @f2 float (ws+ @f3 float)?)?
  { return colorspec_numbers(ctx, cur, lim, rgb, space, f0, f1, f2, f3); }

  ''
  { return NULL; }

  */
}

// Parse a color special ("color push ..."), setting both fill and line colors
static bool parse_color(fz_context *ctx, dvi_colorstate *state, cursor_t cur, cursor_t lim)
{
  float rgb[3];
  uint8_t space;
  if (!parse_colorspec(ctx, cur, lim, rgb, &space))
    return unhandled("color", cur, lim, 0);
  colorstate_set_fill(state, space, rgb);
  colorstate_set_line(state, space, rgb);
  return 1;
}

// Interpret the color operators of a PDF literal ("0 g 0 G", "1 0 0 rg", ...)
static bool parse_pdfcolor(fz_context *ctx, dvi_colorstate *state, cursor_t cur, cursor_t lim)
{
  vstack *stack = vstack_new(ctx);
  bool result = 1;
  fz_var(result);

  fz_try(ctx)
  {
    enum PDF_OP op;
    while ((op = pdf_parse_command(ctx, stack, &cur, lim)) != PDF_NONE)
    {
      val args = vstack_get_values(ctx, stack);
      float v[4];
      int n = args.length;
      if (n > 4) n = 4;
      for (int i = 0; i < n; i++)
      {
        val x = val_array_get(ctx, stack, args, i);
        v[i] = val_is_number(x) ? x.f : 0;
      }
      float rgb[3];
      bool fill = 1;

      switch (op)
      {
        case PDF_OP_G: case PDF_OP_RG: case PDF_OP_K:
        case PDF_OP_SC: case PDF_OP_SCN:
          fill = 0;
          /* fall through */
        case PDF_OP_g: case PDF_OP_rg: case PDF_OP_k:
        case PDF_OP_sc: case PDF_OP_scn:
          if (color_set_components(ctx, rgb, v, n))
          {
            if (fill)
              colorstate_set_fill(state, colorspace_of_components(n), rgb);
            else
              colorstate_set_line(state, colorspace_of_components(n), rgb);
          }
          break;

        default:
          break;
      }
    }
  }
  fz_always(ctx)
    vstack_free(ctx, stack);
  fz_catch(ctx)
  {
    fprintf(stderr, "pdf color: %s\n", fz_caught_message(ctx));
    result = 0;
  }
  return result;
}

// Find the end of a parenthesized PDF string, starting after the "("
static cursor_t pdf_string_end(cursor_t cur, cursor_t lim)
{
  int nesting = 1;
  for (; cur < lim; cur++)
  {
    if (*cur == '\\')
      cur++;
    else if (*cur == '(')
      nesting++;
    else if (*cur == ')' && --nesting == 0)
      break;
  }
  return cur;
}

/****************/
/* Color stacks */
/****************/

static dvi_colorstack *get_colorstack(dvi_context *dc, int index)
{
  if (index == -1)
    return &dc->colorstack;
  if (index < 0 || index >= dc->pdfcolorstacks.capacity)
  {
    fprintf(stderr, "pdfcolorstack %d: no such stack\n", index);
    return NULL;
  }
  return &dc->pdfcolorstacks.stacks[index];
}

static bool
pdfcolorstack_current(fz_context *ctx, dvi_context *dc, dvi_state *st, int index)
{
  dvi_colorstack *stack = get_colorstack(dc, index);
  if (!stack)
    return 0;
  if (stack->depth == 0)
    st->gs.colors = stack->origin;
  else
    st->gs.colors = stack->base[stack->depth - 1];
  return 1;
}

static bool
colorstack_push(fz_context *ctx, dvi_context *dc, dvi_state *st, int index)
{
  dvi_colorstack *stack = get_colorstack(dc, index);
  if (!stack)
    return 0;

  if (stack->depth == stack->limit)
  {
    size_t size = stack->limit;
    size = size == 0 ? 16 : size * 2;
    dvi_colorstate *base = fz_malloc_struct_array(ctx, size, dvi_colorstate);
    if (stack->limit > 0)
    {
      memcpy(base, stack->base, sizeof(dvi_colorstate) * stack->limit);
      fz_free(ctx, stack->base);
    }
    stack->base = base;
    stack->limit = size;
  }

  stack->base[stack->depth] = st->gs.colors;
  stack->depth += 1;

  return 1;
}

static bool
colorstack_pop(fz_context *ctx, dvi_context *dc, dvi_state *st, int index)
{
  dvi_colorstack *stack = get_colorstack(dc, index);
  if (!stack)
    return 0;
  if (stack->depth == 0)
  {
    fprintf(stderr, "pdfcolorstack_pop %d: empty stack\n", index);
    return 0;
  }
  stack->depth -= 1;
  st->gs.colors = stack->base[stack->depth];
  return 1;
}

static dvi_colorstack *
colorstack_init(fz_context *ctx, dvi_context *dc, int index)
{
  if (index < 0)
    return NULL;
  if (index >= dc->pdfcolorstacks.capacity)
  {
    size_t cap = dc->pdfcolorstacks.capacity;
    size_t newcap = cap == 0 ? 4 : cap;
    while (newcap <= index) newcap *= 2;
    dvi_colorstack *stacks = fz_malloc_struct_array(ctx, newcap, dvi_colorstack);
    if (cap > 0)
    {
      memcpy(stacks, dc->pdfcolorstacks.stacks, sizeof(dvi_colorstack) * cap);
      fz_free(ctx, dc->pdfcolorstacks.stacks);
    }
    dc->pdfcolorstacks.stacks = stacks;
    dc->pdfcolorstacks.capacity = newcap;
  }
  return &dc->pdfcolorstacks.stacks[index];
}

/*****************************/
/* Transformations and state */
/*****************************/

// Position of the DVI cursor in the current user space
static fz_point current_point(dvi_context *dc, dvi_state *st)
{
  float s = dc->scale;
  return fz_make_point((st->registers.h - st->origin.h) * s,
                       - (st->registers.v - st->origin.v) * s);
}

// Concatenate a transformation, applied around the current point
static void concat_around_current_point(dvi_context *dc, dvi_state *st, fz_matrix m)
{
  fz_point cp = current_point(dc, st);
  m = fz_concat(fz_translate(-cp.x, -cp.y), fz_concat(m, fz_translate(cp.x, cp.y)));
  st->gs.ctm = fz_concat(m, st->gs.ctm);
}

static void pop_clips(fz_context *ctx, dvi_context *dc, int from, int to)
{
  if (dc->dev)
    for (int i = to; i < from; ++i)
      fz_pop_clip(ctx, dc->dev);
}

static bool gs_push(dvi_state *st)
{
  if (st->gs_stack.depth >= st->gs_stack.limit)
  {
    fprintf(stderr, "pdf: graphic state stack overflow\n");
    return 0;
  }
  st->gs_stack.base[st->gs_stack.depth] = st->gs;
  st->gs_stack.depth += 1;
  return 1;
}

static bool gs_pop(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  if (st->gs_stack.depth == 0)
  {
    fprintf(stderr, "pdf: graphic state stack underflow\n");
    return 0;
  }
  st->gs_stack.depth -= 1;
  int clip_depth = st->gs.clip_depth;
  st->gs = st->gs_stack.base[st->gs_stack.depth];
  pop_clips(ctx, dc, clip_depth, st->gs.clip_depth);
  return 1;
}

// Prepare a state for interpreting a sub-content (form XObject or pattern),
// sharing the stacks of the parent state
static void
enter_substate(dvi_state *sub, const dvi_state *st, fz_matrix ctm, pdf_obj *resources)
{
  *sub = *st;
  sub->gs.ctm = ctm;
  sub->origin.h = sub->origin.v = 0;
  memset(&sub->registers, 0, sizeof(sub->registers));
  sub->registers_stack.base = st->registers_stack.base + st->registers_stack.depth;
  sub->registers_stack.limit = st->registers_stack.limit - st->registers_stack.depth;
  sub->registers_stack.depth = 0;
  sub->gs_stack.base = st->gs_stack.base + st->gs_stack.depth;
  sub->gs_stack.limit = st->gs_stack.limit - st->gs_stack.depth;
  sub->gs_stack.depth = 0;
  sub->coords_stack.base = st->coords_stack.base + st->coords_stack.depth;
  sub->coords_stack.limit = st->coords_stack.limit - st->coords_stack.depth;
  sub->coords_stack.depth = 0;
  sub->base_ctm = ctm;
  if (resources)
    sub->resources = resources;
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

static struct xform_spec xform_spec()
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

static cursor_t
parse_xform_or_dim(struct xform_spec *xf, cursor_t cur, cursor_t lim)
{
  cursor_t f0, f1, f2, f3, f4, f5, mar;
  float r = 0, sx = 1.0, sy = 1.0;
  bool has_matrix = 0;

  while (cur < lim)
  {
    cursor_t tok = cur;
    /*!re2c

    ws
    { continue; }

    "rotate" ws+ @f0 float
    {
      r = pfloat(f0, lim);
      continue;
    }

    "clip" ws+ @f0 int
    {
      xf->clip = pint(f0, lim);
      continue;
    }

    "hide"
    {
      xf->hide = 1;
      continue;
    }

    "scale" ws+ @f0 float
    {
      sx = sy = pfloat(f0, lim);
      continue;
    }

    "xscale" ws+ @f0 float
    {
      sx = pfloat(f0, lim);
      continue;
    }

    "yscale" ws+ @f0 float
    {
      sy = pfloat(f0, lim);
      continue;
    }

    "width" ws+ @f0 dim
    {
      xf->width = pdim(f0, lim);
      continue;
    }

    "height" ws+ @f0 dim
    {
      xf->height = pdim(f0, lim);
      continue;
    }

    "depth" ws+ @f0 dim
    {
      xf->depth = pdim(f0, lim);
      continue;
    }

    "bbox" ws+ @f0 float
           ws+ @f1 float
           ws+ @f2 float
           ws+ @f3 float
    {
      xf->bbox.x0 = pfloat(f0, lim);
      xf->bbox.y0 = pfloat(f1, lim);
      xf->bbox.x1 = pfloat(f2, lim);
      xf->bbox.y1 = pfloat(f3, lim);
      continue;
    }

    "page" ws+ @f0 nat
    {
      xf->page = pint(f0, lim);
      continue;
    }

    "pagebox" ws+ @f0 ("mediabox" | "cropbox" | "artbox" | "bleedbox" | "trimbox")
    {
      int c = f0[0];
      switch (c)
      {
        case 'm':
          xf->pagebox = BOX_MEDIABOX;
          break;
        case 'c':
          xf->pagebox = BOX_CROPBOX;
          break;
        case 'a':
          xf->pagebox = BOX_ARTBOX;
          break;
        case 'b':
          xf->pagebox = BOX_BLEEDBOX;
          break;
        case 't':
          xf->pagebox = BOX_TRIMBOX;
          break;
      }
      continue;
    }

    "matrix" ws+ @f0 float
             ws+ @f1 float
             ws+ @f2 float
             ws+ @f3 float
             ws+ @f4 float
             ws+ @f5 float
    {
      xf->ctm.a = pfloat(f0, lim);
      xf->ctm.b = pfloat(f1, lim);
      xf->ctm.c = pfloat(f2, lim);
      xf->ctm.d = pfloat(f3, lim);
      xf->ctm.e = pfloat(f4, lim);
      xf->ctm.f = pfloat(f5, lim);
      has_matrix = 1;
      continue;
    }

    ''
    {
      cur = tok;
      break;
    }

    */
  }

  // Like dvipdfmx, an explicit matrix takes precedence over scale & rotate,
  // otherwise scaling is applied before rotation.
  if (!has_matrix && (sx != 1.0 || sy != 1.0 || r != 0))
    xf->ctm = fz_concat(fz_scale(sx, sy), fz_rotate(r));

  return cur;
}

/****************/
/* Object store */
/****************/

static pdf_document *store_doc(dvi_context *dc)
{
  return dvi_pdfstore_doc(dc->pdfstore);
}

// Load an object of the store, returning NULL for undefined objects
static pdf_obj *load_object(fz_context *ctx, dvi_context *dc, int num)
{
  pdf_obj *obj = NULL;
  fz_try(ctx)
    obj = pdf_load_object(ctx, store_doc(dc), num);
  fz_catch(ctx)
  {
    fz_ignore_error(ctx);
    obj = NULL;
  }
  if (obj && pdf_is_null(ctx, obj))
  {
    pdf_drop_obj(ctx, obj);
    obj = NULL;
  }
  return obj;
}

// Parse "@ident", returning the object number (allocated if necessary)
static cursor_t parse_name_ref(fz_context *ctx, dvi_context *dc, cursor_t cur, cursor_t lim, int *num, bool create)
{
  cursor_t mar, i0, i1;
  *num = 0;
  cur = skip_ws(cur, lim);

  /*!re2c

  "@" @i0 ident @i1
  {
    *num = dvi_pdfstore_name(ctx, dc->pdfstore, i0, i1 - i0, create);
    return cur;
  }

  ''
  { return NULL; }

  */
}

// The resources that "@resources" refers to
static pdf_obj *current_resources(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  if (dc->forms_depth > 0)
    return pdf_dict_get(ctx, dc->forms[dc->forms_depth - 1].dict, PDF_NAME(Resources));
  return st->resources;
}

static pdf_obj *
lookup_resource(fz_context *ctx, dvi_context *dc, dvi_state *st, pdf_obj *category, const char *name)
{
  pdf_obj *result = NULL;
  if (st->resources)
    result = pdf_dict_gets(ctx, pdf_dict_get(ctx, st->resources, category), name);
  if (!result)
  {
    pdf_obj *res = dvi_pdfstore_page_resources(dc->pdfstore);
    result = pdf_dict_gets(ctx, pdf_dict_get(ctx, res, category), name);
  }
  return result;
}

static void merge_dict(fz_context *ctx, pdf_obj *dst, pdf_obj *src, bool deep)
{
  int n = pdf_dict_len(ctx, src);
  for (int i = 0; i < n; i++)
  {
    pdf_obj *key = pdf_dict_get_key(ctx, src, i);
    pdf_obj *val = pdf_dict_get_val(ctx, src, i);
    pdf_obj *old = pdf_dict_get(ctx, dst, key);
    if (deep && pdf_is_dict(ctx, old) && pdf_is_dict(ctx, val) && old != val)
      merge_dict(ctx, old, val, 0);
    else
      pdf_dict_put(ctx, dst, key, val);
  }
}

static bool is_eol(char c)
{
  return c == '\r' || c == '\n';
}

// Find the "stream" keyword following a dictionary
// (the data of the stream can contain arbitrary bytes, it is not parsed)
static cursor_t find_stream_keyword(cursor_t cur, cursor_t lim)
{
  for (cursor_t p = cur; p + 6 <= lim; p++)
  {
    if (memcmp(p, "stream", 6) != 0 || (p + 6 < lim && !isspace((unsigned char)p[6])))
      continue;
    cursor_t q = p;
    while (q > cur && isspace((unsigned char)q[-1]))
      q--;
    if (q - cur >= 2 && q[-1] == '>' && q[-2] == '>')
      return p;
  }
  return NULL;
}

// pdf:obj @name OBJECT
// pdf:obj @name DICT stream DATA endstream
static void special_obj(fz_context *ctx, dvi_context *dc, cursor_t cur, cursor_t lim)
{
  int num;
  cur = parse_name_ref(ctx, dc, cur, lim, &num, 1);
  if (!cur)
    return;

  cursor_t kw = find_stream_keyword(cur, lim);
  fz_stream *stm = dvi_pdfstore_open(ctx, dc->pdfstore, cur, kw ? kw : lim);
  pdf_obj *obj = NULL, *ref = NULL;
  fz_buffer *buf = NULL;
  fz_var(obj);
  fz_var(ref);
  fz_var(buf);
  fz_try(ctx)
  {
    pdf_document *doc = store_doc(dc);
    obj = dvi_pdfstore_parse(ctx, dc->pdfstore, stm);
    if (obj)
      pdf_update_object(ctx, doc, num, obj);
    if (obj && kw && pdf_is_dict(ctx, obj))
    {
      cursor_t data = kw + 6, end = lim;
      if (data < lim && *data == '\r') data++;
      if (data < lim && *data == '\n') data++;
      for (cursor_t p = lim - 9; p >= data; p--)
      {
        if (memcmp(p, "endstream", 9) == 0)
        {
          end = p;
          break;
        }
      }
      if (end > data && is_eol(end[-1])) end--;
      if (end > data && is_eol(end[-1])) end--;
      buf = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)data, end - data);
      ref = pdf_new_indirect(ctx, doc, num, 0);
      // The data is encoded according to the filters of the dictionary
      pdf_update_stream(ctx, doc, ref, buf, 1);
    }
  }
  fz_always(ctx)
  {
    fz_drop_buffer(ctx, buf);
    pdf_drop_obj(ctx, ref);
    pdf_drop_obj(ctx, obj);
    fz_drop_stream(ctx, stm);
  }
  fz_catch(ctx)
    fz_rethrow(ctx);
}

// pdf:put @name OBJECT
static void special_put(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  pdf_obj *target = NULL, *val = NULL;
  fz_stream *stm = NULL;
  int num = 0;
  bool resources = 0;

  fz_var(target);
  fz_var(val);
  fz_var(stm);

  cur = skip_ws(cur, lim);
  if (lim - cur >= 10 && memcmp(cur, "@resources", 10) == 0 &&
      (lim - cur == 10 || cur[10] == ' ' || cur[10] == '<' || cur[10] == '\n'))
  {
    target = pdf_keep_obj(ctx, current_resources(ctx, dc, st));
    resources = 1;
    cur += 10;
  }
  else
  {
    cur = parse_name_ref(ctx, dc, cur, lim, &num, 1);
    if (!cur)
      return;
    target = load_object(ctx, dc, num);
  }

  fz_try(ctx)
  {
    stm = dvi_pdfstore_open(ctx, dc->pdfstore, cur, lim);
    while ((val = dvi_pdfstore_parse(ctx, dc->pdfstore, stm)))
    {
      if (!target && num)
      {
        // Undefined object: define it
        pdf_update_object(ctx, store_doc(dc), num, val);
        target = pdf_keep_obj(ctx, val);
      }
      else if (pdf_is_dict(ctx, target) && pdf_is_dict(ctx, val))
        merge_dict(ctx, target, val, resources);
      else if (pdf_is_array(ctx, target) && pdf_is_array(ctx, val))
      {
        int n = pdf_array_len(ctx, val);
        for (int i = 0; i < n; i++)
          pdf_array_push(ctx, target, pdf_array_get(ctx, val, i));
      }
      else if (pdf_is_array(ctx, target))
        pdf_array_push(ctx, target, val);
      pdf_drop_obj(ctx, val);
      val = NULL;
    }
  }
  fz_always(ctx)
  {
    pdf_drop_obj(ctx, val);
    pdf_drop_obj(ctx, target);
    fz_drop_stream(ctx, stm);
  }
  fz_catch(ctx)
    fz_rethrow(ctx);
}

// pdf:stream @name (DATA) DICT
static void special_stream(fz_context *ctx, dvi_context *dc, cursor_t cur, cursor_t lim)
{
  int num;
  cur = parse_name_ref(ctx, dc, cur, lim, &num, 1);
  if (!cur)
    return;

  fz_stream *stm = NULL;
  pdf_obj *data = NULL, *dict = NULL, *ref = NULL;
  fz_buffer *buf = NULL;
  fz_var(stm);
  fz_var(data);
  fz_var(dict);
  fz_var(ref);
  fz_var(buf);

  fz_try(ctx)
  {
    pdf_document *doc = store_doc(dc);
    stm = dvi_pdfstore_open(ctx, dc->pdfstore, cur, lim);
    data = dvi_pdfstore_parse(ctx, dc->pdfstore, stm);
    if (!pdf_is_string(ctx, data))
      fz_throw(ctx, FZ_ERROR_SYNTAX, "pdf:stream: expecting a string");
    dict = dvi_pdfstore_parse(ctx, dc->pdfstore, stm);
    if (!pdf_is_dict(ctx, dict))
    {
      pdf_drop_obj(ctx, dict);
      dict = pdf_new_dict(ctx, doc, 1);
    }
    // Data is stored uncompressed
    pdf_dict_del(ctx, dict, PDF_NAME(Filter));
    pdf_dict_del(ctx, dict, PDF_NAME(DecodeParms));
    pdf_update_object(ctx, doc, num, dict);
    buf = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)pdf_to_str_buf(ctx, data),
                                         pdf_to_str_len(ctx, data));
    ref = pdf_new_indirect(ctx, doc, num, 0);
    pdf_update_stream(ctx, doc, ref, buf, 0);
  }
  fz_always(ctx)
  {
    fz_drop_buffer(ctx, buf);
    pdf_drop_obj(ctx, ref);
    pdf_drop_obj(ctx, dict);
    pdf_drop_obj(ctx, data);
    fz_drop_stream(ctx, stm);
  }
  fz_catch(ctx)
    fz_rethrow(ctx);
}

/*******************/
/* Images and PDFs */
/*******************/

static void
embed_pdf(fz_context *ctx, dvi_context *dc, fz_matrix ctm, struct xform_spec *xf, const char *filename)
{
  pdf_page *page = NULL;
  fz_var(page);

  fz_try(ctx)
  {
    pdf_document *doc = dvi_resmanager_get_pdf(ctx, dc->resmanager, filename);
    if (!doc)
      fz_throw(ctx, FZ_ERROR_GENERIC, "cannot open %s", filename);

    page = pdf_load_page(ctx, doc, xf->page > 0 ? xf->page - 1 : 0);

    // Bounds of the page in MuPDF coordinates (origin at the top-left corner)
    fz_rect bounds = pdf_bound_page(ctx, page, FZ_CROP_BOX);
    float bw = bounds.x1 - bounds.x0, bh = bounds.y1 - bounds.y0;
    if (bw <= 0 || bh <= 0)
      bw = bh = 1;

    float sx = 1, sy = 1;
    float w = xf->width, h = xf->height;
    if (w == w && h == h)
    {
      sx = w / bw;
      sy = h / bh;
    }
    else if (w == w)
      sx = sy = w / bw;
    else if (h == h)
      sx = sy = h / bh;

    // Map the page to user space, lower-left corner at the origin
    ctm = fz_concat(xf->ctm, ctm);
    ctm = fz_pre_scale(fz_pre_translate(ctm, 0, bh * sy), sx, -sy);
    ctm = fz_pre_translate(ctm, -bounds.x0, -bounds.y0);
    pdf_run_page(ctx, page, dc->dev, ctm, NULL);
  }
  fz_always(ctx)
    fz_drop_page(ctx, (fz_page*)page);
  fz_catch(ctx)
    fz_rethrow(ctx);
}

static void
embed_image(fz_context *ctx, dvi_context *dc, fz_matrix ctm, struct xform_spec *xf, const char *filename)
{
  fz_image *img = dvi_resmanager_get_img(ctx, dc->resmanager, filename);
  if (!img)
    fz_throw(ctx, FZ_ERROR_GENERIC, "cannot open %s", filename);

  fz_try(ctx)
  {
    ctm = fz_concat(xf->ctm, ctm);
    float ar = (float)img->w / (float)img->h;
    float w = xf->width, h = xf->height;
    if (w != w && h != h)
    {
      // Like dvipdfmx for images without resolution information: 72 dpi
      w = img->w;
      h = img->h;
    }
    else if (w != w)
      w = h * ar;
    else if (h != h)
      h = w / ar;
    ctm = fz_pre_scale(fz_pre_translate(ctm, 0, h), w, -h);
    fz_fill_image(ctx, dc->dev, img, ctm, 1.0, color_params);
  }
  fz_always(ctx)
    fz_drop_image(ctx, img);
  fz_catch(ctx)
    fz_rethrow(ctx);
}

static bool
embed_graphics(fz_context *ctx, dvi_context *dc, fz_matrix ctm, struct xform_spec *xf, const char *filename)
{
  if (!dc->dev)
    return 1;

  const char *ext = filename;

  for (const char *ptr = ext; *ptr; ptr++)
    if (*ptr == '.') ext = ptr + 1;

  bool ok = 1;
  fz_try(ctx)
  {
    if ((ext[0] == 'p' || ext[0] == 'P') &&
        (ext[1] == 'd' || ext[1] == 'D') &&
        (ext[2] == 'f' || ext[2] == 'F'))
      embed_pdf(ctx, dc, ctm, xf, filename);
    else
      embed_image(ctx, dc, ctm, xf, filename);
  }
  fz_catch(ctx)
  {
    fprintf(stderr, "error rendering image %s: %s\n", filename, fz_caught_message(ctx));
    ok = 0;
  }
  return ok;
}

// pdf:image [@name] [dimensions and transformations] (FILENAME) [DICT]
static bool
special_image(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim, bool render)
{
  int num = 0;
  cursor_t after_name = parse_name_ref(ctx, dc, cur, lim, &num, 1);
  if (after_name)
    cur = after_name;

  struct xform_spec xf = xform_spec();
  cur = parse_xform_or_dim(&xf, cur, lim);
  cur = skip_ws(cur, lim);
  if (cur >= lim || *cur != '(')
    return unhandled("pdf image", cur, lim, 0);

  cursor_t fstart = cur + 1, fend = pdf_string_end(fstart, lim);
  char filename[2048];
  int flen = fend - fstart;
  if (flen >= (int)sizeof(filename))
    return 0;
  // Unescape the file name
  int n = 0;
  for (cursor_t p = fstart; p < fend; p++)
  {
    if (*p == '\\' && p + 1 < fend)
      p++;
    filename[n++] = *p;
  }
  filename[n] = 0;

  if (num)
  {
    // Record the image as a named XObject for later use
    pdf_document *doc = store_doc(dc);
    pdf_obj *dict = pdf_new_dict(ctx, doc, 8);
    fz_try(ctx)
    {
      pdf_dict_put(ctx, dict, PDF_NAME(Type), PDF_NAME(XObject));
      pdf_dict_put(ctx, dict, PDF_NAME(Subtype), PDF_NAME(Image));
      pdf_dict_puts_drop(ctx, dict, "TXPFile", pdf_new_text_string(ctx, filename));
      pdf_dict_put_matrix(ctx, dict, PDF_NAME(Matrix), xf.ctm);
      if (xf.width == xf.width)
        pdf_dict_put_real(ctx, dict, PDF_NAME(Width), xf.width);
      if (xf.height == xf.height)
        pdf_dict_put_real(ctx, dict, PDF_NAME(Height), xf.height);
      if (xf.page > 0)
        pdf_dict_put_int(ctx, dict, PDF_NAME(Page), xf.page);
      pdf_update_object(ctx, doc, num, dict);
    }
    fz_always(ctx)
      pdf_drop_obj(ctx, dict);
    fz_catch(ctx)
      fz_rethrow(ctx);
  }

  if (!render || xf.hide)
    return 1;

  return embed_graphics(ctx, dc, dvi_get_ctm(dc, st), &xf, filename);
}

/*******************/
/* Form XObjects   */
/*******************/

static bool source_contains(dvi_context *dc, cursor_t ptr)
{
  const char *source = (const char *)dc->source;
  return source && ptr >= source && ptr <= source + dc->source_len;
}

// pdf:bxobj @name [dimensions]
static bool
form_begin(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim, bool render)
{
  int num;
  cur = parse_name_ref(ctx, dc, cur, lim, &num, 1);
  if (!cur)
    return unhandled("pdf:bxobj without name", cur, lim, 0);

  if (dc->forms_depth >= (int)(sizeof(dc->forms) / sizeof(dc->forms[0])))
  {
    fprintf(stderr, "pdf:bxobj: too many nested forms\n");
    return 0;
  }

  if (!source_contains(dc, lim))
    return 0;

  struct xform_spec xf = xform_spec();
  parse_xform_or_dim(&xf, cur, lim);

  fz_rect bbox;
  if (!fz_is_infinite_rect(xf.bbox))
    bbox = xf.bbox;
  else
  {
    bbox.x0 = 0;
    bbox.y0 = xf.depth == xf.depth ? -xf.depth : 0;
    bbox.x1 = xf.width == xf.width ? xf.width : 0;
    bbox.y1 = xf.height == xf.height ? xf.height : 0;
  }

  pdf_document *doc = store_doc(dc);
  pdf_obj *dict = pdf_new_dict(ctx, doc, 8);
  fz_try(ctx)
  {
    pdf_dict_put(ctx, dict, PDF_NAME(Type), PDF_NAME(XObject));
    pdf_dict_put(ctx, dict, PDF_NAME(Subtype), PDF_NAME(Form));
    pdf_dict_put_rect(ctx, dict, PDF_NAME(BBox), bbox);
    pdf_dict_put_dict(ctx, dict, PDF_NAME(Resources), 4);
  }
  fz_catch(ctx)
  {
    pdf_drop_obj(ctx, dict);
    fz_rethrow(ctx);
  }

  if (render && dc->text)
    dvi_context_flush_text(ctx, dc, st);

  dvi_formcapture *f = &dc->forms[dc->forms_depth++];
  f->num = num;
  f->start = lim - (const char *)dc->source;
  f->dict = dict;
  f->restore = render;
  if (render)
  {
    // The content of the form is not drawn on the page, and changes to the
    // graphic state are local to the form
    f->dev = dc->dev;
    f->gs = st->gs;
    f->origin = st->origin;
    f->gs_depth = st->gs_stack.depth;
    f->coords_depth = st->coords_stack.depth;
    dc->dev = NULL;
  }
  return 1;
}

// pdf:exobj [DICT]
static bool
form_end(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t spc, cursor_t cur, cursor_t lim)
{
  if (dc->forms_depth == 0)
    return unhandled("pdf:exobj without pdf:bxobj", cur, lim, 0);

  dvi_formcapture *f = &dc->forms[--dc->forms_depth];
  pdf_obj *dict = f->dict;
  pdf_obj *attr = NULL, *old = NULL;
  fz_stream *stm = NULL;
  fz_var(attr);
  fz_var(old);
  fz_var(stm);

  if (f->restore)
  {
    dc->dev = f->dev;
    st->gs = f->gs;
    st->origin = f->origin;
    st->gs_stack.depth = f->gs_depth;
    st->coords_stack.depth = f->coords_depth;
  }

  bool ok = 1;
  fz_try(ctx)
  {
    cur = skip_ws(cur, lim);
    if (cur < lim)
    {
      stm = dvi_pdfstore_open(ctx, dc->pdfstore, cur, lim);
      attr = dvi_pdfstore_parse(ctx, dc->pdfstore, stm);
      if (pdf_is_dict(ctx, attr))
        merge_dict(ctx, dict, attr, 0);
    }

    size_t start = f->start;
    size_t end = spc - (const char *)dc->source;
    if (!source_contains(dc, spc) || end < start)
      fz_throw(ctx, FZ_ERROR_GENERIC, "pdf:exobj: invalid form content");

    pdf_dict_puts_drop(ctx, dict, "TXPStart", pdf_new_int(ctx, start));
    pdf_dict_puts_drop(ctx, dict, "TXPEnd", pdf_new_int(ctx, end));

    // The font selected at the beginning of the form is not known when
    // scanning previous pages, in this case keep the last known one.
    if (f->restore)
      pdf_dict_puts_drop(ctx, dict, "TXPFont", pdf_new_int(ctx, st->f));
    else
    {
      old = load_object(ctx, dc, f->num);
      if (pdf_is_dict(ctx, old) &&
          pdf_to_int64(ctx, pdf_dict_gets(ctx, old, "TXPStart")) == (int64_t)start &&
          pdf_dict_gets(ctx, old, "TXPFont"))
        pdf_dict_puts(ctx, dict, "TXPFont", pdf_dict_gets(ctx, old, "TXPFont"));
    }

    pdf_update_object(ctx, store_doc(dc), f->num, dict);
  }
  fz_always(ctx)
  {
    fz_drop_stream(ctx, stm);
    pdf_drop_obj(ctx, attr);
    pdf_drop_obj(ctx, old);
    pdf_drop_obj(ctx, dict);
  }
  fz_catch(ctx)
  {
    fprintf(stderr, "pdf:exobj: %s\n", fz_caught_message(ctx));
    ok = 0;
  }
  return ok;
}

static void fill_rect_path(fz_context *ctx, fz_path *path, fz_rect r)
{
  fz_moveto(ctx, path, r.x0, r.y0);
  fz_lineto(ctx, path, r.x1, r.y0);
  fz_lineto(ctx, path, r.x1, r.y1);
  fz_lineto(ctx, path, r.x0, r.y1);
  fz_closepath(ctx, path);
}

static void clip_rect(fz_context *ctx, dvi_context *dc, fz_rect r, fz_matrix ctm)
{
  fz_path *path = fz_new_path(ctx);
  fz_try(ctx)
  {
    fill_rect_path(ctx, path, r);
    fz_clip_path(ctx, dc->dev, path, 0, ctm, fz_infinite_rect);
  }
  fz_always(ctx)
    fz_drop_path(ctx, path);
  fz_catch(ctx)
    fz_rethrow(ctx);
}

static bool pdf_code(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim);

// The path being constructed is saved when interpreting a sub-content
// (form, pattern cell, soft mask), which can be painted in the middle of the
// construction of a path.
typedef struct
{
  fz_path *path;
  int path_clip;
} saved_path;

static saved_path save_path(dvi_context *dc)
{
  saved_path saved = {dc->path, dc->path_clip};
  dc->path = NULL;
  dc->path_clip = 0;
  return saved;
}

static void restore_path(fz_context *ctx, dvi_context *dc, saved_path saved)
{
  if (dc->path)
    fz_drop_path(ctx, dc->path);
  dc->path = saved.path;
  dc->path_clip = saved.path_clip;
}

// Interpret the content of a form XObject recorded from DVI
static void
run_dvi_form(fz_context *ctx, dvi_context *dc, dvi_state *sub, size_t start, size_t end)
{
  enum dvi_version version = dc->root.version;
  size_t offset = start;
  while (offset < end)
  {
    int len = dvi_instr_size(dc->source + offset, end - offset, version);
    // The last special (pdf:exobj) ends after the recorded range
    if (len <= 0 || offset + len > end)
      break;
    dvi_interp_sub(ctx, dc, sub, dc->source + offset);
    offset += len;
  }
}

// Interpret the content of a form XObject defined by a PDF stream
static void
run_pdf_form(fz_context *ctx, dvi_context *dc, dvi_state *sub, pdf_obj *form)
{
  fz_buffer *buf = pdf_load_stream(ctx, form);
  fz_try(ctx)
  {
    const char *data = (const char *)buf->data;
    pdf_code(ctx, dc, sub, data, data + buf->len);
  }
  fz_always(ctx)
    fz_drop_buffer(ctx, buf);
  fz_catch(ctx)
    fz_rethrow(ctx);
}

enum xobject_mode
{
  XOBJ_DRAW,
  // Draw the content of a soft mask (alpha is applied by the mask)
  XOBJ_MASK,
};

// Draw a form XObject in the current state, with its coordinate space mapped
// by `ctm`
static void
draw_form(fz_context *ctx, dvi_context *dc, dvi_state *st, pdf_obj *form, fz_matrix ctm, enum xobject_mode mode)
{
  if (!dc->dev || dc->replay_depth >= 16)
    return;

  pdf_obj *start = pdf_dict_gets(ctx, form, "TXPStart");
  pdf_obj *end = pdf_dict_gets(ctx, form, "TXPEnd");
  bool from_dvi = start && end;

  if (from_dvi && (!dc->source || pdf_to_int64(ctx, end) > (int64_t)dc->source_len))
    return;
  if (!from_dvi && !pdf_is_stream(ctx, form))
    return;

  ctm = fz_concat(pdf_dict_get_matrix(ctx, form, PDF_NAME(Matrix)), ctm);
  fz_rect bbox = pdf_dict_get_rect(ctx, form, PDF_NAME(BBox));
  pdf_obj *group = pdf_dict_get(ctx, form, PDF_NAME(Group));
  pdf_obj *resources = pdf_dict_get(ctx, form, PDF_NAME(Resources));

  if (dc->text)
    dvi_context_flush_text(ctx, dc, st);

  dvi_state sub;
  enter_substate(&sub, st, ctm, resources);
  sub.version = dc->root.version;
  if (pdf_dict_gets(ctx, form, "TXPFont"))
    sub.f = pdf_to_int(ctx, pdf_dict_gets(ctx, form, "TXPFont"));

  bool clipped = 0, grouped = 0;
  fz_var(clipped);
  fz_var(grouped);

  saved_path path = save_path(dc);
  dc->replay_depth += 1;
  fz_try(ctx)
  {
    if (!fz_is_empty_rect(bbox))
    {
      clip_rect(ctx, dc, bbox, ctm);
      clipped = 1;
    }

    if (mode == XOBJ_MASK)
    {
      sub.gs.fill_alpha = sub.gs.line_alpha = 1;
      sub.gs.blend_mode = FZ_BLEND_NORMAL;
    }
    else if (pdf_is_dict(ctx, group))
    {
      // Transparency group: the current alpha and blend mode apply to the
      // group as a whole.
      fz_rect area = fz_transform_rect(bbox, ctm);
      fz_begin_group(ctx, dc->dev, area, NULL,
                     pdf_dict_get_bool(ctx, group, PDF_NAME(I)),
                     pdf_dict_get_bool(ctx, group, PDF_NAME(K)),
                     st->gs.blend_mode, st->gs.fill_alpha);
      grouped = 1;
      sub.gs.fill_alpha = sub.gs.line_alpha = 1;
      sub.gs.blend_mode = FZ_BLEND_NORMAL;
    }

    if (from_dvi)
      run_dvi_form(ctx, dc, &sub, pdf_to_int64(ctx, start), pdf_to_int64(ctx, end));
    else
      run_pdf_form(ctx, dc, &sub, form);
  }
  fz_always(ctx)
  {
    dc->replay_depth -= 1;
    restore_path(ctx, dc, path);
    if (dc->text)
      dvi_context_flush_text(ctx, dc, &sub);
    pop_clips(ctx, dc, sub.gs.clip_depth, st->gs.clip_depth);
    if (grouped)
      fz_end_group(ctx, dc->dev);
    if (clipped)
      fz_pop_clip(ctx, dc->dev);
  }
  fz_catch(ctx)
    fz_rethrow(ctx);
}

// Draw a XObject (form or image) of the store
static void
draw_xobject(fz_context *ctx, dvi_context *dc, dvi_state *st, pdf_obj *xobj, fz_matrix ctm, struct xform_spec *xf)
{
  if (!dc->dev)
    return;

  pdf_obj *file = pdf_dict_gets(ctx, xobj, "TXPFile");
  if (file)
  {
    // Image defined by pdf:image @name
    struct xform_spec spec = xform_spec();
    spec.ctm = pdf_dict_get_matrix(ctx, xobj, PDF_NAME(Matrix));
    if (pdf_dict_get(ctx, xobj, PDF_NAME(Width)))
      spec.width = pdf_dict_get_real(ctx, xobj, PDF_NAME(Width));
    if (pdf_dict_get(ctx, xobj, PDF_NAME(Height)))
      spec.height = pdf_dict_get_real(ctx, xobj, PDF_NAME(Height));
    spec.page = pdf_dict_get_int(ctx, xobj, PDF_NAME(Page));
    if (xf)
    {
      if (xf->width == xf->width) spec.width = xf->width;
      if (xf->height == xf->height) spec.height = xf->height;
      ctm = fz_concat(xf->ctm, ctm);
    }
    embed_graphics(ctx, dc, ctm, &spec, pdf_to_text_string(ctx, file));
  }
  else if (pdf_name_eq(ctx, pdf_dict_get(ctx, xobj, PDF_NAME(Subtype)), PDF_NAME(Image)))
  {
    fz_image *img = pdf_load_image(ctx, store_doc(dc), xobj);
    fz_try(ctx)
      fz_fill_image(ctx, dc->dev, img, fz_pre_scale(fz_pre_translate(ctm, 0, 1), 1, -1),
                    st->gs.fill_alpha, color_params);
    fz_always(ctx)
      fz_drop_image(ctx, img);
    fz_catch(ctx)
      fz_rethrow(ctx);
  }
  else
  {
    if (xf)
      ctm = fz_concat(xf->ctm, ctm);
    draw_form(ctx, dc, st, xobj, ctm, XOBJ_DRAW);
  }
}

// pdf:uxobj @name [dimensions]
static bool special_uxobj(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  int num;
  cur = parse_name_ref(ctx, dc, cur, lim, &num, 0);
  if (!cur || !num)
    return unhandled("pdf:uxobj: unknown object", cur, lim, 0);

  struct xform_spec xf = xform_spec();
  parse_xform_or_dim(&xf, cur, lim);

  pdf_obj *xobj = pdf_new_indirect(ctx, store_doc(dc), num, 0);
  if (!pdf_is_dict(ctx, xobj))
  {
    pdf_drop_obj(ctx, xobj);
    return unhandled("pdf:uxobj: undefined object", cur, lim, 0);
  }

  bool ok = 1;
  fz_try(ctx)
    draw_xobject(ctx, dc, st, xobj, dvi_get_ctm(dc, st), &xf);
  fz_always(ctx)
    pdf_drop_obj(ctx, xobj);
  fz_catch(ctx)
  {
    fprintf(stderr, "pdf:uxobj: %s\n", fz_caught_message(ctx));
    ok = 0;
  }
  return ok;
}

/************/
/* Painting */
/************/

static fz_path *get_path(fz_context *ctx, dvi_context *dc)
{
  if (!dc->path)
    dc->path = fz_new_path(ctx);
  return dc->path;
}

static void drop_path(fz_context *ctx, dvi_context *dc)
{
  if (dc->path)
  {
    fz_drop_path(ctx, dc->path);
    dc->path = NULL;
  }
  dc->path_clip = 0;
}

static fz_stroke_state *new_stroke_state(fz_context *ctx, dvi_graphicstate *gs)
{
  fz_stroke_state *stst = fz_new_stroke_state_with_dash_len(ctx, gs->dash_len);
  stst->linewidth = gs->line_width;
  stst->linejoin = (fz_linejoin)gs->line_join;
  stst->miterlimit = gs->miter_limit;
  stst->dash_cap = stst->start_cap = stst->end_cap = (fz_linecap)gs->line_caps;
  stst->dash_len = gs->dash_len;
  memcpy(stst->dash_list, gs->dash, sizeof(float) * gs->dash_len);
  stst->dash_phase = gs->dash_phase;
  return stst;
}

// Draw the cell of a tiling pattern, `ptm` maps pattern space to device space
static void
run_pattern_cell(fz_context *ctx, dvi_context *dc, dvi_state *st, pdf_obj *pat, fz_matrix ptm, const float *color)
{
  dvi_state sub;
  enter_substate(&sub, st, ptm, pdf_dict_get(ctx, pat, PDF_NAME(Resources)));
  dvi_graphicstate_init(&sub.gs, ptm);
  sub.gs.clip_depth = st->gs.clip_depth;
  if (color)
  {
    // Uncolored pattern: the color is given when the pattern is selected
    colorstate_set_fill(&sub.gs.colors, DVI_CS_RGB, color);
    colorstate_set_line(&sub.gs.colors, DVI_CS_RGB, color);
    sub.gs.color_locked = 1;
  }

  saved_path path = save_path(dc);
  dc->replay_depth += 1;
  fz_try(ctx)
    run_pdf_form(ctx, dc, &sub, pat);
  fz_always(ctx)
  {
    dc->replay_depth -= 1;
    restore_path(ctx, dc, path);
    pop_clips(ctx, dc, sub.gs.clip_depth, st->gs.clip_depth);
  }
  fz_catch(ctx)
    fz_rethrow(ctx);
}

// Fill (or stroke, if stst is not NULL) a path with a pattern
static void
paint_pattern(fz_context *ctx, dvi_context *dc, dvi_state *st, fz_path *path,
              const fz_stroke_state *stst, int even_odd,
              int pattern, const float color[3], float alpha)
{
  if (dc->replay_depth >= 16)
    return;

  // Keep the reference: the content of tiling patterns is a stream
  pdf_obj *pat = pdf_new_indirect(ctx, store_doc(dc), pattern, 0);
  if (!pdf_is_dict(ctx, pat))
  {
    pdf_drop_obj(ctx, pat);
    return;
  }

  fz_device *dev = dc->dev;
  fz_matrix ctm = st->gs.ctm;
  bool clipped = 0, grouped = 0, tiled = 0;
  fz_shade *shade = NULL;
  fz_var(clipped);
  fz_var(grouped);
  fz_var(tiled);
  fz_var(shade);

  fz_try(ctx)
  {
    fz_rect area = fz_bound_path(ctx, path, stst, ctm);
    if (stst)
      fz_clip_stroke_path(ctx, dev, path, stst, ctm, fz_infinite_rect);
    else
      fz_clip_path(ctx, dev, path, even_odd, ctm, fz_infinite_rect);
    clipped = 1;

    if (alpha < 1)
    {
      fz_begin_group(ctx, dev, area, NULL, 0, 0, FZ_BLEND_NORMAL, alpha);
      grouped = 1;
    }

    // Patterns are expressed in the default coordinate space of the content
    fz_matrix ptm = fz_concat(pdf_dict_get_matrix(ctx, pat, PDF_NAME(Matrix)), st->base_ctm);

    if (pdf_dict_get_int(ctx, pat, PDF_NAME(PatternType)) == 2)
    {
      shade = pdf_load_shading(ctx, store_doc(dc), pdf_dict_get(ctx, pat, PDF_NAME(Shading)));
      fz_fill_shade(ctx, dev, shade, ptm, 1, color_params);
    }
    else
    {
      fz_rect bbox = pdf_dict_get_rect(ctx, pat, PDF_NAME(BBox));
      float xstep = fabsf(pdf_dict_get_real(ctx, pat, PDF_NAME(XStep)));
      float ystep = fabsf(pdf_dict_get_real(ctx, pat, PDF_NAME(YStep)));
      bool uncolored = pdf_dict_get_int(ctx, pat, PDF_NAME(PaintType)) == 2;
      fz_rect parea = fz_transform_rect(area, fz_invert_matrix(ptm));

      if (xstep > 0 && ystep > 0 && !fz_is_empty_rect(bbox))
      {
        fz_begin_tile_id(ctx, dev, parea, bbox, xstep, ystep, ptm, 0);
        tiled = 1;
        run_pattern_cell(ctx, dc, st, pat, ptm, uncolored ? color : NULL);
      }
    }
  }
  fz_always(ctx)
  {
    fz_drop_shade(ctx, shade);
    if (tiled)
      fz_end_tile(ctx, dev);
    if (grouped)
      fz_end_group(ctx, dev);
    if (clipped)
      fz_pop_clip(ctx, dev);
    pdf_drop_obj(ctx, pat);
  }
  fz_catch(ctx)
    fz_rethrow(ctx);
}

static void fill_path(fz_context *ctx, dvi_context *dc, dvi_state *st, fz_path *path, int even_odd)
{
  dvi_graphicstate *gs = &st->gs;
  if (gs->colors.fill_pattern)
    paint_pattern(ctx, dc, st, path, NULL, even_odd, gs->colors.fill_pattern,
                  gs->colors.fill, gs->fill_alpha);
  else
    fz_fill_path(ctx, dc->dev, path, even_odd, gs->ctm, device_cs(ctx),
                 gs->colors.fill, gs->fill_alpha, color_params);
}

static void stroke_path(fz_context *ctx, dvi_context *dc, dvi_state *st, fz_path *path)
{
  dvi_graphicstate *gs = &st->gs;
  fz_stroke_state *stst = new_stroke_state(ctx, gs);
  fz_try(ctx)
  {
    if (gs->colors.line_pattern)
      paint_pattern(ctx, dc, st, path, stst, 0, gs->colors.line_pattern,
                    gs->colors.line, gs->line_alpha);
    else
      fz_stroke_path(ctx, dc->dev, path, stst, gs->ctm, device_cs(ctx),
                     gs->colors.line, gs->line_alpha, color_params);
  }
  fz_always(ctx)
    fz_drop_stroke_state(ctx, stst);
  fz_catch(ctx)
    fz_rethrow(ctx);
}

enum paint_op
{
  PAINT_NONE = 0,
  PAINT_CLOSE = 1,
  PAINT_FILL = 2,
  PAINT_EVEN_ODD = 4,
  PAINT_STROKE = 8,
};

// Finish the current path: paint it, then apply pending clipping
static void paint(fz_context *ctx, dvi_context *dc, dvi_state *st, int op)
{
  fz_path *path = get_path(ctx, dc);
  if (op & PAINT_CLOSE)
    fz_closepath(ctx, path);

  if (dc->dev && (op & (PAINT_FILL | PAINT_STROKE)))
  {
    bool blend = st->gs.blend_mode != FZ_BLEND_NORMAL;
    if (blend)
      fz_begin_group(ctx, dc->dev, fz_infinite_rect, NULL, 0, 0, st->gs.blend_mode, 1);
    fz_try(ctx)
    {
      if (op & PAINT_FILL)
        fill_path(ctx, dc, st, path, (op & PAINT_EVEN_ODD) != 0);
      if (op & PAINT_STROKE)
        stroke_path(ctx, dc, st, path);
    }
    fz_always(ctx)
      if (blend)
        fz_end_group(ctx, dc->dev);
    fz_catch(ctx)
      fz_rethrow(ctx);
  }

  if (dc->path_clip && dc->dev)
  {
    fz_clip_path(ctx, dc->dev, path, dc->path_clip == 2, st->gs.ctm, fz_infinite_rect);
    st->gs.clip_depth += 1;
  }

  drop_path(ctx, dc);
}

/***************************/
/* Color spaces & patterns */
/***************************/

static uint8_t device_colorspace(const char *name)
{
  if (!strcmp(name, "DeviceGray") || !strcmp(name, "G") || !strcmp(name, "CalGray"))
    return DVI_CS_GRAY;
  if (!strcmp(name, "DeviceRGB") || !strcmp(name, "RGB") || !strcmp(name, "CalRGB"))
    return DVI_CS_RGB;
  if (!strcmp(name, "DeviceCMYK") || !strcmp(name, "CMYK"))
    return DVI_CS_CMYK;
  if (!strcmp(name, "Pattern"))
    return DVI_CS_PATTERN;
  return 255;
}

static uint8_t
load_colorspace(fz_context *ctx, dvi_context *dc, dvi_state *st, const char *name)
{
  uint8_t cs = device_colorspace(name);
  if (cs != 255)
    return cs;

  pdf_obj *obj = lookup_resource(ctx, dc, st, PDF_NAME(ColorSpace), name);
  if (pdf_is_name(ctx, obj))
    return device_colorspace(pdf_to_name(ctx, obj));

  pdf_obj *family = pdf_array_get(ctx, obj, 0);
  if (pdf_name_eq(ctx, family, PDF_NAME(Pattern)))
  {
    // [/Pattern base]: uncolored patterns
    pdf_obj *base = pdf_array_get(ctx, obj, 1);
    if (!base)
      return DVI_CS_PATTERN;
    switch (device_colorspace(pdf_to_name(ctx, base)))
    {
      case DVI_CS_GRAY: return DVI_CS_PATTERN_GRAY;
      case DVI_CS_CMYK: return DVI_CS_PATTERN_CMYK;
      default: return DVI_CS_PATTERN_RGB;
    }
  }
  if (pdf_name_eq(ctx, family, PDF_NAME(CalRGB)))
    return DVI_CS_RGB;
  if (pdf_name_eq(ctx, family, PDF_NAME(CalGray)))
    return DVI_CS_GRAY;
  if (pdf_name_eq(ctx, family, PDF_NAME(ICCBased)))
  {
    int n = pdf_dict_get_int(ctx, pdf_array_get(ctx, obj, 1), PDF_NAME(N));
    return colorspace_of_components(n);
  }

  if (!obj)
  {
    fprintf(stderr, "pdf: unknown color space %s\n", name);
    return DVI_CS_GRAY;
  }

  // Other color spaces (Separation, DeviceN, Indexed, Lab) are loaded by MuPDF
  fz_colorspace *fcs = pdf_load_colorspace(ctx, obj);
  for (int i = 0; i < dc->colorspaces_count; i++)
  {
    if (dc->colorspaces[i] == fcs)
    {
      fz_drop_colorspace(ctx, fcs);
      return DVI_CS_LOADED + i;
    }
  }
  int max = sizeof(dc->colorspaces) / sizeof(dc->colorspaces[0]);
  if (dc->colorspaces_count == max)
  {
    fz_drop_colorspace(ctx, fcs);
    return DVI_CS_GRAY;
  }
  dc->colorspaces[dc->colorspaces_count] = fcs;
  return DVI_CS_LOADED + dc->colorspaces_count++;
}

// Initial color when selecting a color space
static void colorspace_initial_color(fz_context *ctx, dvi_context *dc, uint8_t cs, float rgb[3])
{
  if (cs >= DVI_CS_LOADED)
  {
    fz_colorspace *fcs = dc->colorspaces[cs - DVI_CS_LOADED];
    float v[FZ_MAX_COLORS] = {0,};
    if (fz_colorspace_is_subtractive(ctx, fcs) || fz_colorspace_is_device_n(ctx, fcs))
      for (int i = 0; i < FZ_MAX_COLORS; i++) v[i] = 1;
    fz_convert_color(ctx, fcs, v, fz_device_rgb(ctx), rgb, NULL, color_params);
    return;
  }
  color_set_gray(rgb, 0);
}

// Set a color from components in color space `cs`
static void
colorspace_set_color(fz_context *ctx, dvi_context *dc, uint8_t cs, const float *v, int n, float rgb[3])
{
  switch (cs)
  {
    case DVI_CS_GRAY: case DVI_CS_PATTERN_GRAY:
      if (n >= 1) color_set_gray(rgb, v[0]);
      break;
    case DVI_CS_RGB: case DVI_CS_PATTERN_RGB:
      if (n >= 3) color_set_rgb(rgb, v[0], v[1], v[2]);
      break;
    case DVI_CS_CMYK: case DVI_CS_PATTERN_CMYK:
      if (n >= 4) color_set_cmyk(ctx, rgb, v[0], v[1], v[2], v[3]);
      break;
    case DVI_CS_PATTERN:
      break;
    default:
    {
      fz_colorspace *fcs = dc->colorspaces[cs - DVI_CS_LOADED];
      if (n >= fz_colorspace_n(ctx, fcs))
        fz_convert_color(ctx, fcs, v, fz_device_rgb(ctx), rgb, NULL, color_params);
    }
  }
}

static int lookup_pattern(fz_context *ctx, dvi_context *dc, dvi_state *st, const char *name)
{
  pdf_obj *pat = lookup_resource(ctx, dc, st, PDF_NAME(Pattern), name);
  if (!pdf_is_indirect(ctx, pat))
  {
    if (dc->dev)
      fprintf(stderr, "pdf: unknown pattern %s\n", name);
    return 0;
  }
  return pdf_to_num(ctx, pat);
}

/******************************/
/* External graphic state     */
/******************************/

static void
apply_softmask(fz_context *ctx, dvi_context *dc, dvi_state *st, pdf_obj *smask)
{
  if (!dc->dev || !pdf_is_dict(ctx, smask))
    return;

  pdf_obj *form = pdf_dict_get(ctx, smask, PDF_NAME(G));
  if (!pdf_is_dict(ctx, form))
    return;

  bool luminosity = !pdf_name_eq(ctx, pdf_dict_get(ctx, smask, PDF_NAME(S)), PDF_NAME(Alpha));
  fz_matrix ctm = fz_concat(pdf_dict_get_matrix(ctx, form, PDF_NAME(Matrix)), st->gs.ctm);
  fz_rect bbox = fz_transform_rect(pdf_dict_get_rect(ctx, form, PDF_NAME(BBox)), ctm);
  float bc[FZ_MAX_COLORS] = {0,};
  pdf_obj *bcobj = pdf_dict_get(ctx, smask, PDF_NAME(BC));
  if (pdf_is_array(ctx, bcobj))
    bc[0] = pdf_array_get_real(ctx, bcobj, 0);

  fz_begin_mask(ctx, dc->dev, bbox, luminosity, fz_device_gray(ctx), bc, color_params);
  fz_try(ctx)
  {
    dvi_state sub = *st;
    dvi_graphicstate_init(&sub.gs, st->gs.ctm);
    sub.gs.clip_depth = st->gs.clip_depth;
    draw_form(ctx, dc, &sub, form, st->gs.ctm, XOBJ_MASK);
  }
  fz_always(ctx)
    fz_end_mask(ctx, dc->dev);
  fz_catch(ctx)
    fz_rethrow(ctx);

  // The mask applies until the graphic state is restored
  st->gs.clip_depth += 1;
}

// The "gs" operator
static void
apply_extgstate(fz_context *ctx, dvi_context *dc, dvi_state *st, const char *name)
{
  pdf_obj *dict = lookup_resource(ctx, dc, st, PDF_NAME(ExtGState), name);
  if (!pdf_is_dict(ctx, dict))
  {
    // When recording a form, resources can be defined after their use
    if (dc->dev)
      fprintf(stderr, "pdf: unknown graphic state %s\n", name);
    return;
  }

  dvi_graphicstate *gs = &st->gs;
  int n = pdf_dict_len(ctx, dict);
  for (int i = 0; i < n; i++)
  {
    pdf_obj *key = pdf_dict_get_key(ctx, dict, i);
    pdf_obj *val = pdf_dict_get_val(ctx, dict, i);

    if (pdf_name_eq(ctx, key, PDF_NAME(LW)))
      gs->line_width = pdf_to_real(ctx, val);
    else if (pdf_name_eq(ctx, key, PDF_NAME(LC)))
      gs->line_caps = pdf_to_int(ctx, val);
    else if (pdf_name_eq(ctx, key, PDF_NAME(LJ)))
      gs->line_join = pdf_to_int(ctx, val);
    else if (pdf_name_eq(ctx, key, PDF_NAME(ML)))
      gs->miter_limit = pdf_to_real(ctx, val);
    else if (pdf_name_eq(ctx, key, PDF_NAME(D)))
    {
      pdf_obj *dash = pdf_array_get(ctx, val, 0);
      int len = pdf_array_len(ctx, dash);
      if (len > 16) len = 16;
      for (int j = 0; j < len; j++)
        gs->dash[j] = pdf_array_get_real(ctx, dash, j);
      gs->dash_len = len;
      gs->dash_phase = pdf_array_get_real(ctx, val, 1);
    }
    else if (pdf_name_eq(ctx, key, PDF_NAME(CA)))
      gs->line_alpha = fz_clamp(pdf_to_real(ctx, val), 0, 1);
    else if (pdf_name_eq(ctx, key, PDF_NAME(ca)))
      gs->fill_alpha = fz_clamp(pdf_to_real(ctx, val), 0, 1);
    else if (pdf_name_eq(ctx, key, PDF_NAME(BM)))
    {
      if (pdf_is_array(ctx, val))
        val = pdf_array_get(ctx, val, 0);
      int mode = fz_lookup_blendmode(pdf_to_name(ctx, val));
      gs->blend_mode = mode < 0 ? FZ_BLEND_NORMAL : mode;
    }
    else if (pdf_name_eq(ctx, key, PDF_NAME(SMask)))
      apply_softmask(ctx, dc, st, val);
  }
}

/****************************/
/* Interpretation of PDF    */
/****************************/

#define MAX_ARGS 32

static int
get_args(fz_context *ctx, vstack *stack, val *args, int max)
{
  val values = vstack_get_values(ctx, stack);
  int n = values.length;
  if (n > max)
    n = max;
  for (int i = 0; i < n; i++)
    args[i] = val_array_get(ctx, stack, values, i);
  return n;
}

// Get exactly n numeric arguments
static void
get_numbers(fz_context *ctx, const char *op, val *args, int argc, float *v, int n)
{
  if (argc != n)
    fz_throw(ctx, FZ_ERROR_SYNTAX, "%s: expecting %d arguments, got %d", op, n, argc);
  for (int i = 0; i < n; i++)
    v[i] = val_number(ctx, args[i]);
}

static bool
pdf_code(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  vstack *stack = vstack_new(ctx);
  bool result = 1;
  fz_var(result);

  if (dc->text)
    dvi_context_flush_text(ctx, dc, st);

  fz_try(ctx)
  {
    enum PDF_OP op;
    while ((op = pdf_parse_command(ctx, stack, &cur, lim)) != PDF_NONE)
    {
      const char *opname = pdf_op_name(op);
      val args[MAX_ARGS];
      float v[MAX_ARGS];
      int argc = get_args(ctx, stack, args, MAX_ARGS);
      dvi_graphicstate *gs = &st->gs;

      switch (op)
      {
        /* Graphic state */

        case PDF_OP_q:
          gs_push(st);
          break;

        case PDF_OP_Q:
          gs_pop(ctx, dc, st);
          break;

        case PDF_OP_cm:
        {
          get_numbers(ctx, opname, args, argc, v, 6);
          fz_matrix mat = fz_make_matrix(v[0], v[1], v[2], v[3], v[4], v[5]);
          gs->ctm = fz_concat(mat, gs->ctm);
          break;
        }

        case PDF_OP_w:
          get_numbers(ctx, opname, args, argc, v, 1);
          gs->line_width = v[0];
          break;

        case PDF_OP_j:
          get_numbers(ctx, opname, args, argc, v, 1);
          gs->line_join = v[0];
          break;

        case PDF_OP_J:
          get_numbers(ctx, opname, args, argc, v, 1);
          gs->line_caps = v[0];
          break;

        case PDF_OP_M:
          get_numbers(ctx, opname, args, argc, v, 1);
          gs->miter_limit = v[0];
          break;

        case PDF_OP_d:
        {
          if (argc != 2)
            fz_throw(ctx, FZ_ERROR_SYNTAX, "d: expecting 2 arguments");
          int len = val_array_length(ctx, stack, args[0]);
          if (len > 16) len = 16;
          for (int i = 0; i < len; ++i)
            gs->dash[i] = val_number(ctx, val_array_get(ctx, stack, args[0], i));
          gs->dash_len = len;
          gs->dash_phase = val_number(ctx, args[1]);
          break;
        }

        case PDF_OP_gs:
          if (argc == 1 && val_is_name(args[0]))
            apply_extgstate(ctx, dc, st, val_string(ctx, stack, args[0]));
          break;

        case PDF_OP_ri:
        case PDF_OP_i:
          break;

        /* Path construction */

        case PDF_OP_m:
          get_numbers(ctx, opname, args, argc, v, 2);
          fz_moveto(ctx, get_path(ctx, dc), v[0], v[1]);
          break;

        case PDF_OP_l:
          get_numbers(ctx, opname, args, argc, v, 2);
          fz_lineto(ctx, get_path(ctx, dc), v[0], v[1]);
          break;

        case PDF_OP_c:
          get_numbers(ctx, opname, args, argc, v, 6);
          fz_curveto(ctx, get_path(ctx, dc), v[0], v[1], v[2], v[3], v[4], v[5]);
          break;

        case PDF_OP_v:
          get_numbers(ctx, opname, args, argc, v, 4);
          fz_curvetov(ctx, get_path(ctx, dc), v[0], v[1], v[2], v[3]);
          break;

        case PDF_OP_y:
          get_numbers(ctx, opname, args, argc, v, 4);
          fz_curvetoy(ctx, get_path(ctx, dc), v[0], v[1], v[2], v[3]);
          break;

        case PDF_OP_h:
          fz_closepath(ctx, get_path(ctx, dc));
          break;

        case PDF_OP_re:
        {
          get_numbers(ctx, opname, args, argc, v, 4);
          fz_path *path = get_path(ctx, dc);
          // Not fz_rectto: the direction matters for the non-zero rule
          fz_moveto(ctx, path, v[0], v[1]);
          fz_lineto(ctx, path, v[0] + v[2], v[1]);
          fz_lineto(ctx, path, v[0] + v[2], v[1] + v[3]);
          fz_lineto(ctx, path, v[0], v[1] + v[3]);
          fz_closepath(ctx, path);
          break;
        }

        /* Path painting */

        case PDF_OP_S:
          paint(ctx, dc, st, PAINT_STROKE);
          break;
        case PDF_OP_s:
          paint(ctx, dc, st, PAINT_CLOSE | PAINT_STROKE);
          break;
        case PDF_OP_f:
        case PDF_OP_F:
          paint(ctx, dc, st, PAINT_FILL);
          break;
        case PDF_OP_f_star:
          paint(ctx, dc, st, PAINT_FILL | PAINT_EVEN_ODD);
          break;
        case PDF_OP_B:
          paint(ctx, dc, st, PAINT_FILL | PAINT_STROKE);
          break;
        case PDF_OP_B_star:
          paint(ctx, dc, st, PAINT_FILL | PAINT_EVEN_ODD | PAINT_STROKE);
          break;
        case PDF_OP_b:
          paint(ctx, dc, st, PAINT_CLOSE | PAINT_FILL | PAINT_STROKE);
          break;
        case PDF_OP_b_star:
          paint(ctx, dc, st, PAINT_CLOSE | PAINT_FILL | PAINT_EVEN_ODD | PAINT_STROKE);
          break;
        case PDF_OP_n:
          paint(ctx, dc, st, PAINT_NONE);
          break;

        /* Clipping, applied by the next painting operator */

        case PDF_OP_W:
          dc->path_clip = 1;
          break;
        case PDF_OP_W_star:
          dc->path_clip = 2;
          break;

        /* Colors */

        case PDF_OP_G:
        case PDF_OP_g:
        case PDF_OP_RG:
        case PDF_OP_rg:
        case PDF_OP_K:
        case PDF_OP_k:
        {
          if (gs->color_locked)
            break;
          int n = (op == PDF_OP_G || op == PDF_OP_g) ? 1 :
                  (op == PDF_OP_RG || op == PDF_OP_rg) ? 3 : 4;
          get_numbers(ctx, opname, args, argc, v, n);
          float rgb[3];
          color_set_components(ctx, rgb, v, n);
          if (op == PDF_OP_G || op == PDF_OP_RG || op == PDF_OP_K)
            colorstate_set_line(&gs->colors, colorspace_of_components(n), rgb);
          else
            colorstate_set_fill(&gs->colors, colorspace_of_components(n), rgb);
          break;
        }

        case PDF_OP_CS:
        case PDF_OP_cs:
        {
          if (gs->color_locked)
            break;
          if (argc != 1 || !val_is_name(args[0]))
            fz_throw(ctx, FZ_ERROR_SYNTAX, "%s: expecting a name", opname);
          uint8_t cs = load_colorspace(ctx, dc, st, val_string(ctx, stack, args[0]));
          float rgb[3];
          colorspace_initial_color(ctx, dc, cs, rgb);
          if (op == PDF_OP_CS)
            colorstate_set_line(&gs->colors, cs, rgb);
          else
            colorstate_set_fill(&gs->colors, cs, rgb);
          break;
        }

        case PDF_OP_SC:
        case PDF_OP_sc:
        case PDF_OP_SCN:
        case PDF_OP_scn:
        {
          if (gs->color_locked)
            break;
          bool line = (op == PDF_OP_SC || op == PDF_OP_SCN);
          uint8_t cs = line ? gs->colors.line_cs : gs->colors.fill_cs;
          int pattern = 0;
          int n = argc;
          if (n > 0 && val_is_name(args[n - 1]))
          {
            pattern = lookup_pattern(ctx, dc, st, val_string(ctx, stack, args[n - 1]));
            n -= 1;
          }
          for (int i = 0; i < n; i++)
            v[i] = val_number(ctx, args[i]);
          float rgb[3];
          memcpy(rgb, line ? gs->colors.line : gs->colors.fill, sizeof(rgb));
          colorspace_set_color(ctx, dc, cs, v, n, rgb);
          if (line)
          {
            colorstate_set_line(&gs->colors, cs, rgb);
            gs->colors.line_pattern = pattern;
          }
          else
          {
            colorstate_set_fill(&gs->colors, cs, rgb);
            gs->colors.fill_pattern = pattern;
          }
          break;
        }

        /* Shadings and XObjects */

        case PDF_OP_sh:
        {
          if (argc != 1 || !val_is_name(args[0]))
            fz_throw(ctx, FZ_ERROR_SYNTAX, "sh: expecting a name");
          if (!dc->dev)
            break;
          const char *name = val_string(ctx, stack, args[0]);
          pdf_obj *obj = lookup_resource(ctx, dc, st, PDF_NAME(Shading), name);
          if (!obj)
          {
            fprintf(stderr, "pdf: unknown shading %s\n", name);
            break;
          }
          fz_shade *shade = pdf_load_shading(ctx, store_doc(dc), obj);
          fz_try(ctx)
          {
            bool blend = gs->blend_mode != FZ_BLEND_NORMAL;
            if (blend)
              fz_begin_group(ctx, dc->dev, fz_infinite_rect, NULL, 0, 0, gs->blend_mode, 1);
            fz_fill_shade(ctx, dc->dev, shade, gs->ctm, gs->fill_alpha, color_params);
            if (blend)
              fz_end_group(ctx, dc->dev);
          }
          fz_always(ctx)
            fz_drop_shade(ctx, shade);
          fz_catch(ctx)
            fz_rethrow(ctx);
          break;
        }

        case PDF_OP_Do:
        {
          if (argc != 1 || !val_is_name(args[0]))
            fz_throw(ctx, FZ_ERROR_SYNTAX, "Do: expecting a name");
          const char *name = val_string(ctx, stack, args[0]);
          pdf_obj *obj = lookup_resource(ctx, dc, st, PDF_NAME(XObject), name);
          if (!obj)
            fprintf(stderr, "pdf: unknown XObject %s\n", name);
          else
            draw_xobject(ctx, dc, st, obj, gs->ctm, NULL);
          break;
        }

        /* Ignored: marked content, compatibility, type 3 fonts */

        case PDF_OP_MP:
        case PDF_OP_DP:
        case PDF_OP_BMC:
        case PDF_OP_BDC:
        case PDF_OP_EMC:
        case PDF_OP_BX:
        case PDF_OP_EX:
        case PDF_OP_d0:
        case PDF_OP_d1:
          break;

        /* Text operators are not supported */

        case PDF_OP_BT:
        case PDF_OP_ET:
        case PDF_OP_Tc:
        case PDF_OP_Tw:
        case PDF_OP_Tz:
        case PDF_OP_TL:
        case PDF_OP_Tf:
        case PDF_OP_Tr:
        case PDF_OP_Ts:
        case PDF_OP_Td:
        case PDF_OP_TD:
        case PDF_OP_Tm:
        case PDF_OP_T_star:
        case PDF_OP_Tj:
        case PDF_OP_TJ:
        case PDF_OP_squote:
        case PDF_OP_dquote:
        default:
          unhandled(opname, cur, lim, 0);
          break;

        case PDF_NONE:
          break;
      }
    }
  }
  fz_always(ctx)
  {
    vstack_free(ctx, stack);
  }
  fz_catch(ctx)
  {
    fprintf(stderr, "pdf code: %s\n", fz_caught_message(ctx));
    result = 0;
  }
  return result;
}

/************************/
/* pdf: specials        */
/************************/

// pdf:btrans [transformation]
static bool
pdf_btrans(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  struct xform_spec xf = xform_spec();
  cur = parse_xform_or_dim(&xf, cur, lim);
  if (!gs_push(st))
    return 0;
  concat_around_current_point(dc, st, xf.ctm);
  if (cur != lim)
    return unhandled("pdf btrans transformation", cur, lim, 0);
  return 1;
}

// pdf:bcontent
static bool
pdf_bcontent(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  if (st->coords_stack.depth >= st->coords_stack.limit || !gs_push(st))
    return 0;
  st->coords_stack.base[st->coords_stack.depth++] = st->origin;
  st->gs.ctm = dvi_get_ctm(dc, st);
  st->origin.h = st->registers.h;
  st->origin.v = st->registers.v;
  return 1;
}

// pdf:econtent
static bool
pdf_econtent(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  if (st->coords_stack.depth == 0)
    return 0;
  st->origin = st->coords_stack.base[--st->coords_stack.depth];
  return gs_pop(ctx, dc, st);
}

// pdf:literal [direct] CODE
static bool
pdf_literal(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  bool direct = 0;
  for (;;)
  {
    cur = skip_ws(cur, lim);
    if (lim - cur >= 6 && memcmp(cur, "direct", 6) == 0)
    {
      direct = 1;
      cur += 6;
    }
    else if (lim - cur >= 7 && memcmp(cur, "reverse", 7) == 0)
      cur += 7;
    else
      break;
  }

  if (direct)
    return pdf_code(ctx, dc, st, cur, lim);

  fz_point cp = current_point(dc, st);
  st->gs.ctm = fz_pre_translate(st->gs.ctm, cp.x, cp.y);
  bool result = pdf_code(ctx, dc, st, cur, lim);
  st->gs.ctm = fz_pre_translate(st->gs.ctm, -cp.x, -cp.y);
  return result;
}

// pdf:content CODE
static bool
pdf_content(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  int depth = st->gs_stack.depth;
  if (!gs_push(st))
    return 0;
  fz_point cp = current_point(dc, st);
  st->gs.ctm = fz_pre_translate(st->gs.ctm, cp.x, cp.y);
  bool result = pdf_code(ctx, dc, st, cur, lim);
  while (st->gs_stack.depth > depth)
    gs_pop(ctx, dc, st);
  return result;
}

// Fill the page with a color
static bool
fill_background(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  float rgb[3];
  uint8_t space;
  if (!parse_colorspec(ctx, cur, lim, rgb, &space))
    return unhandled("background", cur, lim, 0);
  if (!dc->dev)
    return 1;
  fz_path *path = fz_new_path(ctx);
  fz_try(ctx)
  {
    fill_rect_path(ctx, path, fz_make_rect(-1e5, -1e5, 1e5, 1e5));
    fz_fill_path(ctx, dc->dev, path, 0, dc->root.base_ctm, device_cs(ctx),
                 rgb, 1.0, color_params);
  }
  fz_always(ctx)
    fz_drop_path(ctx, path);
  fz_catch(ctx)
    fz_rethrow(ctx);
  return 1;
}

// pdf:bcolor / pdf:scolor COLOR [COLOR]
static bool
pdf_setcolor(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim, bool push)
{
  float fill[3], line[3];
  uint8_t fill_cs, line_cs;
  cur = parse_colorspec(ctx, cur, lim, fill, &fill_cs);
  if (!cur)
    return unhandled("pdf color", cur, lim, 0);
  if (!parse_colorspec(ctx, cur, lim, line, &line_cs))
  {
    memcpy(line, fill, sizeof(line));
    line_cs = fill_cs;
  }
  if (push && !colorstack_push(ctx, dc, st, -1))
    return 0;
  colorstate_set_fill(&st->gs.colors, fill_cs, fill);
  colorstate_set_line(&st->gs.colors, line_cs, line);
  return 1;
}

static bool
dvi_exec_pdf(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t spc, cursor_t cur, cursor_t lim)
{
  cursor_t mar, f0, f1;

  /*!re2c

  "pagesize" ws+ "width" ws+ @f0 dim
             ws+ "height" ws+ @f1 dim
  { return 1; }

  "pagesize" ws+ "default"
  { return 1; }

  "pagesize"
  { return 1; }

  ("image" | "img" | "epdf")
  { return special_image(ctx, dc, st, cur, lim, 1); }

  ("begintransform" | "begintrans" | "btrans" | "bt")
  { return pdf_btrans(ctx, dc, st, cur, lim); }

  ("endtransform" | "endtrans" | "etrans" | "et")
  { return gs_pop(ctx, dc, st); }

  "bcontent"
  { return pdf_bcontent(ctx, dc, st); }

  "econtent"
  { return pdf_econtent(ctx, dc, st); }

  ("begincolor" | "bcolor" | "bc" | "begingray" | "bgray" | "bg")
  { return pdf_setcolor(ctx, dc, st, cur, lim, 1); }

  ("setcolor" | "scolor" | "sc")
  { return pdf_setcolor(ctx, dc, st, cur, lim, 0); }

  ("endcolor" | "ecolor" | "ec" | "endgray" | "egray" | "eg")
  { return colorstack_pop(ctx, dc, st, -1); }

  ("bgcolor" | "bgc" | "bbc" | "bbg")
  { return fill_background(ctx, dc, st, cur, lim); }

  ("code" | "direct" | "page")
  { return pdf_code(ctx, dc, st, cur, lim); }

  "literal"
  { return pdf_literal(ctx, dc, st, cur, lim); }

  "content"
  { return pdf_content(ctx, dc, st, cur, lim); }

  ("object" | "obj")
  {
    special_obj(ctx, dc, cur, lim);
    return 1;
  }

  "put"
  {
    special_put(ctx, dc, st, cur, lim);
    return 1;
  }

  "stream"
  {
    special_stream(ctx, dc, cur, lim);
    return 1;
  }

  ("beginxobj" | "bxobj" | "bform")
  { return form_begin(ctx, dc, st, cur, lim, 1); }

  ("endxobj" | "exobj" | "eform")
  { return form_end(ctx, dc, st, spc, cur, lim); }

  ("usexobj" | "uxobj")
  { return special_uxobj(ctx, dc, st, cur, lim); }

  // Interactive features & metadata: nothing to render
  ("annotation" | "annotate" | "annot" | "ann" |
   "beginann" | "bannot" | "bann" | "endann" | "eannot" | "eann" |
   "xannot" | "extendann" | "xann" |
   "outline" | "out" | "article" | "art" | "bead" | "thread" |
   "destination" | "dest" | "docinfo" | "docview" | "names" |
   "link" | "nolink" | "tounicode" | "mapline" | "mapfile" |
   "majorversion" | "minorversion" | "encrypt" | "pageresources" |
   "trailerid" | "close" | "bop" | "eop" | "bxgstate" | "exgstate" |
   "fstream")
  { return 1; }

  ''
  { return unhandled("pdf special", cur, lim, 0); }

  */
}

// x:... specials of XeTeX (implemented by xdvipdfmx)
static bool
dvi_exec_xetex(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  cursor_t mar, f0, f1;

  /*!re2c

  "gsave"
  { return gs_push(st); }

  "grestore"
  { return gs_pop(ctx, dc, st); }

  "scale" ws+ @f0 float ws+ @f1 float
  {
    concat_around_current_point(dc, st, fz_scale(pfloat(f0, lim), pfloat(f1, lim)));
    return 1;
  }

  "bscale" ws+ @f0 float ws+ @f1 float
  {
    float sx = pfloat(f0, lim), sy = pfloat(f1, lim);
    if (fabsf(sx) < 1e-7 || fabsf(sy) < 1e-7)
      return 0;
    if (dc->bscale_depth < 16)
    {
      dc->bscale[dc->bscale_depth][0] = 1 / sx;
      dc->bscale[dc->bscale_depth][1] = 1 / sy;
    }
    dc->bscale_depth += 1;
    concat_around_current_point(dc, st, fz_scale(sx, sy));
    return 1;
  }

  "escale"
  {
    if (dc->bscale_depth == 0)
      return 0;
    dc->bscale_depth -= 1;
    if (dc->bscale_depth < 16)
      concat_around_current_point(dc, st, fz_scale(dc->bscale[dc->bscale_depth][0],
                                                   dc->bscale[dc->bscale_depth][1]));
    return 1;
  }

  "rotate" ws+ @f0 float
  {
    concat_around_current_point(dc, st, fz_rotate(pfloat(f0, lim)));
    return 1;
  }

  "backgroundcolor"
  { return fill_background(ctx, dc, st, cur, lim); }

  ("fontmapline" | "fontmapfile" | "papersize" | "shadow" | "colorshadow" |
   "renderingmode" | "initoverlay" | "clipoverlay" | "textcolor" |
   "textcolorpush" | "textcolorpop" | "rulecolor" | "rulecolorpush" |
   "rulecolorpop")
  { return 1; }

  ''
  { return unhandled("xetex special", cur, lim, 0); }

  */
}

bool dvi_exec_special(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  cursor_t mar, i, spc = cur;

  // fprintf(stderr, "special: %.*s\n", (int)(lim - cur), cur);

  // Glyphs are accumulated and drawn in batches: draw them before
  // interpreting a special, as it can change the graphic state.
  if (dc->text)
    dvi_context_flush_text(ctx, dc, st);

  bool result = 0;
  fz_try(ctx)
  {
    for (;;)
    {
      /*!re2c

      ws
      { continue; }

      "landscape"
      { result = 1; break; }

      "pdfcolorstack" ws+ @i nat
                      ws+ "current"
      { result = pdfcolorstack_current(ctx, dc, st, pint(i, lim)); break; }

      "pdfcolorstack" ws+ @i nat
                      ws+ "pop"
      { result = colorstack_pop(ctx, dc, st, pint(i, lim)); break; }

      "pdfcolorstack" ws+ @i nat
                      ws+ "push" ws+ "("
      {
        result = colorstack_push(ctx, dc, st, pint(i, lim)) &&
                 parse_pdfcolor(ctx, &st->gs.colors, cur, pdf_string_end(cur, lim));
        break;
      }

      "pdfcolorstack" ws+ @i nat
                      ws+ "set" ws+ "("
      {
        // Replace the top of the stack
        dvi_colorstack *stack = get_colorstack(dc, pint(i, lim));
        result = stack &&
                 parse_pdfcolor(ctx, &st->gs.colors, cur, pdf_string_end(cur, lim));
        if (result && stack->depth == 0)
          stack->origin = st->gs.colors;
        break;
      }

      "pdfcolorstackinit" ws+ @i nat
                          ws+ ("page" ws+)? ("direct" ws+)? "("
      {
        // Usually initialized when scanning previous pages,
        // but the stack may be defined on the page being rendered
        dvi_colorstack *stack = colorstack_init(ctx, dc, pint(i, lim));
        result = stack &&
                 parse_pdfcolor(ctx, &stack->origin, cur, pdf_string_end(cur, lim));
        if (result && stack->depth == 0)
          st->gs.colors = stack->origin;
        break;
      }

      "color" ws+ "pop"
      { result = colorstack_pop(ctx, dc, st, -1); break; }

      "color" ws+ "push" ws+
      {
        result = colorstack_push(ctx, dc, st, -1) &&
                 parse_color(ctx, &st->gs.colors, cur, lim);
        break;
      }

      "color" ws+
      {
        // Set the color and clear the stack
        dc->colorstack.depth = 0;
        result = parse_color(ctx, &st->gs.colors, cur, lim);
        break;
      }

      "background" ws+
      { result = fill_background(ctx, dc, st, cur, lim); break; }

      "x:" ws*
      { result = dvi_exec_xetex(ctx, dc, st, cur, lim); break; }

      "pdf:" ws*
      { result = dvi_exec_pdf(ctx, dc, st, spc, cur, lim); break; }

      // Specials that are not relevant to rendering
      ("papersize=" | "src:" | "pos:" | "om:" | "dvipdfmx:" | "html:" |
       "header=" | "ps:" | "PSfile=" | "\"" | "!")
      { result = 1; break; }

      ''
      { result = unhandled("special", cur, lim, 0); break; }

      */
    }
  }
  fz_catch(ctx)
  {
    fprintf(stderr, "error in special \"%.*s\": %s\n",
            (int)(lim - spc > 80 ? 80 : lim - spc), spc, fz_caught_message(ctx));
    result = 0;
  }
  return result;
}

// Specials with a document-wide effect, interpreted for all the pages
// preceding the page being rendered
static bool
dvi_init_pdf(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t spc, cursor_t cur, cursor_t lim)
{
  cursor_t mar;

  /*!re2c

  ("image" | "img" | "epdf")
  {
    // Only named images matter
    cursor_t p = skip_ws(cur, lim);
    if (p >= lim || *p != '@')
      return 0;
    return special_image(ctx, dc, st, cur, lim, 0);
  }

  ("object" | "obj")
  {
    special_obj(ctx, dc, cur, lim);
    return 1;
  }

  "put"
  {
    special_put(ctx, dc, st, cur, lim);
    return 1;
  }

  "stream"
  {
    special_stream(ctx, dc, cur, lim);
    return 1;
  }

  ("beginxobj" | "bxobj" | "bform")
  { return form_begin(ctx, dc, st, cur, lim, 0); }

  ("endxobj" | "exobj" | "eform")
  { return form_end(ctx, dc, st, spc, cur, lim); }

  ''
  { return 0; }

  */
}

bool dvi_init_special(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  cursor_t i, mar, spc = cur;
  bool result = 0;

  fz_try(ctx)
  {
    for (;;)
    {
      /*!re2c

      ws
      { continue; }

      "pdfcolorstackinit" ws+ @i nat
                          ws+ ("page" ws+)? ("direct" ws+)? "("
      {
        dvi_colorstack *stack = colorstack_init(ctx, dc, pint(i, lim));
        result = stack &&
                 parse_pdfcolor(ctx, &stack->origin, cur, pdf_string_end(cur, lim));
        break;
      }

      "pdf:" ws*
      { result = dvi_init_pdf(ctx, dc, st, spc, cur, lim); break; }

      ''
      { break; }

      */
    }
  }
  fz_catch(ctx)
  {
    fprintf(stderr, "error in special \"%.*s\": %s\n",
            (int)(lim - spc > 80 ? 80 : lim - spc), spc, fz_caught_message(ctx));
    result = 0;
  }
  return result;
}

void dvi_prescan_special(cursor_t cur, cursor_t lim, float *width, float *height, bool *landscape)
{
  cursor_t f0, f1, mar;

  // fprintf(stderr, "prescan: %.*s\n", (int)(lim - cur), cur);

  /*!re2c

  "landscape"
  {
    *landscape = 1;
    return;
  }

  "pdf:" ws* "pagesize" ws+ "width" ws+ @f0 dim
                        ws+ "height" ws+ @f1 dim
  {
    *width = pdim(f0, lim);
    *height = pdim(f1, lim);
    return;
  }

  "pdf:" ws* "pagesize" ws+ "default"
  { *width = 612; *height = 792; return; }

  "papersize=" @f0 dim "," @f1 dim
  {
    *width = pdim(f0, lim);
    *height = pdim(f1, lim);
    return;
  }

  ''
  { return; }

  */
}
