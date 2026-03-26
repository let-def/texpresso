#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "pagebuffer.h"

#define MARGIN 128

struct pageentry_s {
  fz_irect buffer_space;
  fz_irect cached;
};

struct pagebuffer_s
{
  int pow_w, pow_h;
  SDL_Renderer *renderer;
  SDL_Texture *texture;
  struct pageentry_s *entries;
  int first, last, cap;
  int first_line, last_line;
  void *scratch;
  size_t scratch_size;
};

pagebuffer_t *pagebuffer_new(SDL_Renderer *renderer)
{
  if (renderer == NULL)
    abort();

  pagebuffer_t *result = calloc(1, sizeof(pagebuffer_t));
  if (result == NULL)
    abort();

  result->renderer = renderer;
  return result;
}

void pagebuffer_free(pagebuffer_t *t)
{
  if (t->texture)
    SDL_DestroyTexture(t->texture);
  if (t->entries)
    free(t->entries);
  free(t);
  if (t->scratch)
    free(t->scratch);
}

static int pow2(size_t x)
{
  int result = 0;
  while ((1 << result) < x)
    result += 1;
  return result;
}

void pagebuffer_flush(pagebuffer_t *t)
{
  t->first = 0;
  t->last = 0;
  t->first_line = 0;
  t->last_line = -1;
}

void pagebuffer_invalidate_page(pagebuffer_t *t, int page)
{
  if (0 <= page && page < t->cap)
  {
    t->entries[page].cached.x0 = 0.0;
    t->entries[page].cached.x1 = 0.0;
  }
}

void pagebuffer_reserve(pagebuffer_t *t, size_t width, size_t height)
{
  int pow_w = pow2(width + 2 * MARGIN), pow_h = pow2(height + 2 * MARGIN);

  int tw = -1, th = -1;

  if (t->texture != NULL)
  {
    if (pow_w == t->pow_w && pow_h == t->pow_h)
      return;
    SDL_DestroyTexture(t->texture);
  }

  t->texture = SDL_CreateTexture(t->renderer, SDL_PIXELFORMAT_RGB24,
                                 SDL_TEXTUREACCESS_STREAMING, (1 << pow_w),
                                 (1 << pow_h));
  t->pow_w = pow_w;
  t->pow_h = pow_h;
  pagebuffer_flush(t);
}

static void pagebuffer_scroll_lines(pagebuffer_t *t, int first_line, int last_line)
{
  if (t->first_line <= first_line && last_line <= t->last_line)
  {
    /* Invalidate nothing */
    if (0) puts("invalidate nothing");
    return;
  }

  int first_line_pad = first_line - MARGIN;
  if (first_line_pad < 0)
    first_line_pad = 0;
  int last_line_pad = last_line + MARGIN;

  if (last_line_pad < t->first_line || first_line_pad > t->last_line)
  {
    /* Invalidate everything */
    if (0) puts("invalidate everything");
    t->first_line = first_line_pad;
    t->last_line = last_line_pad;
    t->first = 0;
    t->last = -1;
    return;
  }

  if (first_line_pad < t->first_line)
  {
    /* Invalidate beginning */
    if (0) puts("invalidate beginning");
    t->first_line = first_line_pad;
    if (t->last_line > t->first_line + (1 << t->pow_h))
    {
      t->last_line = t->first_line + (1 << t->pow_h);
      assert (last_line <= t->last_line);
    }
  }
  else
  {
    /* Invalidate ending */
    if (0) puts("invalidate ending");
    t->last_line = last_line_pad;
    if (t->first_line < t->last_line - (1 << t->pow_h))
    {
      t->first_line = t->last_line - (1 << t->pow_h);
      assert (t->first_line <= first_line);
    }
  }

  if (0) printf("first_line:%d\n", t->first_line);
  while (t->first <= t->last &&
         t->entries[t->first].buffer_space.y0 + t->entries[t->first].cached.y1 <= t->first_line)
  {
    if (0) puts("drop first page");
    t->first++;
  }

  if (t->first <= t->last &&
      t->entries[t->first].buffer_space.y0 + t->entries[t->first].cached.y0 < t->first_line)
  {
    if (0) puts("truncate prefix");
    t->entries[t->first].cached.y0 = t->first_line - t->entries[t->first].buffer_space.y0;
  }

  while (t->first <= t->last &&
         t->entries[t->last].buffer_space.y0 + t->entries[t->last].cached.y0 >= t->last_line)
  {
    if (0) puts("drop last page");
    t->last--;
  }

  if (t->first <= t->last &&
      t->entries[t->last].cached.y1 > t->last_line - t->entries[t->last].buffer_space.y0)
  {
    if (0) puts("truncate suffix");
    t->entries[t->last].cached.y1 = t->last_line - t->entries[t->last].buffer_space.y0;
  }
}

