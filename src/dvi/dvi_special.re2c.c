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
    fz_matrix trm = st->gs.text.Tm;
    trm = fz_pre_scale(trm, fs * hs, fs);
    trm = fz_concat(trm, ctm);
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

// Render a simple 2-color axial gradient natively using sampled fills.
// Used when pgf PostScript shading specials are encountered.
static void render_axial_shade(fz_context *ctx, dvi_context *dc, dvi_state *st,
    float x0, float y0, float x1, float y1,
    float c0[3], float c1[3])
{
  if (!dc->dev) return;

  fz_matrix ctm = dvi_get_ctm(dc, st);
  int steps = 80;

  // Compute gradient direction and perpendicular
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.001f) return;

  // Perpendicular direction for line width
  float px = -dy / len, py = dx / len;

  for (int i = 0; i < steps; i++)
  {
    float t = (i + 0.5f) / steps;
    float color[3] = {
      c0[0] + (c1[0] - c0[0]) * t,
      c0[1] + (c1[1] - c0[1]) * t,
      c0[2] + (c1[2] - c0[2]) * t,
    };

    float cx = x0 + dx * (float)i / steps;
    float cy = y0 + dy * (float)i / steps;
    float nx = x0 + dx * (float)(i + 1) / steps;
    float ny = y0 + dy * (float)(i + 1) / steps;

    // Build a thin trapezoid along the gradient
    fz_path *path = fz_new_path(ctx);
    float hw = len / steps * 0.6f; // half-width for overlap
    fz_moveto(ctx, path, cx + px * hw, cy + py * hw);
    fz_lineto(ctx, path, nx + px * hw, ny + py * hw);
    fz_lineto(ctx, path, nx - px * hw, ny - py * hw);
    fz_lineto(ctx, path, cx - px * hw, cy - py * hw);
    fz_closepath(ctx, path);
    fz_fill_path(ctx, dc->dev, path, 0, ctm, device_cs(ctx),
                 color, st->gs.fill_alpha, color_params);
    fz_drop_path(ctx, path);
  }
}

