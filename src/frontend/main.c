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
#include "prot_parser.h"
#include "providers.h"
#include "renderer.h"
#include "synctex.h"
#include "utf_mapping.h"
#include "vstack.h"

/* --- Configuration Constants --- */

#define ENGINE_STEP_LIMIT 10
#define ENGINE_TIMEOUT_US 5000000
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
  txp_engine *eng;
  txp_renderer *doc_renderer;
  SDL_Renderer *sdl_renderer;
  SDL_Window *window;
  fd_poller *fdpoll;

  int page;
  int zoom;

  // Mouse input state
  int last_mouse_x, last_mouse_y;
  enum ui_mouse_status mouse_status;
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

static void render(fz_context *ctx, ui_state *ui)
{
  SDL_SetRenderDrawColor(ui->sdl_renderer, 0, 0, 0, 255);
  SDL_RenderClear(ui->sdl_renderer);
  txp_renderer_render(ctx, ui->doc_renderer);
  SDL_RenderPresent(ui->sdl_renderer);
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

/* --- Engine & Document Logic --- */

// Always compute SyncTeX for now
static bool need_synctex(void)
{
  return true;
}

static bool need_advance(fz_context *ctx, ui_state *ui)
{
  int need = send(page_count, ui->eng) <= ui->page;

  if (!need)
  {
    fz_buffer *buf;
    synctex_t *stx = send(synctex, ui->eng, &buf);
    need = (need_synctex() && synctex_page_count(stx) <= ui->page) ||
           synctex_has_target(stx);
  }

  return (need && send(get_status, ui->eng) == DOC_RUNNING);
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

  int steps = ENGINE_STEP_LIMIT;
  while (need)
  {
    if (!send(step, ui->eng, ctx, false))
      break;

    steps -= 1;
    need = need_advance(ctx, ui);

    if (steps == 0)
    {
      steps = ENGINE_STEP_LIMIT;
      struct timespec curr;
      clock_gettime(CLOCK_MONOTONIC, &curr);

      int delta = (curr.tv_sec - start.tv_sec) * 1000 * 1000 * 1000 +
                  (curr.tv_nsec - start.tv_nsec);

      if (delta > ENGINE_TIMEOUT_US)
        break;
    }
  }
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

    if (clicks == 1)
    {
      diff = txp_renderer_start_selection(ps->ctx, ui->doc_renderer, p);
      diff = txp_renderer_select_char(ps->ctx, ui->doc_renderer, p) || diff;

      fz_buffer *buf;
      synctex_t *stx = send(synctex, ui->eng, &buf);
      if (stx && buf)
      {
        fz_point pt =
            txp_renderer_screen_to_document(ps->ctx, ui->doc_renderer, p);
        float f = 1 / send(scale_factor, ui->eng);
        synctex_scan(ps->ctx, stx, buf, ps->doc_path, ui->page, f * pt.x,
                     f * pt.y);
      }
    }
    else if (clicks == 2)
    {
      diff = txp_renderer_select_word(ps->ctx, ui->doc_renderer, p);
    }

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
      fz_point p = fz_make_point(scale.x * x, scale.y * y);
      if (txp_renderer_drag_selection(ps->ctx, ui->doc_renderer, p))
        needs_render = true;
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

  txp_renderer_config *config =
      txp_renderer_get_config(ctx, ui->doc_renderer);
  config->pan.x += scale_x * dx;
  config->pan.y += scale_y * dy;
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

  txp_renderer_config *config = txp_renderer_get_config(ps->ctx, ui->doc_renderer);
  bool needs_render = false;
  fz_point scale = get_scale_factor(ui->window);

  if (ctrl)
  {
    SDL_FRect rect;
    if (dy != 0 &&
        txp_renderer_page_position(ps->ctx, ui->doc_renderer, &rect, NULL, NULL))
    {
      ui->zoom = fz_maxi(ui->zoom + dy * 100, 0);
      int ww, wh;
      SDL_GetWindowSize(ui->window, &ww, &wh);

      float mx = (mousex - ww / 2.0f) * scale.x;
      float my = (mousey - wh / 2.0f) * scale.y;

      float zf = zoom_factor(ui->zoom);
      float effective_zoom = zf / config->zoom;

      config->pan.x = mx + effective_zoom * (config->pan.x - mx);
      config->pan.y = my + effective_zoom * (config->pan.y - my);
      config->zoom = zf;
      needs_render = true;
    }
  }
  else
  {
    float x = scale.x * dx * 5, y = scale.y * dy * 5;
    if (update_pan(ps->ctx, ui, (int)(x * 100), (int)(y * 100), 0.01f, 0.01f))
    {
      // Note: update_pan uses scale, here we pre-calculated.
      // Simplified: just update config directly for wheel
      config->pan.x -= x;
      config->pan.y += y;
      needs_render = true;
    }
  }

  if (needs_render)
    ps->schedule_event(UI_RENDER_EVENT);
}

/* --- Path Utilities --- */

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

static int find_diff(const fz_buffer *buf, const void *data, int size)
{
  const unsigned char *ptr = data;
  int i, len = fz_mini(buf->len, size);
  for (i = 0; i < len && buf->data[i] == ptr[i]; ++i)
    ;
  return i;
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

  if ((page_count == ui->page - 2 || page_count == ui->page - 1) &&
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

  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
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
  send(notify_file_changes, ui->eng, ps->ctx, e, changed);
}

static uint32_t convert_color(fz_context *ctx, vstack *stack, float frgb[3])
{
  uint8_t rgb[3];
  for (int i = 0; i < 3; ++i)
    rgb[i] = fz_clampi(frgb[i] * 255.0, 0, 255) & 0xFF;
  return (rgb[0] << 16) | (rgb[1] << 8) | (rgb[2]);
}

static void display_page(struct persistent_state *ps, ui_state *ui)
{
  fz_display_list *dl = send(render_page, ui->eng, ps->ctx, ui->page);
  txp_renderer_set_contents(ps->ctx, ui->doc_renderer, dl);
  fz_drop_display_list(ps->ctx, dl);
  ps->schedule_event(UI_RENDER_EVENT);
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

enum pan_to { PAN_TO_TOP, PAN_TO_BOTTOM };

static void pan_to(fz_context *ctx, ui_state *ui, enum pan_to to)
{
  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);
  txp_renderer_bounds bounds;
  if (txp_renderer_page_bounds(ctx, ui->doc_renderer, &bounds))
    config->pan.y =
        (to == PAN_TO_TOP) ? bounds.pan_interval.y : -bounds.pan_interval.y;
}

static void previous_page(struct persistent_state *ps, ui_state *ui, bool pan)
{
  synctex_set_target(send(synctex, ui->eng, NULL), 0, NULL, 0);
  if (ui->page > 0)
  {
    ui->page -= 1;
    int page_count = send(page_count, ui->eng);
    if (page_count > 0 && ui->page >= page_count &&
        send(get_status, ui->eng) == DOC_TERMINATED)
      ui->page = page_count - 1;

    if (pan)
      pan_to(ps->ctx, ui, PAN_TO_BOTTOM);

    ps->schedule_event(UI_RELOAD_EVENT);
  }
}

static void next_page(struct persistent_state *ps, ui_state *ui, bool pan)
{
  synctex_set_target(send(synctex, ui->eng, NULL), 0, NULL, 0);
  ui->page += 1;
  if (pan)
    pan_to(ps->ctx, ui, PAN_TO_TOP);
  ps->schedule_event(UI_RELOAD_EVENT);
}

static void ui_pan(struct persistent_state *ps, ui_state *ui, float factor)
{
  fz_point scale = get_scale_factor(ui->window);
  txp_renderer_config *config = txp_renderer_get_config(ps->ctx, ui->doc_renderer);
  txp_renderer_bounds bounds;

  if (!txp_renderer_page_bounds(ps->ctx, ui->doc_renderer, &bounds))
    return;

  float delta = bounds.window_size.y * scale.y * factor;
  float range = bounds.pan_interval.y < 0 ? 0 : bounds.pan_interval.y;

  if (config->pan.y == -range && factor < 0)
  {
    next_page(ps, ui, 1);
    return;
  }
  if (config->pan.y == range && factor > 0)
  {
    previous_page(ps, ui, 1);
    return;
  }

  config->pan.y += delta;
  ps->schedule_event(UI_RENDER_EVENT);
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
      txp_renderer_config *config =
          txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      config->background_color = convert_color(ps->ctx, stack, cmd.theme.bg);
      config->foreground_color = convert_color(ps->ctx, stack, cmd.theme.fg);
      config->themed_color = 1;
      ps->schedule_event(UI_RENDER_EVENT);
    }
    break;
    case EDIT_PREVIOUS_PAGE:
      previous_page(ps, ui, 0);
      break;
    case EDIT_NEXT_PAGE:
      next_page(ps, ui, 0);
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
      synctex_t *stx = send(synctex, ui->eng, &buf);
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
        synctex_set_target(stx, ui->page, path, cmd.synctex_forward.line);
        ps->schedule_event(UI_STDIN_EVENT);
      }
    }
    break;
    case EDIT_CROP:
    {
      txp_renderer_config *config =
          txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      config->crop = !config->crop;
      ps->schedule_event(UI_RENDER_EVENT);
    }
    break;
    case EDIT_INVERT:
    {
      txp_renderer_config *config =
          txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      config->invert_color = !config->invert_color;
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

static void handle_sdl_event(SDL_Event *e,
                             ui_state *ui,
                             struct persistent_state *ps)
{
  fz_context *ctx = ps->ctx;
  switch (e->type)
  {
    case SDL_KEYDOWN:
      switch (e->key.keysym.sym)
      {
        case SDLK_LEFT:
        case SDLK_PAGEUP:
          previous_page(ps, ui, 0);
          break;
        case SDLK_UP:
          ui_pan(ps, ui, 2.0f / 3.0f);
          break;
        case SDLK_DOWN:
          ui_pan(ps, ui, -2.0f / 3.0f);
          break;
        case SDLK_RIGHT:
        case SDLK_PAGEDOWN:
          next_page(ps, ui, 0);
          break;
        case SDLK_p:
        {
          txp_renderer_config *config =
              txp_renderer_get_config(ctx, ui->doc_renderer);
          config->fit = (config->fit == FIT_PAGE) ? FIT_WIDTH : FIT_PAGE;
          ps->schedule_event(UI_RENDER_EVENT);
        }
        break;
        case SDLK_b:
          SDL_SetWindowBordered(ui->window, !!(SDL_GetWindowFlags(ui->window) &
                                               SDL_WINDOW_BORDERLESS));
          break;
        case SDLK_t:
          SDL_SetWindowAlwaysOnTop(
              ui->window,
              !(SDL_GetWindowFlags(ui->window) & SDL_WINDOW_ALWAYS_ON_TOP));
          break;
        case SDLK_c:
        {
          txp_renderer_config *config =
              txp_renderer_get_config(ctx, ui->doc_renderer);
          config->crop = !config->crop;
          ps->schedule_event(UI_RENDER_EVENT);
        }
        break;
        case SDLK_i:
        {
          txp_renderer_config *config =
              txp_renderer_get_config(ctx, ui->doc_renderer);
          if ((SDL_GetModState() & KMOD_SHIFT))
            config->themed_color = !config->themed_color;
          else
            config->invert_color = !config->invert_color;
          ps->schedule_event(UI_RENDER_EVENT);
        }
        break;
        case SDLK_ESCAPE:
          SDL_SetWindowFullscreen(ui->window, 0);
          break;
        case SDLK_F5:
          SDL_SetWindowFullscreen(ui->window, SDL_WINDOW_FULLSCREEN_DESKTOP);
          {
            txp_renderer_config *config =
                txp_renderer_get_config(ctx, ui->doc_renderer);
            config->fit = FIT_PAGE;
            ps->schedule_event(UI_RENDER_EVENT);
          }
          break;
      }
      break;
    case SDL_MOUSEWHEEL:
    {
      int mx = 0, my = 0;
      float px = 0, py = 0;
#if SDL_VERSION_ATLEAST(2, 0, 260)
      mx = e->wheel.mouseX;
      my = e->wheel.mouseY;
#else
      SDL_GetMouseState(&mx, &my);
#endif
#if SDL_VERSION_ATLEAST(2, 0, 18)
      px = e->wheel.preciseX;
      py = e->wheel.preciseY;
#else
      px = e->wheel.x;
      py = e->wheel.y;
#endif
      bool ctrl = !!(SDL_GetModState() & KMOD_CTRL);
      ui_mouse_wheel(ps, ui, px, py, mx, my, ctrl);
    }
    break;
    case SDL_MOUSEBUTTONDOWN:
      ui_mouse_down(ps, ui, e->button.x, e->button.y, e->button.clicks,
                    SDL_GetModState() & KMOD_CTRL);
      break;
    case SDL_MOUSEBUTTONUP:
      ui_mouse_up(ui);
      break;
    case SDL_MOUSEMOTION:
      ui_mouse_move(ps, ui, e->motion.x, e->motion.y);
      break;
    case SDL_WINDOWEVENT:
      switch (e->window.event)
      {
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_EXPOSED:
          ps->schedule_event(UI_RENDER_EVENT);
          break;
      }
      break;
  }
}

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
    ps->schedule_event(UI_RELOAD_EVENT);
  }
}

