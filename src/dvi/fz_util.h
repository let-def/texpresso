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

#ifndef FZ_UTIL_H
#define FZ_UTIL_H

#define fz_ptr(type, name) type *name = NULL; fz_var(name)
#define fz_try_rethrow(ctx) fz_catch(ctx) { fz_rethrow(ctx); }

static inline fz_matrix fz_post_translate(fz_matrix ctm, float tx, float ty)
{
  ctm.e += tx;
  ctm.f += ty;
  return ctm;
}

static inline fz_matrix fz_flip_vertically(fz_matrix ctm)
{
  ctm.b = -ctm.b;
  ctm.d = -ctm.d;
  ctm.f = ctm.f;
  return ctm;
}

static inline char *dtx_strndup(fz_context *ctx, const void *buf, size_t len)
{
  char *result = fz_malloc_array(ctx, len + 1, char);
  memcpy(result, buf, len);
  result[len] = 0;
  return result;
}

#endif /*!FZ_UTIL_H*/
