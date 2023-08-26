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

#include "renderer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static float clampf(float x, float min, float max)
{
  if (min > max)
    return 0;
  if (x < min)
    return min;
  if (x > max)
    return max;
  return x;
}

typedef struct
{
  int w, h;
  int x, y;
  fz_irect rect;
  float scale;
} texture_state;

struct txp_renderer_s
{
  SDL_Renderer *sdl;
  int output_w, output_h;

  fz_buffer *scratch;
  fz_display_list *contents;
  fz_stext_page *stext;
  int contents_bounds_valid;
  fz_rect contents_bounds;
  txp_renderer_config config;

  SDL_Texture *tex;
  texture_state st;
  fz_point selection_start;
  fz_rect selections[40];
  int selection_count;
  fz_point scale_factor;

  uint32_t cached_bg, cached_fg;
};

static void txp_get_colors(txp_renderer_config *config, uint32_t *bg, uint32_t *fg)
{
  if (!config->invert_color)
  {
    *bg = config->background_color & 0xFFFFFF;
    *fg = config->foreground_color & 0xFFFFFF;
  }
  else
  {
    *fg = config->background_color & 0xFFFFFF;
    *bg = config->foreground_color & 0xFFFFFF;
  }
}

txp_renderer *txp_renderer_new(fz_context *ctx, SDL_Renderer *sdl)
{
  txp_renderer *self;
  fz_try(ctx)
  {
    self = fz_malloc_struct(ctx, txp_renderer);
    self->sdl = sdl;
    self->config.zoom = 1;
    self->config.background_color = 0xFFFFFF;
    self->config.foreground_color = 0x000000;
    self->config.invert_color = 0;
  }
  fz_catch(ctx)
  {
    if (self)
      fz_free(ctx, self);
    fz_rethrow(ctx);
  }
  return self;
}

void txp_renderer_free(fz_context *ctx, txp_renderer *self)
{
  if (self->contents)
    fz_drop_display_list(ctx, self->contents);
  if (self->stext)
    fz_drop_stext_page(ctx, self->stext);
  if (self->tex)
    SDL_DestroyTexture(self->tex);
  if (self->scratch)
    fz_drop_buffer(ctx, self->scratch);
  fz_free(ctx, self);
}

static void update_renderer_size(txp_renderer *self)
{
  SDL_GetRendererOutputSize(self->sdl, &self->output_w, &self->output_h);
}

static void clear_texture(txp_renderer *self)
{
  self->st.x = 0;
  self->st.y = 0;
  self->st.rect = fz_make_irect(0, 0, 0, 0);
}

void txp_renderer_set_contents(fz_context *ctx, txp_renderer *self, fz_display_list *dl)
{
  if (self->contents == dl)
    return;
  fz_keep_display_list(ctx, dl);
  if (self->contents)
    fz_drop_display_list(ctx, self->contents);
  if (self->stext)
    fz_drop_stext_page(ctx, self->stext);
  self->stext = NULL;
  self->contents = dl;
  clear_texture(self);
  self->contents_bounds_valid = 0;
  self->selection_count = 0;
}

fz_display_list *txp_renderer_get_contents(fz_context *ctx, txp_renderer *self)
{
  return self->contents;
}

txp_renderer_config *txp_renderer_get_config(fz_context *ctx, txp_renderer *self)
{
  return &self->config;
}

void txp_renderer_config_changed(fz_context *ctx, txp_renderer *self)
{
}

static fz_rect get_bounds(fz_context *ctx, txp_renderer *self)
{
  fz_rect bounds = fz_bound_display_list(ctx, self->contents);

  if (!self->config.crop)
    return bounds;

  if (!self->contents_bounds_valid)
  {
    self->contents_bounds = fz_empty_rect;
    fz_device * dev = fz_new_bbox_device(ctx, &self->contents_bounds);
    fz_run_display_list(ctx, self->contents, dev, fz_identity, bounds, NULL);
    fz_close_device(ctx, dev);
    fz_drop_device(ctx, dev);
    self->contents_bounds = fz_intersect_rect(bounds, self->contents_bounds);
    self->contents_bounds_valid = 1;
  }

  return self->contents_bounds;
}

