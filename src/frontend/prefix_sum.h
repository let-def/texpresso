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

#ifndef PREFIX_SUM_H_
#define PREFIX_SUM_H_

// Sequence of non-negative values (page heights) supporting updates, prefix
// sums and their inverse in O(log n).
//
// The values are the leaves of a complete binary tree stored in an array:
// node i has children 2i and 2i+1, leaves are at [cap, 2 cap) and each inner
// node holds the sum of its leaves.
// A separator can be added after each of the first `count` values when
// querying, to lay out pages with a gap that depends on the zoom level.

typedef struct
{
  int cap;      // number of leaves, a power of two
  float *sums;  // 2 * cap nodes, sums[0] is unused
} txp_prefix_sum;

void txp_prefix_sum_init(txp_prefix_sum *ps);
void txp_prefix_sum_finalize(txp_prefix_sum *ps);

// Set the value at index, growing the sequence as needed
void txp_prefix_sum_set(txp_prefix_sum *ps, int index, float value);
float txp_prefix_sum_get(const txp_prefix_sum *ps, int index);

// Reset values at and after index to 0
void txp_prefix_sum_truncate(txp_prefix_sum *ps, int index);

// Sum of the values before index, plus sep for each of them
float txp_prefix_sum_query(const txp_prefix_sum *ps, int index, float sep);

// Largest index < count such that query(index, sep) <= sum, or 0
int txp_prefix_sum_find(const txp_prefix_sum *ps, int count, float sum, float sep);

#endif // PREFIX_SUM_H_
