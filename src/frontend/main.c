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
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "base64.h"
#include "driver.h"
#include "editor.h"
#include "engine.h"
#include "fd_poll.h"
#include "mydvi.h"
#include "pagecollection.h"
#include "prot_parser.h"
#include "providers.h"
#include "synctex.h"
#include "utf_mapping.h"
#include "vstack.h"
#include "pagebuffer.h"
#include "viewer.h"

/* --- Configuration Constants --- */

#define ENGINE_STEP_LIMIT 10
#define ENGINE_TIMEOUT_NS 5000000
#define DEFAULT_ENGINE_NAME "texpresso-xetex"
#define MAX_BUFFERED_OPS 64
#define MAX_BUFFERED_CHARS 4096
#define PATH_BUFFER_SIZE 4096

/* --- Helper Macros --- */

#ifdef __APPLE__
#define st_time(a) st_##a##timespec
#else
#define st_time(a) st_##a##tim
#endif

/* --- UI State Definitions --- */

enum ui_mouse_status
{
  UI_MOUSE_NONE,
  UI_MOUSE_SELECT,
  UI_MOUSE_MOVE,
};

typedef struct
{
  Engine *eng;
  SDL_Renderer *sdl_renderer;
  SDL_Window *window;
  fd_poller *fdpoll;
  PageBuffer pbuff;
  Viewer viewer;

  // Mouse input state
  int last_mouse_x, last_mouse_y;
  enum ui_mouse_status mouse_status;

  // Internal state of the advance_engine function to detect transitions from
  // advancing to idle.
  bool advancing;
} ui_state;

/* --- Delayed Change Buffer --- */

static struct
{
  char buffer[MAX_BUFFERED_CHARS];
  int cursor;
  struct editor_change op[MAX_BUFFERED_OPS];
  int count;
} delayed_changes = {0};

/* --- Forward Declarations --- */

static void handle_sdl_event(SDL_Event *e,
                             ui_state *ui,
                             struct persistent_state *ps);
static bool update_pan(fz_context *ctx,
                       ui_state *ui,
                       int dx,
                       int dy,
                       float scale_x,
                       float scale_y);
static void interpret_command(struct persistent_state *ps,
                              ui_state *ui,
                              vstack *stack,
                              val command);

/* --- Utility Functions --- */

static bool is_more_recent(uint64_t *time, char *candidate)
{
  struct stat st;
  if (stat(candidate, &st) == 0 && st.st_time(c).tv_sec > *time)
  {
    *time = st.st_time(c).tv_sec;
    return true;
  }
  return false;
}

static void find_engine(char engine_path[PATH_BUFFER_SIZE],
                        const char *exec_path)
{
  strncpy(engine_path, exec_path, PATH_BUFFER_SIZE - 1);
  engine_path[PATH_BUFFER_SIZE - 1] = '\0';

  char *basename = NULL;
  for (int i = 0; i < PATH_BUFFER_SIZE && engine_path[i]; ++i)
  {
    if (engine_path[i] == '/')
      basename = engine_path + i + 1;
  }

  uint64_t time = 0;
  if (basename)
  {
    strcpy(basename, DEFAULT_ENGINE_NAME);
    if (!is_more_recent(&time, engine_path))
    {
      strcpy(engine_path, DEFAULT_ENGINE_NAME);
    }
  }
}

/*
 * Calculates the relative path of 'path' with respect to 'dir'.
 * Returns the relative string and sets *go_up to the number of "../" needed.
 * If the path is outside the directory tree, go_up > 0.
 */
static const char *relative_path(const char *path, const char *dir, int *go_up)
{
  const char *rel_path = path, *dir_path = dir;

  // 1. Skip common parts
  while (*rel_path && *rel_path == *dir_path)
  {
    if (*rel_path == '/')
    {
      while (*rel_path == '/')
        rel_path += 1;
      while (*dir_path == '/')
        dir_path += 1;
    }
    else
    {
      rel_path += 1;
      dir_path += 1;
    }
  }

  // 2. Go back to last directory separator
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
      assert (*dir_path == '/');
      rel_path += 1;
      dir_path += 1;
    }
  }

  // 3. Count number of '../' needed to go back to dir root
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
      {
        dir_path += 1;
      }
    }
  }

  while (*rel_path == '/')
    rel_path += 1;

  return rel_path;
}

/*
 * Find the offset of the first byte that differs between an fz_buffer and a raw
 * buffer.
 */
static int find_diff(const fz_buffer *buf, const void *data, int size)
{
  const unsigned char *ptr = data;
  int i, len = fz_mini(buf->len, size);
  for (i = 0; i < len && buf->data[i] == ptr[i]; ++i)
    ;
  return i;
}