static fz_stext_page *get_stext(fz_context *ctx, txp_renderer *self)
{
  if (self->stext == NULL)
  {
    if (self->contents == NULL)
      return NULL;
    fz_rect bounds = fz_bound_display_list(ctx, self->contents);
    self->stext = fz_new_stext_page(ctx, bounds);
    fz_device *dev = fz_new_stext_device(ctx, self->stext, NULL);
    fz_run_display_list(ctx, self->contents, dev, fz_identity, bounds, NULL);
    fz_close_device(ctx, dev);
    fz_drop_device(ctx, dev);
  }
  return self->stext;
}

int txp_renderer_page_position(fz_context *ctx, txp_renderer *self, SDL_FRect *prect, fz_point *ptranslate, float *pscale)
{
  if (!self->contents)
    return 0;

  update_renderer_size(self);

  if (self->output_w <= 0 || self->output_h <= 0)
    return 0;

  fz_rect bounds = get_bounds(ctx, self);

  float out_ar = (float)self->output_w / (float)self->output_h;
  float doc_ar = (bounds.x1 - bounds.x0) / (bounds.y1 - bounds.y0);

  float doc_w, doc_h;

  if (out_ar <= doc_ar || self->config.fit == FIT_WIDTH)
  {
    doc_w = self->output_w * self->config.zoom;
    doc_h = doc_w / doc_ar;
  }
  else
  {
    doc_h = self->output_h * self->config.zoom;
    doc_w = doc_h * doc_ar;
  }

  float cx = (doc_w - self->output_w) / 2.0;
  float cy = (doc_h - self->output_h) / 2.0;

  self->config.pan.x = clampf(self->config.pan.x, -cx, cx);
  self->config.pan.y = clampf(self->config.pan.y, -cy, cy);
  // fprintf(stderr, "after clamp: %.02f, %.02f\n", r->vp.pan.x, r->vp.pan.y);

  // fprintf(stderr, "doc size: (%.02f, %.02f), out size: (%d, %d)\n",
  //         doc_w, doc_h, r->size.w, r->size.h);
  // fprintf(stderr, "out_ar: %.02f, doc_ar: %.02f, cx: %.02f, cy: %.02f\n",
  //         out_ar, doc_ar, cx, cy);

  float scale = doc_w / (bounds.x1 - bounds.x0);
  float tx = self->config.pan.x - cx;
  float ty = self->config.pan.y - cy;

  if (prect)
    *prect = (SDL_FRect){.x = tx, .y = ty, .w = doc_w, .h = doc_h};

  if (ptranslate)
    *ptranslate = fz_make_point(tx - bounds.x0 * scale, ty - bounds.y0 * scale);

  if (pscale)
    *pscale = scale;

  return 1;
}

static int ceil_pow2(int i)
{
  int r = 1;
  while (r < i)
    r *= 2;
  return r;
}

static void prepare_texture(fz_context *ctx, txp_renderer *self)
{
  int pw = ceil_pow2(self->output_w), ph = ceil_pow2(self->output_h);

  if (self->tex)
  {
    int tw, th;
    SDL_QueryTexture(self->tex, NULL, NULL, &tw, &th);
    if (tw != pw || th != ph)
    {
      SDL_DestroyTexture(self->tex);
      self->tex = NULL;
    }
  }

  if (!self->tex)
  {
    self->tex = SDL_CreateTexture(self->sdl, SDL_PIXELFORMAT_BGR24,
                                  SDL_TEXTUREACCESS_STREAMING, pw, ph);
    self->st = (texture_state){
        .w = pw,
        .h = ph,
        0,
    };
  }
}

static int fz_irect_area(fz_irect r)
{
  return (r.x1 - r.x0) * (r.y1 - r.y0);
}

#define remap(v, bp, wp) (bp) + ((v) * (wp - bp)) / 255

