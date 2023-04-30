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
#include <stdbool.h>
#include <stdio.h>
#include "pdf_lexer.h"

typedef const char *cursor_t;

/*!re2c

re2c:yyfill:enable = 0;
re2c:eof = -1;
re2c:api = custom;
re2c:api:style = free-form;
re2c:define:YYCURSOR    = *cur;
re2c:define:YYCTYPE     = int;
re2c:define:YYLESSTHAN  = "lim <= *cur";
re2c:define:YYPEEK      = "*cur < lim ? **cur : -1";
re2c:define:YYSKIP      = "++(*cur);";
re2c:define:YYBACKUP    = "mar = *cur;";
re2c:define:YYRESTORE   = "*cur = mar;";
re2c:define:YYSTAGP     = "@@{tag} = *cur;";
re2c:define:YYSTAGN     = "@@{tag} = NULL;";
re2c:define:YYSHIFT     = "*cur += @@{shift};";
re2c:define:YYSHIFTSTAG = "@@{tag} += @@{shift};";
*/
/*!stags:re2c format = 'cursor_t @@;\n'; */

/*!re2c

ws      = [\x20\r\n\t\x0c\x00];
delim   = ('[' | ']' | [()/%><]);
eol     = ('\r' | '\n' | '\r\n');
nat     = [0-9]+;
int     = [+-]? nat;
float   = [+-]? (nat | nat "." nat? | nat? "." nat);
hex     = [0-9A-Fa-f];
*/

static int
pnat(cursor_t ptr, cursor_t lim)
{
  int result = 0;
  for (; ptr < lim && *ptr >= '0' && *ptr <= '9'; ptr += 1)
    result = result * 10 + (*ptr - '0');
  return result;
}

static float
pfloat(cursor_t ptr, cursor_t lim)
{
  int neg = 0;
  double result = 0;
  if (ptr < lim && *ptr == '-')
  {
    neg = 1;
    ptr += 1;
  }
  for (; ptr < lim && *ptr >= '0' && *ptr <= '9'; ptr += 1)
    result = result * 10 + (*ptr - '0');
  if (ptr < lim  && *ptr == '.')
  {
    ptr += 1;
    double pos = 0.1;
    for (; ptr < lim && *ptr >= '0' && *ptr <= '9'; ptr += 1)
    {
      result += pos * (*ptr - '0');
      pos /= 10;
    }
  }

  return neg ? -result : result;
}

const char *
pdf_op_name(enum PDF_OP op)
{
  switch (op)
  {
    case PDF_NONE:
      return "NONE";
#define CASE(name, str) case PDF_OP_##name: return "PDF_" str
      CASE(w,  "w");
      CASE(J, "J");
      CASE(j, "j");
      CASE(M, "M");
      CASE(d, "d");
      CASE(ri, "ri");
      CASE(i, "i");
      CASE(gs, "gs");
      CASE(q, "q");
      CASE(Q, "Q");
      CASE(cm, "cm");
      CASE(m, "m");
      CASE(l, "l");
      CASE(c, "c");
      CASE(v, "v");
      CASE(y, "y");
      CASE(h, "h");
      CASE(re, "re");
      CASE(S, "S");
      CASE(s, "s");
      CASE(f, "f");
      CASE(F, "F");
      CASE(f_star, "f*");
      CASE(B, "B");
      CASE(B_star, "B*");
      CASE(b, "b");
      CASE(b_star, "b*");
      CASE(n, "n");
      CASE(W, "W");
      CASE(W_star, "W*");
      CASE(BT, "BT");
      CASE(ET, "ET");
      CASE(Tc, "Tc");
      CASE(Tw, "Tw");
      CASE(Tz, "Tz");
      CASE(TL, "TL");
      CASE(Tf, "Tf");
      CASE(Tr, "Tr");
      CASE(Ts, "Ts");
      CASE(Td, "Td");
      CASE(TD, "TD");
      CASE(Tm, "Tm");
      CASE(T_star, "T*");
      CASE(Tj, "Tj");
      CASE(TJ, "TJ");
      CASE(squote, "'");
      CASE(dquote, "''");
      CASE(d0, "d0");
      CASE(d1, "d1");
      CASE(CS, "CS");
      CASE(cs, "cs");
      CASE(SC, "SC");
      CASE(sc, "sc");
      CASE(SCN, "SCN");
      CASE(scn, "scn");
      CASE(G, "G");
      CASE(g, "g");
      CASE(RG, "RG");
      CASE(rg, "rg");
      CASE(K, "K");
      CASE(k, "k");
      CASE(sh, "sh");
      CASE(Do, "Do");
      CASE(MP, "MP");
      CASE(DP, "DP");
      CASE(BMC, "BMC");
      CASE(BDC, "BDC");
      CASE(EMC, "EMC");
      CASE(BX, "BX");
      CASE(EX, "EX");
#undef CASE
    default:
      return "INVALID";
  }
}

