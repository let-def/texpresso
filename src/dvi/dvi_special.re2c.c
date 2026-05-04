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

#include <mupdf/pdf.h>
#include <string.h>
#include "fz_util.h"
#include "mydvi.h"
#include "mydvi_interp.h"
#include "pdf_lexer.h"
#include "vstack.h"
#include <math.h>

#define device_cs fz_device_rgb
#define color_params fz_default_color_params

// Global debug counter — set >0 to enable diagnostic output
int g_debug_ctr = 60;

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
float   = int ("." nat*)?;
ws      = " ";
rawunit = ("pt" | "mm" | "cm" | "in");
unit    = "true"? rawunit;
dim     = float unit;

*/

/*!stags:re2c format = 'static cursor_t @@;\n'; */

static bool
unhandled(const char *kind, cursor_t cur, cursor_t lim, int ignored)
{
  (void)kind; (void)cur; (void)lim; (void)ignored;
  // Don't print warnings for unrecognized specials to keep output clean.
  // Return 1 to prevent aborting page rendering.
  return 1;
}

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

static float
punit(cursor_t cur, cursor_t lim)
{
  if (cur + 2 >= lim)
    return 1.0;

  if (cur + 6 >= lim &&
      cur[0] == 't' && cur[1] == 'r' &&
      cur[2] == 'u' && cur[3] == 'e')
  {
    // TODO: "true" units are not yet supported
    cur += 4;
  }

  char c0 = cur[0];
  char c1 = cur[1];

  if (0);
  else if (c0 == 'p' && c1 == 't')
    return 1.0;
  else if (c0 == 'm' && c1 == 'm')
    return 2.845274;
  else if (c0 == 'c' && c1 == 'm')
    return 28.45274;
  else if (c0 == 'i' && c1 == 'n')
    return 72.27;

  return 1.0;
}

static float
pfloat_or_dim(cursor_t ptr, cursor_t lim, bool is_dim)
{
  int neg = 0;
  double result = 0;
  if (ptr < lim && *ptr == '-')
  {
    neg = 1;
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

  if (is_dim)
    result *= punit(ptr, lim) * 800/803;
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

static void color_set_cmyk(float color[3], float c, float m, float y, float k)
{
  color_set_rgb(
    color,
    (1 - c) * (1 - k),
    (1 - m) * (1 - k),
    (1 - y) * (1 - k)
  );
}

static bool
color_parse_rgb(float color[3], cursor_t r, cursor_t g, cursor_t b, cursor_t lim)
{
  color_set_rgb(color, pfloat(r, lim), pfloat(g, lim), pfloat(b, lim));
  return 1;
}

static bool
color_parse_gray(float color[3], cursor_t gray, cursor_t lim)
{
  color_set_gray(color, pfloat(gray, lim));
  return 1;
}

static bool
color_parse_cmyk(float color[3], cursor_t c, cursor_t m, cursor_t y, cursor_t k, cursor_t lim)
{
  color_set_cmyk(color, pfloat(c, lim), pfloat(m, lim),
                        pfloat(y, lim), pfloat(k, lim));
  return 1;
}

static bool parse_color(dvi_colorstate *state, cursor_t cur, cursor_t lim)
{
  cursor_t mar, f0, f1, f2, f3;
  // fprintf(stderr, "parse_color: %.*s\n", (int)(lim - cur), cur);
  float color[3];

  do
  {
    /*!re2c

    " "
    { continue; }

    "gray" ws+ @f0 float
    {
      color_parse_gray(color, f0, lim);
      break;
    }

    "rgb" ws+ @f0 float
          ws+ @f1 float
          ws+ @f2 float
    {
      color_parse_rgb(color, f0, f1, f2, lim);
      break;
    }

    "cmyk" ws+ @f0 float
           ws+ @f1 float
           ws+ @f2 float
           ws+ @f3 float
    {
      color_parse_cmyk(color, f0, f1, f2, f3, lim);
      break;
    }

    ''
    { return unhandled("color", cur, lim, 0); }

    */
  }
  while (0);

  state->fill[0] = state->line[0] = color[0];
  state->fill[1] = state->line[1] = color[1];
  state->fill[2] = state->line[2] = color[2];
  return 1;
}

static bool parse_pdfcolor(dvi_colorstate *state, cursor_t cur, cursor_t lim)
{
  cursor_t mar, f0, f1, f2, f3;

  for (;;)
  {
    /*!re2c

    ")"
    { return 1; }

    " "
    { continue; }

    @f0 float ws+
    @f1 float ws+
    @f2 float ws+
    @f3 float ws+
    "k"
    { return color_parse_cmyk(state->fill, f0, f1, f2, f3, lim); }

    @f0 float ws+
    @f1 float ws+
    @f2 float ws+
    @f3 float ws+
    "K"
    { return color_parse_cmyk(state->line, f0, f1, f2, f3, lim); }

    @f0 float ws+
    "g"
    { return color_parse_gray(state->fill, f0, lim); }

    @f0 float ws+
    "G"
    { return color_parse_gray(state->line, f0, lim); }

    @f0 float ws+
    @f1 float ws+
    @f2 float ws+
    "rg"
    { return color_parse_rgb(state->fill, f0, f1, f2, lim); }

    @f0 float ws+
    @f1 float ws+
    @f2 float ws+
    "RG"
    { return color_parse_rgb(state->line, f0, f1, f2, lim); }

    ''
    { return unhandled("pdf color", cur, lim, 0); }

    */
  }
}

static bool
pdfcolorstack_current(fz_context *ctx, dvi_context *dc, dvi_state *st, int index)
{
  dvi_context_flush_text(ctx, dc, st);
  if (index >= dc->pdfcolorstacks.capacity)
  {
    fprintf(stderr, "pdfcolorstack_current %d: no such stack\n", index);
    return 0;
  }
  //printf("pdfcolorstack_current %d\n", index);
  dvi_colorstack *stack = index == -1 ? &dc->colorstack : &dc->pdfcolorstacks.stacks[index];
  if (stack->depth == 0)
    st->gs.colors = stack->origin;
  else
    st->gs.colors = stack->base[stack->depth - 1];
  return 1;
}

static bool
colorstack_push(fz_context *ctx, dvi_context *dc, dvi_state *st, int index)
{
  dvi_context_flush_text(ctx, dc, st);
  if (index >= dc->pdfcolorstacks.capacity)
  {
    fprintf(stderr, "pdfcolorstack_push %d: no such stack\n", index);
    return 0;
  }
  //printf("pdfcolorstack_push %d\n", index);
  dvi_colorstack *stack = index == -1 ? &dc->colorstack : &dc->pdfcolorstacks.stacks[index];

  //fprintf(stderr, "pdfcolorstack_push: depth=%d, limit=%d\n", stack->depth, stack->limit);
  if (stack->depth == stack->limit)
  {
    size_t size = stack->limit;
    size = size == 0 ? 16 : size * 2;
    dvi_colorstate *base = fz_malloc_struct_array(ctx, size, dvi_colorstate);
    if (!base) abort();
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
  dvi_context_flush_text(ctx, dc, st);
  if (index >= dc->pdfcolorstacks.capacity)
  {
    fprintf(stderr, "pdfcolorstack_pop %d: no such stack\n", index);
    return 0;
  }
  dvi_colorstack *stack = index == -1 ? &dc->colorstack : &dc->pdfcolorstacks.stacks[index];
  if (stack->depth == 0)
  {
    fprintf(stderr, "pdfcolorstack_pop %d: empty stack\n", index);
    return 0;
  }
  stack->depth -= 1;
  st->gs.colors = stack->base[stack->depth];
  return 1;
}

static bool
colorstack_init(fz_context *ctx, dvi_context *dc, dvi_state *st, int index)
{
  if (index >= dc->pdfcolorstacks.capacity)
  {
    size_t cap = dc->pdfcolorstacks.capacity;
    size_t newcap = cap == 0 ? 4 : cap;
    while (newcap < index) newcap *= 2;
    dvi_colorstack *stacks = fz_malloc_struct_array(ctx, newcap, dvi_colorstack);
    if (!stacks) abort();
    if (cap > 0)
    {
      memcpy(stacks, dc->pdfcolorstacks.stacks, sizeof(dvi_colorstack) * cap);
      fz_free(ctx, dc->pdfcolorstacks.stacks);
    }
    dc->pdfcolorstacks.stacks = stacks;
    dc->pdfcolorstacks.capacity = newcap;
  }
  //printf("pdfcolorstackinit %d\n", index);
  dvi_colorstack *stack = &dc->pdfcolorstacks.stacks[index];
  return 1;
}

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
      .pagebox = BOX_UNDEFINED,
  };
}

static bool
embed_pdf(fz_context *ctx, dvi_context *dc, dvi_state *st, struct xform_spec *xf, const char *filename)
{
  fz_try(ctx)
  {
    pdf_document *doc = dvi_resmanager_get_pdf(ctx, dc->resmanager, filename);
    if (!doc)
      return 0;

    // from mupdf/source/pdf/pdf-page.c: pdf_page_obj_transform
    pdf_page *page = pdf_load_page(ctx, doc, xf->page ? xf->page - 1 : 0);
    pdf_obj *pageobj = page ? page->obj : NULL;

    fz_rect mediabox = pdf_to_rect(ctx, pdf_dict_get_inheritable(ctx, pageobj, PDF_NAME(MediaBox)));
    if (fz_is_empty_rect(mediabox))
    {
      mediabox.x0 = 0;
      mediabox.y0 = 0;
      mediabox.x1 = 612;
      mediabox.y1 = 792;
    }

    fz_rect cropbox = pdf_to_rect(ctx, pdf_dict_get_inheritable(ctx, pageobj, PDF_NAME(CropBox)));
    if (!fz_is_empty_rect(cropbox))
      mediabox = fz_intersect_rect(mediabox, cropbox);

    fz_matrix ctm = fz_flip_vertically(dvi_get_ctm(dc, st));
    ctm = fz_concat(xf->ctm, ctm);
    ctm = fz_pre_translate(ctm, 0, mediabox.y0 - mediabox.y1);
    pdf_run_page(ctx, page, dc->dev, ctm, NULL);
  }
  fz_catch(ctx)
  {
    return 0;
  }
  return 1;
}

static bool
embed_image(fz_context *ctx, dvi_context *dc, dvi_state *st, struct xform_spec *xf, const char *filename)
{

  fz_image *img = NULL;
  fz_var(img);

  fz_try(ctx)
  {
    fz_image *img = dvi_resmanager_get_img(ctx, dc->resmanager, filename);
    fz_matrix ctm = fz_concat(xf->ctm, dvi_get_ctm(dc, st));
    // int xres, yres;
    // fz_image_resolution(img, &xres, &yres);
    // fprintf(stderr, "RENDERING IMAGE, size: %d %d, res: %d %d\n",
    //         img->w, img->h, xres, yres);
    //
    float ar = (float)img->w / (float)img->h;
    float w = xf->width, h = xf->height;
    if (w != w && h != h)
    {
      w = img->w;
      h = img->h;
    }
    else if (w != w)
      w = h * ar;
    else if (h != h)
      h = w / ar;
    ctm = fz_pre_scale(fz_pre_translate(ctm, 0, h), w, -h);
    fz_fill_image(ctx, dc->dev, img, ctm, st->gs.fill_alpha, color_params);
  }
  fz_always(ctx)
  {
    if (img)
      fz_drop_image(ctx, img);
  }
  fz_catch(ctx)
  {
    return 0;
  }
  return 1;
}

static bool
embed_graphics(fz_context *ctx, dvi_context *dc, dvi_state *st, struct xform_spec *xf, const char *filename)
{
  if (!dc->dev)
    return 1;

  const char *ext = filename;

  for (const char *ptr = ext; *ptr; ptr++)
    if (*ptr == '.') ext = ptr + 1;

  if ((ext[0] == 'p' || ext[0] == 'P') &&
      (ext[1] == 'd' || ext[1] == 'D') &&
      (ext[2] == 'f' || ext[2] == 'F'))
    return embed_pdf(ctx, dc, st, xf, filename);
  else
    return embed_image(ctx, dc, st, xf, filename);
}

static cursor_t
parse_xform_or_dim(struct xform_spec *xf, cursor_t cur, cursor_t lim)
{
  //fprintf(stderr, "pdf xform: %.*s\n", (int)(lim - cur), cur);
  cursor_t i, f0, f1, f2, f3, f4, f5, mar;
  float r = 0, sx = 1.0, sy = 1.0;

  while (cur < lim)
  {
    /*!re2c

    " "
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
      xf->bbox.x1 = pfloat(f1, lim);
      xf->bbox.y0 = pfloat(f2, lim);
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
      continue;
    }

    ''
    { break; }

    */
  }

  // Not sure about order in which transformations should be applied...

  if (sx != 1.0 || sy != 1.0)
    xf->ctm = fz_pre_scale(xf->ctm, sx, sy);

  if (r)
    xf->ctm = fz_pre_rotate(xf->ctm, r);

  return cur;
}

static bool
pdf_btrans(dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  if (st->gs_stack.depth >= st->gs_stack.limit)
    return 0;
  st->gs_stack.base[st->gs_stack.depth] = st->gs;
  st->gs_stack.depth += 1;

  st->gs.ctm = dvi_get_ctm(dc, st);
  st->gs.h = st->registers.h;
  st->gs.v = st->registers.v;

  if (cur != lim)
  {
    struct xform_spec xf = xform_spec();
    cur = parse_xform_or_dim(&xf, cur, lim);
    st->gs.ctm = fz_concat(xf.ctm, st->gs.ctm);
  }
  if (cur != lim)
    return unhandled("pdf btrans transformation", cur, lim, 0);
  return 1;
}

static bool
pdf_etrans(dvi_context *dc, dvi_state *st)
{
  if (st->gs_stack.depth == 0)
    return 0;
  st->gs_stack.depth -= 1;
  st->gs = st->gs_stack.base[st->gs_stack.depth];
  return 1;
}

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
}

static void get_stroke_state(fz_context *ctx, dvi_state *st, fz_stroke_state *stst)
{
  *stst = fz_default_stroke_state;
  stst->linewidth = st->gs.line_width;
  stst->linejoin = (int)st->gs.line_join;
  stst->miterlimit = (int)st->gs.miter_limit;
  stst->dash_cap = stst->start_cap = stst->end_cap = (int)st->gs.line_caps;
  stst->dash_len = st->gs.dash_len;
  memcpy(stst->dash_list, st->gs.dash, sizeof(float) * st->gs.dash_len);
  stst->dash_phase = st->gs.dash_phase;
}

