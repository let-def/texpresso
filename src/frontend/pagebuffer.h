#ifndef PAGEBUFFER_H_
#define PAGEBUFFER_H_

#include <mupdf/fitz.h>
#include <SDL2/SDL_render.h>
#include <stdbool.h>

/**
 * @brief Caches rasterized pages for efficient scrolling rendering.
 *
 * PageBuffer implements a texture-based caching strategy for PDF pages.
 * It stores a "window" of the infinite canvas in an SDL texture, which wraps
 * around to support seamless scrolling without constant re-rendering.
 *
 * Texture Wrapping:
 * The texture wraps around both axes to support scrolling beyond texture bounds.
 * If texture is 1024x1024 and window is at (200-1200, 500-1300):
 * - X: 200-1024 stays at 200-1024, 1024-1200 wraps to 0-176
 * - Y: 500-1024 stays at 500-1024, 1024-1300 wraps to 0-276
 *
 * Coordinate Systems:
 * - Absolute: Positions relative to infinite document canvas (for position rects)
 * - Relative: Positions relative to page top-left (for visible/cached/render rects)
 *
 * Page Awareness:
 * Vertical offset is global (pages share vertical position), but each page can
 * have different horizontal offset. This supports infinite vertical scroll with
 * per-page horizontal scrolling.
 *
 * Caching Strategy:
 * - Maintains a "window" of pages currently in view (first, last indices)
 * - For each page, tracks buffer_space (absolute position) and cached (relative)
 * - Expands window only when necessary to minimize cache invalidation
 */
typedef struct {
  int pow_w, pow_h;                   ///< Texture dimensions as powers of 2
  SDL_Renderer *renderer;             ///< SDL renderer for texture operations
  SDL_Texture *texture;               ///< Cached page texture
  struct pageentry_s *entries;        ///< Per-page cache metadata
  int first, last;                    ///< Range of pages currently cached
  int cap;                            ///< Allocated entries capacity
  void *scratch;                      ///< Temporary buffer for pixel data
  size_t scratch_size;                ///< Size of scratch buffer
} PageBuffer;

/**
 * @brief Initialize a PageBuffer with an SDL renderer.
 *
 * Sets up the page buffer structure but does not allocate a texture yet.
 * The texture will be created on first use or when reserve() is called.
 *
 * @param pbuff Pointer to the PageBuffer to initialize.
 * @param renderer SDL renderer for texture operations (must remain valid).
 */
void pagebuffer_init(PageBuffer *pbuff, SDL_Renderer *renderer);

/**
 * @brief Cleanup and free all PageBuffer resources.
 *
 * Destroys the texture, frees page entries, and frees the scratch buffer.
 * The PageBuffer can be re-initialized after finalization.
 *
 * @param pbuff Pointer to the PageBuffer to finalize.
 */
void pagebuffer_finalize(PageBuffer *pbuff);

/**
 * @brief Adjust cache size to match window dimensions.
 *
 * Resizes the internal texture to accommodate the target window plus padding.
 * The cache is automatically flushed when dimensions change to invalidate
 * stale cached data.
 *
 * @param pbuff Pointer to the PageBuffer.
 * @param width Target window width in pixels.
 * @param height Target window height in pixels.
 */
void pagebuffer_reserve(PageBuffer *pbuff, size_t width, size_t height);

/**
 * @brief Get the internal texture for debugging.
 *
 * Returns the cached texture for inspection (e.g., rendering a minimap).
 * Do not modify the texture directly; use pagebuffer_update_data() instead.
 *
 * @param pbuff Pointer to the PageBuffer.
 * @return The internal SDL texture.
 */
SDL_Texture *pagebuffer_debug_get_texture(PageBuffer *pbuff);

/**
 * @brief Update page entry with visibility and positioning info.
 *
 * This is the core caching decision function. It determines:
 * - What part of the page is already in cache (cached)
 * - What part needs to be rendered (render)
 * - Expands the cached page range if needed (first, last)
 *
 * Coordinate relationships:
 *   0 <= render.x0 <= cached.x0 <= cached.x1 <= render.x1 <= position.width
 *   0 <= render.y0 <= cached.y0 <= cached.y1 <= render.y1 <= position.height
 *
 * The render rectangle is always larger than or equal to cached.
 * The difference (render - cached) is the region that needs rasterization.
 *
 * @param pbuff Pointer to the PageBuffer.
 * @param page Page index (0-based).
 * @param position Absolute rectangle describing page's position on canvas.
 * @param visible Relative rectangle describing visible subset of page.
 * @param cached Output: Region already in cache (relative to page).
 * @param render Output: Region that needs rendering (relative to page).
 */
void pagebuffer_update_entry(PageBuffer *pbuff, int page, fz_irect position,
                             fz_irect visible, fz_irect *cached,
                             fz_irect *render);

