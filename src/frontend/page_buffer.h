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

#ifndef PAGE_BUFFER_H_
#define PAGE_BUFFER_H_

#include <stdbool.h>
#include <stdint.h>
#include <SDL2/SDL.h>
#include <mupdf/fitz.h>

// Cache of the rasterized pages, in a texture a bit larger than the screen.
//
// Pages are laid out in "buffer space", a pixel space where pages are stacked
// vertically without overlapping (see txp_viewer_page_buffer_rect).
// Buffer space is mapped to the texture modulo its size: the texture caches
// a window of buffer space as tall as the texture, that follows the visible
// area. When scrolling, only the newly exposed parts are rasterized.

typedef struct txp_page_buffer_s txp_page_buffer;

// Filter applied to each line of pixels (RGB) after rasterization
typedef void (*txp_page_buffer_filter)(void *data, uint8_t *pixels, int width);

txp_page_buffer *txp_page_buffer_new(fz_context *ctx, SDL_Renderer *renderer);
void txp_page_buffer_free(fz_context *ctx, txp_page_buffer *pb);

// Forget all the contents (after changing the filter)
void txp_page_buffer_flush(txp_page_buffer *pb);

// Forget the contents of a page (its display list changed)
void txp_page_buffer_invalidate_page(txp_page_buffer *pb, int page);

// Prepare the cache for displaying a screen of the given size (in pixels),
// showing buffer space between rows y0 and y1.
// Returns false if no texture is available.
bool txp_page_buffer_begin(fz_context *ctx, txp_page_buffer *pb,
                           int screen_w, int screen_h, int y0, int y1);

// Make sure the visible part of a page is cached.
// - position: the page in buffer space
// - visible: the visible part of the page, in buffer space
// - dl, area: the display list and the area of it shown in position
//   (NULL for a blank page)
// Returns true if something has been rasterized.
bool txp_page_buffer_update(fz_context *ctx, txp_page_buffer *pb, int page,
                            fz_irect position, fz_irect visible,
                            fz_display_list *dl, fz_rect area,
                            txp_page_buffer_filter filter, void *filter_data);

// Draw rect (in buffer space, cached by a previous update) at (x, y) on the
// renderer
void txp_page_buffer_blit(txp_page_buffer *pb, fz_irect rect, int x, int y);

#endif // PAGE_BUFFER_H_