static void invert_pixmap(fz_context *ctx, fz_pixmap *pix, uint32_t black, uint32_t white)
{
  uint8_t *data0 = fz_pixmap_samples(ctx, pix);
  int stride = fz_pixmap_stride(ctx, pix);
  int width = fz_pixmap_width(ctx, pix);
  int height = fz_pixmap_height(ctx, pix);

  // 0x282c34
  uint8_t dark[3] = {
      (black >> 0) & 0xFF,
      (black >> 8) & 0xFF,
      (black >> 16) & 0xFF,
  };
  uint8_t light[3] = {
      (white >> 0) & 0xFF,
      (white >> 8) & 0xFF,
      (white >> 16) & 0xFF,
  };

  for (int y = 0; y < height; ++y)
  {
    uint8_t *data = data0 + stride * y;
    for (int x = 0; x < width; ++x, data += 3)
    {
      data[0] = remap(data[0], dark[0], light[0]);
      data[1] = remap(data[1], dark[1], light[1]);
      data[2] = remap(data[2], dark[2], light[2]);
    }
  }
}

static void render_rect(fz_context *ctx, txp_renderer *self, fz_rect bounds, void *pixels, int pitch,
                        int x, int y, fz_irect r, float scale)
{
  fz_colorspace *csp = fz_device_bgr(ctx);
  if (pitch == 0)
    pitch = fz_irect_width(r) * 3;
  fz_pixmap *pm = fz_new_pixmap_with_data(ctx, csp, fz_irect_width(r), fz_irect_height(r), NULL, 0, pitch, pixels);
  fz_matrix ctm;
  ctm = fz_translate(-x, -y);
  ctm = fz_pre_scale(ctm, scale, scale);
  ctm = fz_pre_translate(ctm, -bounds.x0, -bounds.y0);
  fz_device *dev = fz_new_draw_device(ctx, ctm, pm);
  fz_clear_pixmap_with_value(ctx, pm, 255);

  bounds.x0 += x / scale;
  bounds.y0 += y / scale;
  bounds.x1 = bounds.x0 + (r.x1 - r.x0) / scale;
  bounds.y1 = bounds.y0 + (r.y1 - r.y0) / scale;

  fz_point p0 = fz_transform_point_xy(bounds.x0, bounds.y0, ctm);
  fz_point p1 = fz_transform_point_xy(bounds.x1, bounds.y1, ctm);
  fprintf(stderr, "optimized bounds: %f,%f - %f,%f\n", p0.x, p0.y, p1.x, p1.y);

  fz_run_display_list(ctx, self->contents, dev, fz_identity, bounds, NULL);
  fz_close_device(ctx, dev);
  fz_drop_device(ctx, dev);

  uint32_t bg, fg;
  txp_get_colors(&self->config, &bg, &fg);
  if (bg != 0x00FFFFFF && fg != 0x00000000)
    invert_pixmap(ctx, pm, fg, bg);
  fz_drop_pixmap(ctx, pm);
}

static void render_inc_rect(fz_context *ctx, txp_renderer *self, fz_rect bounds, void *pixels,
                        int x, int y, fz_irect n, fz_irect r, float scale)
{
  render_rect(ctx, self, bounds, pixels, 0, x + r.x0 - n.x0, y + r.y0 - n.y0, r, scale);
}

static void update_sdl_texture(SDL_Texture *t, int pitch, void *pixels,
                               int x0, int y0, int x1, int y1)
{
  if (x0 < x1 && y0 < y1)
  {
    SDL_Rect r = (SDL_Rect){.x=x0, .y=y0, .w=x1-x0, .h=y1-y0};
    SDL_UpdateTexture(t, &r, pixels, pitch);
  }
}

