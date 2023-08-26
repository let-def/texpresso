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

#include "sexp_parser.h"

#include <stdio.h>
#include <string.h>
#include "vstack.h"

enum json_parser_state
{
  P_JSON_IDLE,
  P_JSON_NAME,
  P_JSON_STRING,
  P_JSON_STRING_ESCAPE,
  P_JSON_STRING_U1,
  P_JSON_STRING_U2,
  P_JSON_STRING_U3,
  P_JSON_STRING_U4,
};

typedef struct
{
  enum json_parser_state state;
  union
  {
    int codepoint;
    struct
    {
      float number;
      float frac;
    };
  };
} json_parser;

static int is_ws(int c)
{
  return (c == ' ') || (c == '\n') || (c == '\r') || (c == '\t');
}

static int is_initial(int c)
{
  return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '!' ||
          c == '$' || c == '%' || c == '&' || c == '*' || c == '/' ||
          c == ':' || c == '<' || c == '=' || c == '>' || c == '?' ||
          c == '_' || c == '^' || c == '-' || c == '+');
}

static int is_digit(int c)
{
  return (c >= '0') && (c <= '9');
}

static int is_octal(int c)
{
  return (c >= '0') && (c <= '7');
}

static int as_hex(int c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + c - 'a';
  if (c >= 'A' && c <= 'F')
    return 10 + c - 'A';
  return -1;
}

static int is_subsequent(int c)
{
  return is_initial(c) || is_digit(c);
}

const sexp_parser initial_sexp_parser = {
    .state = P_IDLE,
    .number = 0.0,
};

const char *json_parse(fz_context *ctx, json_parser *cp, vstack *stack, const
                       char *input, const char *limit)
{
  while (input < limit)
  {
    const char *begin = input;
    switch (cp->state)
    {
      case P_JSON_IDLE:
        while (is_ws(*input))
        {
          input++;
          if (!(input < limit))
            return NULL;
        }
        if (is_digit(*input))
        {

        } else if (is_alpha(*input))
        {

        } else
        {
          switch (*input)
          {
            case '"':
            case '{':
            case '[':

            default:
          }
        }
        break;

      case P_JSON_STRING:
        do
        {
          switch (*input)
          {
            case '"':
              vstack_end_string(ctx, stack);
              input += 1;
              cp->state = P_JSON_IDLE;
              break;
          }

        } while (input < limit);
        break;

      case P_JSON_STRING_ESCAPE:
        switch (*input)
        {
          case '"': case '\\': case '/':
            vstack_push_char(ctx, stack, *input);
            break;

          case 'b': vstack_push_char(ctx, stack, '\b'); break;
          case 'f': vstack_push_char(ctx, stack, '\f'); break;
          case 'n': vstack_push_char(ctx, stack, '\n'); break;
          case 'r': vstack_push_char(ctx, stack, '\r'); break;
          case 't': vstack_push_char(ctx, stack, '\t'); break;

          case 'u':
            cp->state = P_JSON_STRING_U1;
            cp->codepoint = 0;
            input++;
            continue;

          default:
            abort();
        }
        cp->state = P_JSON_STRING;
        input++;
        continue;

      case P_JSON_STRING_U1:
      case P_JSON_STRING_U2:
      case P_JSON_STRING_U3:
      case P_JSON_STRING_U4:
        do
        {
          int c = as_hex(*input);
          if (c == -1) abort();
          cp->codepoint = (cp->codepoint << 4) | c;
          input++;
          if (cp->state == P_JSON_STRING_U4)
          {
            cp->state = P_JSON_STRING;
            break;
          }
          else
            cp->state += 1;
        } while (input < limit);
        break;
    }
  }
  return NULL;
}
