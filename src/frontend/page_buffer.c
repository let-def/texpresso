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

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "page_buffer.h"

// Extra pixels rasterized around the visible area, so that scrolling a bit
// does not need rasterizing
#define MARGIN 128

// Texture dimensions are rounded to multiples of this
#define TEXTURE_GRANULARITY 256

typedef struct
{
  fz_irect position;  // the page in buffer space
  fz_irect cached;    // part of position in the texture, in buffer space
} page_entry;

struct txp_page_buffer_s
{
  SDL_Renderer *renderer;
  SDL_Texture *texture;
  int tw, th;
  int max_w, max_h;

  // The texture caches buffer space rows [window, window + th)
  int window;

  page_entry *entries;
  int entry_count;

  uint8_t *scratch;
  size_t scratch_size;
};

txp_page_buffer *txp_page_buffer_new(fz_context *ctx, SDL_Renderer *renderer)
{
  txp_page_buffer *pb = fz_malloc_struct(ctx, txp_page_buffer);
  pb->renderer = renderer;
  SDL_RendererInfo info;
  if (SDL_GetRendererInfo(renderer, &info) == 0)
  {
    pb->max_w = info.max_texture_width;
    pb->max_h = info.max_texture_height;
  }
  return pb;
}

void txp_page_buffer_free(fz_context *ctx, txp_page_buffer *pb)
{
  if (pb->texture)
    SDL_DestroyTexture(pb->texture);
  fz_free(ctx, pb->entries);
  fz_free(ctx, pb->scratch);
  fz_free(ctx, pb);
}

void txp_page_buffer_flush(txp_page_buffer *pb)
{
  for (int i = 0; i < pb->entry_count; i++)
    pb->entries[i].cached = fz_empty_irect;
}

void txp_page_buffer_invalidate_page(txp_page_buffer *pb, int page)
{
  if (page >= 0 && page < pb->entry_count)
    pb->entries[page].cached = fz_empty_irect;
}

static int texture_size(int needed, int max)
{
  int size = ((needed + TEXTURE_GRANULARITY - 1) / TEXTURE_GRANULARITY) *
             TEXTURE_GRANULARITY;
  if (max > 0 && size > max)
    size = max;
  return size;
}

static fz_irect clip_rows(fz_irect r, int y0, int y1)
{
  return fz_intersect_irect(r, fz_make_irect(INT_MIN / 2, y0, INT_MAX / 2, y1));
}

bool txp_page_buffer_begin(fz_context *ctx, txp_page_buffer *pb,
                           int screen_w, int screen_h, int y0, int y1)
{
  int tw = texture_size(screen_w + 2 * MARGIN, pb->max_w);
  int th = texture_size(screen_h + 2 * MARGIN, pb->max_h);

  // Reallocate when too small, or much larger than needed
  if (!pb->texture || pb->tw < tw || pb->th < th ||
      pb->tw > tw + 2 * TEXTURE_GRANULARITY ||
      pb->th > th + 2 * TEXTURE_GRANULARITY)
  {
    if (pb->texture)
      SDL_DestroyTexture(pb->texture);
    pb->texture = SDL_CreateTexture(pb->renderer, SDL_PIXELFORMAT_RGB24,
                                    SDL_TEXTUREACCESS_STREAMING, tw, th);
    txp_page_buffer_flush(pb);
    if (!pb->texture)
    {
      fprintf(stderr, "[page buffer] cannot create a %dx%d texture: %s\n",
              tw, th, SDL_GetError());
      pb->tw = pb->th = 0;
      return false;
    }
    pb->tw = tw;
    pb->th = th;
    pb->window = y0 - MARGIN;
  }

  // Move the window to cover [y0, y1), with some margin in the direction
  // of the motion
  if (y1 > y0 + pb->th)
    y1 = y0 + pb->th;
  int window = pb->window;
  if (y0 < window)
    window = y0 - MARGIN;
  else if (y1 > window + pb->th)
    window = y1 + MARGIN - pb->th;
  if (window > y0)
    window = y0;

  if (window != pb->window)
  {
    pb->window = window;
    for (int i = 0; i < pb->entry_count; i++)
      pb->entries[i].cached =
          clip_rows(pb->entries[i].cached, window, window + pb->th);
  }

  return true;
}