static void upload_texture_rect(SDL_Texture *tex, fz_irect rect, void *pixels)
{
  // Size of target texture
  int tw, th;
  SDL_QueryTexture(tex, NULL, NULL, &tw, &th);

  // Size of rectangle
  int rw = rect.x1 - rect.x0;
  int rh = rect.y1 - rect.y0;

  // Normalized coordinates
  int x0 = rect.x0 % tw;
  int y0 = rect.y0 % th;
  int x1 = x0 + rw;
  int y1 = y0 + rh;

  // Sanity check
  if (rw > tw) abort();
  if (rh > th) abort();

  // Split image in 9 patches, eventually empty:
  //  --------------
  // | tl | tc | tr |
  // |--------------|
  // | ml | mc | mr |
  // |--------------|
  // | bl | bc | br |
  //  --------------

  int pitch = rw * 3;
  int dc = fz_maxi(-x0, 0) * 3;
  int dr = (tw - x0) * 3;
  int dm = fz_maxi(-y0, 0) * pitch;
  int db = (th - y0) * pitch;

  struct coord { int c0, c1, delta;}
  x[3] = {
    {.c0 = tw + x0     , .c1 = tw + fz_mini(x1, 0) , .delta = 0  },
    {.c0 = fz_maxi(x0, 0) , .c1 = fz_mini(x1, tw)     , .delta = dc },
    {.c0 = 0           , .c1 = x1 - tw          , .delta = dr },
  },
  y[3] = {
    {.c0 = th + y0     , .c1 = th + fz_mini(y1, 0) , .delta = 0  },
    {.c0 = fz_maxi(y0, 0) , .c1 = fz_mini(y1, th)     , .delta = dm },
    {.c0 = 0           , .c1 = y1 - th          , .delta = db },
  };

  // fprintf(stderr, "[patch] begin upload from (%d,%d) to (%d,%d)\n",
  //         rect.x0, rect.y0, rect.x1, rect.y1);

  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
    {
      // fprintf(stderr, "[patch] p(%d,%d) in texture sized %dx%d =\n", i, j, tw, th);
      // fprintf(stderr, " rect from (%d,%d) to (%d,%d) (width: %d, height: %d)\n",
      //         x[i].c0, y[j].c0, x[i].c1, y[j].c1,
      //         x[i].c1 - x[i].c0, y[j].c1 - y[j].c0);
      // fprintf(stderr, " delta = %d (dx:%d dy:%d)\n", x[i].delta + y[j].delta,
      //         x[i].delta / 3, y[j].delta / pitch);
      update_sdl_texture(tex, pitch, pixels + x[i].delta + y[j].delta, x[i].c0,
                         y[j].c0, x[i].c1, y[j].c1);
    }
}

static void render_sdl_texture(SDL_Renderer *r, SDL_Texture *t,
                               int rx, int ry,
                               int x0, int y0, int x1, int y1)
{
  if (x0 < x1 && y0 < y1)
  {
    int w = x1 - x0;
    int h = y1 - y0;
    SDL_Rect src = (SDL_Rect){.x=x0, .y=y0, .w=w, .h=h};
    SDL_Rect dst = (SDL_Rect){.x=rx, .y=ry, .w=w, .h=h};
    SDL_RenderCopy(r, t, &src, &dst);
  }
}


typedef struct timespec stopclock_t;

static void stopclock_start(stopclock_t *sc)
{
  clock_gettime(CLOCK_MONOTONIC, sc);
}

static int stopclock_reset_us(stopclock_t *sc)
{
  stopclock_t stop;
  clock_gettime(CLOCK_MONOTONIC, &stop);
  int result = ((stop.tv_sec - sc->tv_sec) * 1000 * 1000) + (stop.tv_nsec - sc->tv_nsec) / 1000;
  *sc = stop;
  return result;
}

static void render_texture_rect(SDL_Renderer *self, int rx, int ry, SDL_Texture *t, fz_irect rect)
{
  // Size of source texture
  int tw, th;
  SDL_QueryTexture(t, NULL, NULL, &tw, &th);

  // Size of rectangle
  int rw = rect.x1 - rect.x0;
  int rh = rect.y1 - rect.y0;

  // Normalized coordinates
  int x0 = rect.x0 % tw;
  int y0 = rect.y0 % th;
  int x1 = x0 + rw;
  int y1 = y0 + rh;

  // Sanity check
  if (rw > tw) abort();
  if (rh > th) abort();

  // Split image in 9 patches, eventually empty:
  //  --------------
  // | tl | tc | tr |
  // |--------------|
  // | ml | mc | mr |
  // |--------------|
  // | bl | bc | br |
  //  --------------

  struct coord { int c0, c1, delta; }
  x[3] = {
      {.c0 = tw + x0     , .c1 = tw + fz_mini(x1, 0) , .delta = 0           },
      {.c0 = fz_maxi(x0, 0) , .c1 = fz_mini(x1, tw)     , .delta = fz_maxi(-x0, 0) },
      {.c0 = 0           , .c1 = x1 - tw          , .delta = tw - x0     },
  },
  y[3] = {
      {.c0 = th + y0     , .c1 = th + fz_mini(y1, 0) , .delta = 0           },
      {.c0 = fz_maxi(y0, 0) , .c1 = fz_mini(y1, th)     , .delta = fz_maxi(-y0, 0) },
      {.c0 = 0           , .c1 = y1 - th          , .delta = th - y0     },
  };

  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      render_sdl_texture(self, t,
                         rx + x[i].delta,
                         ry + y[j].delta,
                         x[i].c0, y[j].c0, x[i].c1, y[j].c1);

}

