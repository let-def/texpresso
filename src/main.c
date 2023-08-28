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
#include "incdvi.h"
#include "renderer.h"
#include "sprotocol.h"
#include "engine.h"
#include "logo.h"
#include "driver.h"
#include "synctex.h"
#include "vstack.h"
#include "sexp_parser.h"
#include "editor.h"

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

static void find_tectonic(char tectonic_path[4096], const char *exec_path)
{
  strcpy(tectonic_path, exec_path);
  char *basename = NULL;
  for (int i = 0; i < 4096 && tectonic_path[i]; ++i)
    if (tectonic_path[i] == '/')
      basename = tectonic_path + i + 1;
  uint64_t time = 0;
  if (basename)
  {
    strcpy(basename, "texpresso-tonic");
    if (!is_more_recent(&time, tectonic_path))
      strcpy(tectonic_path, "texpresso-tonic");
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
      (synctex_has_target(stx) &&
       !synctex_find_target(ctx, stx, buf, NULL, NULL, NULL));
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
    if (!send(step,ui->eng, ctx, false))
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

      if (delta > 1000000)
        break;
    }
  }
  return need;
}

static fz_point get_scale_factor(SDL_Window *window)
{
  int ww, wh, pw, ph;
  SDL_GetWindowSize(window, &ww, &wh);
  SDL_GetWindowSizeInPixels(window, &pw, &ph);

  return fz_make_point(ww != 0 ? (float)pw / ww : 1,
                       wh != 0 ? (float)ph / wh : 1);
}

/* UI events */

static void ui_mouse_down(fz_context *ctx, ui_state *ui, int x, int y, bool ctrl)
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
      diff = txp_renderer_select_word(ctx, ui->doc_renderer, p);
    }
    else
    {
      diff = txp_renderer_start_selection(ctx, ui->doc_renderer, p);
      diff = txp_renderer_select_char(ctx, ui->doc_renderer, p) || diff;
      ui->last_click_ticks = ticks;

      fz_buffer *buf;
      synctex_t *stx = send(synctex, ui->eng, &buf);
      if (stx && buf)
      {
        fz_point pt = txp_renderer_screen_to_document(ctx, ui->doc_renderer, p);
        float f = 1 / send(scale_factor, ui->eng);
        // pt.x -= 72;
        // pt.y -= 72;
        fprintf(stderr, "click: (%f,%f) mapped:(%f,%f)\n",
                pt.x, pt.y, f * pt.x, f * pt.y);
        synctex_scan(ctx, stx, buf, ui->page, f * pt.x, f * pt.y);
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

static void previous_page(ui_state *ui)
{
  synctex_set_target(send(synctex, ui->eng, NULL), NULL, 0);
  if (ui->page > 0)
  {
    ui->page -= 1;
    int page_count = send(page_count, ui->eng);
    if (page_count > 0 && ui->page >= page_count &&
        send(get_status, ui->eng) == DOC_TERMINATED)
      ui->page = page_count - 1;
    schedule_event(RELOAD_EVENT);
  }
}

static void next_page(ui_state *ui)
{
  synctex_set_target(send(synctex, ui->eng, NULL), NULL, 0);
  ui->page += 1;
  schedule_event(RELOAD_EVENT);
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
  return i;
}

static void realize_change(struct persistent_state *ps,
                             ui_state *ui,
                             const char *path,
                             int offset,
                             int remove,
                             const char *data,
                             int length)
{
  int go_up = 0;
  path = relative_path(path, ps->doc_path, &go_up);
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

  memmove(b->data + offset, data, length);

  fprintf(stderr, "[command] change %s: changed offset %d\n", path, offset);
  send(notify_file_changes, ui->eng, ps->ctx, e, offset);
}

#define BUFFERED_OPS 64
#define BUFFERED_CHARS 4096

struct {
  char buffer[BUFFERED_CHARS];
  int cursor;
  struct delayed_op {
    const char *path;
    const char *data;
    int offset, remove, length;
  } op[BUFFERED_OPS];
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
      struct delayed_op *op = &delayed_changes.op[i];
      realize_change(ps, ui, op->path, op->offset, op->remove, op->data, op->length);
    }
  }
}

