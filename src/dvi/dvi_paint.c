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

// Painting: paths, patterns, shadings, color spaces and graphic state parameters

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


static void fill_rect_path(fz_context *ctx, fz_path *path, fz_rect r)
{
  fz_moveto(ctx, path, r.x0, r.y0);
  fz_lineto(ctx, path, r.x1, r.y0);
  fz_lineto(ctx, path, r.x1, r.y1);
  fz_lineto(ctx, path, r.x0, r.y1);
  fz_closepath(ctx, path);
}

void clip_rect(fz_context *ctx, dvi_context *dc, fz_rect r, fz_matrix ctm)
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


/************/
/* Painting */
/************/

fz_path *get_path(fz_context *ctx, dvi_context *dc)
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


// Finish the current path: paint it, then apply pending clipping
void paint(fz_context *ctx, dvi_context *dc, dvi_state *st, int op)
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

uint8_t
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
void colorspace_initial_color(fz_context *ctx, dvi_context *dc, uint8_t cs, float rgb[3])
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
void
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

int lookup_pattern(fz_context *ctx, dvi_context *dc, dvi_state *st, const char *name)
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
void
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


// Fill the page with a color
bool
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
