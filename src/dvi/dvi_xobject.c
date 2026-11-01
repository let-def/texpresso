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

// Object store, images and form XObjects

#include <ctype.h>
#include <string.h>
#include <math.h>
#include <mupdf/pdf.h>
#include "fz_util.h"
#include "mydvi.h"
#include "mydvi_interp.h"
#include "pdf_lexer.h"
#include "dvi_pdfstore.h"
#include "dvi_special.h"

/****************/
/* Object store */
/****************/

pdf_document *store_doc(dvi_context *dc)
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


// The resources that "@resources" refers to
static pdf_obj *current_resources(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  if (dc->forms_depth > 0)
    return pdf_dict_get(ctx, dc->forms[dc->forms_depth - 1].dict, PDF_NAME(Resources));
  return st->resources;
}

pdf_obj *
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
void special_obj(fz_context *ctx, dvi_context *dc, cursor_t cur, cursor_t lim)
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
void special_put(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
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
void special_stream(fz_context *ctx, dvi_context *dc, cursor_t cur, cursor_t lim)
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

// A page box, in the default user space of the page (not normalized by the
// page transform of MuPDF)
static fz_rect page_box(fz_context *ctx, pdf_obj *page, enum pagebox kind)
{
  pdf_obj *media = pdf_dict_get_inheritable(ctx, page, PDF_NAME(MediaBox));
  pdf_obj *crop = pdf_dict_get_inheritable(ctx, page, PDF_NAME(CropBox));
  pdf_obj *box = NULL;

  switch (kind)
  {
    case BOX_UNDEFINED:
      // Like dvipdfmx: the first box defined, in this order
      box = crop;
      if (!box) box = pdf_dict_get(ctx, page, PDF_NAME(ArtBox));
      if (!box) box = pdf_dict_get(ctx, page, PDF_NAME(TrimBox));
      if (!box) box = pdf_dict_get(ctx, page, PDF_NAME(BleedBox));
      break;
    case BOX_MEDIABOX: box = media; break;
    case BOX_CROPBOX: box = crop; break;
    case BOX_ARTBOX: box = pdf_dict_get(ctx, page, PDF_NAME(ArtBox)); break;
    case BOX_BLEEDBOX: box = pdf_dict_get(ctx, page, PDF_NAME(BleedBox)); break;
    case BOX_TRIMBOX: box = pdf_dict_get(ctx, page, PDF_NAME(TrimBox)); break;
  }

  if (!box && kind != BOX_UNDEFINED)
    box = crop;
  if (!box)
    box = media;
  if (!box)
    return fz_make_rect(0, 0, 612, 792);
  return pdf_to_rect(ctx, box);
}

// The /Rotate entry of a page, as a matrix that keeps the lower-left corner of
// the box in place (see set_transform_matrix in dvipdfmx)
static fz_matrix page_rotation(fz_context *ctx, pdf_obj *page, fz_rect b)
{
  int rot = pdf_to_int(ctx, pdf_dict_get_inheritable(ctx, page, PDF_NAME(Rotate)));
  rot %= 360;
  if (rot < 0)
    rot += 360;
  switch (rot)
  {
    case 90:
      return fz_make_matrix(0, -1, 1, 0, b.x0 - b.y0, b.y0 + b.x1);
    case 180:
      return fz_make_matrix(-1, 0, 0, -1, b.x0 + b.x1, b.y0 + b.y1);
    case 270:
      return fz_make_matrix(0, 1, -1, 0, b.x0 + b.y1, b.y0 - b.x0);
    default:
      return fz_identity;
  }
}

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

    // Follow dvipdfmx: the page is a form whose BBox is the selected page box
    // and whose Matrix implements /Rotate. It is then scaled to the requested
    // dimensions, computed from the rotated box (or the user bbox).
    fz_rect box = page_box(ctx, page->obj, xf->pagebox);
    fz_matrix rotation = page_rotation(ctx, page->obj, box);
    fz_rect rbox = fz_transform_rect(box, rotation);

    bool user_bbox = !fz_is_infinite_rect(xf->bbox);
    fz_rect ref = user_bbox ? xf->bbox : rbox;
    float bw = ref.x1 - ref.x0, bh = ref.y1 - ref.y0;
    if (bw == 0) bw = 1;
    if (bh == 0) bh = 1;

    float sx = 1, sy = 1, dp = 0;
    float w = xf->width, h = xf->height, d = xf->depth == xf->depth ? xf->depth : 0;
    if (w == w && h == h)
    {
      sx = w / bw;
      sy = (h + d) / bh;
      dp = d;
    }
    else if (w == w)
      sx = sy = w / bw;
    else if (h == h)
    {
      sx = sy = (h + d) / bh;
      dp = d;
    }

    fz_matrix scale = fz_make_matrix(sx, 0, 0, sy, 0, -dp);
    if (user_bbox)
      scale = fz_pre_translate(scale, -ref.x0, -ref.y0);

    // From the default user space of the page to the device
    fz_matrix place = fz_concat(scale, fz_concat(xf->ctm, ctm));
    fz_matrix form = fz_concat(rotation, place);

    // pdf_run_page_contents applies MuPDF page transform, cancel it
    fz_rect mediabox;
    fz_matrix page_ctm;
    pdf_page_transform(ctx, page, &mediabox, &page_ctm);

    fz_path *path = fz_new_path(ctx);
    fz_try(ctx)
    {
      fz_rectto(ctx, path, box.x0, box.y0, box.x1, box.y1);
      fz_clip_path(ctx, dc->dev, path, 0, form, fz_infinite_rect);
      if (xf->clip && user_bbox)
      {
        fz_path *user = fz_new_path(ctx);
        fz_try(ctx)
        {
          fz_rectto(ctx, user, ref.x0, ref.y0, ref.x1, ref.y1);
          fz_clip_path(ctx, dc->dev, user, 0, place, fz_infinite_rect);
        }
        fz_always(ctx)
          fz_drop_path(ctx, user);
        fz_catch(ctx)
          fz_rethrow(ctx);
      }
    }
    fz_always(ctx)
      fz_drop_path(ctx, path);
    fz_catch(ctx)
      fz_rethrow(ctx);

    fz_try(ctx)
      pdf_run_page_contents(ctx, page, dc->dev, fz_concat(fz_invert_matrix(page_ctm), form), NULL);
    fz_always(ctx)
    {
      if (xf->clip && user_bbox)
        fz_pop_clip(ctx, dc->dev);
      fz_pop_clip(ctx, dc->dev);
    }
    fz_catch(ctx)
      fz_rethrow(ctx);
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
bool
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

  cursor_t fstart = cur + 1, fend = pdfstring_end(fstart, lim);
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
bool
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
bool
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

saved_path save_path(dvi_context *dc)
{
  saved_path saved = {dc->path, dc->path_clip};
  dc->path = NULL;
  dc->path_clip = 0;
  return saved;
}

void restore_path(fz_context *ctx, dvi_context *dc, saved_path saved)
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
void
run_pdf_form(fz_context *ctx, dvi_context *dc, dvi_state *sub, pdf_obj *form)
{
  fz_buffer *buf = pdf_load_stream(ctx, form);
  fz_try(ctx)
  {
    const char *data = (const char *)buf->data;
    run_pdf_code(ctx, dc, sub, data, data + buf->len);
  }
  fz_always(ctx)
    fz_drop_buffer(ctx, buf);
  fz_catch(ctx)
    fz_rethrow(ctx);
}


// Draw a form XObject in the current state, with its coordinate space mapped
// by `ctm`
void
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
void
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
bool special_uxobj(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
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
