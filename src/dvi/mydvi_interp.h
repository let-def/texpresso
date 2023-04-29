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

#ifndef DVI_INTERP_H
#define DVI_INTERP_H

#include "mydvi.h"

// DVI interpreter

const char *dvi_opname(uint8_t op);

int dvi_preamble_size(const uint8_t *buf, int len);
bool dvi_preamble_parse(fz_context *ctx, dvi_context *dc, dvi_state *st, const uint8_t *buf);

int dvi_instr_size(const uint8_t *buf, int len, enum dvi_version version);
bool dvi_interp_sub(fz_context *ctx, dvi_context *dc, dvi_state *st, const uint8_t *buf);
bool dvi_interp(fz_context *ctx, dvi_context *dc, const uint8_t *buf);
void dvi_interp_init(fz_context *ctx, dvi_context *dc, const uint8_t *bop, int len);
int dvi_interp_bop(const uint8_t *bop, int len, float *width, float *height, bool *landscape);

// DVI primitives

double dvi_scale(dvi_state *st, fixed_t dim);
void dvi_exec_char(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t c, bool set);
bool dvi_exec_push(fz_context *ctx, dvi_context *dc, dvi_state *st);
bool dvi_exec_pop(fz_context *ctx, dvi_context *dc, dvi_state *st);
void dvi_exec_fnt_num(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t f);
void dvi_exec_rule(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t w, uint32_t h);
bool dvi_exec_fnt_def(fz_context *ctx, dvi_context *dc, dvi_state *st,
                 uint32_t f, uint32_t c, uint32_t s, uint32_t d,
                 const char *path, size_t pathlen, const char *name, size_t namelen);
bool dvi_exec_bop(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t c[10], uint32_t p);
void dvi_exec_eop(fz_context *ctx, dvi_context *dc, dvi_state *st);
bool dvi_exec_pre(fz_context *ctx, dvi_context *dc, dvi_state *st,
                  uint8_t i, uint32_t num, uint32_t den, uint32_t mag,
                  const char *comment, size_t len);
void dvi_exec_xdvfontdef(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t fontnum,
                         const char *filename, int filename_len, int index, dvi_xdvfontspec spec);
void dvi_exec_xdvglyphs(fz_context *ctx, dvi_context *dc, dvi_state *st, fixed_t width,
                int char_count, uint16_t *chars,
                int num_glyphs, fixed_t *dx, fixed_t dy0, fixed_t *dy, uint16_t *glyphs);

// Specials

bool dvi_exec_special(fz_context *ctx, dvi_context *dc, dvi_state *st, const char *ptr, const char *lim);
bool dvi_init_special(fz_context *ctx, dvi_context *dc, dvi_state *st, const char *ptr, const char *lim);
void dvi_prescan_special(const char *ptr, const char *lim, float *width, float *height, bool *landscape);

#endif /*!DVI_INTERP_H*/