/**
 * @brief Compute delta between cached and render regions.
 *
 * Calculates which parts of the render region are not in cache.
 * This is the "damage" region that needs to be updated.
 *
 * Output format: Array of up to 4 rectangles (top, left, right, bottom bands).
 * This handles the L-shaped difference when render extends beyond cached.
 *
 * @param cached Region already cached (relative).
 * @param render Region that needs to be rendered (relative).
 * @param output Array of rectangles describing the delta (must have 4 elements).
 * @return Number of rectangles in output (0 if no update needed).
 */
int pagebuffer_delta_rect(fz_irect cached, fz_irect render, fz_irect output[4]);

/**
 * @brief Update cached texture with raw pixel data.
 *
 * Uploads pixel data to the texture, handling texture wrapping automatically.
 * The source rectangle is in absolute coordinates (includes page position).
 *
 * Pixel format: RGB 24-bit (3 bytes per pixel).
 *
 * @param pbuff Pointer to the PageBuffer.
 * @param source Absolute rectangle specifying which region to update.
 * @param data Pointer to pixel data.
 * @param pitch Bytes per row of pixel data (0 defaults to source.width * 3).
 */
void pagebuffer_update_data(PageBuffer *pbuff, fz_irect source,
                            const uint8_t *data, size_t pitch);

/**
 * @brief Callback type for pixel post-processing filters.
 *
 * Filters operate in-place on RGB pixel data.
 *
 * @param user_data Arbitrary user data passed through from filter call.
 * @param pixels Pointer to first pixel (RGB RGB RGB...).
 * @param width Number of pixels in a row (not bytes).
 */
typedef void (*pagebuffer_filter)(void *user_data, uint8_t *pixels, size_t width);

/**
 * @brief Rasterize a display list directly to the cache.
 *
 * Combines MuPDF rasterization with texture upload. This is the high-level
 * function for populating the cache with rendered content.
 *
 * Steps:
 * 1. Calculate scaling to fit display list bounds into position rect
 * 2. Create pixmap for the subset being rendered
 * 3. Draw display list to pixmap with scaling transform
 * 4. Apply optional filter to pixel data
 * 5. Upload to texture with pagebuffer_update_data()
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
                                              void *filter_data);

/**
 * @brief Convenience function: update entry and rasterize as needed.
 *
 * Combines update_entry(), delta_rect(), and update_data_from_display_list()
 * into a single atomic operation. This is the primary function for populating
 * the cache before rendering.
 *
 * @param ctx MuPDF context for rasterization.
 * @param pbuff Pointer to the PageBuffer.
 * @param page Page index to update.
 * @param position Absolute position of the page on canvas.
 * @param visible Relative visible subset of the page.
 * @param dl Display list to render.
 * @param filter Optional pixel filter, or NULL.
 * @param filter_data User data passed to filter callback.
 * @return true if any rasterization occurred (cache was updated).
 */
bool pagebuffer_update_entry_from_display_list(fz_context *ctx, PageBuffer *pbuff,
                                               int page, fz_irect position,
                                               fz_irect visible,
                                               fz_display_list *dl,
                                               pagebuffer_filter filter,
                                               void *filter_data);

/**
 * @brief Clear all cached page entries.
 *
 * Resets the first/last page range and clears all cached regions.
 * The texture memory is retained but marked as needing re-population.
 *
 * Use this when the document structure changes significantly.
 *
 * @param pbuff Pointer to the PageBuffer.
 */
void pagebuffer_flush(PageBuffer *pbuff);

/**
 * @brief Invalidate cache for a specific page.
 *
 * Marks the page's cached region as stale by clearing the cached rectangle.
 * The texture memory is retained but will be re-rendered on next update.
 *
 * @param pbuff Pointer to the PageBuffer.
 * @param page Page index to invalidate.
 */
void pagebuffer_invalidate_page(PageBuffer *pbuff, int page);

/**
 * @brief Blit cached contents to the renderer.
 *
 * Copies a region from the texture to the renderer. Handles texture wrapping
 * automatically by splitting into up to 4 copy operations if needed.
 *
 * @param pbuff Pointer to the PageBuffer.
 * @param x, y Destination coordinates on the renderer.
 * @param source Source region in texture (absolute coordinates).
 */
void pagebuffer_blit(PageBuffer *pbuff, int x, int y, fz_irect source);

/**
 * @brief Convert fz_irect to SDL_Rect.
 *
 * Adapts coordinate format from (x0,y0,x1,y1) to (x,y,w,h).
 *
 * @param r Input rectangle in fz_irect format.
 * @return Rectangle in SDL_Rect format.
 */
static inline SDL_Rect SDL_rect_from_irect(fz_irect r) {
  return (SDL_Rect){.x = r.x0, .y = r.y0,
                    .w = r.x1 - r.x0, .h = r.y1 - r.y0};
}

/**
 * @brief Check if two fz_irect rectangles are equal.
 *
 * @param a First rectangle.
 * @param b Second rectangle.
 * @return true if all coordinates match exactly.
 */
static inline bool fz_irect_equal(fz_irect a, fz_irect b) {
  return a.x0 == b.x0 && a.x1 == b.x1 &&
         a.y0 == b.y0 && a.y1 == b.y1;
}

#endif
