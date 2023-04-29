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
#include "mydvi_interp.h"
#include "fz_util.h"

#define color_params fz_default_color_params

static void output_fill_rect(fz_context *ctx, dvi_context *dc, dvi_state *st, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
  if (ctx && dc->dev)
  {
    float s = dc->scale;
    fz_path *path = fz_new_path(ctx);
    fz_rectto(ctx, path, x0 * s, - y0 * s, x1 * s, - y1 * s);
    fz_fill_path(ctx, dc->dev, path, 0, st->gs.ctm, fz_device_rgb(ctx),
                 st->gs.colors.fill, 1.0, color_params);
    fz_drop_path(ctx, path);
  }
}

static void output_debug_rect(fz_context *ctx, dvi_context *dc, dvi_state *st, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
  if (ctx && dc->dev)
  {
    fz_path *path = fz_new_path(ctx);
    fz_rectto(ctx, path, x0 * dc->scale, - y0 * dc->scale, x1 * dc->scale, - y1 * dc->scale);
    fz_stroke_path(ctx, dc->dev, path, &fz_default_stroke_state, st->gs.ctm,
                   fz_device_rgb(ctx), st->gs.colors.line, 0.8, color_params);
    fz_drop_path(ctx, path);
  }
}

void dvi_context_flush_text(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  if (dc->text)
  {
    if (!dc->dev)
      abort();
    fz_fill_text(ctx, dc->dev, dc->text, fz_identity, fz_device_rgb(ctx),
        st->gs.colors.fill, 1.0, color_params);
    fz_drop_text(ctx, dc->text);
    dc->text = NULL;
  }
}

static fz_text *get_text(fz_context *ctx, dvi_context *dc)
{
  if (!dc->text)
    dc->text = fz_new_text(ctx);
  return dc->text;
}

static dvi_fontdef *dvi_current_font(fz_context *ctx, dvi_state *st)
{
  return dvi_fonttable_get(ctx, st->fonts, st->f);
}

void dvi_exec_char(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t c, bool set)
{
  int debug = 0;

  // fprintf(stderr, "%s_char: %C = %u\n", set ? "set" : "put", c, c);

  dvi_fontdef *def = dvi_current_font(ctx, st);
  if (def->kind != TEX_FONT)
  {
    fprintf(stderr, "dvi_exec_char: expecting TeX font\n");
    abort();
  }

  dvi_font *font = def->tex_font.font;
  fixed_t scale_factor = def->tex_font.spec.scale_factor;
  if (def && font)
  {
    if (!font->fz && !font->vf)
    {
      fprintf(stderr, "No fz nor vf font for %s\n", font->name);
    }
    if (font->fz)
    {
      int u = -1;
      if (c >= 0 && c <= 255)
      {
        if (font->glyph_map)
          u = font->glyph_map[c];
        else
        {
          int *buf = fz_malloc_array(ctx, 256, int);
          if (!buf) abort();
          for (int i = 0; i < 256; ++i) buf[i] = -1;
          font->glyph_map = buf;
        }

        if (u == -1)
        {
          const char *name = NULL;
          if (font->enc)
            name = tex_enc_get(font->enc, c);
          if (name)
            u = fz_encode_character_by_glyph_name(ctx, font->fz, (const char *)name);
          else
            u = fz_encode_character(ctx, font->fz, c);
          font->glyph_map[c] = u;
        }
      }
      else
      {
        fprintf(stderr, "character out of bounds\n");
        u = fz_encode_character(ctx, font->fz, c);
      }

      if (dc->dev)
      {
        float s = dc->scale * scale_factor.value;
        fz_show_glyph(ctx, get_text(ctx, dc), font->fz,
                      fz_pre_scale(dvi_get_ctm(dc, st), s, s), u, c, 0, 0,
                      FZ_BIDI_LTR, FZ_LANG_UNSET);
      }
    }
    else if (font->vf)
    {
      dvi_state vfst;
      tex_vf_char *vfc = tex_vf_get(font->vf, c);
      if (vfc &&
          dvi_state_enter_vf(dc, &vfst, st, tex_vf_fonttable(font->vf), tex_vf_default_font(font->vf), scale_factor))
      {
        int pos = 0, dvi_length = vfc->dvi_length;
        const uint8_t *dvi = vfc->dvi;
        while (pos < dvi_length)
        {
          int size = dvi_instr_size(dvi + pos, dvi_length - pos, DVI_VF);
          if (size <= 0 || size > dvi_length - pos) break;
          //fprintf(stderr, "VF: %s (%d) at offset %d/%d\n", dvi_opname(dvi[pos]), size, pos, dvi_length);
          if (!dvi_interp_sub(ctx, dc, &vfst, dvi + pos))
          {
            fprintf(stderr, "VF: failed\n");
            break;
          }
          pos += size;
        }
      }
      else
        fprintf(stderr, "VirtualFont: cannot enter state (vfc: %p)\n", vfc);
      if (vfc && set)
      {
        st->registers.h += fixed_mul(vfc->width, scale_factor).value;
        return;
      }
    }
    if (set && font->tfm)
    {
      tex_tfm *tfm = font->tfm;
      fixed_t w = fixed_mul(tex_tfm_char_width(tfm, c), scale_factor);
      if (dc->sync.cb || debug)
      {
        float s = dc->scale * scale_factor.value;
        fixed_t h = tex_tfm_char_height(tfm, c);
        fixed_t d = tex_tfm_char_depth(tfm, c);
        if (dc->sync.cb)
          dc->sync.cb(dc->sync.cb_data, dc->sync.pos[0].file,
                     dc->sync.pos[0].line, c,
                      fz_pre_scale(dvi_get_ctm(dc, st), s, s),
                      w.value * dc->scale, h.value * s, d.value * s);
        if (debug)
        {
          h = fixed_mul(h, scale_factor);
          d = fixed_mul(d, scale_factor);
          fprintf(stderr, "setchar%u h:=%d+%d=%d\n", c, st->registers.h,
                  w.value, st->registers.h + w.value);
          fprintf(stderr, "  char: w:%dr, h:%dr, d:%dr\n", w.value, h.value,
                  d.value);
          fprintf(stderr, "  box: (%dr, %dr, %dr, %dr)\n", st->registers.h,
                  st->registers.v - h.value, st->registers.h + w.value,
                  st->registers.v + d.value);

          output_debug_rect(
              ctx, dc, st, st->registers.h, st->registers.v - h.value,
              st->registers.h + w.value, st->registers.v + d.value);
        }
      }
      if (set)
        st->registers.h += w.value;
    }
  }
}

