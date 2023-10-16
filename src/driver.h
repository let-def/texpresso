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

#ifndef DRIVER_H_
#define DRIVER_H_

#include <stdbool.h>
#include <SDL2/SDL.h>
#include <mupdf/fitz/context.h>
#include "renderer.h"

enum custom_events {
  SCAN_EVENT,
  RENDER_EVENT,
  RELOAD_EVENT,
  EVENT_COUNT,
  STDIN_EVENT,
};

struct initial_state
{
  int initialized;
  int page;
  int need_synctex;
  int zoom;
  txp_renderer_config config;
  fz_display_list *display_list;
};

enum editor_protocol
{
  EDITOR_SEXP,
  EDITOR_JSON,
};

struct persistent_state {
  struct initial_state initial;
  enum editor_protocol protocol;
  Uint32 custom_event;

  void (*schedule_event)(enum custom_events ev);
  bool (*should_reload_binary)(void);

  SDL_Window *window;
  SDL_Renderer *renderer;
  fz_context *ctx;

  const char *exe_path, *doc_path, *doc_name;
};

bool texpresso_main(struct persistent_state *ps);

#endif // DRIVER_H_
