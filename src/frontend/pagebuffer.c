#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "pagebuffer.h"

#define MARGIN 128

/**
 * @brief Per-page cache metadata.
 *
 * Tracks the cached region for a single page within the texture.
 */
struct pageentry_s {
  fz_irect buffer_space;  ///< Absolute position of this page on the canvas
  fz_irect cached;        ///< Relative rectangle of currently cached region
};

/**
 * @brief Initialize a PageBuffer with an SDL renderer.
 *
 * Sets up the page buffer structure but does not allocate a texture yet.
 * The texture will be created on first use or when reserve() is called.
 *
 * @param pbuff Pointer to the PageBuffer to initialize.
 * @param renderer SDL renderer for texture operations.
 */
void pagebuffer_init(PageBuffer *pbuff, SDL_Renderer *renderer)
{
  *pbuff = (PageBuffer){0,};
  pbuff->renderer = renderer;
}

/**
 * @brief Cleanup and free all PageBuffer resources.
 *
 * Destroys the texture, frees page entries, and frees the scratch buffer.
 *
 * @param pbuff Pointer to the PageBuffer to finalize.
 */
void pagebuffer_finalize(PageBuffer *pbuff)
{
  if (pbuff->texture)
    SDL_DestroyTexture(pbuff->texture);
  if (pbuff->entries)
    free(pbuff->entries);
  if (pbuff->scratch)
    free(pbuff->scratch);
}

/**
 * @brief Find the smallest power of 2 >= x.
 *
 * Used to determine texture dimensions that are powers of 2 (required by SDL).
 */
static int pow2(size_t x)
{
  int result = 0;
  while ((1 << result) < x)
    result += 1;
  return result;
}

/**
 * @brief Clear all cached page entries.
 *
 * Resets the first/last page range and clears all cached regions.
 * The texture memory is retained but marked as needing re-population.
 */
void pagebuffer_flush(PageBuffer *pbuff)
{
  pbuff->first = 0;
  pbuff->last = -1;
}

/**
 * @brief Invalidate cache for a specific page.
 *
 * Marks the page's cached region as stale by clearing the cached rectangle.
 * The texture memory is retained but will be re-rendered on next update.
 *
 * @param pbuff Pointer to the PageBuffer.
 * @param page Page index to invalidate.
 */
void pagebuffer_invalidate_page(PageBuffer *pbuff, int page)
{
  if (0 <= page && page < pbuff->cap)
  {
    pbuff->entries[page].cached.x0 = 0.0;
    pbuff->entries[page].cached.x1 = 0.0;
  }
}

/**
 * @brief Adjust cache size to match window dimensions.
 *
 * Resizes the internal texture to accommodate the target window plus padding.
 * The cache is automatically flushed when dimensions change.
 *
 * @param pbuff Pointer to the PageBuffer.
 * @param width Target window width in pixels.
 * @param height Target window height in pixels.
 */
void pagebuffer_reserve(PageBuffer *pbuff, size_t width, size_t height)
{
  int pow_w = pow2(width + 2 * MARGIN);
  int pow_h = pow2(height + 2 * MARGIN);

  if (pbuff->texture != NULL)
  {
    if (pow_w == pbuff->pow_w && pow_h == pbuff->pow_h)
      return;
    SDL_DestroyTexture(pbuff->texture);
  }

  pbuff->texture = SDL_CreateTexture(pbuff->renderer, SDL_PIXELFORMAT_RGB24,
                                     SDL_TEXTUREACCESS_STREAMING, (1 << pow_w),
                                     (1 << pow_h));
  pbuff->pow_w = pow_w;
  pbuff->pow_h = pow_h;
  pagebuffer_flush(pbuff);
}

/**
 * @brief Update page entry with visibility and positioning info.
 *
 * This is the core caching decision function. It determines:
 * - What part of the page is already in cache (cached)
 * - What part needs to be rendered (render)
 * - Expands the cached page range if needed (first, last)
 *
 * The logic uses MARGIN (128px) around visible area to minimize re-rendering
 * during scrolling. The cached region is always larger than visible, and
 * render equals cached for the region that needs updating.
 *
 * After determining cached region, it adjusts first/last to include only
 * pages that overlap with the vertical range of the current pages.
 *
 * @param pbuff Pointer to the PageBuffer.
 * @param page Page index (0-based).
 * @param buffer_space Absolute rectangle describing page's position on canvas.
 * @param visible Relative rectangle describing visible subset of page.
 * @param cached Output: Region already in cache (relative to page).
 * @param render Output: Region that needs rendering (relative to page).
 */
