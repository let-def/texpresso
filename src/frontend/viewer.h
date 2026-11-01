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

#ifndef VIEWER_H_
#define VIEWER_H_

#include <stdbool.h>
#include <SDL2/SDL.h>
#include <mupdf/fitz.h>
#include "page_collection.h"

// Position of the pages on screen, and navigation.
//
// The viewer lays out pages of a txp_page_collection:
// - TXP_VIEW_SINGLE: the current page only, edge to edge, moving instantly;
// - TXP_VIEW_CONTINUOUS: all pages stacked vertically, with a margin around
//   them, animated scrolling and zooming, inertia and a scrollbar.
//
// Vertical positions are in document units (points), in the layout: the
// pages stacked from 0. Screen positions are in pixels of the renderer.

typedef enum
{
  TXP_VIEW_SINGLE,
  TXP_VIEW_CONTINUOUS,
} txp_view_mode;

typedef enum
{
  TXP_FIT_PAGE,
  TXP_FIT_WIDTH,
} txp_fit_mode;

// A point on a page, in the coordinates of its display list.
// page is -1 for a point outside of the pages.
typedef struct
{
  int page;
  float x, y;
} txp_doc_coord;

// Pages intersecting the screen, empty if first > last
typedef struct
{
  int first, last;
} txp_visible_range;

typedef enum
{
  TXP_ALIGN_TOP,
  TXP_ALIGN_BOTTOM,
} txp_align;

typedef struct
{
  int x, w, h;
  int thumb_y, thumb_h;
  float alpha;
  int target_alpha;
  bool dragging;
  int drag_offset;
} txp_viewer_scrollbar;

typedef struct
{
  txp_view_mode mode;
  txp_fit_mode fit;

  // Page requested in single page mode
  int page;
  // Page displayed in single page mode: the requested page once the engine
  // produced it, the previous one meanwhile, -1 if none
  int shown;
  // Alignment to apply once the page is available, -1 for none
  int pending_align;

  float zoom, target_zoom;
  // Horizontal offset of the pages from the center of the screen
  float x, target_x;
  // Vertical position of the top of the screen in the layout
  float y, target_y;
  // Vertical velocity, in document units per 1/60s
  float velocity;
  bool animating;
  // The pages are dragged with the mouse, velocity follows the motion
  bool dragging;
  // Moving because of the inertia of a scroll, snapping to page boundaries
  // when done
  bool coasting;

  // Derived from the state above by txp_viewer_update
  int win_w, win_h;
  float scale;   // pixels per document unit
  float gap;     // between pages, in document units
  int margin;    // around the pages, in pixels
  txp_viewer_scrollbar scrollbar;

  // Last frame, to detect changes
  struct {
    float x, y, scale;
    int win_w, win_h, thumb_y, thumb_h, alpha, page;
  } last;
} txp_viewer;

void txp_viewer_init(txp_viewer *v, txp_view_mode mode);

// Switch mode, keeping the point at the center of the screen in place
void txp_viewer_set_mode(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                         txp_view_mode mode);

// Advance animations by dt seconds, for a screen of w x h pixels.
// Returns true if the screen needs to be redrawn.
bool txp_viewer_update(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                       float dt, int w, int h);

// No animation in progress: no need to call update until the next input
bool txp_viewer_is_idle(const txp_viewer *v);

// Layout

txp_visible_range txp_viewer_visible_range(fz_context *ctx, txp_viewer *v,
                                           txp_page_collection *pc);

// Area of the page displayed (see txp_page_collection_bounds)
fz_rect txp_viewer_page_bounds(fz_context *ctx, txp_viewer *v,
                               txp_page_collection *pc, int page);

// Position of a page on screen
fz_irect txp_viewer_page_screen_rect(fz_context *ctx, txp_viewer *v,
                                     txp_page_collection *pc, int page);

