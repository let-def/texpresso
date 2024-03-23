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

#ifndef _RENDERER_H_
#define _RENDERER_H_

#include <stdbool.h>
#include <SDL2/SDL.h>
#include <mupdf/fitz.h>

typedef struct txp_renderer_s txp_renderer;

txp_renderer *txp_renderer_new(fz_context *ctx, SDL_Renderer *sdl);
void txp_renderer_free(fz_context *ctx, txp_renderer *r);

enum txp_fit_mode
{
  FIT_WIDTH,
  FIT_PAGE,
};

typedef struct
{
  float zoom;
  enum txp_fit_mode fit;
  fz_point pan;
  bool crop, themed_color, invert_color;
  uint32_t background_color, foreground_color;
} txp_renderer_config;

void txp_renderer_set_contents(fz_context *ctx, txp_renderer *self, fz_display_list *dl);
fz_display_list *txp_renderer_get_contents(fz_context *ctx, txp_renderer *self);
txp_renderer_config *txp_renderer_get_config(fz_context* ctx, txp_renderer *self);
int txp_renderer_page_position(fz_context *ctx, txp_renderer *self, SDL_FRect *rect, fz_point *translate, float *scale);
void txp_renderer_render(fz_context *ctx, txp_renderer *self);
void txp_renderer_set_scale_factor(fz_context *ctx, txp_renderer *self, fz_point scale);
bool txp_renderer_start_selection(fz_context *ctx, txp_renderer *self, fz_point pt);
bool txp_renderer_drag_selection(fz_context *ctx, txp_renderer *self, fz_point pt);
bool txp_renderer_select_word(fz_context *ctx, txp_renderer *self, fz_point pt);
bool txp_renderer_select_char(fz_context *ctx, txp_renderer *self, fz_point pt);
void txp_renderer_screen_size(fz_context *ctx, txp_renderer *self, int *w, int *h);
fz_point txp_renderer_screen_to_document(fz_context *ctx, txp_renderer *self, fz_point pt);
fz_point txp_renderer_document_to_screen(fz_context *ctx, txp_renderer *self, fz_point pt);

#endif /*!_RENDERER_H_*/
