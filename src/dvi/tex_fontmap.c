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
#include <mupdf/fitz/context.h>
#include "mydvi.h"
#include "fz_util.h"

static unsigned long
sdbm_hash(const void *p)
{
    unsigned long hash = 0;
    int c;

    for (const unsigned char *str = p; (c = *str); str++)
        hash = c + (hash << 6) + (hash << 16) - hash;

    return hash * 2654435761;
}

#define str_hash sdbm_hash

struct tex_fontmap {
  fz_buffer *buffer;
  int mask;
  tex_fontmap_entry *table;
};

tex_fontmap *tex_fontmap_load(fz_context *ctx, fz_stream **streams, int count)
{
  fz_ptr(tex_fontmap, result);
  fz_ptr(fz_buffer, buffer);

  fz_try(ctx)
  {
    fz_buffer *buffer = fz_new_buffer(ctx, 1024 * 1024);
    for (int i = 0; i < count; ++i)
    {
      if (!streams[i]) continue;
      fz_ptr(fz_stream, leech);
      fz_try(ctx)
      {
        leech = fz_open_leecher(ctx, streams[i], buffer);
        while (fz_skip(ctx, leech, 1024 * 1024) > 0);
        fz_append_byte(ctx, buffer, '\n');
      }
      fz_always(ctx)
      {
        if (leech)
          fz_drop_stream(ctx, leech);
      }
      fz_catch(ctx)
      {
        fz_rethrow(ctx);
      }
    }
    fz_append_byte(ctx, buffer, 0);
    fz_trim_buffer(ctx, buffer);

    result = fz_malloc_struct(ctx, tex_fontmap);

    char *ptr = (char *)buffer->data;
    int count = 0, capacity = 0;
    tex_fontmap_entry *rawtable = NULL;

#define is_ws(c) ((c) == ' ' || (c) == '\t')
#define is_nl(c) ((c) == '\n')
#define is_lt(c) ((c) == '<')
#define is_quote(c) ((c) == '"')

#define seek(ptr, pred) while (*(ptr) && !is_nl(*(ptr)) && !(pred(*(ptr)))) (ptr)++
#define fail_if(ptr, pred) if (!*(ptr) || (pred(*ptr))) goto seek_next_line

    while (*ptr)
    {
      tex_fontmap_entry entry = {0,};

      seek(ptr, !is_ws);
      if (*ptr == '\n') { ptr++; continue; }
      if (*ptr == '%') goto seek_next_line;

      entry.pk_font_name = ptr;
      seek(ptr, is_ws); fail_if(ptr, is_nl);
      char *eol = ptr;

      seek(ptr, !is_ws);
      if (!is_lt(*ptr))
      {
        *eol = '\0';
        entry.ps_font_name = ptr;
        seek(ptr, is_ws);
        eol = ptr;
      }
      seek(ptr, !is_ws);

      while (*ptr && !is_nl(*ptr))
      {
        if (is_quote(*ptr))
        {
          ptr++;
          entry.ps_snippet = ptr;
          seek(ptr, is_quote);
          fail_if(ptr, !is_quote);
          *eol = '\0';
          eol = ptr;
          seek(ptr, is_ws);
        }
        else if (is_lt(*ptr))
        {
          ptr++;
          seek(ptr, !is_ws);
          if (*ptr == '[') ptr++;
          seek(ptr, !is_ws);

          char *start = ptr;
          seek(ptr, is_ws);

          if (ptr - start >= 4 &&
              ptr[-4] == '.' && ptr[-3] == 'e' &&
              ptr[-2] == 'n' && ptr[-1] == 'c')
            entry.enc_file_name = start;
          else
            entry.font_file_name = start;
        }
        else
          goto seek_next_line;
        *eol = '\0';
        eol = ptr;

        seek(ptr, !is_ws);
      }

      if (is_nl(*ptr))
      {
        *eol = '\0';
        ptr++;

        if (capacity == 0)
        {
          rawtable = fz_malloc_struct_array(ctx, 128, tex_fontmap_entry);
          capacity = 128;
        }
        else if (count >= capacity)
        {
          tex_fontmap_entry *tmp = fz_malloc_struct_array(ctx, capacity * 2, tex_fontmap_entry);
          memcpy(tmp, rawtable, sizeof(tex_fontmap_entry) * capacity);
          fz_free(ctx, rawtable);
          rawtable = tmp;
          capacity *= 2;
        }

        //printf("register entry:\n");
        //printf("-    pk name: %s\n", entry.pk_font_name);
        //printf("-    ps name: %s\n", entry.ps_font_name);
        //printf("- ps snippet: %s\n", entry.ps_snippet);
        //printf("-   enc file: %s\n", entry.enc_file_name);
        //printf("-  font file: %s\n", entry.font_file_name);
        entry.hash = str_hash(entry.pk_font_name);
        rawtable[count] = entry;
        count += 1;
        continue;
      }

seek_next_line:
      {
        char c = *ptr;
        *ptr = '\0';
        if (0 && entry.pk_font_name)
        {
          fprintf(stderr, "skip entry:\n");
          fprintf(stderr, "-    pk name: %s\n", entry.pk_font_name);
          fprintf(stderr, "-    ps name: %s\n", entry.ps_font_name);
          fprintf(stderr, "- ps snippet: %s\n", entry.ps_snippet);
          fprintf(stderr, "-   enc file: %s\n", entry.enc_file_name);
          fprintf(stderr, "-  font file: %s\n", entry.font_file_name);
        }
        *ptr = c;
        while (*ptr && !is_nl(*ptr)) ptr++;
        if (is_nl(*ptr)) ptr++;
      }
    }

#undef is_ws
#undef is_nl
#undef is_ltgg
#undef is_quote
#undef seek
#undef fail_if

    if (count + count / 4 > capacity)
      capacity *= 2;

    tex_fontmap_entry *hashtable =
      fz_malloc_struct_array(ctx, capacity, tex_fontmap_entry);

    int mask = capacity - 1;
    for (int i = 0; i < count; ++i)
    {
      tex_fontmap_entry entry = rawtable[i];
      int index = entry.hash & mask;
      while (hashtable[index].pk_font_name)
      {
        if ((entry.hash & mask) < (hashtable[index].hash & mask))
        {
          tex_fontmap_entry tmp = hashtable[index];
          hashtable[index] = entry;
          entry = tmp;
        }

        index = (index + 1) & mask;
      }
      hashtable[index] = entry;
    }

    result->buffer = buffer;
    result->mask = mask;
    result->table = hashtable;
    fz_free(ctx, rawtable);
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

void tex_fontmap_free(fz_context *ctx, tex_fontmap *t)
{
  fz_free(ctx, t->table);
  fz_drop_buffer(ctx, t->buffer);
  fz_free(ctx, t);
}

#ifdef CALC_STATS
int max_poschain = 0, max_negchain = 0, lookup_count = 0, lookup_probe = 0;

static void print_chain(void)
{
  fprintf(stderr, "tex_fontmap: max_pos_chain: %d\n", max_poschain);
  fprintf(stderr, "tex_fontmap: max_neg_chain: %d\n", max_negchain);
  fprintf(stderr, "tex_fontmap: lookup_count: %d\n", lookup_count);
  fprintf(stderr, "tex_fontmap: average_chain: %f\n", (double)lookup_probe / (double)lookup_count);
}
#endif

tex_fontmap_entry *tex_fontmap_lookup(tex_fontmap *t, const char *name)
{
#ifdef CALC_STATS
  static int v = 0;
  if (!v)
  {
    v = 1;
    atexit(print_chain);
  }
  lookup_count += 1;
  int chainlen = 0;
#endif


  unsigned long hash = str_hash(name);
  int index = hash & t->mask;

  while (t->table[index].pk_font_name)
  {
#ifdef CALC_STATS
    lookup_probe += 1;
    chainlen += 1;
#endif
    if (t->table[index].hash == hash &&
        !strcmp(t->table[index].pk_font_name, name))
    {
#ifdef CALC_STATS
      if (chainlen > max_poschain)
        max_poschain = chainlen;
#endif
      return &t->table[index];
    }

    index = (index + 1) & t->mask;
  }

#ifdef CALC_STATS
  if (chainlen > max_negchain)
    max_negchain = chainlen;
#endif
  return NULL;
}

tex_fontmap_entry *tex_fontmap_iter(tex_fontmap *t, unsigned *index)
{
  while (*index <= t->mask)
  {
    unsigned i = *index;
    *index += 1;
    if (t->table[i].pk_font_name)
      return &t->table[i];
  }
  return NULL;
}