/* --- Rendering and Layout Utilities --- */

static float zoom_factor(int count)
{
  return expf((float)count / 5000.0f);
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

static bool render(struct persistent_state *ps, ui_state *ui, bool need);

struct repaint_on_resize_env
{
  struct persistent_state *ps;
  ui_state *ui;
};

static int repaint_on_resize(void *data, SDL_Event *event)
{
  struct repaint_on_resize_env *env = data;
  if (event->type == SDL_WINDOWEVENT &&
      event->window.event == SDL_WINDOWEVENT_RESIZED &&
      SDL_GetWindowFromID(event->window.windowID) == env->ui->window)
  {
    int win_w, win_h;
    SDL_GetWindowSize(env->ps->window, &win_w, &win_h);
    viewer_update(&env->ui->viewer, &env->ps->pcoll, 0.0, win_w, win_h);
    render(env->ps, env->ui, true);
  }
  return 0;
}

// Always compute SyncTeX for now
static bool need_synctex(void)
{
  return true;
}

static bool need_advance(struct persistent_state *ps, ui_state *ui)
{
  // 1. No need to advance if the document is fully rendered
  if (send(get_status, ui->eng) == DOC_TERMINATED)
    return false;

  // Set the target to the last page currently visible to the user
  VisibleRange vr = viewer_get_visible_range(ps->ctx, &ui->viewer, &ps->pcoll);
  int target_page = vr.last_page < 0 ? 1 : vr.last_page;

  // 2. Proceed if the target page has not been rendered yet.
  if (send(page_count, ui->eng) <= target_page ||
      pagecollection_count(&ps->pcoll) == target_page + 1)
    return true;

  // 3. Proceed also if synctex has not yet reached the target page
  //    (this can happen because of buffering).
  fz_buffer *buf;
  TexSynctex *stx = send(synctex, ui->eng, &buf);
  if (need_synctex() && synctex_page_count(stx) <= target_page)
    return true;

  // 4. Proceed if a synctex target has been set
  //    (looking for a source line that has not been reached yet).
  if (synctex_has_target(stx))
    return true;

  return false;
}

/**
 * Advances the document rendering engine for a bounded amount of time.
 *
 * This function is designed to be called within the main UI loop. It ensures
 * that rendering progresses without blocking the UI for too long.
 * It also handles the transition from "rendering active" to "idle" and triggers
 * a UI flush to stabilize the display.
 *
 * @param ctx The MuPDF context.
 * @param ui The UI state structure.
 * @return true if the engine still has work to do (needs another slice), false if idle or done.
 */
static bool advance_engine(struct persistent_state *ps, ui_state *ui)
{
  // 1. Determine if we need to render more pages or resolve SyncTeX targets.
  //    This checks against the last visible page and engine status.
  bool need = need_advance(ps, ui);

  // 2. Handle State Transition: Rendering -> Idle
  //    If we were previously advancing (ui->advancing is true) but now we don't
  //    need to, it means we just finished the current rendering batch. We flush
  //    the editor state here to ensure the UI displays the complete, stable
  //    result before we potentially stop rendering or switch to event waiting.
  //    This prevents visual flickering where the user might see a half-rendered
  //    page.
  if (!need && ui->advancing)
    editor_flush();

  // 3. Update the internal "advancing" flag to reflect the current requirement.
  ui->advancing = need;

  // 4. Early Exit: If no work is needed, return immediately.
  //    This allows the caller (UI loop) to switch to an event-waiting mode.
  if (!need)
    return false;

  // 5. Start Time Measurement
  //    We use CLOCK_MONOTONIC to avoid issues with system clock adjustments.
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);

  // 6. Execute Rendering Loop
  //    We run in a loop, executing steps until:
  //    a) The engine reports it's done (send returns false).
  //    b) We no longer need to advance (user scrolled away or target reached).
  //    c) We hit the step limit AND the time budget.
  int steps = ENGINE_STEP_LIMIT;
  while (need)
  {
    // Execute one step of the rendering engine.
    // If the engine terminates or errors, break immediately.
    if (!send(step, ui->eng, ps->ctx, false))
      break;

    // Decrement the step counter for this time slice.
    steps -= 1;

    // Re-evaluate if we still need to render.
    need = need_advance(ps, ui);

    // 7. Time Budget Check
    //    If we have exhausted our step limit for this slice, check the elapsed
    //    time.
    if (steps == 0)
    {
      // Reset step counter for the next slice if we continue.
      steps = ENGINE_STEP_LIMIT;

      struct timespec curr;
      clock_gettime(CLOCK_MONOTONIC, &curr);

      // Calculate elapsed time in nanoseconds.
      // (curr_sec - start_sec) * 1e9 + (curr_nsec - start_nsec)
      int64_t delta = (int64_t)(curr.tv_sec - start.tv_sec) * 1000000000LL +
                      (curr.tv_nsec - start.tv_nsec);

      // If we have exceeded the time budget (e.g., 10ms), break to yield
      // control. This ensures the UI remains responsive even if the rendering
      // engine is slow.
      if (delta > ENGINE_TIMEOUT_NS)
        break;
    }
  }

  // 8. Return Status
  //    Return the current state of 'need'.
  //    - If true: We still have work to do, but we hit a limit (time or steps).
  //               The caller should try again soon.
  //    - If false: We are done with the current batch (either finished or user
  //    moved on).
  return need;
}

