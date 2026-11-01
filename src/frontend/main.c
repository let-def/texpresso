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

#include <SDL2/SDL.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include "mydvi.h"
#include "providers.h"
#include <limits.h>
#include <math.h>
#include "page_buffer.h"
#include "page_collection.h"
#include "viewer.h"
#include "engine.h"
#include "driver.h"
#include "synctex.h"
#include "vstack.h"
#include "prot_parser.h"
#include "editor.h"
#include "base64.h"

struct persistent_state *pstate;

static void schedule_event(enum custom_events ev)
{
  pstate->schedule_event(ev);
}

static bool should_reload_binary(void)
{
  return pstate->should_reload_binary();
}

#ifdef __APPLE__
# define st_time(a) st_##a##timespec
#else
# define st_time(a) st_##a##tim
#endif

static bool is_more_recent(uint64_t *time, char *candidate)
{
  struct stat st;
  if (stat(candidate, &st) == 0 && st.st_time(c).tv_sec > *time)
  {
    *time = st.st_time(c).tv_sec;
    return 1;
  }
  return 0;
}

static void set_more_recent(uint64_t *time, char **result, char *candidate)
{
  if (is_more_recent(time, candidate))
    *result = candidate;
}

static void find_engine(char engine_path[4096], const char *exec_path)
{
  strcpy(engine_path, exec_path);
  char *basename = NULL;
  for (int i = 0; i < 4096 && engine_path[i]; ++i)
    if (engine_path[i] == '/')
      basename = engine_path + i + 1;
  uint64_t time = 0;
  if (basename)
  {
    strcpy(basename, "texpresso-xetex");
    if (!is_more_recent(&time, engine_path))
      strcpy(engine_path, "texpresso-xetex");
  }
}

/* UI state */

enum ui_mouse_status {
  UI_MOUSE_NONE,
  UI_MOUSE_SELECT,
  UI_MOUSE_MOVE,
  UI_MOUSE_DRAG,
  UI_MOUSE_SCROLLBAR,
};

#define SELECTION_RECT_COUNT 400

struct fullscreen_state {
  bool crop;
  txp_fit_mode fit;
  float zoom;
  bool has_backup;
  bool prev_fs;
};

typedef struct {
  txp_engine *eng;
  SDL_Renderer *sdl_renderer;
  SDL_Window *window;

  // Persisted across reloads, in pstate->initial
  txp_page_collection *pages;
  txp_viewer *viewer;
  struct txp_theme *theme;

  txp_page_buffer *buffer;
  bool dirty;
  Uint64 last_update;
  bool was_idle;

  // Text selection, in the coordinates of a page
  struct {
    int page;
    fz_point start;
    fz_rect rects[SELECTION_RECT_COUNT];
    int count;
  } sel;

  // Mouse input state
  int last_mouse_x, last_mouse_y;
  uint32_t last_click_ticks;
  enum ui_mouse_status mouse_status;
  bool click_pending;
  bool advancing;

  // The engine reloads the whole document when it changes (PDF), rather
  // than backtracking (TeX)
  bool reload_all;
} ui_state;

static bool is_continuous(ui_state *ui)
{
  return ui->viewer->mode == TXP_VIEW_CONTINUOUS;
}

// Page the user is looking at: the engine has to produce it
static int target_page(fz_context *ctx, ui_state *ui)
{
  if (!is_continuous(ui))
    return ui->viewer->page;
  txp_visible_range vr = txp_viewer_visible_range(ctx, ui->viewer, ui->pages);
  return vr.first <= vr.last ? vr.last : 0;
}

static fz_point get_scale_factor(SDL_Window *window)
{
  int ww, wh, pw, ph;
  SDL_GetWindowSize(window, &ww, &wh);

#if SDL_VERSION_ATLEAST(2, 0, 26)
  SDL_GetWindowSizeInPixels(window, &pw, &ph);
#else
  SDL_GetRendererOutputSize(SDL_GetRenderer(window), &pw, &ph);
#endif

  return fz_make_point(ww != 0 ? (float)pw / ww : 1,
                       wh != 0 ? (float)ph / wh : 1);
}

/* Colors */

static void theme_colors(struct txp_theme *theme, uint32_t *bg, uint32_t *fg)
{
  uint32_t cbg = 0xFFFFFF, cfg = 0x000000;

  if (theme->themed)
  {
    cbg = theme->background & 0xFFFFFF;
    cfg = theme->foreground & 0xFFFFFF;
  }

  if (theme->inverted)
  {
    uint32_t tmp = cfg;
    cfg = cbg;
    cbg = tmp;
  }

  *fg = cfg;
  *bg = cbg;
}

#define remap(v, bp, wp) (bp) + ((v) * (wp - bp)) / 255

// Map black to the foreground color and white to the background color
static void remap_colors(void *data, uint8_t *pixels, int width)
{
  const uint8_t *c = data;
  for (int x = 0; x < width; ++x, pixels += 3)
  {
    pixels[0] = remap(pixels[0], c[0], c[3]);
    pixels[1] = remap(pixels[1], c[1], c[4]);
    pixels[2] = remap(pixels[2], c[2], c[5]);
  }
}

/* UI rendering */

static void render_caret(SDL_Renderer *sdl, int scale_factor, int x, int y, int h)
{
  if (scale_factor == 0)
    scale_factor = 1;

  SDL_Rect r;
  r.x = x - scale_factor / 2;
  r.y = y + scale_factor;
  r.w = scale_factor;
  r.h = h - scale_factor * 2;
  SDL_RenderFillRect(sdl, &r);

  r.x = x - scale_factor * 3;
  r.y = y;
  r.w = scale_factor * 6;
  r.h = scale_factor;
  SDL_RenderFillRect(sdl, &r);

  r.y = y + h - scale_factor;
  SDL_RenderFillRect(sdl, &r);
}

static void render_selection(fz_context *ctx, ui_state *ui)
{
  if (ui->sel.count == 0)
    return;

  txp_visible_range vr = txp_viewer_visible_range(ctx, ui->viewer, ui->pages);
  if (ui->sel.page < vr.first || ui->sel.page > vr.last)
    return;

  int scale_factor = get_scale_factor(ui->window).x;
  SDL_SetRenderDrawBlendMode(ui->sdl_renderer, SDL_BLENDMODE_BLEND);
  for (int i = 0; i < ui->sel.count; ++i)
  {
    fz_rect fzr = ui->sel.rects[i];
    txp_doc_coord c0 = {ui->sel.page, fzr.x0, fzr.y0};
    txp_doc_coord c1 = {ui->sel.page, fzr.x1, fzr.y1};
    fz_point p0 = txp_viewer_doc_to_screen(ctx, ui->viewer, ui->pages, c0);
    fz_point p1 = txp_viewer_doc_to_screen(ctx, ui->viewer, ui->pages, c1);
    SDL_Rect r = {.x = p0.x, .y = p0.y, .w = p1.x - p0.x, .h = p1.y - p0.y};
    if (r.w == 0)
    {
      SDL_SetRenderDrawColor(ui->sdl_renderer, 96, 96, 255, 128);
      render_caret(ui->sdl_renderer, scale_factor, r.x, r.y, r.h);
    }
    else
    {
      SDL_SetRenderDrawColor(ui->sdl_renderer, 96, 96, 255, 64);
      SDL_RenderFillRect(ui->sdl_renderer, &r);
    }
  }
  SDL_SetRenderDrawBlendMode(ui->sdl_renderer, SDL_BLENDMODE_NONE);
}

