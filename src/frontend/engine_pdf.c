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

struct PdfEngine
{
  struct EngineClass *_class;
  char *path;
  int page_count;
  fz_document *doc;
  bool changed;
};

#define SELF struct PdfEngine *self = (struct PdfEngine*)_self

// Useful routines

TXP_ENGINE_DEF_CLASS;

static void engine_destroy(Engine *_self, fz_context *ctx)
{
  SELF;
  fz_free(ctx, self->path);
  fz_drop_document(ctx, self->doc);
}

static fz_display_list *engine_render_page(Engine *_self,
                                           fz_context *ctx,
                                           int index)
{
  SELF;
  fz_page *page = fz_load_page(ctx, self->doc, index);
  fz_display_list *dl = fz_new_display_list_from_page(ctx, page);
  fz_drop_page(ctx, page);
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
  SELF;
  fz_document *doc = fz_open_document(ctx, self->path);
  if (!doc)
    return;

  fz_drop_document(ctx, self->doc);
  self->doc = doc;
  self->page_count = fz_count_pages(ctx, doc);
  self->changed = 1;
}

static bool engine_end_changes(Engine *_self, fz_context *ctx)
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

static int engine_page_count(Engine *_self)
{
  SELF;
  return self->page_count;
}

static EngineStatus engine_get_status(Engine *_self)
{
  return DOC_TERMINATED;
}

static float engine_scale_factor(Engine *_self)
{
  return 1;
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

Engine *create_pdf_engine(fz_context *ctx, const char *pdf_path)
{
  fz_document *doc = fz_open_document(ctx, pdf_path);
  if (!doc)
    return NULL;

  struct PdfEngine *self = fz_malloc_struct(ctx, struct PdfEngine);
  self->_class = &_class;

  self->path = fz_strdup(ctx, pdf_path);
  self->doc = doc;
  self->page_count = fz_count_pages(ctx, doc);

  return (Engine*)self;
}
