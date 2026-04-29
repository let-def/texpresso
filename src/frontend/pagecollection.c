#include "pagecollection.h"
#include "prefixsum.h"
#include <mupdf/fitz.h>
#include <mupdf/fitz/pool.h>
#include <mupdf/fitz/structured-text.h>
#include <mupdf/fitz/util.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/**
 * @brief Internal representation of a page in the collection.
 *
 * Each page stores a display list that can be rendered to produce
 * the page's visual content.
 */
struct page_s
{
  fz_display_list *dl;        ///< Display list for rendering (owned by this page)
  fz_stext_page *stext;
};

/**
 * @brief Ensures the pages array has capacity for at least `needed` pages.
 *
 * Grows the array by doubling capacity until it meets the requirement.
 * New slots are zero-initialized to ensure safe cleanup even if not all
 * slots are populated.
 *
 * Growth strategy: Start with 4 if empty, otherwise double capacity until
 * we meet the requirement. This provides O(log n) growth with amortized
 * O(1) insertions.
 *
 * @param pcoll Pointer to the PageCollection.
 * @param needed Minimum required capacity.
 * @return true if allocation succeeded, false if out of memory.
 */
static bool ensure_capacity(PageCollection *pcoll, size_t needed)
{
  if (needed <= pcoll->capacity)
    return true;

  // Double capacity until we meet the requirement
  size_t new_cap = pcoll->capacity ? pcoll->capacity : 4;
  while (new_cap < needed) new_cap *= 2;

  struct page_s *new_pages = realloc(pcoll->pages, new_cap * sizeof(struct page_s));
  if (!new_pages)
    return false;

  // Zero-initialize newly allocated slots to ensure safeCleanup
  memset(new_pages + pcoll->capacity, 0, (new_cap - pcoll->capacity) * sizeof(struct page_s));

  pcoll->pages = new_pages;
  pcoll->capacity = new_cap;
  return true;
}

/**
 * @brief Initialize a new PageCollection.
 *
 * Sets up default values for a fresh page collection:
 * - Empty pages array (NULL pointer)
 * - Zero capacity
 * - Prefix sum initialized via psum_init()
 * - count, extra1, extra2 set to 0 or 1
 * - ref_width set to a reasonable default of 500.0
 *
 * Must be called before using any other PageCollection functions.
 * Paired with pagecollection_finalize() for cleanup.
 */
void pagecollection_init(PageCollection *pcoll)
{
  pcoll->pages = NULL;
  pcoll->capacity = 0;
  psum_init(&pcoll->psum);
  pcoll->count = 0;
  pcoll->valid = 0;
  pcoll->extra1 = 1;
  pcoll->extra2 = 1;
  pcoll->ref_width = 500.0;
}

/**
 * @brief Cleanup and free all resources in a PageCollection.
 *
 * This is the destructor for PageCollection. It must be called
 * to prevent memory leaks and to properly release MuPDF resources:
 * - Drops all display lists in the pages array
 * - Finalizes the prefix sum structure
 * - Frees the pages array itself
 *
 * @param ctx MuPDF context for display list cleanup.
 * @param pcoll Pointer to the PageCollection to finalize (may be NULL).
 */
void pagecollection_finalize(fz_context *ctx, PageCollection *pcoll)
{
  if (!pcoll) return;

  // Drop all display lists
  for (size_t i = 0; i < pcoll->count; ++i)
  {
    fz_drop_display_list(ctx, pcoll->pages[i].dl);
    if (pcoll->pages[i].stext)
      fz_drop_stext_page(ctx, pcoll->pages[i].stext);
  }

  // Cleanup prefix sum and free pages array
  psum_finalize(&pcoll->psum);
  free(pcoll->pages);
}

/**
 * @brief Get the count of pages in the collection.
 */
size_t pagecollection_count(const PageCollection *pcoll)
{
  return pcoll ? pcoll->count : 0;
}

/**
 * @brief Returns the number of valid (loaded, up-to-date) pages.
 */
size_t pagecollection_valid_count(const PageCollection *pcoll)
{
  return pcoll ? pcoll->valid : 0;
}

/**
 * @brief Get a display list for a specific page.
 *
 * @param pcoll Pointer to the PageCollection.
 * @param index Zero-based page index.
 * @return Display list for the page, or NULL if index is out of bounds.
 */
