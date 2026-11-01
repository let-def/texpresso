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

#include <math.h>
#include "viewer.h"

// Continuous mode: space around the pages (pixels), between pages
// (document units, bounded by the margin)
#define MARGIN 15
#define PAGE_GAP 10

// Velocity decay per 1/60s, when coasting and when dragging
#define FRICTION 0.95f
#define DRAG_FRICTION 0.5f

// Animation speed: part of the distance to the target covered per 1/60s
#define ANIMATION_SPEED 0.2f

// Moving by a screen keeps that much of the previous screen visible
#define SCREEN_STEP 0.85f

// Snap to a page boundary when coasting stops that close to it (fraction of
// the screen height)
#define SNAP_DISTANCE 0.15f

#define MIN_ZOOM 0.1f
#define MAX_ZOOM 20.0f

#define SCROLLBAR_WIDTH 18
#define SCROLLBAR_MIN_THUMB 20

static bool continuous(const txp_viewer *v)
{
  return v->mode == TXP_VIEW_CONTINUOUS;
}

void txp_viewer_init(txp_viewer *v, txp_view_mode mode)
{
  *v = (txp_viewer){0,};
  v->mode = mode;
  v->fit = TXP_FIT_WIDTH;
  v->pending_align = -1;
  v->zoom = v->target_zoom = 1;
  v->win_w = v->win_h = 1;
  v->scale = 1;
  v->margin = continuous(v) ? MARGIN : 0;
  v->shown = -1;
  v->last.page = -1;
}

/* Layout */

fz_rect txp_viewer_page_bounds(fz_context *ctx, txp_viewer *v,
                               txp_page_collection *pc, int page)
{
  return txp_page_collection_bounds(ctx, pc, page, continuous(v));
}

static int layout_count(const txp_viewer *v, const txp_page_collection *pc)
{
  int count = txp_page_collection_count(pc);
  if (continuous(v))
    return count;
  return (v->shown >= 0 && v->shown < count) ? 1 : 0;
}

static bool in_layout(const txp_viewer *v, const txp_page_collection *pc, int page)
{
  if (continuous(v))
    return page >= 0 && page < txp_page_collection_count(pc);
  return page == v->shown && layout_count(v, pc) > 0;
}

static float layout_top(const txp_viewer *v, const txp_page_collection *pc,
                        int page, float gap)
{
  return continuous(v) ? txp_page_collection_top(pc, page, gap) : 0;
}

static float page_height(fz_context *ctx, txp_viewer *v,
                         txp_page_collection *pc, int page)
{
  fz_rect b = txp_viewer_page_bounds(ctx, v, pc, page);
  return b.y1 - b.y0;
}

static float layout_height(fz_context *ctx, txp_viewer *v,
                           txp_page_collection *pc, float gap)
{
  if (layout_count(v, pc) == 0)
    return 0;
  if (continuous(v))
    return txp_page_collection_height(pc, gap);
  return page_height(ctx, v, pc, v->shown);
}

static int layout_find(const txp_viewer *v, const txp_page_collection *pc, float y)
{
  if (layout_count(v, pc) == 0)
    return -1;
  if (continuous(v))
    return txp_page_collection_find(pc, y, v->gap);
  return v->shown;
}

static float compute_scale(fz_context *ctx, txp_viewer *v,
                           txp_page_collection *pc, float zoom)
{
  int ref = continuous(v) ? 0 : v->shown;
  fz_rect b = txp_viewer_page_bounds(ctx, v, pc, ref);
  if (fz_is_empty_rect(b))
    b = fz_make_rect(0, 0, 612, 792);
  float aw = fz_max(v->win_w - 2 * v->margin, 1);
  float ah = fz_max(v->win_h - 2 * v->margin, 1);
  float s = aw / (b.x1 - b.x0);
  if (v->fit == TXP_FIT_PAGE)
    s = fz_min(s, ah / (b.y1 - b.y0));
  return zoom * s;
}

static float compute_gap(const txp_viewer *v, float scale)
{
  return continuous(v) ? fz_min(PAGE_GAP, v->margin / scale) : 0;
}

