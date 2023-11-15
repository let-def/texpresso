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

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "myabort.h"
#include "json_parser.h"

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

const json_parser initial_json_parser = {
    .state = P_JSON_ELEMENT,
};

static bool json_pop_context(json_parser *cp, vstack *stack)
{
  if (vstack_in_dict(stack) || vstack_in_array(stack))
  {
    cp->state = P_JSON_AFTER_ELEMENT;
    return 1;
  }
  cp->state = P_JSON_ELEMENT;
  return 0;
}

#define POP_CONTEXT(input) \
  if (!json_pop_context(cp, stack)) return (input);

const char *json_parse(fz_context *ctx, json_parser *cp, vstack *stack, const
                       char *input, const char *limit)
{
  while (input < limit)
  {
    switch (cp->state)
    {
      case P_JSON_ELEMENT:
        while (is_ws(*input))
        {
          input++;
          if (!(input < limit))
            return NULL;
        }
        switch (*input)
        {
          case '-':
          case '+':
          case '0': case '1': case '2': case '3': case '4':
          case '5': case '6': case '7': case '8': case '9':
            cp->state = P_JSON_INTEGER_SIGN;
            continue;
          case '"':
            vstack_begin_string(ctx, stack);
            cp->state = P_JSON_STRING;
            break;
          case '{':
            vstack_begin_dict(ctx, stack);
            cp->state = P_JSON_OBJECT;
            break;
          case '[':
            vstack_begin_array(ctx, stack);
            cp->state = P_JSON_ELEMENT;
            break;
          case ']':
            if (!vstack_in_array(stack))
              myabort();
            vstack_end_array(ctx, stack);
            POP_CONTEXT(input + 1);
            continue;
          case 't': cp->state = P_JSON_TRUE_T; break;
          case 'f': cp->state = P_JSON_FALSE_F; break;
          case 'n': cp->state = P_JSON_NULL_N; break;
          default:
            myabort();
        }
        input++;
        break;

      case P_JSON_STRING:
        do
        {
          switch (*input)
          {
            case '\\':
              cp->state = P_JSON_STRING_ESCAPE;
              input += 1;
              break;

            case '"':
              input += 1;
              if (vstack_in_name(stack))
              {
                vstack_end_name(ctx, stack);
                cp->state = P_JSON_AFTER_NAME;
              }
              else
              {
                vstack_end_string(ctx, stack);
                POP_CONTEXT(input);
              }
              break;

            default:
            {
              const char *begin = input;
              do {
                input += 1;
              } while (input < limit && *input != '\\' && *input != '"');
              vstack_push_chars(ctx, stack, begin, input - begin);
              break;
            }
          }
        } while (cp->state == P_JSON_STRING && input < limit);
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
            myabort();
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
          if (c == -1) myabort();
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

      case P_JSON_AFTER_NAME:
        while (is_ws(*input))
        {
          input++;
          if (!(input < limit))
            return NULL;
        }
        if (*input != ':')
          myabort();
        input += 1;
        cp->state = P_JSON_ELEMENT;
        break;

      case P_JSON_AFTER_ELEMENT:
        while (is_ws(*input))
        {
          input++;
          if (!(input < limit))
            return NULL;
        }
        switch (*input)
        {
          case ',':
            if (vstack_in_dict(stack))
              cp->state = P_JSON_OBJECT;
            else if (vstack_in_array(stack))
              cp->state = P_JSON_ELEMENT;
            else
              myabort();
            break;

          case '}':
            vstack_end_dict(ctx, stack);
            POP_CONTEXT(input + 1);
            break;

          case ']':
            vstack_end_array(ctx, stack);
            POP_CONTEXT(input + 1);
            break;

          default:
            myabort();
        }
        input++;
        break;

      case P_JSON_OBJECT:
        while (is_ws(*input))
        {
          input++;
          if (!(input < limit))
            return NULL;
        }
        switch (*input)
        {
          case '}':
            vstack_end_dict(ctx, stack);
            POP_CONTEXT(input + 1);
            break;
          case '"':
            vstack_begin_name(ctx, stack);
            cp->state = P_JSON_STRING;
            break;
          default:
            myabort();
        }
        input++;
        break;

      case P_JSON_INTEGER_SIGN:
        cp->state = P_JSON_INTEGER_DIGITS;
        cp->num = 0.0;
        cp->sign = 0;
        if (*input == '-')
        {
          cp->sign = 1;
          input++;
        }
        else if (*input == '+')
          input++;

      case P_JSON_INTEGER_DIGITS:
        while ((input < limit) && is_digit(*input))
        {
          cp->num = cp->num * 10 + (*input - '0');
          input++;
        }
        if (!(input < limit))
          return NULL;
        if (*input == '.')
        {
          cp->state = P_JSON_FRACTION;
          cp->frac = 0.1;
          input++;
          continue;
        }
        cp->state = P_JSON_EXPONENT;

      case P_JSON_EXPONENT:
        if (cp->sign)
          cp->num = -cp->num;
        if (*input == 'e' || *input == 'E')
        {
          input++;
          cp->state = P_JSON_EXPONENT_SIGN;
        } else
        {
          vstack_push_number(ctx, stack, cp->num);
          POP_CONTEXT(input);
        }
        continue;

      case P_JSON_EXPONENT_SIGN:
        cp->sign = 0;
        if (*input == '+')
          input++;
        else if (*input == '-')
        {
          cp->sign = 1;
          input++;
        }
        cp->exp = 0.0;
        cp->state = P_JSON_EXPONENT_DIGITS;
        continue;

      case P_JSON_EXPONENT_DIGITS:
        while ((input < limit) && is_digit(*input))
        {
          cp->exp = cp->exp * 10 + (*input - '0');
          input++;
        }
        if (!(input < limit))
          return NULL;
        float f = exp(cp->exp * log(10.0));
        if (cp->sign) f = -f;
        vstack_push_number(ctx, stack, cp->num * f);
        POP_CONTEXT(input);
        break;

      case P_JSON_FRACTION:
        while ((input < limit) && is_digit(*input))
        {
          cp->num = cp->num + cp->frac * (*input - '0');
          cp->frac *= 0.1;
          input++;
        }
        if (!(input < limit))
          return NULL;
        cp->state = P_JSON_EXPONENT;
        continue;

      case P_JSON_NULL_N:
        if (*input != 'u')
          myabort();
        cp->state = P_JSON_NULL_NU;
        input++;
        if (!(input < limit))
          return NULL;

      case P_JSON_NULL_NU:
        if (*input != 'l')
          myabort();
        cp->state = P_JSON_NULL_NUL;
        input++;
        if (!(input < limit))
          return NULL;

      case P_JSON_NULL_NUL:
        if (*input != 'l')
          myabort();
        vstack_push_null(ctx, stack);
        input++;
        POP_CONTEXT(input);
        break;

      case P_JSON_TRUE_T:
        if (*input != 'r')
          myabort();
        cp->state = P_JSON_TRUE_TR;
        input++;
        if (!(input < limit))
          return NULL;

      case P_JSON_TRUE_TR:
        if (*input != 'u')
          myabort();
        cp->state = P_JSON_TRUE_TRU;
        input++;
        if (!(input < limit))
          return NULL;

      case P_JSON_TRUE_TRU:
        if (*input != 'e')
          myabort();
        vstack_push_bool(ctx, stack, 1);
        input++;
        POP_CONTEXT(input);
        break;

      case P_JSON_FALSE_F:
        if (*input != 'a')
          myabort();
        cp->state = P_JSON_FALSE_FA;
        input++;
        if (!(input < limit))
          return NULL;

      case P_JSON_FALSE_FA:
        if (*input != 'l')
          myabort();
        cp->state = P_JSON_FALSE_FAL;
        input++;
        if (!(input < limit))
          return NULL;

      case P_JSON_FALSE_FAL:
        if (*input != 's')
          myabort();
        cp->state = P_JSON_FALSE_FALS;
        input++;
        if (!(input < limit))
          return NULL;

      case P_JSON_FALSE_FALS:
        if (*input != 'e')
          myabort();
        vstack_push_bool(ctx, stack, 0);
        input++;
        POP_CONTEXT(input);
        break;
    }
  }
  return NULL;
}