fz_display_list *pagecollection_get(const PageCollection *pcoll, size_t index)
{
  if (!pcoll || index >= pcoll->count)
    return NULL;
  return pcoll->pages[index].dl;
}

fz_stext_page *pagecollection_get_stext(fz_context *ctx, const PageCollection *pcoll, size_t index)
{
  if (!pcoll || index >= pcoll->count)
    return NULL;
  if (!pcoll->pages[index].stext && pcoll->pages[index].dl)
  {
    pcoll->pages[index].stext =
        fz_new_stext_page_from_display_list(ctx, pcoll->pages[index].dl, NULL);
  }
  return pcoll->pages[index].stext;
}

/**
 * @brief Low-level page setter that stores a display list without bounds checking.
 *
 * This is an internal helper function that:
 * - Keeps a reference to the new display list (fz_keep_display_list)
 * - Drops the old display list (fz_drop_display_list)
 * - Updates the prefix sum with the provided height
 *
 * @note Caller must ensure index is valid and dl is not NULL.
 * @note This does not update reference_width or perform capacity checks.
 */
static void pagecollection_set_raw(fz_context *ctx, PageCollection *pcoll, size_t index, fz_display_list *dl, float height)
{
  fz_keep_display_list(ctx, dl);
  fz_drop_display_list(ctx, pcoll->pages[index].dl);
  pcoll->pages[index].dl = dl;
  if (pcoll->pages[index].stext)
  {
    fz_drop_stext_page(ctx, pcoll->pages[index].stext);
    pcoll->pages[index].stext = NULL;
  }
  psum_set(&pcoll->psum, index, height);
}

/**
 * @brief Add or replace a page's display list.
 *
 * This function handles the full lifecycle of page management:
 *
 * 1. Capacity expansion (if needed):
 *    - Uses Fibonacci-like growth strategy for efficient reallocations
 *    - extra1, extra2 track the growth increment pattern
 *    - Each expansion increases count by extra2, then updates extra1/extra2
 *
 * 2. Gap filling:
 *    - If expanding past the current count, fills gaps with empty display lists
 *    - This ensures all page indices in [0, new_count) are valid
 *
 * 3. Reference width tracking:
 *    - If setting page 0, updates ref_width from the page's bounds
 *    - This establishes the baseline for zoom calculations
 *
 * 4. Actual storage:
 *    - Calls pagecollection_set_raw() to store the display list
 *    - Updates prefix sum with page height
 *
 * @param ctx MuPDF context for display list management.
 * @param pcoll Pointer to the PageCollection.
 * @param index Zero-based page index where the display list will be stored.
 * @param dl Display list to store (must not be NULL).
 */
void pagecollection_set(fz_context *ctx, PageCollection *pcoll,
                        size_t index, fz_display_list *dl)
{
  if (!pcoll) return;

  // Get page bounds to compute height
  fz_rect bounds = fz_bound_display_list(ctx, dl);
  float height = bounds.y1 - bounds.y0;

  // Expand if necessary
  if (index >= pcoll->count)
  {
    size_t new_count = pcoll->count;
    size_t extra1 = pcoll->extra1, extra2 = pcoll->extra2;

    // Grow using Fibonacci-like strategy
    // This provides good amortized performance for sequential additions
    while (new_count <= index)
    {
      new_count += extra2;
      size_t extra = extra2 + extra1;
      extra1 = extra2;
      extra2 = extra;
    }

    // Ensure capacity
    if (!ensure_capacity(pcoll, new_count))
      return;

    // Fill gaps with empty display lists if there's a gap
    // (e.g., adding page 10 when count is 3 creates pages 3-9 as empty)
    if (new_count != index + 1)
    {
      fz_display_list *empty = fz_new_display_list(ctx, bounds);
      for (size_t i = pcoll->count; i < new_count; i++)
        pagecollection_set_raw(ctx, pcoll, i, empty, height);
      fz_drop_display_list(ctx, empty);
    }

    pcoll->count = new_count;
    pcoll->extra1 = extra1;
    pcoll->extra2 = extra2;
  }

  // Update reference width from first page
  if (index == 0)
    pcoll->ref_width = bounds.x1 - bounds.x0;

  // Store the display list
  pagecollection_set_raw(ctx, pcoll, index, dl, height);

  if (index >= pcoll->valid)
    pcoll->valid = index + 1;
}

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
void pagecollection_invalidate_after(PageCollection *pcoll, size_t count)
{
  if (pcoll && pcoll->valid > count)
    pcoll->valid = count;
}

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
void pagecollection_truncate(fz_context *ctx, PageCollection *pcoll)
{
  size_t count = pcoll->valid;
  if (!pcoll || pcoll->count == count)
    return;

  // Free pages >= count
  for (size_t i = count, old_count = pcoll->count; i < old_count; ++i)
  {
    fz_drop_display_list(ctx, pcoll->pages[i].dl);
    pcoll->pages[i].dl = NULL;
  }

  psum_truncate(&pcoll->psum, count);
  pcoll->count = count;
  pcoll->extra1 = 1;
  pcoll->extra2 = 1;
}