// Single page mode: show the requested page once it is available, keep
// showing the previous one while it loads
static void update_shown(txp_viewer *v, txp_page_collection *pc)
{
  if (txp_page_collection_has_contents(pc, v->page) ||
      v->shown >= txp_page_collection_count(pc))
    v->shown = v->page < txp_page_collection_count(pc) ? v->page : -1;
}

// Recompute scale and gap after changing zoom or layout
static void relayout(fz_context *ctx, txp_viewer *v, txp_page_collection *pc)
{
  update_shown(v, pc);
  v->margin = continuous(v) ? MARGIN : 0;
  v->scale = compute_scale(ctx, v, pc, v->zoom);
  v->gap = compute_gap(v, v->scale);
}

// Range of vertical positions
static void vertical_limits(fz_context *ctx, txp_viewer *v,
                            txp_page_collection *pc, float scale,
                            float *lo, float *hi)
{
  float vis = v->win_h / scale;
  float m = v->margin / scale;
  float total = layout_height(ctx, v, pc, compute_gap(v, scale));
  if (total + 2 * m <= vis)
  {
    // Fits on screen: centered
    *lo = *hi = (total - vis) / 2;
  }
  else
  {
    *lo = -m;
    *hi = total - vis + m;
  }
}

static float clampf(float x, float lo, float hi)
{
  return x < lo ? lo : (x > hi ? hi : x);
}

// Range of horizontal offsets for a page of width w (centered if it fits)
static float horizontal_limit(const txp_viewer *v, float w, float scale)
{
  float vis = v->win_w / scale;
  if (w <= vis)
    return 0;
  return (w - vis) / 2 + v->margin / scale;
}

static fz_rect page_screen_rectf(fz_context *ctx, txp_viewer *v,
                                 txp_page_collection *pc, int page)
{
  fz_rect b = txp_viewer_page_bounds(ctx, v, pc, page);
  float w = b.x1 - b.x0, h = b.y1 - b.y0;
  float limit = horizontal_limit(v, w, v->scale);
  float x = clampf(v->x, -limit, limit);
  fz_rect r;
  r.x0 = v->win_w / 2.0f - (w / 2 + x) * v->scale;
  r.y0 = (layout_top(v, pc, page, v->gap) - v->y) * v->scale;
  r.x1 = r.x0 + w * v->scale;
  r.y1 = r.y0 + h * v->scale;
  return r;
}

static void page_pixel_size(fz_context *ctx, txp_viewer *v,
                            txp_page_collection *pc, int page, int *w, int *h)
{
  fz_rect b = txp_viewer_page_bounds(ctx, v, pc, page);
  *w = (int)((b.x1 - b.x0) * v->scale + 0.5f);
  *h = (int)((b.y1 - b.y0) * v->scale + 0.5f);
}

fz_irect txp_viewer_page_screen_rect(fz_context *ctx, txp_viewer *v,
                                     txp_page_collection *pc, int page)
{
  if (!in_layout(v, pc, page))
    return fz_empty_irect;
  fz_rect r = page_screen_rectf(ctx, v, pc, page);
  int w, h;
  page_pixel_size(ctx, v, pc, page, &w, &h);
  int x0 = floorf(r.x0), y0 = floorf(r.y0);
  return fz_make_irect(x0, y0, x0 + w, y0 + h);
}

fz_irect txp_viewer_page_buffer_rect(fz_context *ctx, txp_viewer *v,
                                     txp_page_collection *pc, int page)
{
  if (page < 0 || page >= txp_page_collection_count(pc))
    return fz_empty_irect;
  // Rounding might make consecutive pages overlap by a pixel: leave two
  // pixels between them
  int w, h;
  page_pixel_size(ctx, v, pc, page, &w, &h);
  int y0 = (int)floorf(txp_page_collection_top(pc, page, 0) * v->scale) + 2 * page;
  return fz_make_irect(0, y0, w, y0 + h);
}

txp_visible_range txp_viewer_visible_range(fz_context *ctx, txp_viewer *v,
                                           txp_page_collection *pc)
{
  txp_visible_range vr = {0, -1};
  if (layout_count(v, pc) == 0)
    return vr;
  if (continuous(v))
  {
    vr.first = layout_find(v, pc, v->y);
    vr.last = layout_find(v, pc, v->y + v->win_h / v->scale);
  }
  else
    vr.first = vr.last = v->shown;
  return vr;
}

