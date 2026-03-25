#include "psum.h"
#include <stdlib.h>
#include <string.h>

//#define DEBUG_REFERENCE

#ifdef DEBUG_REFERENCE
#include <stdio.h>
#include <assert.h>
#include <math.h>
#endif

struct psum_s
{
  // Capacity, as a power of two.
  // The structure store values for (1 << pow) elements.
  size_t pow;

  // Partial sums, encoded as perfect binary-tree in an array of (1 << pow) elements.
  float *sums;
};

static void psum_init(psum_t *psum)
{
  psum->pow = 0;
  psum->sums = calloc(2, sizeof(float));
  if (!psum->sums)
    abort();
}

psum_t *psum_new(void)
{
  psum_t *psum = malloc(sizeof(struct psum_s));
  if (!psum)
    abort();
  psum_init(psum);
  return psum;
}

void psum_free(psum_t *psum)
{
  if (!psum)
    return;
  free(psum->sums);
  free(psum);
}

static void recompute_prefix(psum_t *restrict psum)
{
  float *data = psum->sums;
  size_t pow = psum->pow;
  while (pow > 0) {
    pow -= 1;
    size_t count = 1 << pow;

    for (size_t i = count; i < 2 * count; i++)
      data[i] = data[2 * i] + data[2 * i + 1];
  }
}

float psum_get(const psum_t *restrict psum, size_t index)
{
  size_t count = (1 << psum->pow);
  if (index >= count)
    return 0.0;
  return psum->sums[count + index];
}

bool psum_set(psum_t *restrict psum, size_t index, float value)
{
  if (index >= (1 << psum->pow))
  {
    if (value == 0.0)
      return false;

    // Grow array
    size_t pow = psum->pow + 1;
    while ((1 << pow) <= index)
        pow += 1;

    float *data = calloc((1 << pow) * 2, sizeof(float));
    if (data == NULL)
      abort();

    memcpy(data + (1 << pow), psum->sums + (1 << psum->pow),
           sizeof(float) * (1 << psum->pow));
    data[(1 << pow) + index] = value;
    free(psum->sums);
    psum->pow = pow;
    psum->sums = data;

    recompute_prefix(psum);
    return true;
  }

  index += (1 << psum->pow);

  if (psum->sums[index] == value)
    return false;

  while (1)
  {
    psum->sums[index] = value;
    value += psum->sums[index ^ 1];
    index >>= 1;
    if (index == 0)
      return true;
  }
}

void psum_truncate(psum_t *psum, size_t count)
{
  if ((1 << psum->pow) <= count)
  {
    // We already have less than count elements
    return;
  }

  if (1 << (psum->pow - 1) <= count)
  {
    // Same power of 2: clear suffix
    for (size_t i = count, end = 1 << psum->pow; i < end; i++)
      psum_set(psum, i, 0.0);
    return;
  }

  // Reallocate
  size_t pow = 0;
  while (1 << pow < count)
      pow += 1;

  float *data = calloc ((1 << pow) * 2, sizeof(float));
  memcpy(data + (1 << pow), psum->sums + (1 << psum->pow), sizeof(float) * count);
  free(psum->sums);
  psum->sums = data;
  psum->pow = pow;
  recompute_prefix(psum);
  return;
}

float psum_query(psum_t *psum, size_t index)
{
  // Index out of bounds
  if (index >= (1 << psum->pow))
    return psum->sums[1];

  size_t i = index + (1 << psum->pow);
  double sum = 0.0;
  while (i > 0)
  {
    if (i & 1)
      sum += psum->sums[i ^ 1];
    i >>= 1;
  }

#ifdef DEBUG_REFERENCE
  {
    double reference = 0.0;
    for (size_t i = (1 << psum->pow), j = (1 << psum->pow) + index0; i < j; i++)
        reference += psum->sums[i];
    if (fabs(sum - reference) > 0.1)
        fprintf(stderr, "index: %d, sum: %f, reference: %f\n", (int)index0, sum, reference);
    assert(fabs(sum - reference) <= 0.1);
  }
#endif

  return sum;
}

size_t psum_reverse_query(psum_t *psum, float sum)
{
  if (sum <= 0.0)
    return 0;
  size_t index = 1;
  size_t count = 1 << psum->pow;
  while (index < count && sum > 0.0)
  {
    index *= 2;
    float left = psum->sums[index];
    if (sum > left)
    {
      sum -= left;
      index += 1;
    }
  }
  return index - count;
}

size_t psum_reverse_query_with_offset(psum_t *psum, float sum, float sep)
{
  if (sum <= 0.0)
    return 0;
  size_t index = 1;
  size_t count = 1 << psum->pow;
  size_t level = count;
  sum += sep;

  while (index < count && sum > 0.0)
  {
    index *= 2;
    level /= 2;
    float left = psum->sums[index] + sep * level;
    if (sum > left)
    {
      sum -= left;
      index += 1;
    }
  }
  return index - count;
}

float psum_total(psum_t *psum)
{
  return psum->sums[1];
}
