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
#include "mydvi.h"

struct dvi_scratch_buf
{
  struct dvi_scratch_buf *prev;
  int size, cursor;
  uint8_t data[0];
};

void *dvi_scratch_alloc(fz_context *ctx, dvi_scratch *t, size_t len)
{
  while (1)
  {
    // Check if there is some spare space in current buffer
    if (t->buf)
    {
      int cursor = t->buf->cursor - len;
      if (len > 3)
      {
        // Align allocation
        if (len > 7)
          cursor &= ~0x7;
        else if (len > 3)
          cursor &= ~0x3;
        else if (len > 1)
          cursor &= ~0x1;
      }
      if (cursor >= 0)
      {
        t->buf->cursor = cursor;
        return &t->buf->data[cursor];
      }
    }

    // Allocate a new buffer
    size_t bufsize = t->buf ? t->buf->size * 2 : 256;
    while (bufsize < len)
      bufsize *= 2;
    struct dvi_scratch_buf *buf =
      fz_malloc(ctx, sizeof(struct dvi_scratch_buf) + bufsize);
    buf->prev = t->buf;
    buf->size = bufsize;
    buf->cursor = bufsize;
    t->buf = buf;
  }
}

static void free_bufs(fz_context *ctx, struct dvi_scratch_buf **pbuf)
{
  struct dvi_scratch_buf *buf = *pbuf;

  while (buf)
  {
    struct dvi_scratch_buf *prev = buf->prev;
    fz_free(ctx, buf);
    buf = prev;
  }

  *pbuf = NULL;
}

void dvi_scratch_init(dvi_scratch *t)
{
  t->buf = NULL;
}

void dvi_scratch_release(fz_context *ctx, dvi_scratch *t)
{
  free_bufs(ctx, &t->buf);
}

void dvi_scratch_clear(fz_context *ctx, dvi_scratch *t)
{
  if (t->buf)
    free_bufs(ctx, &t->buf->prev);
}
