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

// Colors and color stacks

#include <ctype.h>
#include <string.h>
#include <math.h>
#include <mupdf/pdf.h>
#include "fz_util.h"
#include "mydvi.h"
#include "mydvi_interp.h"
#include "pdf_lexer.h"
#include "dvi_pdfstore.h"
#include "dvi_special.h"

/**********/
/* Colors */
/**********/

void color_set_rgb(float color[3], float r, float g, float b)
{
  color[0] = r;
  color[1] = g;
  color[2] = b;
}

void color_set_gray(float color[3], float gray)
{
  color_set_rgb(color, gray, gray, gray);
}

void color_set_cmyk(fz_context *ctx, float color[3], float c, float m, float y, float k)
{
  float cmyk[4] = {c, m, y, k};
  fz_convert_color(ctx, fz_device_cmyk(ctx), cmyk, fz_device_rgb(ctx), color,
                   NULL, color_params);
}

void color_set_hsb(float color[3], float h, float s, float b)
{
  h = (h - floorf(h)) * 6;
  int i = (int)h;
  float f = h - i, p = b * (1 - s), q = b * (1 - s * f), t = b * (1 - s * (1 - f));
  switch (i)
  {
    case 0:  color_set_rgb(color, b, t, p); break;
    case 1:  color_set_rgb(color, q, b, p); break;
    case 2:  color_set_rgb(color, p, b, t); break;
    case 3:  color_set_rgb(color, p, q, b); break;
    case 4:  color_set_rgb(color, t, p, b); break;
    default: color_set_rgb(color, b, p, q); break;
  }
}

// Convert components to RGB, with n = 1 (gray), 3 (rgb) or 4 (cmyk)
bool color_set_components(fz_context *ctx, float color[3], const float *v, int n)
{
  switch (n)
  {
    case 1: color_set_gray(color, v[0]); return 1;
    case 3: color_set_rgb(color, v[0], v[1], v[2]); return 1;
    case 4: color_set_cmyk(ctx, color, v[0], v[1], v[2], v[3]); return 1;
    default: return 0;
  }
}

uint8_t colorspace_of_components(int n)
{
  return n == 4 ? DVI_CS_CMYK : n == 3 ? DVI_CS_RGB : DVI_CS_GRAY;
}

void colorstate_set_fill(dvi_colorstate *cs, uint8_t space, const float rgb[3])
{
  memcpy(cs->fill, rgb, sizeof(float) * 3);
  cs->fill_cs = space;
  cs->fill_pattern = 0;
}

void colorstate_set_line(dvi_colorstate *cs, uint8_t space, const float rgb[3])
{
  memcpy(cs->line, rgb, sizeof(float) * 3);
  cs->line_cs = space;
  cs->line_pattern = 0;
}

cursor_t
colorspec_numbers(fz_context *ctx, cursor_t cur, cursor_t lim, float rgb[3], uint8_t *space,
                  cursor_t f0, cursor_t f1, cursor_t f2, cursor_t f3)
{
  if (f3)
  {
    color_set_cmyk(ctx, rgb, pfloat(f0, lim), pfloat(f1, lim), pfloat(f2, lim), pfloat(f3, lim));
    *space = DVI_CS_CMYK;
  }
  else if (f1)
  {
    color_set_rgb(rgb, pfloat(f0, lim), pfloat(f1, lim), pfloat(f2, lim));
    *space = DVI_CS_RGB;
  }
  else
  {
    color_set_gray(rgb, pfloat(f0, lim));
    *space = DVI_CS_GRAY;
  }
  return cur;
}

// Parse a color special ("color push ..."), setting both fill and line colors
bool parse_color(fz_context *ctx, dvi_colorstate *state, cursor_t cur, cursor_t lim)
{
  float rgb[3];
  uint8_t space;
  if (!parse_colorspec(ctx, cur, lim, rgb, &space))
    return unhandled("color", cur, lim, 0);
  colorstate_set_fill(state, space, rgb);
  colorstate_set_line(state, space, rgb);
  return 1;
}

