#include "prefixsum.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief Recompute all internal prefix sums after leaf values change.
 *
 * This function traverses the binary tree from the second-to-last level
 * up to the root, recalculating each internal node as the sum of its children.
 *
 * Algorithm:
 * - Start at pow-1 (second-to-last level)
 * - For each level, process all nodes from left to right
 * - Each internal node at index i gets value = sums[2*i] + sums[2*i+1]
 * - Stop when reaching the root at index 1
 *
 * Time complexity: O(n) where n = 2^pow (number of leaves)
 *
 * @param ps Pointer to the PrefixSum structure.
 */
static void recompute_prefix(PrefixSum *restrict ps)
{
  float *data = ps->sums;
  size_t pow = ps->pow;

  // Traverse from second-to-last level up to root
  while (pow > 0) {
    pow -= 1;
    size_t count = 1 << pow;

    // Process all nodes at this level
    for (size_t i = count; i < 2 * count; i++)
      data[i] = data[2 * i] + data[2 * i + 1];
  }
}

/**
 * @brief Initialize a PrefixSum structure.
 *
 * Creates a new prefix sum structure with capacity for 2 elements (pow=0).
 * The sums array is allocated with size 2^(pow+1) = 2 elements.
 * All values are implicitly 0.0 until explicitly set.
 *
 * @param ps Pointer to the PrefixSum structure to initialize.
 */
void psum_init(PrefixSum *ps)
{
  ps->pow = 0;
  ps->sums = calloc(2, sizeof(float));
  if (!ps->sums)
    abort();
}

/**
 * @brief Cleanup and free a PrefixSum structure.
 *
 * Frees the internal sums array and resets the structure to a clean state.
 * The structure can be re-initialized after finalization.
 *
 * @param ps Pointer to the PrefixSum structure to finalize (may be NULL).
 */
void psum_finalize(PrefixSum *ps)
{
  free(ps->sums);
  ps->sums = NULL;
}

/**
 * @brief Get the value at a specific index.
 *
 * The index must be less than 2^pow (the current capacity).
 * Out-of-bounds indices return 0.0 (implicit value for unset elements).
 *
 * @param ps Pointer to the PrefixSum structure.
 * @param index Zero-based index to read.
 * @return Value at index, or 0.0 if index is out of bounds.
 */
float psum_get(const PrefixSum *restrict ps, size_t index)
{
  size_t count = (1 << ps->pow);
  if (index >= count)
    return 0.0;
  return ps->sums[count + index];
}

/**
 * @brief Set the value at a specific index.
 *
 * Updates the element at the given index and propagates the change up
 * the binary tree to maintain correct prefix sums.
 *
 * If the index is beyond the current capacity, the structure grows:
 * - Doubles capacity exponentially until index fits
 * - Copies old values to new array
 * - Sets the new value
 * - Recomputes all prefix sums
 *
 * Special case: If setting index beyond capacity with value=0.0,
 * this is a no-op (would be wasted work).
 *
 * Time complexity: O(log n) for update, O(n) for full recomputation when growing.
 *
 * @param ps Pointer to the PrefixSum structure.
 * @param index Zero-based index where the value is stored.
 * @param value The floating-point value to store.
 * @return true if the value changed, false if it remained the same.
 */
bool psum_set(PrefixSum *restrict ps, size_t index, float value)
{
  // Check if we need to grow
  if (index >= (1 << ps->pow))
  {
    if (value == 0.0)
      return false;  // No-op: growing for zero value is wasteful

    // Find minimum capacity needed
    size_t pow = ps->pow + 1;
    while ((1 << pow) <= index)
        pow += 1;

    // Allocate new array (size = 2^(pow+1))
    float *data = calloc((1 << pow) * 2, sizeof(float));
    if (data == NULL)
      abort();

    // Copy old values to new array (shifted by new capacity)
    memcpy(data + (1 << pow), ps->sums + (1 << ps->pow),
           sizeof(float) * (1 << ps->pow));

    // Set the new value
    data[(1 << pow) + index] = value;

    // Replace old array
    free(ps->sums);
    ps->pow = pow;
    ps->sums = data;

    // Recompute all internal nodes
    recompute_prefix(ps);
    return true;
  }

  // Index is within current capacity
  // Map to leaf position in binary tree
  size_t leaf_index = index + (1 << ps->pow);

  // Check if value actually changed
  if (ps->sums[leaf_index] == value)
    return false;

  // Update value and propagate up the tree
  // This is O(log n): traverse from leaf to root
  while (1)
  {
    ps->sums[leaf_index] = value;

    // Add sibling's value to get parent's new sum
    value += ps->sums[leaf_index ^ 1];

    // Move to parent
    leaf_index >>= 1;

    if (leaf_index == 0)  // Reached root
      return true;
  }
}