fz_point txp_viewer_screen_to_page(fz_context *ctx, txp_viewer *v,
                                   txp_page_collection *pc, int page,
                                   float x, float y)
{
  fz_rect b = txp_viewer_page_bounds(ctx, v, pc, page);
  fz_rect r = page_screen_rectf(ctx, v, pc, page);
  return fz_make_point(b.x0 + (x - r.x0) / v->scale,
                       b.y0 + (y - r.y0) / v->scale);
}

txp_doc_coord txp_viewer_screen_to_doc(fz_context *ctx, txp_viewer *v,
                                       txp_page_collection *pc, float x, float y)
{
  txp_doc_coord c = {-1, 0, 0};
  int page = layout_find(v, pc, v->y + y / v->scale);
  if (page < 0)
    return c;
  fz_rect r = page_screen_rectf(ctx, v, pc, page);
  if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1)
    return c;
  fz_point p = txp_viewer_screen_to_page(ctx, v, pc, page, x, y);
  c.page = page;
  c.x = p.x;
  c.y = p.y;
  return c;
}

fz_point txp_viewer_doc_to_screen(fz_context *ctx, txp_viewer *v,
                                  txp_page_collection *pc, txp_doc_coord c)
{
  fz_rect b = txp_viewer_page_bounds(ctx, v, pc, c.page);
  fz_rect r = page_screen_rectf(ctx, v, pc, c.page);
  return fz_make_point(r.x0 + (c.x - b.x0) * v->scale,
                       r.y0 + (c.y - b.y0) * v->scale);
}

int txp_viewer_current_page(fz_context *ctx, txp_viewer *v, txp_page_collection *pc)
{
  if (!continuous(v))
    return v->page;
  return layout_find(v, pc, v->y + v->win_h / v->scale / 2);
}

/* Animation */

static void stop_animation(txp_viewer *v)
{
  v->animating = false;
  v->target_zoom = v->zoom;
  v->target_x = v->x;
  v->target_y = v->y;
}

static void animate_to(txp_viewer *v, float zoom, float x, float y)
{
  v->target_zoom = zoom;
  v->target_x = x;
  v->target_y = y;
  v->velocity = 0;
  v->coasting = false;
  v->animating = true;
}

// Apply a pending alignment once the page is available (single page mode)
static void apply_pending(fz_context *ctx, txp_viewer *v, txp_page_collection *pc)
{
  if (v->pending_align < 0)
    return;
  relayout(ctx, v, pc);
  if (continuous(v) || v->shown != v->page)
    return;
  float lo, hi;
  vertical_limits(ctx, v, pc, v->scale, &lo, &hi);
  v->y = v->pending_align == TXP_ALIGN_TOP ? lo : hi;
  v->pending_align = -1;
  stop_animation(v);
}

// Position showing a page in full if it fits, centered, or its top
static bool page_fits(fz_context *ctx, txp_viewer *v, txp_page_collection *pc, int page)
{
  float vis = v->win_h / v->scale, m = v->margin / v->scale;
  return page_height(ctx, v, pc, page) + 2 * m <= vis;
}

static float page_centered(fz_context *ctx, txp_viewer *v,
                           txp_page_collection *pc, int page)
{
  float vis = v->win_h / v->scale;
  return layout_top(v, pc, page, v->gap) + (page_height(ctx, v, pc, page) - vis) / 2;
}

static float page_top_aligned(fz_context *ctx, txp_viewer *v,
                              txp_page_collection *pc, int page)
{
  return layout_top(v, pc, page, v->gap) - v->margin / v->scale;
}

static float page_bottom_aligned(fz_context *ctx, txp_viewer *v,
                                 txp_page_collection *pc, int page)
{
  float vis = v->win_h / v->scale, m = v->margin / v->scale;
  return layout_top(v, pc, page, v->gap) + page_height(ctx, v, pc, page) + m - vis;
}

