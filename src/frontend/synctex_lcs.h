#ifndef SYNCTEX_LCS_H_
#define SYNCTEX_LCS_H_

#include <mupdf/fitz.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Perceptual word hashing, UTF-8 decoding, and MuPDF text extraction
 * utilities.
 *
 * Provides a streaming pipeline to convert UTF-8 or Unicode codepoints into
 * compact 8-byte perceptual hashes. Includes utilities for dynamic hash storage
 * and direct integration with MuPDF's structured text model.
 */

/* ──────────────────────────────────────────────────────────────
 * 1. Core Hash Type & Utilities
 * ────────────────────────────────────────────────────────────── */

/**
 * @brief A perceptual hash representing a word or semantic unit.
 * Stores an 8-byte hash that enables fast bitwise similarity comparison.
 */
typedef union
{
  uint8_t hash[8];
  uint64_t code;
} WordHash;

/**
 * @brief Check if a WordHash contains valid data.
 * @param hash Hash to validate
 * @return true if hash.code > 0, false otherwise.
 */
bool WordHash_valid(const WordHash hash);

/**
 * @brief Compute similarity score between two WordHash values.
 * @param a First hash
 * @param b Second hash
 * @return 10 for exact match, 0-8 for partial match (number of equal bytes).
 */
int WordHash_match(const WordHash a, const WordHash b);

/* ──────────────────────────────────────────────────────────────
 * 2. WordHasher: Incremental Hash Accumulator
 * ────────────────────────────────────────────────────────────── */

/**
 * @brief Stateful accumulator that converts Unicode codepoints into a WordHash.
 *
 * Algorithm:
 * - Bytes 0-3: Store the first codepoint (LE)
 * - Bytes 4-7: Shift-XOR register for subsequent codepoints
 */
typedef struct
{
  WordHash hash;
  int prefix;
} WordHasher;

/** @brief Initialize a WordHasher to a clean state. */
void WordHasher_init(WordHasher *hasher);

/**
 * @brief Feed a Unicode codepoint into the hasher.
 * @param hasher Pointer to the hasher state
 * @param cp Unicode codepoint (UTF-32)
 */
void WordHasher_push(WordHasher *hasher, uint32_t cp);

/**
 * @brief Finalize and retrieve the accumulated hash.
 * Resets the hasher state after extraction.
 * @return The computed WordHash
 */
WordHash WordHasher_flush(WordHasher *hasher);

/* ──────────────────────────────────────────────────────────────
 * 3. Utf8Decoder: Streaming UTF-8 to Codepoint Converter
 * ────────────────────────────────────────────────────────────── */

/**
 * @brief Stateful decoder for streaming UTF-8 byte sequences.
 */
typedef struct
{
  uint32_t cp;
  int state;   /**< 0 = idle, >0 = bytes remaining */
  int seq_len; /**< Expected sequence length (2, 3, or 4) */
} Utf8Decoder;

/** @brief Initialize the UTF-8 decoder state. */
void Utf8Decoder_init(Utf8Decoder *decoder);

/**
 * @brief Process a single UTF-8 byte.
 * @param decoder Decoder state
 * @param b Input byte
 * @return >0: Valid codepoint ready
 *         -1: Incomplete sequence (awaiting more bytes)
 *          0: Invalid/overlong sequence (flushed)
 */
int Utf8Decoder_next(Utf8Decoder *decoder, uint8_t b);

/* ──────────────────────────────────────────────────────────────
 * 4. Dynamic Hash Array
 * ────────────────────────────────────────────────────────────── */

/**
 * @brief Dynamic array of WordHash values.
 */
typedef struct
{
  WordHash *words;
  int length;
  int capacity;
} WordHashes;

/** @brief Initialize an empty WordHashes array. */
void WordHashes_init(WordHashes *arr);

/** @brief Free internal memory and reset the array. */
void WordHashes_finalize(WordHashes *arr);

/** @brief Clear all entries without freeing memory. */
void WordHashes_clear(WordHashes *arr);

/** @brief Append a hash to the array, growing capacity as needed. */
void WordHashes_append(WordHashes *arr, WordHash hash);

/* ──────────────────────────────────────────────────────────────
 * 5. MuPDF Integration & Page Iteration
 * ────────────────────────────────────────────────────────────── */

/**
 * @brief Extract all word hashes from a MuPDF structured text page.
 * Handles whitespace boundaries, semantic units (CJK/symbols), and hyphens.
 * @param arr Destination array to append hashes to
 * @param page MuPDF structured text page
 */
void WordHashes_append_page(WordHashes *arr, fz_stext_page *page);

/**
 * @brief Map a linear word index to its bounding coordinates on a page.
 * @param page MuPDF structured text page
 * @param index Zero-based word index
 * @param first Output: origin of the first character
 * @param last Output: origin of the last character
 * @return true if index is valid, false otherwise
 */
bool WordHash_page_index_to_coord(fz_stext_page *page,
                                  int index,
                                  fz_point *first,
                                  fz_point *last);

/**
 * @brief Find the closest word index to a given coordinate on a page.
 * @param page MuPDF structured text page
 * @param point Target coordinate
 * @param hit Output: closest character origin found
 * @return Zero-based index of the closest word, or -1 if page is empty
 */
int WordHash_page_coord_to_index(fz_stext_page *page,
                                 fz_point point,
                                 fz_point *hit);

#endif  // SYNCTEX_LCS_H_
