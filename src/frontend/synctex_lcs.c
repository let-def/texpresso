#include "synctex_lcs.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────
 * 1. Core Hash Utilities
 * ────────────────────────────────────────────────────────────── */

bool WordHash_valid(const WordHash hash)
{
  return hash.code > 0;
}

int WordHash_match(const WordHash a, const WordHash b)
{
  if (a.code == b.code)
    return 10;

  int acc = 0;
  for (int i = 0; i < 8; i++)
    if (a.hash[i] == b.hash[i])
      acc++;
  return acc;
}

/* ──────────────────────────────────────────────────────────────
 * 2. WordHasher Implementation
 * ────────────────────────────────────────────────────────────── */

void WordHasher_init(WordHasher *hasher)
{
  *hasher = (WordHasher){
      0,
  };
}

void WordHasher_push(WordHasher *hasher, uint32_t cp)
{
  /* Fill first 4 bytes with the initial codepoint (little-endian) */
  while (hasher->prefix < 4 && cp)
  {
    hasher->hash.hash[hasher->prefix] = cp & 0xFF;
    cp >>= 8;
    hasher->prefix++;
  }

  /* Shift-XOR register for subsequent bytes */
  while (cp)
  {
    uint8_t h4 = hasher->hash.hash[4];
    hasher->hash.hash[4] = ((h4 << 1) | (h4 >> 7)) ^ hasher->hash.hash[5];
    hasher->hash.hash[5] = hasher->hash.hash[6];
    hasher->hash.hash[6] = hasher->hash.hash[7];
    hasher->hash.hash[7] = cp & 0xFF;
    cp >>= 8;
  }
}

WordHash WordHasher_flush(WordHasher *hasher)
{
  WordHash result = hasher->hash;
  *hasher = (WordHasher){
      0,
  };
  return result;
}

/* ──────────────────────────────────────────────────────────────
 * 3. Utf8Decoder Implementation
 * ────────────────────────────────────────────────────────────── */

void Utf8Decoder_init(Utf8Decoder *decoder)
{
  decoder->cp = 0;
  decoder->state = 0;
  decoder->seq_len = 0;
}

int Utf8Decoder_next(Utf8Decoder *decoder, uint8_t b)
{
  if (decoder->state == 0)
  {
    /* Start of a new sequence */
    if (b < 0x80)
      return b; /* 1-byte ASCII */
    else if (b >= 0xC2 && b < 0xE0)
    {
      decoder->cp = b & 0x1F;
      decoder->state = 1;
      decoder->seq_len = 2;
    }
    else if (b >= 0xE0 && b < 0xF0)
    {
      decoder->cp = b & 0x0F;
      decoder->state = 2;
      decoder->seq_len = 3;
    }
    else if (b >= 0xF0 && b <= 0xF4)
    {
      decoder->cp = b & 0x07;
      decoder->state = 3;
      decoder->seq_len = 4;
    }
    else
      return 0; /* Invalid start byte */
    return -1;  /* Incomplete */
  }

  /* Expecting continuation byte (10xxxxxx) */
  if (b < 0x80 || b >= 0xC0)
  {
    decoder->state = 0;
    return 0; /* Invalid continuation → flush */
  }

  decoder->cp = (decoder->cp << 6) | (b & 0x3F);
  decoder->state--;

  if (decoder->state == 0)
  {
    /* Sequence complete → validate against RFC 3629 */
    if (decoder->seq_len == 2 && decoder->cp < 0x80)
      return 0; /* Overlong 2-byte */
    if (decoder->seq_len == 3 &&
        (decoder->cp < 0x800 ||
         (decoder->cp >= 0xD800 && decoder->cp <= 0xDFFF)))
      return 0; /* Overlong 3-byte or surrogate */
    if (decoder->seq_len == 4 &&
        (decoder->cp < 0x10000 || decoder->cp > 0x10FFFF))
      return 0; /* Overlong 4-byte or out of range */
    return decoder->cp;
  }
  return -1; /* Still incomplete */
}

/* ──────────────────────────────────────────────────────────────
 * 4. Unicode Helpers & Internal Iteration Engine
 * ────────────────────────────────────────────────────────────── */

/**
 * @brief Determines if a codepoint is a standalone semantic unit (symbol, CJK,
 * etc.)
 * @return 1 if non-word char, 0 if alphabetic/numeric
 */