// When coasting stops with a page boundary in view, close to a position
// showing a single page, move there: avoid resting with the bottom of a page
// and the top of the next one on screen
static void snap_to_page(fz_context *ctx, txp_viewer *v, txp_page_collection *pc)
{
  float vis = v->win_h / v->scale, m = v->margin / v->scale;
  int p0 = layout_find(v, pc, v->y + m);
  int p1 = layout_find(v, pc, v->y + vis - m);
  if (p0 < 0 || p0 == p1)
    return;

  float best = INFINITY;
  for (int p = p0; p <= p1; p++)
  {
    // Only at reading zoom levels, not when many pages fit on screen
    if (page_height(ctx, v, pc, p) < vis / 2)
      return;
    float stops[2];
    int n = 0;
    if (page_fits(ctx, v, pc, p))
      stops[n++] = page_centered(ctx, v, pc, p);
    else
    {
      stops[n++] = page_top_aligned(ctx, v, pc, p);
      stops[n++] = page_bottom_aligned(ctx, v, pc, p);
    }
    for (int i = 0; i < n; i++)
      if (fabsf(stops[i] - v->y) < fabsf(best - v->y))
        best = stops[i];
  }

  if (fabsf(best - v->y) < SNAP_DISTANCE * vis)
    animate_to(v, v->zoom, v->x, best);
}

static void update_scrollbar(fz_context *ctx, txp_viewer *v,
                             txp_page_collection *pc, float dt)
{
  txp_viewer_scrollbar *sb = &v->scrollbar;
  sb->w = SCROLLBAR_WIDTH;
  sb->x = v->win_w - sb->w;
  sb->h = v->win_h;

  float lo, hi;
  vertical_limits(ctx, v, pc, v->scale, &lo, &hi);
  float vis = v->win_h / v->scale;

  if (!continuous(v) || hi <= lo)
  {
    sb->thumb_h = sb->thumb_y = 0;
    sb->target_alpha = 0;
    sb->dragging = false;
  }
  else
  {
    float ratio = vis / (hi - lo + vis);
    sb->thumb_h = fz_maxi(sb->h * ratio, SCROLLBAR_MIN_THUMB);
    if (sb->thumb_h > sb->h)
      sb->thumb_h = sb->h;
    float norm = clampf((v->y - lo) / (hi - lo), 0, 1);
    sb->thumb_y = norm * (sb->h - sb->thumb_h);
  }

  // Fade in or out in 0.2s
  float step = dt / 0.2f * 256;
  if (sb->alpha < sb->target_alpha)
    sb->alpha = fz_min(sb->alpha + step, sb->target_alpha);
  else if (sb->alpha > sb->target_alpha)
    sb->alpha = fz_max(sb->alpha - step, sb->target_alpha);
}

bool txp_viewer_update(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                       float dt, int w, int h)
{
  v->win_w = w > 0 ? w : 1;
  v->win_h = h > 0 ? h : 1;
  float ts = dt * 60;

  if (v->animating && ts > 0)
  {
    float k = 1 - powf(1 - ANIMATION_SPEED, ts);
    v->zoom += (v->target_zoom - v->zoom) * k;
    v->x += (v->target_x - v->x) * k;
    v->y += (v->target_y - v->y) * k;
    if (fabsf(v->zoom - v->target_zoom) < 0.001f * v->target_zoom &&
        fabsf(v->x - v->target_x) * v->scale < 0.5f &&
        fabsf(v->y - v->target_y) * v->scale < 0.5f)
    {
      v->zoom = v->target_zoom;
      v->x = v->target_x;
      v->y = v->target_y;
      v->animating = false;
    }
  }

  relayout(ctx, v, pc);
  apply_pending(ctx, v, pc);

  float lo, hi;
  vertical_limits(ctx, v, pc, v->scale, &lo, &hi);

  if (v->dragging)
    v->velocity *= powf(DRAG_FRICTION, ts);
  else if (!v->animating && v->velocity != 0)
  {
    v->y += v->velocity * ts;
    v->velocity *= powf(FRICTION, ts);
    if (v->y <= lo || v->y >= hi)
      v->velocity = 0;
    if (fabsf(v->velocity) * v->scale < 0.2f)
    {
      v->velocity = 0;
      v->y = clampf(v->y, lo, hi);
      if (v->coasting)
      {
        v->coasting = false;
        snap_to_page(ctx, v, pc);
      }
    }
  }

  v->y = clampf(v->y, lo, hi);
  if (v->animating)
  {
    // Targets out of range would never be reached
    float tlo, thi;
    vertical_limits(ctx, v, pc, compute_scale(ctx, v, pc, v->target_zoom),
                    &tlo, &thi);
    v->target_y = clampf(v->target_y, tlo, thi);
  }

  // Horizontal range: the widest visible page
  txp_visible_range vr = txp_viewer_visible_range(ctx, v, pc);
  float limit = 0;
  for (int i = vr.first; i <= vr.last; i++)
  {
    fz_rect b = txp_viewer_page_bounds(ctx, v, pc, i);
    limit = fz_max(limit, horizontal_limit(v, b.x1 - b.x0, v->scale));
  }
  if (vr.first <= vr.last)
  {
    v->x = clampf(v->x, -limit, limit);
    if (v->animating)
      v->target_x = clampf(v->target_x, -limit, limit);
  }

  update_scrollbar(ctx, v, pc, dt);

  txp_viewer_scrollbar *sb = &v->scrollbar;
  bool changed =
      v->last.x != v->x || v->last.y != v->y || v->last.scale != v->scale ||
      v->last.win_w != v->win_w || v->last.win_h != v->win_h ||
      v->last.thumb_y != sb->thumb_y || v->last.thumb_h != sb->thumb_h ||
      v->last.alpha != (int)sb->alpha || v->last.page != v->shown;
  v->last.x = v->x;
  v->last.y = v->y;
  v->last.scale = v->scale;
  v->last.win_w = v->win_w;
  v->last.win_h = v->win_h;
  v->last.thumb_y = sb->thumb_y;
  v->last.thumb_h = sb->thumb_h;
  v->last.alpha = (int)sb->alpha;
  v->last.page = v->shown;
  return changed;
}

