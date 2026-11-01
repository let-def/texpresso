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
//   following their definition;
// - color stacks persist from one page to the next (see dvi_color.c).
//
// This file only parses and dispatches specials. The interpretation is done
// by the modules listed in dvi_special.h.

#include <mupdf/pdf.h>
#include <string.h>
#include <ctype.h>
#include "fz_util.h"
#include "mydvi.h"
#include "mydvi_interp.h"
#include "pdf_lexer.h"
#include "vstack.h"
#include "dvi_pdfstore.h"
#include "dvi_special.h"
#include <math.h>



/*!re2c

re2c:yyfill:enable = 0;
re2c:eof = -1;
re2c:flags:input = custom;
re2c:api:style = free-form;
re2c:define:YYCURSOR    = cur;
re2c:define:YYCTYPE     = int;
re2c:define:YYLESSTHAN  = "cur >= lim";
re2c:define:YYPEEK      = "cur < lim ? (unsigned char)*cur : -1";
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


// Parse a color specification as accepted by dvipdfmx:
//   rgb R G B | cmyk C M Y K | gray G | hsb H S B
//   [N] | [R G B] | [C M Y K] | N | R G B | C M Y K
// Returns the cursor after the color, or NULL if no color could be parsed.
cursor_t
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


cursor_t
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

// Parse "@ident", returning the object number (allocated if necessary)
cursor_t parse_name_ref(fz_context *ctx, dvi_context *dc, cursor_t cur, cursor_t lim, int *num, bool create)
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


// Recognize the specials operating on color stacks. Returns the operation,
// or -1 for other specials. On success, `*cur` is moved to the arguments.
static int
match_colorstack(cursor_t *pcur, cursor_t lim, int *index, bool *page)
{
  cursor_t cur = skip_ws(*pcur, lim), mar, i, p;
  int op = -1;
  *index = -1;
  *page = 0;

  /*!re2c

  "color" ws+ "pop"
  { op = COLOR_POP; goto done; }

  "color" ws+ "push" ws+
  { op = COLOR_PUSH; goto done; }

  "color" ws+
  { op = COLOR_SET; goto done; }

  "pdf:" ws* ("begincolor" | "bcolor" | "bc" | "begingray" | "bgray" | "bg")
  { op = PDF_BCOLOR; goto done; }

  "pdf:" ws* ("setcolor" | "scolor" | "sc")
  { op = PDF_SCOLOR; goto done; }

  "pdf:" ws* ("endcolor" | "ecolor" | "ec" | "endgray" | "egray" | "eg")
  { op = COLOR_POP; goto done; }

  // Other pdf: specials ("pdf:bcontent" is not "pdf:bc ontent")
  "pdf:" ws* [a-zA-Z]+
  { return -1; }

  "pdfcolorstack" ws+ @i nat ws+ "current"
  { op = PDFCOLORSTACK_CURRENT; goto stack; }

  "pdfcolorstack" ws+ @i nat ws+ "pop"
  { op = PDFCOLORSTACK_POP; goto stack; }

  "pdfcolorstack" ws+ @i nat ws+ "push" ws+ "("
  { op = PDFCOLORSTACK_PUSH; goto stack; }

  "pdfcolorstack" ws+ @i nat ws+ "set" ws+ "("
  { op = PDFCOLORSTACK_SET; goto stack; }

  "pdfcolorstackinit" ws+ @i nat ws+ @p ("page" ws+)? ("direct" ws+)? "("
  {
    *page = (lim - p >= 4 && memcmp(p, "page", 4) == 0);
    op = PDFCOLORSTACK_INIT;
    goto stack;
  }

  ''
  { return -1; }

  */

stack:
  *index = pint(i, lim);
done:
  *pcur = cur;
  return op;
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
  { return special_btrans(ctx, dc, st, cur, lim); }

  ("endtransform" | "endtrans" | "etrans" | "et")
  { return gs_pop(ctx, dc, st); }

  "bcontent"
  { return special_bcontent(ctx, dc, st); }

  "econtent"
  { return special_econtent(ctx, dc, st); }

  ("bgcolor" | "bgc" | "bbc" | "bbg")
  { return fill_background(ctx, dc, st, cur, lim); }

  ("code" | "direct" | "page")
  { return run_pdf_code(ctx, dc, st, cur, lim); }

  "literal"
  { return special_literal(ctx, dc, st, cur, lim); }

  "content"
  { return special_content(ctx, dc, st, cur, lim); }

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
  { return xetex_scale(dc, st, pfloat(f0, lim), pfloat(f1, lim)); }

  "bscale" ws+ @f0 float ws+ @f1 float
  { return xetex_bscale(dc, st, pfloat(f0, lim), pfloat(f1, lim)); }

  "escale"
  { return xetex_escale(dc, st); }

  "rotate" ws+ @f0 float
  { return xetex_rotate(dc, st, pfloat(f0, lim)); }

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
    int index;
    bool page;
    cursor_t args = cur;
    int op = match_colorstack(&args, lim, &index, &page);
    if (op >= 0)
      result = special_colorstack(ctx, &dc->colors, &st->gs.colors, op, index, page, args, lim);
    else for (;;)
    {
      /*!re2c

      ws
      { continue; }

      "landscape"
      { result = 1; break; }

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
    int index;
    bool page;
    cursor_t args = cur;
    int op = match_colorstack(&args, lim, &index, &page);
    if (op >= 0)
      result = special_colorstack(ctx, &dc->scan_colors, NULL, op, index, page, args, lim);
    else for (;;)
    {
      /*!re2c

      ws
      { continue; }

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

