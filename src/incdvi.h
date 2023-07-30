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

#ifndef INCDVI_H
#define INCDVI_H

#include <mupdf/fitz/buffer.h>
#include <mupdf/fitz/device.h>
#include <stdbool.h>

typedef struct incdvi_s incdvi_t;

incdvi_t *incdvi_new(fz_context *ctx, const char *tectonic_path, const char *document_directory);
void incdvi_free(fz_context *ctx, incdvi_t *d);
void incdvi_reset(incdvi_t *d);
void incdvi_update(fz_context *ctx, incdvi_t *d, fz_buffer *buf);
int incdvi_page_count(incdvi_t *d);
void incdvi_page_dim(incdvi_t *d, fz_buffer *buf, int page, float *width, float *height, bool *landscape);
void incdvi_render_page(fz_context *ctx, incdvi_t *d, fz_buffer *buf, int page, fz_device *dev);
void incdvi_find_page_loc(fz_context *ctx, incdvi_t *d, fz_buffer *buf, int page, float x, float y);
float incdvi_tex_scale_factor(incdvi_t *d);

#endif /*!INCDVI_H*/
