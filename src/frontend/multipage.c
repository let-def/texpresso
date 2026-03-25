#include "multipage.h"
#include "psum.h"
#include <mupdf/fitz.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Internal page representation
typedef struct
{
  fz_display_list *dl;        // Display list (owned)
} page_t;

struct multipage_s
{
  page_t *pages;
  psum_t *psum;
  size_t capacity;       // allocated slots
  size_t count;          // logical count (0 <= count <= capacity)
  size_t valid;          // valid pages (0 <= valid <= count)
  size_t extra1, extra2; // fibonacci sequence for growing count
};

// Helper: ensure at least `needed` pages are allocated
static bool ensure_capacity(multipage_t *mp, size_t needed)
{
  if (needed <= mp->capacity)
    return true;

  size_t new_cap = mp->capacity ? mp->capacity : 4;
  while (new_cap < needed) new_cap *= 2;

  page_t *new_pages = realloc(mp->pages, new_cap * sizeof(page_t));
  if (!new_pages)
    return false;

  // Zero newly allocated pages
  memset(new_pages + mp->capacity, 0, (new_cap - mp->capacity) * sizeof(page_t));

  mp->pages = new_pages;
  mp->capacity = new_cap;
  return true;
}

// Public: constructor
multipage_t *multipage_new(void)
{
  multipage_t *mp = calloc(1, sizeof(multipage_t));
  if (!mp) return NULL;
  mp->pages = NULL;
  mp->capacity = 0;
  mp->psum = psum_new();
  mp->count = 0;
  mp->valid = 0;
  mp->extra1 = 1;
  mp->extra2 = 1;
  return mp;
}

// Public: destructor
void multipage_free(fz_context *ctx, multipage_t *mp)
{
  if (!mp) return;
  for (size_t i = 0; i < mp->count; ++i)
  {
    fz_drop_display_list(ctx, mp->pages[i].dl);
  }
  psum_free(mp->psum);
  free(mp->pages);
  free(mp);
}

size_t multipage_count(const multipage_t *mp)
{
  return mp ? mp->count : 0;
}

// Returns NULL if index < 0 or >= count
fz_display_list *multipage_get(const multipage_t *mp, size_t index)
{
  if (!mp || index >= mp->count)
    return NULL;
  return mp->pages[index].dl;
}

// fz_rect multipage_get_bounds(const multipage_t *mp, int index)
// {
//   static const fz_rect undefined_rect = {0, 0, 0, 0};
//   if (!mp || index < 0 || (size_t)index >= mp->count)
//   {
//     return undefined_rect;
//   }
//   // In real MuPDF, bounds may be cached — here, just return stored value
//   return mp->pages[index].bounds;
// }

static void multipage_set_raw(fz_context *ctx, multipage_t *mp, size_t index, fz_display_list *dl, float height)
{
  fz_keep_display_list(ctx, dl);
  fz_drop_display_list(ctx, mp->pages[index].dl);
  mp->pages[index].dl = dl;
  psum_set(mp->psum, index, height);
}

void multipage_set_page(fz_context *ctx, multipage_t *mp, size_t index, fz_display_list *dl)
{
  if (!mp) return;

  fz_rect bounds = fz_bound_display_list(ctx, dl);
  float height = bounds.y1 - bounds.y0;

  if (index >= mp->count)
  {
    size_t new_count = mp->count, extra1 = mp->extra1, extra2 = mp->extra2;

    while (new_count <= index)
    {
      new_count += extra2;
      size_t extra = extra2 + extra1;
      extra1 = extra2;
      extra2 = extra;
    }

    if (!ensure_capacity(mp, new_count))
      return;

    if (new_count != index + 1)
    {
      fz_display_list *empty = fz_new_display_list(ctx, bounds);
      for (size_t i = mp->count; i < new_count; i++)
        multipage_set_raw(ctx, mp, i, empty, height);
      fz_drop_display_list(ctx, empty);
    }

    mp->count = new_count;
    mp->extra1 = extra1;
    mp->extra2 = extra2;
  }

  multipage_set_raw(ctx, mp, index, dl, height);
  if (index >= mp->valid)
    mp->valid = index + 1;
}

void multipage_truncate(fz_context *ctx, multipage_t *mp)
{
  if (!mp)
    return;

  size_t count = mp->valid, old_count = mp->count;

  // Free pages >= count
  for (size_t i = count, old_count = mp->count; i < old_count; ++i)
  {
    fz_drop_display_list(ctx, mp->pages[i].dl);
    mp->pages[i].dl = NULL;
  }

  psum_truncate(mp->psum, count);
  mp->count = count;
  mp->extra1 = 1;
  mp->extra2 = 1;
}


// Marks pages >= count as invalidated (still displayed but marked out of date)
void multipage_invalidate_after(multipage_t *mp, size_t count)
{
  if (mp && mp->valid > count)
    mp->valid = count;
}

// Returns true if pages >= count were invalidated (i.e., need rebuilding)
size_t multipage_valid_count(const multipage_t *mp)
{
  return mp ? mp->valid : 0;
}

ssize_t multipage_page_below(multipage_t *mp, float offset, float separator)
{
  int candidate = psum_reverse_query_with_offset(mp->psum, offset, separator);
  if (candidate >= mp->count)
    candidate = mp->count - 1;
  return candidate;
}

ssize_t multipage_page_above(multipage_t *mp, float offset, float separator)
{
  return multipage_page_below(mp, offset - separator, separator);
}

float multipage_total_height(multipage_t *mp, float separator)
{
  if (mp->count == 0)
    return 0;
  return psum_total(mp->psum) + separator * (mp->count - 1);
}

float multipage_page_offset(multipage_t *mp, ssize_t page, float separator)
{
  return psum_query(mp->psum, page) + page * separator;
}