/* --- Input Handling (State Updates) --- */

static void ui_mouse_down(struct persistent_state *ps,
                          ui_state *ui,
                          int x,
                          int y,
                          int clicks,
                          bool ctrl)
{
  if (ctrl)
  {
    ui->mouse_status = UI_MOUSE_MOVE;
  }
  else
  {
    ui->mouse_status = UI_MOUSE_SELECT;
    fz_point scale = get_scale_factor(ui->window);
    fz_point p = fz_make_point(scale.x * x, scale.y * y);
    bool diff = false;

    // FIXME
    // if (clicks == 1)
    // {
    //   diff = txp_renderer_start_selection(ps->ctx, ui->doc_renderer, p);
    //   diff = txp_renderer_select_char(ps->ctx, ui->doc_renderer, p) || diff;

    //   fz_buffer *buf;
    //   TexSynctex *stx = send(synctex, ui->eng, &buf);
    //   if (stx && buf)
    //   {
    //     fz_point pt =
    //         txp_renderer_screen_to_document(ps->ctx, ui->doc_renderer, p);
    //     float f = 1 / send(scale_factor, ui->eng);
    //     synctex_scan(ps->ctx, stx, buf, ps->doc_path, get_current_page(ui),
    //                  f * pt.x, f * pt.y);
    //   }
    // }
    // else if (clicks == 2)
    // {
    //   diff = txp_renderer_select_word(ps->ctx, ui->doc_renderer, p);
    // }

    if (diff)
      ps->schedule_event(UI_RENDER_EVENT);
  }

  ui->last_mouse_x = x;
  ui->last_mouse_y = y;
}

static void ui_mouse_up(ui_state *ui)
{
  ui->mouse_status = UI_MOUSE_NONE;
}

static void ui_mouse_move(struct persistent_state *ps, ui_state *ui, int x, int y)
{
  fz_point scale = get_scale_factor(ui->window);
  bool needs_render = false;

  switch (ui->mouse_status)
  {
    case UI_MOUSE_NONE:
      break;

    case UI_MOUSE_SELECT:
    {
      // FIXME
      // fz_point p = fz_make_point(scale.x * x, scale.y * y);
      // if (txp_renderer_drag_selection(ps->ctx, ui->doc_renderer, p))
      //   needs_render = true;
      break;
    }

    case UI_MOUSE_MOVE:
    {
      if (update_pan(ps->ctx, ui, x - ui->last_mouse_x, y - ui->last_mouse_y,
                     scale.x, scale.y))
        needs_render = true;
      ui->last_mouse_x = x;
      ui->last_mouse_y = y;
      break;
    }
  }

  if (needs_render)
    ps->schedule_event(UI_RENDER_EVENT);
}

static bool update_pan(fz_context *ctx,
                       ui_state *ui,
                       int dx,
                       int dy,
                       float scale_x,
                       float scale_y)
{
  if (dx == 0 && dy == 0)
    return false;

  // FIXME
  // txp_renderer_config *config =
  //     txp_renderer_get_config(ctx, ui->doc_renderer);
  // config->pan.x += scale_x * dx;
  // config->pan.y += scale_y * dy;
  return true;
}