/**
 * @brief Binary search for the page at a given scroll offset.
 *
 * Uses the prefix sum structure to efficiently find which page contains
 * the given scroll offset. The offset is in world coordinates, which
 * accounts for both page heights and separator spacing.
 *
 * Algorithm:
 * - Uses psum_reverse_query_with_offset() which performs binary search
 *   on the prefix sum array with linear offset compensation
 * - Result is clamped to [0, count-1] to handle edge cases
 *
 * Examples:
 * - offset = 0: Returns page 0 (top of document)
 * - offset = page_offset(N): Returns page N+1 (start of next page)
 * - offset = page_offset(N) + page_height(N)/2: Returns page N
 *
 * @param pcoll Pointer to the PageCollection.
 * @param offset Vertical scroll position (in world coordinates).
 * @param separator Vertical spacing between pages (in world coordinates).
 * @return Page index containing the offset, clamped to [0, count-1].
 */
ssize_t pagecollection_page_below(PageCollection *pcoll, float offset, float separator)
{
  int candidate = psum_reverse_query_with_offset(&pcoll->psum, offset, separator);
  if (candidate >= pcoll->count)
    candidate = pcoll->count - 1;
  return candidate;
}

/**
 * @brief Find the page above a given offset.
 *
 * This function determines which page would be visible just above a given
 * scroll position. It's useful for computing scroll ranges when rendering.
 *
 * Implementation: Simply delegates to pagecollection_page_below()
 * with offset - separator, effectively moving the query point up by
 * one separator unit.
 *
 * @param pcoll Pointer to the PageCollection.
 * @param offset Vertical scroll position (in world coordinates).
 * @param separator Vertical spacing between pages (in world coordinates).
 * @return Page index above the offset, clamped to [0, count-1].
 */
ssize_t pagecollection_page_above(PageCollection *pcoll, float offset, float separator)
{
  return pagecollection_page_below(pcoll, offset - separator, separator);
}

/**
 * @brief Get the vertical position of a page's top edge.
 *
 * Calculates the world-space Y coordinate where the page begins.
 * This accounts for:
 * - Cumulative height of all preceding pages (via prefix sum)
 * - Separator spacing for each page boundary
 *
 * Formula: page_offset(page) = psum_query(page) + page * separator
 *
 * Where psum_query(page) = sum of heights[0..page-1]
 *
 * @param pcoll Pointer to the PageCollection.
 * @param page Zero-based page index.
 * @param separator Vertical spacing between pages (in world coordinates).
 * @return Y coordinate of the page's top edge (in world coordinates).
 */
float pagecollection_page_offset(PageCollection *pcoll, ssize_t page, float separator)
{
  return psum_query(&pcoll->psum, page) + page * separator;
}

/**
 * @brief Compute the total document height with separators.
 *
 * Formula: total_height = psum_total() + (count - 1) * separator
 *
 * Where psum_total() = sum of all page heights
 *
 * This represents the full document height including:
 * - All page content heights
 * - Spacer gaps between pages (count-1 separators)
 *
 * @param pcoll Pointer to the PageCollection.
 * @param separator Vertical spacing between pages (in world coordinates).
 * @return Total document height including separators.
 */
float pagecollection_total_height(PageCollection *pcoll, float separator)
{
  return psum_total(&pcoll->psum) + separator * (pcoll->count - 1);
}

/**
 * @brief Get the reference width of the document.
 *
 * The reference width is established when the first page is added
 * and is used for consistent zoom calculations throughout the viewer.
 * All pages are scaled relative to this width.
 *
 * @param pcoll Pointer to the PageCollection.
 * @return Reference width, or 0.0 if pcoll is NULL.
 */
float pagecollection_reference_width(const PageCollection *pcoll)
{
  return pcoll->ref_width;
}
