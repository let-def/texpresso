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
#include "page_collection.h"
#include "myabort.h"

// Maximum number of placeholders appended at once
#define MAX_GROWTH 16

struct txp_page_s
{
  fz_display_list *dl;
  fz_rect bounds;
  bool has_content;
  fz_rect content;
  fz_stext_page *stext;
};

void txp_page_collection_init(txp_page_collection *pc)
{
  *pc = (txp_page_collection){0,};
  pc->grow1 = pc->grow2 = 1;
  pc->content_x0 = INFINITY;
  pc->content_x1 = -INFINITY;
  txp_prefix_sum_init(&pc->heights);
}

static void clear_page(fz_context *ctx, txp_page *p)
{
  fz_drop_display_list(ctx, p->dl);
  fz_drop_stext_page(ctx, p->stext);
  *p = (txp_page){0,};
}

void txp_page_collection_finalize(fz_context *ctx, txp_page_collection *pc)
{
  for (int i = 0; i < pc->count; i++)
    clear_page(ctx, &pc->pages[i]);
  fz_free(ctx, pc->pages);
  txp_prefix_sum_finalize(&pc->heights);
  *pc = (txp_page_collection){0,};
}

int txp_page_collection_count(const txp_page_collection *pc)
{
  return pc->count;
}

int txp_page_collection_valid_count(const txp_page_collection *pc)
{
  return pc->valid;
}

fz_display_list *txp_page_collection_get(const txp_page_collection *pc, int index)
{
  if (index < 0 || index >= pc->count)
    return NULL;
  return pc->pages[index].dl;
}

static fz_rect page_content(fz_context *ctx, txp_page_collection *pc, txp_page *p)
{
  if (p->has_content)
    return p->content;

  fz_rect content = fz_empty_rect;
  if (p->dl)
  {
    fz_device *dev = NULL;
    fz_var(dev);
    fz_try(ctx)
    {
      dev = fz_new_bbox_device(ctx, &content);
      fz_run_display_list(ctx, p->dl, dev, fz_identity, p->bounds, NULL);
      fz_close_device(ctx, dev);
    }
    fz_always(ctx)
      fz_drop_device(ctx, dev);
    fz_catch(ctx)
    {
      fprintf(stderr, "[pages] cannot compute page contents: %s\n",
              fz_caught_message(ctx));
      content = fz_empty_rect;
    }
    content = fz_intersect_rect(content, p->bounds);
  }

  // Nothing to crop to (blank page): keep the whole page
  if (fz_is_empty_rect(content))
    content = p->bounds;
  else
  {
    if (content.x0 < pc->content_x0) pc->content_x0 = content.x0;
    if (content.x1 > pc->content_x1) pc->content_x1 = content.x1;
  }

  p->content = content;
  p->has_content = true;
  return content;
}

static float page_height(fz_context *ctx, txp_page_collection *pc, txp_page *p)
{
  fz_rect r = pc->crop ? page_content(ctx, pc, p) : p->bounds;
  return r.y1 - r.y0;
}

fz_rect txp_page_collection_bounds(fz_context *ctx, txp_page_collection *pc,
                                   int index, bool columns)
{
  if (index < 0 || index >= pc->count)
    return fz_empty_rect;
  txp_page *p = &pc->pages[index];
  if (!pc->crop)
    return p->bounds;
  fz_rect r = page_content(ctx, pc, p);
  if (columns && pc->content_x0 < pc->content_x1)
  {
    r.x0 = fz_max(pc->content_x0, p->bounds.x0);
    r.x1 = fz_min(pc->content_x1, p->bounds.x1);
    if (r.x1 <= r.x0)
      r = p->bounds;
  }
  return r;
}

fz_stext_page *txp_page_collection_stext(fz_context *ctx, txp_page_collection *pc, int index)
{
  if (index < 0 || index >= pc->count)
    return NULL;
  txp_page *p = &pc->pages[index];
  if (!p->dl)
    return NULL;
  if (!p->stext)
  {
    fz_stext_page *stext = NULL;
    fz_device *dev = NULL;
    fz_var(stext);
    fz_var(dev);
    fz_try(ctx)
    {
      stext = fz_new_stext_page(ctx, p->bounds);
      dev = fz_new_stext_device(ctx, stext, NULL);
      fz_run_display_list(ctx, p->dl, dev, fz_identity, p->bounds, NULL);
      fz_close_device(ctx, dev);
    }
    fz_always(ctx)
      fz_drop_device(ctx, dev);
    fz_catch(ctx)
    {
      fprintf(stderr, "[pages] cannot extract page text: %s\n",
              fz_caught_message(ctx));
      fz_drop_stext_page(ctx, stext);
      stext = NULL;
    }
    p->stext = stext;
  }
  return p->stext;
}

