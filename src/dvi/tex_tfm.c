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

// the char info word for each character consists of 4 bytes holding the following information:
// width index w, height index (h), depth index (d), italic correction index (it),
// tag (tg) and a remainder:
//
// byte 1   | byte 2  | byte 3  | byte 4
// xxxxxxxx | xxxx xxxx | xxxxxx xx | xxxxxxxx
// w    | h  d  | it   tg | remainder

// typedef struct tfm_char_info_s {
//   uint8_t w, h_d, it_tg, remainder;
// } tfm_char_info_t;

struct tex_tfm {
  uint32_t checksum;
  uint16_t first_char, last_char;
  fixed_t design_size;

  void *buffer;

  uint32_t *char_table;

  fixed_t
    *width_table, *height_table, *depth_table,
    *italic_table,
    params[7], ascent, descent;
};

enum tex_tfm_param {
  PARAM_SPACE = 1,
  PARAM_SPACE_STRETCH = 2,
  PARAM_SPACE_SHRINK = 3,
  PARAM_QUAD = 5,
};

fixed_t tex_tfm_ascent(const tex_tfm *t)
{
  return fixed_mul(t->ascent, t->design_size);
}

fixed_t tex_tfm_descent(const tex_tfm* t)
{
  return fixed_mul(t->descent, t->design_size);
}

uint32_t tex_tfm_checksum(const tex_tfm *t)
{
  return t->checksum;
}

uint16_t tex_tfm_first_char(const tex_tfm *t)
{
  return t->first_char;
}

uint16_t tex_tfm_last_char(const tex_tfm *t)
{
  return t->last_char;
}

fixed_t tex_tfm_design_size(const tex_tfm *t)
{
  return t->design_size;
}

static fixed_t tex_tfm_scaled_param(const tex_tfm *t, enum tex_tfm_param p)
{
  return fixed_mul(t->params[p], t->design_size);
}

fixed_t tex_tfm_space(const tex_tfm *t)
{
  return tex_tfm_scaled_param(t, PARAM_SPACE);
}

fixed_t tex_tfm_space_stretch(const tex_tfm *t)
{
  return tex_tfm_scaled_param(t, PARAM_SPACE_STRETCH);
}

fixed_t tex_tfm_space_shrink(const tex_tfm *t)
{
  return tex_tfm_scaled_param(t, PARAM_SPACE_SHRINK);
}

fixed_t tex_tfm_quad(const tex_tfm *t)
{
  fixed_t r = tex_tfm_scaled_param(t, PARAM_QUAD);
  if (r.value == 0) r = t->design_size;
  return r;
}

static int tex_tfm_char_index(const tex_tfm *t, int c)
{
  if (c < t->first_char || c > t->last_char)
    return -1;
  return c - t->first_char;
}

fixed_t tex_tfm_char_width(const tex_tfm *t, int c)
{
  int index = tex_tfm_char_index(t, c);
  if (index == -1)
    return fixed_make(0);
  index = (t->char_table[index] >> 24) & 0xFF;
  fixed_t x = t->width_table[index];
  return x; //, t->design_size);
}

fixed_t tex_tfm_char_height(const tex_tfm *t, int c)
{
  int index = tex_tfm_char_index(t, c);
  if (index == -1)
    return fixed_make(0);
  index = (t->char_table[index] >> 20) & 0x0F;
  fixed_t x = t->height_table[index];
  return x;// fixed_mul(x, t->design_size);
}

fixed_t tex_tfm_char_depth(const tex_tfm *t, int c)
{
  int index = tex_tfm_char_index(t, c);
  if (index == -1)
    return fixed_make(0);
  index = (t->char_table[index] >> 16) & 0x0F;
  fixed_t x = t->depth_table[index];
  return x;// fixed_mul(x, t->design_size);
}

fixed_t tex_tfm_italic_corr(const tex_tfm *t, int c)
{
  int index = tex_tfm_char_index(t, c);
  if (index == -1)
    return fixed_make(0);
  index = (t->char_table[index] >> 10) & 0x3F;
  fixed_t x = t->italic_table[index];
  return fixed_mul(x, t->design_size);
}

static void tex_tfm_decode_words(void *buf, int n)
{
  for (; n > 0; n--)
  {
    ((fixed_t*)(buf))->value = decode_s32(buf);
    buf += 4;
  }
}

