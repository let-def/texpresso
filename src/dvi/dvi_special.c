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

#include <mupdf/pdf/object.h>
#include <string.h>
#include "fz_util.h"
#include "mydvi.h"
#include "mydvi_interp.h"
#include "pdf_lexer.h"
#include "vstack.h"
#include <math.h>

#define device_cs fz_device_rgb
#define color_params fz_default_color_params

typedef const char *cursor_t;

/*!re2c

re2c:yyfill:enable = 0;
re2c:eof = -1;
re2c:api = custom;
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
  if (!ignored)
    fprintf(stderr, "unhandled %s: \"%.*s\"\n", kind, (int)(lim - cur), cur);
  return 0;
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
  cursor_t mar, f0, f1, f2;
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
    if (w != w) w = h * ar;
    if (h != h) h = w / ar;
    ctm = fz_pre_scale(fz_pre_translate(ctm, 0, h), w, -h);
    fz_fill_image(ctx, dc->dev, img, ctm, 1.0, color_params);
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

static bool
pdf_code(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  vstack *stack = vstack_new(ctx);
  fz_var(cur);
  fz_var(stack);

  // fprintf(stderr, "pdf code: %.*s\n", (int)(lim - cur), cur);
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
          fz_matrix ctm = fz_concat(mat, dvi_get_ctm(dc, st));
          dvi_set_ctm(st, ctm);
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
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_stroke_state stst = fz_default_stroke_state;
            stst.linewidth = st->gs.line_width;
            stst.linejoin = (int)st->gs.line_join;
            stst.start_cap = stst.end_cap = (int)st->gs.line_caps;
            stst.miterlimit = (int)st->gs.miter_limit;
            fz_path *path = get_path(ctx, dc);
            fz_closepath(ctx, path);
            fz_fill_path(ctx, dc->dev, path, 0, ctm, device_cs(ctx),
                         st->gs.colors.fill, 1.0, color_params);
            fz_stroke_path(ctx, dc->dev, path, &stst, ctm, device_cs(ctx),
                           st->gs.colors.line, 1.0, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_b_star:
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_stroke_state stst = fz_default_stroke_state;
            stst.linewidth = st->gs.line_width;
            stst.linejoin = (int)st->gs.line_join;
            stst.start_cap = stst.end_cap = (int)st->gs.line_caps;
            stst.miterlimit = (int)st->gs.miter_limit;
            fz_path *path = get_path(ctx, dc);
            fz_closepath(ctx, path);
            fz_fill_path(ctx, dc->dev, path, 1, ctm, device_cs(ctx),
                         st->gs.colors.fill, 1.0, color_params);
            fz_stroke_path(ctx, dc->dev, path, &stst, ctm, device_cs(ctx),
                           st->gs.colors.line, 1.0, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_B:
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_stroke_state stst = fz_default_stroke_state;
            stst.linewidth = st->gs.line_width;
            stst.linejoin = (int)st->gs.line_join;
            stst.start_cap = stst.end_cap = (int)st->gs.line_caps;
            stst.miterlimit = (int)st->gs.miter_limit;
            fz_path *path = get_path(ctx, dc);
            fz_fill_path(ctx, dc->dev, path, 0, ctm, device_cs(ctx),
                         st->gs.colors.fill, 1.0, color_params);
            fz_stroke_path(ctx, dc->dev, path, &stst, ctm, device_cs(ctx),
                           st->gs.colors.line, 1.0, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_B_star:
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_stroke_state stst = fz_default_stroke_state;
            stst.linewidth = st->gs.line_width;
            stst.linejoin = (int)st->gs.line_join;
            stst.start_cap = stst.end_cap = (int)st->gs.line_caps;
            stst.miterlimit = (int)st->gs.miter_limit;
            fz_path *path = get_path(ctx, dc);
            fz_fill_path(ctx, dc->dev, path, 1, ctm, device_cs(ctx),
                         st->gs.colors.fill, 1.0, color_params);
            fz_stroke_path(ctx, dc->dev, path, &stst, ctm, device_cs(ctx),
                           st->gs.colors.fill, 1.0, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_f:
        case PDF_OP_F:
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_path *path = get_path(ctx, dc);
            fz_fill_path(ctx, dc->dev, path, 0, ctm, device_cs(ctx),
                         st->gs.colors.fill, 1.0, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_f_star:
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_path *path = get_path(ctx, dc);
            fz_fill_path(ctx, dc->dev, path, 1, ctm, device_cs(ctx),
                         st->gs.colors.fill, 1.0, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_S:
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_stroke_state stst = fz_default_stroke_state;
            stst.linewidth = st->gs.line_width;
            stst.linejoin = (int)st->gs.line_join;
            stst.start_cap = stst.end_cap = (int)st->gs.line_caps;
            stst.miterlimit = (int)st->gs.miter_limit;
            fz_path *path = get_path(ctx, dc);
            fz_stroke_path(ctx, dc->dev, path, &stst, ctm, device_cs(ctx),
                           st->gs.colors.line, 1.0, color_params);
          }
          drop_path(ctx, dc);
          break;

        case PDF_OP_s:
          if (dc->dev)
          {
            fz_matrix ctm = dvi_get_ctm(dc, st);
            fz_stroke_state stst = fz_default_stroke_state;
            stst.linewidth = st->gs.line_width;
            stst.linejoin = (int)st->gs.line_join;
            stst.miterlimit = (int)st->gs.miter_limit;
            stst.dash_cap = stst.start_cap = stst.end_cap =
                (int)st->gs.line_caps;
            stst.dash_len = st->gs.dash_len;
            memcpy(stst.dash_list, st->gs.dash,
                   sizeof(float) * st->gs.dash_len);
            stst.dash_phase = st->gs.dash_phase;
            fz_path *path = get_path(ctx, dc);
            fz_closepath(ctx, path);
            fz_stroke_path(ctx, dc->dev, path, &stst, ctm, device_cs(ctx),
                           st->gs.colors.line, 1.0, color_params);
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
          if (st->gs.dash_len > 4) st->gs.dash_len = 4;
          for (int i = 0; i < st->gs.dash_len; ++i)
            st->gs.dash[i] = val_number(ctx, val_array_get(ctx, stack, v[0], i));
          st->gs.dash_phase = val_number(ctx, v[1]);
          break;
        }

        case PDF_OP_ri:
        case PDF_OP_i:
        case PDF_OP_gs:
        case PDF_OP_v:
        case PDF_OP_y:
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
        case PDF_OP_d0:
        case PDF_OP_d1:
        case PDF_OP_CS:
        case PDF_OP_cs:
        case PDF_OP_SC:
        case PDF_OP_sc:
        case PDF_OP_SCN:
        case PDF_OP_scn:
        case PDF_OP_sh:
        case PDF_OP_Do:
        case PDF_OP_MP:
        case PDF_OP_DP:
        case PDF_OP_BMC:
        case PDF_OP_BDC:
        case PDF_OP_EMC:
        case PDF_OP_BX:
        case PDF_OP_EX:
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
    return 0;
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
      color_set_cmyk(st->gs.colors.fill, pfloat(f0, lim), pfloat(f1, lim), pfloat(f2, lim), pfloat(f3, lim));
    else if (f1)
      color_set_rgb(st->gs.colors.fill, pfloat(f0, lim), pfloat(f1, lim), pfloat(f2, lim));
    else
      color_set_gray(st->gs.colors.fill, pfloat(f4 ? f4 : f0, lim));
    return 1;
  }

  ("endcolor" | "ecolor" | "ec")
  {
    return colorstack_pop(ctx, dc, st, -1);
  }

  "code"
  { return pdf_code(ctx, dc, st, cur, lim); }

  ''
  { return unhandled("pdf special", cur, lim, 0); }

  */
}