void pagebuffer_scroll(pagebuffer_t *t, int first_line, int last_line, int first_page, int last_page)
{
  if (0) printf("before first:%d last:%d\n", t->first, t->last);
  assert (first_page <= last_page);
  assert (first_line <= last_line);
  assert (last_line - first_line <= 1 << t->pow_h);
  pagebuffer_scroll_lines(t, first_line, last_line);
  if (0) printf("middle first:%d last:%d\n", t->first, t->last);
  if (t->cap <= last_page)
  {
    int cap = t->cap ? t->cap : 4;
    while (cap <= last_page)
      cap *= 2;
    t->entries = realloc(t->entries, sizeof(struct pageentry_s) * cap);
    if (t->entries == NULL)
      abort();
    memset(t->entries + t->cap, 0, sizeof(struct pageentry_s) * (cap - t->cap));
    t->cap = cap;
  }

  if (first_page > t->last + 1 || last_page + 1 < t->first || t->first > t->last)
  {
    if (0) puts("clear both");
    t->first = first_page;
    t->last = last_page;
  }
  else if (first_page < t->first)
  {
    if (0) puts("clear first");
    last_page = t->first - 1;
    t->first = first_page;
  }
  else
  {
    if (0) puts("clear last");
    first_page = t->last + 1;
    t->last = last_page;
  }
  for (int i = first_page; i <= last_page; i++)
  {
    if (0) printf("clear %d\n", i);
    t->entries[i] = (struct pageentry_s){0};
  }

  if (0) printf("after  first:%d last:%d\n", t->first, t->last);
}

static bool same_irect(fz_irect a, fz_irect b)
{
  return a.x0 == b.x0 && a.x1 == b.x1 && a.y0 == b.y0 && a.y1 == b.y1;
}

void pagebuffer_update_entry(pagebuffer_t *t, int page, fz_irect buffer_space,
                             fz_irect visible, fz_irect *cached, fz_irect *render)
{
  assert(t->first <= page && page <= t->last);
  struct pageentry_s *entry = &t->entries[page];
  if (0) printf("page:%d\n", page);
  if (0) printf(" cached:(%d,%d) (%d,%d)\n", entry->cached.x0, entry->cached.y0, entry->cached.x1, entry->cached.y1);
  if (0) printf(" visible:(%d,%d) (%d,%d)\n", visible.x0, visible.y0, visible.x1, visible.y1);

  bool diff = !same_irect(buffer_space, entry->buffer_space) ||
    fz_is_empty_irect(fz_intersect_irect(entry->cached, visible));

  if (diff)
  {
    entry->buffer_space = buffer_space;
    entry->cached = fz_make_irect(visible.x0, visible.y0,
                                  visible.x0, visible.y0);
  }
  *cached = entry->cached;

  if (diff || entry->cached.x0 > visible.x0)
  {
    entry->cached.x0 = fz_maxi(visible.x0 - MARGIN, 0);
    if (entry->cached.x1 > entry->cached.x0 + (1 << t->pow_w))
      entry->cached.x1 = entry->cached.x0 + (1 << t->pow_w);
  }

  if (diff || entry->cached.x1 < visible.x1)
  {
    entry->cached.x1 = fz_mini(visible.x1 + MARGIN, buffer_space.x1 - buffer_space.x0);
    if (entry->cached.x0 < entry->cached.x1 - (1 << t->pow_w))
      entry->cached.x0 = entry->cached.x1 - (1 << t->pow_w);
  }

  if (diff || entry->cached.y0 > visible.y0)
  {
    entry->cached.y0 = fz_maxi(visible.y0 - MARGIN, 0);
    if (entry->cached.y1 > entry->cached.y0 + (1 << t->pow_h))
      entry->cached.y1 = entry->cached.y0 + (1 << t->pow_h);
  }

  if (diff || entry->cached.y1 < visible.y1)
  {
    entry->cached.y1 = fz_mini(visible.y1 + MARGIN, buffer_space.y1 - buffer_space.y0);
    if (entry->cached.y0 < entry->cached.y1 - (1 << t->pow_h))
      entry->cached.y0 = entry->cached.y1 - (1 << t->pow_h);
  }

  *render = entry->cached;
}

int pagebuffer_debug_firstline(pagebuffer_t *t)
{
  return t->first_line;
}

int pagebuffer_debug_lastline(pagebuffer_t *t)
{
  return t->last_line;
}