// Resolve a font by name for TikZ text rendering.
// Uses a simple cache in dvi_context to avoid reloading fonts.
static fz_font *resolve_special_font(fz_context *ctx, dvi_context *dc, dvi_state *st, const char *name)
{
  if (!name || !*name)
    return NULL;

  // Skip leading '/'
  const char *fname = (*name == '/') ? name + 1 : name;

  // Check cache
  for (int i = 0; i < dc->font_cache_count; i++)
  {
    if (strcmp(dc->font_cache[i].name, fname) == 0)
      return dc->font_cache[i].font;
  }

  // Try MuPDF built-in font
  fz_font *font = NULL;
  fz_try(ctx)
  {
    font = fz_new_font_from_file(ctx, NULL, fname, 0, 0);
  }
  fz_catch(ctx)
  {
    font = NULL;
  }

  // Fallback: try current DVI font
  if (!font)
  {
    dvi_fontdef *def = dvi_fonttable_get(ctx, st->fonts, st->f);
    if (def && def->kind == TEX_FONT && def->tex_font.font && def->tex_font.font->fz)
      font = def->tex_font.font->fz;
  }

  // Cache it
  if (font && dc->font_cache_count < DVI_FONT_CACHE_SIZE)
  {
    size_t len = strlen(fname);
    if (len > 63) len = 63;
    memcpy(dc->font_cache[dc->font_cache_count].name, fname, len);
    dc->font_cache[dc->font_cache_count].name[len] = 0;
    dc->font_cache[dc->font_cache_count].font = font;
    dc->font_cache_count++;
  }

  return font;
}

