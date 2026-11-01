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

#include "mydvi.h"
#include "dvi_pdfstore.h"

dvi_context *dvi_context_new(fz_context *ctx, dvi_reshooks hooks)
{
  dvi_context *dc = fz_malloc_struct(ctx, dvi_context);

  dc->dev = NULL;
  dc->resmanager = dvi_resmanager_new(ctx, hooks);
  dc->pdfstore = dvi_pdfstore_new(ctx);
  dvi_scratch_init(&dc->scratch);

  dvi_state *st = &dc->root;
  st->fonts = dvi_fonttable_new(ctx);
  dvi_graphicstate_init(&st->gs, fz_identity);
  st->registers_stack.base = &dc->registers_stack[0];
  st->registers_stack.depth = 0;
  st->registers_stack.limit = sizeof(dc->registers_stack)/sizeof(dc->registers_stack[0]);
  st->gs_stack.base = &dc->gs_stack[0];
  st->gs_stack.depth = 0;
  st->gs_stack.limit = sizeof(dc->gs_stack)/sizeof(dc->gs_stack[0]);
  st->coords_stack.base = &dc->coords_stack[0];
  st->coords_stack.depth = 0;
  st->coords_stack.limit = sizeof(dc->coords_stack)/sizeof(dc->coords_stack[0]);
  st->base_ctm = fz_identity;
  st->resources = dvi_pdfstore_page_resources(dc->pdfstore);
  return dc;
}

void dvi_graphicstate_init(dvi_graphicstate *gs, fz_matrix ctm)
{
  *gs = (dvi_graphicstate){0,};
  gs->ctm = ctm;
  gs->line_width = 1;
  gs->miter_limit = 10;
  gs->fill_alpha = 1;
  gs->line_alpha = 1;
  gs->blend_mode = FZ_BLEND_NORMAL;
}

static void
dvi_context_set_device(fz_context *ctx, dvi_context *dc, fz_device *dev)
{
  if (dev == dc->dev)
    return;
  if (dc->dev)
    fz_drop_device(ctx, dc->dev);
  dc->dev = dev;
  if (dev)
    fz_keep_device(ctx, dev);
}

void dvi_context_free(fz_context *ctx, dvi_context *dc)
{
  dvi_context_set_device(ctx, dc, NULL);
  dvi_resmanager_free(ctx, dc->resmanager);
  dvi_pdfstore_free(ctx, dc->pdfstore);
  for (int i = 0; i < dc->colorspaces_count; i++)
    fz_drop_colorspace(ctx, dc->colorspaces[i]);
  fz_free(ctx, dc->colorstack.base);
  for (int i = 0; i < dc->pdfcolorstacks.capacity; i++)
    fz_free(ctx, dc->pdfcolorstacks.stacks[i].base);
  fz_free(ctx, dc->pdfcolorstacks.stacks);
  dvi_fonttable_free(ctx, dc->root.fonts);
  dvi_scratch_release(ctx, &dc->scratch);
  fz_free(ctx, dc);
}

static void drop_forms(fz_context *ctx, dvi_context *dc)
{
  for (int i = 0; i < dc->forms_depth; i++)
    pdf_drop_obj(ctx, dc->forms[i].dict);
  dc->forms_depth = 0;
}

void dvi_context_begin_frame(fz_context *ctx, dvi_context *dc, fz_device *dev, float page_height)
{
  dvi_context_set_device(ctx, dc, dev);
  dvi_state *st = &dc->root;
  st->registers_stack.depth = 0;
  st->gs_stack.depth = 0;
  st->coords_stack.depth = 0;

  // The default user space of PDF: origin at the bottom-left of the page
  fz_matrix page_ctm = fz_make_matrix(1, 0, 0, -1, 0, page_height);
  dvi_graphicstate_init(&st->gs, page_ctm);
  st->base_ctm = page_ctm;
  st->resources = dvi_pdfstore_page_resources(dc->pdfstore);

  // DVI origin is one inch right and down from the top-left corner
  st->origin.h = -72 / dc->scale;
  st->origin.v = (page_height - 72) / dc->scale;

  drop_forms(ctx, dc);
  dc->replay_depth = 0;
  dc->bscale_depth = 0;
  if (dc->path)
  {
    fz_drop_path(ctx, dc->path);
    dc->path = NULL;
  }
  dc->path_clip = 0;

  dc->colorstack.depth = 0;
  for (int i = 0; i < dc->pdfcolorstacks.capacity; i++)
    dc->pdfcolorstacks.stacks[i].depth = 0;
}

void dvi_context_end_frame(fz_context *ctx, dvi_context *dc)
{
  if (dc->forms_depth > 0)
  {
    fprintf(stderr, "pdf:bxobj: ending frame with %d unterminated forms\n", dc->forms_depth);
    drop_forms(ctx, dc);
  }
  dvi_scratch_clear(ctx, &dc->scratch);
  dvi_context_set_device(ctx, dc, NULL);

  if (dc->colorstack.depth > 0)
    fprintf(stderr, "default color stack: ending frame with %d colors\n", dc->colorstack.depth);

  for (int i = 0; i < dc->pdfcolorstacks.capacity; i++)
    if (dc->pdfcolorstacks.stacks[i].depth > 0)
      fprintf(stderr, "default color stack: ending frame with %d colors\n",
              dc->pdfcolorstacks.stacks[i].depth);
}

dvi_state *dvi_context_state(dvi_context *dc)
{
  return &dc->root;
}

bool dvi_state_enter_vf(dvi_context *dc, dvi_state *vfst, const dvi_state *st, dvi_fonttable *fonts, int font, fixed_t scale)
{
  float s = fixed_double(scale);
  vfst->version = DVI_VF;
  vfst->f = font;
  vfst->gs = st->gs;
  vfst->gs.ctm = fz_pre_scale(dvi_get_ctm(dc, st), s, s);
  vfst->origin.h = vfst->origin.v = 0;
  vfst->base_ctm = st->base_ctm;
  vfst->resources = st->resources;
  vfst->registers.h = 0;
  vfst->registers.v = 0;
  vfst->registers.x = 0;
  vfst->registers.y = 0;
  vfst->registers.w = 0;
  vfst->registers.z = 0;
  vfst->registers_stack.base = st->registers_stack.base + st->registers_stack.depth;
  vfst->registers_stack.limit = st->registers_stack.limit - st->registers_stack.depth;
  vfst->registers_stack.depth = 0;
  vfst->gs_stack.base = st->gs_stack.base + st->gs_stack.depth;
  vfst->gs_stack.limit = st->gs_stack.limit - st->gs_stack.depth;
  vfst->gs_stack.depth = 0;
  vfst->coords_stack.base = st->coords_stack.base + st->coords_stack.depth;
  vfst->coords_stack.limit = st->coords_stack.limit - st->coords_stack.depth;
  vfst->coords_stack.depth = 0;
  vfst->fonts = fonts;
  return 1;
}


void dvi_context_set_source(dvi_context *dc, const uint8_t *data, size_t len)
{
  dc->source = data;
  dc->source_len = len;
}

void dvi_context_invalidate_source(fz_context *ctx, dvi_context *dc, size_t offset)
{
  dvi_pdfstore_invalidate(ctx, dc->pdfstore, offset);
  // Forms are recorded at most up to the end of the scanned part of the DVI
  drop_forms(ctx, dc);
}