static void vstack_push_hex(fz_context *ctx, vstack *t, int c0, int c1)
{
  if (c0 >= '0' && c0 <= '9')
    c0 = c0 - '0';
  else if (c0 >= 'a' && c0 <= 'z')
    c0 = 10 + c0 - 'a';
  else if (c0 >= 'A' && c0 <= 'Z')
    c0 = 10 + c0 - 'A';
  else
    abort();
  if (c1 >= '0' && c1 <= '9')
    c1 = c1 - '0';
  else if (c1 >= 'a' && c1 <= 'z')
    c1 = 10 + c1 - 'a';
  else if (c1 >= 'A' && c1 <= 'Z')
    c1 = 10 + c1 - 'A';
  else
    abort();

  vstack_push_char(ctx, t, c0 * 16 + c1);
}

static void
parse_hexstring(fz_context *ctx, vstack *t, cursor_t *cur, cursor_t lim)
{
  vstack_begin_hexstring(ctx, t);
  cursor_t mar, p0, p1;
  while (*cur < lim)
  {
    /*!re2c

    @p0 hex ws* @p1 hex
    {
      vstack_push_hex(ctx, t, *p0, *p1);
      continue;
    }

    @p0 hex ws* ">"
    {
      vstack_push_hex(ctx, t, *p0, '0');
      vstack_end_hexstring(ctx, t);
      return;
    }

    ">"
    { vstack_end_hexstring(ctx, t);
      return; }

    ws
    { continue; }

    .
    { goto err; }
    */
  }

err:
  fz_throw(ctx, 0, "parse_hexstring: unterminated hexstring");
}

static void
sync_string(fz_context *ctx, vstack *t, cursor_t sync, cursor_t cur)
{
  vstack_push_chars(ctx, t, sync, cur - sync);
}

static void
sync_char(fz_context *ctx, vstack *t, cursor_t sync, cursor_t cur, int c)
{
  sync_string(ctx, t, sync, cur);
  vstack_push_char(ctx, t, c);
}

static void
parse_string(fz_context *ctx, vstack *t, cursor_t *cur, cursor_t lim)
{
  int nesting = 1;
  cursor_t sync = *cur;

  vstack_begin_string(ctx, t);
  while (*cur < lim)
  {
    cursor_t p0 = *cur, p1;
    /*!re2c

    "("
    { nesting += 1; continue; }

    ")"
    { nesting -= 1;
      if (nesting > 0)
        continue;
      sync_string(ctx, t, sync, p0);
      vstack_end_string(ctx, t);
      return;
    }

    eol
    {
      sync_char(ctx, t, sync, p0, '\x0A');
      sync = *cur;
      continue;
    }

    "\\" @p1 ([()\\] | eol)
    {
      sync_string(ctx, t, sync, p0);
      sync = p1;
      continue;
    }

    "\\n" { sync_char(ctx, t, sync, p0, '\n'); sync = *cur; continue; }
    "\\r" { sync_char(ctx, t, sync, p0, '\r'); sync = *cur; continue; }
    "\\t" { sync_char(ctx, t, sync, p0, '\t'); sync = *cur; continue; }
    "\\b" { sync_char(ctx, t, sync, p0, '\b'); sync = *cur; continue; }
    "\\f" { sync_char(ctx, t, sync, p0, '\f'); sync = *cur; continue; }

    "\\" @p1 [0-7]{1,3}
    { int c = 0;
      for (; p1 < *cur; p1++)
        c = c * 8 + (*p1 - '0');
      sync_char(ctx, t, sync, p0, c);
      sync = *cur;
      continue;
    }

    .
    { continue; }

    */
  }
  fz_throw(ctx, 0, "parse_string: unterminated string");
}

static void
parse_name(fz_context *ctx, vstack *t, cursor_t *cur, cursor_t lim)
{
  vstack_begin_name(ctx, t);

  cursor_t sync = *cur;
  while (*cur < lim)
  {
    cursor_t mar, p0 = *cur;
    /*!re2c

      '#00' | '#'
      { fz_throw(ctx, 0, "parse_name: NULL byte"); }

      '#' [0-9a-fA-F]{2}
      {
        sync_string(ctx, t, sync, p0);
        sync = *cur;
        vstack_push_hex(ctx, t, p0[1], p0[2]);
      }

      ws | delim
      {
        *cur = p0;
        goto done;
      }

      .
      { continue; }
    */
  }

done:
  sync_string(ctx, t, sync, *cur);
  vstack_end_name(ctx, t);
}

