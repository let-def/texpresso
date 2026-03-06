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

static int is_subsequent(int c)
{
  return is_initial(c) || is_digit(c);
}

const sexp_parser initial_sexp_parser = {
    .state = P_IDLE,
    .number = 0.0,
};

const char *sexp_parse(fz_context *ctx, sexp_parser *cp, vstack *stack, const
                       char *input, const char *limit)
{
  while (input < limit)
  {
    const char *begin = input;
    switch (cp->state)
    {
      case P_IDLE:
        while (input < limit)
        {
          char c = *input;
          input += 1;
          if (is_ws(c))
            continue;
          else if (c == '(')
            vstack_begin_array(ctx, stack);
          else if (c == ')')
          {
            vstack_end_array(ctx, stack);
            if (vstack_at_top_level(stack))
              return input;
          }
          else if (c == '"')
          {
            vstack_begin_string(ctx, stack);
            cp->state = P_STRING;
          }
          else if (is_digit(c))
          {
            cp->number = c - '0';
            cp->state = P_POS_NUMBER;
          }
          else if (c == '+')
          {
            cp->number = 0;
            cp->state = P_POS_NUMBER;
          }
          else if (c == '-')
          {
            cp->number = 0;
            cp->state = P_NEG_NUMBER;
          }
          else if (is_initial(c))
          {
            vstack_begin_name(ctx, stack);
            vstack_push_char(ctx, stack, c);
            cp->state = P_IDENT;
          }
          else
            fz_throw(ctx, 0, "sexp parser: unexpected character %C\n", c);
          break;
        }
        break;

      case P_IDENT:
        while (input < limit && is_subsequent(*input))
          input += 1;
        if (begin != input)
          vstack_push_chars(ctx, stack, begin, input - begin);
        if (input < limit)
        {
          vstack_end_name(ctx, stack);
          cp->state = P_IDLE;
        }
        break;

      case P_POS_NUMBER:
      case P_NEG_NUMBER:
        while (input < limit && is_digit(*input))
        {
          cp->number = cp->number * 10.0 + (*input - '0');
          input += 1;
        }
        if (input >= limit)
          break;
        if (*input != '.')
        {
          float n = cp->number;
          n = (cp->state == P_POS_NUMBER) ? n : -n;
          vstack_push_number(ctx, stack, n);
          cp->state = P_IDLE;
          break;
        }

        input += 1;
        cp->state =
            (cp->state == P_POS_NUMBER) ? P_POS_NUMBER_FRAC : P_NEG_NUMBER_FRAC;
        cp->frac = 0.1;

      case P_POS_NUMBER_FRAC:
      case P_NEG_NUMBER_FRAC:
        while (input < limit && is_digit(*input))
        {
          cp->number = cp->number + cp->frac * (*input - '0');
          cp->frac /= 10.0;
          input += 1;
        }
        if (input < limit)
        {
          float n = cp->number;
          n = (cp->state == P_POS_NUMBER_FRAC) ? n : -n;
          vstack_push_number(ctx, stack, n);
          cp->state = P_IDLE;
        }
        break;

      case P_STRING_ESCAPE:
        switch (*input)
        {
          case 'n':
            vstack_push_char(ctx, stack, '\n');
            break;
          case 'r':
            vstack_push_char(ctx, stack, '\r');
            break;
          case 't':
            vstack_push_char(ctx, stack, '\t');
            break;
          case '\t': case ' ': case '\n': case '\r':
            break;
          default:
            if (is_octal(*input))
            {
              cp->state = P_STRING_OCTAL1;
              cp->octal = (*input - '0');
              input += 1;
              continue;
            }
            else
              vstack_push_char(ctx, stack, *input);
            break;
        }
        input += 1;
        cp->state = P_STRING;
        if (input >= limit)
          break;
        begin = input;

      case P_STRING:
        while (input < limit && *input != '"' && *input != '\\')
          input += 1;
        if (begin != input)
          vstack_push_chars(ctx, stack, begin, input - begin);
        if (input < limit)
        {
          if (*input == '\\')
            cp->state = P_STRING_ESCAPE;
          else /* *input == '"' */
          {
            vstack_end_string(ctx, stack);
            cp->state = P_IDLE;
          }
          input += 1;
        }
        break;

      case P_STRING_OCTAL1:
        if (!is_octal(*input))
        {
          vstack_push_char(ctx, stack, cp->octal);
          cp->state = P_STRING;
          break;
        }
        cp->octal = cp->octal * 8 + (*input - '0');
        input += 1;
        if (input == limit)
        {
          cp->state = P_STRING_OCTAL2;
          break;
        }

      case P_STRING_OCTAL2:
        if (is_octal(*input))
        {
          cp->octal = cp->octal * 8 + (*input - '0');
          input += 1;
        }
        vstack_push_char(ctx, stack, cp->octal);
        cp->state = P_STRING;
        break;
    }
  }
  return NULL;
}