bool txp_viewer_is_idle(const txp_viewer *v)
{
  return !v->animating && v->velocity == 0 &&
         (int)v->scrollbar.alpha == v->scrollbar.target_alpha;
}

/* Navigation */

void txp_viewer_scroll(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                       float dx, float dy)
{
  stop_animation(v);
  v->coasting = false;
  v->x += dx / v->scale;
  v->y += dy / v->scale;
  if (v->dragging)
    v->velocity = v->velocity * 0.6f + (dy / v->scale) * 0.4f;
  else
    v->velocity = 0;
  // Keep the position in range, so that moving back responds immediately
  float lo, hi;
  vertical_limits(ctx, v, pc, v->scale, &lo, &hi);
  v->y = clampf(v->y, lo, hi);
}

void txp_viewer_drag_begin(txp_viewer *v)
{
  stop_animation(v);
  v->dragging = true;
  v->coasting = false;
  v->velocity = 0;
}

void txp_viewer_drag_end(txp_viewer *v)
{
  if (!v->dragging)
    return;
  v->dragging = false;
  if (continuous(v) && v->velocity != 0)
    v->coasting = true;
  else
    v->velocity = 0;
}

void txp_viewer_fling(txp_viewer *v, float dy)
{
  stop_animation(v);
  v->velocity += dy / v->scale;
  v->coasting = true;
}

void txp_viewer_stop(txp_viewer *v)
{
  stop_animation(v);
  v->velocity = 0;
  v->coasting = false;
  v->dragging = false;
}

bool txp_viewer_scroll_screen(fz_context *ctx, txp_viewer *v,
                              txp_page_collection *pc, float fraction)
{
  float lo, hi;
  float vis = v->win_h / v->scale, eps = 0.5f / v->scale;

  if (!continuous(v))
  {
    vertical_limits(ctx, v, pc, v->scale, &lo, &hi);
    if (fraction > 0 ? v->y >= hi - eps : v->y <= lo + eps)
      return false;
    txp_viewer_stop(v);
    v->y = clampf(v->y + fraction * vis, lo, hi);
    v->target_y = v->y;
    return true;
  }

  float zoom = v->animating ? v->target_zoom : v->zoom;
  float x = v->animating ? v->target_x : v->x;
  float y = v->animating ? v->target_y : v->y;
  vertical_limits(ctx, v, pc, compute_scale(ctx, v, pc, zoom), &lo, &hi);
  animate_to(v, zoom, x, clampf(y + fraction * vis, lo, hi));
  return true;
}