// Draw the screen, for the layout computed by the last txp_viewer_update
static void draw(fz_context *ctx, ui_state *ui)
{
  txp_viewer *v = ui->viewer;
  txp_page_collection *pc = ui->pages;
  fz_irect screen = fz_make_irect(0, 0, v->win_w, v->win_h);
  txp_visible_range vr = txp_viewer_visible_range(ctx, v, pc);

  // Visible part of the pages, in buffer space
  fz_irect *visible = NULL;
  int count = vr.last - vr.first + 1;
  if (count > 0)
    visible = fz_malloc_struct_array(ctx, count, fz_irect);

  int y0 = INT_MAX, y1 = INT_MIN;
  for (int i = 0; i < count; i++)
  {
    int page = vr.first + i;
    fz_irect s = txp_viewer_page_screen_rect(ctx, v, pc, page);
    fz_irect b = txp_viewer_page_buffer_rect(ctx, v, pc, page);
    fz_irect r = fz_intersect_irect(s, screen);
    if (fz_is_empty_irect(r))
    {
      visible[i] = fz_empty_irect;
      continue;
    }
    visible[i] = fz_make_irect(r.x0 - s.x0 + b.x0, r.y0 - s.y0 + b.y0,
                               r.x1 - s.x0 + b.x0, r.y1 - s.y0 + b.y0);
    y0 = fz_mini(y0, visible[i].y0);
    y1 = fz_maxi(y1, visible[i].y1);
  }

  if (is_continuous(ui))
    SDL_SetRenderDrawColor(ui->sdl_renderer, 96, 96, 96, 255);
  else
    SDL_SetRenderDrawColor(ui->sdl_renderer, 0, 0, 0, 255);
  SDL_RenderClear(ui->sdl_renderer);

  if (y0 < y1 && txp_page_buffer_begin(ctx, ui->buffer, v->win_w, v->win_h, y0, y1))
  {
    uint32_t bg, fg;
    theme_colors(ui->theme, &bg, &fg);
    uint8_t colors[6] = {
      (fg >> 16) & 0xFF, (fg >> 8) & 0xFF, fg & 0xFF,
      (bg >> 16) & 0xFF, (bg >> 8) & 0xFF, bg & 0xFF,
    };
    txp_page_buffer_filter filter =
        (bg == 0xFFFFFF && fg == 0x000000) ? NULL : remap_colors;

    for (int i = 0; i < count; i++)
    {
      if (fz_is_empty_irect(visible[i]))
        continue;
      int page = vr.first + i;
      fz_irect s = txp_viewer_page_screen_rect(ctx, v, pc, page);
      fz_irect b = txp_viewer_page_buffer_rect(ctx, v, pc, page);
      txp_page_buffer_update(ctx, ui->buffer, page, b, visible[i],
                             txp_page_collection_get(pc, page),
                             txp_viewer_page_bounds(ctx, v, pc, page),
                             filter, colors);
      txp_page_buffer_blit(ui->buffer, visible[i],
                           visible[i].x0 - b.x0 + s.x0,
                           visible[i].y0 - b.y0 + s.y0);
    }
  }
  fz_free(ctx, visible);

  render_selection(ctx, ui);
  txp_viewer_draw_scrollbar(v, ui->sdl_renderer);
  SDL_RenderPresent(ui->sdl_renderer);
  ui->dirty = false;
}

// Advance animations and redraw if needed
static void frame(fz_context *ctx, ui_state *ui)
{
  Uint64 now = SDL_GetPerformanceCounter();
  float dt = (float)(now - ui->last_update) / SDL_GetPerformanceFrequency();
  // Nothing moved since the last update: start animations from a frame
  if (ui->was_idle || dt > 0.1f)
    dt = 1.0f / 60;
  ui->last_update = now;

  int w, h;
  SDL_GetRendererOutputSize(ui->sdl_renderer, &w, &h);
  if (txp_viewer_update(ctx, ui->viewer, ui->pages, dt, w, h))
    ui->dirty = true;
  ui->was_idle = txp_viewer_is_idle(ui->viewer);
  if (ui->dirty)
    draw(ctx, ui);
}

static void render(fz_context *ctx, ui_state *ui)
{
  ui->dirty = true;
  frame(ctx, ui);
}

struct repaint_on_resize_env
{
  fz_context *ctx;
  ui_state *ui;
};

static int repaint_on_resize(void *data, SDL_Event *event)
{
  struct repaint_on_resize_env *env = data;
  if (event->type == SDL_WINDOWEVENT &&
      event->window.event == SDL_WINDOWEVENT_RESIZED &&
      SDL_GetWindowFromID(event->window.windowID) == env->ui->window)
  {
    render(env->ctx, env->ui);
  }
  return 0;
}

/* Document processing */

// The engine might have backtracked: forget the pages it no longer has
static void check_pages(ui_state *ui)
{
  txp_page_collection_invalidate_after(ui->pages, send(page_count, ui->eng));
}

static void clear_selection(ui_state *ui)
{
  if (ui->sel.count != 0)
    ui->dirty = true;
  ui->sel.count = 0;
}

// Fetch the pages produced by the engine, for at most 20ms.
// Returns true if some pages remain to be fetched.
static bool sync_pages(struct persistent_state *ps, ui_state *ui)
{
  fz_context *ctx = ps->ctx;
  txp_page_collection *pc = ui->pages;
  check_pages(ui);

  uint32_t start = SDL_GetTicks();
  int count = send(page_count, ui->eng);
  for (int i = txp_page_collection_valid_count(pc); i < count; i++)
  {
    if (SDL_GetTicks() - start > 20)
      return true;

    fz_display_list *dl = NULL;
    fz_var(dl);
    fz_try(ctx)
      dl = send(render_page, ui->eng, ctx, i);
    fz_catch(ctx)
    {
      // Keep the page blank
      fprintf(stderr, "[render] cannot render page %d: %s\n", i,
              fz_caught_message(ctx));
      dl = NULL;
    }
    txp_page_collection_set(ctx, pc, i, dl);
    fz_drop_display_list(ctx, dl);
    txp_page_buffer_invalidate_page(ui->buffer, i);
    if (ui->sel.page == i)
      clear_selection(ui);
    ui->dirty = true;
  }

  if (send(get_status, ui->eng) == DOC_TERMINATED)
  {
    if (txp_page_collection_count(pc) > count)
    {
      txp_page_collection_truncate(ctx, pc);
      ui->dirty = true;
    }
    if (!is_continuous(ui) && ui->viewer->page >= count && count > 0)
      ui->viewer->page = count - 1;
  }
  return false;
}

static bool need_advance(fz_context *ctx, ui_state *ui)
{
  if (send(get_status, ui->eng) != DOC_RUNNING)
    return false;

  if (send(is_finishing, ui->eng))
    return true;

  int target = target_page(ctx, ui);
  int count = send(page_count, ui->eng);
  if (count <= target)
    return true;

  // Look one page ahead, to know whether the document continues
  if (is_continuous(ui) && count == target + 1)
    return true;

  fz_buffer *buf;
  synctex_t *stx = send(synctex, ui->eng, &buf);
  return synctex_page_count(stx) <= target || synctex_has_target(stx);
}

static bool advance_engine(fz_context *ctx, ui_state *ui)
{
  bool need = need_advance(ctx, ui);
  if (!need && ui->advancing)
    editor_flush();
  ui->advancing = need;
  if (!need)
    return false;

  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);

  int steps = 10;
  while (need)
  {
    bool stepped = send(step, ui->eng, ctx, false);
    check_pages(ui);
    if (!stepped)
      break;

    steps -= 1;
    need = need_advance(ctx, ui);

    if (steps == 0)
    {
      steps = 10;

      struct timespec curr;
      clock_gettime(CLOCK_MONOTONIC, &curr);

      int delta =
        (curr.tv_sec - start.tv_sec) * 1000 * 1000 * 1000 +
        (curr.tv_nsec - start.tv_nsec);

      if (delta > 5000000)
        break;
    }
  }
  return need;
}

/* Selection */

static float point_to_rect_dist(fz_point p, fz_rect r)
{
  float dx = fz_max(0, fz_max(r.x0 - p.x, p.x - r.x1));
  float dy = fz_max(0, fz_max(r.y0 - p.y, p.y - r.y1));
  return sqrtf(dx * dx + dy * dy);
}

static void set_quads(ui_state *ui, int page, fz_quad *quads, int count)
{
  bool diff = count != ui->sel.count || page != ui->sel.page;
  ui->sel.page = page;
  ui->sel.count = count;

  for (int i = 0; i < count; ++i)
  {
    fz_rect r = fz_rect_from_quad(quads[i]);
    if (!diff)
      diff =
          (r.x0 != ui->sel.rects[i].x0 || r.y0 != ui->sel.rects[i].y0 ||
           r.x1 != ui->sel.rects[i].x1 || r.y1 != ui->sel.rects[i].y1);
    ui->sel.rects[i] = r;
  }

  if (diff)
    ui->dirty = true;
}

static void start_selection(fz_context *ctx, ui_state *ui, txp_doc_coord c)
{
  clear_selection(ui);
  ui->sel.page = c.page;
  ui->sel.start = fz_make_point(c.x, c.y);
}

