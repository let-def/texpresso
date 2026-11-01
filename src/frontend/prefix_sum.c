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

#include <stdlib.h>
#include <string.h>
#include "prefix_sum.h"
#include "myabort.h"

void txp_prefix_sum_init(txp_prefix_sum *ps)
{
  ps->cap = 1;
  ps->sums = calloc(2, sizeof(float));
  if (!ps->sums)
    myabort();
}

void txp_prefix_sum_finalize(txp_prefix_sum *ps)
{
  free(ps->sums);
  ps->sums = NULL;
  ps->cap = 0;
}

static void recompute_nodes(txp_prefix_sum *ps)
{
  for (int i = ps->cap - 1; i > 0; i--)
    ps->sums[i] = ps->sums[2 * i] + ps->sums[2 * i + 1];
}

void txp_prefix_sum_set(txp_prefix_sum *ps, int index, float value)
{
  if (index < 0)
    myabort();

  if (index >= ps->cap)
  {
    if (value == 0)
      return;
    int cap = ps->cap;
    while (cap <= index)
      cap *= 2;
    float *sums = calloc(2 * cap, sizeof(float));
    if (!sums)
      myabort();
    memcpy(sums + cap, ps->sums + ps->cap, sizeof(float) * ps->cap);
    free(ps->sums);
    ps->sums = sums;
    ps->cap = cap;
    ps->sums[cap + index] = value;
    recompute_nodes(ps);
    return;
  }

  int node = ps->cap + index;
  if (ps->sums[node] == value)
    return;
  ps->sums[node] = value;
  // Recompute rather than propagate a delta: no rounding errors accumulate
  for (node /= 2; node > 0; node /= 2)
    ps->sums[node] = ps->sums[2 * node] + ps->sums[2 * node + 1];
}

float txp_prefix_sum_get(const txp_prefix_sum *ps, int index)
{
  if (index < 0 || index >= ps->cap)
    return 0;
  return ps->sums[ps->cap + index];
}

void txp_prefix_sum_truncate(txp_prefix_sum *ps, int index)
{
  if (index < 0)
    index = 0;
  if (index >= ps->cap)
    return;
  memset(ps->sums + ps->cap + index, 0, sizeof(float) * (ps->cap - index));
  recompute_nodes(ps);
}

float txp_prefix_sum_query(const txp_prefix_sum *ps, int index, float sep)
{
  if (index <= 0)
    return 0;

  float extra = sep * index;
  if (index >= ps->cap)
    return ps->sums[1] + extra;

  // Add the left siblings on the path from the leaf to the root
  float sum = 0;
  for (int node = ps->cap + index; node > 1; node /= 2)
    if (node & 1)
      sum += ps->sums[node - 1];
  return sum + extra;
}

int txp_prefix_sum_find(const txp_prefix_sum *ps, int count, float sum, float sep)
{
  if (count <= 0 || sum <= 0)
    return 0;

  // Past the leaves, the values are 0
  if (count > ps->cap)
  {
    float total = ps->sums[1] + sep * ps->cap;
    if (sum >= total)
    {
      if (sep <= 0)
        return count - 1;
      float index = ps->cap + (sum - total) / sep;
      return index >= count - 1 ? count - 1 : (int)index;
    }
  }

  // Descend from the root, keeping track of the first leaf under the node.
  // The weight of a subtree is the sum of its values plus one separator for
  // each of its leaves before count.
  int node = 1, first = 0, width = ps->cap;
  while (node < ps->cap)
  {
    width /= 2;
    int left = 2 * node;
    int seps = count - first;
    if (seps > width) seps = width;
    if (seps < 0) seps = 0;
    float weight = ps->sums[left] + sep * seps;
    if (sum >= weight)
    {
      sum -= weight;
      node = left + 1;
      first += width;
    }
    else
      node = left;
  }

  int index = node - ps->cap;
  if (index >= count)
    index = count - 1;
  return index;
}