static void update_texture_sub(SDL_Texture *tex, fz_irect source,
                               const uint8_t *data, size_t dpitch)
{
  uint8_t *pixels;
  int tpitch;
  SDL_Rect r = SDL_rect_from_irect(source);
  SDL_LockTexture(tex, &r, (void**)&pixels, &tpitch);

  int line_bytes = (source.x1 - source.x0) * 3;
  for (int y = source.y0; y < source.y1; y++)
  {
    memcpy(pixels, data, line_bytes);
    pixels += tpitch;
    data += dpitch;
  }

  SDL_UnlockTexture(tex);
}

SDL_Texture *pagebuffer_debug_get_texture(pagebuffer_t *t)
{
  return t->texture;
}

void pagebuffer_update_data(pagebuffer_t *t, fz_irect source,
                            const uint8_t *data, size_t pitch)
{
  if (fz_irect_is_empty(source))
    return;

  SDL_Texture *tex = t->texture;
  if (!tex)
    return;

  int w, h;
  SDL_QueryTexture(tex, NULL, NULL, &w, &h);

  if (pitch == 0)
    pitch = (source.x1 - source.x0) * 3;

  source.x0 %= w;
  source.x1 %= w;
  source.y0 %= h;
  source.y1 %= h;

  int dx = (w - source.x0) * 3;
  int dy = (h - source.y0) * pitch;

  if (source.x1 > source.x0)
  {
    if (source.y1 > source.y0)
    {
      update_texture_sub(tex, source, data, pitch);
    }
    else
    {
      update_texture_sub(tex, fz_make_irect(source.x0, source.y0, source.x1, h), data, pitch);
      update_texture_sub(tex, fz_make_irect(source.x0, 0, source.x1, source.y1), data + dy, pitch);
    }
  }
  else
  {
    if (source.y1 > source.y0)
    {
      update_texture_sub(tex, fz_make_irect(source.x0, source.y0, w, source.y1), data, pitch);
      update_texture_sub(tex, fz_make_irect(0, source.y0, source.x1, source.y1), data + dx, pitch);
    } else {
      update_texture_sub(tex, fz_make_irect(source.x0, source.y0, w, h), data, pitch);
      update_texture_sub(tex, fz_make_irect(0, source.y0, source.x1, h), data + dx, pitch);
      update_texture_sub(tex, fz_make_irect(source.x0, 0, w, source.y1), data + dy, pitch);
      update_texture_sub(tex, fz_make_irect(0, 0, source.x1, source.y1), data + dy + dx, pitch);
    }
  }
}

static void render_copy_fz(SDL_Renderer *ren, SDL_Texture *tex, int x, int y, fz_irect src)
{
  SDL_Rect ssrc = SDL_rect_from_irect(src);
  SDL_Rect sdst = {.x = x, .y = y, .w = ssrc.w, .h = ssrc.h};
  SDL_RenderCopy(ren, tex, &ssrc, &sdst);
}

void pagebuffer_blit(pagebuffer_t *t, int x, int y, fz_irect source)
{
  if (source.x0 == source.x1 || source.y0 == source.y1)
    return;

  SDL_Texture *tex = t->texture;
  if (!tex)
    return;

  int w, h;
  SDL_QueryTexture(tex, NULL, NULL, &w, &h);

  source.x0 %= w;
  source.x1 %= w;
  source.y0 %= h;
  source.y1 %= h;

  int dx = w - source.x0;
  int dy = h - source.y0;

  if (source.x1 > source.x0)
  {
    if (source.y1 > source.y0)
      render_copy_fz(t->renderer, tex, x, y, source);
    else
    {
      render_copy_fz(t->renderer, tex, x, y,
                     fz_make_irect(source.x0, source.y0, source.x1, h));
      render_copy_fz(t->renderer, tex, x, y + dy,
                     fz_make_irect(source.x0, 0, source.x1, source.y1));
    }
  } else {
    if (source.y1 > source.y0)
    {
      render_copy_fz(t->renderer, tex, x, y,
                     fz_make_irect(source.x0, source.y0, w, source.y1));
      render_copy_fz(t->renderer, tex, x + dx, y,
                     fz_make_irect(0, source.y0, source.x1, source.y1));
    } else {
      render_copy_fz(t->renderer, tex, x, y,
                     fz_make_irect(source.x0, source.y0, w, h));
      render_copy_fz(t->renderer, tex, x, y + dy,
                     fz_make_irect(source.x0, 0, w, source.y1));
      render_copy_fz(t->renderer, tex, x + dx, y,
                     fz_make_irect(0, source.y0, source.x1, h));
      render_copy_fz(t->renderer, tex, x + dx, y + dy,
                     fz_make_irect(0, 0, source.x1, source.y1));
    }
  }
}