static void drag_selection(fz_context *ctx, ui_state *ui, float x, float y)
{
  fz_stext_page *page = txp_page_collection_stext(ctx, ui->pages, ui->sel.page);
  if (!page)
    return;

  fz_point p = txp_viewer_screen_to_page(ctx, ui->viewer, ui->pages,
                                         ui->sel.page, x, y);
  fz_quad quads[SELECTION_RECT_COUNT];
  int count = fz_highlight_selection(ctx, page, ui->sel.start, p,
                                     quads, SELECTION_RECT_COUNT);
  set_quads(ui, ui->sel.page, quads, count);
}

// Place a caret at the character under the point
static void select_char(fz_context *ctx, ui_state *ui, txp_doc_coord c)
{
  fz_stext_page *page = txp_page_collection_stext(ctx, ui->pages, c.page);
  if (!page)
    return;

  float scale = ui->viewer->scale;
  fz_point p, p0, p1;
  p0 = p1 = p = fz_make_point(c.x, c.y);
  fz_quad q = fz_snap_selection(ctx, page, &p0, &p1, FZ_SELECT_WORDS);

  int count = 0;

  if (point_to_rect_dist(p, fz_rect_from_quad(q)) * scale > 20)
  {
    p0 = p;
    p0.x -= 8;
    p1 = p0;
    q = fz_snap_selection(ctx, page, &p0, &p1, FZ_SELECT_WORDS);

    if (point_to_rect_dist(p, fz_rect_from_quad(q)) * scale < 20)
    {
      q.ul = q.ur;
      q.ll = q.lr;
      count = 1;
    }
  }
  else
  {
    p0 = p1 = p;
    count = 1;
    q = fz_snap_selection(ctx, page, &p0, &p1, FZ_SELECT_CHARS);
  }

  set_quads(ui, c.page, &q, count);
}

static void select_word(fz_context *ctx, ui_state *ui, txp_doc_coord c)
{
  fz_stext_page *page = txp_page_collection_stext(ctx, ui->pages, c.page);
  if (!page)
    return;

  float scale = ui->viewer->scale;
  fz_point p, p0, p1;
  p0 = p1 = p = fz_make_point(c.x, c.y);
  fz_quad q = fz_snap_selection(ctx, page, &p0, &p1, FZ_SELECT_WORDS);

  if (point_to_rect_dist(p, fz_rect_from_quad(q)) * scale > 20)
  {
    p0 = p;
    p0.x -= 16;
    p1 = p0;
    q = fz_snap_selection(ctx, page, &p0, &p1, FZ_SELECT_WORDS);
  }

  set_quads(ui, c.page, &q, 1);
}

/* UI events */

static void synctex_click(struct persistent_state *ps, ui_state *ui, txp_doc_coord c)
{
  if (c.page < 0)
    return;
  fz_buffer *buf;
  synctex_t *stx = send(synctex, ui->eng, &buf);
  if (!stx || !buf)
    return;
  float f = 1 / send(scale_factor, ui->eng);
  fprintf(stderr, "click: page %d (%f,%f) mapped:(%f,%f)\n",
          c.page, c.x, c.y, f * c.x, f * c.y);
  synctex_hit_t hit;
  if (synctex_scan(ps->ctx, stx, buf, c.page, f * c.x, f * c.y, &hit))
    editor_synctex(ps->doc_path, hit.fname, hit.len, hit.line, hit.column);
}

static void ui_mouse_down(struct persistent_state *ps, ui_state *ui,
                          int x, int y, int clicks, bool ctrl)
{
  fz_context *ctx = ps->ctx;
  fz_point scale = get_scale_factor(ui->window);
  float px = scale.x * x, py = scale.y * y;
  txp_doc_coord c = txp_viewer_screen_to_doc(ctx, ui->viewer, ui->pages, px, py);

  bool near = abs(ui->last_mouse_x - x) < 30 && abs(ui->last_mouse_y - y) < 30;
  ui->last_mouse_x = x;
  ui->last_mouse_y = y;

  if (is_continuous(ui))
  {
    if (txp_viewer_scrollbar_press(ctx, ui->viewer, ui->pages, px, py))
    {
      ui->mouse_status = UI_MOUSE_SCROLLBAR;
      return;
    }

    if (!ctrl)
    {
      if (clicks >= 2 && c.page >= 0)
      {
        // Double click: fit the page width, triple click: toggle cropping
        if (clicks > 2)
        {
          bool crop = !txp_page_collection_get_crop(ui->pages);
          txp_page_collection_set_crop(ctx, ui->pages, crop);
          txp_page_buffer_flush(ui->buffer);
          ui->dirty = true;
        }
        txp_viewer_fit_width_at(ctx, ui->viewer, ui->pages, c);
        ui->mouse_status = UI_MOUSE_NONE;
        return;
      }
      // Drag the pages, or click if the mouse does not move
      txp_viewer_drag_begin(ui->viewer);
      ui->mouse_status = UI_MOUSE_DRAG;
      ui->click_pending = true;
      return;
    }
  }
  else if (ctrl)
  {
    ui->mouse_status = UI_MOUSE_MOVE;
    return;
  }

  // Select text
  ui->mouse_status = UI_MOUSE_SELECT;

  uint32_t ticks = SDL_GetTicks();
  bool double_click = ticks - ui->last_click_ticks < 500 && near;
  if (c.page < 0)
  {
    clear_selection(ui);
    ui->mouse_status = UI_MOUSE_NONE;
  }
  else if (double_click && c.page == ui->sel.page)
    select_word(ctx, ui, c);
  else
  {
    start_selection(ctx, ui, c);
    select_char(ctx, ui, c);
    ui->last_click_ticks = ticks;
    synctex_click(ps, ui, c);
  }
}

static void ui_mouse_up(struct persistent_state *ps, ui_state *ui, int x, int y)
{
  fz_context *ctx = ps->ctx;
  switch (ui->mouse_status)
  {
    case UI_MOUSE_SCROLLBAR:
      txp_viewer_scrollbar_release(ui->viewer);
      break;

    case UI_MOUSE_DRAG:
      SDL_SetRelativeMouseMode(SDL_FALSE);
      if (ui->click_pending)
      {
        // A click without moving: jump to the source
        txp_viewer_stop(ui->viewer);
        fz_point scale = get_scale_factor(ui->window);
        txp_doc_coord c = txp_viewer_screen_to_doc(ctx, ui->viewer, ui->pages,
                                                   scale.x * x, scale.y * y);
        if (c.page >= 0)
        {
          select_char(ctx, ui, c);
          synctex_click(ps, ui, c);
        }
        else
          clear_selection(ui);
      }
      else
        txp_viewer_drag_end(ui->viewer);
      break;

    default:
      break;
  }
  ui->click_pending = false;
  ui->mouse_status = UI_MOUSE_NONE;
}

static void ui_mouse_move(struct persistent_state *ps, ui_state *ui,
                          int x, int y, int xrel, int yrel)
{
  fz_context *ctx = ps->ctx;
  fz_point scale = get_scale_factor(ui->window);
  float px = scale.x * x, py = scale.y * y;

  if (txp_viewer_scrollbar_motion(ctx, ui->viewer, ui->pages, px, py))
    return;

  switch (ui->mouse_status)
  {
    case UI_MOUSE_NONE:
    case UI_MOUSE_SCROLLBAR:
      break;

    case UI_MOUSE_SELECT:
      drag_selection(ctx, ui, px, py);
      break;

    case UI_MOUSE_MOVE:
    {
      int dx = x - ui->last_mouse_x;
      int dy = y - ui->last_mouse_y;
      if (dx != 0 || dy != 0)
      {
        txp_viewer_scroll(ctx, ui->viewer, ui->pages, -scale.x * dx, -scale.y * dy);
        ui->last_mouse_x = x;
        ui->last_mouse_y = y;
      }
      break;
    }

    case UI_MOUSE_DRAG:
    {
      if (ui->click_pending &&
          (abs(x - ui->last_mouse_x) >= 10 || abs(y - ui->last_mouse_y) >= 10))
        ui->click_pending = false;
      if (!ui->click_pending)
      {
        txp_viewer_scroll(ctx, ui->viewer, ui->pages,
                          -scale.x * xrel, -scale.y * yrel);
        // Keep dragging when the pointer reaches the edge of the window
        int ww, wh;
        SDL_GetWindowSize(ui->window, &ww, &wh);
        if (x + xrel <= 0 || x + xrel >= ww - 1 ||
            y + yrel <= 0 || y + yrel >= wh - 1)
          SDL_SetRelativeMouseMode(SDL_TRUE);
      }
      break;
    }
  }
}