static void update_texture(fz_context *ctx,
                           txp_renderer *self,
                           const SDL_FRect *page_rect,
                           const SDL_FRect *view_rect)
{
  SDL_FRect tex_rect = (SDL_FRect){
      .x = view_rect->x - page_rect->x,
      .y = view_rect->y - page_rect->y,
      .w = view_rect->w,
      .h = view_rect->h,
  };
  prepare_texture(ctx, self);

  int x = tex_rect.x;
  int y = tex_rect.y;
  int w = tex_rect.w;
  int h = tex_rect.h;

  fz_rect bounds = get_bounds(ctx, self);
  float doc_w = bounds.x1 - bounds.x0;
  float scale = (page_rect->w / doc_w);

  int done = 0;

  if (scale != self->st.scale)
  {
    self->st.scale = scale;
    // TODO: Full rerender
  }
  else if (self->st.x != x ||
           self->st.y != y ||
           fz_irect_width(self->st.rect) != w ||
           fz_irect_height(self->st.rect) != h)
  {
    // TODO: Incremental rerender
    // Find reusable area in texture
    fz_irect o = self->st.rect;
    fz_irect n;
    n.x0 = o.x0 - self->st.x + x,
    n.y0 = o.y0 - self->st.y + y,
    n.x1 = n.x0 + w;
    n.y1 = n.y0 + h;
    self->st.x = x;
    self->st.y = y;
    self->st.rect = n;

    fz_irect overlap = fz_intersect_irect(o, n);
    if (fz_is_empty_irect(overlap))
      fprintf(stderr, "No overlap, rerendering full texture\n");
    else
    {
      fprintf(stderr, "Overlap: %d pixels\n", fz_irect_area(overlap));

      fz_irect tl = fz_make_irect(n.x0, n.y0, fz_mini(n.x1, o.x0), fz_mini(n.y1, o.y1));
      fz_irect tr = fz_make_irect(fz_maxi(o.x0, n.x0), n.y0, n.x1, fz_mini(n.y1, o.y0));
      fz_irect bl = fz_make_irect(n.x0, fz_maxi(n.y0, o.y1), fz_mini(n.x1, o.x1), n.y1);
      fz_irect br = fz_make_irect(fz_maxi(n.x0, o.x1), fz_maxi(n.y0, o.y0), n.x1, n.y1);

      int new_pixels = fz_irect_area(tl);
      new_pixels = fz_maxi(new_pixels, fz_irect_area(tr));
      new_pixels = fz_maxi(new_pixels, fz_irect_area(bl));
      new_pixels = fz_maxi(new_pixels, fz_irect_area(br));

      if (self->scratch == NULL)
        self->scratch = fz_new_buffer(ctx, new_pixels * 3);
      else if (self->scratch->len < new_pixels * 3)
        fz_resize_buffer(ctx, self->scratch, new_pixels * 3);

      void *pixels = self->scratch->data;

      stopclock_t sc;
      stopclock_start(&sc);

      if (!fz_is_empty_irect(tl))
      {
        render_inc_rect(ctx, self, bounds, pixels, x, y, n, tl, scale);
        fprintf(stderr, "render tl: %dus\n", stopclock_reset_us(&sc));
        upload_texture_rect(self->tex, tl, pixels);
        fprintf(stderr, "upload tl: %dus\n", stopclock_reset_us(&sc));
        fprintf(stderr, "  tl: %d pixels\n", fz_irect_area(tl));
      }

      if (!fz_is_empty_irect(tr))
      {
        render_inc_rect(ctx, self, bounds, pixels, x, y, n, tr, scale);
        fprintf(stderr, "render tr: %dus\n", stopclock_reset_us(&sc));
        upload_texture_rect(self->tex, tr, pixels);
        fprintf(stderr, "upload tr: %dus\n", stopclock_reset_us(&sc));
        fprintf(stderr, "  tr: %d pixels\n", fz_irect_area(tr));
      }

      if (!fz_is_empty_irect(bl))
      {
        render_inc_rect(ctx, self, bounds, pixels, x, y, n, bl, scale);
        fprintf(stderr, "render bl: %dus\n", stopclock_reset_us(&sc));
        upload_texture_rect(self->tex, bl, pixels);
        fprintf(stderr, "upload bl: %dus\n", stopclock_reset_us(&sc));
        fprintf(stderr, "  bl: %d pixels\n", fz_irect_area(bl));
      }

      if (!fz_is_empty_irect(br))
      {
        render_inc_rect(ctx, self, bounds, pixels, x, y, n, br, scale);
        fprintf(stderr, "render br: %dus\n", stopclock_reset_us(&sc));
        upload_texture_rect(self->tex, br, pixels);
        fprintf(stderr, "upload br: %dus\n", stopclock_reset_us(&sc));
        fprintf(stderr, "  br: %d pixels\n", fz_irect_area(br));
      }
      done = 1;
    }
  }
  else
    done = 1;

  #define STRESS 0

  if (done)
    return;

  void *pixels;
  int pitch;

  if (STRESS)
  {
    int new_pixels = w * h;
    if (self->scratch == NULL)
      self->scratch = fz_new_buffer(ctx, new_pixels * 3);
    else if (self->scratch->len < new_pixels * 3)
      fz_resize_buffer(ctx, self->scratch, new_pixels * 3);

    pixels = self->scratch->data;
    pitch = w * 3;
  }
  else
  {
    SDL_Rect lock_rect = (SDL_Rect){.x = 0, .y = 0, .w = w, .h = h};
    SDL_LockTexture(self->tex, &lock_rect, &pixels, &pitch);
  }

  fz_irect full_rect = fz_make_irect(0, 0, w, h);
  render_rect(ctx, self, bounds, pixels, pitch, x, y, full_rect, scale);

  self->st.x = x;
  self->st.y = y;

  int x0, y0;
  if (STRESS)
  {
    x0 = (rand() % (self->st.w * 2)) - self->st.w;
    y0 = (rand() % (self->st.h * 2)) - self->st.h;
  }
  else
  {
    x0 = 0;
    y0 = 0;
    SDL_UnlockTexture(self->tex);
  }

  self->st.rect.x0 = x0;
  self->st.rect.y0 = y0;
  self->st.rect.x1 = x0 + w;
  self->st.rect.y1 = y0 + h;

  if (STRESS)
  {
    upload_texture_rect(self->tex, self->st.rect, pixels);
  }

  // fprintf(stderr, "[txp_renderer] updated texture, new pixels: %d\n", w * h);
}