// Interpret the color operators of a PDF literal ("0 g 0 G", "1 0 0 rg", ...)
bool parse_pdfcolor(fz_context *ctx, dvi_colorstate *state, cursor_t cur, cursor_t lim)
{
  vstack *stack = vstack_new(ctx);
  bool result = 1;
  fz_var(result);

  fz_try(ctx)
  {
    enum PDF_OP op;
    while ((op = pdf_parse_command(ctx, stack, &cur, lim)) != PDF_NONE)
    {
      val args = vstack_get_values(ctx, stack);
      float v[4];
      int n = args.length;
      if (n > 4) n = 4;
      for (int i = 0; i < n; i++)
      {
        val x = val_array_get(ctx, stack, args, i);
        v[i] = val_is_number(x) ? x.f : 0;
      }
      float rgb[3];
      bool fill = 1;

      switch (op)
      {
        case PDF_OP_G: case PDF_OP_RG: case PDF_OP_K:
        case PDF_OP_SC: case PDF_OP_SCN:
          fill = 0;
          /* fall through */
        case PDF_OP_g: case PDF_OP_rg: case PDF_OP_k:
        case PDF_OP_sc: case PDF_OP_scn:
          if (color_set_components(ctx, rgb, v, n))
          {
            if (fill)
              colorstate_set_fill(state, colorspace_of_components(n), rgb);
            else
              colorstate_set_line(state, colorspace_of_components(n), rgb);
          }
          break;

        default:
          break;
      }
    }
  }
  fz_always(ctx)
    vstack_free(ctx, stack);
  fz_catch(ctx)
  {
    fprintf(stderr, "pdf color: %s\n", fz_caught_message(ctx));
    result = 0;
  }
  return result;
}

// Find the end of a parenthesized PDF string, starting after the "("
cursor_t pdfstring_end(cursor_t cur, cursor_t lim)
{
  int nesting = 1;
  for (; cur < lim; cur++)
  {
    if (*cur == '\\')
      cur++;
    else if (*cur == '(')
      nesting++;
    else if (*cur == ')' && --nesting == 0)
      break;
  }
  return cur;
}
/****************/
/* Color stacks */
/****************/

void dvi_colorstacks_clear(fz_context *ctx, dvi_colorstacks *cs)
{
  fz_free(ctx, cs->main.base);
  for (int i = 0; i < cs->capacity; i++)
    fz_free(ctx, cs->stacks[i].base);
  fz_free(ctx, cs->stacks);
  *cs = (dvi_colorstacks){0,};
}

static void colorstack_copy(fz_context *ctx, dvi_colorstack *dst, const dvi_colorstack *src)
{
  *dst = *src;
  dst->base = NULL;
  dst->limit = src->depth;
  if (src->depth > 0)
  {
    dst->base = fz_malloc_struct_array(ctx, src->depth, dvi_colorstate);
    memcpy(dst->base, src->base, sizeof(dvi_colorstate) * src->depth);
  }
}

void dvi_colorstacks_copy(fz_context *ctx, dvi_colorstacks *dst, const dvi_colorstacks *src)
{
  dvi_colorstacks_clear(ctx, dst);
  if (!src)
    return;
  colorstack_copy(ctx, &dst->main, &src->main);
  if (src->capacity > 0)
  {
    dst->stacks = fz_malloc_struct_array(ctx, src->capacity, dvi_colorstack);
    dst->capacity = src->capacity;
    for (int i = 0; i < src->capacity; i++)
      colorstack_copy(ctx, &dst->stacks[i], &src->stacks[i]);
  }
}

// The current color of a stack
const dvi_colorstate *colorstack_top(const dvi_colorstack *stack)
{
  return stack->depth > 0 ? &stack->base[stack->depth - 1] : &stack->origin;
}

static void colorstack_push(fz_context *ctx, dvi_colorstack *stack, const dvi_colorstate *color)
{
  if (stack->depth == stack->limit)
  {
    int size = stack->limit == 0 ? 16 : stack->limit * 2;
    dvi_colorstate *base = fz_malloc_struct_array(ctx, size, dvi_colorstate);
    if (stack->depth > 0)
      memcpy(base, stack->base, sizeof(dvi_colorstate) * stack->depth);
    fz_free(ctx, stack->base);
    stack->base = base;
    stack->limit = size;
  }
  stack->base[stack->depth] = *color;
  stack->depth += 1;
}

static bool colorstack_pop(dvi_colorstack *stack)
{
  if (stack->depth == 0)
    return 0;
  stack->depth -= 1;
  return 1;
}

// Replace the current color
static void colorstack_set(dvi_colorstack *stack, const dvi_colorstate *color)
{
  if (stack->depth > 0)
    stack->base[stack->depth - 1] = *color;
  else
    stack->origin = *color;
}

// A pdfcolorstack, NULL if it has not been initialized
static dvi_colorstack *pdfcolorstack_get(dvi_colorstacks *cs, int index)
{
  if (index < 0 || index >= cs->capacity || !cs->stacks[index].defined)
  {
    fprintf(stderr, "pdfcolorstack %d: no such stack\n", index);
    return NULL;
  }
  return &cs->stacks[index];
}

