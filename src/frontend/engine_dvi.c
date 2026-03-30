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

#include <mupdf/fitz.h>
#include "engine.h"
#include "incdvi.h"

struct DviEngine
{
  struct EngineClass *_class;
  fz_buffer *buffer;
  IncDVI *dvi;
};

#define SELF struct DviEngine *self = (struct DviEngine*)_self

// Useful routines

TXP_ENGINE_DEF_CLASS;

static void engine_destroy(Engine *_self, fz_context *ctx)
{
  SELF;
  fz_drop_buffer(ctx, self->buffer);
  incdvi_free(ctx, self->dvi);
}

static fz_display_list *engine_render_page(Engine *_self,
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

static bool engine_step(Engine *_self,
                        fz_context *ctx,
                        bool restart_if_needed)
{
  return 0;
}

static void engine_begin_changes(Engine *_self, fz_context *ctx)
{
}

static void engine_detect_changes(Engine *_self, fz_context *ctx)
{
}

static bool engine_end_changes(Engine *_self, fz_context *ctx)
{
  return 0;
}


static int engine_page_count(Engine *_self)
{
  SELF;
  return incdvi_page_count(self->dvi);
}

static EngineStatus engine_get_status(Engine *_self)
{
  return DOC_TERMINATED;
}

static float engine_scale_factor(Engine *_self)
{
  SELF;
  return incdvi_tex_scale_factor(self->dvi);
}

static TexSynctex *engine_synctex(Engine *_self, fz_buffer **buf)
{
  return NULL;
}

static FileEntry *engine_find_file(Engine *_self, fz_context *ctx, const char *path)
{
  return NULL;
}

static void engine_notify_file_changes(Engine *_self,
                                       fz_context *ctx,
                                       FileEntry *entry,
                                       int offset)
{
}

Engine *create_dvi_engine(fz_context *ctx,
                          const char *dvi_path,
                          dvi_reshooks hooks)
{
  fz_buffer *buffer = fz_read_file(ctx, dvi_path);
  struct DviEngine *self = fz_malloc_struct(ctx, struct DviEngine);
  self->_class = &_class;
  self->buffer = buffer;
  self->dvi = incdvi_new(ctx, hooks);
  incdvi_update(ctx, self->dvi, buffer);
  return (Engine*)self;
}