enum PDF_OP
pdf_parse_command(fz_context *ctx, vstack *t, cursor_t *cur, cursor_t lim)
{
  cursor_t mar, p0, p1;

  while (*cur < lim)
  {
    /*!re2c

    ws
    { continue; }

    "true"
    {
      vstack_push_bool(ctx, t, 1);
      continue;
    }

    "false"
    {
      vstack_push_bool(ctx, t, 0);
      continue;
    }

    "("
    {
      parse_string(ctx, t, cur, lim);
      continue;
    }

    "<"
    {
      parse_hexstring(ctx, t, cur, lim);
      continue;
    }

    "<<"
    {
      vstack_begin_dict(ctx, t);
      continue;
    }

    ">>"
    {
      vstack_end_dict(ctx, t);
      continue;
    }

    "["
    {
      vstack_begin_array(ctx, t);
      continue;
    }

    "]"
    {
      vstack_end_array(ctx, t);
      continue;
    }

    @p0 nat ws+ @p1 nat ws+ "R"
    {
      vstack_push_ref(ctx, t, pnat(p0, lim), pnat(p1, lim));
      continue;
    }

    "null"
    {
      vstack_push_null(ctx, t);
      continue;
    }

    "/"
    {
      parse_name(ctx, t, cur, lim);
      continue;
    }

    @p0 float
    {
      vstack_push_number(ctx, t, pfloat(p0, *cur));
      continue;
    }

    "BI"
    { fz_throw(ctx, 0, "parse_command: BI: TODO"); }

    "w"   { return PDF_OP_w;   }
    "J"   { return PDF_OP_J;   }
    "j"   { return PDF_OP_j;   }
    "M"   { return PDF_OP_M;   }
    "d"   { return PDF_OP_d;   }
    "ri"  { return PDF_OP_ri;  }
    "i"   { return PDF_OP_i;   }
    "gs"  { return PDF_OP_gs;  }
    "q"   { return PDF_OP_q;   }
    "Q"   { return PDF_OP_Q;   }
    "cm"  { return PDF_OP_cm;  }
    "m"   { return PDF_OP_m;   }
    "l"   { return PDF_OP_l;   }
    "c"   { return PDF_OP_c;   }
    "v"   { return PDF_OP_v;   }
    "y"   { return PDF_OP_y;   }
    "h"   { return PDF_OP_h;   }
    "re"  { return PDF_OP_re;  }
    "S"   { return PDF_OP_S;   }
    "s"   { return PDF_OP_s;   }
    "f"   { return PDF_OP_f;   }
    "F"   { return PDF_OP_F;   }
    "f*"  { return PDF_OP_f_star; }
    "B"   { return PDF_OP_B;   }
    "B*"  { return PDF_OP_B_star; }
    "b"   { return PDF_OP_b;   }
    "b*"  { return PDF_OP_b_star; }
    "n"   { return PDF_OP_n;   }
    "W"   { return PDF_OP_W;   }
    "W*"  { return PDF_OP_W_star; }
    "BT"  { return PDF_OP_BT;  }
    "ET"  { return PDF_OP_ET;  }
    "Tc"  { return PDF_OP_Tc;  }
    "Tw"  { return PDF_OP_Tw;  }
    "Tz"  { return PDF_OP_Tz;  }
    "TL"  { return PDF_OP_TL;  }
    "Tf"  { return PDF_OP_Tf;  }
    "Tr"  { return PDF_OP_Tr;  }
    "Ts"  { return PDF_OP_Ts;  }
    "Td"  { return PDF_OP_Td;  }
    "TD"  { return PDF_OP_TD;  }
    "Tm"  { return PDF_OP_Tm;  }
    "T*"  { return PDF_OP_T_star; }
    "Tj"  { return PDF_OP_Tj;  }
    "TJ"  { return PDF_OP_TJ;  }
    "'"   { return PDF_OP_squote; }
    "''"  { return PDF_OP_dquote; }
    "d0"  { return PDF_OP_d0;  }
    "d1"  { return PDF_OP_d1;  }
    "CS"  { return PDF_OP_CS;  }
    "cs"  { return PDF_OP_cs;  }
    "SC"  { return PDF_OP_SC;  }
    "sc"  { return PDF_OP_sc;  }
    "SCN" { return PDF_OP_SCN; }
    "scn" { return PDF_OP_scn; }
    "G"   { return PDF_OP_G;   }
    "g"   { return PDF_OP_g;   }
    "RG"  { return PDF_OP_RG;  }
    "rg"  { return PDF_OP_rg;  }
    "K"   { return PDF_OP_K;   }
    "k"   { return PDF_OP_k;   }
    "sh"  { return PDF_OP_sh;  }
    "Do"  { return PDF_OP_Do;  }
    "MP"  { return PDF_OP_MP;  }
    "DP"  { return PDF_OP_DP;  }
    "BMC" { return PDF_OP_BMC; }
    "BDC" { return PDF_OP_BDC; }
    "EMC" { return PDF_OP_EMC; }
    "BX"  { return PDF_OP_BX;  }
    "EX"  { return PDF_OP_EX;  }

    '' { fz_throw(ctx, 0, "parse_command: invalid input"); }
    */
  }

  return PDF_NONE;
}