static void ui_mouse_wheel(fz_context *ctx, ui_state *ui, float dx, float dy,
                           int mousex, int mousey, bool ctrl, bool shift)
{
  if (ui->mouse_status != UI_MOUSE_NONE)
    return;

  fz_point scale = get_scale_factor(ui->window);
  txp_viewer *v = ui->viewer;

  if (ctrl || (shift && is_continuous(ui)))
  {
    if (dy != 0)
      txp_viewer_zoom_at(ctx, v, ui->pages,
                         v->zoom * expf(dy * (is_continuous(ui) ? 0.1f : 0.02f)),
                         scale.x * mousex, scale.y * mousey);
  }
  else if (is_continuous(ui))
  {
    if (dx != 0)
      txp_viewer_scroll(ctx, v, ui->pages, scale.x * dx * 5, 0);
    if (dy != 0)
      txp_viewer_fling(v, -scale.y * dy * 10);
  }
  else
    txp_viewer_scroll(ctx, v, ui->pages, scale.x * dx * 5, -scale.y * dy * 5);
}

/* Stdin polling */

static int SDLCALL poll_stdin_thread_main(void *data)
{
  int *pipes = data;
  char c;
  int n;

  while (1)
  {
    n = read(pipes[0], &c, 1);
    if (n == -1)
    {
      if (errno == EINTR)
        continue;
      return 1;
    }

    if (n == 0)
      return 1;

    if (c == 'q')
      return 0;

    if (c != 'c')
      abort();

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLRDNORM;
    fds[0].revents = 0;
    fds[1].fd = pipes[0];
    fds[1].events = POLLRDNORM;
    fds[1].revents = 0;

    while (1)
    {
      n = poll(fds, 2, -1);
      if (n == -1)
      {
        if (errno == EINTR)
          continue;
        return 1;
      }
      // A closed pipe reports POLLHUP without POLLRDNORM (Linux)
      if ((fds[0].revents & (POLLRDNORM | POLLHUP | POLLERR)) != 0)
        schedule_event(STDIN_EVENT);
      break;
    }
  }
}

// In initialize mode, stop once the first page is ready or the engine is done
static bool initialize_done(struct persistent_state *ps, ui_state *ui, bool stdin_eof)
{
  return ps->initialize_only &&
         (send(page_count, ui->eng) > 0 ||
          (send(get_status, ui->eng) == DOC_TERMINATED && stdin_eof));
}

static bool poll_stdin(void)
{
  struct pollfd fd;
  fd.fd = STDIN_FILENO;
  fd.events = POLLRDNORM;
  fd.revents = 0;
  // A closed pipe reports POLLHUP without POLLRDNORM (Linux)
  return (poll(&fd, 1, 0) == 1) &&
         ((fd.revents & (POLLRDNORM | POLLHUP | POLLERR)) != 0);
}

static void wakeup_poll_thread(int poll_stdin_pipe[2], char c)
{
  while (1)
  {
    int n = write(poll_stdin_pipe[1], &c, 1);
    if (n == 1)
      break;
    if (n == -1 && errno == EINTR)
      continue;
    perror("write(poll_stdin_pipe, _, _)");
    break;
  }
}


/* Navigation */

static void previous_page(fz_context *ctx, ui_state *ui, bool pan)
{
  synctex_set_target(send(synctex, ui->eng, NULL), 0, NULL, 0);
  txp_viewer *v = ui->viewer;

  if (is_continuous(ui))
  {
    txp_viewer_step_page(ctx, v, ui->pages, -1);
    return;
  }

  if (v->page > 0)
  {
    int page = v->page - 1;

    int page_count = send(page_count, ui->eng);
    if (page_count > 0 && page >= page_count &&
        send(get_status, ui->eng) == DOC_TERMINATED)
      page = page_count - 1;

    if (pan)
      txp_viewer_goto_page(ctx, v, ui->pages, page, TXP_ALIGN_BOTTOM);
    else
      v->page = page;
    schedule_event(RELOAD_EVENT);
  }
}

static void next_page(fz_context *ctx, ui_state *ui, bool pan)
{
  synctex_set_target(send(synctex, ui->eng, NULL), 0, NULL, 0);
  txp_viewer *v = ui->viewer;

  if (is_continuous(ui))
  {
    txp_viewer_step_page(ctx, v, ui->pages, 1);
    return;
  }

  if (pan)
    txp_viewer_goto_page(ctx, v, ui->pages, v->page + 1, TXP_ALIGN_TOP);
  else
    v->page += 1;
  schedule_event(RELOAD_EVENT);
}

static void ui_pan_x(fz_context *ctx, ui_state *ui, float factor)
{
  txp_viewer *v = ui->viewer;
  txp_viewer_scroll(ctx, v, ui->pages, -v->win_w * factor, 0);
}

// Move up (factor > 0) or down (factor < 0) by a fraction of the screen.
// In single page mode, continue on the next page at the bottom of a page.
static void ui_pan(fz_context *ctx, ui_state *ui, float factor)
{
  if (txp_viewer_scroll_screen(ctx, ui->viewer, ui->pages, -factor))
    return;
  if (factor < 0)
    next_page(ctx, ui, 1);
  else
    previous_page(ctx, ui, 1);
}

// Move by a screen, page by page in continuous mode
static void ui_screen(fz_context *ctx, ui_state *ui, int dir)
{
  if (is_continuous(ui))
    txp_viewer_step(ctx, ui->viewer, ui->pages, dir);
  else
    ui_pan(ctx, ui, -dir * 2.0 / 3.0);
}

static void ui_zoom(fz_context *ctx, ui_state *ui, float factor)
{
  txp_viewer *v = ui->viewer;
  txp_viewer_zoom_at(ctx, v, ui->pages, v->zoom * factor,
                     v->win_w / 2.0f, v->win_h / 2.0f);
}

static void ui_set_crop(fz_context *ctx, ui_state *ui, bool crop)
{
  if (crop == txp_page_collection_get_crop(ui->pages))
    return;
  txp_doc_coord c = txp_viewer_get_anchor(ctx, ui->viewer, ui->pages);
  txp_page_collection_set_crop(ctx, ui->pages, crop);
  txp_viewer_set_anchor(ctx, ui->viewer, ui->pages, c);
  ui->dirty = true;
}

static void ui_set_mode(fz_context *ctx, ui_state *ui, txp_view_mode mode)
{
  clear_selection(ui);
  txp_viewer_set_mode(ctx, ui->viewer, ui->pages, mode);
  ui->dirty = true;
}

static const char *relative_path(const char *path, const char *dir, int *go_up)
{
  const char *rel_path = path, *dir_path = dir;

  // Skip common parts
  while (*rel_path && *rel_path == *dir_path)
  {
    if (*rel_path == '/')
    {
      while (*rel_path == '/') rel_path += 1;
      while (*dir_path == '/') dir_path += 1;
    }
    else
    {
      rel_path += 1;
      dir_path += 1;
    }
  }

  // Go back to last directory separator
  if (*rel_path && *dir_path)
  {
    rel_path -= 1;
    dir_path -= 1;
    while (path < rel_path && *rel_path != '/')
    {
      rel_path -= 1;
      dir_path -= 1;
    }
    if (*rel_path == '/')
    {
      if (*dir_path != '/') abort();
      rel_path += 1;
      dir_path += 1;
    }
  }

  // Count number of '../'
  *go_up = 0;
  if (*dir_path)
  {
    *go_up = 1;
    while (*dir_path)
    {
      if (*dir_path == '/')
      {
        *go_up += 1;
        while (*dir_path == '/')
          dir_path += 1;
      }
      else
        dir_path += 1;
    }
  }

  while (*rel_path == '/')
    rel_path += 1;

  return rel_path;
}

static int find_diff(const fz_buffer *buf, const void *data, int size)
{
  const unsigned char *ptr = data;
  int i, len = fz_mini(buf->len, size);
  for (i = 0; i < len && buf->data[i] == ptr[i]; ++i);
  fprintf(stderr, "i:%d len:%d size:%d\n", i, (int)buf->len, size);
  return i;
}

#include "utf_mapping.h"

