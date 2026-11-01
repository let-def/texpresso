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

// Graphic state, transformations and the pdf: specials manipulating them

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
void concat_around_current_point(dvi_context *dc, dvi_state *st, fz_matrix m)
{
  fz_point cp = current_point(dc, st);
  m = fz_concat(fz_translate(-cp.x, -cp.y), fz_concat(m, fz_translate(cp.x, cp.y)));
  st->gs.ctm = fz_concat(m, st->gs.ctm);
}

void pop_clips(fz_context *ctx, dvi_context *dc, int from, int to)
{
  if (dc->dev)
    for (int i = to; i < from; ++i)
      fz_pop_clip(ctx, dc->dev);
}

bool gs_push(dvi_state *st)
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

bool gs_pop(fz_context *ctx, dvi_context *dc, dvi_state *st)
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
void
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

/************************/
/* pdf: specials        */
/************************/

// pdf:btrans [transformation]
bool
special_btrans(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
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
bool
special_bcontent(fz_context *ctx, dvi_context *dc, dvi_state *st)
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
bool
special_econtent(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  if (st->coords_stack.depth == 0)
    return 0;
  st->origin = st->coords_stack.base[--st->coords_stack.depth];
  return gs_pop(ctx, dc, st);
}

// pdf:literal [direct] CODE
bool
special_literal(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
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
    return run_pdf_code(ctx, dc, st, cur, lim);

  fz_point cp = current_point(dc, st);
  st->gs.ctm = fz_pre_translate(st->gs.ctm, cp.x, cp.y);
  bool result = run_pdf_code(ctx, dc, st, cur, lim);
  st->gs.ctm = fz_pre_translate(st->gs.ctm, -cp.x, -cp.y);
  return result;
}

// pdf:content CODE
bool
special_content(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  // Like dvipdfmx, this is "q cm CODE Q": CODE does not have to be balanced
  // (trimclip opens a group with "pdf:content q" and closes it with
  // "pdf:literal direct Q")
  if (!gs_push(st))
    return 0;
  fz_point cp = current_point(dc, st);
  st->gs.ctm = fz_pre_translate(st->gs.ctm, cp.x, cp.y);
  bool result = run_pdf_code(ctx, dc, st, cur, lim);
  gs_pop(ctx, dc, st);
  return result;
}

/******************/
/* x: specials    */
/******************/

// x:scale SX SY
bool xetex_scale(dvi_context *dc, dvi_state *st, float sx, float sy)
{
  concat_around_current_point(dc, st, fz_scale(sx, sy));
  return 1;
}

// x:bscale SX SY ... x:escale
bool xetex_bscale(dvi_context *dc, dvi_state *st, float sx, float sy)
{
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

bool xetex_escale(dvi_context *dc, dvi_state *st)
{
  if (dc->bscale_depth == 0)
    return 0;
  dc->bscale_depth -= 1;
  if (dc->bscale_depth < 16)
    concat_around_current_point(dc, st, fz_scale(dc->bscale[dc->bscale_depth][0],
                                                 dc->bscale[dc->bscale_depth][1]));
  return 1;
}

// x:rotate ANGLE
bool xetex_rotate(dvi_context *dc, dvi_state *st, float angle)
{
  concat_around_current_point(dc, st, fz_rotate(angle));
  return 1;
}
