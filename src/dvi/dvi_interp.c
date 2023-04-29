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

#include "mydvi.h"
#include "mydvi_interp.h"
#include "mydvi_opcodes.h"
#include "intcodec.h"

#define case4(I, n) case I##1: case I##2: case I##3: case I##4: n = op - I##1 + 1;
#define PRINTF_DEBUG 0

#define CHECK_LEN(n) \
    do { if (len <= (n)) return -(n+1); } while(0)

int dvi_preamble_size(const uint8_t *buf, int len)
{
  CHECK_LEN(0);
  if (buf[0] != PRE)
    return 0;
  CHECK_LEN(15);
  return 15 + buf[14];
}

int dvi_instr_size(const uint8_t *buf, int len, enum dvi_version version)
{
  CHECK_LEN(0);

  enum dvi_opcode op = buf[0];

  if (op <= SET_CHAR_127)
    return 1;

  if (op >= FNT_NUM_0 && op <= FNT_NUM_63)
    return 1;

  switch (op)
  {
#define INSTR4(F,V) F(SET,V) F(PUT,V) F(RIGHT,V) F(DOWN,V) F(FNT,V) F(W,V) F(X,V) F(Y,V) F(Z,V)
#define CASE(I,N) case I##N:

    INSTR4(CASE,1) return 2;
    INSTR4(CASE,2) return 3;
    INSTR4(CASE,3) return 4;
    INSTR4(CASE,4) return 5;

#undef INSTR4
#undef CASE

    case SET_RULE: case PUT_RULE:
    return 9;

    case NOP: case EOP:
    case PUSH: case POP:
    case W0: case X0: case Y0: case Z0:
    case PADDING:
    case BEGIN_REFLECT: case END_REFLECT:
    return 1;

    case BOP:
    return 45;

    case XXX1:
    CHECK_LEN(1);
    return 2 + decode_u8(buf + 1);

    case XXX2:
    CHECK_LEN(2);
    return 3 + decode_u16(buf + 1);

    case XXX3:
    CHECK_LEN(3);
    return 4 + decode_u24(buf + 1);

    case XXX4:
    CHECK_LEN(4);
    return 5 + decode_u32(buf + 1);

    case FNT_DEF1: case FNT_DEF2: case FNT_DEF3: case FNT_DEF4:
    {
      int offset = 14 + op - FNT_DEF1;
      CHECK_LEN(offset + 1);
      return 2 + offset + decode_u16(buf + offset);
    }

    case PRE:
    return dvi_preamble_size(buf, len);

    case POST:
    return 29;

    case POST_POST:
    return 6;

    // https://tex.stackexchange.com/questions/496061/syntax-and-semantics-of-xdv-commands-xetex

    case XDV_NATIVE_FONT_DEF:
    {
      CHECK_LEN(11);
      uint16_t flags = decode_u16(buf + 9);
      uint8_t psname_len = buf[11];

      int size = 16 + psname_len;

      // XDV_FLAG_VERTICAL = 0x0100,
      // XDV_FLAG_COLORED  = 0x0200,
      // XDV_FLAG_EXTEND   = 0x1000,
      // XDV_FLAG_SLANT    = 0x2000,
      // XDV_FLAG_EMBOLDEN = 0x4000,

      if (flags & ~XDV_FLAG_ALL)
        return 0;
      if (flags & XDV_FLAG_COLORED)
        size += 4;
      if (flags & XDV_FLAG_EXTEND)
        size += 4;
      if (flags & XDV_FLAG_SLANT)
        size += 4;
      if (flags & XDV_FLAG_EMBOLDEN)
        size += 4;
      if (flags & XDV_FLAG_VARIATIONS)
      {
        if (version != DVI_XDV5)
          return 0;

        CHECK_LEN(size + 2);

        uint16_t num_variations = decode_u16(buf + size);
        size += 2;
        size += num_variations * 4;
      }

      return size;
    }

    case XDV_GLYPHS:
    {
      CHECK_LEN(6);
      uint16_t n = decode_u16(buf + 5);
      return 7 + 10 * n;
    }

    case XDV_TEXT_GLYPHS:
    {
      CHECK_LEN(3);
      int size = 3;
      uint16_t l = decode_u16(buf + 1);
      size += 2 * l;
      size += 4;
      CHECK_LEN(size + 2);
      uint16_t n = decode_u16(buf + size);
      size += 2;
      size += 10 * n;
      return size;
    }

    /*case PTEXDIR:*/

    default:
    return 0;
  }
}