bool dvi_exec_push(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  dvi_context_flush_text(ctx, dc, st);
  if (st->registers_stack.depth >= st->registers_stack.limit)
    return 0;
  st->registers_stack.base[st->registers_stack.depth] = st->registers;
  st->registers_stack.depth += 1;
  return 1;
}

bool dvi_exec_pop(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  dvi_context_flush_text(ctx, dc, st);
  if (st->registers_stack.depth == 0)
    return 0;
  st->registers_stack.depth -= 1;
  st->registers = st->registers_stack.base[st->registers_stack.depth];
  return 1;
}

void dvi_exec_fnt_num(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t f)
{
  (void)dc;
  if (!dvi_current_font(ctx, st))
    fprintf(stderr, "fnt_num: undefined font %u\n", f);
  st->f = f;
}

void dvi_exec_rule(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t w, uint32_t h)
{
  int32_t x = st->registers.h - st->gs.h;
  int32_t y = st->registers.v - st->gs.v;

  //fprintf(stderr, "rule: (%fpt, %fpt, %fpt, %fpt)\n", fx, fy, fw, fh);
  output_fill_rect(ctx, dc, st, x, y, x + w, y - h);
}

bool dvi_exec_fnt_def(fz_context *ctx, dvi_context *dc, dvi_state *st,
                 uint32_t f, uint32_t c, uint32_t s, uint32_t d,
                 const char *path, size_t pathlen, const char *name, size_t namelen)
{
  // fprintf(stderr, "fnt_def:\n");
  // fprintf(stderr, "  f: %u\n", f);
  // fprintf(stderr, "  c: %u\n", c);
  // fprintf(stderr, "  s: %f (%u)\n", fixed_double(fixed_make(s)), s);
  // fprintf(stderr, "  d: %f (%u)\n", fixed_double(fixed_make(d)), d);
  // fprintf(stderr, "  path: ");
  // fwrite(path, 1, pathlen, stderr);
  // fprintf(stderr, "\n");
  // fprintf(stderr, "  name: ");
  // fwrite(name, 1, namelen, stderr);
  // fprintf(stderr, "\n");
  dvi_fontdef *def = dvi_fonttable_get(ctx, st->fonts, f);
  if (def)
  {
    def->kind = TEX_FONT;
    def->tex_font.font = dvi_resmanager_get_tex_font(ctx, dc->resmanager, name, namelen);
    def->tex_font.spec.checksum = c;
    def->tex_font.spec.scale_factor = fixed_make(s);
    def->tex_font.spec.design_size = fixed_make(d);
  }

  return 1;
}

