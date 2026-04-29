#include "synctex_lcs.h"

#include <float.h>
#include <math.h>
#include <mupdf/fitz/geometry.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "mupdf/fitz/structured-text.h"

/* ────────── */
/* WordHasher */
/* ────────── */

typedef union
{
  uint8_t hash[8];
  uint64_t code;
} WordHash;

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

bool WordHash_valid(const WordHash a)
{
  return a.code > 0;
}

/* ────────── */
/* WordHasher */
/* ────────── */

typedef struct
{
  WordHash hash;
  int prefix;
} WordHasher;

void WordHasher_init(WordHasher *c)
{
  *c = (WordHasher){0,};
}

void WordHasher_push(WordHasher *c, uint32_t cp)
{
  while (c->prefix < 4 && cp)
  {
    c->hash.hash[c->prefix] = cp & 0xFF;
    cp >>= 8;
    c->prefix++;
  }
  while (cp)
  {
    uint8_t h4 = c->hash.hash[4];
    c->hash.hash[4] = ((h4 << 1) | (h4 >> 7)) ^ c->hash.hash[5];
    c->hash.hash[5] = c->hash.hash[6];
    c->hash.hash[6] = c->hash.hash[7];
    c->hash.hash[7] = cp & 0xFF;
    cp >>= 8;
  }
}

/* Emit hash and reset state */
WordHash WordHasher_flush(WordHasher *c)
{
  WordHash result = c->hash;
  *c = (WordHasher){0,};
  return result;
}

/* ─────────── */
/* Utf8Decoder */
/* ─────────── */

typedef struct
{
  uint32_t cp;
  int state;    // 0 = idle, >0 = bytes remaining to complete sequence
  int seq_len;  // 2, 3, or 4 (only valid when state > 0)
} Utf8Decoder;

/* Initialize decoder state */
void Utf8Decoder_init(Utf8Decoder *d)
{
  d->cp = 0;
  d->state = 0;
  d->seq_len = 0;
}

/*
 * Feed a single UTF-8 byte to the decoder.
 * Returns:
 *   > 0 : Valid codepoint ready
 *   -1  : Incomplete sequence (waiting for more bytes)
 *    0  : Invalid sequence (flushed)
 */
int Utf8Decoder_next(Utf8Decoder *d, uint8_t b)
{
  if (d->state == 0)
  {
    /* Start of a new sequence */
    if (b < 0x80)
      return b; /* 1-byte ASCII */
    else if (b >= 0xC2 && b < 0xE0)
    {
      d->cp = b & 0x1F;
      d->state = 1;
      d->seq_len = 2;
    }
    else if (b >= 0xE0 && b < 0xF0)
    {
      d->cp = b & 0x0F;
      d->state = 2;
      d->seq_len = 3;
    }
    else if (b >= 0xF0 && b <= 0xF4)
    {
      d->cp = b & 0x07;
      d->state = 3;
      d->seq_len = 4;
    }
    else
      return 0; /* Invalid start byte (0x80-0xBF, 0xC0-0xC1, 0xF5-0xFF) */
    return -1; /* Incomplete */
  }
  /* Expecting continuation byte (10xxxxxx) */
  else if (b < 0x80 || b >= 0xC0)
  {
    d->state = 0;
    return 0; /* Invalid continuation → flush */
  }
  else
  {
    d->cp = (d->cp << 6) | (b & 0x3F);
    d->state--;

    if (d->state == 0)
    {
      /* Sequence complete → validate against RFC 3629 */
      if (d->seq_len == 2 && d->cp < 0x80)
        return 0; /* Overlong 2-byte */
      if (d->seq_len == 3 &&
          (d->cp < 0x800 || (d->cp >= 0xD800 && d->cp <= 0xDFFF)))
        return 0; /* Overlong 3-byte or surrogate */
      if (d->seq_len == 4 && (d->cp < 0x10000 || d->cp > 0x10FFFF))
        return 0; /* Overlong 4-byte or out of range */
      return d->cp;
    }
    return -1; /* Still incomplete */
  }
}

/* ─────────────── */
/* Unicode helpers */
/* ─────────────── */

static inline int is_non_word_char(uint32_t cp)
{
  if (cp < 0x80)
    return !((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
             (cp >= '0' && cp <= '9'));
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

typedef struct {
  WordHash *words;
  int length, capacity;
} WordHashes;

void WordHashes_init(WordHashes *wh)
{
  *wh = (WordHashes){0,};
}

void WordHashes_finalize(WordHashes *wh)
{
  free(wh->words);
  *wh = (WordHashes){0,};
}

void WordHashes_clear(WordHashes *wh)
{
  wh->length = 0;
}

void WordHashes_append(WordHashes *whs, WordHash wh)
{
  if (whs->length == whs->capacity)
  {
    int cap = whs->capacity ? whs->capacity * 2 : 32;
    whs->words = realloc(whs->words, sizeof(WordHash) * cap);
    if (!whs->words)
      abort();
  }
  whs->words[whs->length++] = wh;
}

static bool iter_word(
    void *env,
    fz_stext_char *first,
    fz_stext_char *last,
    WordHash wh,
    bool (*fn)(void *, fz_stext_char *first, fz_stext_char *last, WordHash wh))
{
  return WordHash_valid(wh) && fn(env, first, last, wh);
}

static bool WordHash_iter_page(
    fz_stext_page *page,
    bool (*fn)(void *, fz_stext_char *first, fz_stext_char *last, WordHash wh),
    void *env)
{
  WordHasher hasher;
  WordHasher_init(&hasher);
  fz_stext_char *first, *last;

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
          if (!ch->next)
            return false;
        }
        else
        {
          if (first == NULL)
            first = ch;
          last = ch;
          WordHasher_push(&hasher, cp);
        }
      }
      if (iter_word(env, first, last, WordHasher_flush(&hasher), fn))
        return true;
    }
  }
  return iter_word(env, first, last, WordHasher_flush(&hasher), fn);
}

static bool append_word(void *env, fz_stext_char *first, fz_stext_char *last, WordHash wh)
{
  (void)first;
  (void)last;
  WordHashes *whs = env;
  WordHashes_append(whs, wh);
  return false;
}

void WordHashes_append_page(WordHashes *whs, fz_stext_page *page)
{
  WordHash_iter_page(page, append_word, whs);
}

struct counter {
  int count;
  fz_point *first, *last;
};

static bool iter_counter(void *env, fz_stext_char *first, fz_stext_char *last, WordHash wh)
{
  struct counter *counter = env;

  if (counter->count > 0)
  {
    counter->count--;
    return false;
  }

  *counter->first = first->origin;
  *counter->last = last->origin;
  return true;
}

bool WordHash_page_index_to_coord(fz_stext_page *page,
                                  int index,
                                  fz_point *first,
                                  fz_point *last)
{
  struct counter counter = {
      .count = index,
      .first = first,
      .last = last,
  };
  return WordHash_iter_page(page, iter_counter, &counter);
}

struct finder {
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

static bool iter_finder(void *env, fz_stext_char *first, fz_stext_char *last, WordHash wh)
{
  struct finder *finder = env;

  bool a = first && iter_finder_point(finder, first->origin);
  bool b = last && iter_finder_point(finder, last->origin);

  if (a || b)
    finder->index = finder->best;

  finder->best++;

  return false;
}

int WordHash_page_coord_to_index(fz_stext_page *page, fz_point point, fz_point *hit)
{
  struct finder finder = {
      .index = 0,
      .best = -1,
      .hit = hit,
      .target = point,
      .sqdist = INFINITY,
  };
  WordHash_iter_page(page, iter_finder, &finder);
  return finder.best;
}