static void render_caret(txp_renderer *self, int x, int y, int h)
{
  int scale_factor = self->scale_factor.x;
  if (scale_factor == 0)
    scale_factor = 1;

  SDL_Rect r;
  r.x = x - scale_factor / 2;
  r.y = y + scale_factor;
  r.w = scale_factor;
  r.h = h - scale_factor * 2;
  SDL_RenderFillRect(self->sdl, &r);

  r.x = x - scale_factor * 3;
  r.y = y;
  r.w = scale_factor * 6;
  r.h = scale_factor;
  SDL_RenderFillRect(self->sdl, &r);

  r.y = y + h - scale_factor;
  SDL_RenderFillRect(self->sdl, &r);
}


void txp_renderer_render(fz_context *ctx, txp_renderer *self)
{
  SDL_FRect page_rect;
  float scale;

  // fprintf(stderr, "[txp_renderer] txp_renderer_render: compute page pos\n");

  if (!txp_renderer_page_position(ctx, self, &page_rect, NULL, &scale))
    return;

  uint32_t bg, fg;
  txp_get_colors(&self->config, &bg, &fg);
  if (self->cached_bg != bg || self->cached_fg != fg)
  {
    self->cached_bg = bg;
    self->cached_fg = fg;
    clear_texture(self);
  }

  // fprintf(stderr, "[txp_renderer] page rect: {x=%.02f y=%.02f w=%.02f
  // h=%.02f}\n",
  //         page_rect.x, page_rect.y, page_rect.w, page_rect.h);

  const SDL_FRect screen_rect =
      (SDL_FRect){.x = 0, .y = 0, .w = self->output_w, .h = self->output_h};

  // fprintf(stderr, "[txp_renderer] screen rect: {x=%.02f y=%.02f w=%.02f h=%.02f}\n",
  //         screen_rect.x, screen_rect.y, screen_rect.w, screen_rect.h);

  SDL_FRect view_rect;

  // fprintf(stderr, "[txp_renderer] txp_renderer_render: intersect with screen\n");

  if (!SDL_IntersectFRect(&page_rect, &screen_rect, &view_rect))
    return;

  // fprintf(stderr, "[txp_renderer] txp_renderer_render: update texture\n");

  struct timespec update_start, update_end;
  clock_gettime(CLOCK_MONOTONIC, &update_start);
  update_texture(ctx, self, &page_rect, &view_rect);
  clock_gettime(CLOCK_MONOTONIC, &update_end);
  // fprintf(stderr, "[txp_renderer] updated texture in %ldus\n",
  //         (update_end.tv_sec - update_start.tv_sec) * 1000 * 1000 +
  //         (update_end.tv_nsec - update_start.tv_nsec) / 1000);
  // fprintf(stderr, "[txp_renderer] txp_renderer_render: blit texture to screen\n");

  int bx0 = floorf(view_rect.x);
  int by0 = floorf(view_rect.y);
  int pixel_pushed = 0;
  render_texture_rect(self->sdl, bx0, by0, self->tex, self->st.rect);
  if (self->selection_count != 0)
  {
    SDL_SetRenderDrawBlendMode(self->sdl, SDL_BLENDMODE_BLEND);
    fz_rect bounds = get_bounds(ctx, self);

    for (int i = 0; i < self->selection_count; ++i)
    {
      fz_rect fzr = self->selections[i];
      SDL_Rect r;
      r.x = page_rect.x + (fzr.x0 - bounds.x0) * scale;
      r.y = page_rect.y + (fzr.y0 - bounds.y0) * scale;
      r.w = (fzr.x1 - fzr.x0) * scale;
      r.h = (fzr.y1 - fzr.y0) * scale;
      if (r.w == 0)
      {
        SDL_SetRenderDrawColor(self->sdl, 96, 96, 255, 128);
        render_caret(self, r.x, r.y, r.h);
      }
      else
      {
        // fprintf(stderr, "[render] fill rect: %d %d %d %d\n", r.x, r.y, r.w, r.h);
        SDL_SetRenderDrawColor(self->sdl, 96, 96, 255, 64);
        SDL_RenderFillRect(self->sdl, &r);
      }
    }
  }
  // fprintf(stderr, "[render] pixels pushed to screen: %d\n", pixel_pushed);
}

