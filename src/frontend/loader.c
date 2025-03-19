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

#include <dlfcn.h>
#include <string.h>
#include <sys/stat.h>
#include "driver.h"

char so_path[4096];
int so_ino, so_dev;

static bool should_reload_binary(void)
{
  struct stat st;
  if (stat(so_path, &st) != 0)
  {
    perror("stat");
    fprintf(stderr, "stat(\"%s\") failed\n", so_path);
    return 0;
  }
  return (so_ino != st.st_ino || so_dev != st.st_dev);
}

bool texpresso_main(struct persistent_state *ps)
{
  ps->should_reload_binary = &should_reload_binary;

  strcpy(so_path, ps->exe_path);
  strcat(so_path, ".so");

  struct stat st;
  if (stat(so_path, &st) != 0)
  {
    perror("stat");
    fprintf(stderr, "stat(\"%s\") failed\n", so_path);
    return 0;
  }
  so_ino = st.st_ino;
  so_dev = st.st_dev;

  void *handle = dlopen(so_path, RTLD_LAZY | RTLD_LOCAL);

  if (!handle)
  {
    perror("dlopen");
    fprintf(stderr, "dlopen(\"%s\") failed\n", so_path);
    return 0;
  }

  bool (*main)(struct persistent_state *ps) = dlsym(handle, "texpresso_main");

  if (!main)
  {
    perror("dlsym");
    fprintf(stderr, "dlsym(\"texpresso_main\") failed\n");
    return 0;
  }

  bool result = main(ps);
  dlclose(handle);

  return result;
}