static inline int is_non_word_char(uint32_t cp)
{
  if (cp < 0x80)
    return !((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
             (cp >= '0' && cp <= '9'));
  /* Latin Extended, Greek, Cyrillic, Arabic, Devanagari are treated as word
   * chars */
  if (cp >= 0x00C0 && cp <= 0x024F)
    return 0;
  if (cp >= 0x0370 && cp <= 0x03FF)
    return 0;
  if (cp >= 0x0400 && cp <= 0x04FF)
    return 0;
  if (cp >= 0x0600 && cp <= 0x06FF)
    return 0;
  if (cp >= 0x0900 && cp <= 0x097F)
    return 0;
  return 1;
}

/**
 * @brief Internal callback wrapper that validates hash before invocation
 */
static bool iter_word(
    void *env,
    fz_stext_char *first,
    fz_stext_char *last,
    WordHash wh,
    bool (*fn)(void *, fz_stext_char *, fz_stext_char *, WordHash))
{
  return WordHash_valid(wh) && fn(env, first, last, wh);
}

/**
 * @brief Core page traversal engine. Walks DFS order, respects boundaries,
 * and invokes a callback for each recognized word/semantic unit.
 */
static bool WordHash_iter_page(
    fz_stext_page *page,
    bool (*fn)(void *, fz_stext_char *, fz_stext_char *, WordHash),
    void *env)
{
  WordHasher hasher;
  WordHasher_init(&hasher);
  fz_stext_char *first = NULL, *last = NULL;

  for (fz_stext_page_block_iterator it =
           fz_stext_page_block_iterator_begin_dfs(page);
       !fz_stext_page_block_iterator_eod_dfs(it);
       it = fz_stext_page_block_iterator_next_dfs(it))
  {
    if (it.block->type != FZ_STEXT_BLOCK_TEXT)
      continue;

    for (fz_stext_line *line = it.block->u.t.first_line; line;
         line = line->next)
    {
      bool unterminated = false;
      for (fz_stext_char *ch = line->first_char; ch; ch = ch->next)
      {
        uint32_t cp = ch->c;

        if (fz_is_unicode_space_equivalent(cp) || fz_is_unicode_whitespace(cp))
        {
          if (iter_word(env, first, last, WordHasher_flush(&hasher), fn))
            return true;
          first = last = NULL;
        }
        else if (is_non_word_char(cp))
        {
          if (iter_word(env, first, last, WordHasher_flush(&hasher), fn))
            return true;
          WordHasher_push(&hasher, cp);
          if (iter_word(env, ch, ch, WordHasher_flush(&hasher), fn))
            return true;
          first = last = NULL;
        }
        else if (fz_is_unicode_hyphen(cp))
        {
          /* Hyphens are ignored */
          /* Hyphens at end of line causes the word to continue on next line */
          if (!ch->next)
          {
            unterminated = true;
            break;
          }
        }
        else
        {
          if (first == NULL)
            first = ch;
          last = ch;
          WordHasher_push(&hasher, cp);
        }
      }
      /* Flush line boundary */
      if (!unterminated)
        if (iter_word(env, first, last, WordHasher_flush(&hasher), fn))
          return true;
    }
  }
  /* Flush final word (if page finished on hyphen) */
  return iter_word(env, first, last, WordHasher_flush(&hasher), fn);
}

/* ──────────────────────────────────────────────────────────────
 * 5. Dynamic Hash Array Implementation
 * ────────────────────────────────────────────────────────────── */

void WordHashes_init(WordHashes *arr)
{
  *arr = (WordHashes){
      0,
  };
}

void WordHashes_finalize(WordHashes *arr)
{
  free(arr->words);
  *arr = (WordHashes){
      0,
  };
}

void WordHashes_clear(WordHashes *arr)
{
  arr->length = 0;
}

void WordHashes_append(WordHashes *arr, WordHash hash)
{
  if (arr->length == arr->capacity)
  {
    int cap = arr->capacity ? arr->capacity * 2 : 32;
    arr->words = realloc(arr->words, sizeof(WordHash) * cap);
    if (!arr->words)
      abort();
  }
  arr->words[arr->length++] = hash;
}

static bool append_word(void *env,
                        fz_stext_char *first,
                        fz_stext_char *last,
                        WordHash wh)
{
  (void)first;
  (void)last;
  WordHashes *arr = env;
  WordHashes_append(arr, wh);
  return false;
}

void WordHashes_append_page(WordHashes *arr, fz_stext_page *page)
{
  WordHash_iter_page(page, append_word, arr);
}

/* ──────────────────────────────────────────────────────────────
 * 6. Index ↔ Coordinate Mapping
 * ────────────────────────────────────────────────────────────── */

struct counter
{
  int count;
  fz_point *first, *last;
};

static bool iter_counter(void *env,
                         fz_stext_char *first,
                         fz_stext_char *last,
                         WordHash wh)
{
  struct counter *ctr = env;
  if (ctr->count > 0)
  {
    ctr->count--;
    return false;
  }
  *ctr->first = first->origin;
  *ctr->last = last->origin;
  return true;
}

bool WordHash_page_index_to_coord(fz_stext_page *page,
                                  int index,
                                  fz_point *first,
                                  fz_point *last)
{
  struct counter ctr = {.count = index, .first = first, .last = last};
  return WordHash_iter_page(page, iter_counter, &ctr);
}

struct finder
{
  int index;
  int best;
  fz_point target;
  fz_point *hit;
  float sqdist;
};

static bool iter_finder_point(struct finder *finder, fz_point point)
{
  float dx = point.x - finder->target.x;
  float dy = point.y - finder->target.y;
  float sqdist = dx * dx + dy * dy;

  if (sqdist < finder->sqdist)
  {
    *finder->hit = point;
    finder->sqdist = sqdist;
    return true;
  }
  return false;
}

static bool iter_finder(void *env,
                        fz_stext_char *first,
                        fz_stext_char *last,
                        WordHash wh)
{
  struct finder *finder = env;
  bool a = first && iter_finder_point(finder, first->origin);
  bool b = last && iter_finder_point(finder, last->origin);

  if (a || b)
    finder->index = finder->best;
  finder->best++;
  return false;
}

int WordHash_page_coord_to_index(fz_stext_page *page,
                                 fz_point point,
                                 fz_point *hit)
{
  struct finder finder = {
      .index = 0, .best = -1, .hit = hit, .target = point, .sqdist = INFINITY};
  WordHash_iter_page(page, iter_finder, &finder);
  return finder.best;
}

/* ──────────────────────────────────────────────────────────────
 * 7. Weighted-LCS for alignement of WordHashes
 * ────────────────────────────────────────────────────────────── */

/* ──────────────────────────────────────────────────────────────
 * Forward Weighted-LCS: Aligns a[0..n-1] with b[0..m-1]
 * Returns: heap-allocated array of size (m+1). Caller must free().
 *          Returns NULL on allocation failure.
 * ────────────────────────────────────────────────────────────── */
static int *wlcs_forward(const WordHash *a, int n, const WordHash *b, int m)
{
  if (n < 0 || m < 0)
    return NULL;

  int *prev = calloc(sizeof(int), m + 1);
  int *curr = calloc(sizeof(int), m + 1);
  if (!prev || !curr)
  {
    free(prev);
    free(curr);
    return NULL;
  }

  for (int i = 1; i <= n; i++)
  {
    curr[0] = 0; /* Boundary: empty b prefix */
    for (int j = 1; j <= m; j++)
    {
      int score = WordHash_match(a[i - 1], b[j - 1]);
      int diag = prev[j - 1] + score;
      int up = prev[j];
      int left = curr[j - 1];
      curr[j] = (diag >= up) ? ((diag >= left) ? diag : left)
                             : ((up >= left) ? up : left);
    }
    /* Swap buffers: prev becomes the new result row */
    int *tmp = prev;
    prev = curr;
    curr = tmp;
  }

  free(curr); /* curr holds the discarded previous row */
  return prev;
}

/* ──────────────────────────────────────────────────────────────
 * Backward Weighted-LCS: Aligns a[0..n-1] with b[0..m-1] in reverse
 * Returns: heap-allocated array of size (m+1). Caller must free().
 *          Returns NULL on allocation failure.
 * ────────────────────────────────────────────────────────────── */
static int *wlcs_backward(const WordHash *a, int n, const WordHash *b, int m)
{
  if (n < 0 || m < 0)
    return NULL;

  int *prev = calloc(sizeof(int), m + 1);
  int *curr = calloc(sizeof(int), m + 1);
  if (!prev || !curr)
  {
    free(prev);
    free(curr);
    return NULL;
  }

  for (int i = n - 1; i >= 0; i--)
  {
    curr[m] = 0; /* Boundary: empty b suffix */
    for (int j = m - 1; j >= 0; j--)
    {
      int score = WordHash_match(a[i], b[j]);
      int diag = prev[j + 1] + score;
      int up = prev[j];       /* Skip b[j] */
      int left = curr[j + 1]; /* Skip a[i] */
      curr[j] = (diag >= up) ? ((diag >= left) ? diag : left)
                             : ((up >= left) ? up : left);
    }
    int *tmp = prev;
    prev = curr;
    curr = tmp;
  }

  free(curr);
  return prev;
}

/* ──────────────────────────────────────────────────────────────
 * Finds the optimal split point in b that maximizes combined
 * forward + backward similarity scores relative to a[a_index].
 * Memory-safe: allocates and frees DP rows internally.
 * ────────────────────────────────────────────────────────────── */
int WordHash_align(const WordHash *a,
                   size_t a_len,
                   size_t a_index,
                   const WordHash *b,
                   size_t b_len)
{
  if (a_index < 0 || a_index > a_len || b_len < 0)
    return -1;

  int *fwd = wlcs_forward(a, a_index, b, b_len);
  int *bwd = wlcs_backward(a + a_index, a_len - a_index, b, b_len);

  if (!fwd || !bwd)
  {
    free(fwd);
    free(bwd);
    return -1;
  }

  size_t best_j = 0;
  int best_score = -1;
  for (size_t j = 0; j <= b_len; j++)
  {
    int score = fwd[j] + bwd[j];
    if (score > best_score)
    {
      best_score = score;
      best_j = j;
    }
  }

  free(fwd);
  free(bwd);
  return best_j;
}
