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
#include "dvi_pdfstore.h"

struct named_obj
{
  char *name;
  unsigned long hash;
  int num;
};

struct dvi_pdfstore
{
  pdf_document *doc;
  pdf_obj *page_resources;

  // Open-addressing hash table from names to object numbers
  struct named_obj *names;
  int names_count, names_mask;
};

static unsigned long name_hash(const char *name, int len)
{
  unsigned long hash = 5381;
  for (int i = 0; i < len; ++i)
    hash = hash * 33 + (unsigned char)name[i];
  return hash;
}

dvi_pdfstore *dvi_pdfstore_new(fz_context *ctx)
{
  dvi_pdfstore *ps = fz_malloc_struct(ctx, dvi_pdfstore);
  fz_try(ctx)
  {
    ps->doc = pdf_create_document(ctx);
    ps->page_resources = pdf_new_dict(ctx, ps->doc, 8);
    ps->names_mask = 63;
    ps->names = fz_malloc_struct_array(ctx, ps->names_mask + 1, struct named_obj);
  }
  fz_catch(ctx)
  {
    dvi_pdfstore_free(ctx, ps);
    fz_rethrow(ctx);
  }
  return ps;
}

void dvi_pdfstore_free(fz_context *ctx, dvi_pdfstore *ps)
{
  if (!ps)
    return;
  if (ps->names)
  {
    for (int i = 0; i <= ps->names_mask; ++i)
      fz_free(ctx, ps->names[i].name);
    fz_free(ctx, ps->names);
  }
  pdf_drop_obj(ctx, ps->page_resources);
  pdf_drop_document(ctx, ps->doc);
  fz_free(ctx, ps);
}

pdf_document *dvi_pdfstore_doc(dvi_pdfstore *ps)
{
  return ps->doc;
}

pdf_obj *dvi_pdfstore_page_resources(dvi_pdfstore *ps)
{
  return ps->page_resources;
}

static void insert_name(struct named_obj *table, int mask, struct named_obj entry)
{
  int index = entry.hash & mask;
  while (table[index].name)
    index = (index + 1) & mask;
  table[index] = entry;
}

int dvi_pdfstore_name(fz_context *ctx, dvi_pdfstore *ps, const char *name, int len, bool create)
{
  unsigned long hash = name_hash(name, len);
  int index = hash & ps->names_mask;

  while (ps->names[index].name)
  {
    struct named_obj *n = &ps->names[index];
    if (n->hash == hash && strncmp(n->name, name, len) == 0 && n->name[len] == 0)
      return n->num;
    index = (index + 1) & ps->names_mask;
  }

  if (!create)
    return 0;

  if ((ps->names_count + 1) * 2 > ps->names_mask)
  {
    int mask = ps->names_mask * 2 + 1;
    struct named_obj *table = fz_malloc_struct_array(ctx, mask + 1, struct named_obj);
    for (int i = 0; i <= ps->names_mask; ++i)
      if (ps->names[i].name)
        insert_name(table, mask, ps->names[i]);
    fz_free(ctx, ps->names);
    ps->names = table;
    ps->names_mask = mask;
  }

  struct named_obj entry;
  entry.name = fz_malloc(ctx, len + 1);
  memcpy(entry.name, name, len);
  entry.name[len] = 0;
  entry.hash = hash;
  fz_try(ctx)
    entry.num = pdf_create_object(ctx, ps->doc);
  fz_catch(ctx)
  {
    fz_free(ctx, entry.name);
    fz_rethrow(ctx);
  }
  insert_name(ps->names, ps->names_mask, entry);
  ps->names_count += 1;
  return entry.num;
}

static bool is_space(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0;
}

static bool is_delim(char c)
{
  return c == '(' || c == ')' || c == '/' || c == '<' || c == '>' ||
         c == '[' || c == ']' || c == '{' || c == '}' || c == '%';
}

fz_stream *dvi_pdfstore_open(fz_context *ctx, dvi_pdfstore *ps,
                             const char *cur, const char *lim)
{
  fz_buffer *buf = fz_new_buffer(ctx, lim - cur + 16);
  fz_stream *stm = NULL;

  fz_try(ctx)
  {
    const char *chunk = cur;
    while (cur < lim)
    {
      char c = *cur;
      if (c == '(')
      {
        // Skip strings (reference markers have no meaning there)
        int nesting = 0;
        for (; cur < lim; cur++)
        {
          if (*cur == '\\')
            cur++;
          else if (*cur == '(')
            nesting++;
          else if (*cur == ')' && --nesting == 0)
          {
            cur++;
            break;
          }
        }
      }
      else if (c == '/')
      {
        // Skip names (pgf uses names such as /pgf@ca0.5)
        cur++;
        while (cur < lim && !is_space(*cur) && !is_delim(*cur))
          cur++;
      }
      else if (c == '%')
      {
        while (cur < lim && *cur != '\n' && *cur != '\r')
          cur++;
      }
      else if (c == '@')
      {
        const char *name = cur + 1;
        cur = name;
        while (cur < lim && !is_space(*cur) && !is_delim(*cur))
          cur++;
        fz_append_data(ctx, buf, chunk, name - 1 - chunk);
        int num = dvi_pdfstore_name(ctx, ps, name, cur - name, 1);
        fz_append_printf(ctx, buf, " %d 0 R ", num);
        chunk = cur;
      }
      else
        cur++;
    }
    fz_append_data(ctx, buf, chunk, cur - chunk);
    stm = fz_open_buffer(ctx, buf);
  }
  fz_always(ctx)
    fz_drop_buffer(ctx, buf);
  fz_catch(ctx)
    fz_rethrow(ctx);

  return stm;
}

pdf_obj *dvi_pdfstore_parse(fz_context *ctx, dvi_pdfstore *ps, fz_stream *stm)
{
  int c;
  while ((c = fz_peek_byte(ctx, stm)) != EOF && is_space(c))
    fz_read_byte(ctx, stm);
  if (c == EOF)
    return NULL;

  pdf_lexbuf lexbuf;
  pdf_obj *result = NULL;
  pdf_lexbuf_init(ctx, &lexbuf, PDF_LEXBUF_SMALL);
  fz_try(ctx)
    result = pdf_parse_stm_obj(ctx, ps->doc, stm, &lexbuf);
  fz_always(ctx)
    pdf_lexbuf_fin(ctx, &lexbuf);
  fz_catch(ctx)
    fz_rethrow(ctx);
  return result;
}

void dvi_pdfstore_invalidate(fz_context *ctx, dvi_pdfstore *ps, size_t offset)
{
  for (int i = 0; i <= ps->names_mask; ++i)
  {
    if (!ps->names[i].name)
      continue;
    pdf_obj *obj = NULL;
    fz_var(obj);
    fz_try(ctx)
    {
      obj = pdf_load_object(ctx, ps->doc, ps->names[i].num);
      if (pdf_is_dict(ctx, obj) &&
          pdf_dict_gets(ctx, obj, "TXPStart") &&
          pdf_to_int64(ctx, pdf_dict_gets(ctx, obj, "TXPEnd")) > (int64_t)offset)
      {
        pdf_dict_dels(ctx, obj, "TXPStart");
        pdf_dict_dels(ctx, obj, "TXPEnd");
      }
    }
    fz_always(ctx)
      pdf_drop_obj(ctx, obj);
    fz_catch(ctx)
      fz_ignore_error(ctx);
  }
}
