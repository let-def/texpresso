#ifndef PAGECOLLECTION_H
#define PAGECOLLECTION_H

#include <mupdf/fitz/structured-text.h>
#include <stdbool.h>
#include <stddef.h>
#include <mupdf/fitz.h>
#include "prefixsum.h"

/**
 * @brief Represents a collection of pages with cached display lists.
 *
 * PageCollection manages a dynamic array of pages, each containing a display list
 * for rendering. It maintains a prefix sum structure (psum) to efficiently compute
 * cumulative heights and perform scroll-based page lookups.
 *
 * It supports *valid* and *invalid* pages: valid pages are loaded and up-to-date;
 * invalid pages (beyond `valid`) are stale or empty but still allocated.
 * The structure allows forward-only population (with lookahead allocation) and
 * backtracking via invalidation.
 *
 * Lifecycle semantics:
 * - Pages are populated sequentially from index `0`.
 * - Pages may be invalidated (marked stale) only by backtracking: `pagecollection_invalidate_after`.
 * - `valid` tracks up-to-date pages; `count` is the total allocated size.
 * - When the final document length is known, call `pagecollection_truncate` to drop invalid pages.
 *
 * Ownership:
 * - PageCollection *owns* its display lists: it holds references and manages `fz_keep/hold`.
 * - `pagecollection_finalize()` drops all references.
 *
 * Vertical layout semantics:
 * - Y = 0 at top, increases downward.
 * - Separator: vertical whitespace between pages.
 * - Invalid pages are visible but *not* considered "up-to-date" (e.g., may show placeholders).
 */
typedef struct {
  struct page_s *pages;         ///< Array of page entries
  PrefixSum psum;               ///< Prefix sum of page heights
  size_t capacity;              ///< Allocated array size (always >= count)
  size_t count;                 ///< Total number of allocated pages (including invalid)
  size_t valid;                 ///< Number of valid (loaded, up-to-date) pages
  size_t extra1, extra2;        ///< Growth factors (Fibonacci-like)
  float ref_width;              ///< Reference width for zoom calculations
} PageCollection;

/**
 * @brief Initializes a PageCollection structure.
 *
 * Sets up the collection with:
 * - Empty pages array (NULL)
 * - Zero `count` and `valid`
 * - Prefix sum initialized
 * - `extra1 = extra2 = 1`
 * - `ref_width = 500.0`
 *
 * @param pcoll Pointer to the PageCollection to initialize (must not be NULL).
 */
void pagecollection_init(PageCollection *pcoll);

/**
 * @brief Cleanup and destroy a PageCollection.
 *
 * Frees all resources:
 * - Drops all display lists (valid and invalid)
 * - Finalizes the prefix sum
 * - Frees the pages array
 *
 * @param ctx MuPDF context for resource cleanup.
 * @param pcoll Pointer to the PageCollection to finalize (may be NULL).
 */
void pagecollection_finalize(fz_context *ctx, PageCollection *pcoll);

// ============================================================================
// State Query
// ============================================================================

/**
 * @brief Returns the total number of allocated pages (including invalid).
 *
 * This is the physical size of the internal array. Pages at indices `>= valid`
 * are invalid (stale or empty).
 *
 * @param pcoll Pointer to the PageCollection (may be NULL).
 * @return Total allocated pages, or 0 if `pcoll` is NULL.
 */
size_t pagecollection_count(const PageCollection *pcoll);

/**
 * @brief Returns the number of valid (loaded, up-to-date) pages.
 *
 * Always satisfies `valid <= count`. Pages beyond `valid - 1` are invalid.
 *
 * @param pcoll Pointer to the PageCollection (may be NULL).
 * @return Number of valid pages.
 */
size_t pagecollection_valid_count(const PageCollection *pcoll);

/**
 * @brief Returns the display list at `index`.
 *
 * Returns NULL if:
 * - `index >= count` (out of bounds)
 * - `index >= valid` (page is invalid/stale)
 * - `pcoll` is NULL
 *
 * @param pcoll Pointer to the PageCollection.
 * @param index Zero-based page index.
 * @return Display list (may be NULL for invalid pages).
 */
fz_display_list *pagecollection_get(const PageCollection *pcoll, size_t index);

fz_stext_page *pagecollection_get_stext(fz_context *ctx, const PageCollection *pcoll, size_t index);

/**
 * @brief Returns the reference width of the collection.
 *
 * The reference width is set from the first page added (`index == 0`). It serves
 * as the baseline for zoom calculations and page scaling.
 *
 * @param pcoll Pointer to the PageCollection (may be NULL).
 * @return Reference width, or `0.0` if `pcoll` is NULL or not yet set.
 */
float pagecollection_reference_width(const PageCollection *pcoll);