void pagebuffer_update_entry(PageBuffer *pbuff, int page, fz_irect buffer_space,
                             fz_irect visible, fz_irect *cached, fz_irect *render)
{
  assert(page >= 0);

  // Extend entries array if needed
  if (pbuff->cap <= page)
  {
    int cap = pbuff->cap ? pbuff->cap : 4;
    while (cap <= page)
      cap *= 2;
    pbuff->entries = realloc(pbuff->entries, sizeof(struct pageentry_s) * cap);
    if (pbuff->entries == NULL)
      abort();
    memset(pbuff->entries + pbuff->cap, 0, sizeof(struct pageentry_s) * (cap - pbuff->cap));
    pbuff->cap = cap;
  }
  struct pageentry_s *entry = &pbuff->entries[page];

  // Add page to cached range if not already there
  if (!(pbuff->first <= page && page <= pbuff->last))
  {
    if (pbuff->last < pbuff->first)
      pbuff->last = pbuff->first = page;
    while (pbuff->first > page) {
      pbuff->first--;
      pbuff->entries[pbuff->first] = (struct pageentry_s){0,};
    }
    while (pbuff->last < page) {
      pbuff->last++;
      pbuff->entries[pbuff->last] = (struct pageentry_s){0,};
    }
    *entry = (struct pageentry_s){0,};
  }

  // Determine if we need to recompute cached region
  bool diff = !fz_irect_equal(buffer_space, entry->buffer_space) ||
    fz_is_empty_irect(fz_intersect_irect(entry->cached, visible));

  if (diff)
  {
    entry->buffer_space = buffer_space;
    entry->cached = fz_make_irect(visible.x0, visible.y0,
                                  visible.x0, visible.y0);
  }
  *cached = entry->cached;

  // Expand cached.x0 if needed (with MARGIN)
  if (diff || entry->cached.x0 > visible.x0)
  {
    entry->cached.x0 = fz_maxi(visible.x0 - MARGIN, 0);
    if (entry->cached.x1 > entry->cached.x0 + (1 << pbuff->pow_w))
      entry->cached.x1 = entry->cached.x0 + (1 << pbuff->pow_w);
  }

  // Expand cached.x1 if needed (with MARGIN)
  if (diff || entry->cached.x1 < visible.x1) {
    entry->cached.x1 = fz_mini(visible.x1 + MARGIN, buffer_space.x1 - buffer_space.x0);
    if (entry->cached.x0 < entry->cached.x1 - (1 << pbuff->pow_w))
      entry->cached.x0 = entry->cached.x1 - (1 << pbuff->pow_w);
  }

  // Expand cached.y0 if needed (with MARGIN)
  if (diff || entry->cached.y0 > visible.y0)
  {
    entry->cached.y0 = fz_maxi(visible.y0 - MARGIN, 0);
    if (entry->cached.y1 > entry->cached.y0 + (1 << pbuff->pow_h))
      entry->cached.y1 = entry->cached.y0 + (1 << pbuff->pow_h);
  }

  // Expand cached.y1 if needed (with MARGIN)
  if (diff || entry->cached.y1 < visible.y1)
  {
    entry->cached.y1 = fz_mini(visible.y1 + MARGIN, buffer_space.y1 - buffer_space.y0);
    if (entry->cached.y0 < entry->cached.y1 - (1 << pbuff->pow_h))
      entry->cached.y0 = entry->cached.y1 - (1 << pbuff->pow_h);
  }

  *render = entry->cached;

  // Compute vertical range this page's cache covers
  int first_line = buffer_space.y0 + entry->cached.y1 - (1 << pbuff->pow_h);
  int last_line = buffer_space.y0 + entry->cached.y0 + (1 << pbuff->pow_h);

  // Trim first pages that are entirely above our range
  while (pbuff->first < page)
  {
    if (!(fz_is_empty_irect(pbuff->entries[pbuff->first].buffer_space) ||
          (pbuff->entries[pbuff->first].buffer_space.y0 +
               pbuff->entries[pbuff->first].cached.y1 < first_line)))
      break;
    pbuff->first++;
  }
  // Clip first page's cached region to our vertical range
  if (pbuff->entries[pbuff->first].buffer_space.y0 + pbuff->entries[pbuff->first].cached.y0 < first_line)
    pbuff->entries[pbuff->first].cached.y0 = first_line - pbuff->entries[pbuff->first].buffer_space.y0;

  // Trim last pages that are entirely below our range
  while (pbuff->last > page)
  {
    if (!(fz_is_empty_irect(pbuff->entries[pbuff->last].buffer_space) ||
          (pbuff->entries[pbuff->last].buffer_space.y0 + pbuff->entries[pbuff->last].cached.y0 >
           last_line)))
        break;
    pbuff->last--;
  }
  // Clip last page's cached region to our vertical range
  if (pbuff->entries[pbuff->last].buffer_space.y0 + pbuff->entries[pbuff->last].cached.y1 > last_line)
    pbuff->entries[pbuff->last].cached.y1 = last_line - pbuff->entries[pbuff->last].buffer_space.y0;
}

