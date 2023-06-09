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

#include "synctex.h"
#include <math.h>
#include <string.h>

struct offset_buffer
{
  int *ptr, len, cap;
};

static void ob_init(struct offset_buffer *ob)
{
  *ob = (struct offset_buffer){.ptr = NULL, .len = 0, .cap = 0};
}

static void ob_free(fz_context *ctx, struct offset_buffer *ob)
{
  if (ob->ptr)
    fz_free(ctx, ob->ptr);
  ob_init(ob);
}

static void ob_rollback(fz_context *ctx, struct offset_buffer *ob, int offset)
{
  while (ob->len > 0 && ob->ptr[ob->len - 1] >= offset)
    ob->len -= 1;
}

static void ob_append(fz_context *ctx, struct offset_buffer *ob, int offset)
{
  if (ob->len >= ob->cap)
  {
    if (ob->len != 0)
    {
      int cap = ob->cap * 2;
      int *ptr = fz_malloc_array(ctx, cap, int);
      memcpy(ptr, ob->ptr, sizeof(int) * ob->cap);
      fz_free(ctx, ob->ptr);
      ob->ptr = ptr;
      ob->cap = cap;
    }
    else
    {
      ob->ptr = fz_malloc_array(ctx, 32, int);
      ob->cap = 32;
    }
  }
  ob->ptr[ob->len] = offset;
  ob->len += 1;
}

struct synctex_s
{
  struct offset_buffer inputs, pages;
  int bol, cur;
};

synctex_t *synctex_new(fz_context *ctx)
{
  synctex_t *stx = fz_malloc_struct(ctx, synctex_t);
  ob_init(&stx->inputs);
  ob_init(&stx->pages);
  stx->cur = 0;
  return stx;
}

void synctex_free(fz_context *ctx, synctex_t *stx)
{
  ob_free(ctx, &stx->inputs);
  ob_free(ctx, &stx->pages);
  fz_free(ctx, stx);
}

void synctex_rollback(fz_context *ctx, synctex_t *stx, size_t offset)
{
  ob_rollback(ctx, &stx->pages, offset);
  ob_rollback(ctx, &stx->inputs, offset);
  if (stx->cur > offset)
    stx->cur = offset;
}

static const uint8_t *string_parse_int(const uint8_t *string, int *i)
{
  *i = 0;
  int neg = 0;
  if (*string == '-')
  {
    neg = 1;
    string += 1;
  }
  while (*string >= '0' && *string <= '9')
  {
    *i = *i * 10 + (*string - '0');
    string += 1;
  }
  if (neg)
    *i = - *i;
  return string;
}

static const uint8_t *string_skip_prefix(const uint8_t *string, const char *prefix)
{
  const uint8_t *ptr = (void *)prefix;
  while (*ptr)
  {
    if (*ptr == *string)
    {
      ptr += 1;
      string += 1;
    }
    else
      return NULL;
  }
  return string;
}

static void synctex_process_line(fz_context *ctx, synctex_t *stx, int offset, const uint8_t *bol, uint8_t *eol)
{
  int index = 0;
  uint8_t c = *bol;
  bol += 1;

  switch (c)
  {
    case '{': case '}':
    {
      int is_closing = (c == '}');
      if (!(bol = string_parse_int(bol, &index))) break;
      if (index != stx->pages.len / 2 + 1 || is_closing != (stx->pages.len & 1))
      {
        fprintf(stderr, "[synctex] Invalid page index: index=%d/is_closing=%d expected=%d/%d\n",
                index, is_closing, stx->pages.len / 2 + 1, stx->pages.len & 1);
      }
      ob_append(ctx, &stx->pages, offset);
      break;
    }

    case 'I':
    {
      if (!(bol = string_skip_prefix(bol, "nput:"))) break;
      if (!(bol = string_parse_int(bol, &index))) break;
      if (!(bol = string_skip_prefix(bol, ":"))) break;
      if (index != stx->inputs.len + 1)
      {
        fprintf(stderr, "[synctex] Invalid input index: index=%d expected=%d\n",
                index, stx->inputs.len + 1);
      }
      ob_append(ctx, &stx->inputs, offset);
      break;
    }

    default:
      break;
  }
}