bool dvi_exec_bop(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t c[10], uint32_t p)
{
  // fprintf(stderr, "beginning_of_page\n");
  memset(&st->registers, 0, sizeof(dvi_registers));
  (void)dc;
  if (st->gs_stack.depth != 0)
  {
    fprintf(stderr, "beginning_of_page: transformation stack was not at empty\n");
    st->gs_stack.depth = 0;
  }
  if (st->registers_stack.depth != 0)
  {
    fprintf(stderr, "beginning_of_page: stack was not at empty\n");
    st->registers_stack.depth = 0;
  }
  (void)c;
  (void)p;
  return 1;
}

void dvi_exec_eop(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  dvi_context_flush_text(ctx, dc, st);
  // fprintf(stderr, "end_of_page\n");
}

bool dvi_exec_pre(fz_context *ctx, dvi_context *dc, dvi_state *st, uint8_t i, uint32_t num, uint32_t den, uint32_t mag, const char *comment, size_t len)
{
  (void)dc;
  fprintf(stderr, "pre:\n");
  fprintf(stderr, "  i: %u\n", i);
  fprintf(stderr, "  num: %u\n", num);
  fprintf(stderr, "  den: %u\n", den);
  fprintf(stderr, "  mag: %u\n", mag);
  fprintf(stderr, "  comment: %.*s\n", (int)len, comment);

  dc->scale = num/254000.0*72.27/den*mag/1000.0 * 800/803;

  return 1;
}

void dvi_exec_xdvfontdef(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t fontnum,
    const char *name, int name_len, int index, dvi_xdvfontspec spec)
{
  dvi_fontdef *def = dvi_fonttable_get(ctx, st->fonts, fontnum);
  if (def)
  {
    def->kind = XDV_FONT;
    def->xdv_font.font = dvi_resmanager_get_xdv_font(ctx, dc->resmanager, name, name_len, index);
    def->xdv_font.spec = spec;
  }
}

void dvi_exec_xdvglyphs(fz_context *ctx, dvi_context *dc, dvi_state *st, fixed_t width,
                int char_count, uint16_t *chars,
                int num_glyphs, fixed_t *dx, fixed_t dy0, fixed_t *dy, uint16_t *glyphs)
{
  //fprintf(stderr, "dvi_exec_xdvglyphs: width:%d, chars:%d, glyphs:%d\n", width.value, char_count, num_glyphs);
  dvi_fontdef *def = dvi_current_font(ctx, st);
  if (def->kind != XDV_FONT)
  {
    fprintf(stderr, "dvi_exec_xdvglyphs: expecting XDV font\n");
    abort();
  }
  fz_font *font = def->xdv_font.font;
  fixed_t size = def->xdv_font.spec.size;
  dvi_sync_cb *cb = dc->sync.cb;
  if (!dc->dev && !cb);
  else if (font)
  {
    float ds = dc->scale;
    float fs = size.value * ds;

    int32_t sh = st->registers.h - st->gs.h;
    int32_t sv = st->registers.v + dy0.value - st->gs.v;
    if (dc->dev)
    {
      fz_text *text = get_text(ctx, dc);
      for (int i = 0; i < num_glyphs; ++i)
      {
        int32_t h = sh + dx[i].value;
        int32_t v = dy ? sv + dy[i].value : sv;
        fz_matrix ctm =
            fz_pre_scale(fz_pre_translate(st->gs.ctm, h * ds, -v * ds), fs, fs);
        fz_show_glyph(ctx, text, font, ctm, glyphs[i], 0, 0, 0, FZ_BIDI_LTR,
                      FZ_LANG_UNSET);
      }
    }
    if (cb)
    {
      for (int i = 0; i < num_glyphs; ++i)
      {
        int32_t h = sh + dx[i].value;
        int32_t v = dy ? sv + dy[i].value : sv;
        fz_rect r =
          fz_bound_glyph(ctx, font, glyphs[i], fz_identity);
        fz_matrix ctm =
          fz_pre_scale(fz_pre_translate(st->gs.ctm, (h + r.x0 * size.value) * ds, -v * ds), fs, fs);
        cb(dc->sync.cb_data, dc->sync.pos[0].file, dc->sync.pos[0].line,
           (chars && i < char_count) ? chars[i] : ' ',
           ctm, (r.x1 - r.x0), r.y0, r.y1);
      }
    }
  }
  else
    fprintf(stderr, "dvi_exec_xdvglyphs: font not found\n");
  st->registers.h += width.value;
}