static float point_to_rect_dist(fz_point p, fz_rect r)
{
  float dx = fz_max(0, fz_max(r.x0 - p.x, p.x - r.x1));
  float dy = fz_max(0, fz_max(r.y0 - p.y, p.y - r.y1));
  return sqrtf(dx * dx + dy * dy);
}

// static int is_point_in_rect(fz_point p, fz_rect r)
// {
//   return (p.x >= r.x0 && p.x <= r.x1) && (p.y >= r.y0 && p.y <= r.y1);
// }

bool txp_renderer_start_selection(fz_context *ctx, txp_renderer *self, fz_point pt)
{
  int has_sel = self->selection_count != 0;
  self->selection_count = 0;

  SDL_FRect page_rect;
  float scale;
  if (!txp_renderer_page_position(ctx, self, &page_rect, NULL, &scale))
    return has_sel;

  fz_rect bounds = get_bounds(ctx, self);
  fz_point p = fz_make_point(bounds.x0 + (pt.x - page_rect.x) / scale,
                             bounds.y0 + (pt.y - page_rect.y) / scale);
  self->selection_start = p;

  return has_sel;
}

static int set_quads(fz_context *ctx, txp_renderer *self, fz_quad *quads, int count)
{
  int diff = count != self->selection_count;
  self->selection_count = count;

  for (int i = 0; i < count; ++i)
  {
    fz_rect r = fz_rect_from_quad(quads[i]);
    if (!diff)
      diff =
          (r.x0 != self->selections[i].x0 || r.y0 != self->selections[i].y0 ||
           r.x1 != self->selections[i].x1 || r.y1 != self->selections[i].y1);
    self->selections[i] = r;
  }

  return diff;
}