static void *reserve_scratch(fz_context *ctx, txp_page_buffer *pb, size_t size)
{
  if (pb->scratch_size < size)
  {
    fz_free(ctx, pb->scratch);
    pb->scratch = NULL;
    pb->scratch_size = 0;
    pb->scratch = fz_malloc(ctx, size);
    pb->scratch_size = size;
  }
  return pb->scratch;
}

static int pos_mod(int x, int m)
{
  int r = x % m;
  return r < 0 ? r + m : r;
}

// Split a rectangle of buffer space in at most four parts that do not wrap
// around the texture. For each part, set tex (rectangle in the texture) and
// dx, dy (offset of the part in the rectangle).
static int split_rect(txp_page_buffer *pb, fz_irect r, SDL_Rect tex[4],
                      int dx[4], int dy[4])
{
  int w = r.x1 - r.x0, h = r.y1 - r.y0;
  int tx = pos_mod(r.x0, pb->tw), ty = pos_mod(r.y0, pb->th);
  int w0 = fz_mini(w, pb->tw - tx), h0 = fz_mini(h, pb->th - ty);
  int n = 0;

  for (int j = 0; j < 2; j++)
  {
    int y = j == 0 ? ty : 0;
    int ph = j == 0 ? h0 : h - h0;
    if (ph <= 0) continue;
    for (int i = 0; i < 2; i++)
    {
      int x = i == 0 ? tx : 0;
      int pw = i == 0 ? w0 : w - w0;
      if (pw <= 0) continue;
      tex[n] = (SDL_Rect){.x = x, .y = y, .w = pw, .h = ph};
      dx[n] = i == 0 ? 0 : w0;
      dy[n] = j == 0 ? 0 : h0;
      n++;
    }
  }
  return n;
}

static void upload(txp_page_buffer *pb, fz_irect r, const uint8_t *data, int pitch)
{
  SDL_Rect tex[4];
  int dx[4], dy[4];
  int n = split_rect(pb, r, tex, dx, dy);
  for (int i = 0; i < n; i++)
    if (SDL_UpdateTexture(pb->texture, &tex[i],
                          data + dy[i] * pitch + dx[i] * 3, pitch) != 0)
      fprintf(stderr, "[page buffer] cannot update texture: %s\n", SDL_GetError());
}

// Rasterize r (in buffer space) of a page
static void rasterize(fz_context *ctx, txp_page_buffer *pb, fz_irect position,
                      fz_irect r, fz_display_list *dl, fz_rect area,
                      txp_page_buffer_filter filter, void *filter_data)
{
  int w = r.x1 - r.x0, h = r.y1 - r.y0;
  if (w <= 0 || h <= 0)
    return;

  int pitch = w * 3;
  uint8_t *data = reserve_scratch(ctx, pb, (size_t)pitch * h);
  memset(data, 255, (size_t)pitch * h);

  if (dl && area.x1 > area.x0 && area.y1 > area.y0)
  {
    // Rasterize relative to the page, buffer coordinates can be large
    fz_irect bbox = fz_make_irect(r.x0 - position.x0, r.y0 - position.y0,
                                  r.x1 - position.x0, r.y1 - position.y0);
    float sx = (float)(position.x1 - position.x0) / (area.x1 - area.x0);
    float sy = (float)(position.y1 - position.y0) / (area.y1 - area.y0);
    fz_matrix ctm = fz_concat(fz_translate(-area.x0, -area.y0), fz_scale(sx, sy));

    fz_pixmap *pix = NULL;
    fz_device *dev = NULL;
    fz_var(pix);
    fz_var(dev);
    fz_try(ctx)
    {
      pix = fz_new_pixmap_with_bbox_and_data(ctx, fz_device_rgb(ctx), bbox,
                                             NULL, 0, data);
      dev = fz_new_draw_device(ctx, fz_identity, pix);
      fz_run_display_list(ctx, dl, dev, ctm, fz_rect_from_irect(bbox), NULL);
      fz_close_device(ctx, dev);
    }
    fz_always(ctx)
    {
      fz_drop_device(ctx, dev);
      fz_drop_pixmap(ctx, pix);
    }
    fz_catch(ctx)
      // Keep the partial result: retrying would fail again
      fprintf(stderr, "[page buffer] rendering failed: %s\n",
              fz_caught_message(ctx));
  }

  if (filter)
    for (int y = 0; y < h; y++)
      filter(filter_data, data + y * pitch, w);

  upload(pb, r, data, pitch);
}

