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

#include <unistd.h>
#include <signal.h>
#include <mupdf/fitz/document.h>
#include "logo.h"
#include "driver.h"

#ifdef __APPLE__
#include <sys/syslimits.h>
#else
#include <linux/limits.h>
#endif

/* Custom SDL events  */

Uint32 custom_event = 0;

void schedule_event(enum custom_events ev)
{
  static char scheduled[EVENT_COUNT] = {0,};

  char *sched = scheduled + ev;
  if (!*sched)
  {
    *sched = 1;
    SDL_Event event;
    SDL_zero(event);
    event.type = custom_event;
    event.user.code = ev;
    event.user.data1 = sched;
    if (SDL_PushEvent(&event) < 0)
      abort();
  }
}

static void signal_usr1(int sig)
{
  (void)sig;

  static volatile int barrier = 0;

  if (!barrier)
  {
    barrier = 1;
    schedule_event(SCAN_EVENT);
    barrier = 0;
  }
}

/* Misc routines */

static char *last_index(char *path, char needle)
{
  char *result = path;
  while (*path)
  {
    if (*path == needle)
      result = path + 1;
    path += 1;
  }
  return result;
}

static bool get_executable_path(char path[PATH_MAX])
{
#ifdef __APPLE__
  uint32_t size = PATH_MAX;
  char exe_path[PATH_MAX];
  if (_NSGetExecutablePath(exe_path, &size) != 0)
    return 0;
#else
  char *exe_path = "/proc/self/exe";
#endif
  return realpath(exe_path, path);
}

static bool should_reload_binary(void)
{
  return 0;
}

int main(int argc, const char **argv)
{
  char work_dir[PATH_MAX];

  if (!getcwd(work_dir, PATH_MAX))
  {
    perror("get working directory");
    abort();
  }

  fprintf(stderr, "[info] working directory: %s\n", work_dir);

  char exe_path[PATH_MAX];

  if (!get_executable_path(exe_path) && !realpath(argv[0], exe_path))
  {
    perror("finding executable path");
    abort();
  }
  fprintf(stderr, "[info] executable path: %s\n", exe_path);

  const char *doc_arg = NULL;
  enum editor_protocol protocol = EDITOR_SEXP;
  bool line_output = 0;

  int inclusion_path_size = 1;
  for (int i = 1; i < argc; i++)
  {
    const char *arg = argv[i];

    if (arg[0] == '-')
    {
      if (arg[1] == 'j' &&
        arg[2] == 's' &&
        arg[3] == 'o' &&
        arg[4] == 'n' &&
        arg[5] == '\0')
      {
        protocol = EDITOR_JSON;
      }
      else if (arg[1] == 'I' && arg[2] == '\0')
      {
        i += 1;
        if (i == argc)
        {
          fprintf(stderr, "[error] Expecting a path after -I\n");
          exit(1);
        }
        inclusion_path_size += 1 + strlen(argv[i]);
      }
      else if (arg[1] == 'l' &&
        arg[2] == 'i' &&
        arg[3] == 'n' &&
        arg[4] == 'e' &&
        arg[5] == 's' &&
        arg[6] == '\0')
      {
        line_output = 1;
      }
      else
      {
        fprintf(stderr, "[error] Unknown option %s\n", arg);
        exit(1);
      }

      continue;
    }

    if (doc_arg == NULL)
    {
      doc_arg = arg;
      continue;
    }

    fprintf(stderr, "[error] Expecting a single document argument, got %s and %s\n",
            doc_arg, arg);
    exit(1);
  }

  if (doc_arg == NULL)
  {
    fprintf(stderr, "Usage: texpresso [-I path]* [-json] root_file.tex\n");
    exit(1);
  }

  char *inclusion_path = malloc(inclusion_path_size);
  if (!inclusion_path) abort();

  char *p = inclusion_path;
  for (int i = 1; i < argc; i++)
  {
    if (argv[i][0] == '-' && argv[i][1] == 'I' && argv[i][2] == '\0')
    {
      i += 1;
      p = stpcpy(p, argv[i]) + 1;
    }
  }
  *p = '\0';

  // Move to TeX document directory
  char doc_path[PATH_MAX];
  if (!realpath(doc_arg, doc_path))
  {
    perror("finding document path");
    abort();
  }

  char *doc_name = last_index(doc_path, '/');
  if (doc_path == doc_name) abort();
  doc_name[-1] = '\x00';

  fprintf(stderr, "[info] document path: %s\n", doc_path);
  fprintf(stderr, "[info] document name: %s\n", doc_name);

  if (chdir(doc_path) == -1)
  {
    perror("chdir to document path");
    abort();
  }

  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  fz_register_document_handlers(ctx);

  //Initialize SDL
  if (SDL_Init(SDL_INIT_VIDEO) < 0)
  {
    fprintf(stderr, "SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
    abort();
  }

  custom_event = SDL_RegisterEvents(1);
  signal(SIGUSR1, signal_usr1);

  //Create window
  char window_title[128] = "TeXpresso ";
  strcat(window_title, doc_name);

  SDL_Window *window;
  window = SDL_CreateWindow(window_title,
    SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
    700, 900,
    SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE
  );

  if (window == NULL)
  {
    fprintf(stderr, "Window could not be created! SDL_Error: %s\n", SDL_GetError() );
    abort();
  }

  SDL_Surface *logo = texpresso_logo();
  fprintf(stderr, "texpresso logo: %dx%d\n", logo->w, logo->h);
  SDL_SetWindowIcon(window, logo);
  SDL_FreeSurface(logo);

  SDL_Renderer *renderer;
  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);

  struct persistent_state pstate = {
      .initial = {0,},
      .protocol = protocol,
      .line_output = line_output,
      .window = window,
      .renderer = renderer,
      .ctx = ctx,
      .exe_path = exe_path,
      .doc_path = doc_path,
      .doc_name = doc_name,
      .inclusion_path = inclusion_path,
      .custom_event = custom_event,
      .schedule_event = &schedule_event,
      .should_reload_binary = &should_reload_binary,
  };

  while (texpresso_main(&pstate));

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  fz_drop_context(ctx);

  return 0;
}
