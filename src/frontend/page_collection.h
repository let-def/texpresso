/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef PAGE_COLLECTION_H_
#define PAGE_COLLECTION_H_

#include <stdbool.h>
#include <mupdf/fitz.h>
#include "prefix_sum.h"

// The pages of the document being displayed, and their vertical layout.
//
// Pages [0, valid) are up-to-date with the engine output. Pages [valid,
// count) are stale (the engine backtracked past them) or placeholders, blank
// pages appended ahead of the output while the document is still being
// processed, so that the layout does not shrink and grow back during an
// edit and the viewer can scroll past the last page produced.
//
// With cropping, the displayed area of a page is its content bounding box.
// The heights of the pages (their displayed area) are kept in a prefix sum.

typedef struct txp_page_s txp_page;

typedef struct
{
  txp_page *pages;
  int count, valid, cap;
  int grow1, grow2;
  txp_prefix_sum heights;
  bool crop;
  // Union of the horizontal content bounds of the pages, used to crop all
  // pages of a continuous layout to the same columns
  float content_x0, content_x1;
} txp_page_collection;

void txp_page_collection_init(txp_page_collection *pc);
void txp_page_collection_finalize(fz_context *ctx, txp_page_collection *pc);

int txp_page_collection_count(const txp_page_collection *pc);
int txp_page_collection_valid_count(const txp_page_collection *pc);

// Display list of a page, or NULL for a placeholder (or out of bounds)
fz_display_list *txp_page_collection_get(const txp_page_collection *pc, int index);

// The page has been produced by the engine, it might be stale
// (false for placeholders)
bool txp_page_collection_has_contents(const txp_page_collection *pc, int index);

// Text of a page, extracted on demand, or NULL
fz_stext_page *txp_page_collection_stext(fz_context *ctx, txp_page_collection *pc, int index);

// Store the page at index, which becomes valid as well as all the pages
// before it. dl can be NULL (a rendering error): the page is blank.
void txp_page_collection_set(fz_context *ctx, txp_page_collection *pc,
                             int index, fz_display_list *dl);

// Mark the pages after count as stale
void txp_page_collection_invalidate_after(txp_page_collection *pc, int count);

// Drop the stale pages and placeholders (the document is complete)
void txp_page_collection_truncate(fz_context *ctx, txp_page_collection *pc);

// Area of a page displayed, in document coordinates. With columns, a cropped
// page spans the content bounds of all pages horizontally.
fz_rect txp_page_collection_bounds(fz_context *ctx, txp_page_collection *pc,
                                   int index, bool columns);

bool txp_page_collection_get_crop(const txp_page_collection *pc);
void txp_page_collection_set_crop(fz_context *ctx, txp_page_collection *pc, bool crop);

// Vertical layout: pages stacked from 0 with sep between each of them

// Top of a page
float txp_page_collection_top(const txp_page_collection *pc, int index, float sep);

// Page at vertical offset y, the separator below a page belonging to it.
// Clamped to [0, count), -1 if there are no pages.
int txp_page_collection_find(const txp_page_collection *pc, float y, float sep);

// Height of the layout
float txp_page_collection_height(const txp_page_collection *pc, float sep);

#endif // PAGE_COLLECTION_H_
