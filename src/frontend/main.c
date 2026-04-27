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
#include "renderer.h"
#include "engine.h"
#include "driver.h"
#include "synctex.h"
#include "vstack.h"
#include "prot_parser.h"
#include "editor.h"
#include "base64.h"
#include "qoi.h"

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
};

typedef struct {
  txp_engine *eng;
  txp_renderer *doc_renderer;
  SDL_Renderer *sdl_renderer;
  SDL_Window *window;

  int page;
  int need_synctex;
  int zoom;

  // Mouse input state
  int last_mouse_x, last_mouse_y;
  uint32_t last_click_ticks;
  enum ui_mouse_status mouse_status;
  bool advancing;
} ui_state;

/* UI rendering */

static float zoom_factor(int count)
{
  return expf((float)count / 5000.0f);
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

/* Document processing */

static bool need_advance(fz_context *ctx, ui_state *ui)
{
  int need = send(page_count, ui->eng) <= ui->page;

  if (!need)
  {
    fz_buffer *buf;
    synctex_t *stx = send(synctex, ui->eng, &buf);
    need =
      (ui->need_synctex && synctex_page_count(stx) <= ui->page) ||
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

  int steps = 10;
  while (need)
  {
    if (!send(step, ui->eng, ctx, false))
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

/* UI events */

static void ui_mouse_down(struct persistent_state *ps, ui_state *ui, int x, int y, bool ctrl)
{
  if (ctrl)
    ui->mouse_status = UI_MOUSE_MOVE;
  else
  {
    ui->mouse_status = UI_MOUSE_SELECT;
    fz_point scale = get_scale_factor(ui->window);
    fz_point p = fz_make_point(scale.x * x, scale.y * y);

    uint32_t ticks = SDL_GetTicks();

    bool double_click = ticks - ui->last_click_ticks < 500 &&
                        abs(ui->last_mouse_x - x) < 30 && abs(ui->last_mouse_y - y) < 30;

    bool diff;

    if (double_click)
    {
      diff = txp_renderer_select_word(ps->ctx, ui->doc_renderer, p);
    }
    else
    {
      diff = txp_renderer_start_selection(ps->ctx, ui->doc_renderer, p);
      diff = txp_renderer_select_char(ps->ctx, ui->doc_renderer, p) || diff;
      ui->last_click_ticks = ticks;

      fz_buffer *buf;
      synctex_t *stx = send(synctex, ui->eng, &buf);
      if (stx && buf)
      {
        fz_point pt = txp_renderer_screen_to_document(ps->ctx, ui->doc_renderer, p);
        float f = 1 / send(scale_factor, ui->eng);
        // pt.x -= 72;
        // pt.y -= 72;
        fprintf(stderr, "click: (%f,%f) mapped:(%f,%f)\n",
                pt.x, pt.y, f * pt.x, f * pt.y);
        synctex_scan(ps->ctx, stx, buf, ps->doc_path, ui->page, f * pt.x, f * pt.y);
      }
    }

    if (diff)
      schedule_event(RENDER_EVENT);
  }

  ui->last_mouse_x = x;
  ui->last_mouse_y = y;
}

static void ui_mouse_up(ui_state *ui)
{
  ui->mouse_status = UI_MOUSE_NONE;
}

static void ui_mouse_move(fz_context *ctx, ui_state *ui, int x, int y)
{
  fz_point scale = get_scale_factor(ui->window);
  switch (ui->mouse_status)
  {
    case UI_MOUSE_NONE:
      break;

    case UI_MOUSE_SELECT:
    {
      fz_point p = fz_make_point(scale.x * x, scale.y * y);
      // fprintf(stderr, "drag sel\n");
      if (txp_renderer_drag_selection(ctx, ui->doc_renderer, p))
        schedule_event(RENDER_EVENT);
      break;
    }

    case UI_MOUSE_MOVE:
    {
      txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);
      int dx = x - ui->last_mouse_x;
      int dy = y - ui->last_mouse_y;
      if (dx != 0 || dy != 0)
      {
        config->pan.x += scale.x * dx;
        config->pan.y += scale.y * dy;
        ui->last_mouse_x = x;
        ui->last_mouse_y = y;
        schedule_event(RENDER_EVENT);
      }
      break;
    }
  }
}

static void ui_mouse_wheel(fz_context *ctx, ui_state *ui, float dx, float dy, int mousex, int mousey, bool ctrl, int timestamp)
{
  fz_point scale = get_scale_factor(ui->window);

  if (ui->mouse_status != UI_MOUSE_NONE)
    return;

  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);

  if (ctrl)
  {
    SDL_FRect rect;
    if (dy != 0 && txp_renderer_page_position(ctx, ui->doc_renderer, &rect, NULL, NULL))
    {
      ui->zoom = fz_maxi(ui->zoom + dy * 100, 0);
      int ww, wh;
      SDL_GetWindowSize(ui->window, &ww, &wh);
      float mx = (mousex - ww / 2.0f) * scale.x;
      float my = (mousey - wh / 2.0f) * scale.y;
      float of = config->zoom, nf = zoom_factor(ui->zoom);
      config->pan.x = mx + nf * ((config->pan.x - mx) / of);
      config->pan.y = my + nf * ((config->pan.y - my) / of);
      config->zoom = nf;
      schedule_event(RENDER_EVENT);
    }
  }
  else
  {
    (void)timestamp;
    float x = scale.x * dx * 5;
    float y = scale.y * dy * 5;
    config->pan.x -= x;
    config->pan.y += y;
    // fprintf(stderr, "wheel pan: (%.02f, %.02f) raw:(%.02f, %.02f)\n", x, y, dx, dy);
    schedule_event(RENDER_EVENT);
  }
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
      if ((fds[0].revents & POLLRDNORM) != 0)
        schedule_event(STDIN_EVENT);
      break;
    }
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

/* Command interpreter */

enum pan_to { PAN_TO_TOP, PAN_TO_BOTTOM };
static void pan_to(fz_context *ctx, ui_state *ui, enum pan_to to)
{
  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);
  txp_renderer_bounds bounds;
  if (txp_renderer_page_bounds(ctx, ui->doc_renderer, &bounds))
    config->pan.y = (to == PAN_TO_TOP) ? bounds.pan_interval.y : -bounds.pan_interval.y;
  // a helper function for other UI actions, so no event scheduled
}

static void previous_page(fz_context *ctx, ui_state *ui, bool pan)
{
  synctex_set_target(send(synctex, ui->eng, NULL), 0, NULL, 0);
  if (ui->page > 0)
  {
    ui->page -= 1;

    int page_count = send(page_count, ui->eng);
    if (page_count > 0 && ui->page >= page_count &&
        send(get_status, ui->eng) == DOC_TERMINATED)
      ui->page = page_count - 1;

    // FIXME: technically, this is slightly incorrect.
    // The new page has not been loaded yet, so we compute the coordinate with
    // respect to the page currently displayed. Most of the time, pages have the
    // same dimension, so this is fine.
    if (pan)
      pan_to(ctx, ui, PAN_TO_BOTTOM);

    schedule_event(RELOAD_EVENT);
  }
}

static void next_page(fz_context *ctx, ui_state *ui, bool pan)
{
  synctex_set_target(send(synctex, ui->eng, NULL), 0, NULL, 0);
  ui->page += 1;
  // FIXME: Same remark as in previous_page.
  if (pan)
    pan_to(ctx, ui, PAN_TO_TOP);
  schedule_event(RELOAD_EVENT);
}

static void ui_pan(fz_context *ctx, ui_state *ui, float factor)
{
  fz_point scale = get_scale_factor(ui->window);

  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);

  txp_renderer_bounds bounds;
  if (!txp_renderer_page_bounds(ctx, ui->doc_renderer, &bounds))
    return;

  float delta = bounds.window_size.y * scale.y * factor;
  float range = bounds.pan_interval.y < 0 ? 0 : bounds.pan_interval.y;

  //fprintf(stderr, "ui_pan: factor:%.02f delta:%.02f current:%.02f range:%.02f\n",
  //        factor, delta, config->pan.y, range);

  if (config->pan.y == -range && factor < 0)
  {
    next_page(ctx, ui, 1);
    return;
  }

  if (config->pan.y == range && factor > 0)
  {
    previous_page(ctx, ui, 1);
    return;
  }

  config->pan.y += delta;
  schedule_event(RENDER_EVENT);
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

  if ((page_count == ui->page - 2 || page_count == ui->page - 1) &&
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
  int go_up = 0;
  path = relative_path(path, ps->doc_path, &go_up);
  if (go_up > 0)
  {
    fprintf(stderr, "[command] open %s: file has a different root, skipping\n", path);
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
    fprintf(stderr, "[command] open %s: known file, updating\n", path);
    changed = find_diff(e->edit_data, data, size);
    if (e->edit_data->cap < size)
      fz_resize_buffer(ps->ctx, e->edit_data, size + 128);
    e->edit_data->len = size;
    memcpy(e->edit_data->data, data, size);
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
    fprintf(stderr, "[command] open %s: changed offset is %d\n", path, changed);
    send(notify_file_changes, ui->eng, ps->ctx, e, changed);
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

static void display_page(struct persistent_state *ps, ui_state *ui)
{
  fz_display_list *dl = send(render_page, ui->eng, ps->ctx, ui->page);
  txp_renderer_set_contents(ps->ctx, ui->doc_renderer, dl);
  fz_drop_display_list(ps->ctx, dl);
  schedule_event(RENDER_EVENT);
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
      schedule_event(RENDER_EVENT);
      fprintf(stderr, "[command] theme %x %x\n",
              config->background_color, config->foreground_color);
    }
    break;

    case EDIT_PREVIOUS_PAGE:
      previous_page(ps->ctx, ui, 0);
      break;

    case EDIT_NEXT_PAGE:
      next_page(ps->ctx, ui, 0);
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
        synctex_set_target(stx, ui->page, path, cmd.synctex_forward.line);
        schedule_event(STDIN_EVENT);
      }
    }
    break;

    case EDIT_CROP:
    {
      txp_renderer_config *config =
          txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      config->crop = !config->crop;
      schedule_event(RENDER_EVENT);
    }
    break;
    case EDIT_INVERT:
    {
      txp_renderer_config *config =
          txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      config->invert_color = !config->invert_color;
      schedule_event(RENDER_EVENT);
    }
    break;
  }
}