/* --- Entry Point --- */

bool texpresso_main(struct persistent_state *ps)
{
  editor_set_protocol(ps->protocol);
  editor_set_line_output(ps->line_output);

  ui_state raw_ui, *ui = &raw_ui;
  ui->window = ps->window;

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
    ui->eng = txp_create_pdf_engine(ps->ctx, ps->doc_name);
  else
  {
    dvi_reshooks hooks;
    if (using_texlive)
      hooks = dvi_texlive_hooks(ps->ctx, ps->doc_path);
    else
      hooks = dvi_tectonic_hooks(ps->ctx, ps->doc_path);

    if (doc_ext && (strcmp(doc_ext, "dvi") == 0 || strcmp(doc_ext, "xdv") == 0))
      ui->eng = txp_create_dvi_engine(ps->ctx, ps->doc_name, hooks);
    else
      ui->eng = txp_create_tex_engine(ps->ctx, engine_path, using_texlive,
                                      ps->stream_mode, ps->inclusion_path,
                                      ps->doc_name, hooks);
  }

  ui->sdl_renderer = ps->renderer;
  ui->doc_renderer = txp_renderer_new(ps->ctx, ui->sdl_renderer);

  if (ps->initial.initialized)
  {
    SDL_FlushEvents(ps->custom_events, ps->custom_events + CUSTOM_EVENT_COUNT - 1);
    ui->page = ps->initial.page;
    ui->zoom = ps->initial.zoom;
    *txp_renderer_get_config(ps->ctx, ui->doc_renderer) = ps->initial.config;
    txp_renderer_set_contents(ps->ctx, ui->doc_renderer,
                              ps->initial.display_list);
    editor_reset_sync();
  }
  else
  {
    ui->page = 0;
    ui->zoom = 0;
  }

  ui->mouse_status = UI_MOUSE_NONE;
  ui->last_mouse_x = -1000;
  ui->last_mouse_y = -1000;

  bool quit = false, hotload = false;
  fz_context *ctx = ps->ctx;

  send(step, ui->eng, ctx, true);
  render(ctx, ui);
  ps->schedule_event(UI_RELOAD_EVENT);

  struct repaint_on_resize_env repaint_env = {.ctx = ctx, .ui = ui};
  SDL_AddEventWatch(repaint_on_resize, &repaint_env);

  fd_poller *poller = fd_poller_new(ps->custom_events + CUSTOM_EVENT_FD);
  fd_poller_watch(poller, STDIN_FILENO);

  struct stdin_state stdin_st;
  stdin_init(ctx, &stdin_st, ps->protocol);

  while (!quit)
  {
    SDL_Event e;
    bool has_event = SDL_PollEvent(&e);

    // Process document
    {
      int before_page_count = send(page_count, ui->eng);
      bool advance = advance_engine(ctx, ui);
      int after_page_count = send(page_count, ui->eng);
      fflush(stdout);

      if (ui->page >= before_page_count && ui->page < after_page_count)
        ps->schedule_event(UI_RELOAD_EVENT);

      if (!has_event)
      {
        if (advance)
          continue;
        has_event = SDL_WaitEvent(&e);
        if (!has_event)
        {
          fprintf(stderr, "SDL_WaitEvent error: %s\n", SDL_GetError());
          break;
        }
      }

      fz_buffer *buf;
      synctex_t *stx = send(synctex, ui->eng, &buf);
      int page = -1, x = -1, y = -1;
      if (synctex_find_target(ctx, stx, buf, &page, &x, &y))
      {
        fprintf(stderr, "[synctex forward] sync: hit page %d, coordinates (%d, %d)\n",
                page, x, y);

        if (page != ui->page)
        {
          ui->page = page;
          display_page(ps, ui);
        }

        float f = send(scale_factor, ui->eng);
        fz_point p = fz_make_point(f * x, f * y);
        fz_point pt = txp_renderer_document_to_screen(ctx, ui->doc_renderer, p);
        fprintf(stderr, "[synctex forward] position on screen: (%.02f, %.02f)\n",
                pt.x, pt.y);
        int w, h;
        txp_renderer_screen_size(ctx, ui->doc_renderer, &w, &h);
        float margin_lo = h / 4.0f;
        float margin_hi = h / 3.0f;

        txp_renderer_config *config =
            txp_renderer_get_config(ctx, ui->doc_renderer);
        float delta = 0.0f;
        if (pt.y < margin_lo)
          delta = -pt.y + margin_hi;
        else if (pt.y >= h - margin_lo)
          delta = h - pt.y - margin_hi;
        fprintf(stderr, "[synctex forward] pan.y = %.02f + %.02f = %.02f\n",
                config->pan.y, delta, config->pan.y + delta);
        config->pan.y += delta;
        if (delta != 0.0f)
          ps->schedule_event(UI_RENDER_EVENT);
      }
    }

    txp_renderer_set_scale_factor(ctx, ui->doc_renderer,
                                  get_scale_factor(ui->window));

    // Process Event
    if (e.type == ps->custom_events + CUSTOM_EVENT_UI)
    {
      *(char *)e.user.data1 = 0;
      switch (e.user.code)
      {
        case UI_SCAN_EVENT:
          if (ps->should_hotload_binary())
          {
            quit = hotload = true;
            continue;
          }
          send(begin_changes, ui->eng, ctx);
          flush_changes(ps, ui);
          send(detect_changes, ui->eng, ctx);
          if (send(end_changes, ui->eng, ctx))
          {
            send(step, ui->eng, ctx, true);
            ps->schedule_event(UI_RELOAD_EVENT);
          }
          break;
        case UI_RENDER_EVENT:
          render(ctx, ui);
          send(begin_changes, ui->eng, ctx);
          flush_changes(ps, ui);
          if (send(end_changes, ui->eng, ctx))
          {
            send(step, ui->eng, ctx, true);
            ps->schedule_event(UI_RELOAD_EVENT);
          }
          break;
        case UI_RELOAD_EVENT:
        {
          int page_count = send(page_count, ui->eng);
          if (ui->page >= page_count &&
              send(get_status, ui->eng) == DOC_TERMINATED)
          {
            if (page_count > 0)
              ui->page = page_count - 1;
          }
          if (ui->page < page_count)
            display_page(ps, ui);
        }
        break;
        case UI_STDIN_EVENT:
          break;
      }
    }
    else if (e.type == ps->custom_events + CUSTOM_EVENT_FD)
    {
      if (e.user.code == STDIN_FILENO)
        stdin_process(&stdin_st, ps, poller, ui);
    }
    else
    {
      handle_sdl_event(&e, ui, ps);
      if (e.type == SDL_QUIT ||
          e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q)
        quit = true;
    }

    if (ps->initialize_only)
    {
      fprintf(stderr, "[info] Initialize mode: terminating engine process\n");
      quit = true;
    }
  }

  // Cleanup
  stdin_cleanup(ctx, &stdin_st);
  fd_poller_free(poller);
  SDL_DelEventWatch(repaint_on_resize, &repaint_env);

  if (ps->initial.initialized && ps->initial.display_list)
    fz_drop_display_list(ctx, ps->initial.display_list);

  ps->initial.initialized = true;
  ps->initial.page = ui->page;
  ps->initial.zoom = ui->zoom;
  ps->initial.config = *txp_renderer_get_config(ctx, ui->doc_renderer);
  ps->initial.display_list = txp_renderer_get_contents(ctx, ui->doc_renderer);
  if (ps->initial.display_list)
    fz_keep_display_list(ctx, ps->initial.display_list);

  txp_renderer_free(ctx, ui->doc_renderer);
  send(destroy, ui->eng, ctx);

  return hotload;
}
