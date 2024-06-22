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

struct pdf_engine
{
  struct txp_engine_class *_class;
  char *path;
  int page_count;
  fz_document *doc;
  bool changed;
};

#define SELF struct pdf_engine *self = (struct pdf_engine*)_self

// Useful routines

TXP_ENGINE_DEF_CLASS;

static void engine_destroy(txp_engine *_self, fz_context *ctx)
{
  SELF;
  fz_free(ctx, self->path);
  fz_drop_document(ctx, self->doc);
}

static fz_display_list *engine_render_page(txp_engine *_self,
                                           fz_context *ctx,
                                           int index)
{
  SELF;
  fz_page *page = fz_load_page(ctx, self->doc, index);
  fz_display_list *dl = fz_new_display_list_from_page(ctx, page);
  fz_drop_page(ctx, page);
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
  SELF;
  fz_document *doc = fz_open_document(ctx, self->path);
  if (!doc)
    return;

  fz_drop_document(ctx, self->doc);
  self->doc = doc;
  self->page_count = fz_count_pages(ctx, doc);
  self->changed = 1;
}

static bool engine_end_changes(txp_engine *_self, fz_context *ctx)
{
  SELF;
  if (self->changed)
  {
    self->changed = 0;
    return 1;
  }
  else
  return 0;
}

static int engine_page_count(txp_engine *_self)
{
  SELF;
  return self->page_count;
}

static txp_engine_status engine_get_status(txp_engine *_self)
{
  return DOC_TERMINATED;
}

static float engine_scale_factor(txp_engine *_self)
{
  return 1;
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

txp_engine *txp_create_pdf_engine(fz_context *ctx, const char *pdf_path)
{
  fz_document *doc = fz_open_document(ctx, pdf_path);
  if (!doc)
    return NULL;

  struct pdf_engine *self = fz_malloc_struct(ctx, struct pdf_engine);
  self->_class = &_class;

  self->path = fz_strdup(ctx, pdf_path);
  self->doc = doc;
  self->page_count = fz_count_pages(ctx, doc);

  return (txp_engine*)self;
}