/* Base64 encoding for headless PNG output */

static const char b64_table[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const unsigned char *data, size_t len, size_t *out_len)
{
  size_t olen = 4 * ((len + 2) / 3);
  char *out = malloc(olen + 1);
  if (!out) return NULL;

  size_t i = 0, j = 0;
  for (; i + 2 < len; i += 3)
  {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i+1] << 8) | data[i+2];
    out[j++] = b64_table[(v >> 18) & 0x3F];
    out[j++] = b64_table[(v >> 12) & 0x3F];
    out[j++] = b64_table[(v >> 6) & 0x3F];
    out[j++] = b64_table[v & 0x3F];
  }
  if (i < len)
  {
    uint32_t v = (uint32_t)data[i] << 16;
    if (i + 1 < len) v |= (uint32_t)data[i+1] << 8;
    out[j++] = b64_table[(v >> 18) & 0x3F];
    out[j++] = b64_table[(v >> 12) & 0x3F];
    out[j++] = (i + 1 < len) ? b64_table[(v >> 6) & 0x3F] : '=';
    out[j++] = '=';
  }
  out[j] = '\0';
  *out_len = j;
  return out;
}

/* Headless rendering: render display_list to QOI and output as JSON */

// Store last render parameters for synctex backward coordinate transform
static float headless_render_scale = 1.0f;
static fz_rect headless_render_bounds = {0};