bool txp_renderer_drag_selection(fz_context *ctx, txp_renderer *self, fz_point pt)
{
  fz_stext_page *page = get_stext(ctx, self);
  if (!page)
    return 0;

  SDL_FRect page_rect;
  fz_point translate, p;
  float scale;

  if (!txp_renderer_page_position(ctx, self, &page_rect, &translate, &scale))
    return 0;

  p = fz_make_point((pt.x - translate.x) / scale, (pt.y - translate.y) / scale);

  fz_quad quads[40];
  int count = fz_highlight_selection(ctx, page, self->selection_start, p, quads, 40);

  return set_quads(ctx, self, quads, count);
}

bool txp_renderer_select_char(fz_context *ctx, txp_renderer *self, fz_point pt)
{
  fz_stext_page *page = get_stext(ctx, self);
  if (!page)
    return 0;

  SDL_FRect page_rect;
  fz_point translate, p, p0, p1;
  float scale;
  fz_quad q;

  if (!txp_renderer_page_position(ctx, self, &page_rect, &translate, &scale))
    return 0;

  p0 = p1 = p = fz_make_point((pt.x - translate.x) / scale, (pt.y - translate.y) / scale);
  q = fz_snap_selection(ctx, page, &p0, &p1, FZ_SELECT_WORDS);

  int count = 0;

  if (point_to_rect_dist(p, fz_rect_from_quad(q)) * scale > 20)
  {
    p0 = p;
    p0.x -= 8;
    p1 = p0;
    q = fz_snap_selection(ctx, page, &p0, &p1, FZ_SELECT_WORDS);

    if (point_to_rect_dist(p, fz_rect_from_quad(q)) * scale < 20)
    {
      q.ul = q.ur;
      q.ll = q.lr;
      count = 1;
    }
  }
  else
  {
    p0 = p1 = p;
    count = 1;
    q = fz_snap_selection(ctx, page, &p0, &p1, FZ_SELECT_CHARS);
  }

  fz_rect r = fz_rect_from_quad(q);
  fprintf(stderr, "sel rect: (%f,%f)-(%f,%f)\n", r.x0, r.y0, r.x1, r.y1);


  return set_quads(ctx, self, &q, count);
}

bool txp_renderer_select_word(fz_context *ctx, txp_renderer *self, fz_point pt)
{
  fz_stext_page *page = get_stext(ctx, self);
  if (!page)
    return 0;

  SDL_FRect page_rect;
  fz_point translate, p, p0, p1;
  float scale;
  fz_quad q;

  if (!txp_renderer_page_position(ctx, self, &page_rect, &translate, &scale))
    return 0;

  p0 = p1 = p = fz_make_point((pt.x - translate.x) / scale, (pt.y - translate.y) / scale);
  q = fz_snap_selection(ctx, page, &p0, &p1, FZ_SELECT_WORDS);

  if (point_to_rect_dist(p, fz_rect_from_quad(q)) * scale > 20)
  {
    p0 = p;
    p0.x -= 16;
    p1 = p0;
    q = fz_snap_selection(ctx, page, &p0, &p1, FZ_SELECT_WORDS);
  }

  return set_quads(ctx, self, &q, 1);
}

void txp_renderer_set_scale_factor(fz_context *ctx, txp_renderer *self, fz_point scale)
{
  self->scale_factor = scale;
}

fz_point txp_renderer_screen_to_document(fz_context *ctx, txp_renderer *self, fz_point pt)
{
  fz_point translate;
  float scale;
  if (!txp_renderer_page_position(ctx, self, NULL, &translate, &scale))
    return fz_make_point(0, 0);
  return fz_make_point((pt.x - translate.x) / scale, (pt.y - translate.y) / scale);
}

fz_point txp_renderer_document_to_screen(fz_context *ctx, txp_renderer *self, fz_point pt)
{
  fz_point translate;
  float scale;
  if (!txp_renderer_page_position(ctx, self, NULL, &translate, &scale))
    return fz_make_point(0, 0);
  return fz_make_point(pt.x * scale + translate.x, pt.y * scale + translate.y);
}

void txp_renderer_screen_size(fz_context *ctx, txp_renderer *self, int *w, int *h)
{
  *w = self->output_w;
  *h = self->output_h;
}