static dvi_colorstack *pdfcolorstack_init(fz_context *ctx, dvi_colorstacks *cs, int index)
{
  if (index < 0 || index > 1023)
    return NULL;
  if (index >= cs->capacity)
  {
    int cap = cs->capacity == 0 ? 4 : cs->capacity;
    while (cap <= index)
      cap *= 2;
    dvi_colorstack *stacks = fz_malloc_struct_array(ctx, cap, dvi_colorstack);
    if (cs->capacity > 0)
      memcpy(stacks, cs->stacks, sizeof(dvi_colorstack) * cs->capacity);
    fz_free(ctx, cs->stacks);
    cs->stacks = stacks;
    cs->capacity = cap;
  }
  dvi_colorstack *stack = &cs->stacks[index];
  fz_free(ctx, stack->base);
  *stack = (dvi_colorstack){0,};
  stack->defined = 1;
  return stack;
}

// The colors at the beginning of a page: the current color of the main stack,
// then of the pdfcolorstacks declared with "page" (like dvipdfmx)
void dvi_colorstacks_page_color(const dvi_colorstacks *cs, dvi_colorstate *color)
{
  *color = *colorstack_top(&cs->main);
  for (int i = 0; i < cs->capacity; i++)
    if (cs->stacks[i].defined && cs->stacks[i].page)
      *color = *colorstack_top(&cs->stacks[i]);
}

// Parse "pdf:bcolor" / "pdf:scolor" arguments: FILL [STROKE]
static bool parse_pdf_setcolor(fz_context *ctx, dvi_colorstate *color, cursor_t cur, cursor_t lim)
{
  float fill[3], line[3];
  uint8_t fill_cs, line_cs;
  cur = parse_colorspec(ctx, cur, lim, fill, &fill_cs);
  if (!cur)
    return unhandled("pdf color", cur, lim, 0);
  if (!parse_colorspec(ctx, cur, lim, line, &line_cs))
  {
    memcpy(line, fill, sizeof(line));
    line_cs = fill_cs;
  }
  colorstate_set_fill(color, fill_cs, fill);
  colorstate_set_line(color, line_cs, line);
  return 1;
}

// Interpret a color stack special on the stacks `cs`. The arguments start at
// `cur`; for pdfcolorstack operations taking a literal, `cur` is after the
// opening parenthesis.
// When rendering, `current` is the color of the graphic state, updated to the
// new current color.
bool special_colorstack(fz_context *ctx, dvi_colorstacks *cs, dvi_colorstate *current,
                        enum colorstack_op op, int index, bool page,
                        cursor_t cur, cursor_t lim)
{
  dvi_colorstack *stack = &cs->main;
  dvi_colorstate color;

  if (op >= PDFCOLORSTACK_PUSH && op != PDFCOLORSTACK_INIT)
  {
    stack = pdfcolorstack_get(cs, index);
    if (!stack)
      return 0;
  }

  switch (op)
  {
    case COLOR_PUSH:
      // An unknown color (e.g. a named one) pushes the current color again,
      // so that the matching pop stays balanced
      color = *colorstack_top(stack);
      if (!parse_color(ctx, &color, cur, lim))
        color = *colorstack_top(stack);
      colorstack_push(ctx, stack, &color);
      break;

    case COLOR_SET:
      // "color X": clear the stack and set the color
      color = *colorstack_top(stack);
      if (!parse_color(ctx, &color, cur, lim))
        return 0;
      stack->depth = 0;
      stack->origin = color;
      break;

    case PDF_BCOLOR:
    case PDF_SCOLOR:
      color = *colorstack_top(stack);
      if (!parse_pdf_setcolor(ctx, &color, cur, lim))
      {
        // Keep "pdf:bcolor" balanced with its "pdf:ecolor"
        if (op != PDF_BCOLOR)
          return 0;
        color = *colorstack_top(stack);
      }
      if (op == PDF_BCOLOR)
        colorstack_push(ctx, stack, &color);
      else
        colorstack_set(stack, &color);
      break;

    case COLOR_POP:
    case PDFCOLORSTACK_POP:
      if (!colorstack_pop(stack))
      {
        fprintf(stderr, "color pop: empty stack\n");
        return 0;
      }
      break;

    case PDFCOLORSTACK_PUSH:
    case PDFCOLORSTACK_SET:
      color = *colorstack_top(stack);
      if (!parse_pdfcolor(ctx, &color, cur, pdfstring_end(cur, lim)))
      {
        // Keep the push balanced with its pop
        if (op != PDFCOLORSTACK_PUSH)
          return 0;
        color = *colorstack_top(stack);
      }
      if (op == PDFCOLORSTACK_PUSH)
        colorstack_push(ctx, stack, &color);
      else
        colorstack_set(stack, &color);
      break;

    case PDFCOLORSTACK_CURRENT:
      break;

    case PDFCOLORSTACK_INIT:
      stack = pdfcolorstack_init(ctx, cs, index);
      if (!stack)
        return 0;
      stack->page = page;
      if (!parse_pdfcolor(ctx, &stack->origin, cur, pdfstring_end(cur, lim)))
        return 0;
      break;
  }

  if (current)
    *current = *colorstack_top(stack);
  return 1;
}