/**
 * @brief Compute delta between cached and render regions.
 *
 * Calculates which parts of the render region are not in cache.
 * This produces up to 4 rectangles representing the "damage" regions:
 * 1. Top band (above cached)
 * 2. Left band (between cached.x0 and cached.x1, below top band)
 * 3. Right band (between cached.x0 and cached.x1, above cached.y1)
 * 4. Bottom band (below cached)
 *
 * These can be rendered in any order.
 *
 * @param cached Region already cached (relative).
 * @param render Region that needs to be rendered (relative).
 * @param output Array of rectangles describing the delta (must have 4 elements).
 * @return Number of rectangles in output (0 if no update needed).
 */
int pagebuffer_delta_rect(fz_irect cached, fz_irect render, fz_irect output[4])
{
  fz_irect *o = output;

  // Top band (above cached)
  *o = fz_make_irect(render.x0, render.y0, render.x1, cached.y0);
  if (!fz_is_empty_irect(*o))
    o++;

  // Left band (between cached.x0 and cached.x1, below top band)
  *o = fz_make_irect(render.x0, cached.y0, cached.x0, cached.y1);
  if (!fz_is_empty_irect(*o))
    o++;

  // Right band (between cached.x0 and cached.x1, above cached.y1)
  *o = fz_make_irect(cached.x1, cached.y0, render.x1, cached.y1);
  if (!fz_is_empty_irect(*o))
    o++;

  // Bottom band (below cached)
  *o = fz_make_irect(render.x0, cached.y1, render.x1, render.y1);
  if (!fz_is_empty_irect(*o))
    o++;

  return o - output;
}

/**
 * @brief Upload a sub-region of pixel data to the texture.
 *
 * This handles texture wrapping when the source rectangle crosses
 * texture boundaries.
 *
 * @param tex Texture to update.
 * @param source Source region in texture coordinates.
 * @param data Pointer to pixel data.
 * @param dpitch Bytes per row of pixel data.
 */
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

/**
 * @brief Get the internal texture for debugging.
 *
 * @param pbuff Pointer to the PageBuffer.
 * @return The internal SDL texture.
 */
SDL_Texture *pagebuffer_debug_get_texture(PageBuffer *pbuff)
{
  return pbuff->texture;
}

/**
 * @brief Update cached texture with raw pixel data.
 *
 * Handles texture wrapping automatically by splitting the upload into
 * up to 4 operations if the source crosses texture boundaries.
 *
 * @param pbuff Pointer to the PageBuffer.
 * @param source Absolute rectangle specifying which region to update.
 * @param data Pointer to pixel data.
 * @param pitch Bytes per row of pixel data (0 defaults to source.width * 3).
 */
void pagebuffer_update_data(PageBuffer *pbuff, fz_irect source,
                            const uint8_t *data, size_t pitch)
{
  if (fz_is_empty_irect(source))
    return;

  SDL_Texture *tex = pbuff->texture;
  if (!tex)
    return;

  int w, h;
  SDL_QueryTexture(tex, NULL, NULL, &w, &h);

  if (pitch == 0)
    pitch = (source.x1 - source.x0) * 3;

  // Apply texture wrapping (modulo texture dimensions)
  source.x0 %= w;
  source.x1 %= w;
  source.y0 %= h;
  source.y1 %= h;

  int dx = (w - source.x0) * 3;
  int dy = (h - source.y0) * pitch;

  // Handle wrapping in X and Y axes
  if (source.x1 > source.x0)
  {
    if (source.y1 > source.y0)
    {
      // No wrapping: single upload
      update_texture_sub(tex, source, data, pitch);
    }
    else
    {
      // Y-wrapping: two uploads (top and bottom)
      update_texture_sub(tex, fz_make_irect(source.x0, source.y0, source.x1, h), data, pitch);
      update_texture_sub(tex, fz_make_irect(source.x0, 0, source.x1, source.y1), data + dy, pitch);
    }
  }
  else
  {
    if (source.y1 > source.y0)
    {
      // X-wrapping: two uploads (left and right)
      update_texture_sub(tex, fz_make_irect(source.x0, source.y0, w, source.y1), data, pitch);
      update_texture_sub(tex, fz_make_irect(0, source.y0, source.x1, source.y1), data + dx, pitch);
    }
    else
    {
      // Both X and Y wrapping: four uploads
      update_texture_sub(tex, fz_make_irect(source.x0, source.y0, w, h), data, pitch);
      update_texture_sub(tex, fz_make_irect(0, source.y0, source.x1, h), data + dx, pitch);
      update_texture_sub(tex, fz_make_irect(source.x0, 0, w, source.y1), data + dy, pitch);
      update_texture_sub(tex, fz_make_irect(0, 0, source.x1, source.y1), data + dy + dx, pitch);
    }
  }
}