static void headless_render_page(fz_context *ctx, fz_display_list *dl,
                                  int page, int width, int height)
{
  if (!dl) return;

  fprintf(stderr, "[headless] ENTER render page=%d width=%d height=%d\n", page, width, height);
  fflush(stderr);

  fz_rect bounds = fz_bound_display_list(ctx, dl);
  float page_w = bounds.x1 - bounds.x0;
  float page_h = bounds.y1 - bounds.y0;

  if (page_w <= 0 || page_h <= 0) return;

  float scale = (float)width / page_w;
  int actual_h = (int)(page_h * scale);
  if (actual_h > height) {
    scale = (float)height / page_h;
    actual_h = height;
  }
  int actual_w = (int)(page_w * scale);

  headless_render_scale = scale;
  headless_render_bounds = bounds;

  fz_pixmap *pix = NULL;
  fz_var(pix);

  fz_try(ctx)
  {
    fz_matrix ctm = fz_pre_scale(fz_translate(-bounds.x0, -bounds.y0), scale, scale);
    fz_colorspace *cs = fz_device_rgb(ctx);
    fz_irect irect = fz_make_irect(0, 0, actual_w, actual_h);
    // alpha=0: MuPDF composites directly onto the white background
    pix = fz_new_pixmap_with_bbox(ctx, cs, irect, NULL, 0);
    fz_clear_pixmap_with_value(ctx, pix, 0xFF);

    fz_device *dev = fz_new_draw_device(ctx, ctm, pix);
    fz_run_display_list(ctx, dl, dev, fz_identity, fz_infinite_rect, NULL);
    fz_close_device(ctx, dev);
    fz_drop_device(ctx, dev);

    // Use MuPDF API for robust dimension extraction
    int pw = fz_pixmap_width(ctx, pix);
    int ph = fz_pixmap_height(ctx, pix);
    int stride = fz_pixmap_stride(ctx, pix);
    int n = fz_pixmap_components(ctx, pix);
    unsigned char *samples = fz_pixmap_samples(ctx, pix);

    // Diagnostic: log pixmap properties and first pixel of each of first rows
    fflush(stderr); fprintf(stderr, "[headless] pixmap: pw=%d ph=%d stride=%d n=%d packed_row=%d stride_row=%d\n",
            pw, ph, stride, n, pw * n, stride);
    if (ph >= 2 && pw >= 1) {
      unsigned char *row0 = samples;
      unsigned char *row1 = samples + stride;
      fflush(stderr); fprintf(stderr, "[headless] row0[0..2]=(%d,%d,%d) row1[0..2]=(%d,%d,%d)\n",
              row0[0], row0[1], row0[2], row1[0], row1[1], row1[2]);
    }

    // Allocate packed RGBA buffer (no padding between rows)
    size_t rgba_len = (size_t)pw * ph * 4;
    unsigned char *rgba = malloc(rgba_len);
    if (rgba)
    {
      // Row-by-row copy: source uses stride, dest uses packed pw*4
      for (int y = 0; y < ph; y++)
      {
        unsigned char *src_row = samples + (size_t)y * stride;
        unsigned char *dst_row = rgba + (size_t)y * pw * 4;
        if (n >= 4)
        {
          // Interleaved RGBA
          for (int x = 0; x < pw; x++)
          {
            dst_row[x * 4 + 0] = src_row[x * n + 0];
            dst_row[x * 4 + 1] = src_row[x * n + 1];
            dst_row[x * 4 + 2] = src_row[x * n + 2];
            dst_row[x * 4 + 3] = src_row[x * n + 3];
          }
        }
        else
        {
          // RGB only; fill alpha with 255
          for (int x = 0; x < pw; x++)
          {
            dst_row[x * 4 + 0] = src_row[x * n + 0];
            dst_row[x * 4 + 1] = src_row[x * n + 1];
            dst_row[x * 4 + 2] = src_row[x * n + 2];
            dst_row[x * 4 + 3] = 255;
          }
        }
      }

      // Diagnostic: verify packed RGBA - compare row 0 end vs row 1 start
      if (ph >= 2 && pw >= 2) {
        unsigned char *r0_last = rgba + (pw - 1) * 4;
        unsigned char *r1_first = rgba + pw * 4;
        fflush(stderr); fprintf(stderr, "[headless] rgba: r0[%d]=(%d,%d,%d,%d) r1[0]=(%d,%d,%d,%d)\n",
                pw-1,
                r0_last[0], r0_last[1], r0_last[2], r0_last[3],
                r1_first[0], r1_first[1], r1_first[2], r1_first[3]);
        // Sample middle pixel too
        int mid = pw / 2;
        unsigned char *r0_mid = rgba + mid * 4;
        unsigned char *r1_mid = rgba + pw * 4 + mid * 4;
        fflush(stderr); fprintf(stderr, "[headless] rgba: r0[%d]=(%d,%d,%d,%d) r1[%d]=(%d,%d,%d,%d)\n",
                mid,
                r0_mid[0], r0_mid[1], r0_mid[2], r0_mid[3],
                mid,
                r1_mid[0], r1_mid[1], r1_mid[2], r1_mid[3]);
      fflush(stderr);
      }

      qoi_desc desc = {
        .width = (unsigned int)pw,
        .height = (unsigned int)ph,
        .channels = 4,
        .colorspace = QOI_SRGB
      };
      int qoi_len;
      void *qoi_data = qoi_encode(rgba, &desc, &qoi_len);
      free(rgba);

      if (qoi_data)
      {
        size_t b64_len;
        char *b64 = base64_encode((unsigned char*)qoi_data, qoi_len, &b64_len);
        free(qoi_data);
        if (b64)
        {
          fprintf(stdout, "[\"page-rendered\",%d,%d,%d,\"%s\"]\n",
                  page, pw, ph, b64);
          fflush(stdout);
          free(b64);
        }
      }
    }
  }
  fz_always(ctx)
  {
    fz_drop_pixmap(ctx, pix);
  }
  fz_catch(ctx)
  {
    fprintf(stderr, "[headless] render error: %s\n", fz_caught_message(ctx));
  }
}

