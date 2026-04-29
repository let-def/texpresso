#ifndef PREFIXSUM_H_
#define PREFIXSUM_H_

#include <stdlib.h>
#include <stdbool.h>

/**
 * @brief Prefix sum data structure using a perfect binary tree representation.
 *
 * This structure maintains a dynamic array of floating-point values and efficiently
 * computes prefix sums (sum of elements from index 0 to i-1). It uses a complete
 * binary tree stored in an array where:
 * - Root is at index 1
 * - Children of node i are at indices 2*i and 2*i+1
 * - Leaf values are stored at indices [capacity, 2*capacity)
 * - Internal nodes store sums of their subtrees
 *
 * The capacity is always a power of two. The structure can grow dynamically
 * to accommodate new elements, but never shrinks (except via truncate).
 *
 * Operations:
 * - Set: O(log n) - updates element and propagates changes up the tree
 * - Query: O(log n) - traverses from leaf to root collecting sum
 * - Reverse query: O(log n) - binary search for target sum
 * - Total: O(1) - returns root value directly
 *
 * Use cases:
 * - Efficient cumulative height calculation for scroll positioning
 * - Page lookup by vertical offset
 * - Dynamic array with fast prefix sum queries
 */
typedef struct {
  size_t pow;             ///< Capacity exponent: array holds 2^pow elements
  float *sums;            ///< Binary tree array of size 2^(pow+1)
} PrefixSum;

/**
 * @brief Initialize a PrefixSum structure.
 *
 * Creates a fresh prefix sum structure with initial capacity of 2 elements.
 * All values are implicitly 0.0 until explicitly set.
 *
 * @param ps Pointer to the PrefixSum structure to initialize.
 */
void psum_init(PrefixSum *ps);

/**
 * @brief Cleanup and free a PrefixSum structure.
 *
 * Frees the internal sums array and resets the structure to a clean state.
 * The structure can be re-initialized after finalization.
 *
 * @param ps Pointer to the PrefixSum structure to finalize (may be NULL).
 */
void psum_finalize(PrefixSum *ps);

/**
 * @brief Reset elements beyond a given count to zero.
 *
 * Sets all elements at indices >= count to 0.0, effectively truncating
 * the useful data without reducing the underlying capacity.
 *
 * If the current capacity already holds <= count elements, this is a no-op.
 * If the capacity is much larger than count, it clears the tail of the array
 * without reallocating.
 *
 * @param ps Pointer to the PrefixSum structure.
 * @param count New logical size (elements at indices >= count are set to 0).
 */
void psum_truncate(PrefixSum *ps, size_t count);

/**
 * @brief Set the value at a specific index.
 *
 * Updates the element at the given index and propagates the change up
 * the binary tree to maintain correct prefix sums.
 *
 * If the index is beyond the current capacity, the structure grows by
 * doubling the capacity until it can accommodate the index.
 *
 * @param ps Pointer to the PrefixSum structure.
 * @param index Zero-based index where the value is stored.
 * @param value The floating-point value to store.
 * @return true if the value changed, false if it remained the same.
 */
bool psum_set(PrefixSum *ps, size_t index, float value);

/**
 * @brief Get the value at a specific index.
 *
 * @param ps Pointer to the PrefixSum structure.
 * @param index Zero-based index to read.
 * @return Value at index, or 0.0 if index is out of bounds.
 */
float psum_get(const PrefixSum *restrict ps, size_t index);

/**
 * @brief Query the prefix sum up to (but excluding) a given index.
 *
 * Computes the sum of all elements from index 0 through (index-1).
 *
 * Special cases:
 * - query(0) = 0.0 (no elements before index 0)
 * - query(i) for i >= capacity returns total sum of all stored elements
 *
 * Algorithm: Start at leaf node for index, traverse up to root,
 * collecting values from left siblings when moving right.
 *
 * @param ps Pointer to the PrefixSum structure.
 * @param index Upper bound (exclusive) for the sum.
 * @return Sum of elements [0, index), or 0.0 if index is 0.
 */
float psum_query(PrefixSum *ps, size_t index);

/**
 * @brief Find the lowest index i where prefix_sum(i) >= target.
 *
 * Performs a binary search to find the smallest index i such that
 * psum_query(i+1) >= sum, which equivalently means:
 * psum_query(i) < sum <= psum_query(i+1)
 *
 * This is useful for finding which element contains a given cumulative value.
 *
 * Examples:
 * - If elements = [3, 5, 2], prefix_sums = [0, 3, 8, 10]
 * - reverse_query(0) = 0 (prefix_sum(1)=3 > 0)
 * - reverse_query(3) = 1 (prefix_sum(2)=8 > 3)
 * - reverse_query(8) = 2 (prefix_sum(3)=10 > 8)
 * - reverse_query(100) = 2 (max index, no element has sum > 100)
 *
 * @param ps Pointer to the PrefixSum structure.
 * @param sum Target cumulative sum to search for.
 * @return Lowest index i satisfying psum_query(i+1) >= sum.
 */
size_t psum_reverse_query(PrefixSum *ps, float sum);

/**
 * @brief Find the lowest index with a linear offset applied.
 *
 * This variant of reverse_query incorporates a linear offset (slope) into
 * the comparison. It finds the smallest index i such that:
 *
 *   psum_query(i+1) + i * sep >= sum
 *
 * This is equivalent to finding where the cumulative sum plus a linearly
 * increasing offset exceeds the target. The offset grows as i * sep.
 *
 * Use cases:
 * - Finding page containing a scroll position with spacing
 * - Adjusting for non-uniform element spacing
 * - Search in transformed coordinate space
 *
 * Algorithm: Similar to reverse_query but accounts for the linear term
 * by subtracting sep from the remaining sum at each step.
 *
 * @param ps Pointer to the PrefixSum structure.
 * @param sum Target cumulative sum (including offset).
 * @param sep Linear offset factor applied per index unit.
 * @return Lowest index i where psum_query(i+1) + i*sep >= sum.
 */
size_t psum_reverse_query_with_offset(PrefixSum *ps, float sum, float sep);

/**
 * @brief Return the total sum of all stored elements.
 *
 * This is equivalent to psum_query(ps, SIZE_MAX) but runs in O(1) time
 * by directly reading the root of the binary tree.
 *
 * @param ps Pointer to the PrefixSum structure.
 * @return Sum of all elements from index 0 to the last defined index.
 */
float psum_total(PrefixSum *ps);

#endif