void txp_viewer_step(fz_context *ctx, txp_viewer *v, txp_page_collection *pc, int dir)
{
  if (!continuous(v))
  {
    txp_viewer_scroll_screen(ctx, v, pc, dir * SCREEN_STEP);
    return;
  }

  int count = layout_count(v, pc);
  if (count == 0)
    return;

  float vis = v->win_h / v->scale, m = v->margin / v->scale;
  float eps = 0.5f / v->scale, step = SCREEN_STEP * vis;
  // Continue from where an animation in progress is going
  float cur = v->animating ? v->target_y : v->y;
  float lo, hi;
  vertical_limits(ctx, v, pc, v->scale, &lo, &hi);
  float target;

  if (dir > 0)
  {
    // Page at the top of the screen
    int p = layout_find(v, pc, cur + m);
    float bottom = page_bottom_aligned(ctx, v, pc, p);
    if (bottom > cur + eps)
    {
      // The bottom of the page is not visible yet
      if (page_fits(ctx, v, pc, p))
        target = page_centered(ctx, v, pc, p);
      else
        target = fz_min(cur + step, bottom);
    }
    else
    {
      // Show the next page
      target = hi;
      for (int q = p + 1; q < count; q++)
      {
        float t = page_fits(ctx, v, pc, q) ? page_centered(ctx, v, pc, q)
                                           : page_top_aligned(ctx, v, pc, q);
        if (t > cur + eps)
        {
          target = t;
          break;
        }
      }
    }
  }
  else
  {
    // Page at the bottom of the screen
    int p = layout_find(v, pc, cur + vis - m);
    float top = page_top_aligned(ctx, v, pc, p);
    if (top < cur - eps)
    {
      // The top of the page is not visible yet
      if (page_fits(ctx, v, pc, p))
        target = page_centered(ctx, v, pc, p);
      else
        target = fz_max(cur - step, top);
    }
    else
    {
      // Show the previous page, from its bottom
      target = lo;
      for (int q = p - 1; q >= 0; q--)
      {
        float t = page_fits(ctx, v, pc, q) ? page_centered(ctx, v, pc, q)
                                           : page_bottom_aligned(ctx, v, pc, q);
        if (t < cur - eps)
        {
          target = t;
          break;
        }
      }
    }
  }

  animate_to(v, v->zoom, v->x, clampf(target, lo, hi));
}

// Position showing a page from its top, or centered if it fits
static float page_position(fz_context *ctx, txp_viewer *v,
                           txp_page_collection *pc, int page)
{
  return page_fits(ctx, v, pc, page) ? page_centered(ctx, v, pc, page)
                                     : page_top_aligned(ctx, v, pc, page);
}

void txp_viewer_step_page(fz_context *ctx, txp_viewer *v, txp_page_collection *pc, int dir)
{
  int count = layout_count(v, pc);
  if (!continuous(v) || count == 0)
    return;

  float eps = 0.5f / v->scale;
  float cur = v->animating ? v->target_y : v->y;
  float lo, hi;
  vertical_limits(ctx, v, pc, v->scale, &lo, &hi);
  float target = dir > 0 ? hi : lo;
  int p = layout_find(v, pc, cur);

  if (dir > 0)
  {
    for (int q = fz_maxi(p - 1, 0); q < count; q++)
    {
      float t = page_position(ctx, v, pc, q);
      if (t > cur + eps)
      {
        target = t;
        break;
      }
    }
  }
  else
  {
    for (int q = fz_mini(p + 1, count - 1); q >= 0; q--)
    {
      float t = page_position(ctx, v, pc, q);
      if (t < cur - eps)
      {
        target = t;
        break;
      }
    }
  }

  animate_to(v, v->zoom, v->x, clampf(target, lo, hi));
}

void txp_viewer_goto_page(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                          int page, txp_align align)
{
  if (page < 0)
    return;

  if (!continuous(v))
  {
    v->page = page;
    v->pending_align = align;
    v->velocity = 0;
    apply_pending(ctx, v, pc);
    return;
  }

  int count = layout_count(v, pc);
  if (count == 0)
    return;
  if (page >= count)
    page = count - 1;
  float y;
  if (page_fits(ctx, v, pc, page) || align == TXP_ALIGN_TOP)
    y = page_position(ctx, v, pc, page);
  else
    y = page_bottom_aligned(ctx, v, pc, page);
  float lo, hi;
  vertical_limits(ctx, v, pc, v->scale, &lo, &hi);
  animate_to(v, v->zoom, v->x, clampf(y, lo, hi));
}