static bool irect_equal(fz_irect a, fz_irect b)
{
  return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

static bool contains(fz_irect outer, fz_irect inner)
{
  return outer.x0 <= inner.x0 && outer.y0 <= inner.y0 &&
         inner.x1 <= outer.x1 && inner.y1 <= outer.y1;
}

bool txp_page_buffer_update(fz_context *ctx, txp_page_buffer *pb, int page,
                            fz_irect position, fz_irect visible,
                            fz_display_list *dl, fz_rect area,
                            txp_page_buffer_filter filter, void *filter_data)
{
  if (!pb->texture || page < 0)
    return false;

  if (page >= pb->entry_count)
  {
    int count = pb->entry_count ? pb->entry_count : 8;
    while (count <= page)
      count *= 2;
    page_entry *entries = fz_malloc_struct_array(ctx, count, page_entry);
    if (pb->entry_count > 0)
      memcpy(entries, pb->entries, sizeof(page_entry) * pb->entry_count);
    fz_free(ctx, pb->entries);
    pb->entries = entries;
    pb->entry_count = count;
  }

  page_entry *e = &pb->entries[page];
  if (!irect_equal(e->position, position))
  {
    e->position = position;
    e->cached = fz_empty_irect;
  }

  int wy0 = pb->window, wy1 = pb->window + pb->th;
  visible = clip_rows(fz_intersect_irect(visible, position), wy0, wy1);
  if (visible.x1 - visible.x0 > pb->tw)
    visible.x1 = visible.x0 + pb->tw;
  if (fz_is_empty_irect(visible) || contains(e->cached, visible))
    return false;

  // Cache the visible part with some margin
  fz_irect want = fz_make_irect(visible.x0 - MARGIN, visible.y0 - MARGIN,
                                visible.x1 + MARGIN, visible.y1 + MARGIN);
  want = clip_rows(fz_intersect_irect(want, position), wy0, wy1);
  if (want.x1 - want.x0 > pb->tw)
  {
    int extra = pb->tw - (visible.x1 - visible.x0);
    want.x0 = fz_maxi(want.x0, visible.x0 - extra / 2);
    want.x1 = fz_mini(want.x1, want.x0 + pb->tw);
  }

  // Pages of a different layout (another zoom level) might occupy the same
  // rows of the texture
  for (int i = 0; i < pb->entry_count; i++)
  {
    page_entry *o = &pb->entries[i];
    if (i != page && !fz_is_empty_irect(o->cached) &&
        o->cached.y0 < want.y1 && want.y0 < o->cached.y1)
      o->cached = fz_empty_irect;
  }

  // Rasterize what is not cached yet
  fz_irect keep = fz_intersect_irect(e->cached, want);
  if (fz_is_empty_irect(keep))
    rasterize(ctx, pb, position, want, dl, area, filter, filter_data);
  else
  {
    fz_irect parts[4] = {
      fz_make_irect(want.x0, want.y0, want.x1, keep.y0),
      fz_make_irect(want.x0, keep.y1, want.x1, want.y1),
      fz_make_irect(want.x0, keep.y0, keep.x0, keep.y1),
      fz_make_irect(keep.x1, keep.y0, want.x1, keep.y1),
    };
    for (int i = 0; i < 4; i++)
      if (!fz_is_empty_irect(parts[i]))
        rasterize(ctx, pb, position, parts[i], dl, area, filter, filter_data);
  }
  e->cached = want;
  return true;
}

void txp_page_buffer_blit(txp_page_buffer *pb, fz_irect rect, int x, int y)
{
  if (!pb->texture || fz_is_empty_irect(rect))
    return;
  SDL_Rect tex[4];
  int dx[4], dy[4];
  int n = split_rect(pb, rect, tex, dx, dy);
  for (int i = 0; i < n; i++)
  {
    SDL_Rect dst = {.x = x + dx[i], .y = y + dy[i], .w = tex[i].w, .h = tex[i].h};
    SDL_RenderCopy(pb->renderer, pb->texture, &tex[i], &dst);
  }
}
