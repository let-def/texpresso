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

#ifndef GENERIC_ENGINE_H_
#define GENERIC_ENGINE_H_

#include <mupdf/fitz/display-list.h>
#include "state.h"
#include "incdvi.h"
#include "synctex.h"

#define send(method, ...) \
  (send__extract_first(__VA_ARGS__, NULL)->_class->method((txp_engine*)__VA_ARGS__))

#define send__extract_first(x, ...) (x)

typedef struct txp_engine_s txp_engine;

txp_engine *txp_create_tex_engine(fz_context *ctx,
                                  const char *tectonic_path,
                                  const char *inclusion_path,
                                  const char *tex_dir,
                                  const char *tex_name);

txp_engine *txp_create_pdf_engine(fz_context *ctx, const char *pdf_path);

txp_engine *txp_create_dvi_engine(fz_context *ctx,
                                  const char *tectonic_path,
                                  const char *dvi_dir,
                                  const char *dvi_path);

typedef enum {
  DOC_RUNNING,
  DOC_BACK,
  DOC_TERMINATED
} txp_engine_status;

struct txp_engine_s {
  struct txp_engine_class *_class;
};

struct txp_engine_class
{
  void (*destroy)(txp_engine *self, fz_context *ctx);
  bool (*step)(txp_engine *self, fz_context *ctx, bool restart_if_needed);
  void (*begin_changes)(txp_engine *self, fz_context *ctx);
  void (*detect_changes)(txp_engine *self, fz_context *ctx);
  bool (*end_changes)(txp_engine *self, fz_context *ctx);
  int (*page_count)(txp_engine *self);
  fz_display_list *(*render_page)(txp_engine *self, fz_context *ctx, int page);
  txp_engine_status (*get_status)(txp_engine *self);
  float (*scale_factor)(txp_engine *self);
  synctex_t *(*synctex)(txp_engine *self, fz_buffer **buf);
  fileentry_t *(*find_file)(txp_engine *self, fz_context *ctx, const char *path);
  void (*notify_file_changes)(txp_engine *self, fz_context *ctx, fileentry_t *entry, int offset);
};

#define TXP_ENGINE_DEF_CLASS                                                \
  static void engine_destroy(txp_engine *_self, fz_context *ctx);           \
  static fz_display_list *engine_render_page(txp_engine *_self,             \
                                             fz_context *ctx, int page);    \
  static bool engine_step(txp_engine *_self, fz_context *ctx,               \
                          bool restart_if_needed);                          \
  static void engine_begin_changes(txp_engine *_self, fz_context *ctx);     \
  static void engine_detect_changes(txp_engine *_self, fz_context *ctx);    \
  static bool engine_end_changes(txp_engine *_self, fz_context *ctx);       \
  static int engine_page_count(txp_engine *_self);                          \
  static txp_engine_status engine_get_status(txp_engine *_self);            \
  static float engine_scale_factor(txp_engine *_self);                      \
  static synctex_t *engine_synctex(txp_engine *_self, fz_buffer **buf);     \
  static fileentry_t *engine_find_file(txp_engine *_self, fz_context *ctx,  \
                                       const char *path);                   \
  static void engine_notify_file_changes(txp_engine *self, fz_context *ctx, \
                                         fileentry_t *entry, int offset);   \
                                                                            \
  static struct txp_engine_class _class = {                                 \
      .destroy = engine_destroy,                                            \
      .step = engine_step,                                                  \
      .page_count = engine_page_count,                                      \
      .render_page = engine_render_page,                                    \
      .get_status = engine_get_status,                                      \
      .scale_factor = engine_scale_factor,                                  \
      .synctex = engine_synctex,                                            \
      .find_file = engine_find_file,                                        \
      .begin_changes = engine_begin_changes,                                \
      .detect_changes = engine_detect_changes,                              \
      .end_changes = engine_end_changes,                                    \
      .notify_file_changes = engine_notify_file_changes,                    \
  }

#endif // GENERIC_ENGINE_H_