/**
 * @brief Copy a region from texture to renderer.
 *
 * @param ren Renderer to copy to.
 * @param tex Source texture.
 * @param x, y Destination position.
 * @param src Source region in texture.
 */
static void render_copy_fz(SDL_Renderer *ren, SDL_Texture *tex, int x, int y, fz_irect src)
{
  SDL_Rect ssrc = SDL_rect_from_irect(src);
  SDL_Rect sdst = {.x = x, .y = y, .w = ssrc.w, .h = ssrc.h};
  SDL_RenderCopy(ren, tex, &ssrc, &sdst);
}

/**
 * @brief Blit cached contents to the renderer.
 *
 * Copies a region from the texture to the renderer, handling texture wrapping
 * automatically by splitting into up to 4 copy operations if needed.
 *
 * @param pbuff Pointer to the PageBuffer.
 * @param x, y Destination coordinates on the renderer.
 * @param source Source region in texture (absolute coordinates).
 */
void pagebuffer_blit(PageBuffer *pbuff, int x, int y, fz_irect source)
{
  if (source.x0 == source.x1 || source.y0 == source.y1)
    return;

  SDL_Texture *tex = pbuff->texture;
  if (!tex)
    return;

  int w, h;
  SDL_QueryTexture(tex, NULL, NULL, &w, &h);

  // Apply texture wrapping
  source.x0 %= w;
  source.x1 %= w;
  source.y0 %= h;
  source.y1 %= h;

  int dx = w - source.x0;
  int dy = h - source.y0;

  if (source.x1 > source.x0)
  {
    if (source.y1 > source.y0)
    {
      render_copy_fz(pbuff->renderer, tex, x, y, source);
    }
    else
    {
      render_copy_fz(pbuff->renderer, tex, x, y,
                     fz_make_irect(source.x0, source.y0, source.x1, h));
      render_copy_fz(pbuff->renderer, tex, x, y + dy,
                     fz_make_irect(source.x0, 0, source.x1, source.y1));
    }
  }
  else
  {
    if (source.y1 > source.y0)
    {
      render_copy_fz(pbuff->renderer, tex, x, y,
                     fz_make_irect(source.x0, source.y0, w, source.y1));
      render_copy_fz(pbuff->renderer, tex, x + dx, y,
                     fz_make_irect(0, source.y0, source.x1, source.y1));
    }
    else
    {
      render_copy_fz(pbuff->renderer, tex, x, y,
                     fz_make_irect(source.x0, source.y0, w, h));
      render_copy_fz(pbuff->renderer, tex, x, y + dy,
                     fz_make_irect(source.x0, 0, w, source.y1));
      render_copy_fz(pbuff->renderer, tex, x + dx, y,
                     fz_make_irect(0, source.y0, source.x1, h));
      render_copy_fz(pbuff->renderer, tex, x + dx, y + dy,
                     fz_make_irect(0, 0, source.x1, source.y1));
    }
  }
}

/**
 * @brief Ensure scratch buffer has at least the specified size.
 *
 * @param pbuff Pointer to the PageBuffer.
 * @param width Width of pixel buffer needed.
 * @param height Height of pixel buffer needed.
 * @return Pointer to scratch buffer (at least width * height * 3 bytes).
 */
static void *reserve_scratch(PageBuffer *pbuff, int width, int height)
{
  size_t size = width * height * 3;
  if (pbuff->scratch_size < size)
  {
    pbuff->scratch = realloc(pbuff->scratch, size);
    pbuff->scratch_size = size;
    if (!pbuff->scratch)
      abort();
  }
  return pbuff->scratch;
}

