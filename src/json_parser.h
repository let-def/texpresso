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

#ifndef JSON_PARSER_H_
#define JSON_PARSER_H_

#include "../dvi/vstack.h"

enum json_parser_state
{
  P_JSON_ELEMENT,
  P_JSON_STRING,
  P_JSON_STRING_ESCAPE,
  P_JSON_STRING_U1,
  P_JSON_STRING_U2,
  P_JSON_STRING_U3,
  P_JSON_STRING_U4,
  P_JSON_OBJECT,
  P_JSON_AFTER_ELEMENT,
  P_JSON_AFTER_NAME,
  P_JSON_INTEGER_SIGN,
  P_JSON_INTEGER_DIGITS,
  P_JSON_EXPONENT,
  P_JSON_EXPONENT_SIGN,
  P_JSON_EXPONENT_DIGITS,
  P_JSON_FRACTION,
  P_JSON_NULL_N,
  P_JSON_NULL_NU,
  P_JSON_NULL_NUL,
  P_JSON_TRUE_T,
  P_JSON_TRUE_TR,
  P_JSON_TRUE_TRU,
  P_JSON_FALSE_F,
  P_JSON_FALSE_FA,
  P_JSON_FALSE_FAL,
  P_JSON_FALSE_FALS,
};

typedef struct
{
  enum json_parser_state state;
  union
  {
    int codepoint;
    struct
    {
      int sign, exp_sign;
      float num, frac, exp;
    };
  };
} json_parser;

extern const json_parser initial_json_parser;

const char *json_parse(fz_context *ctx, json_parser *cp, vstack *stack, const
                       char *input, const char *limit);

#endif // JSON_PARSER_H_
