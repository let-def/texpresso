#ifndef MULTIPAGE_H
#define MULTIPAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <mupdf/fitz.h>

typedef struct multipage_s multipage_t;

multipage_t *multipage_new(void);
void multipage_free(fz_context *ctx, multipage_t *mp);

size_t multipage_count(const multipage_t *mp);
fz_display_list *multipage_get(multipage_t *mp, size_t index);
//fz_rect multipage_get_bounds(const multipage_t *mp, size_t index);

// Increase reference count of dl
void multipage_set_page(fz_context *ctx, multipage_t *mp, size_t index, fz_display_list *dl);

// Truncates: frees and removes pages >= count
void multipage_truncate(fz_context *ctx, multipage_t *mp, size_t count);

// Marks pages >= count as invalidated (still displayed but marked out of date)
void multipage_invalidate_after(multipage_t *mp, size_t count);

// Returns true if pages >= count were invalidated (i.e., need rebuilding)
size_t multipage_valid_count(const multipage_t *mp);

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