void synctex_update(fz_context *ctx, synctex_t *stx, fz_buffer *buf)
{
  int cur = stx->cur, len = buf->len;

  if (len <= cur)
  {
    if (len < cur)
      synctex_rollback(ctx, stx, len);
    return;
  }

  uint8_t *ptr = buf->data;
  int bol = stx->bol;

  if (bol > cur)
  {
    bol = cur;
    while (bol > 0 && ptr[bol - 1] != '\n')
      bol -= 1;
  }

  while (cur < len)
  {
    if (ptr[cur] == '\n')
    {
      if (cur > bol)
        synctex_process_line(ctx, stx, bol, ptr + bol, ptr + cur);
      cur += 1;
      bol = cur;
    }
    else
      cur += 1;
  }

  stx->bol = bol;
  stx->cur = cur;
}

int synctex_page_count(synctex_t *stx)
{
  return stx ? stx->pages.len / 2 : 0;
}

int synctex_input_count(synctex_t *stx)
{
  return stx ? stx->inputs.len : 0;
}

void synctex_page_offset(fz_context *ctx, synctex_t *stx, unsigned index, int *bop, int *eop)
{
  if (index * 2 + 1 >= stx->pages.len)
    abort();

  *bop = stx->pages.ptr[2 * index + 0];
  *eop = stx->pages.ptr[2 * index + 1];
}

int synctex_input_offset(fz_context *ctx, synctex_t *stx, unsigned index)
{
  if (index >= stx->inputs.len)
    abort();

  return stx->inputs.ptr[index];
}

enum kind {
  STEX_ENTER_V,
  STEX_ENTER_H,
  STEX_LEAVE_V,
  STEX_LEAVE_H,
  STEX_CURRENT,
  STEX_OTHER,
};

struct link
{
  int tag, line, column;
};

struct point
{
  int x, y;
};

struct size
{
  int width, height, depth;
};

struct record
{
  enum kind kind;
  struct link link;
  struct point point;
  struct size size;
};

static const uint8_t *nextline(const uint8_t *ptr)
{
  while (*ptr != '\n')
    ptr++;
  return ptr + 1;
}

static const uint8_t *
skip_tree(const uint8_t *ptr, uint8_t open, uint8_t close)
{
  int nest = 1;
  while (nest > 0)
  {
    if (*ptr == open)
      nest += 1;
    else if (*ptr == close)
      nest -= 1;
    else if (*ptr == '}')
      break;
    ptr = nextline(ptr);
  }
  return ptr;
}

static const uint8_t *
skip_record(const uint8_t *ptr, struct record *r)
{
  switch (r->kind)
  {
    case STEX_ENTER_H:
      return skip_tree(ptr, '(', ')');
    case STEX_ENTER_V:
      return skip_tree(ptr, '[', ']');
    default:
      return ptr;
  }
}

static _Bool
parse_link(const uint8_t **ptr, struct link *link)
{
  *ptr = string_parse_int(*ptr, &link->tag);
  if (**ptr != ',')
    return 0;
  *ptr = string_parse_int(*ptr + 1, &link->line);
  if (**ptr == ',')
    *ptr = string_parse_int(*ptr + 1, &link->column);
  else
    link->column = -1;
  return 1;
}

static _Bool
parse_point(const uint8_t **ptr, struct point *point)
{
  *ptr = string_parse_int(*ptr, &point->x);
  if (**ptr != ',')
    return 0;
  if (**ptr == '=')
    *ptr += 1;
  else
    *ptr = string_parse_int(*ptr + 1, &point->y);
  return 1;
}

static _Bool
parse_size(const uint8_t **ptr, struct size *size)
{
  *ptr = string_parse_int(*ptr, &size->width);
  if (**ptr != ',')
    return 0;
  *ptr = string_parse_int(*ptr + 1, &size->height);
  if (**ptr != ',')
    return 0;
  *ptr = string_parse_int(*ptr + 1, &size->depth);
  return 1;
}

