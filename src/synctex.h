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

#ifndef SYNCTEX_H_
#define SYNCTEX_H_

#include <mupdf/fitz/context.h>
#include <mupdf/fitz/buffer.h>

typedef struct synctex_s synctex_t;

synctex_t *synctex_new(fz_context *ctx);
void synctex_free(fz_context *ctx, synctex_t *stx);
void synctex_rollback(fz_context *ctx, synctex_t *stx, size_t offset);
void synctex_update(fz_context *ctx, synctex_t *stx, fz_buffer *buf);
int synctex_page_count(synctex_t *stx);
int synctex_input_count(synctex_t *stx);
void synctex_page_offset(fz_context *ctx, synctex_t *stx, unsigned index, int *bop, int *eop);
int synctex_input_offset(fz_context *ctx, synctex_t *stx, unsigned index);
void synctex_scan(fz_context *ctx, synctex_t *stx, fz_buffer *buf, const char *doc_dir, unsigned page, int x, int y);

int synctex_has_target(synctex_t *stx);
void synctex_set_target(synctex_t *stx, const char *path, int line);
int synctex_find_target(fz_context *ctx, synctex_t *stx, fz_buffer *buf,
                        int current, int *page, int *x, int *y);

#endif // SYNCTEX_H_