static void realize_change(struct persistent_state *ps,
                           ui_state *ui,
                           struct editor_change *op)
{
  int go_up = 0;
  const char *path = relative_path(op->path, ps->doc_path, &go_up);
  if (go_up > 0)
  {
    fprintf(stderr, "[command] change %s: file has a different root, skipping\n", path);
    return;
  }

  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] change %s: file not found, skipping\n", path);
    return;
  }

  fz_buffer *b = e->edit_data;
  if (!b)
  {
    fprintf(stderr, "[command] change %s: file not opened, skipping\n", path);
    return;
  }

  int offset = op->span.offset, remove = op->span.remove, length = op->length;

  if (op->base == BASE_LINE)
  {
    // Compute byte offsets from line offsets
    int line = offset, count = remove;

    offset = remove = 0;

    uint8_t *p = b->data;
    size_t len = b->len;

    while (line > 0 && offset < len)
    {
      if (p[offset] == '\n')
        line -= 1;
      offset++;
    }

    if (line > 0)
    {
      fprintf(stderr, "[command] change line %s: invalid line number, skipping\n", path);
      return;
    }

    remove = offset;
    while (count > 0 && remove < len)
    {
      if (p[remove] == '\n')
        count -= 1;
      remove++;
    }

    if (count > 1)
    {
      fprintf(stderr, "[command] change line %s: invalid line count, skipping\n", path);
      return;
    }

    remove -= offset;
  }
  else if (op->base == BASE_RANGE)
  {
    // Compute byte offsets from line offsets
    int line = op->range.start_line;
    offset = remove = 0;

    uint8_t *p = b->data;
    size_t len = b->len;

    while (line > 0 && offset < len)
    {
      if (p[offset] == '\n')
        line -= 1;
      offset++;
    }

    if (line > 0)
    {
      fprintf(stderr, "[command] change range %s: invalid start line, skipping\n", path);
      return;
    }

    int start_char_offset = utf16_to_utf8_offset(p + offset, p + len, op->range.start_char);
    if (start_char_offset == -1)
    {
      fprintf(stderr, "[command] change range %s: invalid start char, skipping\n", path);
      return;
    }

    remove = offset;
    offset += start_char_offset;

    line = op->range.end_line - op->range.start_line;
    if (line < 0)
    {
      fprintf(stderr, "[command] change range %s: invalid end line, skipping\n", path);
      return;
    }

    while (line > 0 && remove < len)
    {
      if (p[remove] == '\n')
        line -= 1;
      remove++;
    }

    if (line > 0)
    {
      fprintf(stderr, "[command] change range %s: invalid end line, skipping\n", path);
      return;
    }

    int end_char_offset = utf16_to_utf8_offset(p + remove, p + len, op->range.end_char);
    if (end_char_offset == -1)
    {
      fprintf(stderr, "[command] change range %s: invalid end char, skipping\n", path);
      return;
    }

    remove += end_char_offset;
    remove -= offset;
  }

  if (remove < 0 || offset < 0 || offset + remove > b->len)
  {
    fprintf(stderr, "[command] change %s: invalid range, skipping\n", path);
    return;
  }

  if (b->len - remove + length > b->cap)
    fz_resize_buffer(ps->ctx, b, b->len - remove + length + 128);

  memmove(b->data + offset + length, b->data + offset + remove,
          b->len - offset - remove);

  b->len = b->len - remove + length;

  memmove(b->data + offset, op->data, length);

  fprintf(stderr, "[command] change %s: changed offset %d\n", path, offset);
  send(notify_file_changes, ui->eng, ps->ctx, e, offset);
}

#define BUFFERED_OPS 64
#define BUFFERED_CHARS 4096
#define T_IDLE_MS 500
#define MAX_RERUNS 5

struct {
  char buffer[BUFFERED_CHARS];
  int cursor;
  struct editor_change op[BUFFERED_OPS];
  int count;
} delayed_changes = {0,};

static void flush_changes(struct persistent_state *ps,
                          ui_state *ui)
{
  int count = delayed_changes.count;
  if (count)
  {
    delayed_changes.count = 0;
    delayed_changes.cursor = 0;
    for (int i = 0; i < count; ++i)
    {
      struct editor_change *op = &delayed_changes.op[i];
      realize_change(ps, ui, op);
    }
  }
}

static void interpret_change(struct persistent_state *ps,
                             ui_state *ui,
                             struct editor_change *op)
{
  int plen = strlen(op->path);
  int page_count = send(page_count, ui->eng);
  int cursor = delayed_changes.cursor;

  int page = target_page(ps->ctx, ui);
  if ((page_count == page - 2 || page_count == page - 1) &&
      send(get_status, ui->eng) == DOC_RUNNING &&
      delayed_changes.count < BUFFERED_OPS &&
      cursor + plen + 1 + op->length <= BUFFERED_CHARS)
  {
    char *op_path = delayed_changes.buffer + cursor;
    memcpy(op_path, op->path, plen + 1);
    cursor += plen + 1;
    char *op_data = delayed_changes.buffer + cursor;
    memcpy(op_data, op->data, op->length);
    cursor += op->length;
    delayed_changes.cursor = cursor;

    delayed_changes.op[delayed_changes.count] = *op;
    delayed_changes.op[delayed_changes.count].path = op_path;
    delayed_changes.op[delayed_changes.count].data = op_data;
    delayed_changes.count += 1;
  }
  else
  {
    flush_changes(ps, ui);
    realize_change(ps, ui, op);
  }
}

static void interpret_open(struct persistent_state *ps,
                           ui_state *ui,
                           const char *path,
                           const void *data,
                           int size)
{
  if (path[0] == '/')
  {
    int go_up = 0;
    path = relative_path(path, ps->doc_path, &go_up);
    if (go_up > 0)
    {
      fprintf(stderr, "[command] open %s: file has a different root, skipping\n", path);
      return;
    }
  }

  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] open %s: file not found, skipping\n", path);
    return;
  }

  flush_changes(ps, ui);

  int changed = -1;
  bool had_edit_data = (e->edit_data != NULL);

  if (e->edit_data)
  {
    fprintf(stderr, "[command] open %s: known file, updating\n", path);
    changed = find_diff(e->edit_data, data, size);
    if (e->edit_data->cap < size)
      fz_resize_buffer(ps->ctx, e->edit_data, size + 128);
    e->edit_data->len = size;
    memcpy(e->edit_data->data, data, size);
    // The editor now owns the contents (not a convergence stash anymore)
    e->edit_data_from_convergence = false;
  }
  else
  {
    fprintf(stderr, "[command] open %s: new file\n", path);
    e->edit_data = fz_new_buffer_from_copied_data(ps->ctx, data, size);
    if (e->fs_data)
      changed = find_diff(e->fs_data, data, size);
    else if (e->seen >= 0)
      changed = 0;
  }

  if (changed >= 0)
  {
    if (e->promised && !had_edit_data)
      fprintf(stderr, "[command] open %s: resolving deferred query\n", path);
    else
    {
      fprintf(stderr, "[command] open %s: changed offset is %d\n", path, changed);
      send(notify_file_changes, ui->eng, ps->ctx, e, changed);
    }
  }
}

static void interpret_close(struct persistent_state *ps,
                            ui_state *ui,
                            const char *path)
{
  int go_up = 0;
  path = relative_path(path, ps->doc_path, &go_up);
  if (go_up > 0)
  {
    fprintf(stderr, "[command] close %s: file has a different root, skipping\n", path);
    return;
  }

  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] close %s: file not found, skipping\n", path);
    return;
  }

  if (!e->edit_data)
  {
    fprintf(stderr, "[command] close %s: file not opened, skipping\n", path);
    return;
  }

  flush_changes(ps, ui);

  int changed = 0;

  if (e->fs_data)
    changed = find_diff(e->fs_data, e->edit_data->data, e->edit_data->len);

  fz_drop_buffer(ps->ctx, e->edit_data);
  e->edit_data = NULL;
  e->edit_data_from_convergence = false;

  fprintf(stderr, "[command] close %s: closing, changed offset %d\n", path,
          changed);

  send(notify_file_changes, ui->eng, ps->ctx, e, changed);
}


static uint32_t convert_color(fz_context *ctx, vstack *stack, float frgb[3])
{
  uint8_t rgb[3];

  for (int i = 0; i < 3; ++i)
    rgb[i] = fz_clampi(frgb[i] * 255.0, 0, 255) & 0xFF;

  return (rgb[0] << 16) | (rgb[1] << 8) | (rgb[2]);
}

#if !SDL_VERSION_ATLEAST(2, 0, 16)
static void
SDL_SetWindowAlwaysOnTop(SDL_Window *window, SDL_bool state)
{
  (void)window;
  (void)state;
  fprintf(stderr, "[info] stay-on-top feature is not available with "
                  "SDL older than 2.16.0\n");
}
#endif