static void ui_mouse_wheel(struct persistent_state *ps,
                           ui_state *ui,
                           float dx,
                           float dy,
                           int mousex,
                           int mousey,
                           bool ctrl)
{
  if (ui->mouse_status != UI_MOUSE_NONE)
    return;

  // FIXME
  // txp_renderer_config *config = txp_renderer_get_config(ps->ctx, ui->doc_renderer);
  // bool needs_render = false;
  // fz_point scale = get_scale_factor(ui->window);

  // if (ctrl)
  // {
  //   SDL_FRect rect;
  //   if (dy != 0 &&
  //       txp_renderer_page_position(ps->ctx, ui->doc_renderer, &rect, NULL, NULL))
  //   {
  //     ui->zoom = fz_maxi(ui->zoom + dy * 100, 0);
  //     int ww, wh;
  //     SDL_GetWindowSize(ui->window, &ww, &wh);

  //     float mx = (mousex - ww / 2.0f) * scale.x;
  //     float my = (mousey - wh / 2.0f) * scale.y;

  //     float zf = zoom_factor(ui->zoom);
  //     float effective_zoom = zf / config->zoom;

  //     config->pan.x = mx + effective_zoom * (config->pan.x - mx);
  //     config->pan.y = my + effective_zoom * (config->pan.y - my);
  //     config->zoom = zf;
  //     needs_render = true;
  //   }
  // }
  // else
  // {
  //   float x = scale.x * dx * 5, y = scale.y * dy * 5;
  //   if (update_pan(ps->ctx, ui, (int)(x * 100), (int)(y * 100), 0.01f, 0.01f))
  //   {
  //     // Note: update_pan uses scale, here we pre-calculated.
  //     // Simplified: just update config directly for wheel
  //     config->pan.x -= x;
  //     config->pan.y += y;
  //     needs_render = true;
  //   }
  // }

  // if (needs_render)
  //   ps->schedule_event(UI_RENDER_EVENT);
}

/* --- Editor Command Interpretation --- */

static void realize_change(struct persistent_state *ps,
                           ui_state *ui,
                           struct editor_change *op)
{
  int go_up = 0;
  const char *path = relative_path(op->path, ps->doc_path, &go_up);
  if (go_up > 0)
  {
    fprintf(stderr,
            "[command] change %s: file has a different root, skipping\n", path);
    return;
  }

  FileEntry *e = send(find_file, ui->eng, ps->ctx, path);
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
      fprintf(stderr,
              "[command] change line %s: invalid line number, skipping\n",
              path);
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
      fprintf(stderr,
              "[command] change line %s: invalid line count, skipping\n", path);
      return;
    }
    remove -= offset;
  }
  else if (op->base == BASE_RANGE)
  {
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
      fprintf(stderr,
              "[command] change range %s: invalid start line, skipping\n",
              path);
      return;
    }

    int start_char_offset =
        utf16_to_utf8_offset(p + offset, p + len, op->range.start_char);
    if (start_char_offset == -1)
    {
      fprintf(stderr,
              "[command] change range %s: invalid start char, skipping\n",
              path);
      return;
    }

    remove = offset;
    offset += start_char_offset;

    line = op->range.end_line - op->range.start_line;
    if (line < 0)
    {
      fprintf(stderr, "[command] change range %s: invalid end line, skipping\n",
              path);
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
      fprintf(stderr, "[command] change range %s: invalid end line, skipping\n",
              path);
      return;
    }

    int end_char_offset =
        utf16_to_utf8_offset(p + remove, p + len, op->range.end_char);
    if (end_char_offset == -1)
    {
      fprintf(stderr, "[command] change range %s: invalid end char, skipping\n",
              path);
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

  send(notify_file_changes, ui->eng, ps->ctx, e, offset);
}

static void flush_changes(struct persistent_state *ps, ui_state *ui)
{
  int count = delayed_changes.count;
  if (count == 0)
    return;

  delayed_changes.count = 0;
  delayed_changes.cursor = 0;
  for (int i = 0; i < count; ++i)
  {
    struct editor_change *op = &delayed_changes.op[i];
    realize_change(ps, ui, op);
  }
}

static void interpret_change(struct persistent_state *ps,
                             ui_state *ui,
                             struct editor_change *op)
{
  int plen = strlen(op->path);
  int page_count = send(page_count, ui->eng);
  VisibleRange vr = viewer_get_visible_range(ps->ctx, &ui->viewer, &ps->pcoll);
  int cursor = delayed_changes.cursor;
  if ((page_count >= vr.first_page - 2 && page_count <= vr.last_page) &&
      send(get_status, ui->eng) == DOC_RUNNING &&
      delayed_changes.count < MAX_BUFFERED_OPS &&
      cursor + plen + 1 + op->length <= MAX_BUFFERED_CHARS)
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
  int go_up = 0;
  path = relative_path(path, ps->doc_path, &go_up);
  if (go_up > 0)
  {
    fprintf(stderr, "[command] open %s: file has a different root, skipping\n",
            path);
    return;
  }