void txp_viewer_zoom_at(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                        float zoom, float x, float y)
{
  zoom = clampf(zoom, MIN_ZOOM, MAX_ZOOM);
  txp_viewer_stop(v);

  // Point under (x, y): page and position in the page
  float ly = v->y + y / v->scale;
  int page = layout_find(v, pc, ly);
  float dy = page < 0 ? ly : ly - layout_top(v, pc, page, v->gap);
  float u = (x - v->win_w / 2.0f) / v->scale + v->x;

  v->zoom = v->target_zoom = zoom;
  relayout(ctx, v, pc);

  float top = page < 0 ? 0 : layout_top(v, pc, page, v->gap);
  v->y = v->target_y = top + dy - y / v->scale;
  v->x = v->target_x = u - (x - v->win_w / 2.0f) / v->scale;
}

txp_doc_coord txp_viewer_get_anchor(fz_context *ctx, txp_viewer *v,
                                    txp_page_collection *pc)
{
  txp_doc_coord c = {-1, 0, 0};
  float ly = v->y + v->win_h / v->scale / 2;
  int page = layout_find(v, pc, ly);
  if (page < 0)
  {
    if (!continuous(v))
      c.page = v->page;
    return c;
  }
  fz_rect b = txp_viewer_page_bounds(ctx, v, pc, page);
  c.page = page;
  c.x = (b.x0 + b.x1) / 2;
  c.y = b.y0 + ly - layout_top(v, pc, page, v->gap);
  return c;
}

void txp_viewer_set_anchor(fz_context *ctx, txp_viewer *v,
                           txp_page_collection *pc, txp_doc_coord c)
{
  if (c.page < 0)
    return;
  if (!continuous(v))
    v->page = c.page;
  relayout(ctx, v, pc);
  if (!in_layout(v, pc, c.page))
    return;
  fz_rect b = txp_viewer_page_bounds(ctx, v, pc, c.page);
  float ly = layout_top(v, pc, c.page, v->gap) + c.y - b.y0;
  txp_viewer_stop(v);
  v->y = v->target_y = ly - v->win_h / v->scale / 2;
}

void txp_viewer_set_mode(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                         txp_view_mode mode)
{
  if (v->mode == mode)
    return;
  txp_doc_coord c = txp_viewer_get_anchor(ctx, v, pc);
  v->mode = mode;
  v->pending_align = -1;
  txp_viewer_stop(v);
  txp_viewer_set_anchor(ctx, v, pc, c);
}

void txp_viewer_set_fit(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                        txp_fit_mode fit)
{
  txp_doc_coord c = txp_viewer_get_anchor(ctx, v, pc);
  v->fit = fit;
  v->zoom = v->target_zoom = 1;
  txp_viewer_set_anchor(ctx, v, pc, c);
}

void txp_viewer_fit_width_at(fz_context *ctx, txp_viewer *v,
                             txp_page_collection *pc, txp_doc_coord c)
{
  if (!in_layout(v, pc, c.page))
    return;
  fz_rect b = txp_viewer_page_bounds(ctx, v, pc, c.page);
  float aw = fz_max(v->win_w - 2 * v->margin, 1);
  float scale = aw / (b.x1 - b.x0);
  float zoom = clampf(scale / compute_scale(ctx, v, pc, 1), MIN_ZOOM, MAX_ZOOM);
  scale = compute_scale(ctx, v, pc, zoom);
  float ly = layout_top(v, pc, c.page, compute_gap(v, scale)) + c.y - b.y0;
  animate_to(v, zoom, 0, ly - v->win_h / scale / 2);
}

