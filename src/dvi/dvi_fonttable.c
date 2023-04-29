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

#include <stdlib.h>
#include <string.h>
#include "mydvi.h"

struct dvi_fonttable
{
  dvi_fontdef *buffer;
  int capacity;
};

dvi_fonttable *dvi_fonttable_new(fz_context *ctx)
{
  dvi_fonttable *ft = fz_malloc_struct(ctx, dvi_fonttable);
  ft->buffer = NULL;
  ft->capacity = 0;
  return ft;
}

void dvi_fonttable_free(fz_context *ctx, dvi_fonttable *ft)
{
  if (ft->buffer)
    fz_free(ctx, ft->buffer);
  fz_free(ctx, ft);
}

dvi_fontdef *dvi_fonttable_get(fz_context *ctx, dvi_fonttable *ft, int f)
{
  if (f < 0 || f > 9999)
  {
    printf("dvi_fonttable_get(_, %d)\n", f);
    abort();
  }
  if (f >= ft->capacity)
  {
    int capacity = 2;
    while (capacity <= f)
      capacity = capacity << 1;
    dvi_fontdef *buffer = fz_malloc_struct_array(ctx, capacity, dvi_fontdef);
    if (ft->buffer)
    {
      memcpy(buffer, ft->buffer, sizeof(dvi_fontdef) * ft->capacity);
      fz_free(ctx, ft->buffer);
    }
    ft->capacity = capacity;
    ft->buffer = buffer;
  }
  return &ft->buffer[f];
}

