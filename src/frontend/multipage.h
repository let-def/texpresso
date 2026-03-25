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

// // Marks pages >= count as invalidated (not freed, but will be skipped on render)
// void multipage_invalidate_after(multipage_t *mp, int count);
//
// // Returns true if pages >= count were invalidated (i.e., need rebuilding)
// bool multipage_has_invalidated_pages(const multipage_t *mp);
//
// // Clear invalidation flags (e.g., after rebuild)
// void multipage_clear_invalidated(multipage_t *mp);

int multipage_page_below(multipage_t *mp, float offset, float separator);

int multipage_page_above(multipage_t *mp, float offset, float separator);

float multipage_page_offset(multipage_t *mp, int page, float separator);

float multipage_total_height(multipage_t *mp, float separator);

#endif // MULTIPAGE_H