static const uint8_t *
parse_line(const uint8_t *ptr, struct record *r)
{
  if (!ptr) return NULL;
  if (ptr[-1] != '\n') abort();

  int has_link = 0, has_point = 0, has_size = 0;

  switch (*ptr)
  {
    case 'x':
      r->kind = STEX_CURRENT;
      has_link = has_point = 1;
      break;

    case '(':
      r->kind = STEX_ENTER_H;
      has_link = has_point = has_size = 1;
      break;

    case ')':
      r->kind = STEX_LEAVE_H;
      break;

    case '[':
      r->kind = STEX_ENTER_V;
      has_link = has_point = has_size = 1;
      break;

    case ']':
      r->kind = STEX_LEAVE_V;
      break;

    case '}':
      return NULL;

    default:
      r->kind = STEX_OTHER;
      break;
  }

  ptr += 1;

  if (has_link && !parse_link(&ptr, &r->link))
    abort();

  if (has_point && ((*ptr++ != ':') || !parse_point(&ptr, &r->point)))
    abort();

  if (has_size && ((*ptr++ != ':') || !parse_size(&ptr, &r->size)))
    abort();

  return nextline(ptr);
}

struct candidate {
  float area;
  fz_irect rect;
  struct link link;
};

static float rect_area(fz_irect r)
{
  return (float)(r.y1 - r.y0) * (float)(r.x1 - r.x0);
}

static void
parse_tree(const uint8_t *ptr, int x, int y, struct candidate *c)
{
  int nest = 0;
  struct size saved[256];

  struct record r = {0,};
  while ((ptr = parse_line(ptr, &r)))
  {
    fz_irect rect;
    rect.x0 = r.point.x;
    rect.x1 = r.point.x + r.size.width;
    rect.y0 = r.point.y - r.size.height;
    rect.y1 = r.point.y + r.size.depth;
    switch (r.kind)
    {
      case STEX_CURRENT:
        if (rect.y0 <= y && y <= rect.y1)
        {
          if (rect.x0 < x)
            rect.x1 = x;
          else
          {
            rect.x1 = rect.x0;
            rect.x0 = x;
          }
          float area = rect_area(rect);
          if (area < c->area)
          {
            c->area = area;
            c->rect = rect;
            c->link = r.link;
          }
        }
      case STEX_ENTER_H:
      case STEX_ENTER_V:
        if (fz_is_point_inside_irect(x, y, rect))
        {
          float area = rect_area(rect);
          if (area < c->area)
          {
            c->area = area;
            c->rect = rect;
            c->link = r.link;
          }
          saved[nest] = r.size;
          nest += 1;
        }
        else
          ptr = skip_record(ptr, &r);
        break;
      case STEX_LEAVE_H:
      case STEX_LEAVE_V:
        nest -= 1;
        if (nest < 0)
          return;
        r.size = saved[nest];
      case STEX_OTHER:
        continue;
    }
  }
}

static void output_sexp_string(FILE *f, const char *ptr, int len)
{
  for (const char *lim = ptr + len; ptr < lim; ptr++)
  {
    char c = *ptr;
    switch (c)
    {
      case '\t':
        putc_unlocked('\\', f);
        c = 't';
        break;
      case '\r':
        putc_unlocked('\\', f);
        c = 'r';
        break;
      case '\n':
        c = 'n';
      case '"':
      case '\\':
        putc_unlocked('\\', f);
    }
    putc_unlocked(c, f);
  }
}

void synctex_scan(fz_context *ctx, synctex_t *stx, fz_buffer *buf, unsigned page, int x, int y)
{
  if (synctex_page_count(stx) <= page)
    return;

  int bop, eop;
  synctex_page_offset(ctx, stx, page, &bop, &eop);

  const uint8_t *ptr = &buf->data[bop];

  struct candidate c = {0,};
  c.area = INFINITY;

  parse_tree(ptr, x, y, &c);
  if (c.link.tag)
  {
    const uint8_t *filename = &buf->data[stx->inputs.ptr[c.link.tag-1]];
    while (*filename != ':') filename++;
    filename++;
    while (*filename != ':') filename++;
    filename++;
    const uint8_t *fend = filename;
    while (*fend != '\n') fend++;
    int len = (fend - filename);
    fprintf(stderr,
            "synctex best candidate: (%d,%d)-(%d,%d) "
            "file:%.*s line:%d column:%d\n",
            c.rect.x0, c.rect.y0, c.rect.x1, c.rect.y1,
            len, filename,
            c.link.line, c.link.column);
    fprintf(stdout, "(synctex \"");
    output_sexp_string(stdout, (const void *)filename, len);
    fprintf(stdout, "\" %d %d)\n", c.link.line, c.link.column);
  }
}
