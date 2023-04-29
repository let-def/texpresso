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

#ifndef SEXP_PARSER_H_
#define SEXP_PARSER_H_

#include "../dvi/vstack.h"

enum sexp_parser_state
{
  P_IDLE,
  P_IDENT,
  P_POS_NUMBER,
  P_NEG_NUMBER,
  P_POS_NUMBER_FRAC,
  P_NEG_NUMBER_FRAC,
  P_STRING,
  P_STRING_ESCAPE,
  P_STRING_OCTAL1,
  P_STRING_OCTAL2,
};

typedef struct
{
  enum sexp_parser_state state;
  union
  {
    int octal;
    struct
    {
      float number;
      float frac;
    };
  };
} sexp_parser;

extern const sexp_parser initial_sexp_parser;

const char *sexp_parse(fz_context *ctx, sexp_parser *cp, vstack *stack, const
                       char *input, const char *limit);

#endif // SEXP_PARSER_H_