bool dvi_exec_special(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  cursor_t mar, i, j;

  // fprintf(stderr, "special: %.*s\n", (int)(lim - cur), cur);

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

    "I" ws+ @i nat ws+ @j nat
    {
      dc->sync.pos[1] = dc->sync.pos[0];
      dc->sync.pos[0].file = pnat(i, lim);
      dc->sync.pos[0].line = pnat(j, lim);
      return 1;
    }

    "p"
    {
      dvi_sync_pos prev = dc->sync.pos[1];
      dc->sync.pos[1] = dc->sync.pos[0];
      dc->sync.pos[0] = prev;
      return 1;
    }

    "P"
    {
      dvi_sync_pos prev = dc->sync.pos[1];
      dc->sync.pos[1] = dc->sync.pos[0];
      dc->sync.pos[0] = prev;
      dc->sync.pos[0].line += 1;
      return 1;
    }

    "P" @i ['0'-'9']
    {
      dvi_sync_pos prev = dc->sync.pos[1];
      dc->sync.pos[1] = dc->sync.pos[0];
      dc->sync.pos[0] = prev;
      dc->sync.pos[0].line += (*i) - '0';
      return 1;
    }

    "P" ws+ @i nat
    {
      dvi_sync_pos prev = dc->sync.pos[1];
      dc->sync.pos[1] = dc->sync.pos[0];
      dc->sync.pos[0].line = pnat(i, lim);
      return 1;
    }

    "l"
    {
      dc->sync.pos[0].line += 1;
      return 1;
    }

    "L"
    {
      dc->sync.pos[0].line += 2;
      return 1;
    }

    "L" @i ['0'-'9']
    {
      dc->sync.pos[0].line += (*i) - '0';
      return 1;
    }

    "L" ws+ @i nat
    {
      dc->sync.pos[0].line = pnat(i, lim);
      return 1;
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
