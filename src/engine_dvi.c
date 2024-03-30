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

#include <stdlib.h>
#include <mupdf/fitz.h>
#include "engine.h"
#include "incdvi.h"

struct dvi_engine
{
  struct txp_engine_class *_class;
  fz_buffer *buffer;
  incdvi_t *dvi;
};

#define SELF struct dvi_engine *self = (struct dvi_engine*)_self

// Useful routines

TXP_ENGINE_DEF_CLASS;

static void engine_destroy(txp_engine *_self, fz_context *ctx)
{
  SELF;
  fz_drop_buffer(ctx, self->buffer);
  incdvi_free(ctx, self->dvi);
}

static fz_display_list *engine_render_page(txp_engine *_self,
                                           fz_context *ctx,
                                           int index)
{
  SELF;
  float width, height;
  incdvi_page_dim(self->dvi, self->buffer, index, &width, &height, NULL);
  fz_rect box = fz_make_rect(0, 0, width, height);
  fz_display_list *dl = fz_new_display_list(ctx, box);
  fz_device *dev = fz_new_list_device(ctx, dl);
  incdvi_render_page(ctx, self->dvi, self->buffer, index, dev);
  fz_close_device(ctx, dev);
  fz_drop_device(ctx, dev);
  return dl;
}

static bool engine_step(txp_engine *_self,
                        fz_context *ctx,
                        bool restart_if_needed)
{
  return 0;
}

static void engine_begin_changes(txp_engine *_self, fz_context *ctx)
{
}

static void engine_detect_changes(txp_engine *_self, fz_context *ctx)
{
}

static bool engine_end_changes(txp_engine *_self, fz_context *ctx)
{
  return 0;
}


static int engine_page_count(txp_engine *_self)
{
  SELF;
  return incdvi_page_count(self->dvi);
}

static txp_engine_status engine_get_status(txp_engine *_self)
{
  return DOC_TERMINATED;
}

static float engine_scale_factor(txp_engine *_self)
{
  SELF;
  return incdvi_tex_scale_factor(self->dvi);
}

static synctex_t *engine_synctex(txp_engine *_self, fz_buffer **buf)
{
  return NULL;
}

static fileentry_t *engine_find_file(txp_engine *_self, fz_context *ctx, const char *path)
{
  return NULL;
}

static void engine_notify_file_changes(txp_engine *_self,
                                       fz_context *ctx,
                                       fileentry_t *entry,
                                       int offset)
{
}

txp_engine *txp_create_dvi_engine(fz_context *ctx, const char *tectonic_path, const char *dvi_dir, const char *dvi_path)
{
  fz_buffer *buffer = fz_read_file(ctx, dvi_path);
  struct dvi_engine *self = fz_malloc_struct(ctx, struct dvi_engine);
  self->_class = &_class;
  self->buffer = buffer;
  bundle_server *bundle = bundle_server_start(ctx, tectonic_path, dvi_dir);
  self->dvi = incdvi_new(ctx, bundle_server_hooks(bundle));
  incdvi_update(ctx, self->dvi, buffer);
  return (txp_engine*)self;
}
