#ifndef PAGEBUFFER_H_
#define PAGEBUFFER_H_

#include <mupdf/fitz.h>
#include <SDL2/SDL_render.h>
#include <stdbool.h>

// ============================================================================
// Page Buffer API
// ============================================================================

typedef struct pagebuffer_s pagebuffer_t;

// pagebuffer_t caches rasterized pages to blit them efficiently when scrolling.
//
// Design Overview:
// The buffer manages a visible window that scrolls over an infinite canvas,
// rendering only invalidated areas when the window changes. This window is
// stored in a texture at least as large as the visible window (plus padding
// to avoid constant re-rendering during scroll).
//
// Texture Wrapping:
// The texture wraps around to support seamless scrolling. If the texture is
// 1024x1024 and the window covers (200-1200, 500-1300):
// - Horizontal part (200-1024) maps to (200-1024)
// - Horizontal part (1024-1200) wraps to (0-176)
// - Vertical part (500-1024) maps to (500-1024)
// - Vertical part (1024-1300) wraps to (0-276)
//
// This wrapping happens transparently during caching and rendering.
//
// Page Awareness:
// Caching is page-aware. The vertical offset is global (all pages share a
// unique vertical position), but each page can have a different horizontal
// offset. This supports infinite vertical scroll with per-page horizontal
// scrolling.
//
// Coordinate Systems:
// - Absolute: Position relative to the infinite canvas (used for position).
// - Relative: Position relative to the page's top-left corner (used for
//   visible, cached, and render rectangles).

// ============================================================================
// LIFECYCLE
// ============================================================================

// Creates a pagebuffer rendering to the provided SDL_Renderer.
// Returns NULL on allocation failure.
// Note: Renderer must remain valid for the lifetime of the pagebuffer.
pagebuffer_t *pagebuffer_new(SDL_Renderer *renderer);

// Frees the buffer and releases all internal resources.
void pagebuffer_free(pagebuffer_t *buffer);

// ============================================================================
// CONFIGURATION
// ============================================================================

// Adjusts the cache size to match the target window dimensions.
// The cache is automatically flushed when the window is resized.
// Width and height are in pixels.
void pagebuffer_reserve(pagebuffer_t *t, size_t width, size_t height);

// ============================================================================
// DEBUGGING
// ============================================================================

// Returns the internal texture for debugging purposes.
// Do not modify this texture directly; use pagebuffer_update_data() instead.
SDL_Texture *pagebuffer_debug_get_texture(pagebuffer_t *t);

// Returns the first window line actually cached.
// Useful for debugging cache coverage.
int pagebuffer_debug_firstline(pagebuffer_t *t);

// Returns the last window line actually cached.
// Useful for debugging cache coverage.
int pagebuffer_debug_lastline(pagebuffer_t *t);

// ============================================================================
// SCROLLING
// ============================================================================

// Scrolls the vertical window of the page buffer.
// first_line: Top line of the visible window (global vertical coordinate).
// last_line: Bottom line of the visible window (global vertical coordinate).
// first_page: First page that is visible in the window.
// last_page: Last page that is visible in the window.
// Note: Updates cached/render regions accordingly.
void pagebuffer_scroll(pagebuffer_t *t, int first_line, int last_line,
                       int first_page, int last_page);

// ============================================================================
// CACHE MANAGEMENT
// ============================================================================

// Updates a page entry with position and visibility information.
//
// Parameters:
//   page: Page index (0-based).
//   position: Absolute rectangle describing the page's position in the canvas.
//   visible: Relative rectangle describing the visible subset of the page.
//            Must satisfy: 0 <= visible.x0 <= visible.x1 <= position.width
//   cached: Output - Returns the part of the page already in cache (relative).
//   render: Output - Returns the part that should be rendered (relative).
//
// Constraints:
//   0 <= render.x0 <= cached.x0 <= cached.x1 <= render.x1 <= position.width
//   0 <= render.y0 <= cached.y0 <= cached.y1 <= render.y1 <= position.height
//
// Note: After this call, the buffer assumes the render rectangle will be
// populated via pagebuffer_update_data().
void pagebuffer_update_entry(pagebuffer_t *t, int page, fz_irect position,
                             fz_irect visible, fz_irect *cached,
                             fz_irect *render);