  FileEntry *e = send(find_file, ui->eng, ps->ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] open %s: file not found, skipping\n", path);
    return;
  }

  flush_changes(ps, ui);
  int changed = -1;

  if (e->edit_data)
  {
    changed = find_diff(e->edit_data, data, size);
    if (e->edit_data->cap < size)
      fz_resize_buffer(ps->ctx, e->edit_data, size + 128);
    e->edit_data->len = size;
    memcpy(e->edit_data->data, data, size);
  }
  else
  {
    e->edit_data = fz_new_buffer_from_copied_data(ps->ctx, data, size);
    if (e->fs_data)
      changed = find_diff(e->fs_data, data, size);
    else if (e->seen >= 0)
      changed = 0;
  }

  if (changed >= 0)
    send(notify_file_changes, ui->eng, ps->ctx, e, changed);
}

static void interpret_close(struct persistent_state *ps,
                            ui_state *ui,
                            const char *path)
{
  int go_up = 0;
  path = relative_path(path, ps->doc_path, &go_up);
  if (go_up > 0)
  {
    fprintf(stderr, "[command] close %s: file has a different root, skipping\n",
            path);
    return;
  }

  FileEntry *e = send(find_file, ui->eng, ps->ctx, path);
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
static void SDL_SetWindowAlwaysOnTop(SDL_Window *window, SDL_bool state)
{
  (void)window;
  (void)state;
  fprintf(stderr,
          "[info] stay-on-top feature is not available with SDL older than "
          "2.16.0\n");
}
#endif

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
      // txp_renderer_config *config =
      //     txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      // config->background_color = convert_color(ps->ctx, stack, cmd.theme.bg);
      // config->foreground_color = convert_color(ps->ctx, stack, cmd.theme.fg);
      // config->themed_color = 1;
      // ps->schedule_event(UI_RENDER_EVENT);
    }
    break;
    case EDIT_PREVIOUS_PAGE:
      // FIXME: Todo
      //previous_page(ps, ui, 0);
      break;
    case EDIT_NEXT_PAGE:
      // FIXME: Todo
      //next_page(ps, ui, 0);
      break;
    case EDIT_MOVE_WINDOW:
    {
      float x = cmd.move_window.x, y = cmd.move_window.y, w = cmd.move_window.w,
            h = cmd.move_window.h;
      int x0 = x, y0 = y;
      SDL_SetWindowPosition(ui->window, x, y);
      SDL_GetWindowPosition(ui->window, &x0, &y0);
      SDL_SetWindowSize(ui->window, w + x - x0, h + y - y0);
    }
    break;
    case EDIT_MAP_WINDOW:
    {
      float x = cmd.move_window.x, y = cmd.move_window.y, w = cmd.move_window.w,
            h = cmd.move_window.h;
      int x0 = x, y0 = y;
      SDL_SetWindowBordered(ui->window, SDL_FALSE);
      SDL_SetWindowAlwaysOnTop(ui->window, SDL_TRUE);
      SDL_SetWindowPosition(ui->window, x, y);
      SDL_GetWindowPosition(ui->window, &x0, &y0);
      SDL_SetWindowSize(ui->window, w + x - x0, h + y - y0);
    }
    break;
    case EDIT_UNMAP_WINDOW:
    {
      if (!(SDL_GetWindowFlags(ui->window) & SDL_WINDOW_INPUT_FOCUS))
        SDL_SetWindowBordered(ui->window, SDL_TRUE);
      SDL_SetWindowAlwaysOnTop(ui->window, SDL_FALSE);
    }
    break;
    case EDIT_RESCAN:
      ps->schedule_event(UI_SCAN_EVENT);
      break;
    case EDIT_STAY_ON_TOP:
      SDL_SetWindowAlwaysOnTop(ui->window, cmd.stay_on_top.status);
      break;
    case EDIT_SYNCTEX_FORWARD:
    {
      fz_buffer *buf;
      TexSynctex *stx = send(synctex, ui->eng, &buf);
      int go_up = 0;
      const char *path =
          relative_path(cmd.synctex_forward.path, ps->doc_path, &go_up);
      if (go_up > 0)
      {
        fprintf(stderr,
                "[command] synctex-forward %s: file has a different root, "
                "skipping\n",
                path);
      }
      else
      {
        VisibleRange vr = viewer_get_visible_range(ps->ctx, &ui->viewer, &ps->pcoll);
        synctex_set_target(stx, (vr.first_page + vr.last_page) / 2, path, cmd.synctex_forward.line);
      }
    }
    break;
    case EDIT_CROP:
    {
      // txp_renderer_config *config =
      //     txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      // config->crop = !config->crop;
      ps->schedule_event(UI_RENDER_EVENT);
    }
    break;
    case EDIT_INVERT:
    {
      // txp_renderer_config *config =
      //     txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      // config->invert_color = !config->invert_color;
      ps->schedule_event(UI_RENDER_EVENT);
    }
    break;
  }
}

