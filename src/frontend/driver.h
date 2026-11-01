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
#include "page_collection.h"
#include "viewer.h"

enum custom_events {
  SCAN_EVENT,
  RENDER_EVENT,
  RELOAD_EVENT,

  EVENT_COUNT,
};

struct txp_theme
{
  bool themed, inverted;
  uint32_t background, foreground;
};

// UI state kept when reloading the binary
struct initial_state
{
  int initialized;
  txp_page_collection pages;
  txp_viewer viewer;
  struct txp_theme theme;
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
  // Readiness of a watched file descriptor (see fd_poll.h)
  Uint32 fd_event;

  void (*schedule_event)(enum custom_events ev);
  bool (*should_reload_binary)(void);

  SDL_Window *window;
  SDL_Renderer *renderer;
  fz_context *ctx;

  const char *exe_path, *doc_path, *doc_name, *inclusion_path;

  bool line_output, use_tectonic, use_texlive, initialize_only, stream_mode;
  // Show all pages, scrolling continuously, rather than one page at a time
  bool continuous;
  bool paused;
  bool rerun_enabled;
  bool rerun_once_pending;
};

bool texpresso_main(struct persistent_state *ps);

#endif // DRIVER_H_
