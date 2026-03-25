#ifndef MULTIPAGE_H
#define MULTIPAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <mupdf/fitz.h>

// ============================================================================
// Multipage Document API
// ============================================================================

typedef struct multipage_s multipage_t;

// Terminology
// ===========
// 
// Lifecycle: multipage_t manages a collection of fz_display_list objects and
//   tracks their vertical positions.
// Ownership: multipage_t holds a reference to each stored display list.
//   multipage_free() decrements these references and frees resources.
// Validity and Count:
//   Documents are populated sequentially from index 0 and invalidated only
//   by backtracking (marking all pages past a certain index as invalid).
//   While populating, `count` grows automatically as needed.
//
// `valid_count` reflects pages that are set and known to be up-to-date.
// `count` may exceed `valid_count` in two situations:
// - Backtracking occurred (multipage_invalidate_after): valid_count decreases
//   but count remains unchanged.
// - Pages were added beyond current count (multipage_set_page): valid_count
//   grows exactly as needed, but blank pages are added to count to prepare for
//   future additions without flickering.
//
// A page is 'valid' if it contains loaded, up-to-date content.
// Visually, pages beyond valid_count are 'invalid' (stale or empty).
// multipage_truncate() drops invalid pages once the actual document length is known.

// Lifecycle
// =========

// Allocates and initializes a new multipage structure.
// Returns NULL on allocation failure.
multipage_t *multipage_new(void);

// Frees the multipage structure and releases all internal display list references.
// Requires fz_context *ctx for proper resource cleanup.
void multipage_free(fz_context *ctx, multipage_t *mp);

// State query
// ===========

// Returns the total number of visible pages.
// Returns 0 if the structure is empty.
size_t multipage_count(const multipage_t *mp);

// Returns the number of valid (loaded, up-to-date) pages.
// Always <= multipage_count(mp).
size_t multipage_valid_count(const multipage_t *mp);

// Returns the display list at `index`.
// Returns NULL if `index` is out of bounds or the page is invalid.
fz_display_list *multipage_get(const multipage_t *mp, size_t index);

// State modification
// ==================

// Sets the display list for `index` and increments its reference count.
// Marks all pages from 0 to `index` as valid.
// Caller retains ownership; multipage_t acquires its own reference to dl.
// If `index >= multipage_count()`, `multipage_count()` is grown to be higher
// than `index` (possibly by many pages, to avoid visual instabilities caused by
// repeatedly adding a single page).
void multipage_set_page(fz_context *ctx, multipage_t *mp, size_t index, fz_display_list *dl);

// Marks all pages at indices >= count as invalid (stale).
// Pages are not freed; they remain displayed until replaced or truncated.
// Updates valid_count to `count` (count remains unchanged).
void multipage_invalidate_after(multipage_t *mp, size_t count);

// Truncates the structure to `multipage_valid_count()` pages.
// Updates `count` to `valid_count`.
void multipage_truncate(fz_context *ctx, multipage_t *mp);

// ============================================================================
// Vertical layout
// ============================================================================
//
// Terminology
// ===========
// 
// Terminology
// - Coordinate System: Y=0 is at the top; Y increases downwards.
// - Page Index: Valid indices range from `0` to `multipage_count(mp) - 1`.
// - Empty Document: If count is 0, index functions return -1; height functions return 0.
// - Separator: Vertical space between pages (two contiguous pages are separated by `separator`).
// - Clamping: Query results are clamped to the valid index range if `offset` is out of bounds.

// Returns the index of the page visually above `offset` (rounds down).
// If the document is empty, returns -1. If `offset` is out of bounds, result is clamped.
ssize_t multipage_page_above(multipage_t *mp, float offset, float separator);

// Returns the index of the page visually below `offset` (rounds up).
// If the document is empty, returns -1. If `offset` is out of bounds, result is clamped.
ssize_t multipage_page_below(multipage_t *mp, float offset, float separator);

// Returns the vertical offset (Y-coordinate) at which `page` starts.
float multipage_page_offset(multipage_t *mp, ssize_t page, float separator);

// Returns the total height of the document.
// Formula: sum(page_heights) + separator * (multipage_count(mp) - 1)
// Returns 0 if the document is empty.
float multipage_total_height(multipage_t *mp, float separator);

#endif // MULTIPAGE_H