static void interpret_register(struct persistent_state *ps,
                               ui_state *ui,
                               const char *path)
{
  if (path[0] == '/')
  {
    int go_up = 0;
    path = relative_path(path, ps->doc_path, &go_up);
    if (go_up > 0)
    {
      fprintf(stderr, "[command] register %s: file has a different root, skipping\n", path);
      return;
    }
  }

  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] register %s: file not found, skipping\n", path);
    return;
  }

  e->promised = true;
  fprintf(stderr, "[command] register %s: marked as promised\n", path);
}

static void interpret_command(struct persistent_state *ps,
                              ui_state *ui,
                              vstack *stack,
                              val command)
{
  struct editor_command cmd;
  if (!editor_parse(ps->ctx, stack, command, &cmd))
    return;

  switch (cmd.tag)
  {
    case EDIT_OPEN:
      if (cmd.open.base64)
      {
        unsigned char *buf = malloc(cmd.open.length);
        if (!buf) break;
        memcpy(buf, cmd.open.data, cmd.open.length);
        int decoded_len = base64_decode(buf, cmd.open.length);
        if (decoded_len < 0)
        {
          fprintf(stderr, "[command] open-base64: invalid base64 data\n");
          free(buf);
          break;
        }
        interpret_open(ps, ui, cmd.open.path, (const char *)buf, decoded_len);
        free(buf);
      }
      else
        interpret_open(ps, ui, cmd.open.path, cmd.open.data, cmd.open.length);
      break;

    case EDIT_CLOSE:
      interpret_close(ps, ui, cmd.close.path);
      break;

    case EDIT_CHANGE:
      interpret_change(ps, ui, &cmd.change);
      break;

    case EDIT_THEME:
    {
      struct txp_theme *theme = ui->theme;
      theme->background = convert_color(ps->ctx, stack, cmd.theme.bg);
      theme->foreground = convert_color(ps->ctx, stack, cmd.theme.fg);
      theme->themed = 1;
      txp_page_buffer_flush(ui->buffer);
      schedule_event(RENDER_EVENT);
      fprintf(stderr, "[command] theme %x %x\n",
              theme->background, theme->foreground);
    }
    break;

    case EDIT_PREVIOUS_PAGE:
      previous_page(ps->ctx, ui, 0);
      schedule_event(RENDER_EVENT);
      break;

    case EDIT_NEXT_PAGE:
      next_page(ps->ctx, ui, 0);
      schedule_event(RENDER_EVENT);
      break;

    case EDIT_MOVE_WINDOW:
    {
      float x = cmd.move_window.x, y = cmd.move_window.y,
            w = cmd.move_window.w, h = cmd.move_window.h;
      int x0 = x, y0 = y;
      SDL_SetWindowPosition(ui->window, x, y);
      SDL_GetWindowPosition(ui->window, &x0, &y0);
      SDL_SetWindowSize(ui->window, w + x - x0, h + y - y0);
      fprintf(stderr, "[command] move-window %f %f %f %f (pos: %d %d)\n",
              x, y, w, h, x0, y0);
    }
    break;

    case EDIT_MAP_WINDOW:
    {
      float x = cmd.move_window.x, y = cmd.move_window.y,
            w = cmd.move_window.w, h = cmd.move_window.h;
      int x0 = x, y0 = y;
      SDL_SetWindowBordered(ui->window, SDL_FALSE);
      SDL_SetWindowAlwaysOnTop(ui->window, SDL_TRUE);
      SDL_SetWindowPosition(ui->window, x, y);
      SDL_GetWindowPosition(ui->window, &x0, &y0);
      SDL_SetWindowSize(ui->window, w + x - x0, h + y - y0);
      fprintf(stderr, "[command] map-window %f %f %f %f (pos: %d %d)\n",
              x, y, w, h, x0, y0);
    }
    break;

    case EDIT_UNMAP_WINDOW:
    {
      if (!(SDL_GetWindowFlags(ui->window) & SDL_WINDOW_INPUT_FOCUS))
        SDL_SetWindowBordered(ui->window, SDL_TRUE);
      SDL_SetWindowAlwaysOnTop(ui->window, SDL_FALSE);
      fprintf(stderr, "[command] unmap-window\n");
    }
    break;

    case EDIT_RESCAN:
      schedule_event(SCAN_EVENT);
      break;

    case EDIT_STAY_ON_TOP:
      SDL_SetWindowAlwaysOnTop(ui->window, cmd.stay_on_top.status);
      fprintf(stderr, "[command] stay-on-top %d\n", cmd.stay_on_top.status);
      break;

    case EDIT_SYNCTEX_FORWARD:
    {
      fz_buffer *buf;
      synctex_t *stx = send(synctex, ui->eng, &buf);
      int go_up = 0;
      const char *path = relative_path(cmd.synctex_forward.path, ps->doc_path, &go_up);
      if (go_up > 0)
      {
        fprintf(stderr,
                "[command] synctex-forward %s: file has a different root, skipping\n",
                path);
      }
      else
      {
        int page = txp_viewer_current_page(ps->ctx, ui->viewer, ui->pages);
        synctex_set_target(stx, page < 0 ? 0 : page, path, cmd.synctex_forward.line);
        schedule_event(STDIN_EVENT);
      }
    }
    break;

    case EDIT_CROP:
      ui_set_crop(ps->ctx, ui, !txp_page_collection_get_crop(ui->pages));
      schedule_event(RENDER_EVENT);
      break;

    case EDIT_INVERT:
      ui->theme->inverted = !ui->theme->inverted;
      txp_page_buffer_flush(ui->buffer);
      schedule_event(RENDER_EVENT);
      break;

    case EDIT_REGISTER:
      interpret_register(ps, ui, cmd.reg.path);
      break;

    case EDIT_PAUSE:
      ps->paused = true;
      fprintf(stderr, "[command] pause: engine stepping suspended\n");
      break;

    case EDIT_RESUME:
      ps->paused = false;
      fprintf(stderr, "[command] resume: engine stepping enabled\n");
      // Spawn the worker if -stream started paused (no-op otherwise)
      send(step, ui->eng, ps->ctx, true);
      check_pages(ui);
      schedule_event(SCAN_EVENT);
      break;

    case EDIT_RERUN:
      ps->rerun_enabled = cmd.rerun.status;
      fprintf(stderr, "[command] rerun %s\n",
              ps->rerun_enabled ? "enabled" : "disabled");
      break;

    case EDIT_RERUN_ONCE:
      ps->rerun_once_pending = true;
      fprintf(stderr, "[command] rerun-once: pending immediate pass\n");
      schedule_event(SCAN_EVENT);
      break;
  }
}

