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
#include "incdvi.h"
#include "mydvi.h"
#include "mydvi_interp.h"
#include "mydvi_opcodes.h"

struct incdvi_s
{
  int offset;
  int fontdef_offset;
  int page_len, page_cap;
  int *pages;
  dvi_context *dc;
};

static int add_page(fz_context *ctx, incdvi_t *d)
{
  int result = d->page_len;
  if (result == d->page_cap)
  {
    if (d->page_cap == 0)
    {
      d->pages = fz_malloc_struct_array(ctx, 8, int);
      d->page_cap = 8;
    }
    else
    {
      int *pages = fz_malloc_struct_array(ctx, d->page_cap * 2, int);
      memcpy(pages, d->pages, sizeof(int) * d->page_cap);
      fz_free(ctx, d->pages);
      d->pages = pages;
      d->page_cap = d->page_cap * 2;
    }
  }
  d->page_len += 1;
  return result;
}

incdvi_t *incdvi_new(fz_context *ctx, const char *tectonic_path, const char *document_directory)
{
  incdvi_t *d = fz_malloc_struct(ctx, incdvi_t);
  d->dc = dvi_context_new(ctx, dvi_bundle_serve_hooks(ctx, tectonic_path, document_directory));
  return d;
}

void incdvi_free(fz_context *ctx, incdvi_t *d)
{
  if (d->pages)
    fz_free(ctx, d->pages);
  dvi_context_free(ctx, d->dc);
  fz_free(ctx, d);
}

void incdvi_reset(incdvi_t *d)
{
  d->offset = 0;
  d->fontdef_offset = 0;
  d->page_len = 0;
}

void incdvi_update(fz_context *ctx, incdvi_t *d, fz_buffer *buf)
{
  if (buf == NULL)
  {
    incdvi_reset(d);
    return;
  }

  int len = buf->len;

  if (d->offset > len)
  {
    while (d->page_len > 0 && d->pages[d->page_len - 1] >= len)
      d->page_len -= 1;
    if (d->page_len == 0)
      d->offset = 0;
    else
    {
      d->page_len -= 1;
      d->offset = d->pages[d->page_len];
    }
  }

  if (d->offset == 0)
  {
    if (d->page_len != 0) abort();
    int plen = dvi_preamble_size(buf->data, len);
    if (plen > 0)
    {
      if (dvi_preamble_parse(ctx, d->dc, dvi_context_state(d->dc), buf->data))
        d->offset = plen;
    }
  }

  if (d->offset > 0)
  {
    enum dvi_version version = dvi_context_state(d->dc)->version;
    while (d->offset < len)
    {
      int ilen = dvi_instr_size(buf->data + d->offset, len - d->offset, version);
      if (ilen <= 0)
        break;
      if (buf->data[d->offset] == BOP || buf->data[d->offset] == EOP)
      {
        int page = add_page(ctx, d);
        if (!(page & 1) != (buf->data[d->offset] == BOP))
          abort();
        d->pages[page] = d->offset;
      }
      d->offset += ilen;
    }
  }

  if (d->fontdef_offset > d->offset)
    d->fontdef_offset = d->offset;
}

int incdvi_page_count(incdvi_t *d)
{
  return (d->page_len / 2);
}

static void incdvi_parse_fontdef(fz_context *ctx, incdvi_t *restrict d, fz_buffer *buf, int offset)
{
  if (offset > buf->len) abort();
  enum dvi_version version = dvi_context_state(d->dc)->version;
  while (d->fontdef_offset < offset)
  {
    int ilen = dvi_instr_size(
        buf->data + d->fontdef_offset,
        offset - d->fontdef_offset,
        version
        );
    if (ilen <= 0)
      break;
    if (buf->data[d->fontdef_offset] >= XXX1 && buf->data[d->fontdef_offset] <= XXX4)
      dvi_interp_init(ctx, d->dc, buf->data + d->fontdef_offset, offset - d->fontdef_offset);
    if (dvi_is_fontdef(buf->data[d->fontdef_offset]))
    {
      dvi_interp(ctx, d->dc, buf->data + d->fontdef_offset);
    }
    d->fontdef_offset += ilen;
  }
}

void incdvi_page_dim(incdvi_t *d, fz_buffer *buf, int page, float *width, float *height, bool *landscape)
{
  bool _landscape;
  if (!landscape) landscape = &_landscape;
  *landscape = 0;
  if (page < 0 || page >= incdvi_page_count(d)) abort();
  int bop = d->pages[page * 2];
  if (dvi_interp_bop(buf->data + bop, buf->len - bop, width, height, landscape) <= 0)
    abort();
  if (*landscape)
  {
    float tmp = *width;
    *width = *height;
    *height = tmp;
  }
}

void incdvi_render_page(fz_context *ctx, incdvi_t *d, fz_buffer *buf, int page, fz_device *dev)
{
  if (page < 0 || page >= incdvi_page_count(d)) abort();
  int offset = d->pages[page * 2];
  int eop = d->pages[page * 2 + 1];
  incdvi_parse_fontdef(ctx, d, buf, offset);

  dvi_context *dc = d->dc;
  enum dvi_version version = dvi_context_state(dc)->version;
  dvi_context_begin_frame(ctx, d->dc, dev);
  while (offset < eop)
  {
    int ilen = dvi_instr_size(buf->data + offset, eop - offset, version);
    if (ilen <= 0) abort();
    dvi_interp(ctx, dc, buf->data + offset);
    offset += ilen;
  }
  dvi_context_end_frame(ctx, dc);
}

float incdvi_tex_scale_factor(incdvi_t *d)
{
  if (d->page_len == 0)
    return 1;
  return d->dc->scale;
}