#define APP4(F, A1, A2, A3, A4) F(A1) F(A2) F(A3) F(A4)
#define PRINT(I) case I: return #I;
#define PRINT4(I) APP4(PRINT, I##1, I##2, I##3, I##4)

const char *dvi_opname(uint8_t op)
{
  if (op <= SET_CHAR_127) return "SET_CHAR";
  if (op >= FNT_NUM_0 && op <= FNT_NUM_63) return "FNT_NUM";
  switch (op)
  {
    PRINT4(W)   PRINT4(X)   PRINT4(Y)       PRINT4(Z)
    PRINT4(PUT) PRINT4(SET) PRINT4(RIGHT)   PRINT4(DOWN)
    PRINT4(FNT) PRINT4(XXX) PRINT4(FNT_DEF)
    PRINT(SET_RULE)
    PRINT(PUT_RULE)
    PRINT(NOP)
    PRINT(BOP)
    PRINT(EOP)
    PRINT(PUSH)
    PRINT(POP)
    PRINT(PRE)
    PRINT(POST)
    PRINT(POST_POST)
    PRINT(PADDING)
    PRINT(BEGIN_REFLECT)
    PRINT(END_REFLECT)
    PRINT(XDV_NATIVE_FONT_DEF)
    PRINT(XDV_GLYPHS)
    PRINT(XDV_TEXT_GLYPHS)
    PRINT(PTEXDIR)
    default: return "(unknown bytecode)";
  };
}

#undef APP4
#undef PRINT
#undef PRINT4

bool dvi_preamble_parse(fz_context *ctx, dvi_context *dc, dvi_state *st, const uint8_t *buf)
{
  enum dvi_opcode op = read_u8(&buf);
  if (op != PRE)
  {
    fprintf(stderr, "dvi_parse_preamble: invalid opcode (expecting PRE)\n");
    return 0;
  }
  uint8_t  i   = read_u8(&buf);
  uint32_t num = read_u32(&buf);
  uint32_t den = read_u32(&buf);
  uint32_t mag = read_u32(&buf);
  uint8_t  len = read_u8(&buf);
  const char *comment = (const char *)buf;
  return dvi_exec_pre(ctx, dc, st, i, num, den, mag, comment, len);
}

