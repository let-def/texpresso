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

#include "myabort.h"
#include <execinfo.h>
#include <stdlib.h>
#include <stdio.h>

#define BT_BUF_SIZE 100

void myabort_(const char *file, int line, const char *msg, uint32_t code)
{
  if (code == 42424242)
    fprintf(stderr, "Aborting from %s:%d (%s)\n", file, line, msg);
  else
    fprintf(stderr, "Aborting from %s:%d (%s: %08X, '%c%c%c%c')\n", file, line, msg, code, code & 0xFF, (code >> 8) & 0XFF, (code >> 16) & 0xFF, (code >> 24) & 0xFF);

  print_backtrace();
  abort();
}