/**
 * @brief Rasterize a display list directly to the cache.
 *
 * Combines MuPDF rasterization with texture upload. This is the high-level
 * function for populating the cache with rendered content.
 *
 * Algorithm:
 * 1. Calculate scaling to map display list bounds to position rect
 * 2. Create transformation matrix: translate to origin, then scale
 * 3. Create pixmap for the subset being rendered (in local page coords)
 * 4. Draw display list to pixmap with transformation
 * 5. Optionally apply color filter
 * 6. Convert subset to absolute coordinates and upload to texture
 *
 * @param ctx MuPDF context for rasterization.
 * @param pbuff Pointer to the PageBuffer.
 * @param position Absolute position of the page on canvas.
 * @param subset Subset of page to rasterize (relative to page).
 * @param dl Display list to render.
 * @param filter Optional pixel filter, or NULL.
 * @param filter_data User data passed to filter callback.
 */
void pagebuffer_update_data_from_display_list(fz_context *ctx, PageBuffer *pbuff,
                                              fz_irect position,
                                              fz_irect subset,
                                              fz_display_list *dl,
                                              pagebuffer_filter filter,
                                              void *filter_data)
{
  fz_matrix ctm;

  // Calculate scaling to fit display list bounds into position rectangle
  fz_rect bounds = fz_bound_display_list(ctx, dl);

  float sw = (float)(position.x1 - position.x0) / (bounds.x1 - bounds.x0);
  float sh = (float)(position.y1 - position.y0) / (bounds.y1 - bounds.y0);

  // Build transformation: translate to origin -> scale
  ctm = fz_translate(-bounds.x0, -bounds.y0);
  ctm = fz_concat(ctm, fz_scale(sw, sh));

  // Create pixmap for subset (local page coordinates)
  uint8_t *data = reserve_scratch(pbuff, subset.x1 - subset.x0, subset.y1 - subset.y0);
  fz_pixmap *pix = fz_new_pixmap_with_bbox_and_data(ctx, fz_device_rgb(ctx), subset, NULL, 0, data);
  fz_clear_pixmap_with_value(ctx, pix, 255);

  // Draw display list to pixmap
  fz_device *dev = fz_new_draw_device(ctx, fz_identity, pix);
  fz_run_display_list(ctx, dl, dev, ctm, fz_rect_from_irect(subset), NULL);

  fz_close_device(ctx, dev);
  fz_drop_device(ctx, dev);

  fz_drop_pixmap(ctx, pix);

  // Convert subset to absolute coordinates for texture upload
  subset.x0 += position.x0;
  subset.y0 += position.y0;
  subset.x1 += position.x0;
  subset.y1 += position.y0;

  int w = subset.x1 - subset.x0;
  int h = subset.y1 - subset.y0;

  // Apply filter if provided
  if (filter)
    for (int y = 0; y < h; y++)
      filter(filter_data, data + y * w * 3, w);

  pagebuffer_update_data(pbuff, subset, data, 0);
}

/**
 * @brief Convenience function: update entry and rasterize as needed.
 *
 * Combines update_entry(), delta_rect(), and update_data_from_display_list()
 * into a single atomic operation. This is the primary function for populating
 * the cache before rendering.
 *
 * Algorithm:
 * 1. Determine cached and render regions for this page
 * 2. Compute the delta (regions that need updating)
 * 3. For each delta region, rasterize and upload
 * 4. Return whether any rasterization occurred
 *
 * @param ctx MuPDF context for rasterization.
 * @param pbuff Pointer to the PageBuffer.
 * @param page Page index to update.
 * @param position Absolute position of the page on canvas.
 * @param visible Relative visible subset of the page.
 * @param dl Display list to render.
 * @param filter Optional pixel filter, or NULL.
 * @param filter_data User data passed to filter callback.
 * @return true if any rasterization occurred.
 */
bool pagebuffer_update_entry_from_display_list(fz_context *ctx, PageBuffer *pbuff,
                                               int page, fz_irect position,
                                               fz_irect visible,
                                               fz_display_list *dl,
                                               pagebuffer_filter filter,
                                               void *filter_data)
{
  fz_irect cached_rect, render_rect;
  pagebuffer_update_entry(pbuff, page, position, visible, &cached_rect, &render_rect);

  fz_irect delta[4];
  int count = pagebuffer_delta_rect(cached_rect, render_rect, delta);

  // Rasterize each delta region
  for (int i = 0; i < count; i++)
    pagebuffer_update_data_from_display_list(ctx, pbuff, position, delta[i], dl, filter, filter_data);

  return (count > 0);
}