// Computes the delta between cached and render regions.
// Returns the number of rectangles needed (0-4) to describe the difference.
// output: Array of up to 4 fz_irect rectangles describing the delta.
// Returns 0 if cached and render are identical (nothing to update).
int pagebuffer_delta_rect(fz_irect cached, fz_irect render, fz_irect output[4]);

// Updates the cached contents with raw pixel data.
// source: Absolute rectangle (usually render rect shifted by position).
// data: Pointer to pixel data (RGB 24).
// pitch: Bytes per row. If 0, defaults to (source.width * 3).
//        Data buffer size must be pitch * source.height.
void pagebuffer_update_data(pagebuffer_t *t, fz_irect source,
                            const uint8_t *data, size_t pitch);

// A filter is a function to post-process pixels in place before caching.
// It operates on an array pixels[width * 3].
typedef void (*pagebuffer_filter)(void *user_data, uint8_t *pixels, size_t width);

// Renders a display list directly to the cache.
// Combines pagebuffer_update_data() with fitz rasterization.
// position: Absolute position of the page.
// subset: Relative subset to rasterize (typically the render rectangle).
// dl: Display list to rasterize.
// filter: An optional filter to apply to colors.
void pagebuffer_update_data_from_display_list(fz_context *ctx, pagebuffer_t *t,
                                              fz_irect position,
                                              fz_irect subset,
                                              fz_display_list *dl,
                                              pagebuffer_filter filter,
                                              void *filter_data);

// Convenience: Updates a page entry and rasterizes where needed.
// Combines pagebuffer_update_entry() and pagebuffer_update_data_from_display_list()
// using pagebuffer_delta_rect() for efficiency.
// page: Page index.
// position: Absolute position of the page.
// visible: Relative visible subset.
// dl: Display list to rasterize.
void pagebuffer_update_entry_from_display_list(fz_context *ctx, pagebuffer_t *t,
                                               int page, fz_irect position,
                                               fz_irect visible,
                                               fz_display_list *dl,
                                               pagebuffer_filter filter,
                                               void *filter_data);

// ============================================================================
// INVALIDATION
// ============================================================================

// Forgets all cached contents.
// Use when document structure changes significantly.
void pagebuffer_flush(pagebuffer_t *t);

// Forgets cached contents of a specific page.
// The texture is retained but marked as stale for next render.
void pagebuffer_invalidate_page(pagebuffer_t *t, int page);

// ============================================================================
// RENDERING
// ============================================================================

// Blits cached contents to the renderer.
// source: Absolute rectangle describing the region to blit.
// x, y: Destination coordinates on the renderer.
void pagebuffer_blit(pagebuffer_t *t, int x, int y, fz_irect source);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Converts an fz_irect to an SDL_Rect.
// Note: fz_irect uses (x0, y0, x1, y1) while SDL_Rect uses (x, y, w, h).
static inline SDL_Rect SDL_rect_from_irect(fz_irect r) {
  return (SDL_Rect){.x = r.x0, .y = r.y0,
                    .w = r.x1 - r.x0, .h = r.y1 - r.y0};
}

// Checks if an fz_irect is empty (invalid or zero-sized).
// Returns true if x1 <= x0 or y1 <= y0.
static inline bool fz_irect_is_empty(fz_irect r) {
  return r.x1 <= r.x0 || r.y1 <= r.y0;
}

// Checks if two fz_irect rectangles are equal.
// Returns true if all coordinates match exactly.
static inline bool fz_irect_equal(fz_irect a, fz_irect b) {
  return a.x0 == b.x0 && a.x1 == b.x1 &&
         a.y0 == b.y0 && a.y1 == b.y1;
}

// Return the relative subset of absolute rectangle that intersects canvas
fz_irect pagebuffer_relative_clipped_area(fz_irect absolute, fz_irect canvas);

#endif // PAGEBUFFER_H_