bool dvi_interp_sub(fz_context *ctx, dvi_context *dc, dvi_state *st, const uint8_t *buf)
{
  enum dvi_opcode op = read_u8(&buf);

  if (op <= SET_CHAR_127)
  {
    dvi_exec_char(ctx, dc, st, op, true);
    return 1;
  }

  if (op >= FNT_NUM_0 && op <= FNT_NUM_63)
  {
    dvi_exec_fnt_num(ctx, dc, st, op - FNT_NUM_0);
    return 1;
  }

  int n;
  uint16_t xdv_chars_count = 0;
  uint16_t *xdv_chars = NULL;

  switch (op)
  {
    case4(SET, n)
    {
      dvi_exec_char(ctx, dc, st, decode_uB(buf, n), true);
      return 1;
    }

    case4(PUT, n)
    {
      dvi_exec_char(ctx, dc, st, decode_uB(buf, n), false);
      return 1;
    }

    case4(RIGHT, n)
    {
      st->registers.h += decode_sB(buf, n);
      return 1;
    }

    case W0:
    st->registers.h += st->registers.w;
    return 1;

    case4(W, n)
    {
      int32_t a = decode_sB(buf, n);
      st->registers.w = a;
      st->registers.h += a;
      return 1;
    }

    case X0:
    {
      st->registers.h += st->registers.x;
      return 1;
    }

    case4(X, n)
    {
      int32_t a = decode_sB(buf, n);
      st->registers.x = a;
      st->registers.h += a;
      return 1;
    }

    case4(DOWN, n)
    {
      int32_t d = decode_sB(buf, n);
      st->registers.v += d;
      return 1;
    }

    case Y0:
    {
      st->registers.v += st->registers.y;
      return 1;
    }

    case4(Y, n)
    {
      int32_t a = decode_sB(buf, n);
      st->registers.y = a;
      st->registers.v += a;
      return 1;
    }

    case Z0:
    st->registers.v += st->registers.z;
    return 1;

    case4(Z, n)
    {
      int32_t a = decode_sB(buf, n);
      st->registers.z = a;
      st->registers.v += a;
    }
    return 1;

    case4(FNT, n)
    {
      dvi_exec_fnt_num(ctx, dc, st, decode_uB(buf, n));
      return 1;
    }

    case SET_RULE: case PUT_RULE:
    {
      uint32_t h = decode_u32(buf);
      uint32_t w = decode_u32(buf + 4);
      dvi_exec_rule(ctx, dc, st, w, h);
      if (op == SET_RULE)
        st->registers.h += w;
      return 1;
    }

    case NOP:
    return 1;

    case EOP:
    dvi_exec_eop(ctx, dc, st);
    return 0;

    case PADDING:
    return 0;

    case PUSH:
    dvi_exec_push(ctx, dc, st);
    return 1;

    case POP:
    dvi_exec_pop(ctx, dc, st);
    return 1;


    case BEGIN_REFLECT: case END_REFLECT:
    return 0;

    case4(XXX, n)
    {
      uint32_t k = read_uB(&buf, n);
      const char *ptr = (const char *)buf;
      if (!dvi_exec_special(ctx, dc, st, ptr, ptr + k))
      {
        // fprintf(stderr, "interp: special failed: %.*s\n", k, ptr);
        return 0;
      }
      return 1;
    }

    case4(FNT_DEF, n)
    {
      uint32_t k = read_uB(&buf, n);
      uint32_t c = read_u32(&buf);
      uint32_t s = read_u32(&buf);
      uint32_t d = read_u32(&buf);
      uint8_t a  = read_u8(&buf);
      uint8_t l  = read_u8(&buf);
      const char *path = (const char *)buf;
      const char *name = path + a;
      return dvi_exec_fnt_def(ctx, dc, st, k, c, s, d, path, a, name, l);
    }

    case BOP:
    {
      uint32_t c[10], p;
      for (int i = 0; i < 10; ++i)
        c[i] = decode_u32(buf + i * 4);
      p = decode_u32(buf + 1 + 10 * 4);
      return dvi_exec_bop(ctx, dc, st, c, p);
    }

    case PRE:
    {
      fprintf(stderr, "dvi_interp: unexpected preamble\n");
      return 0;
    }

    case POST:
    return 0;

    case POST_POST:
    return 0;

    // https://tex.stackexchange.com/questions/496061/syntax-and-semantics-of-xdv-commands-xetex

    case XDV_NATIVE_FONT_DEF:
    {
      int32_t fontnum = read_s32(&buf);
      dvi_xdvfontspec spec;
      spec.size = read_fixed(&buf);
      spec.flags = read_u16(&buf);

      int filename_len = read_u8(&buf);

      uint8_t fmname_len = 0, stname_len = 0;
      if (st->version == DVI_XDV5)
      {
        fmname_len = read_u8(&buf);
        stname_len = read_u8(&buf);
      }

      const char *filename = (const char *)buf;
      buf += filename_len;

      int index = 0;

      if (st->version == DVI_XDV5)
        buf = buf + fmname_len + stname_len;
      else
        index = read_u32(&buf);

      if (spec.flags & XDV_FLAG_COLORED)
        spec.rgba = read_u32(&buf);
      if (spec.flags & XDV_FLAG_EXTEND)
        spec.extend = read_s32(&buf);
      if (spec.flags & XDV_FLAG_SLANT)
        spec.slant = read_s32(&buf);
      if (spec.flags & XDV_FLAG_EMBOLDEN)
        spec.bold = read_s32(&buf);
      if (spec.flags & XDV_FLAG_VARIATIONS)
      {
        uint16_t variations = read_s16(&buf);
        buf += variations * 4;
      }
      dvi_exec_xdvfontdef(ctx, dc, st, fontnum, filename, filename_len, index, spec);

      return 1;
    }

    case XDV_TEXT_GLYPHS:
    xdv_chars_count = read_s16(&buf);
    xdv_chars =
      DC_ALLOC(ctx, dc, uint16_t, xdv_chars_count);

    case XDV_GLYPHS:
    {
      fixed_t width = read_fixed(&buf);
      uint16_t num_glyphs = read_u16(&buf);
      fixed_t *dx, *dy;
      uint16_t *glyphs;

      dx = DC_ALLOC(ctx, dc, fixed_t, num_glyphs);
      dy = DC_ALLOC(ctx, dc, fixed_t, num_glyphs);
      for (int i = 0; i < num_glyphs; ++i)
      {
        dx[i] = read_fixed(&buf);
        dy[i] = read_fixed(&buf);
      }

      glyphs = DC_ALLOC(ctx, dc, uint16_t, num_glyphs);
      for (int i = 0; i < num_glyphs; ++i)
        glyphs[i] = read_u16(&buf);

      dvi_exec_xdvglyphs(ctx, dc, st, width, xdv_chars_count, xdv_chars, num_glyphs, dx, fixed_make(0), dy, glyphs);
      return 1;
    }

    case XDV_GLYPH_STRING:
    {
      fixed_t width = read_fixed(&buf);
      uint16_t num_glyphs = read_u16(&buf);
      fixed_t *dx, dy0;
      uint16_t *glyphs;

      dx = DC_ALLOC(ctx, dc, fixed_t, num_glyphs);
      for (int i = 0; i < num_glyphs; ++i)
        dx[i] = read_fixed(&buf);

      dy0 = read_fixed(&buf);

      glyphs = DC_ALLOC(ctx, dc, uint16_t, num_glyphs);
      for (int i = 0; i < num_glyphs; ++i)
        glyphs[i] = read_u16(&buf);

      dvi_exec_xdvglyphs(ctx, dc, st, width, 0, NULL, num_glyphs, dx, dy0, NULL, glyphs);
      return 1;
    }

    /*case PTEXDIR:*/

    default:
    return 0;
  }
}