/* Headless main loop (no SDL window) */

static bool headless_advance_engine(fz_context *ctx, txp_engine *eng, int page)
{
  bool need = (send(page_count, eng) <= page) &&
              (send(get_status, eng) == DOC_RUNNING);
  if (!need) return false;

  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);

  int steps = 10;
  while (need)
  {
    if (!send(step, eng, ctx, false))
      break;

    steps -= 1;
    need = (send(page_count, eng) <= page) &&
           (send(get_status, eng) == DOC_RUNNING);

    if (steps == 0)
    {
      steps = 10;
      struct timespec curr;
      clock_gettime(CLOCK_MONOTONIC, &curr);
      int delta = (curr.tv_sec - start.tv_sec) * 1000000000 +
                  (curr.tv_nsec - start.tv_nsec);
      if (delta > 5000000)
        break;
    }
  }
  return need;
}

static bool texpresso_main_headless(struct persistent_state *ps)
{
  editor_set_protocol(ps->protocol);
  editor_set_line_output(ps->line_output);
  pstate = ps;

  bool using_texlive = 0;

  if (ps->use_texlive)
  {
    if (!texlive_available())
    {
      fprintf(stderr, "[fatal] cannot find kpsewhich command\n");
      return 0;
    }
    using_texlive = 1;
  }
  else if (ps->use_tectonic)
  {
    if (!tectonic_available())
    {
      fprintf(stderr, "[fatal] cannot find tectonic command\n");
      return 0;
    }
  }
  else if (!(using_texlive = texlive_available()) || !tectonic_available())
  {
    fprintf(stderr, "[fatal] cannot find tectonic nor kpsewhich\n");
    return 0;
  }

  const char *doc_ext = NULL;
  for (const char *ptr = ps->doc_name; *ptr; ptr++)
    if (*ptr == '.') doc_ext = ptr + 1;

  char engine_path[4096];
  find_engine(engine_path, ps->exe_path);
  fprintf(stderr, "[info] engine path: %s\n", engine_path);

  txp_engine *eng;

  if (doc_ext && strcmp(doc_ext, "pdf") == 0)
    eng = txp_create_pdf_engine(ps->ctx, ps->doc_name);
  else
  {
    dvi_reshooks hooks;
    if (using_texlive)
      hooks = dvi_texlive_hooks(ps->ctx, ps->doc_path);
    else
      hooks = dvi_tectonic_hooks(ps->ctx, ps->doc_path);

    if (doc_ext && (strcmp(doc_ext, "dvi") == 0 || strcmp(doc_ext, "xdv") == 0))
      eng = txp_create_dvi_engine(ps->ctx, ps->doc_name, hooks);
    else
      eng = txp_create_tex_engine(ps->ctx, engine_path, using_texlive,
                                  ps->stream_mode, ps->inclusion_path,
                                  ps->doc_name, hooks);
  }

  int page = 0;
  int render_width = 2400;  // default for 2x DPI, updated via render-size command
  int render_height = 3200;
  bool quit = 0;
  int last_rendered_page = -1;
  int last_page_count = 0;
  int pending_synctex_forward = 0;
  char *pending_synctex_path = NULL;
  int pending_synctex_line = 0;

  // Create a minimal ui_state for reusing command handlers
  ui_state headless_ui;
  memset(&headless_ui, 0, sizeof(headless_ui));
  headless_ui.eng = eng;
  headless_ui.page = 0;
  ui_state *ui = &headless_ui;

  vstack *cmd_stack = vstack_new(ps->ctx);
  prot_parser cmd_parser;
  prot_initialize(&cmd_parser, (ps->protocol == EDITOR_JSON));

  send(step, eng, ps->ctx, true);

  // Output initial page count
  fprintf(stdout, "[\"page-count\",%d]\n", send(page_count, eng));
  fflush(stdout);

  while (!quit)
  {
    // Poll stdin for commands
    struct pollfd fds[1];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLRDNORM;
    fds[0].revents = 0;

    int timeout_ms = headless_advance_engine(ps->ctx, eng, page) ? 0 : 50;
    int poll_result = poll(fds, 1, timeout_ms);

    // Process stdin
    send(begin_changes, eng, ps->ctx);
    if (poll_result > 0 && (fds[0].revents & POLLRDNORM))
    {
      char buffer[4096];
      int n = read(STDIN_FILENO, buffer, 4096);
      if (n == 0)
      {
        quit = 1;
        send(end_changes, eng, ps->ctx);
        break;
      }
      if (n > 0)
      {
        const char *ptr = buffer, *lim = buffer + n;
        fz_try(ps->ctx)
        {
          while ((ptr = prot_parse(ps->ctx, &cmd_parser, cmd_stack, ptr, lim)))
          {
            val cmds = vstack_get_values(ps->ctx, cmd_stack);
            int n_cmds = val_array_length(ps->ctx, cmd_stack, cmds);
            for (int i = 0; i < n_cmds; i++)
            {
              val cmd = val_array_get(ps->ctx, cmd_stack, cmds, i);
              // Parse command manually in headless mode
              struct editor_command ecmd;
              if (editor_parse(ps->ctx, cmd_stack, cmd, &ecmd))
              {
                switch (ecmd.tag)
                {
                  case EDIT_OPEN:
                    interpret_open(ps, ui, ecmd.open.path, ecmd.open.data, ecmd.open.length);
                    break;
                  case EDIT_CLOSE:
                    interpret_close(ps, ui, ecmd.close.path);
                    break;
                  case EDIT_CHANGE:
                    realize_change(ps, ui, &ecmd.change);
                    break;
                  case EDIT_PREVIOUS_PAGE:
                    if (page > 0) page -= 1;
                    ui->page = page;
                    break;
                  case EDIT_NEXT_PAGE:
                    page += 1;
                    ui->page = page;
                    break;
                  case EDIT_RESCAN:
                    send(detect_changes, eng, ps->ctx);
                    break;
                  case EDIT_SYNCTEX_FORWARD:
                  {
                    // Store target for later processing (after engine steps)
                    pending_synctex_forward = 1;
                    free(pending_synctex_path);
                    pending_synctex_path = strdup(ecmd.synctex_forward.path);
                    pending_synctex_line = ecmd.synctex_forward.line;
                    break;
                  }
                  case EDIT_SYNCTEX_BACKWARD:
                  {
                    fz_buffer *buf;
                    synctex_t *stx = send(synctex, eng, &buf);
                    int n_pages = stx ? synctex_page_count(stx) : 0;
                    fprintf(stderr, "[headless] synctex-backward: page=%d px=%.0f py=%.0f "
                            "scale=%.3f bounds=(%.1f,%.1f)-(%.1f,%.1f) synctex_pages=%d stx=%p buf=%p\n",
                            ecmd.synctex_backward.page,
                            ecmd.synctex_backward.x, ecmd.synctex_backward.y,
                            headless_render_scale,
                            headless_render_bounds.x0, headless_render_bounds.y0,
                            headless_render_bounds.x1, headless_render_bounds.y1,
                            n_pages, (void*)stx, (void*)buf);
                    if (stx && buf)
                    {
                      // Convert pixel coordinates to document coordinates
                      float px = ecmd.synctex_backward.x;
                      float py = ecmd.synctex_backward.y;
                      float doc_x = px / headless_render_scale + headless_render_bounds.x0;
                      float doc_y = py / headless_render_scale + headless_render_bounds.y0;
                      float f = 1.0f / send(scale_factor, eng);
                      fprintf(stderr, "[headless] synctex-backward: doc_x=%.1f doc_y=%.1f f=%.4f final_x=%d final_y=%d\n",
                              doc_x, doc_y, f, (int)(f * doc_x), (int)(f * doc_y));
                      synctex_scan(ps->ctx, stx, buf, ps->doc_path,
                                   ecmd.synctex_backward.page,
                                   (int)(f * doc_x), (int)(f * doc_y));
                    }
                    break;
                  }
                  case EDIT_RENDER_SIZE:
                  {
                    int w = ecmd.render_size.width;
                    if (w > 0 && w <= 7680)
                    {
                      render_width = w;
                      render_height = (int)((float)w * 1.414f); // A4-ish aspect
                      last_rendered_page = -1; // force re-render at new res
                    }
                    break;
                  }
                  default:
                    break;
                }
              }
            }
          }
        }
        fz_catch(ps->ctx)
        {
          fprintf(stderr, "[headless] stdin parse error: %s\n",
                  fz_caught_message(ps->ctx));
          vstack_reset(ps->ctx, cmd_stack);
          prot_reinitialize(&cmd_parser);
        }
      }
    }

    if (send(end_changes, eng, ps->ctx))
    {
      send(step, eng, ps->ctx, true);
      last_rendered_page = -1; // Force re-render
    }

    // Advance engine
    headless_advance_engine(ps->ctx, eng, page);

    // Check for page count changes
    int current_page_count = send(page_count, eng);
    if (current_page_count != last_page_count)
    {
      last_page_count = current_page_count;
      fprintf(stdout, "[\"page-count\",%d]\n", current_page_count);
      fflush(stdout);
    }

    // Clamp page to available pages
    if (page >= current_page_count && current_page_count > 0 &&
        send(get_status, eng) == DOC_TERMINATED)
      page = current_page_count - 1;

    // Render current page if changed
    if (page < current_page_count && page != last_rendered_page)
    {
      fz_display_list *dl = send(render_page, eng, ps->ctx, page);
      if (dl)
      {
        headless_render_page(ps->ctx, dl, page, render_width, render_height);
        fz_drop_display_list(ps->ctx, dl);
        last_rendered_page = page;
      }
    }

    // Handle synctex forward (after engine steps for consistent state)
    {
      fz_buffer *buf;
      synctex_t *stx = send(synctex, eng, &buf);

      if (pending_synctex_forward && pending_synctex_path) {
        pending_synctex_forward = 0;
        int go_up2 = 0;
        const char *spath = relative_path(pending_synctex_path, ps->doc_path, &go_up2);
        if (go_up2 == 0)
          synctex_set_target(stx, page, spath, pending_synctex_line);
        free(pending_synctex_path);
        pending_synctex_path = NULL;
      }

      int stx_page = -1, stx_x = -1, stx_y = -1;
      if (synctex_find_target(ps->ctx, stx, buf, &stx_page, &stx_x, &stx_y))
      {
        if (stx_page != page)
        {
          page = stx_page;
          last_rendered_page = -1; // Force re-render
        }
      }
    }

    editor_flush();
    fflush(stdout);
  }

  vstack_free(ps->ctx, cmd_stack);
  send(destroy, eng, ps->ctx);

  return 0;
}