static bool poll_stdin(void)
{
  struct pollfd fd;
  fd.fd = STDIN_FILENO;
  fd.events = POLLRDNORM;
  fd.revents = 0;
  return (poll(&fd, 1, 0) == 1) && ((fd.revents & POLLRDNORM) != 0);
}

/* --- Main Event Handler --- */

struct stdin_state {
  vstack *cmd_stack;
  prot_parser cmd_parser;
  bool eof;
};

static void stdin_init(fz_context *ctx, struct stdin_state *st, enum editor_protocol protocol)
{
  st->cmd_stack = vstack_new(ctx);
  prot_initialize(&st->cmd_parser, protocol == EDITOR_JSON);
  st->eof = false;
}

static void stdin_cleanup(fz_context *ctx, struct stdin_state *st)
{
  vstack_free(ctx, st->cmd_stack);
}

static void stdin_process(struct stdin_state *st,
                          struct persistent_state *ps,
                          fd_poller *poller,
                          ui_state *ui)
{
  // Process stdin
  send(begin_changes, ui->eng, ps->ctx);
  char buffer[4096];
  int n = -1;
  while (!st->eof && poll_stdin() &&
         (n = read(STDIN_FILENO, buffer, 4096)) != 0)
  {
    if (n == -1)
    {
      if (errno == EINTR)
        continue;
      perror("poll stdin");
      break;
    }

    const char *ptr = buffer, *lim = buffer + n;
    fz_try(ps->ctx)
    {
      while ((ptr = prot_parse(ps->ctx, &st->cmd_parser, st->cmd_stack, ptr, lim)))
      {
        val cmds = vstack_get_values(ps->ctx, st->cmd_stack);
        int n_cmds = val_array_length(ps->ctx, st->cmd_stack, cmds);
        for (int i = 0; i < n_cmds; i++)
        {
          val cmd = val_array_get(ps->ctx, st->cmd_stack, cmds, i);
          interpret_command(ps, ui, st->cmd_stack, cmd);
        }
      }
    }
    fz_catch(ps->ctx)
    {
      fprintf(stderr, "error while reading stdin commands: %s\n",
              fz_caught_message(ps->ctx));
      vstack_reset(ps->ctx, st->cmd_stack);
      prot_reinitialize(&st->cmd_parser);
    }
  }
  if (n == 0)
    st->eof = true;
  if (!st->eof)
    fd_poller_watch(poller, STDIN_FILENO);
  if (send(end_changes, ui->eng, ps->ctx))
  {
    send(step, ui->eng, ps->ctx, true);
    ps->schedule_event(UI_RENDER_EVENT);
  }
}

/* UI rendering */

static bool prepare_rendering(struct persistent_state *ps,
                              ui_state *ui,
                              VisibleRange vr,
                              fz_irect prect)
{
  if (vr.first_page < 0)
    return false;

  bool result = false;
  for (int i = vr.first_page; i <= vr.last_page; i++)
  {
    fz_irect window_rect =
        viewer_get_page_screen_rect(ps->ctx, &ui->viewer, &ps->pcoll, i);
    fz_irect buffer_rect =
        viewer_get_page_buffer_rect(ps->ctx, &ui->viewer, &ps->pcoll, i);
    fz_irect visible_rect = fz_relative_clipped_area(window_rect, prect);
    if (pagebuffer_update_entry_from_display_list(
        ps->ctx, &ui->pbuff, i, buffer_rect, visible_rect,
        pagecollection_get(&ps->pcoll, i), NULL, NULL))
      result = true;
  }
  return result;
}

static void render_pages(struct persistent_state *ps,
                         ui_state *ui,
                         VisibleRange vr,
                         fz_irect prect)
{
  if (vr.first_page < 0)
    return;

  for (int i = vr.first_page; i <= vr.last_page; i++)
  {
    fz_irect window_rect =
      viewer_get_page_screen_rect(ps->ctx, &ui->viewer, &ps->pcoll, i);
    fz_irect buffer_rect =
      viewer_get_page_buffer_rect(ps->ctx, &ui->viewer, &ps->pcoll, i);
    fz_irect visible_rect =
        fz_relative_clipped_area(window_rect, prect);

    int x = window_rect.x0 + visible_rect.x0;
    int y = window_rect.y0 + visible_rect.y0;
    visible_rect.x0 += buffer_rect.x0;
    visible_rect.y0 += buffer_rect.y0;
    visible_rect.x1 += buffer_rect.x0;
    visible_rect.y1 += buffer_rect.y0;
    pagebuffer_blit(&ui->pbuff, x, y, visible_rect);
  }
}

