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
#include "mydvi.h"
#include "fz_util.h"

struct tex_enc {
  fz_buffer *buffer;
  const char *name;
  const char *entries[256];
};

tex_enc *tex_enc_load(fz_context *ctx, fz_stream *stream)
{
  fz_ptr(tex_enc, result);
  fz_ptr(fz_buffer, buffer);

  fz_try(ctx)
  {
    buffer = fz_read_all(ctx, stream, 4096);
    fz_trim_buffer(ctx, buffer);
    result = fz_malloc_struct(ctx, tex_enc);
    result->buffer = buffer;

#define is_nl(c) ((c) == '\n')

#define is_delim(c) \
  (c) == '/' || (c) == '%' || (c) == '[' || (c) == ']'

#define is_delim_or_ws(c) \
  is_delim(c) || (c) == ' ' || (c) == '\t'

#define seek(ptr, pred) \
  while (*(ptr) && !is_nl(*(ptr)) && !(pred(*(ptr)))) (ptr)++

    char *ptr = (char *)buffer->data;
    char *ending = ptr + buffer->len;
    int entry = -1;

    while (*ptr)
    {
      seek(ptr, is_delim);
      if (*ptr == '%')
      {
        //fprintf(stderr, "skipping comment\n");
        seek(ptr, is_nl);
        if (*ptr) ptr++;
        continue;
      }
      if (*ptr == '[')
      {
        //fprintf(stderr, "beginning entries\n");
        entry = 0;
        ptr++;
        continue;
      }
      if (*ptr == ']')
      {
        //fprintf(stderr, "finishing entries\n");
        break;
      }
      if (*ptr == '\n')
      {
        ptr++;
        continue;
      }
      if (*ptr == '/')
      {
        //fprintf(stderr, "parsing name\n");
        ptr++;
        const char *name = ptr;
        seek(ptr, is_delim_or_ws);
        *ending = '\0';
        ending = ptr;

        if (entry == -1)
        {
          if (result->name)
          {
            int c = *ending;
            *ending = 0;
            //fprintf(stderr, "tex_enc_load: parse errors, already named (%s and %s)\n", result->name, name);
            *ending = c;
          }
          result->name = name;
        }
        else if (entry <= 255)
        {
          result->entries[entry] = name;
          entry++;
        }
        else
        {
          int c = *ending;
          *ending = 0;
          //fprintf(stderr, "tex_enc_load: extra entry %s", name);
          *ending = c;
        }
      }
    }
    *ending = '\x0';

    if (entry < 256)
      fprintf(stderr, "tex_enc_load: incomplete encoding, %d entries\n", entry);
  }
  fz_catch(ctx)
  {
    if (result)
      fz_free(ctx, result);
    if (buffer)
      fz_drop_buffer(ctx, buffer);
    fz_rethrow(ctx);
  }

  return result;
}

void tex_enc_free(fz_context *ctx, tex_enc *t)
{
  fz_drop_buffer(ctx, t->buffer);
  fz_free(ctx, t);
}

const char *tex_enc_get(tex_enc *t, uint8_t code)
{
  return t->entries[code];
}