// Render a simple radial gradient natively.
static void render_radial_shade(fz_context *ctx, dvi_context *dc, dvi_state *st,
    float cx, float cy, float r0, float r1,
    float c0[3], float c1[3])
{
  if (!dc->dev) return;

  fz_matrix ctm = dvi_get_ctm(dc, st);
  int steps = 60;
  float r_max = r1 > r0 ? r1 : r0;
  float r_min = r0 < r1 ? r0 : r1;

  for (int i = 0; i < steps; i++)
  {
    float t_inner = (float)i / steps;
    float t_outer = (float)(i + 1) / steps;
    float ri = r_min + (r_max - r_min) * t_inner;
    float ro = r_min + (r_max - r_min) * t_outer;
    float t = (t_inner + t_outer) * 0.5f;

    float color[3] = {
      c0[0] + (c1[0] - c0[0]) * t,
      c0[1] + (c1[1] - c0[1]) * t,
      c0[2] + (c1[2] - c0[2]) * t,
    };

    // Draw a thick ring
    fz_path *path = fz_new_path(ctx);
    // Outer circle
    fz_moveto(ctx, path, cx + ro, cy);
    fz_curveto(ctx, path, cx + ro, cy + ro * 0.552f, cx + ro * 0.552f, cy + ro, cx, cy + ro);
    fz_curveto(ctx, path, cx - ro * 0.552f, cy + ro, cx - ro, cy + ro * 0.552f, cx - ro, cy);
    fz_curveto(ctx, path, cx - ro, cy - ro * 0.552f, cx - ro * 0.552f, cy - ro, cx, cy - ro);
    fz_curveto(ctx, path, cx + ro * 0.552f, cy - ro, cx + ro, cy - ro * 0.552f, cx + ro, cy);
    fz_closepath(ctx, path);
    // Inner circle (reverse direction for even-odd fill)
    fz_moveto(ctx, path, cx + ri, cy);
    fz_curveto(ctx, path, cx + ri, cy - ri * 0.552f, cx + ri * 0.552f, cy - ri, cx, cy - ri);
    fz_curveto(ctx, path, cx - ri * 0.552f, cy - ri, cx - ri, cy - ri * 0.552f, cx - ri, cy);
    fz_curveto(ctx, path, cx - ri, cy + ri * 0.552f, cx - ri * 0.552f, cy + ri, cx, cy + ri);
    fz_curveto(ctx, path, cx + ri * 0.552f, cy + ri, cx + ri, cy + ri * 0.552f, cx + ri, cy);
    fz_closepath(ctx, path);
    fz_fill_path(ctx, dc->dev, path, 1, ctm, device_cs(ctx),
                 color, st->gs.fill_alpha, color_params);
    fz_drop_path(ctx, path);
  }
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
void ps_state_reset()           { ps_sp = 0; ps_func_count = 0; }

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
        if (p + 3 <= end && memcmp(p, "def", 3) == 0) {
          p += 3;
          ps_define_func(ns, nl, bs, bl);
          fprintf(stderr, "DBG ps_def: /%.*s = %.*s\n", nl, ns, bl, bs);
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

    // Try as number
    char *ep;
    float fv = strtof(tmp, &ep);
    if (ep == tmp + tl && tl > 0 && tmp[0] != '/' && tmp[0] != '[' && tmp[0] != ']')
    {
      ps_push(fv);
      continue;
    }

    // --- Command dispatch ---
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
      drop_path(ctx, dc);
    }
    else if (strcmp(tmp, "pgfstr") == 0) {
      const char *b = ps_lookup_func("pgfsc");
      if (b && *b) ps_exec_body(ctx, dc, st, b, strlen(b), PS_COLOR_STROKE);
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
      drop_path(ctx, dc);
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
    // PGF ellipse (rx ry x y pgfe)
    else if (strcmp(tmp, "pgfe") == 0) {
      if (ps_depth() >= 4) {
        float y=ps_pop(), x=ps_pop(), ry=ps_pop(), rx=ps_pop();
        fz_path *path = get_path(ctx, dc);
        float k = 0.5522847498f;
        fz_moveto(ctx, path, x+rx, y);
        fz_curveto(ctx, path, x+rx,y+k*ry, x+k*rx,y+ry, x,y+ry);
        fz_curveto(ctx, path, x-k*rx,y+ry, x-rx,y+k*ry, x-rx,y);
        fz_curveto(ctx, path, x-rx,y-k*ry, x-k*rx,y-ry, x,y-ry);
        fz_curveto(ctx, path, x+k*rx,y-ry, x+rx,y-k*ry, x+rx,y);
        fz_closepath(ctx, path);
      }
    }
    // PS concat: [a b c d e f] concat — concatenate matrix to CTM
    // This is how PGF/dvips driver applies translations and transformations
    else if (strcmp(tmp, "concat") == 0) {
      if (ps_depth() >= 6) {
        float f=ps_pop(), e=ps_pop(), d=ps_pop(), c=ps_pop(), b=ps_pop(), a=ps_pop();
        fz_matrix mat;
        mat.a = a; mat.b = b; mat.c = c;
        mat.d = d; mat.e = e; mat.f = f;
        st->gs.ctm = fz_concat(mat, st->gs.ctm);
        st->gs.h = st->registers.h;
        st->gs.v = st->registers.v;
        ps_clear(); // consume any leftover values
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
    else if (strcmp(tmp, "get") == 0 || strcmp(tmp, "ifelse") == 0) {
      // PS dict/array get and conditional — not needed, consume 2 values
      if (ps_depth() >= 2) { ps_pop(); ps_pop(); }
    }
    else if (strcmp(tmp, "/pgfsmaskinplace") == 0) {
      // PGF soft mask placeholder — no-op for now
    }
    // PGF cleanup / end markers
    else if (strcmp(tmp, "pgfc") == 0) { /* no-op */ }
    // Try as a user-defined function call
    else {
      const char *b = ps_lookup_func(tmp);
      if (b) ps_exec_body(ctx, dc, st, b, strlen(b), PS_COLOR_BOTH);
    }
  }

  ps_clear();
  return 1;
}

// Execute a stored PS function body (simple token interpreter)
static void
ps_exec_body(fz_context *ctx, dvi_context *dc, dvi_state *st,
              const char *body, int body_len, int color_target)
{
  const char *p = body, *end = body + body_len;
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

    char *ep;
    float fv = strtof(tmp, &ep);
    if (ep == tmp + tl && tl > 0 && tmp[0] != '/') {
      ps_push(fv);
      continue;
    }

    int ct = color_target;
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
        ps_clear();
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
    // Stroke/fill within function bodies
    else if (strcmp(tmp, "pgfstr") == 0) {
      if (dc->dev) {
        fz_matrix ctm = dvi_get_ctm(dc, st);
        fz_stroke_state sst;
        get_stroke_state(ctx, st, &sst);
        fz_stroke_path(ctx, dc->dev, get_path(ctx,dc), &sst, ctm,
                       device_cs(ctx), st->gs.colors.line, st->gs.stroke_alpha, color_params);
      }
      drop_path(ctx, dc);
    }
    else if (strcmp(tmp, "pgffill") == 0) {
      if (dc->dev) {
        fz_matrix ctm = dvi_get_ctm(dc, st);
        fz_fill_path(ctx, dc->dev, get_path(ctx,dc), 0, ctm,
                     device_cs(ctx), st->gs.colors.fill, st->gs.fill_alpha, color_params);
      }
      drop_path(ctx, dc);
    }
    // PGF ellipse within function bodies
    else if (strcmp(tmp, "pgfe") == 0) {
      if (ps_depth() >= 4) {
        float y=ps_pop(), x=ps_pop(), ry=ps_pop(), rx=ps_pop();
        fz_path *path = get_path(ctx, dc);
        float k = 0.5522847498f;
        fz_moveto(ctx, path, x+rx, y);
        fz_curveto(ctx, path, x+rx,y+k*ry, x+k*rx,y+ry, x,y+ry);
        fz_curveto(ctx, path, x-k*rx,y+ry, x-rx,y+k*ry, x-rx,y);
        fz_curveto(ctx, path, x-rx,y-k*ry, x-k*rx,y-ry, x,y-ry);
        fz_curveto(ctx, path, x+k*rx,y-ry, x+rx,y-k*ry, x+rx,y);
        fz_closepath(ctx, path);
      }
    }
    // concat for CTM transforms within function bodies
    else if (strcmp(tmp, "concat") == 0) {
      if (ps_depth() >= 6) {
        float f=ps_pop(), e=ps_pop(), d=ps_pop(), c=ps_pop(), b=ps_pop(), a=ps_pop();
        fz_matrix mat;
        mat.a = a; mat.b = b; mat.c = c;
        mat.d = d; mat.e = e; mat.f = f;
        st->gs.ctm = fz_concat(mat, st->gs.ctm);
        st->gs.h = st->registers.h;
        st->gs.v = st->registers.v;
        ps_clear();
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
    else if (strcmp(tmp, "get") == 0 || strcmp(tmp, "ifelse") == 0) {
      if (ps_depth() >= 2) { ps_pop(); ps_pop(); }
    }
    // ignore other commands in body
  }
  ps_clear();
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

          render_axial_shade(ctx, dc, st, sx, sy, ex, ey, c0, c1);
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

          render_radial_shade(ctx, dc, st, sx, sy, r0, r1, c0, c1);
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

  "papersize=" @f0 dim "," @f1 dim
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

  { int plen = lim - cur; if (plen > 60) plen = 60;
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

    "!" ws* "/pgf"
    {
      // PostScript shading special from pgf/pgfplots.
      // These define and invoke shading functions in PostScript.
      // We parse the parameters and render the shading natively
      // for common types (axial, radial).
      // Return 1 to mark as handled even if we can't parse it.
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