static void reserve(fz_context *ctx, txp_page_collection *pc, int count)
{
  if (count <= pc->cap)
    return;
  int cap = pc->cap ? pc->cap : 8;
  while (cap < count)
    cap *= 2;
  txp_page *pages = fz_malloc_struct_array(ctx, cap, txp_page);
  if (pc->count > 0)
    memcpy(pages, pc->pages, sizeof(txp_page) * pc->count);
  fz_free(ctx, pc->pages);
  pc->pages = pages;
  pc->cap = cap;
}

static void set_page(fz_context *ctx, txp_page_collection *pc, int index,
                     fz_display_list *dl, fz_rect bounds)
{
  txp_page *p = &pc->pages[index];
  clear_page(ctx, p);
  p->dl = fz_keep_display_list(ctx, dl);
  p->bounds = bounds;
  txp_prefix_sum_set(&pc->heights, index, page_height(ctx, pc, p));
}

void txp_page_collection_set(fz_context *ctx, txp_page_collection *pc,
                             int index, fz_display_list *dl)
{
  if (index < 0)
    myabort();

  fz_rect bounds;
  if (dl)
    bounds = fz_bound_display_list(ctx, dl);
  else if (index < pc->count)
    bounds = pc->pages[index].bounds;
  else if (pc->count > 0)
    bounds = pc->pages[pc->count - 1].bounds;
  else
    bounds = fz_make_rect(0, 0, 612, 792);
  if (fz_is_empty_rect(bounds))
    bounds = fz_make_rect(0, 0, 612, 792);

  if (index >= pc->count)
  {
    // Append placeholders, more and more of them as the document grows
    int count = pc->count;
    while (count <= index)
    {
      count += pc->grow2;
      int grow = fz_mini(pc->grow1 + pc->grow2, MAX_GROWTH);
      pc->grow1 = pc->grow2;
      pc->grow2 = grow;
    }
    reserve(ctx, pc, count);
    for (int i = pc->count; i < count; i++)
    {
      pc->pages[i] = (txp_page){0,};
      pc->count = i + 1;
      set_page(ctx, pc, i, NULL, bounds);
    }
  }

  set_page(ctx, pc, index, dl, bounds);
  if (pc->valid <= index)
    pc->valid = index + 1;
}

void txp_page_collection_invalidate_after(txp_page_collection *pc, int count)
{
  if (count < 0)
    count = 0;
  if (pc->valid > count)
    pc->valid = count;
}

void txp_page_collection_truncate(fz_context *ctx, txp_page_collection *pc)
{
  if (pc->count == pc->valid)
    return;
  for (int i = pc->valid; i < pc->count; i++)
    clear_page(ctx, &pc->pages[i]);
  pc->count = pc->valid;
  pc->grow1 = pc->grow2 = 1;
  txp_prefix_sum_truncate(&pc->heights, pc->count);
}

bool txp_page_collection_get_crop(const txp_page_collection *pc)
{
  return pc->crop;
}

void txp_page_collection_set_crop(fz_context *ctx, txp_page_collection *pc, bool crop)
{
  if (pc->crop == crop)
    return;
  pc->crop = crop;
  for (int i = 0; i < pc->count; i++)
    txp_prefix_sum_set(&pc->heights, i, page_height(ctx, pc, &pc->pages[i]));
}

float txp_page_collection_top(const txp_page_collection *pc, int index, float sep)
{
  if (index > pc->count)
    index = pc->count;
  return txp_prefix_sum_query(&pc->heights, index, sep);
}

int txp_page_collection_find(const txp_page_collection *pc, float y, float sep)
{
  if (pc->count == 0)
    return -1;
  return txp_prefix_sum_find(&pc->heights, pc->count, y, sep);
}

float txp_page_collection_height(const txp_page_collection *pc, float sep)
{
  if (pc->count == 0)
    return 0;
  return txp_prefix_sum_query(&pc->heights, pc->count, sep) - sep;
}