static bool render(struct persistent_state *ps, ui_state *ui, bool need)
{
  int pw, ph;
  SDL_GetRendererOutputSize(ps->renderer, &pw, &ph);
  pagebuffer_reserve(&ui->pbuff, pw, ph);
  fz_irect prect = fz_make_irect(0, 0, pw, ph);
  VisibleRange vr = viewer_get_visible_range(ps->ctx, &ui->viewer, &ps->pcoll);
  //printf("first_page:%d last_page:%d\n", vr.first_page, vr.last_page);

  if (prepare_rendering(ps, ui, vr, prect))
    need = true;

  if (need)
  {
    SDL_SetRenderDrawColor(ps->renderer, 96, 96, 96, 255);
    SDL_RenderClear(ps->renderer);
    render_pages(ps, ui, vr, prect);
    viewer_draw_scrollbar(&ui->viewer, ps->renderer);
    SDL_RenderPresent(ps->renderer);
  }
  return need;
}

/* --- Entry Point --- */

bool texpresso_main(struct persistent_state *ps)
{
  editor_set_protocol(ps->protocol);
  editor_set_line_output(ps->line_output);

  ui_state raw_ui, *ui = &raw_ui;
  ui->window = ps->window;
  viewer_init(&ui->viewer);

  bool using_texlive = false;
  if (ps->use_texlive)
  {
    if (!texlive_available())
    {
      fprintf(
          stderr,
          "[fatal] cannot find kpsewhich command for texlive integration\n");
      return false;
    }
    using_texlive = true;
  }
  else if (ps->use_tectonic)
  {
    if (!tectonic_available())
    {
      fprintf(stderr, "[fatal] cannot find tectonic\n");
      return false;
    }
  }
  else if (!(using_texlive = texlive_available()) && !tectonic_available())
  {
    fprintf(stderr, "[fatal] cannot find tectonic nor kpsewhich (texlive)\n");
    return false;
  }

  const char *doc_ext = NULL;
  for (const char *ptr = ps->doc_name; *ptr; ptr++)
    if (*ptr == '.')
      doc_ext = ptr + 1;

  char engine_path[PATH_BUFFER_SIZE];
  find_engine(engine_path, ps->exe_path);
  fprintf(stderr, "[info] engine path: %s\n", engine_path);

  if (doc_ext && strcmp(doc_ext, "pdf") == 0)
    ui->eng = create_pdf_engine(ps->ctx, ps->doc_name);
  else
  {
     dvi_resloader loader;
     if (using_texlive)
       loader = dvi_texlive_loader(ps->ctx, ps->doc_path);
     else
       loader = dvi_tectonic_loader(ps->ctx, ps->doc_path);

     if (doc_ext && (strcmp(doc_ext, "dvi") == 0 || strcmp(doc_ext, "xdv") == 0))
       ui->eng = create_dvi_engine(ps->ctx, ps->doc_name, loader);
     else
       ui->eng = create_tex_engine(ps->ctx, engine_path, using_texlive,
                                   ps->stream_mode, ps->inclusion_path,
                                   ps->doc_name, loader);
   }

  ui->sdl_renderer = ps->renderer;
  pagebuffer_init(&ui->pbuff, ui->sdl_renderer);
  pagecollection_invalidate_after(&ps->pcoll, 0);

  if (ps->initial.initialized)
  {
    SDL_FlushEvents(ps->custom_events, ps->custom_events + CUSTOM_EVENT_COUNT - 1);
    editor_reset_sync();
  }

  ui->mouse_status = UI_MOUSE_NONE;
  ui->last_mouse_x = -1000;
  ui->last_mouse_y = -1000;

  bool hotload = false;
  fz_context *ctx = ps->ctx;

  send(step, ui->eng, ctx, true);
  render(ps, ui, true);
  ps->schedule_event(UI_RENDER_EVENT);

  struct repaint_on_resize_env repaint_env = {.ps = ps, .ui = ui};
  SDL_AddEventWatch(repaint_on_resize, &repaint_env);

  fd_poller *poller = fd_poller_new(ps->custom_events + CUSTOM_EVENT_FD);
  fd_poller_watch(poller, STDIN_FILENO);

  struct stdin_state stdin_st;
  stdin_init(ctx, &stdin_st, ps->protocol);

  Uint64 last_time = SDL_GetPerformanceCounter();
  Uint64 freq = SDL_GetPerformanceFrequency();

  while (1)
  {
    bool running = true, rerender = false, advance = false;

    // Process events
    for (SDL_Event e; SDL_PollEvent(&e); )
    {
      // Delegate Input to Engine
      viewer_handle_event(ctx, &ui->viewer, &ps->pcoll, &e, ps->window);

      if (e.type == ps->custom_events + CUSTOM_EVENT_UI)
      {
        *(char *)e.user.data1 = 0;
        switch (e.user.code)
        {
          case UI_SCAN_EVENT:
            if (ps->should_hotload_binary())
            {
              running = false;
              hotload = true;
              continue;
            }
            send(begin_changes, ui->eng, ctx);
            flush_changes(ps, ui);
            send(detect_changes, ui->eng, ctx);
            if (send(end_changes, ui->eng, ctx))
            {
              send(step, ui->eng, ctx, true);
              rerender = true;
            }
            break;
          case UI_STEP_EVENT:
            break;
        }
      }
      if (e.type == ps->custom_events + CUSTOM_EVENT_FD)
      {
        if (e.user.code == STDIN_FILENO)
          stdin_process(&stdin_st, ps, poller, ui);
      }
      if (e.type == SDL_QUIT ||
          e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q)
        running = false;
      if (e.type == SDL_WINDOWEVENT)
        rerender = true;
    }

    if (!running)
      break;

    // Update document
    {
      pagecollection_invalidate_after(&ps->pcoll, send(page_count, ui->eng));
      advance = advance_engine(ps, ui);
      VisibleRange vr = viewer_get_visible_range(ps->ctx, &ui->viewer, &ps->pcoll);

      int before = pagecollection_valid_count(&ps->pcoll);
      int after = send(page_count, ui->eng);
      for (int i = before; i < after; i++)
      {
        fz_display_list *dl = send(render_page, ui->eng, ps->ctx, i);
        pagecollection_set(ctx, &ps->pcoll, i, dl);
        pagebuffer_invalidate_page(&ui->pbuff, i);
        fz_drop_display_list(ps->ctx, dl);
      }
      if (send(get_status, ui->eng) == DOC_TERMINATED)
        pagecollection_truncate(ctx, &ps->pcoll);
      fflush(stdout);

      if (vr.first_page <= after && before <= vr.last_page)
        rerender = true;
    }

    // Update physics

    Uint64 now = SDL_GetPerformanceCounter();
    float dt = (float)(now - last_time) / freq;
    last_time = now;
    if (dt > 0.1f) dt = 0.1f;  // Clamp delta time

    int win_w, win_h;
    SDL_GetWindowSize(ps->window, &win_w, &win_h);

    viewer_update(&ui->viewer, &ps->pcoll, dt, win_w, win_h);
    rerender = viewer_need_rerender(ctx, &ui->viewer, &ps->pcoll) || rerender;

    // Check synctex target
    fz_buffer *buf;
    TexSynctex *stx = send(synctex, ui->eng, &buf);
    int page = -1, x = -1, y = -1;
    if (synctex_find_target(ctx, stx, buf, &page, &x, &y))
    {
      fprintf(stderr,
              "[synctex forward] sync: hit page %d, coordinates (%d, %d)\n",
              page, x, y);
      float f = send(scale_factor, ui->eng);
      DocCoord coord = {.page_index = page, .x = f * x, .y = f * y};
      viewer_scroll_to_doc_coord(ctx, &ui->viewer, &ps->pcoll, coord, VIEWER_SCROLL_IF_NOT_CENTERED, 0.2);
    }

    rerender = render(ps, ui, rerender);

    // Wait for next event
    // - Idle: wait indefinitely for any event
    // - Animating: wait with timeout to continue animation
    if (!advance)
    {
      if (viewer_is_idle(&ui->viewer))
        SDL_WaitEvent(NULL);
      else if (!rerender)
        SDL_WaitEventTimeout(NULL, 10);
    }

    if (ps->initialize_only)
    {
      fprintf(stderr, "[info] Initialize mode: terminating engine process\n");
      running = false;
    }

  }

  // Cleanup
  stdin_cleanup(ctx, &stdin_st);
  fd_poller_free(poller);
  SDL_DelEventWatch(repaint_on_resize, &repaint_env);

  ps->initial.initialized = true;
  // FIXME: persist viewer state?

  send(destroy, ui->eng, ctx);
  pagebuffer_finalize(&ui->pbuff);

  return hotload;
}