void txp_viewer_reveal(fz_context *ctx, txp_viewer *v, txp_page_collection *pc,
                       txp_doc_coord c)
{
  if (c.page < 0)
    return;

  if (!continuous(v))
  {
    if (c.page != v->page)
    {
      v->page = c.page;
      v->pending_align = -1;
    }
    relayout(ctx, v, pc);
    if (!in_layout(v, pc, c.page))
      return;
    // Keep the point away from the top and bottom quarters of the screen
    fz_point p = txp_viewer_doc_to_screen(ctx, v, pc, c);
    float h = v->win_h, lo = h / 4, hi = h / 3;
    if (p.y < lo)
      v->y -= (hi - p.y) / v->scale;
    else if (p.y >= h - lo)
      v->y += (p.y - (h - hi)) / v->scale;
    v->target_y = v->y;
    return;
  }

  if (!in_layout(v, pc, c.page))
    return;

  // Scroll when the point is out of the middle of the screen, to show it
  // in the upper part where the following lines are visible too
  fz_point p = txp_viewer_doc_to_screen(ctx, v, pc, c);
  float zoom = v->animating ? v->target_zoom : v->zoom;
  float x = v->animating ? v->target_x : v->x;
  float y = v->animating ? v->target_y : v->y;
  bool move = false;
  if (fabsf(p.y / v->win_h - 0.5f) > 0.3f)
  {
    y = v->y + (p.y - 0.25f * v->win_h) / v->scale;
    move = true;
  }
  if (p.x < v->margin || p.x > v->win_w - v->margin)
  {
    x = v->x + (p.x - v->win_w / 2.0f) / v->scale;
    move = true;
  }
  if (move)
    animate_to(v, zoom, x, y);
}

/* Scrollbar */

static void hover_scrollbar(txp_viewer_scrollbar *sb, int x, int y)
{
  if (sb->thumb_h <= 0)
    return;
  if (sb->dragging || (x >= sb->x - 10 && y >= 0 && y < sb->h))
    sb->target_alpha = 255;
  else if (x >= sb->x - 100)
    sb->target_alpha = 128;
  else
    sb->target_alpha = 0;
}

static void drag_scrollbar(fz_context *ctx, txp_viewer *v,
                           txp_page_collection *pc, int y)
{
  txp_viewer_scrollbar *sb = &v->scrollbar;
  float lo, hi;
  vertical_limits(ctx, v, pc, v->scale, &lo, &hi);
  int track = sb->h - sb->thumb_h;
  if (track <= 0 || hi <= lo)
    return;
  float norm = clampf((float)(y - sb->drag_offset) / track, 0, 1);
  txp_viewer_stop(v);
  v->y = v->target_y = lo + norm * (hi - lo);
}

bool txp_viewer_scrollbar_press(fz_context *ctx, txp_viewer *v,
                                txp_page_collection *pc, int x, int y)
{
  txp_viewer_scrollbar *sb = &v->scrollbar;
  if (!continuous(v) || sb->thumb_h <= 0 || x < sb->x - 10)
    return false;
  sb->dragging = true;
  sb->target_alpha = 255;
  if (y >= sb->thumb_y && y < sb->thumb_y + sb->thumb_h)
    sb->drag_offset = y - sb->thumb_y;
  else
  {
    // Jump so that the thumb is centered on the pointer
    sb->drag_offset = sb->thumb_h / 2;
    drag_scrollbar(ctx, v, pc, y);
  }
  return true;
}

bool txp_viewer_scrollbar_motion(fz_context *ctx, txp_viewer *v,
                                 txp_page_collection *pc, int x, int y)
{
  txp_viewer_scrollbar *sb = &v->scrollbar;
  if (continuous(v))
    hover_scrollbar(sb, x, y);
  if (!sb->dragging)
    return false;
  drag_scrollbar(ctx, v, pc, y);
  return true;
}

bool txp_viewer_scrollbar_release(txp_viewer *v)
{
  if (!v->scrollbar.dragging)
    return false;
  v->scrollbar.dragging = false;
  return true;
}

void txp_viewer_draw_scrollbar(txp_viewer *v, SDL_Renderer *renderer)
{
  txp_viewer_scrollbar *sb = &v->scrollbar;
  int alpha = (int)sb->alpha;
  if (!continuous(v) || sb->thumb_h <= 0 || alpha <= 0)
    return;

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 50, 50, 50, alpha);
  SDL_Rect track = {sb->x, 0, sb->w, sb->h};
  SDL_RenderFillRect(renderer, &track);
  int color = sb->dragging ? 200 : 180;
  SDL_SetRenderDrawColor(renderer, color, color, color, alpha);
  SDL_Rect thumb = {sb->x, sb->thumb_y, sb->w, sb->thumb_h};
  SDL_RenderFillRect(renderer, &thumb);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}