// ============================================================================
// State Modification
// ============================================================================

/**
 * @brief Sets or replaces a page's display list and marks all prior pages as valid.
 *
 * - If `index >= count`, the array grows (using Fibonacci-like expansion).
 * - All pages `0 .. index` are marked **valid** (`valid = index + 1`).
 * - If intermediate pages existed, they are preserved; blank display lists are inserted
 *   as needed to maintain continuity.
 * - The display list’s reference count is incremented (`fz_keep_display_list`).
 * - If `index == 0`, `ref_width` is updated from the first page'sbbox.
 *
 * This is the primary way to populate pages *forward*. Backtracking requires
 * `pagecollection_invalidate_after` first.
 *
 * @param ctx MuPDF context for resource management.
 * @param pcoll Pointer to the PageCollection.
 * @param index Zero-based page index to update.
 * @param dl Display list to store (must not be NULL).
 */
void pagecollection_set(fz_context *ctx, PageCollection *pcoll,
                        size_t index, fz_display_list *dl);

/**
 * @brief Marks all pages at indices >= `count` as invalid (stale).
 *
 * Pages remain allocated and retain their display lists until truncated or replaced.
 * `valid` is set to `count`; `count` itself is unchanged.
 *
 * This supports **backtracking** during incremental document loading: e.g., if
 * re-parsing reveals fewer pages than initially anticipated.
 *
 * @param pcoll Pointer to the PageCollection.
 * @param count New effective `valid`. Pages `>= count` become invalid.
 */
void pagecollection_invalidate_after(PageCollection *pcoll, size_t count);

/**
 * @brief Truncates the collection to its current `valid`.
 *
 * Drops all pages beyond `valid - 1`, freeing their display lists and
 * shrinking the logical size:
 *   - `count = valid`
 *   - Memory is *not* reclaimed (capacity remains), but `pages[]` beyond new count
 *     are cleared (display lists dropped, set to NULL).
 *   - `extra1` and `extra2` are reset to `1` (growth factors reset for new forward loads).
 *
 * This is called once the final document length is known (e.g., after full parse).
 *
 * @param ctx MuPDF context for display list cleanup.
 * @param pcoll Pointer to the PageCollection.
 */
void pagecollection_truncate(fz_context *ctx, PageCollection *pcoll);

// ============================================================================
// Vertical Layout / Scrolling Support
// ============================================================================

/**
 * @brief Finds the page whose content *starts at or below* `offset`.
 *
 * Returns the index of the page that is *visually below* the given offset
 * (rounds up). Handles boundaries and empty documents robustly.
 *
 * - Empty document (`count == 0`): returns `-1`.
 * - `offset <= 0`: returns `0`.
 * - Beyond total height: returns `count - 1`.
 * - Invalid page: still returns its index (layout considers physical size).
 *
 * @param pcoll Pointer to the PageCollection.
 * @param offset Vertical scroll position (in world coordinates; Y = 0 at top).
 * @param separator Vertical spacing between pages.
 * @return Page index (>= 0) or `-1` if document is empty.
 */
ssize_t pagecollection_page_below(PageCollection *pcoll, float offset, float separator);

/**
 * @brief Finds the page *above* a given scroll offset (rounds down).
 *
 * Equivalent to `pagecollection_page_below(offset - separator, separator)`.
 * Useful for determining visible page ranges.
 *
 * @param pcoll Pointer to the PageCollection.
 * @param offset Vertical scroll position.
 * @param separator Vertical spacing between pages.
 * @return Page index (>= 0) or `-1` if document is empty.
 */
ssize_t pagecollection_page_above(PageCollection *pcoll, float offset, float separator);

/**
 * @brief Computes the vertical position (Y-coordinate) where `page` starts.
 *
 * Formula:
 * `sum(page_heights[0 .. page-1]) + page * separator`
 *
 * - If `page < 0` → `0.0`
 * - If `page >= count` → clamped to last page’s offset.
 * - Invalid pages still have a valid geometric offset.
 *
 * @param pcoll Pointer to the PageCollection.
 * @param page Zero-based page index.
 * @param separator Vertical spacing between pages.
 * @return Y-coordinate of the page’s top edge.
 */
float pagecollection_page_offset(PageCollection *pcoll, ssize_t page, float separator);

/**
 * @brief Computes the total height of all pages including separators.
 *
 * Formula:
 * `sum(all_page_heights) + (count - 1) * separator`
 *
 * Returns `0.0` if `count == 0`.
 *
 * @param pcoll Pointer to the PageCollection.
 * @param separator Vertical spacing between pages.
 * @return Total document height (in world coordinates).
 */
float pagecollection_total_height(PageCollection *pcoll, float separator);

#endif // PAGECOLLECTION_H