static void interpret_change(struct persistent_state *ps,
                             ui_state *ui,
                             const char *path,
                             int offset,
                             int remove,
                             const char *data,
                             int length)
{
  int plen = strlen(path);
  int page_count = send(page_count, ui->eng);
  int cursor = delayed_changes.cursor;

  if ((page_count == ui->page - 2 || page_count == ui->page - 1) &&
      send(get_status, ui->eng) == DOC_RUNNING &&
      delayed_changes.count < BUFFERED_OPS &&
      cursor + plen + 1 + length <= BUFFERED_CHARS)
  {
    char *op_path = delayed_changes.buffer + cursor;
    memcpy(op_path, path, plen + 1);
    cursor += plen + 1;
    char *op_data = delayed_changes.buffer + cursor;
    memcpy(op_data, data, length);
    cursor += length;
    delayed_changes.cursor = cursor;

    delayed_changes.op[delayed_changes.count] = (struct delayed_op){
      .path = op_path,
      .data = op_data,
      .offset = offset,
      .remove = remove,
      .length = length,
    };
    delayed_changes.count += 1;
  }
  else
  {
    flush_changes(ps, ui);
    realize_change(ps, ui, path, offset, remove, data, length);
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
      interpret_open(ps, ui, cmd.open.path, cmd.open.data, cmd.open.length);
      break;

    case EDIT_CLOSE:
      interpret_close(ps, ui, cmd.close.path);
      break;

    case EDIT_CHANGE:
      interpret_change(ps, ui, cmd.change.path, cmd.change.offset,
                       cmd.change.remove_length, cmd.change.data,
                       cmd.change.insert_length);
      break;

    case EDIT_THEME:
      ps->theme_bg = convert_color(ps->ctx, stack, cmd.theme.bg);
      ps->theme_fg = convert_color(ps->ctx, stack, cmd.theme.fg);
      txp_renderer_config *config =
          txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      if (config->background_color != 0xFFFFFF &&
          config->foreground_color != 0x000000)
      {
        config->background_color = ps->theme_bg;
        config->foreground_color = ps->theme_fg;
        schedule_event(RENDER_EVENT);
      }
      fprintf(stderr, "[command] theme %x %x\n", ps->theme_bg, ps->theme_fg);
      break;

    case EDIT_PREVIOUS_PAGE:
      previous_page(ui);
      break;

    case EDIT_NEXT_PAGE:
      next_page(ui);
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
        synctex_set_target(stx, path, cmd.synctex_forward.line);
    }
    break;
  }
}

/* Entry point */

bool texpresso_main(struct persistent_state *ps)
{
  editor_set_protocol(ps->protocol);
  pstate = ps;

  ui_state raw_ui, *ui = &raw_ui;

  ui->window = ps->window;

  const char *doc_ext = NULL;

  for (const char *ptr = ps->doc_name; *ptr; ptr++)
    if (*ptr == '.')
      doc_ext = ptr + 1;

  char tectonic_path[4096];
  find_tectonic(tectonic_path, ps->exe_path);
  fprintf(stderr, "[info] tectonic path: %s\n", tectonic_path);

  if (doc_ext && strcmp(doc_ext, "pdf") == 0)
    ui->eng = txp_create_pdf_engine(ps->ctx, ps->doc_name);
  else if (doc_ext && (strcmp(doc_ext, "dvi") == 0 || strcmp(doc_ext, "xdv") == 0))
    ui->eng = txp_create_dvi_engine(ps->ctx, tectonic_path, ps->doc_path, ps->doc_name);
  else
    ui->eng = txp_create_tex_engine(ps->ctx, tectonic_path, ps->doc_path, ps->doc_name);

  ui->sdl_renderer = ps->renderer;
  ui->doc_renderer = txp_renderer_new(ps->ctx, ui->sdl_renderer);

  if (ps->initial.initialized)
  {
    ui->page = ps->initial.page;
    ui->zoom = ps->initial.zoom;
    ui->need_synctex = ps->initial.need_synctex;
    *txp_renderer_get_config(ps->ctx, ui->doc_renderer) = ps->initial.config;
    txp_renderer_set_contents(ps->ctx, ui->doc_renderer, ps->initial.display_list);
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
  sexp_parser cmd_parser = initial_sexp_parser;

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
        while ((ptr = sexp_parse(ps->ctx, &cmd_parser, cmd_stack, ptr, lim)))
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
        cmd_parser = initial_sexp_parser;
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
      if (synctex_has_target(stx) &&
          synctex_find_target(ps->ctx, stx, buf, &page, &x, &y))
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
        float margin = h / 5.0;

        txp_renderer_config *config =
            txp_renderer_get_config(ps->ctx, ui->doc_renderer);

        float delta = 0.0;
        if (pt.y < margin)
          delta = - pt.y + margin;
        else if (pt.y >= h - margin)
          delta = h - pt.y - margin;
        fprintf(stderr, "[synctex forward] pan.y = %.02f + %.02f = %.02f\n",
                config->pan.y, delta, config->pan.y + delta);
        config->pan.y += delta;
        synctex_set_target(stx, NULL, 0);
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
            previous_page(ui);
            break;

          case SDLK_RIGHT:
          case SDLK_PAGEDOWN:
            next_page(ui);
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
            if (!(SDL_GetModState() & KMOD_SHIFT))
              config->invert_color = !config->invert_color;
            else
            {
              if (config->foreground_color == 0x000000 &&
                  config->background_color == 0xFFFFFF)
              {
                config->foreground_color = ps->theme_fg;
                config->background_color = ps->theme_bg;
              }
              else
              {
                config->foreground_color = 0x000000;
                config->background_color = 0xFFFFFF;
              }
            }
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
        ui_mouse_wheel(ps->ctx, ui, e.wheel.preciseX, e.wheel.preciseY,
                       e.wheel.mouseX, e.wheel.mouseY,
                       SDL_GetModState() & KMOD_CTRL, e.wheel.timestamp);
        break;

      case SDL_MOUSEBUTTONDOWN:
        ui_mouse_down(ps->ctx, ui, e.button.x, e.button.y,
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