/* Entry point */

bool texpresso_main(struct persistent_state *ps)
{
  if (ps->headless)
    return texpresso_main_headless(ps);

  editor_set_protocol(ps->protocol);
  editor_set_line_output(ps->line_output);
  pstate = ps;

  ui_state raw_ui, *ui = &raw_ui;

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
    ui->page = ps->initial.page;
    ui->zoom = ps->initial.zoom;
    ui->need_synctex = ps->initial.need_synctex;
    *txp_renderer_get_config(ps->ctx, ui->doc_renderer) = ps->initial.config;
    txp_renderer_set_contents(ps->ctx, ui->doc_renderer,
                              ps->initial.display_list);
    editor_reset_sync();
  }
  else
  {
    ui->page = 0;
    ui->zoom = 0;
    ui->need_synctex = 1;
  }

  ui->mouse_status = UI_MOUSE_NONE;
  ui->last_mouse_x = -1000;
  ui->last_mouse_y = -1000;
  ui->last_click_ticks = SDL_GetTicks() - 200000000;

  bool quit = 0, reload = 0;
  send(step, ui->eng, ps->ctx, true);
  render(ps->ctx, ui);
  schedule_event(RELOAD_EVENT);

  struct repaint_on_resize_env repaint_on_resize_env = {.ctx = ps->ctx, .ui = ui};
  SDL_AddEventWatch(repaint_on_resize, &repaint_on_resize_env);

  vstack *cmd_stack = vstack_new(ps->ctx);
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

  while (!quit)
  {
    SDL_Event e;
    bool has_event = SDL_PollEvent(&e);

    // Process stdin
    send(begin_changes, ui->eng, ps->ctx);
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
      fz_try(ps->ctx)
      {
        while ((ptr = prot_parse(ps->ctx, &cmd_parser, cmd_stack, ptr, lim)))
        {
          val cmds = vstack_get_values(ps->ctx, cmd_stack);
          int n_cmds = val_array_length(ps->ctx, cmd_stack, cmds);
          for (int i = 0; i < n_cmds; i++)
          {
            val cmd = val_array_get(ps->ctx, cmd_stack, cmds, i);
            interpret_command(ps, ui, cmd_stack, cmd);
          }
        }
      }
      fz_catch(ps->ctx)
      {
        fprintf(stderr, "error while reading stdin commands: %s\n",
                fz_caught_message(ps->ctx));
        vstack_reset(ps->ctx, cmd_stack);
        prot_reinitialize(&cmd_parser);
      }
    }
    if (n == 0) stdin_eof = 1;

    if (send(end_changes, ui->eng, ps->ctx))
    {
      send(step, ui->eng, ps->ctx, true);
      schedule_event(RELOAD_EVENT);
    }

    // Process document
    {
      int before_page_count = send(page_count, ui->eng);
      bool advance = advance_engine(ps->ctx, ui);
      int after_page_count = send(page_count, ui->eng);
      fflush(stdout);

      if (ui->page >= before_page_count && ui->page < after_page_count)
        schedule_event(RELOAD_EVENT);

      if (!has_event)
      {
        if (advance)
          continue;
        if (!stdin_eof)
          wakeup_poll_thread(poll_stdin_pipe, 'c');
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
      if (synctex_find_target(ps->ctx, stx, buf, &page, &x, &y))
      {
        fprintf(stderr, "[synctex forward] sync: hit page %d, coordinates (%d, %d)\n",
                page, x, y);

        if (page != ui->page)
        {
          ui->page = page;
          display_page(ps, ui);
        }

        // FIXME: Scroll to point
        float f = send(scale_factor, ui->eng);
        fz_point p = fz_make_point(f * x, f * y);
        fz_point pt = txp_renderer_document_to_screen(ps->ctx, ui->doc_renderer, p);
        fprintf(stderr, "[synctex forward] position on screen: (%.02f, %.02f)\n",
                pt.x, pt.y);
        int w, h;
        txp_renderer_screen_size(ps->ctx, ui->doc_renderer, &w, &h);
        float margin_lo = h / 4.0;
        float margin_hi = h / 3.0;

        txp_renderer_config *config =
            txp_renderer_get_config(ps->ctx, ui->doc_renderer);

        float delta = 0.0;
        if (pt.y < margin_lo)
          delta = - pt.y + margin_hi;
        else if (pt.y >= h - margin_lo)
          delta = h - pt.y - margin_hi;
        fprintf(stderr, "[synctex forward] pan.y = %.02f + %.02f = %.02f\n",
                config->pan.y, delta, config->pan.y + delta);
        config->pan.y += delta;
        if (delta != 0.0)
          schedule_event(RENDER_EVENT);
      }
    }

    txp_renderer_set_scale_factor(ps->ctx, ui->doc_renderer,
                                  get_scale_factor(ui->window));
    txp_renderer_config *config =
        txp_renderer_get_config(ps->ctx, ui->doc_renderer);

    // Process event
    switch (e.type)
    {
      case SDL_QUIT:
        quit = 1;
        break;

      case SDL_KEYDOWN:
        switch (e.key.keysym.sym)
        {
          case SDLK_LEFT:
          case SDLK_PAGEUP:
            previous_page(ps->ctx, ui, 0);
            break;

          case SDLK_UP:
            ui_pan(ps->ctx, ui, 2.0/3.0);
            break;

          case SDLK_DOWN:
            ui_pan(ps->ctx, ui, -2.0/3.0);
            break;

          case SDLK_RIGHT:
          case SDLK_PAGEDOWN:
            next_page(ps->ctx, ui, 0);
            break;

          case SDLK_p:
            config->fit = (config->fit == FIT_PAGE) ? FIT_WIDTH : FIT_PAGE;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_b:
            SDL_SetWindowBordered(
                ui->window,
                !!(SDL_GetWindowFlags(ui->window) & SDL_WINDOW_BORDERLESS));
            break;

          case SDLK_t:
            SDL_SetWindowAlwaysOnTop(
                ui->window,
                !(SDL_GetWindowFlags(ui->window) & SDL_WINDOW_ALWAYS_ON_TOP));
            break;

          case SDLK_c:
            config->crop = !config->crop;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_i:
            if ((SDL_GetModState() & KMOD_SHIFT))
              config->themed_color = !config->themed_color;
            else
              config->invert_color = !config->invert_color;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_ESCAPE:
            SDL_SetWindowFullscreen(ui->window, 0);
            break;

          case SDLK_F5:
            SDL_SetWindowFullscreen(ui->window, SDL_WINDOW_FULLSCREEN_DESKTOP);
            config->fit = FIT_PAGE;
            schedule_event(RENDER_EVENT);
            break;

          // case SDLK_r:
          //   reload = 1;
          case SDLK_q:
            quit = 1;
            break;
        }
        break;

      case SDL_MOUSEWHEEL:
        {
           int mx = 0, my = 0;
           float px = 0, py = 0;
#if SDL_VERSION_ATLEAST(2, 0, 260)
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
          bool ctrl = !!(SDL_GetModState() & KMOD_CTRL);
          ui_mouse_wheel(ps->ctx, ui, px, py, mx, my, ctrl, e.wheel.timestamp);
        }
        break;

      case SDL_MOUSEBUTTONDOWN:
        ui_mouse_down(ps, ui, e.button.x, e.button.y,
                      SDL_GetModState() & KMOD_CTRL);
        break;

      case SDL_MOUSEBUTTONUP:
        ui_mouse_up(ui);
        break;

      case SDL_MOUSEMOTION:
        ui_mouse_move(ps->ctx, ui, e.motion.x, e.motion.y);
        break;

      case SDL_WINDOWEVENT:
        switch (e.window.event)
        {
          case SDL_WINDOWEVENT_SIZE_CHANGED:
          case SDL_WINDOWEVENT_RESIZED:
          case SDL_WINDOWEVENT_EXPOSED:
            schedule_event(RENDER_EVENT);
            break;
        }
        break;
    }

    if (e.type == ps->custom_event)
    {
      int page_count;
      *(char *)e.user.data1 = 0;
      switch (e.user.code)
      {
        case SCAN_EVENT:
          if (should_reload_binary())
          {
            quit = reload = 1;
            continue;
          }
          send(begin_changes, ui->eng, ps->ctx);
          flush_changes(ps, ui);
          send(detect_changes, ui->eng, ps->ctx);
          if (send(end_changes, ui->eng, ps->ctx))
          {
            send(step, ui->eng, ps->ctx, true);
            schedule_event(RELOAD_EVENT);
          }
          break;

        case RENDER_EVENT:
          render(ps->ctx, ui);
          send(begin_changes, ui->eng, ps->ctx);
          flush_changes(ps, ui);
          if (send(end_changes, ui->eng, ps->ctx))
          {
            send(step, ui->eng, ps->ctx, true);
            schedule_event(RELOAD_EVENT);
          }
          break;

        case RELOAD_EVENT:
          page_count = send(page_count, ui->eng);
          if (ui->page >= page_count &&
              send(get_status, ui->eng) == DOC_TERMINATED)
          {
            if (page_count > 0)
              ui->page = page_count - 1;
          }
          if (ui->page < page_count)
            display_page(ps, ui);
          break;

        case STDIN_EVENT:
          break;
      }
    }
    if (ps->initialize_only)
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

  if (ps->initial.initialized && ps->initial.display_list)
    fz_drop_display_list(ps->ctx, ps->initial.display_list);
  ps->initial.initialized = 1;
  ps->initial.page = ui->page;
  ps->initial.need_synctex = ui->need_synctex;
  ps->initial.zoom = ui->zoom;
  ps->initial.config = *txp_renderer_get_config(ps->ctx, ui->doc_renderer);
  ps->initial.display_list = txp_renderer_get_contents(ps->ctx, ui->doc_renderer);
  if (ps->initial.display_list)
    fz_keep_display_list(ps->ctx, ps->initial.display_list);

  txp_renderer_free(ps->ctx, ui->doc_renderer);
  send(destroy, ui->eng, ps->ctx);

  return reload;
}