// Decode a single UTF-8 code point from str at offset *pi.
// Returns the Unicode code point and advances *pi past the consumed bytes.
// Returns 0 and advances by 1 byte on invalid sequence.
static int decode_utf8(const char *str, size_t len, size_t *pi)
{
  size_t i = *pi;
  unsigned char c = (unsigned char)str[i];
  int cp;
  int extra;

  if (c < 0x80) {
    *pi = i + 1;
    return c;
  }
  if (c < 0xC2) goto invalid;
  if (c < 0xE0)      { cp = c & 0x1F; extra = 1; }
  else if (c < 0xF0) { cp = c & 0x0F; extra = 2; }
  else if (c < 0xF8) { cp = c & 0x07; extra = 3; }
  else goto invalid;

  *pi = i + 1 + extra;
  if (i + extra >= len) return 0;
  for (int j = 0; j < extra; j++) {
    unsigned char nb = (unsigned char)str[i + 1 + j];
    if ((nb & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (nb & 0x3F);
  }
  return cp;

invalid:
  *pi = i + 1;
  return 0;
}

// Show a text string for Tj operator, advancing text matrix.
static void show_special_text(fz_context *ctx, dvi_context *dc, dvi_state *st,
                               const char *str, size_t len, fz_font *font)
{
  if (!font || !str || !len || !dc->dev)
    return;

  if (!dc->text)
    dc->text = fz_new_text(ctx);
  fz_text *text = dc->text;
  fz_matrix ctm = dvi_get_ctm(dc, st);
  float fs = st->gs.text.font_size;
  float hs = st->gs.text.scale;

  size_t i = 0;
  while (i < len)
  {
    int cp = decode_utf8(str, len, &i);
    if (cp == 0)
      continue;
    int glyph = fz_encode_character(ctx, font, cp);
    if (glyph == 0)
    {
      // For missing glyphs, advance Tm by a default space to avoid overlap
      float tx = fs * 0.25f * hs;
      st->gs.text.Tm = fz_pre_translate(st->gs.text.Tm, tx, 0);
      continue;
    }
    float adv = fz_advance_glyph(ctx, font, glyph, 0);
    // TRM = CTM × Tm × Tfs: font_size scales the glyph only,
    // not the page-level translation.
    fz_matrix trm = fz_pre_scale(fz_identity, fs * hs, fs);  // Tfs
    trm = fz_concat(trm, st->gs.text.Tm);                     // Tfs × Tm
    trm = fz_concat(trm, ctm);                                // (Tfs × Tm) × CTM
    fz_show_glyph(ctx, text, font, trm, glyph, cp, 0, 0, FZ_BIDI_LTR, FZ_LANG_UNSET);
    // Advance text matrix horizontally
    float tx = (adv * fs + st->gs.text.char_space) * hs + st->gs.text.word_space;
    st->gs.text.Tm = fz_pre_translate(st->gs.text.Tm, tx, 0);
  }
}

static bool
pdf_code(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  vstack *stack = vstack_new(ctx);
  fz_var(cur);
  fz_var(stack);

  // fprintf(stderr, "pdf code: %.*s\n", (int)(lim - cur), cur);
  // Diagnostics: log first few PDF code snippets
  static int code_call_ctr = 30;
  if (code_call_ctr > 0) {
    int preview_len = lim - cur;
    if (preview_len > 50) preview_len = 50;
    fprintf(stderr, "DBG pdf_code[%d]: '%.*s'%s  dev=%p  gs_depth=%d\n",
      30-code_call_ctr, preview_len, cur,
      (lim-cur > 50) ? "..." : "", (void*)dc->dev, st->gs_stack.depth);
    code_call_ctr--;
  }
  fz_try(ctx)
  {
    enum PDF_OP op;
    do {
      cursor_t cur0 = cur;
      op = pdf_parse_command(ctx, stack, &cur, lim);
      switch (op)
      {
        case PDF_OP_cm:
        {
          float fmat[6];
          vstack_get_floats(ctx, stack, fmat, 6);
          fz_matrix mat;
          mat.a = fmat[0]; mat.b = fmat[1]; mat.c = fmat[2];
          mat.d = fmat[3]; mat.e = fmat[4]; mat.f = fmat[5];
          if (g_debug_ctr > 0) { fprintf(stderr, "DBG cm: mat=[%.2f %.2f %.2f %.2f %.2f %.2f]\n", mat.a, mat.b, mat.c, mat.d, mat.e, mat.f); g_debug_ctr--; }
          fz_matrix ctm = fz_concat(mat, dvi_get_ctm(dc, st));
          dvi_set_ctm(st, ctm);
          if (g_debug_ctr > 0) { fprintf(stderr, "DBG cm result: gs.ctm=[%.2f %.2f %.2f %.2f %.2f %.2f] gs.h=%d gs.v=%d\n", st->gs.ctm.a, st->gs.ctm.b, st->gs.ctm.c, st->gs.ctm.d, st->gs.ctm.e, st->gs.ctm.f, st->gs.h, st->gs.v); g_debug_ctr--; }
          break;
        }
        case PDF_OP_q:
        {
          if (st->gs_stack.depth >= st->gs_stack.limit)
            fz_throw(ctx, 0, "PDF q: stack overflow");
          st->gs_stack.base[st->gs_stack.depth] = st->gs;
          st->gs_stack.depth += 1;
          break;
        }
        case PDF_OP_Q:
        {
          if (st->gs_stack.depth == 0)
            fz_throw(ctx, 0, "PDF Q: stack underflow");
          st->gs_stack.depth -= 1;

          int clip_depth0 = st->gs.clip_depth;
          st->gs = st->gs_stack.base[st->gs_stack.depth];
          if (dc->dev)
            for (int i = st->gs.clip_depth; i < clip_depth0; ++i)
              fz_pop_clip(ctx, dc->dev);
          break;
        }
        case PDF_OP_G:
        {
          float c[1];
          vstack_get_floats(ctx, stack, c, 1);
          color_set_gray(st->gs.colors.line, c[0]);
          break;
        }
        case PDF_OP_g:
        {
          float c[1];
          vstack_get_floats(ctx, stack, c, 1);
          color_set_gray(st->gs.colors.fill, c[0]);
          break;
        }
        case PDF_OP_RG:
        {
          float c[3];
          vstack_get_floats(ctx, stack, c, 3);
          color_set_rgb(st->gs.colors.line, c[0], c[1], c[2]);
          break;
        }
        case PDF_OP_rg:
        {
          float c[3];
          vstack_get_floats(ctx, stack, c, 3);
          color_set_rgb(st->gs.colors.fill, c[0], c[1], c[2]);
          break;
        }
        case PDF_OP_K:
        {
          float c[4];
          vstack_get_floats(ctx, stack, c, 4);
          color_set_cmyk(st->gs.colors.line, c[0], c[1], c[2], c[3]);
          break;
        }
        case PDF_OP_k:
        {
          float c[4];
          vstack_get_floats(ctx, stack, c, 4);
          color_set_cmyk(st->gs.colors.fill, c[0], c[1], c[2], c[3]);
          break;
        }

        case PDF_OP_w:
        {
          vstack_get_floats(ctx, stack, &st->gs.line_width, 1);
          break;
        }

        case PDF_OP_j:
        {
          float c[1];
          vstack_get_floats(ctx, stack, c, 1);
          st->gs.line_join = c[0];
          break;
        }

        case PDF_OP_J:
        {
          float c[1];
          vstack_get_floats(ctx, stack, c, 1);
          st->gs.line_caps = c[0];
          break;
        }

        case PDF_OP_M:
        {
          float c[1];
          vstack_get_floats(ctx, stack, c, 1);
          st->gs.miter_limit = c[0];
          break;
        }

        case PDF_OP_m:
        {
          float c[2];
          vstack_get_floats(ctx, stack, c, 2);
          fz_moveto(ctx, get_path(ctx, dc), c[0], c[1]);
          break;
        }

        case PDF_OP_l:
        {
          float c[2];
          vstack_get_floats(ctx, stack, c, 2);
          fz_lineto(ctx, get_path(ctx, dc), c[0], c[1]);
          break;
        }

        case PDF_OP_c:
        {
          float c[6];
          vstack_get_floats(ctx, stack, c, 6);
          fz_curveto(ctx, get_path(ctx, dc), c[0], c[1], c[2], c[3], c[4], c[5]);
          break;
        }

        case PDF_OP_b:
          if (g_debug_ctr > 0) { fprintf(stderr, "DBG b (close+fill+stroke): dev=%p\n", (void*)dc->dev); g_debug_ctr--; }
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_stroke_state stst;
            get_stroke_state(ctx, st, &stst);
            fz_path *path = get_path(ctx, dc);
            fz_closepath(ctx, path);
            fz_fill_path(ctx, dc->dev, path, 0, ctm, device_cs(ctx),
                         st->gs.colors.fill, st->gs.fill_alpha, color_params);
            fz_stroke_path(ctx, dc->dev, path, &stst, ctm, device_cs(ctx),
                           st->gs.colors.line, st->gs.stroke_alpha, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_b_star:
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_stroke_state stst;
            get_stroke_state(ctx, st, &stst);
            fz_path *path = get_path(ctx, dc);
            fz_closepath(ctx, path);
            fz_fill_path(ctx, dc->dev, path, 1, ctm, device_cs(ctx),
                         st->gs.colors.fill, st->gs.fill_alpha, color_params);
            fz_stroke_path(ctx, dc->dev, path, &stst, ctm, device_cs(ctx),
                           st->gs.colors.line, st->gs.stroke_alpha, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_B:
          if (g_debug_ctr > 0) { fprintf(stderr, "DBG B (fill+stroke): dev=%p\n", (void*)dc->dev); g_debug_ctr--; }
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_stroke_state stst;
            get_stroke_state(ctx, st, &stst);
            fz_path *path = get_path(ctx, dc);
            fz_fill_path(ctx, dc->dev, path, 0, ctm, device_cs(ctx),
                         st->gs.colors.fill, st->gs.fill_alpha, color_params);
            fz_stroke_path(ctx, dc->dev, path, &stst, ctm, device_cs(ctx),
                           st->gs.colors.line, st->gs.stroke_alpha, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_B_star:
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_stroke_state stst;
            get_stroke_state(ctx, st, &stst);
            fz_path *path = get_path(ctx, dc);
            fz_fill_path(ctx, dc->dev, path, 1, ctm, device_cs(ctx),
                         st->gs.colors.fill, st->gs.fill_alpha, color_params);
            fz_stroke_path(ctx, dc->dev, path, &stst, ctm, device_cs(ctx),
                           st->gs.colors.fill, st->gs.fill_alpha, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_f:
        case PDF_OP_F:
          if (g_debug_ctr > 0) { fprintf(stderr, "DBG FILL: dev=%p line=[%.2f %.2f %.2f]\n", (void*)dc->dev, st->gs.colors.line[0], st->gs.colors.line[1], st->gs.colors.line[2]); g_debug_ctr--; }
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_path *path = get_path(ctx, dc);
            fz_fill_path(ctx, dc->dev, path, 0, ctm, device_cs(ctx),
                         st->gs.colors.fill, st->gs.fill_alpha, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_f_star:
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_path *path = get_path(ctx, dc);
            fz_fill_path(ctx, dc->dev, path, 1, ctm, device_cs(ctx),
                         st->gs.colors.fill, st->gs.fill_alpha, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_S:
          if (g_debug_ctr > 0) { fprintf(stderr, "DBG STROKE: dev=%p fill=[%.2f %.2f %.2f] lw=%.2f\n", (void*)dc->dev, st->gs.colors.fill[0], st->gs.colors.fill[1], st->gs.colors.fill[2], st->gs.line_width); g_debug_ctr--; }
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_stroke_state stst;
            get_stroke_state(ctx, st, &stst);
            fz_path *path = get_path(ctx, dc);
            fz_stroke_path(ctx, dc->dev, path, &stst, ctm, device_cs(ctx),
                           st->gs.colors.line, st->gs.stroke_alpha, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_s:
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_stroke_state stst;
            get_stroke_state(ctx, st, &stst);
            fz_path *path = get_path(ctx, dc);
            fz_closepath(ctx, path);
            fz_stroke_path(ctx, dc->dev, path, &stst, ctm, device_cs(ctx),
                           st->gs.colors.line, st->gs.stroke_alpha, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_h:
        {
          fz_path *path = get_path(ctx, dc);
          fz_closepath(ctx, path);
          break;
        }

        case PDF_OP_re:
        {
          float c[4];
          vstack_get_floats(ctx, stack, c, 4);
          fz_rectto(ctx, get_path(ctx, dc), c[0], c[1], c[0] + c[2], c[1] + c[3]);
          break;
        }

        case PDF_OP_n:
        {
          if (g_debug_ctr > 0) { fprintf(stderr, "DBG n (drop path)\n"); g_debug_ctr--; }
          drop_path(ctx, dc);
          break;
        }

        case PDF_OP_W:
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_clip_path(ctx, dc->dev, get_path(ctx, dc), 0, ctm,
                         fz_infinite_rect);
            st->gs.clip_depth += 1;
          }
          break;

        case PDF_OP_W_star:
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_clip_path(ctx, dc->dev, get_path(ctx, dc), 1, ctm,
                         fz_infinite_rect);
            st->gs.clip_depth += 1;
          }
          break;

        case PDF_OP_d:
        {
          val v[2];
          vstack_get_arguments(ctx, stack, v, 2);
          st->gs.dash_len = val_array_length(ctx, stack, v[0]);
          if (st->gs.dash_len > 32) st->gs.dash_len = 32;
          for (int i = 0; i < st->gs.dash_len; ++i)
            st->gs.dash[i] = val_number(ctx, val_array_get(ctx, stack, v[0], i));
          st->gs.dash_phase = val_number(ctx, v[1]);
          break;
        }

        case PDF_OP_v:
        {
          fz_path *path = get_path(ctx, dc);
          float c[4];
          fz_point pt = fz_currentpoint(ctx, path);
          vstack_get_floats(ctx, stack, c, 4);
          fz_curveto(ctx, path, pt.x, pt.y, c[0], c[1], c[2], c[3]);
          break;
        }
        case PDF_OP_y:
        {
          fz_path *path = get_path(ctx, dc);
          float c[4];
          vstack_get_floats(ctx, stack, c, 4);
          fz_curveto(ctx, path, c[0], c[1], c[2], c[3], c[2], c[3]);
          break;
        }
        // -- Text operators (Phase 1) --
        case PDF_OP_BT:
        {
          // PDF defaults for text state (Table 5.3 in PDF 1.7 spec)
          st->gs.text.Tm = fz_identity;
          st->gs.text.Tlm = fz_identity;
          st->gs.text.char_space = 0;   // Tc
          st->gs.text.word_space = 0;   // Tw
          st->gs.text.scale = 1.0f;     // Tz (100% = no scaling)
          st->gs.text.leading = 0;      // TL
          st->gs.text.render = 0;       // Tr (fill)
          st->gs.text.rise = 0;         // Ts
          st->gs.text.in_text = 1;
          break;
        }
        case PDF_OP_ET:
        {
          st->gs.text.in_text = 0;
          dvi_context_flush_text(ctx, dc, st);
          break;
        }
        case PDF_OP_Tc:
        {
          float c[1];
          vstack_get_floats(ctx, stack, c, 1);
          st->gs.text.char_space = c[0];
          break;
        }
        case PDF_OP_Tw:
        {
          float c[1];
          vstack_get_floats(ctx, stack, c, 1);
          st->gs.text.word_space = c[0];
          break;
        }
        case PDF_OP_Tz:
        {
          float c[1];
          vstack_get_floats(ctx, stack, c, 1);
          st->gs.text.scale = c[0] / 100.0f;
          break;
        }
        case PDF_OP_TL:
        {
          float c[1];
          vstack_get_floats(ctx, stack, c, 1);
          st->gs.text.leading = c[0];
          break;
        }
        case PDF_OP_Tf:
        {
          val v[2];
          vstack_get_arguments(ctx, stack, v, 2);
          const char *name = val_as_name(ctx, stack, v[0]);
          float size = val_number(ctx, v[1]);
          st->gs.text.font_size = size;
          if (name)
          {
            size_t len = strlen(name);
            if (len > 63) len = 63;
            memcpy(st->gs.text.font_name, name, len);
            st->gs.text.font_name[len] = 0;
          }
          break;
        }
        case PDF_OP_Tr:
        {
          float c[1];
          vstack_get_floats(ctx, stack, c, 1);
          st->gs.text.render = (int)c[0];
          break;
        }
        case PDF_OP_Ts:
        {
          float c[1];
          vstack_get_floats(ctx, stack, c, 1);
          st->gs.text.rise = c[0];
          break;
        }
        case PDF_OP_Td:
        {
          float c[2];
          vstack_get_floats(ctx, stack, c, 2);
          fz_matrix delta = fz_translate(c[0], c[1]);
          st->gs.text.Tlm = fz_concat(delta, st->gs.text.Tlm);
          st->gs.text.Tm = st->gs.text.Tlm;
          break;
        }
        case PDF_OP_TD:
        {
          float c[2];
          vstack_get_floats(ctx, stack, c, 2);
          st->gs.text.leading = -c[1];
          fz_matrix delta = fz_translate(c[0], c[1]);
          st->gs.text.Tlm = fz_concat(delta, st->gs.text.Tlm);
          st->gs.text.Tm = st->gs.text.Tlm;
          break;
        }
        case PDF_OP_Tm:
        {
          float fmat[6];
          vstack_get_floats(ctx, stack, fmat, 6);
          st->gs.text.Tlm.a = fmat[0]; st->gs.text.Tlm.b = fmat[1];
          st->gs.text.Tlm.c = fmat[2]; st->gs.text.Tlm.d = fmat[3];
          st->gs.text.Tlm.e = fmat[4]; st->gs.text.Tlm.f = fmat[5];
          st->gs.text.Tm = st->gs.text.Tlm;
          break;
        }
        case PDF_OP_T_star:
        {
          fz_matrix delta = fz_translate(0, -st->gs.text.leading);
          st->gs.text.Tlm = fz_concat(delta, st->gs.text.Tlm);
          st->gs.text.Tm = st->gs.text.Tlm;
          break;
        }
        case PDF_OP_Tj:
        {
          val v[1];
          vstack_get_arguments(ctx, stack, v, 1);
          const char *str = val_as_string(ctx, stack, v[0]);
          size_t slen = val_string_length(ctx, stack, v[0]);
          fz_font *font = resolve_special_font(ctx, dc, st, st->gs.text.font_name);
          show_special_text(ctx, dc, st, str, slen, font);
          break;
        }
        case PDF_OP_TJ:
        {
          val v[1];
          vstack_get_arguments(ctx, stack, v, 1);
          int alen = val_array_length(ctx, stack, v[0]);
          fz_font *font = resolve_special_font(ctx, dc, st, st->gs.text.font_name);
          for (int i = 0; i < alen; i++)
          {
            val elem = val_array_get(ctx, stack, v[0], i);
            if (val_is_string(elem))
            {
              const char *str = val_string(ctx, stack, elem);
              size_t slen = val_string_length(ctx, stack, elem);
              show_special_text(ctx, dc, st, str, slen, font);
            }
            else if (val_is_number(elem))
            {
              // Kerning adjustment: move text matrix by -kern/1000 * font_size
              float kern = val_number(ctx, elem);
              float tx = -kern / 1000.0f * st->gs.text.font_size * st->gs.text.scale;
              st->gs.text.Tm = fz_pre_translate(st->gs.text.Tm, tx, 0);
            }
          }
          break;
        }
        case PDF_OP_squote:
        {
          // ' = T* + Tj
          fz_matrix delta = fz_translate(0, -st->gs.text.leading);
          st->gs.text.Tlm = fz_concat(delta, st->gs.text.Tlm);
          st->gs.text.Tm = st->gs.text.Tlm;
          val v[1];
          vstack_get_arguments(ctx, stack, v, 1);
          const char *str = val_as_string(ctx, stack, v[0]);
          size_t slen = val_string_length(ctx, stack, v[0]);
          fz_font *font = resolve_special_font(ctx, dc, st, st->gs.text.font_name);
          show_special_text(ctx, dc, st, str, slen, font);
          break;
        }
        case PDF_OP_dquote:
        {
          // '' = set Tw/Tc + T* + Tj
          float c[2];
          vstack_get_floats(ctx, stack, c, 2);
          st->gs.text.word_space = c[0];
          st->gs.text.char_space = c[1];
          fz_matrix delta = fz_translate(0, -st->gs.text.leading);
          st->gs.text.Tlm = fz_concat(delta, st->gs.text.Tlm);
          st->gs.text.Tm = st->gs.text.Tlm;
          val v[3];
          vstack_get_arguments(ctx, stack, v, 3);
          const char *str = val_as_string(ctx, stack, v[2]);
          size_t slen = val_string_length(ctx, stack, v[2]);
          fz_font *font = resolve_special_font(ctx, dc, st, st->gs.text.font_name);
          show_special_text(ctx, dc, st, str, slen, font);
          break;
        }

        // No-ops: operators without visual effect.
        // IMPORTANT: must consume operands from the stack to prevent corruption.
        case PDF_OP_MP:
        {
          val v[1];
          vstack_get_arguments(ctx, stack, v, 1); // tag name
          break;
        }
        case PDF_OP_DP:
        {
          val v[2];
          vstack_get_arguments(ctx, stack, v, 2); // tag name + properties
          break;
        }
        case PDF_OP_BMC:
        {
          val v[1];
          vstack_get_arguments(ctx, stack, v, 1); // tag name
          break;
        }
        case PDF_OP_BDC:
        {
          // BDC has 1 or 2 operands (tag + optional property dict)
          // Try popping 2; if the second isn't a dict, it's fine
          val v[2];
          vstack_get_arguments(ctx, stack, v, 2); // tag name + properties
          break;
        }
        case PDF_OP_EMC:
          // EMC: 0 operands
          break;

        case PDF_OP_BX:
        case PDF_OP_EX:
          // Compatibility operators: 0 operands
          break;

        case PDF_OP_ri:
        {
          val v[1];
          vstack_get_arguments(ctx, stack, v, 1); // rendering intent name
          break;
        }
        case PDF_OP_i:
        {
          float c[1];
          vstack_get_floats(ctx, stack, c, 1); // flatness value
          break;
        }

        case PDF_OP_d0:
        {
          float c[2];
          vstack_get_floats(ctx, stack, c, 2); // wx wy
          break;
        }
        case PDF_OP_d1:
        {
          float c[6];
          vstack_get_floats(ctx, stack, c, 6); // wx wy llx lly urx ury
          break;
        }

        // Color space operators: TikZ primarily uses direct color operators
        // (rg/RG/g/G/k/K) which are already supported.
        case PDF_OP_CS:
        case PDF_OP_cs:
        {
          val v[1];
          vstack_get_arguments(ctx, stack, v, 1); // color space name
          break;
        }
        case PDF_OP_SC:
        {
          float c[3];
          vstack_get_floats(ctx, stack, c, 3);
          color_set_rgb(st->gs.colors.line, c[0], c[1], c[2]);
          break;
        }
        case PDF_OP_sc:
        {
          float c[3];
          vstack_get_floats(ctx, stack, c, 3);
          color_set_rgb(st->gs.colors.fill, c[0], c[1], c[2]);
          break;
        }
        case PDF_OP_SCN:
        {
          float c[3];
          vstack_get_floats(ctx, stack, c, 3);
          color_set_rgb(st->gs.colors.line, c[0], c[1], c[2]);
          break;
        }
        case PDF_OP_scn:
        {
          float c[3];
          vstack_get_floats(ctx, stack, c, 3);
          color_set_rgb(st->gs.colors.fill, c[0], c[1], c[2]);
          break;
        }

        // Still pending implementation: consume operands to prevent stack leak
        case PDF_OP_gs:
        {
          val v[1];
          vstack_get_arguments(ctx, stack, v, 1); // ExtGState name
          break;
        }
        case PDF_OP_sh:
        {
          val v[1];
          vstack_get_arguments(ctx, stack, v, 1); // shading name
          break;
        }
        case PDF_OP_Do:
        {
          val v[1];
          vstack_get_arguments(ctx, stack, v, 1); // XObject name
          break;
        }
        default:
          fprintf(stderr, "pdf unhandled op %s in:\n%.*s\n", pdf_op_name(op),
                  (int)(cur - cur0), cur0);
          break;

        case PDF_NONE:
          break;
      }
    } while (op != PDF_NONE);
  }
  fz_always(ctx)
  {
    vstack_free(ctx, stack);
  }
  fz_catch(ctx)
  {
    // Don't abort page rendering on unknown PDF operators
    fprintf(stderr, "unhandled pdf content operator near: \"%.*s\"\n",
            (int)(lim - cur), cur);
    return 1;
  }
  return 1;
}

static cursor_t parse_pdf_string(char *buf, char *end, cursor_t cur, cursor_t lim)
{
  if (buf >= end)
    return cur;

  int nesting = 1;
  end -= 1;
  while (cur < lim)
  {
    if (*cur == '(')
      nesting += 1;
    else if (*cur == ')')
    {
      nesting -= 1;
      if (nesting == 0)
        break;
    }
    else if (*cur == '\\')
    {
      cur += 1;
      if (cur >= lim)
        break;
    }
    if (buf < end)
    {
      *buf = *cur;
      buf += 1;
    }
    cur += 1;
  }
  *buf = 0;
  return cur;
}

#define MAX_SUBFUNC 8

typedef struct {
  float c0[3], c1[3];
} shade_subfunc;

// Evaluate a stitching function at parameter t (0..1).
// nsub sub-functions partition the domain via nsub-1 bounds.
// Each sub-function i has C0[i], C1[i] and does linear interpolation
// across its sub-domain.
static void shade_eval_color(float out[3], int nsub, const shade_subfunc *sf,
                             const float *bounds, float t)
{
  if (t < 0) t = 0; if (t > 1) t = 1;
  if (nsub <= 1) {
    float s = t;
    out[0] = sf[0].c0[0] + s * (sf[0].c1[0] - sf[0].c0[0]);
    out[1] = sf[0].c0[1] + s * (sf[0].c1[1] - sf[0].c0[1]);
    out[2] = sf[0].c0[2] + s * (sf[0].c1[2] - sf[0].c0[2]);
    return;
  }
  // Find which sub-function covers t
  int k = nsub - 1; // default: last segment
  for (int i = 0; i < nsub - 1; i++) {
    if (t < bounds[i]) { k = i; break; }
  }
  // Map t to sub-function's local [0,1]
  float lo = (k == 0) ? 0.0f : bounds[k-1];
  float hi = (k == nsub - 1) ? 1.0f : bounds[k];
  float s = (t - lo) / (hi - lo);
  out[0] = sf[k].c0[0] + s * (sf[k].c1[0] - sf[k].c0[0]);
  out[1] = sf[k].c0[1] + s * (sf[k].c1[1] - sf[k].c0[1]);
  out[2] = sf[k].c0[2] + s * (sf[k].c1[2] - sf[k].c0[2]);
}

static void render_axial_shade_full(fz_context *ctx, dvi_context *dc, dvi_state *st,
    float x0, float y0, float x1, float y1,
    int nsub, const shade_subfunc *sf, const float *bounds,
    fz_path *clip_path);
static void render_radial_shade(fz_context *ctx, dvi_context *dc, dvi_state *st,
    float cx, float cy, float r0, float r1,
    const shade_subfunc *sf, int nsub, const float *bounds, int nbounds);
static void render_axial_shade(fz_context *ctx, dvi_context *dc, dvi_state *st,
    float x0, float y0, float x1, float y1, float c0[3], float c1[3], fz_path *clip_path);

// Parse a PostScript shading pattern dictionary from raw PS content.
// Scans for /Coords [x0 y0 x1 y1], /C0 [r g b], /C1 [r g b],
// and /Bounds for stitching functions.
// Returns true if a shading was found and rendered natively.
static bool try_parse_ps_shading(fz_context *ctx, dvi_context *dc, dvi_state *st,
                                  const char *p, const char *end)
{
  fprintf(stderr, "DBG shade: called, len=%d content=%.500s\n", (int)(end-p), p);
  fprintf(stderr, "DBG shade full[%d]: ", (int)(end-p));
  for (const char *dp = p; dp < end; dp++) fputc(*dp, stderr);
  fprintf(stderr, "\n");
  // Scan for /Coords [x0 y0 x1 y1] (axial, 4 vals) or
  // /Coords [x0 y0 r0 x1 y1 r1] (radial, 6 vals)
  float cx0=0, cy0=0, cx1=0, cy1=0, rad_r0=0, rad_r1=0;
  int ncoords = 0;
  bool has_coords = false;
  {
    const char *s = p;
    while (s + 8 < end) {
      if (memcmp(s, "/Coords", 7) == 0 && (s[7] == ' ' || s[7] == '\n' || s[7] == '[')) {
        s += 7;
        while (s < end && (*s == ' ' || *s == '\n')) s++;
        if (s < end && *s == '[') { s++;
          char tmp[64]; int ti;
          float vals[6]; int vi = 0;
          while (vi < 6 && s < end) {
            while (s < end && (*s == ' ' || *s == '\n')) s++;
            if (s >= end || *s == ']') break;
            const char *ns = s;
            while (ns < end && *ns != ' ' && *ns != '\n' && *ns != ']') ns++;
            ti = ns - s; if (ti > 63) ti = 63;
            memcpy(tmp, s, ti); tmp[ti] = 0;
            vals[vi++] = strtof(tmp, NULL);
            s = ns;
          }
          ncoords = vi;
          if (vi >= 4) {
            cx0=vals[0]; cy0=vals[1]; cx1=vals[2]; cy1=vals[3]; has_coords = true;
          }
          if (vi >= 6) {
            rad_r0 = vals[2];
            rad_r1 = vals[5];
          }
        }
        break;
      }
      s++;
    }
  }
  if (!has_coords) return false;

  // Collect sub-function C0/C1 values in order.
  // Also parse /Bounds array for stitching function domain partition.
  shade_subfunc subs[MAX_SUBFUNC];
  int nsub = 0;
  float bounds[MAX_SUBFUNC - 1];
  int nbounds = 0;
  {
    const char *s = p;
    while (s + 4 < end) {
      bool is_c0 = (memcmp(s, "/C0", 3) == 0 && (s[3] == ' ' || s[3] == '\n' || s[3] == '['));
      bool is_c1 = (memcmp(s, "/C1", 3) == 0 && (s[3] == ' ' || s[3] == '\n' || s[3] == '['));
      if (is_c0 || is_c1) {
        s += 3;
        while (s < end && (*s == ' ' || *s == '\n')) s++;
        if (s < end && *s == '[') { s++;
          float vals[3]; int vi = 0;
          while (vi < 3 && s < end) {
            while (s < end && (*s == ' ' || *s == '\n')) s++;
            if (s >= end || *s == ']') break;
            const char *ns = s;
            while (ns < end && *ns != ' ' && *ns != '\n' && *ns != ']') ns++;
            int ti = ns - s; if (ti > 63) ti = 63;
            char tmp[64]; memcpy(tmp, s, ti); tmp[ti] = 0;
            vals[vi++] = strtof(tmp, NULL);
            s = ns;
          }
          if (vi >= 3) {
            if (is_c0) {
              if (nsub < MAX_SUBFUNC) {
                subs[nsub].c0[0]=vals[0]; subs[nsub].c0[1]=vals[1]; subs[nsub].c0[2]=vals[2];
                subs[nsub].c1[0]=vals[0]; subs[nsub].c1[1]=vals[1]; subs[nsub].c1[2]=vals[2];
                // If this is after the first sub-function, c1 of previous was already set.
                // The previous sub's c1 is either still default or was set by a /C1 entry.
                nsub++;
              }
              fprintf(stderr, "DBG shade: found C0=[%.2f %.2f %.2f] (sub %d)\n", vals[0],vals[1],vals[2], nsub-1);
            } else {
              if (nsub > 0) {
                subs[nsub-1].c1[0]=vals[0]; subs[nsub-1].c1[1]=vals[1]; subs[nsub-1].c1[2]=vals[2];
              }
              fprintf(stderr, "DBG shade: found C1=[%.2f %.2f %.2f] (sub %d)\n", vals[0],vals[1],vals[2], nsub-1);
            }
          }
        }
      } else if (memcmp(s, "/Bounds", 7) == 0 && (s[7]==' ' || s[7]=='\n' || s[7]=='[')) {
        s += 7;
        while (s < end && (*s == ' ' || *s == '\n')) s++;
        if (s < end && *s == '[') { s++;
          while (nbounds < MAX_SUBFUNC - 1 && s < end) {
            while (s < end && (*s == ' ' || *s == '\n')) s++;
            if (s >= end || *s == ']') break;
            const char *ns = s;
            while (ns < end && *ns != ' ' && *ns != '\n' && *ns != ']') ns++;
            int ti = ns - s; if (ti > 63) ti = 63;
            char tmp[64]; memcpy(tmp, s, ti); tmp[ti] = 0;
            bounds[nbounds++] = strtof(tmp, NULL);
            s = ns;
          }
        }
      } else {
        s++;
      }
    }
  }
  if (nsub == 0) { fprintf(stderr, "DBG shade: no sub-functions found\n"); return false; }

  // Normalise bounds from absolute to [0,1] range using the shading domain.
  {
    const char *s = p;
    while (s + 8 < end) {
      if (memcmp(s, "/Domain", 7) == 0 && (s[7]==' ' || s[7]=='\n' || s[7]=='[')) {
        s += 7;
        while (s < end && (*s == ' ' || *s == '\n')) s++;
        if (s < end && *s == '[') { s++;
          float d0=0, d1=100;
          char tmp[64]; int ti;
          const char *ns = s;
          while (ns < end && *ns != ' ' && *ns != '\n' && *ns != ']') ns++;
          ti = ns - s; if (ti > 63) ti = 63;
          memcpy(tmp, s, ti); tmp[ti]=0; d0 = strtof(tmp, NULL);
          s = ns;
          while (s < end && (*s == ' ' || *s == '\n')) s++;
          ns = s;
          while (ns < end && *ns != ' ' && *ns != '\n' && *ns != ']') ns++;
          ti = ns - s; if (ti > 63) ti = 63;
          memcpy(tmp, s, ti); tmp[ti]=0; d1 = strtof(tmp, NULL);
          float dr = (d1 != d0) ? d1 - d0 : 1.0f;
          for (int i = 0; i < nbounds; i++)
            bounds[i] = (bounds[i] - d0) / dr;
        }
        break;
      }
      s++;
    }
  }

  // Determine shading type: look for /ShadingType
  int shade_type = 2; // default axial
  {
    const char *s = p;
    while (s + 13 < end) {
      if (memcmp(s, "/ShadingType", 12) == 0) {
        s += 12;
        while (s < end && (*s == ' ' || *s == '\n')) s++;
        if (s < end && *s >= '0' && *s <= '9') {
          shade_type = *s - '0';
        }
        break;
      }
      s++;
    }
  }

  if (shade_type == 2) {
    fprintf(stderr, "DBG shade: axial coords=[%.2f %.2f %.2f %.2f] nsub=%d nbounds=%d\n",
            cx0, cy0, cx1, cy1, nsub, nbounds);
    for (int i = 0; i < nsub; i++)
      fprintf(stderr, "DBG shade:   sub[%d] c0=[%.2f %.2f %.2f] c1=[%.2f %.2f %.2f]\n",
              i, subs[i].c0[0],subs[i].c0[1],subs[i].c0[2],
              subs[i].c1[0],subs[i].c1[1],subs[i].c1[2]);
    for (int i = 0; i < nbounds; i++)
      fprintf(stderr, "DBG shade:   bound[%d]=%.4f\n", i, bounds[i]);
    {
      fz_matrix ctm = dvi_get_ctm(dc, st);
      fprintf(stderr, "DBG shade: CTM=[%.2f %.2f %.2f %.2f %.2f %.2f]\n",
              ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f);
    }
    fprintf(stderr, "DBG shade: calling render_axial_shade_full now\n");
    render_axial_shade_full(ctx, dc, st, cx0, cy0, cx1, cy1,
                            nsub, subs, nbounds > 0 ? bounds : NULL, get_path(ctx, dc));
    fprintf(stderr, "DBG shade: render_axial_shade_full returned\n");
  } else if (shade_type == 3) {
    fprintf(stderr, "DBG shade: radial nsub=%d\n", nsub);
    for (int i = 0; i < nsub; i++)
      fprintf(stderr, "DBG shade:   sub[%d] c0=[%.2f %.2f %.2f] c1=[%.2f %.2f %.2f]\n",
              i, subs[i].c0[0],subs[i].c0[1],subs[i].c0[2],
              subs[i].c1[0],subs[i].c1[1],subs[i].c1[2]);
    float cx=cx0, cy=cy0, r0=0, r1;
    if (ncoords >= 6) {
      r0 = rad_r0;
      r1 = rad_r1;
    } else {
      r1 = (float)sqrt((cx1-cx0)*(cx1-cx0)+(cy1-cy0)*(cy1-cy0));
    }
    fprintf(stderr, "DBG shade: radial cx=%.1f cy=%.1f r0=%.1f r1=%.1f ncoords=%d\n",
            cx, cy, r0, r1, ncoords);
    if (nsub > 0) {
      render_radial_shade(ctx, dc, st, cx, cy, r0, r1,
                          subs, nsub, bounds, nbounds > 0 ? bounds : NULL);
    }
  } else {
    return false;
  }
  return true;
}

// Simple wrapper for callers with a single C0/C1 pair.
static void render_axial_shade(fz_context *ctx, dvi_context *dc, dvi_state *st,
    float x0, float y0, float x1, float y1,
    float c0[3], float c1[3], fz_path *clip_path)
{
  shade_subfunc sf;
  sf.c0[0]=c0[0]; sf.c0[1]=c0[1]; sf.c0[2]=c0[2];
  sf.c1[0]=c1[0]; sf.c1[1]=c1[1]; sf.c1[2]=c1[2];
  render_axial_shade_full(ctx, dc, st, x0, y0, x1, y1, 1, &sf, NULL, clip_path);
}

// Render axial gradient with proper stitching function support.
static void render_axial_shade_full(fz_context *ctx, dvi_context *dc, dvi_state *st,
    float x0, float y0, float x1, float y1,
    int nsub, const shade_subfunc *sf, const float *bounds,
    fz_path *clip_path)
{
  if (!dc->dev) return;

  fz_matrix ctm = dvi_get_ctm(dc, st);
  fprintf(stderr, "DBG render_axial_full: alpha=%.2f steps=500\n", st->gs.fill_alpha);
  for (int st = 0; st <= 4; st++) {
    float ts = st * 0.25f;
    float cs[3]; shade_eval_color(cs, nsub, sf, bounds, ts);
    fprintf(stderr, "DBG render_axial_full: t=%.2f color=[%.2f %.2f %.2f]\n", ts, cs[0], cs[1], cs[2]);
  }
  int steps = 500;

  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.001f) return;

  float px = -dy / len, py = dx / len;
  float gx = dx / len, gy = dy / len;

  // Perpendicular half-width from path bounds
  float hw = 200.0f;
  if (clip_path) {
    fz_rect r = fz_bound_path(ctx, clip_path, 0, fz_identity);
    if (!fz_is_infinite_rect(r) && !fz_is_empty_rect(r)) {
      float max_dist = 0;
      float corners[4][2] = {{r.x0, r.y0}, {r.x0, r.y1}, {r.x1, r.y0}, {r.x1, r.y1}};
      for (int c = 0; c < 4; c++) {
        float rx = corners[c][0] - x0, ry = corners[c][1] - y0;
        float perp = fabsf(ry * dx - rx * dy) / len;
        if (perp > max_dist) max_dist = perp;
      }
      hw = max_dist + 1.0f;
    }
  }

  float alpha = st->gs.fill_alpha;
  bool use_group = (alpha < 0.999f);
  if (use_group) {
    fz_rect dev_bbox;
    if (clip_path) {
      fz_rect r = fz_bound_path(ctx, clip_path, 0, fz_identity);
      dev_bbox = fz_transform_rect(r, ctm);
    } else {
      float corners[4][2] = {
        {x0 + px * hw, y0 + py * hw}, {x0 - px * hw, y0 - py * hw},
        {x1 + px * hw, y1 + py * hw}, {x1 - px * hw, y1 - py * hw}};
      dev_bbox = fz_empty_rect;
      for (int c = 0; c < 4; c++) {
        fz_point pt = fz_transform_point_xy(corners[c][0], corners[c][1], ctm);
        dev_bbox = fz_include_point_in_rect(dev_bbox, pt);
      }
    }
    fz_begin_group(ctx, dc->dev, dev_bbox, NULL, 1, 0, 0, alpha);
  }

  float step_size = len / steps;
  float overlap = step_size * 0.5f;
  float fill_a = use_group ? 1.0f : alpha;
  for (int i = 0; i < steps; i++)
  {
    float t = (i + 0.5f) / steps;
    float color[3];
    shade_eval_color(color, nsub, sf, bounds, t);

    float tv0 = (float)i / steps;
    float tv1 = (float)(i + 1) / steps;
    float cx = x0 + dx * tv0 - gx * overlap;
    float cy = y0 + dy * tv0 - gy * overlap;
    float nx = x0 + dx * tv1 + gx * overlap;
    float ny = y0 + dy * tv1 + gy * overlap;

    fz_path *path = fz_new_path(ctx);
    fz_moveto(ctx, path, cx + px * hw, cy + py * hw);
    fz_lineto(ctx, path, nx + px * hw, ny + py * hw);
    fz_lineto(ctx, path, nx - px * hw, ny - py * hw);
    fz_lineto(ctx, path, cx - px * hw, cy - py * hw);
    fz_closepath(ctx, path);
    fz_fill_path(ctx, dc->dev, path, 0, ctm, device_cs(ctx),
                 color, fill_a, color_params);
    fz_drop_path(ctx, path);
  }

  if (use_group)
    fz_end_group(ctx, dc->dev);
}

// Render a simple radial gradient natively.
static void render_radial_shade(fz_context *ctx, dvi_context *dc, dvi_state *st,
    float cx, float cy, float r0, float r1,
    const shade_subfunc *sf, int nsub, const float *bounds, int nbounds)
{
  if (!dc->dev) return;

  fz_matrix ctm = dvi_get_ctm(dc, st);
  int steps = 400;
  float r_max = r1 > r0 ? r1 : r0;
  float r_min = r0 < r1 ? r0 : r1;

  float alpha = st->gs.fill_alpha;
  bool use_group = (alpha < 0.999f);
  if (use_group) {
    fz_rect r = {cx - r_max, cy - r_max, cx + r_max, cy + r_max};
    fz_rect dev_bbox = fz_transform_rect(r, ctm);
    fz_begin_group(ctx, dc->dev, dev_bbox, NULL, 1, 0, 0, alpha);
  }
  float fill_a = use_group ? 1.0f : alpha;

  for (int i = 0; i < steps; i++)
  {
    float t_inner = (float)i / steps;
    float t_outer = (float)(i + 1) / steps;
    float ri = r_min + (r_max - r_min) * t_inner;
    float ro = r_min + (r_max - r_min) * t_outer;
    float t = (t_inner + t_outer) * 0.5f;

    float color[3];
    shade_eval_color(color, nsub, sf, bounds, t);

    // Thick ring via even-odd fill
    fz_path *path = fz_new_path(ctx);
    fz_moveto(ctx, path, cx + ro, cy);
    fz_curveto(ctx, path, cx + ro, cy + ro * 0.552f, cx + ro * 0.552f, cy + ro, cx, cy + ro);
    fz_curveto(ctx, path, cx - ro * 0.552f, cy + ro, cx - ro, cy + ro * 0.552f, cx - ro, cy);
    fz_curveto(ctx, path, cx - ro, cy - ro * 0.552f, cx - ro * 0.552f, cy - ro, cx, cy - ro);
    fz_curveto(ctx, path, cx + ro * 0.552f, cy - ro, cx + ro, cy - ro * 0.552f, cx + ro, cy);
    fz_closepath(ctx, path);
    fz_moveto(ctx, path, cx + ri, cy);
    fz_curveto(ctx, path, cx + ri, cy - ri * 0.552f, cx + ri * 0.552f, cy - ri, cx, cy - ri);
    fz_curveto(ctx, path, cx - ri * 0.552f, cy - ri, cx - ri, cy - ri * 0.552f, cx - ri, cy);
    fz_curveto(ctx, path, cx - ri, cy + ri * 0.552f, cx - ri * 0.552f, cy + ri, cx, cy + ri);
    fz_curveto(ctx, path, cx + ri * 0.552f, cy + ri, cx + ri, cy + ri * 0.552f, cx + ri, cy);
    fz_closepath(ctx, path);
    fz_fill_path(ctx, dc->dev, path, 1, ctm, device_cs(ctx),
                 color, fill_a, color_params);
    fz_drop_path(ctx, path);
  }

  if (use_group)
    fz_end_group(ctx, dc->dev);
}

// ---- PS (PostScript) command interpreter for PGF/TikZ ----
// PGF/TikZ outputs PostScript drawing commands via \special{ps:: ...}
// We maintain a small value stack and a function dictionary.

#define PS_STACK_MAX 32
#define PS_FUNC_MAX  16

typedef struct {
  char name[32];
  char body[256];
  int body_len;
} ps_func_def;

static float ps_stack[PS_STACK_MAX];
static int   ps_sp = 0;
static ps_func_def ps_funcs[PS_FUNC_MAX];
static int   ps_func_count = 0;

static void ps_push(float v)   { if (ps_sp < PS_STACK_MAX) ps_stack[ps_sp++] = v; }
static float ps_pop()           { return ps_sp > 0 ? ps_stack[--ps_sp] : 0.0f; }
static int   ps_depth()         { return ps_sp; }
static void  ps_clear()         { ps_sp = 0; }
void ps_state_reset()           { ps_sp = 0; /* preserve ps_func_count across pages */ }

// Forward declaration
static void ps_define_func(const char *name, int nl, const char *body, int bl);
static const char *ps_lookup_func(const char *name);

// Parse PS function definitions from a string without executing any commands.
// Handles both "def" and "bind def". Used to pre-scan "!" /pgf specials.
static void ps_parse_defs(const char *p, const char *end)
{
  while (p < end)
  {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    if (p >= end) break;

    if (*p != '/') {
      // Skip non-definition tokens
      while (p < end && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t' && *p != '/') p++;
      continue;
    }
    p++; // skip '/'
    const char *ns = p;
    while (p < end && *p != '{' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    int nl = p - ns;
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    if (p >= end || *p != '{') continue;
    p++; // skip '{'
    const char *bs = p;
    int depth = 1;
    while (p < end && depth > 0) {
      if (*p == '{') depth++; else if (*p == '}') depth--;
      p++;
    }
    int bl = p - bs - 1;
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    // Handle both "bind def" and "def"
    bool is_bind = false;
    if (p + 4 <= end && memcmp(p, "bind", 4) == 0) {
      is_bind = true;
      p += 4;
      while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    }
    if (p + 3 <= end && memcmp(p, "def", 3) == 0) {
      p += 3;
      if (nl > 0 && bl >= 0) {
        // Always overwrite: with function preservation across pages,
        // we MUST allow ! specials to clear stale per-element colors
        // (pgffc/pgfsc) and redefine library functions after reset.
        ps_define_func(ns, nl, bs, bl);
        fprintf(stderr, "DBG ps_def[!]: /%.*s = %.*s%s\n", nl, ns, bl, bs, is_bind ? " [bind]" : "");
      }
    }
  }
}

static void ps_define_func(const char *name, int nl, const char *body, int bl)
{
  // Overwrite existing definition with the same name
  for (int i = 0; i < ps_func_count; i++) {
    if ((int)strlen(ps_funcs[i].name) == nl && memcmp(ps_funcs[i].name, name, nl) == 0) {
      if (bl > 255) bl = 255;
      memcpy(ps_funcs[i].body, body, bl);
      ps_funcs[i].body[bl] = 0;
      ps_funcs[i].body_len = bl;
      return;
    }
  }
  // New definition
  if (ps_func_count >= PS_FUNC_MAX) return;
  ps_func_def *f = &ps_funcs[ps_func_count++];
  if (nl > 31) nl = 31;  memcpy(f->name, name, nl); f->name[nl] = 0;
  if (bl > 255) bl = 255; memcpy(f->body, body, bl); f->body[bl] = 0;
  f->body_len = bl;
}

static const char *ps_lookup_func(const char *name)
{
  for (int i = 0; i < ps_func_count; i++)
    if (strcmp(ps_funcs[i].name, name) == 0) return ps_funcs[i].body;
  return NULL;
}

enum { PS_COLOR_FILL=0, PS_COLOR_STROKE=1, PS_COLOR_BOTH=2 };
static void ps_exec_body(fz_context *ctx, dvi_context *dc, dvi_state *st,
                          const char *body, int body_len, int color_target);

static bool
ps_code(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  static int ps_call_cnt = 0;
  if (ps_call_cnt < 3) {
    int pl = lim - cur; if (pl > 60) pl = 60;
    fprintf(stderr, "PS_CODE_V2 [%d]: %.*s dev=%p\n", ps_call_cnt, pl, cur, (void*)dc->dev);
    ps_call_cnt++;
  }
  const char *p = cur, *end = lim;

  // Track whether a fill/stroke operation was performed that didn't
  // drop the path.  pgffill leaves the path so that pgfstr can stroke
  // it in a \filldraw.  But when the special ends and no pgfstr
  // followed, we must drop the path ourselves.
  bool rendered = false;

  while (p < end)
  {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    if (p >= end) break;

    // Handle function definition:  /name{body}def
    if (*p == '/')
    {
      p++;
      const char *ns = p;
      while (p < end && *p != '{' && *p != ' ') p++;
      int nl = p - ns;
      while (p < end && *p == ' ') p++;
      if (p < end && *p == '{') {
        p++;
        const char *bs = p;
        int depth = 1;
        while (p < end && depth > 0) {
          if (*p == '{') depth++; else if (*p == '}') depth--;
          p++;
        }
        int bl = p - bs - 1; // without closing }
        while (p < end && *p == ' ') p++;
        // Support both "def" and "bind def"
        bool is_bind = false;
        if (p + 4 <= end && memcmp(p, "bind", 4) == 0) {
          is_bind = true;
          p += 4;
          while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
        }
        if (p + 3 <= end && memcmp(p, "def", 3) == 0) {
          p += 3;
          ps_define_func(ns, nl, bs, bl);
          fprintf(stderr, "DBG ps_def: /%.*s = %.*s%s\n", nl, ns, bl, bs, is_bind ? " [bind]" : "");
          // Immediately execute color function bodies so that colors are
          // set even if later ps_lookup_func fails (e.g. due to corruption).
          if (nl == 5 && memcmp(ns, "pgffc", 5) == 0) {
            ps_exec_body(ctx, dc, st, bs, bl, PS_COLOR_FILL);
            ps_clear();
          } else if (nl == 5 && memcmp(ns, "pgfsc", 5) == 0) {
            ps_exec_body(ctx, dc, st, bs, bl, PS_COLOR_STROKE);
            ps_clear();
            fprintf(stderr, "DBG color_exec: /%.*s fill=[%.2f %.2f %.2f] line=[%.2f %.2f %.2f]\n",
                    nl, ns, st->gs.colors.fill[0], st->gs.colors.fill[1], st->gs.colors.fill[2],
                    st->gs.colors.line[0], st->gs.colors.line[1], st->gs.colors.line[2]);
          }
          continue;
        }
      }
      continue;
    }

    // Parse token
    const char *ts = p;
    while (p < end && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    int tl = p - ts;
    if (tl == 0) continue;

    char tmp[64];
    if (tl > 63) tl = 63;
    memcpy(tmp, ts, tl); tmp[tl] = 0;

    // Try as number (strip [ and ] from PS array syntax like [1.0 or -4.08])
    char *num_start = tmp;
    if (*num_start == '[') num_start++;
    int num_len = strlen(num_start);
    if (num_len > 0 && num_start[num_len-1] == ']') {
      num_start[num_len-1] = 0;
      num_len--;
    }
    char *ep;
    float fv = strtof(num_start, &ep);
    if (ep == num_start + num_len && num_len > 0 && *num_start != '/')
    {
      ps_push(fv);
      continue;
    }

    // --- Command dispatch ---
    // PS array markers (standalone [ and ])
    if (tmp[0] == '[' && tmp[1] == 0) continue;
    if (tmp[0] == ']' && tmp[1] == 0) continue;
    // Path building
    if (strcmp(tmp, "moveto") == 0) {
      if (ps_depth() >= 2) { float y=ps_pop(), x=ps_pop(); fz_moveto(ctx, get_path(ctx,dc), x, y); }
    }
    else if (strcmp(tmp, "lineto") == 0) {
      if (ps_depth() >= 2) { float y=ps_pop(), x=ps_pop(); fz_lineto(ctx, get_path(ctx,dc), x, y); }
    }
    else if (strcmp(tmp, "curveto") == 0) {
      if (ps_depth() >= 6) {
        float y3=ps_pop(),x3=ps_pop(), y2=ps_pop(),x2=ps_pop(), y1=ps_pop(),x1=ps_pop();
        fz_curveto(ctx, get_path(ctx,dc), x1,y1, x2,y2, x3,y3);
      }
    }
    else if (strcmp(tmp, "closepath") == 0) {
      fz_closepath(ctx, get_path(ctx,dc));
    }
    else if (strcmp(tmp, "newpath") == 0) {
      drop_path(ctx, dc);
    }
    // PGF fill / stroke
    // IMPORTANT: fill does NOT drop the path — the same path is
    // often stroked by a subsequent pgfstr. Path is dropped at
    // pgfstr, newpath, or pgfc.
    else if (strcmp(tmp, "pgffill") == 0) {
      const char *b = ps_lookup_func("pgffc");
      if (b && *b) ps_exec_body(ctx, dc, st, b, strlen(b), PS_COLOR_FILL);
      float *fc = st->gs.colors.fill;
      fprintf(stderr, "DBG pgffill: pgffc=%s FILL=[%.2f %.2f %.2f] alpha=%.2f\n",
              b&&*b?b:"(empty)", fc[0], fc[1], fc[2], st->gs.fill_alpha);
      if (dc->dev) {
        fz_matrix ctm = dvi_get_ctm(dc, st);
        fz_fill_path(ctx, dc->dev, get_path(ctx,dc), 0, ctm,
                     device_cs(ctx), fc, st->gs.fill_alpha, color_params);
      }
      rendered = true;
      // Do NOT drop path — pgfstr may follow within the same special
    }
    else if (strcmp(tmp, "pgfstr") == 0) {
      const char *b = ps_lookup_func("pgfsc");
      if (b && *b) ps_exec_body(ctx, dc, st, b, strlen(b), PS_COLOR_STROKE);
      // When pgfsc is empty, keep current line color (set by inline
      // setgray/setrgbcolor/setcmykcolor commands)
      float *lc = st->gs.colors.line;
      fprintf(stderr, "DBG pgfstr: pgfsc=%s LINE=[%.2f %.2f %.2f] lw=%.2f alpha=%.2f\n",
              b&&*b?b:"(empty)", lc[0], lc[1], lc[2], st->gs.line_width, st->gs.stroke_alpha);
      if (dc->dev) {
        fz_matrix ctm = dvi_get_ctm(dc, st);
        fz_stroke_state sst;
        get_stroke_state(ctx, st, &sst);
        fz_stroke_path(ctx, dc->dev, get_path(ctx,dc), &sst, ctm,
                       device_cs(ctx), lc, st->gs.stroke_alpha, color_params);
      }
      drop_path(ctx, dc); // stroke is final, drop path
      rendered = true;
    }
    // Graphics state
    else if (strcmp(tmp, "gsave") == 0 || strcmp(tmp, "save") == 0) {
      if (st->gs_stack.depth < st->gs_stack.limit) {
        st->gs_stack.base[st->gs_stack.depth] = st->gs;
        st->gs_stack.depth += 1;
      }
    }
    else if (strcmp(tmp, "grestore") == 0 || strcmp(tmp, "restore") == 0) {
      if (st->gs_stack.depth > 0) {
        int cd0 = st->gs.clip_depth;
        st->gs_stack.depth -= 1;
        st->gs = st->gs_stack.base[st->gs_stack.depth];
        if (dc->dev) for (int i = st->gs.clip_depth; i < cd0; ++i) fz_pop_clip(ctx, dc->dev);
      }
    }
    // Line width
    else if (strcmp(tmp, "pgfw") == 0 || strcmp(tmp, "setlinewidth") == 0) {
      if (ps_depth() >= 1) st->gs.line_width = ps_pop();
    }
    // Line cap / join
    else if (strcmp(tmp, "setlinecap") == 0) {
      if (ps_depth() >= 1) st->gs.line_caps = (int)ps_pop();
    }
    else if (strcmp(tmp, "setlinejoin") == 0) {
      if (ps_depth() >= 1) st->gs.line_join = (int)ps_pop();
    }
    // Dash pattern
    else if (strcmp(tmp, "setdash") == 0) {
      if (ps_depth() >= 1) {
        // Pop the dash phase (top of stack)
        float ph = ps_pop();
        st->gs.dash_phase = ph;
        // Remaining values on the stack are the dash array
        // (pushed in order by PS interpreter from [a b c ...])
        int n = ps_depth();
        st->gs.dash_len = n;
        if (n > 32) n = 32;
        for (int i = 0; i < n; i++) {
          st->gs.dash[i] = ps_stack[ps_sp - n + i];
        }
        ps_clear(); // consume all array values
      }
    }
    // Inline PS color operators: set the "current color" in PostScript,
    // which affects both fill and stroke. We apply them to both fill
    // and line colors so that subsequent pgfstr/pgffill use the right
    // color even when pgfsc/pgffc are not redefined.
    else if (strcmp(tmp, "setgray") == 0) {
      if (ps_depth() >= 1) {
        float g = ps_pop();
        color_set_gray(st->gs.colors.fill, g);
        color_set_gray(st->gs.colors.line, g);
      }
    }
    else if (strcmp(tmp, "setrgbcolor") == 0) {
      if (ps_depth() >= 3) {
        float b = ps_pop(), g = ps_pop(), r = ps_pop();
        color_set_rgb(st->gs.colors.fill, r, g, b);
        color_set_rgb(st->gs.colors.line, r, g, b);
      }
    }
    else if (strcmp(tmp, "setcmykcolor") == 0) {
      if (ps_depth() >= 4) {
        float k = ps_pop(), y = ps_pop(), m = ps_pop(), c = ps_pop();
        color_set_cmyk(st->gs.colors.fill, c, m, y, k);
        color_set_cmyk(st->gs.colors.line, c, m, y, k);
      }
    }
    // PGF pgfe: default rectangle (height, width, x, y) from PS stack
    // PGF always redefines pgfe for non-rectangle shapes via other PS commands
    else if (strcmp(tmp, "pgfe") == 0) {
      if (ps_depth() >= 4) {
        float y=ps_pop(), x=ps_pop(), w=ps_pop(), h=ps_pop();
        // Args: bottom→top [height, width, x_start, y_start]
        fz_rectto(ctx, get_path(ctx, dc), x, y, x + w, y + h);
      }
    }
    // PS << dictionary start — used by PGF for shading patterns.
    // Parse the following dictionary content for shading parameters
    // and render natively via try_parse_ps_shading.
    else if (strcmp(tmp, "<<") == 0) {
      fprintf(stderr, "DBG shade << handler: dev=%p trying parse\n", (void*)dc->dev);
      if (try_parse_ps_shading(ctx, dc, st, p, end)) {
        drop_path(ctx, dc);
        ps_clear();
        rendered = true;
        break;
      }
    }
    // PS concat: [a b c d e f] concat — concatenate matrix to CTM
    // This is how PGF/dvips driver applies translations and transformations.
    // PS coordinates are relative to the page top-left (Y-down),
    // device coordinates are relative to page bottom-left (Y-up).
    // Conversion: device_y = page_height - 72 - ps_y
    else if (strcmp(tmp, "concat") == 0) {
      if (ps_depth() >= 6) {
        float f=ps_pop(), e=ps_pop(), d=ps_pop(), c=ps_pop(), b=ps_pop(), a=ps_pop();
        fz_matrix mat;
        mat.a = a; mat.b = b; mat.c = c;
        mat.d = d; mat.e = e; mat.f = f;
        // Chain mat to dvi_get_ctm(dc, st) which includes the TeX
        // page position (reg.h/reg.v relative to gs.h/gs.v) applied
        // to the current CTM.  This preserves the TeX positioning
        // that would otherwise be lost when gs.h/gs.v are zeroed.
        fz_matrix ctm_before = dvi_get_ctm(dc, st);
        st->gs.ctm = fz_concat(mat, ctm_before);
        fprintf(stderr, "DBG concat: mat=[%.2f %.2f %.2f %.2f %.2f %.2f] before=[%.2f %.2f %.2f %.2f %.2f %.2f] after=[%.2f %.2f %.2f %.2f %.2f %.2f]\n",
                a,b,c,d,e,f,
                ctm_before.a,ctm_before.b,ctm_before.c,ctm_before.d,ctm_before.e,ctm_before.f,
                st->gs.ctm.a,st->gs.ctm.b,st->gs.ctm.c,st->gs.ctm.d,st->gs.ctm.e,st->gs.ctm.f);
        st->gs.h = st->registers.h;
        st->gs.v = st->registers.v;
        ps_clear();
      }
    }
    // PGF opacity commands
    else if (strcmp(tmp, ".pgfsetfillopacityalpha") == 0) {
      if (ps_depth() >= 1) st->gs.fill_alpha = ps_pop();
    }
    else if (strcmp(tmp, ".pgfsetstrokeopacityalpha") == 0) {
      if (ps_depth() >= 1) st->gs.stroke_alpha = ps_pop();
    }
    // PS stack operations
    else if (strcmp(tmp, "dup") == 0) {
      if (ps_depth() >= 1) { float v = ps_pop(); ps_push(v); ps_push(v); }
    }
    else if (strcmp(tmp, "pop") == 0) {
      ps_pop(); // discard top of stack
    }
    else if (strcmp(tmp, "exch") == 0) {
      if (ps_depth() >= 2) { float a=ps_pop(), b=ps_pop(); ps_push(a); ps_push(b); }
    }
    else if (strcmp(tmp, "neg") == 0) {
      if (ps_depth() >= 1) { float v = ps_pop(); ps_push(-v); }
    }
    // PS currentpoint: push current path point (x,y) onto stack
    else if (strcmp(tmp, "currentpoint") == 0) {
      fz_point pt = fz_currentpoint(ctx, get_path(ctx, dc));
      ps_push(pt.x); ps_push(pt.y);
    }
    // PS translate: tx ty translate — apply to full CTM (incl. TeX position)
    else if (strcmp(tmp, "translate") == 0) {
      if (ps_depth() >= 2) {
        float ty=ps_pop(), tx=ps_pop();
        st->gs.ctm = fz_pre_translate(dvi_get_ctm(dc, st), tx, ty);
        st->gs.h = st->registers.h;
        st->gs.v = st->registers.v;
      }
    }
    // PS scale: sx sy scale — apply to full CTM (incl. TeX position)
    else if (strcmp(tmp, "scale") == 0) {
      if (ps_depth() >= 2) {
        float sy=ps_pop(), sx=ps_pop();
        fz_matrix before = dvi_get_ctm(dc, st);
        st->gs.ctm = fz_pre_scale(before, sx, sy);
        static int scale_dbg = 10;
        if (scale_dbg > 0) {
          fprintf(stderr, "DBG PS scale: sx=%.4f sy=%.4f before=[%.2f %.2f %.2f %.2f %.2f %.2f] after=[%.2f %.2f %.2f %.2f %.2f %.2f] gs.h=%d->%d\n",
                  sx, sy, before.a,before.b,before.c,before.d,before.e,before.f,
                  st->gs.ctm.a,st->gs.ctm.b,st->gs.ctm.c,st->gs.ctm.d,st->gs.ctm.e,st->gs.ctm.f,
                  st->gs.h, st->registers.h);
          scale_dbg--;
        }
        st->gs.h = st->registers.h;
        st->gs.v = st->registers.v;
      }
    }
    // PS rotate: angle rotate — apply to full CTM (incl. TeX position)
    else if (strcmp(tmp, "rotate") == 0) {
      if (ps_depth() >= 1) {
        float angle = ps_pop();
        st->gs.ctm = fz_pre_rotate(dvi_get_ctm(dc, st), angle);
        st->gs.h = st->registers.h;
        st->gs.v = st->registers.v;
      }
    }
    // PGF /a function: pops x,y, does moveto (initializes path start)
    else if (strcmp(tmp, "a") == 0) {
      if (ps_depth() >= 2) {
        float y=ps_pop(), x=ps_pop();
        fz_moveto(ctx, get_path(ctx, dc), x, y);
      }
    }
    else if (strcmp(tmp, "rlineto") == 0) {
      if (ps_depth() >= 2) {
        float y=ps_pop(), x=ps_pop();
        fz_path *p = get_path(ctx, dc);
        fz_point cp = fz_currentpoint(ctx, p);
        fz_lineto(ctx, p, cp.x + x, cp.y + y);
      }
    }
    // PS clip operators: W/W* (PDF names), clip/eoclip (PS names)
    // Also handle {clip} {eoclip} — PS code blocks in PGF mask expression
    else if (strcmp(tmp, "W") == 0 || strcmp(tmp, "W*") == 0 ||
             strcmp(tmp, "clip") == 0 || strcmp(tmp, "eoclip") == 0 ||
             strcmp(tmp, "{clip}") == 0 || strcmp(tmp, "{eoclip}") == 0) {
      int eofill = (strcmp(tmp, "W*") == 0 || strcmp(tmp, "eoclip") == 0 ||
                    strcmp(tmp, "{eoclip}") == 0) ? 1 : 0;
      fprintf(stderr, "DBG %s: dev=%p path=%p clip_depth=%d\n",
              tmp, (void*)dc->dev, (void*)dc->path, st->gs.clip_depth);
      if (dc->dev && dc->path) {
        fz_matrix ctm = dvi_get_ctm(dc, st);
        fz_clip_path(ctx, dc->dev, dc->path, eofill, ctm,
                     fz_infinite_rect);
        st->gs.clip_depth += 1;
      }
      drop_path(ctx, dc);
    }
    else if (strcmp(tmp, "get") == 0 || strcmp(tmp, "ifelse") == 0) {
      // PS dict/array get and conditional — not needed, consume 2 values
      if (ps_depth() >= 2) { ps_pop(); ps_pop(); }
    }
    else if (strcmp(tmp, "/pgfsmaskinplace") == 0) {
      // PGF soft mask placeholder — no-op for now
    }
    // PGF dvips stroke/fill aliases
    else if (strcmp(tmp, "pgfs") == 0 || strcmp(tmp, "pgfS") == 0) {
      // pgfs = PGF stroke (dvips alias for pgfstr)
      const char *b = ps_lookup_func("pgfsc");
      if (b && *b) ps_exec_body(ctx, dc, st, b, strlen(b), PS_COLOR_STROKE);
      // When pgfsc is empty, keep current line color
      if (dc->dev) {
        fz_matrix ctm = dvi_get_ctm(dc, st);
        fz_stroke_state sst;
        get_stroke_state(ctx, st, &sst);
        fz_stroke_path(ctx, dc->dev, get_path(ctx,dc), &sst, ctm,
                       device_cs(ctx), st->gs.colors.line, st->gs.stroke_alpha, color_params);
      }
      drop_path(ctx, dc);
      rendered = true;
    }
    else if (strcmp(tmp, "pgfr") == 0 || strcmp(tmp, "pgfR") == 0) {
      // pgfr = PGF fill (dvips alias for pgffill)
      const char *b = ps_lookup_func("pgffc");
      if (b && *b) ps_exec_body(ctx, dc, st, b, strlen(b), PS_COLOR_FILL);
      if (dc->dev) {
        fz_matrix ctm = dvi_get_ctm(dc, st);
        fz_fill_path(ctx, dc->dev, get_path(ctx,dc), 0, ctm,
                     device_cs(ctx), st->gs.colors.fill, st->gs.fill_alpha, color_params);
      }
      rendered = true;
      // Do NOT drop path — pgfstr may follow within the same special
    }
    // PGF cleanup / end markers
    else if (strcmp(tmp, "pgfc") == 0 || strcmp(tmp, "pgfo") == 0) { /* no-op */ }
    // PGF shading function invocations — intercept for native rendering.
    // These are called via ps:: specials with params on the PS stack.
    // We handle them before the general ps_lookup_func fallback because
    // ps_exec_body cannot execute the shading PS operators natively,
    // which would leave the path filled with the default (black) color.
    else if (strcmp(tmp, "pgfHrgb") == 0 || strcmp(tmp, "pgfVrgb") == 0 ||
             strcmp(tmp, "pgfArgb") == 0) {
      // Axial RGB: sx sy ex ey R1 G1 B1 R2 G2 B2 depth (11 params)
      if (ps_depth() >= 11) {
        float depth = ps_pop();
        float b2=ps_pop(), g2=ps_pop(), r2=ps_pop();
        float b1=ps_pop(), g1=ps_pop(), r1=ps_pop();
        float ey=ps_pop(), ex=ps_pop(), sy=ps_pop(), sx=ps_pop();
        float c0[3] = {r1, g1, b1};
        float c1[3] = {r2, g2, b2};
        (void)depth;
        render_axial_shade(ctx, dc, st, sx, sy, ex, ey, c0, c1, NULL);
        ps_clear();
        rendered = true;
      }
    }
    else if (strcmp(tmp, "pgfHcmyk") == 0 || strcmp(tmp, "pgfVcmyk") == 0 ||
             strcmp(tmp, "pgfAcmyk") == 0) {
      // Axial CMYK: sx sy ex ey C1 M1 Y1 K1 C2 M2 Y2 K2 depth (13 params)
      if (ps_depth() >= 13) {
        float depth = ps_pop();
        float k2=ps_pop(), y2=ps_pop(), m2=ps_pop(), c2_c=ps_pop();
        float k1=ps_pop(), y1=ps_pop(), m1=ps_pop(), c1_c=ps_pop();
        float ey=ps_pop(), ex=ps_pop(), sy=ps_pop(), sx=ps_pop();
        (void)depth;
        float c0[3], c1[3];
        color_set_cmyk(c0, c1_c, m1, y1, k1);
        color_set_cmyk(c1, c2_c, m2, y2, k2);
        render_axial_shade(ctx, dc, st, sx, sy, ex, ey, c0, c1, NULL);
        ps_clear();
        rendered = true;
      }
    }
    else if (strcmp(tmp, "pgfRrgb") == 0 || strcmp(tmp, "pgfR1rgb") == 0) {
      // Radial RGB: sx sy sr ex ey er R1 G1 B1 R2 G2 B2 (12 params)
      if (ps_depth() >= 12) {
        float b2=ps_pop(), g2=ps_pop(), r2=ps_pop();
        float b1=ps_pop(), g1=ps_pop(), r1=ps_pop();
        float er=ps_pop(), ey=ps_pop(), ex=ps_pop();
        float sr=ps_pop(), sy=ps_pop(), sx=ps_pop();
        float c0[3] = {r1, g1, b1};
        float c1[3] = {r2, g2, b2};
        {
        shade_subfunc sf = {{c0[0],c0[1],c0[2]},{c1[0],c1[1],c1[2]}};
        render_radial_shade(ctx, dc, st, sx, sy, sr, er, &sf, 1, NULL, 0);
        }
        ps_clear();
        rendered = true;
      }
    }
    // Try as a user-defined function call
    else {
      const char *b = ps_lookup_func(tmp);
      if (b) {
        ps_exec_body(ctx, dc, st, b, strlen(b), PS_COLOR_BOTH);
        ps_clear(); // user functions must not leak stack values
        rendered = true; // user-defined functions typically render
      }
    }
  }

  // Drop path if a fill/stroke operation left it behind
  // (pgffill keeps the path for a possible pgfstr within the same
  // special; pgfstr already drops it).  Without this, the next
  // drawing operation inherits stale path segments.
  if (rendered && dc->path)
    drop_path(ctx, dc);

  ps_clear();
  return 1;
}

// Execute a stored PS function body (simple token interpreter)
// The stack should already have the function arguments; body consumes them.
// Do NOT save/restore ps_sp — let the body naturally consume and push values.
static void
ps_exec_body(fz_context *ctx, dvi_context *dc, dvi_state *st,
              const char *body, int body_len, int color_target)
{
  const char *p = body, *end = body + body_len;
  bool rendered = false;
  while (p < end)
  {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    if (p >= end) break;

    const char *ts = p;
    while (p < end && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    int tl = p - ts;
    if (tl == 0) continue;

    char tmp[64];
    if (tl > 63) tl = 63;
    memcpy(tmp, ts, tl); tmp[tl] = 0;

    // Strip [ and ] from PS array syntax
    char *num_start = tmp;
    if (*num_start == '[') num_start++;
    int num_len = strlen(num_start);
    if (num_len > 0 && num_start[num_len-1] == ']') {
      num_start[num_len-1] = 0;
      num_len--;
    }
    char *ep;
    float fv = strtof(num_start, &ep);
    if (ep == num_start + num_len && num_len > 0 && *num_start != '/') {
      ps_push(fv);
      continue;
    }

    int ct = color_target;
    // PS array markers
    if (tmp[0] == '[' && tmp[1] == 0) continue;
    if (tmp[0] == ']' && tmp[1] == 0) continue;
    // Same commands as ps_code but without function defs
    if (strcmp(tmp, "setgray") == 0) {
      if (ps_depth() >= 1) { float g=ps_pop();
        if (ct != PS_COLOR_STROKE) color_set_gray(st->gs.colors.fill,g);
        if (ct != PS_COLOR_FILL)   color_set_gray(st->gs.colors.line,g);
      }
    }
    else if (strcmp(tmp, "setrgbcolor") == 0) {
      if (ps_depth() >= 3) { float b=ps_pop(),g=ps_pop(),r=ps_pop();
        if (ct != PS_COLOR_STROKE) color_set_rgb(st->gs.colors.fill,r,g,b);
        if (ct != PS_COLOR_FILL)   color_set_rgb(st->gs.colors.line,r,g,b);
      }
    }
    else if (strcmp(tmp, "setcmykcolor") == 0) {
      if (ps_depth() >= 4) { float k=ps_pop(),y=ps_pop(),m=ps_pop(),c=ps_pop();
        if (ct != PS_COLOR_STROKE) color_set_cmyk(st->gs.colors.fill,c,m,y,k);
        if (ct != PS_COLOR_FILL)   color_set_cmyk(st->gs.colors.line,c,m,y,k);
      }
    }
    else if (strcmp(tmp, "fillopacity") == 0) {
      if (ps_depth() >= 1) st->gs.fill_alpha = ps_pop();
    }
    else if (strcmp(tmp, "strokeopacity") == 0) {
      if (ps_depth() >= 1) st->gs.stroke_alpha = ps_pop();
    }
    else if (strcmp(tmp, "pgfw") == 0 || strcmp(tmp, "setlinewidth") == 0) {
      if (ps_depth() >= 1) st->gs.line_width = ps_pop();
    }
    else if (strcmp(tmp, "setlinecap") == 0) {
      if (ps_depth() >= 1) st->gs.line_caps = (int)ps_pop();
    }
    else if (strcmp(tmp, "setlinejoin") == 0) {
      if (ps_depth() >= 1) st->gs.line_join = (int)ps_pop();
    }
    else if (strcmp(tmp, "setdash") == 0) {
      if (ps_depth() >= 1) {
        float ph = ps_pop();
        st->gs.dash_phase = ph;
        int n = ps_depth();
        st->gs.dash_len = n;
        if (n > 32) n = 32;
        for (int i = 0; i < n; i++)
          st->gs.dash[i] = ps_stack[ps_sp - n + i];
        ps_sp -= n; // only consume the dash array values
      }
    }
    // Path building commands (used in pgf function bodies like pgf1-pgf8)
    else if (strcmp(tmp, "moveto") == 0) {
      if (ps_depth() >= 2) { float y=ps_pop(), x=ps_pop(); fz_moveto(ctx, get_path(ctx,dc), x, y); }
    }
    else if (strcmp(tmp, "lineto") == 0) {
      if (ps_depth() >= 2) { float y=ps_pop(), x=ps_pop(); fz_lineto(ctx, get_path(ctx,dc), x, y); }
    }
    else if (strcmp(tmp, "curveto") == 0) {
      if (ps_depth() >= 6) {
        float y3=ps_pop(),x3=ps_pop(), y2=ps_pop(),x2=ps_pop(), y1=ps_pop(),x1=ps_pop();
        fz_curveto(ctx, get_path(ctx,dc), x1,y1, x2,y2, x3,y3);
      }
    }
    else if (strcmp(tmp, "closepath") == 0) {
      fz_closepath(ctx, get_path(ctx,dc));
    }
    else if (strcmp(tmp, "newpath") == 0) {
      drop_path(ctx, dc);
    }
    // Graphics state (save/restore within function bodies)
    else if (strcmp(tmp, "gsave") == 0 || strcmp(tmp, "save") == 0) {
      if (st->gs_stack.depth < st->gs_stack.limit) {
        st->gs_stack.base[st->gs_stack.depth] = st->gs;
        st->gs_stack.depth += 1;
      }
    }
    else if (strcmp(tmp, "grestore") == 0 || strcmp(tmp, "restore") == 0) {
      if (st->gs_stack.depth > 0) {
        int cd0 = st->gs.clip_depth;
        st->gs_stack.depth -= 1;
        st->gs = st->gs_stack.base[st->gs_stack.depth];
        if (dc->dev) for (int i = st->gs.clip_depth; i < cd0; ++i) fz_pop_clip(ctx, dc->dev);
      }
    }
    // PS clip within function bodies
    else if (strcmp(tmp, "clip") == 0 || strcmp(tmp, "eoclip") == 0 ||
             strcmp(tmp, "W") == 0 || strcmp(tmp, "W*") == 0 ||
             strcmp(tmp, "{clip}") == 0 || strcmp(tmp, "{eoclip}") == 0) {
      int eofill = (strcmp(tmp, "W*") == 0 || strcmp(tmp, "eoclip") == 0 ||
                    strcmp(tmp, "{eoclip}") == 0) ? 1 : 0;
      if (dc->dev && dc->path) {
        fz_matrix ctm = dvi_get_ctm(dc, st);
        fz_clip_path(ctx, dc->dev, dc->path, eofill, ctm, fz_infinite_rect);
        st->gs.clip_depth += 1;
      }
      drop_path(ctx, dc);
    }
    // Stroke/fill within function bodies
    else if (strcmp(tmp, "pgfstr") == 0) {
      if (dc->dev) {
        fz_matrix ctm = dvi_get_ctm(dc, st);
        fz_stroke_state sst;
        get_stroke_state(ctx, st, &sst);
        fz_stroke_path(ctx, dc->dev, get_path(ctx,dc), &sst, ctm,
                       device_cs(ctx), st->gs.colors.line, st->gs.stroke_alpha, color_params);
      }
      drop_path(ctx, dc); // stroke is final
      rendered = true;
    }
    else if (strcmp(tmp, "pgffill") == 0) {
      if (dc->dev) {
        fz_matrix ctm = dvi_get_ctm(dc, st);
        fz_fill_path(ctx, dc->dev, get_path(ctx,dc), 0, ctm,
                     device_cs(ctx), st->gs.colors.fill, st->gs.fill_alpha, color_params);
      }
      rendered = true;
      // Do NOT drop path — pgfstr may follow
    }
    // concat for CTM transforms within function bodies.
    // Chain to dvi_get_ctm so the TeX page position is included,
    // same rationale as the top-level concat handler in ps_code.
    else if (strcmp(tmp, "concat") == 0) {
      if (ps_depth() >= 6) {
        float f=ps_pop(), e=ps_pop(), d=ps_pop(), c=ps_pop(), b=ps_pop(), a=ps_pop();
        fz_matrix mat;
        mat.a = a; mat.b = b; mat.c = c;
        mat.d = d; mat.e = e; mat.f = f;
        st->gs.ctm = fz_concat(mat, dvi_get_ctm(dc, st));
        st->gs.h = st->registers.h;
        st->gs.v = st->registers.v;
        // 6 values already popped, stack is correct
      }
    }
    // PGF opacity within function bodies
    else if (strcmp(tmp, ".pgfsetfillopacityalpha") == 0) {
      if (ps_depth() >= 1) st->gs.fill_alpha = ps_pop();
    }
    else if (strcmp(tmp, ".pgfsetstrokeopacityalpha") == 0) {
      if (ps_depth() >= 1) st->gs.stroke_alpha = ps_pop();
    }
    // PS stack ops within function bodies
    else if (strcmp(tmp, "dup") == 0) {
      if (ps_depth() >= 1) { float v = ps_pop(); ps_push(v); ps_push(v); }
    }
    else if (strcmp(tmp, "pop") == 0) {
      ps_pop();
    }
    else if (strcmp(tmp, "exch") == 0) {
      if (ps_depth() >= 2) { float a=ps_pop(), b=ps_pop(); ps_push(a); ps_push(b); }
    }
    else if (strcmp(tmp, "neg") == 0) {
      if (ps_depth() >= 1) { float v = ps_pop(); ps_push(-v); }
    }
    // PS currentpoint / translate / scale / rotate (used by graphicx)
    else if (strcmp(tmp, "currentpoint") == 0) {
      fz_point pt = fz_currentpoint(ctx, get_path(ctx, dc));
      ps_push(pt.x); ps_push(pt.y);
    }
    else if (strcmp(tmp, "translate") == 0) {
      if (ps_depth() >= 2) {
        float ty=ps_pop(), tx=ps_pop();
        st->gs.ctm = fz_pre_translate(dvi_get_ctm(dc, st), tx, ty);
        st->gs.h = st->registers.h;
        st->gs.v = st->registers.v;
      }
    }
    else if (strcmp(tmp, "scale") == 0) {
      if (ps_depth() >= 2) {
        float sy=ps_pop(), sx=ps_pop();
        st->gs.ctm = fz_pre_scale(dvi_get_ctm(dc, st), sx, sy);
        st->gs.h = st->registers.h;
        st->gs.v = st->registers.v;
      }
    }
    else if (strcmp(tmp, "rotate") == 0) {
      if (ps_depth() >= 1) {
        float angle = ps_pop();
        st->gs.ctm = fz_pre_rotate(dvi_get_ctm(dc, st), angle);
        st->gs.h = st->registers.h;
        st->gs.v = st->registers.v;
      }
    }
    else if (strcmp(tmp, "a") == 0) {
      // PGF /a function: pops x,y, does moveto
      if (ps_depth() >= 2) {
        float y=ps_pop(), x=ps_pop();
        fz_moveto(ctx, get_path(ctx, dc), x, y);
      }
    }
    else if (strcmp(tmp, "rlineto") == 0) {
      if (ps_depth() >= 2) {
        float y=ps_pop(), x=ps_pop();
        fz_path *p = get_path(ctx, dc);
        fz_point cp = fz_currentpoint(ctx, p);
        fz_lineto(ctx, p, cp.x + x, cp.y + y);
      }
    }
    // PS clip operators in function bodies
    else if (strcmp(tmp, "W") == 0 || strcmp(tmp, "W*") == 0) {
      int eofill = (tmp[0] == 'W' && tmp[1] == '*') ? 1 : 0;
      if (dc->dev) {
        fz_matrix ctm = dvi_get_ctm(dc, st);
        fz_clip_path(ctx, dc->dev, get_path(ctx, dc), eofill, ctm,
                     fz_infinite_rect);
        st->gs.clip_depth += 1;
      }
      drop_path(ctx, dc);
    }
    else if (strcmp(tmp, "get") == 0 || strcmp(tmp, "ifelse") == 0) {
      if (ps_depth() >= 2) { ps_pop(); ps_pop(); }
    }
    // PGF dvips stroke/fill aliases in function bodies
    else if (strcmp(tmp, "pgfs") == 0 || strcmp(tmp, "pgfS") == 0) {
      if (dc->dev) {
        fz_matrix ctm = dvi_get_ctm(dc, st);
        fz_stroke_state sst;
        get_stroke_state(ctx, st, &sst);
        fz_stroke_path(ctx, dc->dev, get_path(ctx,dc), &sst, ctm,
                       device_cs(ctx), st->gs.colors.line, st->gs.stroke_alpha, color_params);
      }
      drop_path(ctx, dc);
      rendered = true;
    }
    else if (strcmp(tmp, "pgfr") == 0 || strcmp(tmp, "pgfR") == 0) {
      if (dc->dev) {
        fz_matrix ctm = dvi_get_ctm(dc, st);
        fz_fill_path(ctx, dc->dev, get_path(ctx,dc), 0, ctm,
                     device_cs(ctx), st->gs.colors.fill, st->gs.fill_alpha, color_params);
      }
      rendered = true;
      // Do NOT drop path — pgfstr may follow within the same body
    }
    // ignore other commands in body
  }

  // Drop path if a fill left it behind (same rationale as ps_code)
  if (rendered && dc->path)
    drop_path(ctx, dc);
}

// Parse a pgf PostScript shading special and render natively.
// Handles common shading types: axial (pgfHrgb/Vrgb/Argb) and radial (pgfRrgb).
// Returns 1 if handled, 0 if not.
static bool
dvi_exec_pgf_shading(fz_context *ctx, dvi_context *dc, dvi_state *st,
                      cursor_t cur, cursor_t lim)
{
  // Scan for known shading function invocations.
  // Skip past function definitions (between { and } bind def).
  // Look for: param1 param2 ... paramN funcName

  // Known shading functions and their parameter counts
  // Axial: 11 params (startx starty endx endy R1 G1 B1 R2 G2 B2 depth) + funcName
  // Radial: more complex

  // Simple approach: scan for function names and parse backward for params
  static const char *func_names[] = {
    "pgfHrgb", "pgfVrgb", "pgfArgb",  // axial RGB
    "pgfHcmyk", "pgfVcmyk", "pgfAcmyk", // axial CMYK
    "pgfRrgb", "pgfR1rgb",            // radial RGB
    NULL
  };

  for (int fi = 0; func_names[fi]; fi++)
  {
    const char *fname = func_names[fi];
    int flen = strlen(fname);

    // Search for function name in the special content
    for (cursor_t p = cur; p + flen <= lim; p++)
    {
      if (memcmp(p, fname, flen) == 0)
      {
        // Check that it's a standalone token (preceded by whitespace/newline)
        if (p > cur && !(*(p-1) == ' ' || *(p-1) == '\n' || *(p-1) == '\r'))
          continue;
        // Check end of token
        if (p + flen < lim && !(*(p+flen) == ' ' || *(p+flen) == '\n' || *(p+flen) == '\r'))
          continue;

        // Parse backwards to get parameters
        // Skip the function definition if this is a 'def' not an invocation
        cursor_t after = p + flen;
        // Skip trailing whitespace
        while (after < lim && (*after == ' ' || *after == '\n' || *after == '\r'))
          after++;

        // This is an invocation if not followed by '{' or 'bind def'
        // For now, try to parse parameters before the function name
        float params[20] = {0};
        int nparams = 0;
        cursor_t q = p;

        // Scan backwards to find start of numeric parameters
        while (q > cur && nparams < 15)
        {
          // Skip whitespace backwards
          while (q > cur && (*(q-1) == ' ' || *(q-1) == '\n' || *(q-1) == '\r'))
            q--;

          // Find start of this token
          cursor_t tok_start = q;
          while (tok_start > cur && *(tok_start-1) != ' ' && *(tok_start-1) != '\n'
                 && *(tok_start-1) != '\r' && *(tok_start-1) != '{' && *(tok_start-1) != '}')
            tok_start--;

          // Try to parse as float
          char tmp[64];
          int tlen = q - tok_start;
          if (tlen > 63) break;
          memcpy(tmp, tok_start, tlen);
          tmp[tlen] = 0;

          // Check if this is a number
          bool is_num = true;
          int dots = 0;
          for (int k = 0; k < tlen; k++)
          {
            if (tmp[k] == '-') { if (k > 0) { is_num = false; break; } }
            else if (tmp[k] == '.') { dots++; if (dots > 1) { is_num = false; break; } }
            else if (tmp[k] < '0' || tmp[k] > '9') { is_num = false; break; }
          }
          if (!is_num || tlen == 0) break;

          params[nparams++] = pfloat(tok_start, q);
          q = tok_start;
        }

        if (nparams < 2) continue;

        // Reverse params (we parsed them backwards)
        for (int i = 0; i < nparams / 2; i++)
        {
          float tmp2 = params[i];
          params[i] = params[nparams - 1 - i];
          params[nparams - 1 - i] = tmp2;
        }

        // Check if this is an axial shading
        bool is_axial = (fi <= 2);  // pgfHrgb, pgfVrgb, pgfArgb
        bool is_cmyk = (fi >= 3 && fi <= 5);
        bool is_radial = (fi >= 6);

        if (is_axial && nparams >= 11)
        {
          float sx = params[0], sy = params[1];
          float ex = params[2], ey = params[3];
          float c0[3], c1[3];

          if (is_cmyk)
          {
            color_set_cmyk(c0, params[4], params[5], params[6], params[7]);
            color_set_cmyk(c1, params[8], params[9], params[10], params[11]);
          }
          else
          {
            color_set_rgb(c0, params[4], params[5], params[6]);
            color_set_rgb(c1, params[8], params[9], params[10]);
          }

          render_axial_shade(ctx, dc, st, sx, sy, ex, ey, c0, c1, NULL);
          return 1;
        }

        if (is_radial && nparams >= 10)
        {
          // Radial: startx starty startr endx endy endr R1 G1 B1 R2 G2 B2
          float sx = params[0], sy = params[1];
          float ex = params[2], ey = params[3];
          float c0[3] = {params[4], params[5], params[6]};
          float c1[3] = {params[7], params[8], params[9]};
          float r0 = 0, r1 = sqrtf((ex - sx) * (ex - sx) + (ey - sy) * (ey - sy));

          {
          shade_subfunc sf = {{c0[0],c0[1],c0[2]},{c1[0],c1[1],c1[2]}};
          render_radial_shade(ctx, dc, st, sx, sy, r0, r1, &sf, 1, NULL, 0);
          }
          return 1;
        }

        // If we found a function name but couldn't parse params, still count as handled
        return 1;
      }
    }
  }

  // Not a recognized shading pattern
  return 1; // Still return 1 to avoid aborting the page
}

static bool
dvi_exec_pdf(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  cursor_t mar, i, f0, f1, f2, f3, f4, f5, pxform = NULL, pstart, pend;

  /*!re2c

  "pagesize" ws+ "width" ws+ @f0 dim
             ws+ "height" ws+ @f1 dim
  { return 1; }

  "pagesize" ws+ "default"
  { return 1; }

  @f0 "image" ws+ @pxform ([a-z] | ws | float)* @pstart "("
  {
    struct xform_spec xf = xform_spec();
    pxform = parse_xform_or_dim(&xf, pxform, pstart);
    if (pxform != pstart)
      fprintf(stderr, "pdf unhandled transformation: %.*s\n",
              (int)(pstart - pxform), pxform);
    char filename[2048];
    cur = parse_pdf_string(filename, filename + 2048, cur, lim);
    if (!embed_graphics(ctx, dc, st, &xf, filename))
    {
      fprintf(stderr, "error rendering image: %.*s", (int)(lim-f0), f0);
      return 0;
    }
    else
      return 1;
  }

  ("begintransform" | "btrans" | "bt") ws*
  { return pdf_btrans(dc, st, cur, lim); }

  ("endtransform" | "etrans" | "et")
  { return pdf_etrans(dc, st); }

  @f0 ("bcontent" | "econtent") @f1
  {
    // bcontent/econtent: these delimit content streams but the tikz
    // engine also emits explicit q/Q and btrans/etrans for state
    // management. Treating these as gsave/grestore would double-push
    // and cause the text to lose the btrans CTM.
    if (f1 != lim)
      fprintf(stderr, "unhandled pdf content: %.*s\n",
              (int)(lim - f0), f0);
    return 1;
  }

  ("begincolor" | "bcolor" | "bc") ws*
  (@f4 float | "[" ws* @f0 float (ws+ @f1 float ws+ @f2 float (ws+ @f3 float)?)? ws* "]")
  {
    if (!colorstack_push(ctx, dc, st, -1))
      return 0;
    if (f3)
    {
      color_set_cmyk(st->gs.colors.fill, pfloat(f0, lim), pfloat(f1, lim), pfloat(f2, lim), pfloat(f3, lim));
      color_set_cmyk(st->gs.colors.line, pfloat(f0, lim), pfloat(f1, lim), pfloat(f2, lim), pfloat(f3, lim));
    }
    else if (f1)
    {
      color_set_rgb(st->gs.colors.fill, pfloat(f0, lim), pfloat(f1, lim), pfloat(f2, lim));
      color_set_rgb(st->gs.colors.line, pfloat(f0, lim), pfloat(f1, lim), pfloat(f2, lim));
    }
    else
    {
      color_set_gray(st->gs.colors.fill, pfloat(f4 ? f4 : f0, lim));
      color_set_gray(st->gs.colors.line, pfloat(f4 ? f4 : f0, lim));
    }
    return 1;
  }

  ("endcolor" | "ecolor" | "ec")
  {
    return colorstack_pop(ctx, dc, st, -1);
  }

  "q"
  {
    if (st->gs_stack.depth >= st->gs_stack.limit) return 0;
    st->gs_stack.base[st->gs_stack.depth] = st->gs;
    st->gs_stack.depth += 1;
    return 1;
  }

  "Q"
  {
    if (st->gs_stack.depth == 0) return 0;
    int clip_depth0 = st->gs.clip_depth;
    st->gs_stack.depth -= 1;
    st->gs = st->gs_stack.base[st->gs_stack.depth];
    if (dc->dev)
      for (int i = st->gs.clip_depth; i < clip_depth0; ++i)
        fz_pop_clip(ctx, dc->dev);
    return 1;
  }

  "cm" ws+ @f0 float ws+ @f1 float ws+ @f2 float ws+
              @f3 float ws+ @f4 float ws+ @f5 float
  {
    // Standalone cm operator (used by PGF for text positioning in nodes)
    fz_matrix mat;
    mat.a = pfloat(f0, lim); mat.b = pfloat(f1, lim);
    mat.c = pfloat(f2, lim); mat.d = pfloat(f3, lim);
    mat.e = pfloat(f4, lim); mat.f = pfloat(f5, lim);
    fz_matrix ctm = fz_concat(mat, dvi_get_ctm(dc, st));
    dvi_set_ctm(st, ctm);
    return 1;
  }

  "code"
  { return pdf_code(ctx, dc, st, cur, lim); }

  "literal" ws+ ("direct" ws+)?
  { return pdf_code(ctx, dc, st, cur, lim); }

  ''
  { return unhandled("pdf special", cur, lim, 0); }

  */
}

bool dvi_exec_special(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  cursor_t mar, i, j;

  { int plen = lim - cur; if (plen > 500) plen = 500;
    fprintf(stderr, "TRACE sp(%.*s) gs.h=%d gs.v=%d\n", plen, cur, st->gs.h, st->gs.v); }

  for (;;)
  {
    /*!re2c

    " "
    { continue; }

    "landscape"
    { return 1; }

    "pdfcolorstack" ws+ @i nat
                    ws+ "current"
    { return pdfcolorstack_current(ctx, dc, st, pint(i, lim)); }

    "pdfcolorstack" ws+ @i nat
                    ws+ "pop"
    { return colorstack_pop(ctx, dc, st, pint(i, lim)); }

    "pdfcolorstack" ws+ @i nat
                    ws+ "push" ws+ "("
    {
      return colorstack_push(ctx, dc, st, pint(i, lim)) &&
             parse_pdfcolor(&st->gs.colors, cur, lim);
    }

    "pdfcolorstackinit" ws+ @i nat
                        ws+ "page" ws+ "direct" ws+ "("
    {
      return colorstack_init(ctx, dc, st, pint(i, lim)) &&
             parse_pdfcolor(&st->gs.colors, cur, lim);
    }

    "color" ws+ "pop"
    { return colorstack_pop(ctx, dc, st, -1); }

    "color" ws+ "push" ws+
    {
      return colorstack_push(ctx, dc, st, -1) &&
             parse_color(&st->gs.colors, cur, lim);
    }

    "x:"
    {
      struct xform_spec xf = xform_spec();
      cur = parse_xform_or_dim(&xf, cur, lim);
      st->gs.ctm = fz_concat(xf.ctm, st->gs.ctm);
      if (cur < lim)
        return unhandled("pdf x", cur, lim, 0);
      return 1;
    }

    "pdf:" ws*
    { return dvi_exec_pdf(ctx, dc, st, cur, lim); }

    "papersize=" @i dim "," @j dim
    {
      dc->page_width  = pdim(i, lim);
      dc->page_height = pdim(j, lim);
      fprintf(stderr, "DBG papersize: w=%.2f h=%.2f\n", dc->page_width, dc->page_height);
      return 1;
    }

    "!" ws* "/pgf"
    {
      // PGF "!" specials: always parse function definitions.
      // IMPORTANT: the match consumed "/pgf" from the content (e.g. from
      // "/pgfsc"), so back up 4 chars so ps_parse_defs sees the full
      // function name including the /pgf prefix.
      cursor_t content_start = cur - 4; // include "/pgf" that was consumed
      ps_parse_defs(content_start, lim);
      if (dc->dev)
        return dvi_exec_pgf_shading(ctx, dc, st, cur, lim);
      return 1;
    }

    "ps:" ws*
    {
      // ps: (single colon) — state management: pgfsc/pgffc clear, etc.
      // Process same as ps:: so that pgfsc{}/pgffc{} clears take effect.
      return ps_code(ctx, dc, st, cur, lim);
    }

    "ps::" ws*
    {
      return ps_code(ctx, dc, st, cur, lim);
    }

    ''
    { return unhandled("special", cur, lim, 0); }

    */
  }
}

bool dvi_init_special(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  cursor_t i, f0, f1, mar;

  /*!re2c

  "pdfcolorstackinit" ws+ @i nat
                      ws+ "page" ws+ "direct" ws+ "("
  {
    return colorstack_init(ctx, dc, st, pint(i, lim)) &&
           parse_pdfcolor(&st->gs.colors, cur, lim);
  }

  ''
  { return 0; }

  */
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

  ''
  { return; }

  */
}
