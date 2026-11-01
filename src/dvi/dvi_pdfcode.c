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

// Interpretation of PDF content streams (pdf:code, pdf:literal, ...)

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


/****************************/
/* Interpretation of PDF    */
/****************************/

#define MAX_ARGS 32

static int
get_args(fz_context *ctx, vstack *stack, val *args, int max)
{
  val values = vstack_get_values(ctx, stack);
  int n = values.length;
  if (n > max)
    n = max;
  for (int i = 0; i < n; i++)
    args[i] = val_array_get(ctx, stack, values, i);
  return n;
}

// Get exactly n numeric arguments
static void
get_numbers(fz_context *ctx, const char *op, val *args, int argc, float *v, int n)
{
  if (argc != n)
    fz_throw(ctx, FZ_ERROR_SYNTAX, "%s: expecting %d arguments, got %d", op, n, argc);
  for (int i = 0; i < n; i++)
    v[i] = val_number(ctx, args[i]);
}

bool
run_pdf_code(fz_context *ctx, dvi_context *dc, dvi_state *st, cursor_t cur, cursor_t lim)
{
  vstack *stack = vstack_new(ctx);
  bool result = 1;
  fz_var(result);

  if (dc->text)
    dvi_context_flush_text(ctx, dc, st);

  fz_try(ctx)
  {
    enum PDF_OP op;
    while ((op = pdf_parse_command(ctx, stack, &cur, lim)) != PDF_NONE)
    {
      const char *opname = pdf_op_name(op);
      val args[MAX_ARGS];
      float v[MAX_ARGS];
      int argc = get_args(ctx, stack, args, MAX_ARGS);
      dvi_graphicstate *gs = &st->gs;

      switch (op)
      {
        /* Graphic state */

        case PDF_OP_q:
          gs_push(st);
          break;

        case PDF_OP_Q:
          gs_pop(ctx, dc, st);
          break;

        case PDF_OP_cm:
        {
          get_numbers(ctx, opname, args, argc, v, 6);
          fz_matrix mat = fz_make_matrix(v[0], v[1], v[2], v[3], v[4], v[5]);
          gs->ctm = fz_concat(mat, gs->ctm);
          break;
        }

        case PDF_OP_w:
          get_numbers(ctx, opname, args, argc, v, 1);
          gs->line_width = v[0];
          break;

        case PDF_OP_j:
          get_numbers(ctx, opname, args, argc, v, 1);
          gs->line_join = v[0];
          break;

        case PDF_OP_J:
          get_numbers(ctx, opname, args, argc, v, 1);
          gs->line_caps = v[0];
          break;

        case PDF_OP_M:
          get_numbers(ctx, opname, args, argc, v, 1);
          gs->miter_limit = v[0];
          break;

        case PDF_OP_d:
        {
          if (argc != 2)
            fz_throw(ctx, FZ_ERROR_SYNTAX, "d: expecting 2 arguments");
          int len = val_array_length(ctx, stack, args[0]);
          if (len > 16) len = 16;
          for (int i = 0; i < len; ++i)
            gs->dash[i] = val_number(ctx, val_array_get(ctx, stack, args[0], i));
          gs->dash_len = len;
          gs->dash_phase = val_number(ctx, args[1]);
          break;
        }

        case PDF_OP_gs:
          if (argc == 1 && val_is_name(args[0]))
            apply_extgstate(ctx, dc, st, val_string(ctx, stack, args[0]));
          break;

        case PDF_OP_ri:
        case PDF_OP_i:
          break;

        /* Path construction */

        case PDF_OP_m:
          get_numbers(ctx, opname, args, argc, v, 2);
          fz_moveto(ctx, get_path(ctx, dc), v[0], v[1]);
          break;

        case PDF_OP_l:
          get_numbers(ctx, opname, args, argc, v, 2);
          fz_lineto(ctx, get_path(ctx, dc), v[0], v[1]);
          break;

        case PDF_OP_c:
          get_numbers(ctx, opname, args, argc, v, 6);
          fz_curveto(ctx, get_path(ctx, dc), v[0], v[1], v[2], v[3], v[4], v[5]);
          break;

        case PDF_OP_v:
          get_numbers(ctx, opname, args, argc, v, 4);
          fz_curvetov(ctx, get_path(ctx, dc), v[0], v[1], v[2], v[3]);
          break;

        case PDF_OP_y:
          get_numbers(ctx, opname, args, argc, v, 4);
          fz_curvetoy(ctx, get_path(ctx, dc), v[0], v[1], v[2], v[3]);
          break;

        case PDF_OP_h:
          fz_closepath(ctx, get_path(ctx, dc));
          break;

        case PDF_OP_re:
        {
          get_numbers(ctx, opname, args, argc, v, 4);
          fz_path *path = get_path(ctx, dc);
          // Not fz_rectto: the direction matters for the non-zero rule
          fz_moveto(ctx, path, v[0], v[1]);
          fz_lineto(ctx, path, v[0] + v[2], v[1]);
          fz_lineto(ctx, path, v[0] + v[2], v[1] + v[3]);
          fz_lineto(ctx, path, v[0], v[1] + v[3]);
          fz_closepath(ctx, path);
          break;
        }

        /* Path painting */

        case PDF_OP_S:
          paint(ctx, dc, st, PAINT_STROKE);
          break;
        case PDF_OP_s:
          paint(ctx, dc, st, PAINT_CLOSE | PAINT_STROKE);
          break;
        case PDF_OP_f:
        case PDF_OP_F:
          paint(ctx, dc, st, PAINT_FILL);
          break;
        case PDF_OP_f_star:
          paint(ctx, dc, st, PAINT_FILL | PAINT_EVEN_ODD);
          break;
        case PDF_OP_B:
          paint(ctx, dc, st, PAINT_FILL | PAINT_STROKE);
          break;
        case PDF_OP_B_star:
          paint(ctx, dc, st, PAINT_FILL | PAINT_EVEN_ODD | PAINT_STROKE);
          break;
        case PDF_OP_b:
          paint(ctx, dc, st, PAINT_CLOSE | PAINT_FILL | PAINT_STROKE);
          break;
        case PDF_OP_b_star:
          paint(ctx, dc, st, PAINT_CLOSE | PAINT_FILL | PAINT_EVEN_ODD | PAINT_STROKE);
          break;
        case PDF_OP_n:
          paint(ctx, dc, st, PAINT_NONE);
          break;

        /* Clipping, applied by the next painting operator */

        case PDF_OP_W:
          dc->path_clip = 1;
          break;
        case PDF_OP_W_star:
          dc->path_clip = 2;
          break;

        /* Colors */

        case PDF_OP_G:
        case PDF_OP_g:
        case PDF_OP_RG:
        case PDF_OP_rg:
        case PDF_OP_K:
        case PDF_OP_k:
        {
          if (gs->color_locked)
            break;
          int n = (op == PDF_OP_G || op == PDF_OP_g) ? 1 :
                  (op == PDF_OP_RG || op == PDF_OP_rg) ? 3 : 4;
          get_numbers(ctx, opname, args, argc, v, n);
          float rgb[3];
          color_set_components(ctx, rgb, v, n);
          if (op == PDF_OP_G || op == PDF_OP_RG || op == PDF_OP_K)
            colorstate_set_line(&gs->colors, colorspace_of_components(n), rgb);
          else
            colorstate_set_fill(&gs->colors, colorspace_of_components(n), rgb);
          break;
        }

        case PDF_OP_CS:
        case PDF_OP_cs:
        {
          if (gs->color_locked)
            break;
          if (argc != 1 || !val_is_name(args[0]))
            fz_throw(ctx, FZ_ERROR_SYNTAX, "%s: expecting a name", opname);
          uint8_t cs = load_colorspace(ctx, dc, st, val_string(ctx, stack, args[0]));
          float rgb[3];
          colorspace_initial_color(ctx, dc, cs, rgb);
          if (op == PDF_OP_CS)
            colorstate_set_line(&gs->colors, cs, rgb);
          else
            colorstate_set_fill(&gs->colors, cs, rgb);
          break;
        }

        case PDF_OP_SC:
        case PDF_OP_sc:
        case PDF_OP_SCN:
        case PDF_OP_scn:
        {
          if (gs->color_locked)
            break;
          bool line = (op == PDF_OP_SC || op == PDF_OP_SCN);
          uint8_t cs = line ? gs->colors.line_cs : gs->colors.fill_cs;
          int pattern = 0;
          int n = argc;
          if (n > 0 && val_is_name(args[n - 1]))
          {
            pattern = lookup_pattern(ctx, dc, st, val_string(ctx, stack, args[n - 1]));
            n -= 1;
          }
          for (int i = 0; i < n; i++)
            v[i] = val_number(ctx, args[i]);
          float rgb[3];
          memcpy(rgb, line ? gs->colors.line : gs->colors.fill, sizeof(rgb));
          colorspace_set_color(ctx, dc, cs, v, n, rgb);
          if (line)
          {
            colorstate_set_line(&gs->colors, cs, rgb);
            gs->colors.line_pattern = pattern;
          }
          else
          {
            colorstate_set_fill(&gs->colors, cs, rgb);
            gs->colors.fill_pattern = pattern;
          }
          break;
        }

        /* Shadings and XObjects */

        case PDF_OP_sh:
        {
          if (argc != 1 || !val_is_name(args[0]))
            fz_throw(ctx, FZ_ERROR_SYNTAX, "sh: expecting a name");
          if (!dc->dev)
            break;
          const char *name = val_string(ctx, stack, args[0]);
          pdf_obj *obj = lookup_resource(ctx, dc, st, PDF_NAME(Shading), name);
          if (!obj)
          {
            fprintf(stderr, "pdf: unknown shading %s\n", name);
            break;
          }
          fz_shade *shade = pdf_load_shading(ctx, store_doc(dc), obj);
          fz_try(ctx)
          {
            bool blend = gs->blend_mode != FZ_BLEND_NORMAL;
            if (blend)
              fz_begin_group(ctx, dc->dev, fz_infinite_rect, NULL, 0, 0, gs->blend_mode, 1);
            fz_fill_shade(ctx, dc->dev, shade, gs->ctm, gs->fill_alpha, color_params);
            if (blend)
              fz_end_group(ctx, dc->dev);
          }
          fz_always(ctx)
            fz_drop_shade(ctx, shade);
          fz_catch(ctx)
            fz_rethrow(ctx);
          break;
        }

        case PDF_OP_Do:
        {
          if (argc != 1 || !val_is_name(args[0]))
            fz_throw(ctx, FZ_ERROR_SYNTAX, "Do: expecting a name");
          const char *name = val_string(ctx, stack, args[0]);
          pdf_obj *obj = lookup_resource(ctx, dc, st, PDF_NAME(XObject), name);
          if (!obj)
            fprintf(stderr, "pdf: unknown XObject %s\n", name);
          else
            draw_xobject(ctx, dc, st, obj, gs->ctm, NULL);
          break;
        }

        /* Ignored: marked content, compatibility, type 3 fonts */

        case PDF_OP_MP:
        case PDF_OP_DP:
        case PDF_OP_BMC:
        case PDF_OP_BDC:
        case PDF_OP_EMC:
        case PDF_OP_BX:
        case PDF_OP_EX:
        case PDF_OP_d0:
        case PDF_OP_d1:
          break;

        /* Text operators are not supported */

        case PDF_OP_BT:
        case PDF_OP_ET:
        case PDF_OP_Tc:
        case PDF_OP_Tw:
        case PDF_OP_Tz:
        case PDF_OP_TL:
        case PDF_OP_Tf:
        case PDF_OP_Tr:
        case PDF_OP_Ts:
        case PDF_OP_Td:
        case PDF_OP_TD:
        case PDF_OP_Tm:
        case PDF_OP_T_star:
        case PDF_OP_Tj:
        case PDF_OP_TJ:
        case PDF_OP_squote:
        case PDF_OP_dquote:
        default:
          unhandled(opname, cur, lim, 0);
          break;

        case PDF_NONE:
          break;
      }
    }
  }
  fz_always(ctx)
  {
    vstack_free(ctx, stack);
  }
  fz_catch(ctx)
  {
    fprintf(stderr, "pdf code: %s\n", fz_caught_message(ctx));
    result = 0;
  }
  return result;
}