// Show whole pages in fullscreen, restore the view when leaving it
static void sync_fullscreen_state(fz_context *ctx,
                                  struct fullscreen_state *fs,
                                  ui_state *ui)
{
  bool cur_fs = (SDL_GetWindowFlags(ui->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
  if (cur_fs == fs->prev_fs) return;

  txp_viewer *v = ui->viewer;
  if (cur_fs) {
    if (!fs->has_backup) {
      fs->crop = txp_page_collection_get_crop(ui->pages);
      fs->fit  = v->fit;
      fs->zoom = v->zoom;
      fs->has_backup = true;
    }
    ui_set_crop(ctx, ui, false);
    txp_viewer_set_fit(ctx, v, ui->pages, TXP_FIT_PAGE);
  } else if (fs->has_backup) {
    ui_set_crop(ctx, ui, fs->crop);
    txp_viewer_set_fit(ctx, v, ui->pages, fs->fit);
    v->zoom = v->target_zoom = fs->zoom;
    fs->has_backup = false;
  }
  fs->prev_fs = cur_fs;
}

// The document changed: forget the pages that are no longer valid, restart
// the engine
static void document_changed(struct persistent_state *ps, ui_state *ui)
{
  if (ui->reload_all)
    txp_page_collection_invalidate_after(ui->pages, 0);
  check_pages(ui);
  if (!ps->paused)
  {
    send(step, ui->eng, ps->ctx, true);
    check_pages(ui);
  }
  schedule_event(RELOAD_EVENT);
}

/* Entry point */

bool texpresso_main(struct persistent_state *ps)
{
  editor_set_protocol(ps->protocol);
  editor_set_line_output(ps->line_output);
  pstate = ps;
  fz_context *ctx = ps->ctx;

  struct fullscreen_state fs = {
    .has_backup = false,
    .prev_fs = (SDL_GetWindowFlags(ps->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0,
  };

  ui_state raw_ui = {0,}, *ui = &raw_ui;

  ui->window = ps->window;

  bool using_texlive = 0;

  if (ps->use_texlive)
  {
    if (!texlive_available())
    {
      fprintf(stderr,
              "[fatal] cannot find kpsewhich command for texlive integration "
              "(please make sure it is installed and visible in PATH)\n");
      return 0;
    }
    using_texlive = 1;
  }
  else if (ps->use_tectonic)
  {
    if (!tectonic_available())
    {
      fprintf(stderr,
              "[fatal] cannot find tectonic "
              "(please make sure it is installed and visible in PATH)\n");
      return 0;
    }
  }
  else if (!(using_texlive = texlive_available()) && !tectonic_available())
  {
    fprintf(stderr,
            "[fatal] cannot find tectonic nor kpsewhich (texlive)"
            "(please make sure at least one of them is installed and visible in PATH)\n");
    return 0;
  }

  const char *doc_ext = NULL;

  for (const char *ptr = ps->doc_name; *ptr; ptr++)
    if (*ptr == '.')
      doc_ext = ptr + 1;

  char engine_path[4096];
  find_engine(engine_path, ps->exe_path);
  fprintf(stderr, "[info] engine path: %s\n", engine_path);

  if (doc_ext && strcmp(doc_ext, "pdf") == 0)
  {
    ui->eng = txp_create_pdf_engine(ctx, ps->doc_name);
    ui->reload_all = true;
  }
  else
  {
    dvi_reshooks hooks;
    if (using_texlive)
      hooks = dvi_texlive_hooks(ctx, ps->doc_path);
    else
      hooks = dvi_tectonic_hooks(ctx, ps->doc_path);

    if (doc_ext && (strcmp(doc_ext, "dvi") == 0 || strcmp(doc_ext, "xdv") == 0))
    {
      ui->eng = txp_create_dvi_engine(ctx, ps->doc_name, hooks);
      ui->reload_all = true;
    }
    else
      ui->eng = txp_create_tex_engine(ctx, engine_path, using_texlive,
                                      ps->stream_mode, ps->inclusion_path,
                                      ps->doc_name, hooks);
  }

  ui->sdl_renderer = ps->renderer;
  ui->buffer = txp_page_buffer_new(ctx, ui->sdl_renderer);

  // Pages, view and theme survive reloads
  if (ps->initial.initialized)
  {
    // Keep showing the previous pages until the new engine replaces them
    txp_page_collection_invalidate_after(&ps->initial.pages, 0);
    editor_reset_sync();
  }
  else
  {
    txp_page_collection_init(&ps->initial.pages);
    txp_viewer_init(&ps->initial.viewer,
                    ps->continuous ? TXP_VIEW_CONTINUOUS : TXP_VIEW_SINGLE);
    ps->initial.theme = (struct txp_theme){
      .themed = 1,
      .background = 0xFFFFFF,
      .foreground = 0x000000,
    };
  }
  ui->pages = &ps->initial.pages;
  ui->viewer = &ps->initial.viewer;
  ui->theme = &ps->initial.theme;
  txp_viewer_stop(ui->viewer);

  ui->mouse_status = UI_MOUSE_NONE;
  ui->last_mouse_x = -1000;
  ui->last_mouse_y = -1000;
  ui->last_click_ticks = SDL_GetTicks() - 200000000;
  ui->sel.page = -1;
  ui->last_update = SDL_GetPerformanceCounter();
  ui->was_idle = true;

  bool quit = 0, reload = 0;
  if (!ps->paused)
    send(step, ui->eng, ctx, true);
  render(ctx, ui);
  schedule_event(RELOAD_EVENT);

  struct repaint_on_resize_env repaint_on_resize_env = {.ctx = ctx, .ui = ui};
  SDL_AddEventWatch(repaint_on_resize, &repaint_on_resize_env);

  vstack *cmd_stack = vstack_new(ctx);
  prot_parser cmd_parser;
  prot_initialize(&cmd_parser, (ps->protocol == EDITOR_JSON));

  // Start watching stdin
  int poll_stdin_pipe[2];
  if (pipe(poll_stdin_pipe) == -1)
  {
    perror("pipe");
    abort();
  }

  SDL_Thread *poll_stdin_thread =
    SDL_CreateThread(poll_stdin_thread_main, "poll_stdin_thread", poll_stdin_pipe);
  bool stdin_eof = 0;
  int rerun_count = 0;
  uint32_t last_frame_ticks = SDL_GetTicks();

  while (!quit)
  {
    SDL_Event e;
    bool has_event = SDL_PollEvent(&e);

    // Process stdin
    send(begin_changes, ui->eng, ctx);
    char buffer[4096];
    int n = -1;
    while (!stdin_eof && poll_stdin() && (n = read(STDIN_FILENO, buffer, 4096)) != 0)
    {
      if (n == -1)
      {
        if (errno == EINTR)
          continue;
        perror("poll stdin");
        break;
      }

      fprintf(stderr, "stdin: %.*s\n", n, buffer);

      const char *ptr = buffer, *lim = buffer + n;
      fz_try(ctx)
      {
        while ((ptr = prot_parse(ctx, &cmd_parser, cmd_stack, ptr, lim)))
        {
          val cmds = vstack_get_values(ctx, cmd_stack);
          int n_cmds = val_array_length(ctx, cmd_stack, cmds);
          for (int i = 0; i < n_cmds; i++)
          {
            val cmd = val_array_get(ctx, cmd_stack, cmds, i);
            interpret_command(ps, ui, cmd_stack, cmd);
          }
        }
      }
      fz_catch(ctx)
      {
        fprintf(stderr, "error while reading stdin commands: %s\n",
                fz_caught_message(ctx));
        vstack_reset(ctx, cmd_stack);
        prot_reinitialize(&cmd_parser);
      }
    }
    if (n == 0) stdin_eof = 1;

    if (send(end_changes, ui->eng, ctx))
    {
      document_changed(ps, ui);
      rerun_count = 0;
    }

    // Process document
    {
      bool advance = !ps->paused && advance_engine(ctx, ui);
      send(finish_convergence, ui->eng, ctx);
      if (sync_pages(ps, ui))
        advance = true;
      fflush(stdout);

      // Fire an on-demand rerun as soon as the engine is ready, regardless of
      // idle state. Runs in the main body of the loop (not inside the idle
      // wait) so it triggers even during active compilation cycles.
      bool aux_ready = !send(is_finishing, ui->eng)
                       && send(aux_dirty, ui->eng);
      if (ps->rerun_once_pending && aux_ready)
      {
        ps->rerun_once_pending = false;
        fprintf(stderr, "[rerun] on-demand: finishing pass\n");
        send(start_finishing, ui->eng);
        check_pages(ui);
        schedule_event(RELOAD_EVENT);
        continue;
      }

      fz_buffer *buf;
      synctex_t *stx = send(synctex, ui->eng, &buf);
      int page = -1, x = -1, y = -1;
      if (synctex_find_target(ctx, stx, buf, &page, &x, &y))
      {
        fprintf(stderr, "[synctex forward] sync: hit page %d, coordinates (%d, %d)\n",
                page, x, y);
        float f = send(scale_factor, ui->eng);
        txp_doc_coord c = {.page = page, .x = f * x, .y = f * y};
        if (page >= 0 && page < txp_page_collection_count(ui->pages))
          txp_viewer_reveal(ctx, ui->viewer, ui->pages, c);
        ui->dirty = true;
      }

      if (!has_event)
      {
        // Animate and redraw once the pending events have been processed
        frame(ctx, ui);
        last_frame_ticks = SDL_GetTicks();

        if (advance)
          continue;
        // Nothing might wake us up if the engine stopped without output
        if (initialize_done(ps, ui, stdin_eof))
        {
          fprintf(stderr, "[info] Initialize mode: terminating engine process\n");
          quit = 1;
          continue;
        }

        if (!txp_viewer_is_idle(ui->viewer))
        {
          // Next animation frame (stdin is polled at each iteration)
          if (!SDL_WaitEventTimeout(&e, 10))
            continue;
          has_event = 1;
        }
        else
        {
          if (!stdin_eof)
            wakeup_poll_thread(poll_stdin_pipe, 'c');

          bool rerun_eligible = ps->rerun_enabled
                                && rerun_count < MAX_RERUNS
                                && aux_ready;
          if (rerun_eligible)
            has_event = SDL_WaitEventTimeout(&e, T_IDLE_MS);
          else
            has_event = SDL_WaitEvent(&e);
          if (!has_event)
          {
            if (rerun_eligible)
            {
              rerun_count++;
              fprintf(stderr, "[rerun] idle %dms: finishing pass %d/%d\n",
                      T_IDLE_MS, rerun_count, MAX_RERUNS);
              send(start_finishing, ui->eng);
              check_pages(ui);
              schedule_event(RELOAD_EVENT);
              continue;
            }
            fprintf(stderr, "SDL_WaitEvent error: %s\n", SDL_GetError());
            break;
          }
        }
      }
      else if (SDL_GetTicks() - last_frame_ticks >= 16)
      {
        // Keep animating during a stream of events
        frame(ctx, ui);
        last_frame_ticks = SDL_GetTicks();
      }
    }

    // Process event
    switch (e.type)
    {
      case SDL_QUIT:
        quit = 1;
        break;

      case SDL_KEYDOWN:
      {
        bool shift = SDL_GetModState() & KMOD_SHIFT;
        switch (e.key.keysym.sym)
        {
          case SDLK_LEFT:
          case SDLK_PAGEUP:
            previous_page(ctx, ui, 0);
            break;

          case SDLK_RIGHT:
          case SDLK_PAGEDOWN:
            next_page(ctx, ui, 0);
            break;

          case SDLK_UP:
            ui_screen(ctx, ui, -1);
            break;

          case SDLK_DOWN:
          case SDLK_SPACE:
            ui_screen(ctx, ui, 1);
            break;

          case SDLK_HOME:
            if (is_continuous(ui))
              txp_viewer_goto_page(ctx, ui->viewer, ui->pages, 0, TXP_ALIGN_TOP);
            break;

          case SDLK_END:
            if (is_continuous(ui))
              txp_viewer_goto_page(ctx, ui->viewer, ui->pages,
                                   txp_page_collection_count(ui->pages) - 1,
                                   TXP_ALIGN_BOTTOM);
            break;

          case SDLK_PLUS:
          case SDLK_KP_PLUS:
          case SDLK_EQUALS: // Handles standard '=' key (often sharing '+' on keyboards)
            ui_zoom(ctx, ui, 1.1);
            break;

          case SDLK_MINUS:
          case SDLK_KP_MINUS:
            ui_zoom(ctx, ui, 1 / 1.1);
            break;

          case SDLK_h:
            ui_pan_x(ctx, ui, shift ? 1.0/5.0 : 1.0/25.0);
            break;

          case SDLK_l:
            ui_pan_x(ctx, ui, shift ? -1.0/5.0 : -1.0/25.0);
            break;

          case SDLK_j:
            ui_pan(ctx, ui, shift ? -1.0/5.0 : -1.0/25.0);
            break;

          case SDLK_k:
            ui_pan(ctx, ui, shift ? 1.0/5.0 : 1.0/25.0);
            break;

          case SDLK_b:
            if (shift)
            {
              // Shift + B: Toggle Window Border
              SDL_SetWindowBordered(
                  ui->window,
                  !!(SDL_GetWindowFlags(ui->window) & SDL_WINDOW_BORDERLESS));
            }
            else
              ui_screen(ctx, ui, -1); // Lowercase b: Page up (matching up arrow)
            break;

          case SDLK_w:
            txp_viewer_set_fit(ctx, ui->viewer, ui->pages, TXP_FIT_WIDTH);
            break;

          case SDLK_p:
            txp_viewer_set_fit(ctx, ui->viewer, ui->pages, TXP_FIT_PAGE);
            break;

          case SDLK_v:
            ui_set_mode(ctx, ui, is_continuous(ui) ? TXP_VIEW_SINGLE
                                                   : TXP_VIEW_CONTINUOUS);
            break;

          case SDLK_t:
            SDL_SetWindowAlwaysOnTop(
                ui->window,
                !(SDL_GetWindowFlags(ui->window) & SDL_WINDOW_ALWAYS_ON_TOP));
            break;

          case SDLK_c:
            ui_set_crop(ctx, ui, !txp_page_collection_get_crop(ui->pages));
            break;

          case SDLK_i:
            if (shift)
              ui->theme->themed = !ui->theme->themed;
            else
              ui->theme->inverted = !ui->theme->inverted;
            txp_page_buffer_flush(ui->buffer);
            ui->dirty = true;
            break;

          case SDLK_ESCAPE:
            SDL_SetWindowFullscreen(ui->window, 0);
            break;

          // Toggle Fullscreen
          case SDLK_f:
          case SDLK_F5:
          case SDLK_F11:
            SDL_SetWindowFullscreen(ui->window,
              (SDL_GetWindowFlags(ui->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ?
                0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            ui->dirty = true;
            break;

          // case SDLK_r:
          //   reload = 1;
          case SDLK_q:
            quit = 1;
            break;
        }
        break;
      }

      case SDL_MOUSEWHEEL:
        {
           int mx = 0, my = 0;
           float px = 0, py = 0;
#if SDL_VERSION_ATLEAST(2, 26, 0)
           mx = e.wheel.mouseX;
           my = e.wheel.mouseY;
#else
           SDL_GetMouseState(&mx, &my);
#endif
#if SDL_VERSION_ATLEAST(2, 0, 18)
          px = e.wheel.preciseX;
          py = e.wheel.preciseY;
#else
          px = e.wheel.x;
          py = e.wheel.y;
#endif
          SDL_Keymod mod = SDL_GetModState();
          ui_mouse_wheel(ctx, ui, px, py, mx, my,
                         !!(mod & KMOD_CTRL), !!(mod & KMOD_SHIFT));
        }
        break;

      case SDL_MOUSEBUTTONDOWN:
        if (e.button.button == SDL_BUTTON_LEFT)
          ui_mouse_down(ps, ui, e.button.x, e.button.y, e.button.clicks,
                        SDL_GetModState() & KMOD_CTRL);
        break;

      case SDL_MOUSEBUTTONUP:
        if (e.button.button == SDL_BUTTON_LEFT)
          ui_mouse_up(ps, ui, e.button.x, e.button.y);
        break;

      case SDL_MOUSEMOTION:
        ui_mouse_move(ps, ui, e.motion.x, e.motion.y, e.motion.xrel, e.motion.yrel);
        break;

      case SDL_WINDOWEVENT:
        switch (e.window.event)
        {
          case SDL_WINDOWEVENT_SIZE_CHANGED:
          case SDL_WINDOWEVENT_RESIZED:
          case SDL_WINDOWEVENT_EXPOSED:
            sync_fullscreen_state(ctx, &fs, ui);
            ui->dirty = true;
            break;
        }
        break;
    }

    if (e.type == ps->custom_event)
    {
      SDL_AtomicSet(e.user.data1, 0);
      switch (e.user.code)
      {
        case SCAN_EVENT:
          if (should_reload_binary())
          {
            quit = reload = 1;
            continue;
          }
          send(begin_changes, ui->eng, ctx);
          flush_changes(ps, ui);
          send(detect_changes, ui->eng, ctx);
          if (send(end_changes, ui->eng, ctx))
            document_changed(ps, ui);
          break;

        case RENDER_EVENT:
          render(ctx, ui);
          last_frame_ticks = SDL_GetTicks();
          send(begin_changes, ui->eng, ctx);
          flush_changes(ps, ui);
          if (send(end_changes, ui->eng, ctx))
            document_changed(ps, ui);
          break;

        case RELOAD_EVENT:
          sync_pages(ps, ui);
          ui->dirty = true;
          break;

        case STDIN_EVENT:
          break;
      }
    }
    if (initialize_done(ps, ui, stdin_eof))
    {
      fprintf(stderr, "[info] Initialize mode: terminating engine process\n");
      quit = 1;
    }
  }

  {
    int status = 0;
    wakeup_poll_thread(poll_stdin_pipe, 'q');
    SDL_WaitThread(poll_stdin_thread, &status);
    close(poll_stdin_pipe[0]);
    close(poll_stdin_pipe[1]);
  }

  SDL_DelEventWatch(repaint_on_resize, &repaint_on_resize_env);
  SDL_SetRelativeMouseMode(SDL_FALSE);

  // Pages, view and theme stay in ps->initial for the next run
  txp_viewer_stop(ui->viewer);
  if (reload)
    ps->initial.initialized = 1;
  else
  {
    txp_page_collection_finalize(ctx, ui->pages);
    ps->initial.initialized = 0;
  }

  vstack_free(ctx, cmd_stack);
  txp_page_buffer_free(ctx, ui->buffer);
  send(destroy, ui->eng, ctx);

  return reload;
}