/**
 * @brief Reset elements beyond a given count to zero.
 *
 * This function sets all elements at indices >= count to 0.0,
 * effectively truncating the useful data without reducing capacity.
 *
 * Algorithm:
 * - If current capacity <= count: already sufficient, return
 * - If current capacity is same power of 2 as count: clear tail elements
 * - If we need to reduce capacity: reallocate to smaller size
 *
 * When clearing tail elements (same power of 2), it uses psum_set() which
 * will also update internal nodes, but since we're setting to 0.0, this
 * is efficient (no tree updates needed if value stays 0).
 *
 * When reducing capacity, it allocates a new smaller array and copies
 * only the first count elements.
 *
 * @param ps Pointer to the PrefixSum structure.
 * @param count New logical size (elements at indices >= count are set to 0).
 */
void psum_truncate(PrefixSum *ps, size_t count)
{
  size_t current_cap = 1 << ps->pow;

  // Already have less than count elements
  if (current_cap <= count)
    return;

  // Same power of 2: just clear the tail
  if (1 << (ps->pow - 1) <= count)
  {
    for (size_t i = count; i < current_cap; i++)
      psum_set(ps, i, 0.0);
    return;
  }

  // Need to reduce capacity: reallocate to smaller size
  size_t new_pow = 0;
  while ((1 << new_pow) < count)
      new_pow += 1;

  float *data = calloc ((1 << new_pow) * 2, sizeof(float));
  if (data == NULL)
    abort();

  memcpy(data + (1 << new_pow), ps->sums + (1 << ps->pow),
         sizeof(float) * count);

  free(ps->sums);
  ps->sums = data;
  ps->pow = new_pow;
  recompute_prefix(ps);
}

/**
 * @brief Query the prefix sum up to (but excluding) a given index.
 *
 * Computes the sum of all elements from index 0 through (index-1).
 *
 * This uses a binary tree traversal:
 * - Start at the leaf node corresponding to index
 * - Traverse up to the root
 * - When moving from a left child to parent, add the right sibling's value
 * - When moving from a right child to parent, don't add anything
 *
 * The result is the sum of all elements to the left of the query position.
 *
 * Special cases:
 * - query(0) = 0.0 (no elements before index 0)
 * - query(i) for i >= capacity returns total sum of all stored elements
 *
 * @param ps Pointer to the PrefixSum structure.
 * @param index Upper bound (exclusive) for the sum.
 * @return Sum of elements [0, index), or 0.0 if index is 0.
 */
float psum_query(PrefixSum *ps, size_t index)
{
  // Query beyond capacity returns total sum (root value)
  size_t capacity = 1 << ps->pow;
  if (index >= capacity)
    return ps->sums[1];

  // Start at leaf node for index
  size_t leaf_index = index + capacity;
  double sum = 0.0;

  // Traverse up to root, collecting from right siblings
  while (leaf_index > 0)
  {
    // If leaf_index is odd, it's a right child; add left sibling
    if (leaf_index & 1)
      sum += ps->sums[leaf_index ^ 1];  // XOR with 1 gave left sibling
    leaf_index >>= 1;  // Move to parent
  }

  return sum;
}