static void tex_tfm_read_tables(tex_tfm *t, void *block, int nw, int nh, int nd, int ni)
{
  int char_count = (t->last_char - t->first_char + 1);
  tex_tfm_decode_words(block, char_count + nw + nh + nd + ni);

  t->char_table = (uint32_t*)block;
  block += char_count * 4;

  t->width_table = block;
  block += nw * 4;

  t->height_table = block;
  block += nh * 4;

  t->depth_table = block;
  block += nd * 4;

  t->italic_table = block;
  block += ni * 4;

  int32_t ascent = 0, descent = 0;
  for (int i = 0; i < nh; i++)
    if (t->height_table[i].value > ascent)
      ascent = t->height_table[i].value;
  for (int i = 0; i < nd; i++)
    if (t->depth_table[i].value > descent)
      descent = t->depth_table[i].value;

  t->ascent.value = ascent;
  t->descent.value = descent;
}

static void tex_tfm_read_parameters(fixed_t *params, int np, void *block)
{
  if (np > 7) np = 7;

  for (int i = 0; i < np; ++i)
  {
    params[i].value = decode_s32(block + 4 * i);
  }
}

#define FAIL(...) fz_throw(ctx, 0, "tex_tfm_load: " __VA_ARGS__)

tex_tfm *tex_tfm_load(fz_context *ctx, fz_stream *stm)
{
  uint8_t buf[24];
  const uint8_t *p = buf;
  fz_ptr(void, buffer);
  fz_ptr(tex_tfm, t);

  if (fz_read(ctx, stm, buf, 24) != 24)
    FAIL("Cannot read header");

  fz_try(ctx)
  {
    /* word  1 */ uint16_t lf = read_u16(&p); // length of entire file in 4 byte words
    /* word  2 */ uint16_t lh = read_u16(&p); // length of header in 4 byte words
    /* word  3 */ uint16_t bc = read_u16(&p); // smallest character code in font
    /* word  4 */ uint16_t ec = read_u16(&p); // largest character code in font
    /* word  5 */ uint16_t nw = read_u16(&p); // number of words in width table
    /* word  6 */ uint16_t nh = read_u16(&p); // number of words in height table
    /* word  7 */ uint16_t nd = read_u16(&p); // number of words in depth table
    /* word  8 */ uint16_t ni = read_u16(&p); // number of words in italic corr. table
    /* word  9 */ uint16_t nl = read_u16(&p); // number of words in lig/kern table
    /* word 10 */ uint16_t nk = read_u16(&p); // number of words in kern table
    /* word 11 */ uint16_t ne = read_u16(&p); // number of words in ext. char table
    /* word 12 */ uint16_t np = read_u16(&p); // number of font parameter words

    if (p - buf != 24) abort();

#ifdef DEBUG_TFM
    printf("lf = %4u   %% length of the entire file, in words\n",        lf);
    printf("lh = %4u   %% length of the header data, in words\n",        lh);
    printf("bc = %4u   %% smallest character code in the font\n",        bc);
    printf("ec = %4u   %% largest character code in the font\n",         ec);
    printf("nw = %4u   %% number of words in the width table\n",         nw);
    printf("nh = %4u   %% number of words in the height table\n",        nh);
    printf("nd = %4u   %% number of words in the depth table\n",         nd);
    printf("ni = %4u   %% number of words in the italic correction table\n",   ni);
    printf("nl = %4u   %% number of words in the lig/kern table\n",        nl);
    printf("nk = %4u   %% number of words in the kern table\n",          nk);
    printf("ne = %4u   %% number of words in the extensible character table\n",  ne);
    printf("np = %4u   %% number of font parameter words\n",           np);
#endif

    int expected_len = 6+lh+(ec - bc +1)+nw+nh+nd+ni+nl+nk+ne+np;
    if (expected_len != lf)
    {
      fprintf(stderr, "length = %d, expected %d\n", lf, expected_len);
      FAIL("Inconsistent length values");
    }
    if (lh < 2)
      FAIL("Header is too small");
    if (bc >= ec || ec > 255 || ne > 256)
      FAIL("Character codes out of range");

    size_t remainder_size = 4 * (lf - 6);
    buffer = fz_malloc(ctx, remainder_size);
    if (fz_read(ctx, stm, buffer, remainder_size) != remainder_size)
      FAIL("Cann,t read file body");

    t = fz_malloc_struct(ctx, tex_tfm);
    t->buffer = buffer;
    t->first_char = bc;
    t->last_char = ec;

    // Read header prefix
    t->checksum = decode_u32(buffer);
    t->design_size = fixed_make(decode_u32(buffer + 4));

    tex_tfm_read_tables(t, buffer + lh * 4, nw, nh, nd, ni);
    tex_tfm_read_parameters(t->params, np, buffer + 4 * (lf - 6 - np));
  }
  fz_catch(ctx)
  {
    if (buffer)
      fz_free(ctx, buffer);
    if (t)
      fz_free(ctx, t);
    fz_rethrow(ctx);
  }

  return t;
}

void tex_tfm_free(fz_context *ctx, tex_tfm *t)
{
  fz_free(ctx, t->buffer);
  fz_free(ctx, t);
}
