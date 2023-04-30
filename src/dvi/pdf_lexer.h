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

#ifndef PDF_LEXER_H_
#define PDF_LEXER_H_

#include "vstack.h"

enum PDF_OP
{
  PDF_NONE,
  PDF_OP_w,
  PDF_OP_J,
  PDF_OP_j,
  PDF_OP_M,
  PDF_OP_d,
  PDF_OP_ri,
  PDF_OP_i,
  PDF_OP_gs,
  PDF_OP_q,
  PDF_OP_Q,
  PDF_OP_cm,
  PDF_OP_m,
  PDF_OP_l,
  PDF_OP_c,
  PDF_OP_v,
  PDF_OP_y,
  PDF_OP_h,
  PDF_OP_re,
  PDF_OP_S,
  PDF_OP_s,
  PDF_OP_f,
  PDF_OP_F,
  PDF_OP_f_star,
  PDF_OP_B,
  PDF_OP_B_star,
  PDF_OP_b,
  PDF_OP_b_star,
  PDF_OP_n,
  PDF_OP_W,
  PDF_OP_W_star,
  PDF_OP_BT,
  PDF_OP_ET,
  PDF_OP_Tc,
  PDF_OP_Tw,
  PDF_OP_Tz,
  PDF_OP_TL,
  PDF_OP_Tf,
  PDF_OP_Tr,
  PDF_OP_Ts,
  PDF_OP_Td,
  PDF_OP_TD,
  PDF_OP_Tm,
  PDF_OP_T_star,
  PDF_OP_Tj,
  PDF_OP_TJ,
  PDF_OP_squote,
  PDF_OP_dquote,
  PDF_OP_d0,
  PDF_OP_d1,
  PDF_OP_CS,
  PDF_OP_cs,
  PDF_OP_SC,
  PDF_OP_sc,
  PDF_OP_SCN,
  PDF_OP_scn,
  PDF_OP_G,
  PDF_OP_g,
  PDF_OP_RG,
  PDF_OP_rg,
  PDF_OP_K,
  PDF_OP_k,
  PDF_OP_sh,
  PDF_OP_Do,
  PDF_OP_MP,
  PDF_OP_DP,
  PDF_OP_BMC,
  PDF_OP_BDC,
  PDF_OP_EMC,
  PDF_OP_BX,
  PDF_OP_EX,
};

const char *
pdf_op_name(enum PDF_OP op);

enum PDF_OP
pdf_parse_command(fz_context *ctx, vstack *t, const char **cur, const char *lim);

#endif // PDF_LEXER_H_