/**
 * @brief Find the lowest index i where prefix_sum(i) >= target.
 *
 * This performs a binary search on the prefix sum tree to find the smallest
 * index i such that psum_query(i+1) >= sum. Equivalently, it finds the
 * smallest i where:
 *
 *   sum(0..i) >= target
 *
 * Algorithm (binary search on tree):
 * - Start at root (index 1)
 * - At each node, check if target fits in left subtree
 * - If left subtree sum >= target, go left
 * - Otherwise, subtract left sum and go right
 * - When reaching a leaf, return the leaf index
 *
 * This runs in O(log n) time by traversing the height of the tree.
 *
 * Examples:
 * - If elements = [3, 5, 2], prefix_sums = [0, 3, 8, 10]
 * - reverse_query(0) = 0 (sum(0..0)=3 >= 0)
 * - reverse_query(3) = 1 (sum(0..1)=8 >= 3)
 * - reverse_query(8) = 2 (sum(0..2)=10 >= 8)
 *
 * @param ps Pointer to the PrefixSum structure.
 * @param sum Target cumulative sum to search for.
 * @return Lowest index i satisfying sum(0..i) >= sum.
 */
size_t psum_reverse_query(PrefixSum *ps, float sum)
{
  // Early return for edge cases
  if (sum <= 0.0)
    return 0;

  size_t index = 1;           // Start at root
  size_t count = 1 << ps->pow;  // Number of leaves

  // Binary search down the tree
  while (index < count && sum > 0.0)
  {
    index *= 2;  // Go to left child
    float left = ps->sums[index];  // Sum of left subtree

    if (sum > left)
    {
      sum -= left;  // Target exceeds left subtree, account for it
      index += 1;   // Go to right child
    }
  }

  return index - count;  // Convert tree index to leaf index
}

/**
 * @brief Find the lowest index with a linear offset applied.
 *
 * This variant of reverse_query finds the smallest index i such that:
 *
 *   sum(0..i) + i * sep >= target
 *
 * The linear offset (sep * i) accounts for non-uniform spacing. For example,
 * if each page is separated by `sep` units, this finds which page contains
 * a given scroll position.
 *
 * Algorithm adjustment from base reverse_query:
 * - When considering a node at level with 'level' leaves under it,
 *   the offset contribution is sep * (level - 1) or approximately sep * level
 * - Subtract the adjusted sum (tree_sum + sep * level) from target
 * - Proceed as in base reverse_query
 *
 * The key insight is that as we traverse down, the offset decreases because
 * we're considering fewer remaining indices.
 *
 * @param ps Pointer to the PrefixSum structure.
 * @param sum Target cumulative sum (including offset).
 * @param sep Linear offset factor applied per index unit.
 * @return Lowest index i where sum(0..i) + i*sep >= sum.
 */
size_t psum_reverse_query_with_offset(PrefixSum *ps, float sum, float sep)
{
  // Early return for edge cases
  if (sum <= 0.0)
    return 0;

  size_t index = 1;           // Start at root
  size_t count = 1 << ps->pow;  // Number of leaves
  size_t level = count;         // Number of leaves under current node
  sum += sep;                   // Adjust target to account for offset formula

  // Binary search with offset adjustment
  while (index < count && sum > 0.0)
  {
    index *= 2;           // Go to left child
    level /= 2;           // Half the leaves under this node
    float left = ps->sums[index] + sep * level;  // Left sum + offset

    if (sum > left)
    {
      sum -= left;        // Target exceeds this subtree
      index += 1;         // Go to right child
    }
  }

  return index - count;  // Convert tree index to leaf index
}

/**
 * @brief Return the total sum of all stored elements.
 *
 * This is equivalent to psum_query(ps, SIZE_MAX) but runs in O(1) time
 * by directly reading the root of the binary tree (at index 1).
 *
 * The root always contains the sum of all leaf values, which is the
 * total of all elements stored in the prefix sum structure.
 *
 * @param ps Pointer to the PrefixSum structure.
 * @return Sum of all elements from index 0 to the last defined index.
 */
float psum_total(PrefixSum *ps)
{
  return ps->sums[1];
}