bool dvi_interp(fz_context *ctx, dvi_context *dc, const uint8_t *buf)
{
  return dvi_interp_sub(ctx, dc, dvi_context_state(dc), buf);
}

int dvi_interp_bop(const uint8_t *buf, int len, float *width, float *height, bool *landscape)
{
  if (len == 0)
    return -1;
  if (*buf != BOP)
    abort();

  *width = 612;
  *height = 792;
  *landscape = false;

  int pos = 45; /*size of BOP*/

  while (pos < len && (
          (XXX1 <= buf[pos] && buf[pos] <= XXX4) ||
          (buf[pos] == PUSH) || (buf[pos] == POP)
        )
      )
  {
    if (buf[pos] == PUSH || buf[pos] == POP)
    {
      pos += 1;
      continue;
    }
    int n = (buf[pos] - XXX1) + 1;
    CHECK_LEN(pos + 1 + n);
    pos += 1;
    int size = decode_uB(buf+pos, n);
    pos += n;
    CHECK_LEN(pos + size);
    const char * ptr = (const char *)buf + pos;
    dvi_prescan_special(ptr, ptr + len, width, height, landscape);
    pos += size;
  }

  return pos;
}

void dvi_interp_init(fz_context *ctx, dvi_context *dc, const uint8_t *buf, int len)
{
  if (len > 0 && XXX1 <= buf[0] && buf[0] <= XXX4)
  {
    int n = (buf[0] - XXX1) + 1;
    if (1 + n > len) return;
    int size = decode_uB(buf+1, n);
    if (1 + n + size > len) return;
    const char *ptr = (const char *)buf + 1 + n;
    dvi_init_special(ctx, dc, dvi_context_state(dc), ptr, ptr + len);
  }
}
