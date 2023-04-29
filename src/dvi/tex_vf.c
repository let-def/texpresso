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

#include <string.h>
#include "mydvi.h"
#include "intcodec.h"
#include "fz_util.h"

struct tex_vf
{
  fz_buffer *buffer;
  uint8_t comment_len, *comment;
  uint32_t checksum;
  fixed_t design_size;
  dvi_fonttable *fonts;
  tex_vf_char *chars;
  int char_cap;
  int default_font;
};

enum vf_op
{
  LONG_CHAR = 242,
  FNT_DEF1  = 243,
  FNT_DEF2  = 244,
  FNT_DEF3  = 245,
  FNT_DEF4  = 246,
  PRE       = 247,
  POST      = 248
};

static tex_vf_char *tex_vf_at(fz_context *ctx, tex_vf *vf, int code)
{
  if (code >= vf->char_cap)
  {
    int capacity = 8;
    while (capacity <= code)
      capacity *= 2;
    tex_vf_char *chars = fz_malloc_struct_array(ctx, capacity, tex_vf_char);
    if (vf->chars)
    {
      memcpy(chars, vf->chars, sizeof(tex_vf_char) * vf->char_cap);
      fz_free(ctx, vf->chars);
    }
    vf->chars = chars;
    vf->char_cap = capacity;
  }
  return &vf->chars[code];
}

#define FAIL(...) fz_throw(ctx, 0, "tex_vf_load: " __VA_ARGS__)

tex_vf *tex_vf_load(fz_context *ctx, dvi_resmanager *manager, fz_stream *stm)
{
  fz_ptr(fz_buffer, buffer);
  fz_ptr(tex_vf, vf);

  fz_try(ctx)
  {
    buffer = fz_read_all(ctx, stm, 4096);
    fz_trim_buffer(ctx, buffer);
    vf = fz_malloc_struct(ctx, tex_vf);

    if (buffer->len < 16) FAIL("file is too small");
    if (buffer->data[0] != PRE) FAIL("file doesn't start with preamble");
    if (buffer->data[1] != DVI_VF) FAIL("invalid preamble ID");

    vf->buffer = buffer;
    vf->comment_len = buffer->data[2];
    vf->comment = buffer->data + 3;
    vf->fonts = dvi_fonttable_new(ctx);
    vf->default_font = -1;

    const uint8_t
      *cursor = buffer->data + 3 + vf->comment_len,
      *end = buffer->data + buffer->len;

    vf->checksum = read_u32(&cursor);
    vf->design_size = fixed_make(read_s32(&cursor));

    while (cursor < end)
    {
      uint8_t op = read_u8(&cursor);
      if (op == POST)
        break;
      else if (op <= LONG_CHAR)
      {
        uint32_t len, code, width;

        if (op == LONG_CHAR)
        {
          if (cursor + 12 >= end) FAIL("truncated file");
          len = read_u32(&cursor);
          code = read_u32(&cursor);
          width = read_u32(&cursor);
        }
        else
        {
          if (cursor + 4 >= end) FAIL("truncated file");
          len = op;
          code = read_u8(&cursor);
          width = read_u24(&cursor);
        }
        if (cursor + len >= end)
          FAIL("truncated file (or DVI program is too long)");

        tex_vf_char *c = tex_vf_at(ctx, vf, code);
        c->dvi = cursor;
        c->dvi_length = len;
        c->width = fixed_make(width);
        cursor += len;
      }
      else if (op >= FNT_DEF1 && op <= FNT_DEF4)
      {
        int n = op - FNT_DEF1 + 1;
        if (cursor + n + 13 >= end)
          FAIL("truncated file");
        uint32_t font_id = read_uB(&cursor, n);
        if (vf->default_font == -1)
          vf->default_font = font_id;
        uint32_t checksum = read_u32(&cursor);
        fixed_t scale_factor = fixed_make(read_s32(&cursor));
        fixed_t design_size = fixed_make(read_s32(&cursor));
        int name_len = 0;
        name_len += read_u8(&cursor);
        name_len += read_u8(&cursor);
        const char *name = (const char *)cursor;
        cursor += name_len;

        // int c = *cursor;
        // *(char*)cursor = 0;
        // fprintf(stderr, "vf: defining font %s\n", name);
        // *(char*)cursor = c;

        if (cursor > end)
          FAIL("truncated file");

        dvi_fontdef *fontdef = dvi_fonttable_get(ctx, vf->fonts, font_id);
        if (!fontdef) abort();

        fontdef->kind = TEX_FONT;
        fontdef->tex_font.font = dvi_resmanager_get_tex_font(ctx, manager, name, name_len);
        fontdef->tex_font.spec.checksum     = checksum;
        fontdef->tex_font.spec.scale_factor = scale_factor;
        fontdef->tex_font.spec.design_size  = design_size;
      }
      else
        FAIL("invalid opcode");
    }

    if (cursor > end)
      FAIL("truncated file");
  }
  fz_catch(ctx)
  {
    if (buffer)
      fz_drop_buffer(ctx, buffer);
    if (vf)
    {
      if (vf->fonts)
        dvi_fonttable_free(ctx, vf->fonts);
      fz_free(ctx, vf);
    }
    fz_rethrow(ctx);
  }

  return vf;
}

tex_vf_char *tex_vf_get(tex_vf *vf, int code)
{
  if (code >= 0 && code < vf->char_cap)
  {
    tex_vf_char *c = &vf->chars[code];
    if (c->dvi)
      return c;
  }

  return NULL;
}

dvi_fonttable *tex_vf_fonttable(tex_vf *vf)
{
  return vf->fonts;
}

int tex_vf_default_font(tex_vf *vf)
{
  return vf->default_font;
}