int pagebuffer_delta_rect(fz_irect cached, fz_irect render, fz_irect output[4])
{
  fz_irect *o = output;

  *o = fz_make_irect(render.x0, render.y0, render.x1, cached.y0);
  if (!fz_is_empty_irect(*o))
    o++;

  *o = fz_make_irect(render.x0, cached.y0, cached.x0, cached.y1);
  if (!fz_is_empty_irect(*o))
    o++;

  *o = fz_make_irect(cached.x1, cached.y0, render.x1, cached.y1);
  if (!fz_is_empty_irect(*o))
    o++;

  *o = fz_make_irect(render.x0, cached.y1, render.x1, render.y1);
  if (!fz_is_empty_irect(*o))
    o++;

  return o - output;
}

static void *reserve_scratch(pagebuffer_t *t, int width, int height)
{
  size_t size = width * height * 3;
  if (t->scratch_size < size)
  {
    t->scratch = realloc(t->scratch, size);
    t->scratch_size = size;
    if (!t->scratch)
      abort();
  }
  return t->scratch;
}

void pagebuffer_update_data_from_display_list(fz_context *ctx, pagebuffer_t *t,
                                              fz_irect position,
                                              fz_irect subset,
                                              fz_display_list *dl,
                                              pagebuffer_filter filter,
                                              void *filter_data)
{
  fz_matrix ctm;

  // A. Rasterize display list

  // Calculate the scaling matrix
  // We map the display list's internal bounds to the 'position' rectangle
  fz_rect bounds = fz_bound_display_list(ctx, dl);

  // Calculate scaling to fit 'bounds' into 'position'
  float sw = (float)(position.x1 - position.x0) / (bounds.x1 - bounds.x0);
  float sh = (float)(position.y1 - position.y0) / (bounds.y1 - bounds.y0);

  // Build the transformation:
  // Translate to origin -> Scale -> Translate to target position
  ctm = fz_translate(-bounds.x0, -bounds.y0);
  ctm = fz_concat(ctm, fz_scale(sw, sh));

  // Create pixmap (this defines our output buffer size)
  uint8_t *data = reserve_scratch(t, subset.x1 - subset.x0, subset.y1 - subset.y0);
  fz_pixmap *pix = fz_new_pixmap_with_bbox_and_data(ctx, fz_device_rgb(ctx), subset, NULL, 0, data);
  fz_clear_pixmap_with_value(ctx, pix, 255);

  // Draw
  fz_device *dev = fz_new_draw_device(ctx, fz_identity, pix);
  fz_run_display_list(ctx, dl, dev, ctm, fz_rect_from_irect(subset), NULL);

  fz_close_device(ctx, dev);
  fz_drop_device(ctx, dev);

  fz_drop_pixmap(ctx, pix);

  // B. Upload to texture

  subset.x0 += position.x0;
  subset.y0 += position.y0;
  subset.x1 += position.x0;
  subset.y1 += position.y0;

  int w = subset.x1 - subset.x0;
  int h = subset.y1 - subset.y0;

  if (filter)
    for (int y = 0; y < h; y++)
      filter(filter_data, data + y * w * 3, w);

  pagebuffer_update_data(t, subset, data, 0);
}

void pagebuffer_update_entry_from_display_list(fz_context *ctx, pagebuffer_t *t,
                                               int page, fz_irect position,
                                               fz_irect visible,
                                               fz_display_list *dl,
                                               pagebuffer_filter filter,
                                               void *filter_data)
{
  fz_irect cached_rect, render_rect;
  pagebuffer_update_entry(t, page, position, visible, &cached_rect, &render_rect);

  fz_irect delta[4];
  int count = pagebuffer_delta_rect(cached_rect, render_rect, delta);
  int w = position.x1 - position.x0;
  int h = position.y1 - position.y0;

  for (int i = 0; i < count; i++)
    pagebuffer_update_data_from_display_list(ctx, t, position, delta[i], dl, filter, filter_data);
}

// Return the relative subset of absolute rectangle that intersects canvas
fz_irect pagebuffer_relative_clipped_area(fz_irect absolute, fz_irect canvas)
{
  fz_irect inter = fz_intersect_irect(absolute, canvas);
  inter.x0 -= absolute.x0;
  inter.y0 -= absolute.y0;
  inter.x1 -= absolute.x0;
  inter.y1 -= absolute.y0;
  return inter;
}