// Position of a page in the buffer space of txp_page_buffer: pages are
// stacked without overlapping, with the same size as on screen
fz_irect txp_viewer_page_buffer_rect(fz_context *ctx, txp_viewer *v,
                                     txp_page_collection *pc, int page);

txp_doc_coord txp_viewer_screen_to_doc(fz_context *ctx, txp_viewer *v,
                                       txp_page_collection *pc, float x, float y);
fz_point txp_viewer_doc_to_screen(fz_context *ctx, txp_viewer *v,
                                  txp_page_collection *pc, txp_doc_coord c);

// A point of the screen in the coordinates of a page, even outside of it
fz_point txp_viewer_screen_to_page(fz_context *ctx, txp_viewer *v,
                                   txp_page_collection *pc, int page,
                                   float x, float y);

// Page at the center of the screen, -1 if none
int txp_viewer_current_page(fz_context *ctx, txp_viewer *v, txp_page_collection *pc);

// Navigation

// Move by (dx, dy) pixels at once
void txp_viewer_scroll(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                       float dx, float dy);

// Dragging: txp_viewer_scroll follows the mouse, the pages keep moving
// when released (continuous mode)
void txp_viewer_drag_begin(txp_viewer *v);
void txp_viewer_drag_end(txp_viewer *v);

// Give some vertical velocity (in pixels per 1/60s)
void txp_viewer_fling(txp_viewer *v, float dy);

// Stop any movement
void txp_viewer_stop(txp_viewer *v);

// Single page mode: move by a fraction of the screen height, returns false
// if already at the top (fraction < 0) or bottom (fraction > 0) of the page.
// Continuous mode: move by a fraction of the screen, smoothly.
bool txp_viewer_scroll_screen(fz_context *ctx, txp_viewer *v,
                              txp_page_collection *pc, float fraction);

// Continuous mode: move to the next (dir > 0) or previous (dir < 0) stop.
// The stops are the positions showing a page as fully as possible: centered
// if it fits on screen, otherwise one screen at a time, from the top-aligned
// to the bottom-aligned positions. A page boundary is never stopped across.
void txp_viewer_step(fz_context *ctx, txp_viewer *v, txp_page_collection *pc, int dir);

// Continuous mode: move to the next (dir > 0) or previous page (dir < 0)
void txp_viewer_step_page(fz_context *ctx, txp_viewer *v, txp_page_collection *pc, int dir);

// Show a page, top or bottom first
void txp_viewer_goto_page(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                          int page, txp_align align);

// Zoom to zoom, keeping the point at (x, y) on screen in place
void txp_viewer_zoom_at(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                        float zoom, float x, float y);

void txp_viewer_set_fit(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                        txp_fit_mode fit);

// Continuous mode: zoom so that the page fills the width of the screen,
// centered on a point
void txp_viewer_fit_width_at(fz_context *ctx, txp_viewer *v,
                             txp_page_collection *pc, txp_doc_coord c);

// Scroll if needed for a point to be visible, not too close to the edges
void txp_viewer_reveal(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                       txp_doc_coord c);

// Point at the center of the screen, to restore the position after the
// layout changed (cropping, mode switch)
txp_doc_coord txp_viewer_get_anchor(fz_context *ctx, txp_viewer *v,
                                    txp_page_collection *pc);
void txp_viewer_set_anchor(fz_context *ctx, txp_viewer *v,
                           txp_page_collection *pc, txp_doc_coord c);

// Scrollbar (continuous mode)

// Mouse events (in pixels), return true if the scrollbar handled them
bool txp_viewer_scrollbar_press(fz_context *ctx, txp_viewer *v,
                                txp_page_collection *pc, int x, int y);
bool txp_viewer_scrollbar_motion(fz_context *ctx, txp_viewer *v,
                                 txp_page_collection *pc, int x, int y);
bool txp_viewer_scrollbar_release(txp_viewer *v);

void txp_viewer_draw_scrollbar(txp_viewer *v, SDL_Renderer *renderer);

#endif // VIEWER_H_
