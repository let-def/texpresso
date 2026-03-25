#ifndef PSUM_H_
#define PSUM_H_

#include <stdlib.h>
#include <stdbool.h>

typedef struct psum_s psum_t;

/**
 * @brief Create a new prefix sum data structure.
 * @return A pointer to a newly allocated psum_t structure, or NULL on failure.
 * @note The caller is responsible for freeing the returned pointer using psum_free().
 */
psum_t *psum_new(void);

/**
 * @brief Destroy and free a prefix sum data structure.
 * @param psum Pointer to the psum_t structure to be freed (may be NULL).
 * @note After calling psum_free(), the pointer becomes invalid and must not be used.
 */
void psum_free(psum_t *psum);

/**
 * @brief Reset (to 0.0) all elements at indices higher than count.
 * @param psum Pointer to the psum_t structure.
 * @param count The new size (number of elements) to retain.
 */
void psum_truncate(psum_t *psum, size_t count);

/**
 * @brief Set the value at a specific index.
 * @param psum Pointer to the psum_t structure.
 * @param index The index (0-based) at which to set the value.
 * @param value The floating-point value to store.
 * @return Return true if the cell value has changed.
 * @note Updates the underlying element and refreshes the prefix sum structure as needed.
 * @note The structure is resized if necessary to store the element.
 */
bool psum_set(psum_t *psum, size_t index, float value);

float psum_get(const psum_t *restrict psum, size_t index);

/**
 * @brief Query the prefix sum up to (but excluding) a given index.
 * @param psum Pointer to the psum_t structure.
 * @param index The index (0-based) up to which to compute the sum.
 * @return The sum of all elements from index 0 through 'index-1'.
 * @note Returns 0.0f if 'index' is 0.
 * @note Any index can be queried; indices beyond the current data size are treated as 0.0.
 */
float psum_query(psum_t *psum, size_t index);

/**
 * @brief Find the lowest index `i` such that `psum_query(i) < sum <= psum_query(i + 1)`.
 * @param psum Pointer to the psum_t structure.
 * @param sum The target cumulative sum.
 * @return The lowest index satisfying the condition.
 * @note This function assumes all stored values are non-negative.
 * @note If `sum` is negative or zero, or if no such index exists (e.g., sum > total), 
 *       returns 0.
 */
size_t psum_reverse_query(psum_t *psum, float sum);

/**
 * @brief Find the lowest index `i` such that:
 *        `psum_query(i) + (i * sep) < sum <= psum_query(i + 1) + (i * offset)`.
 * 
 * @brief This variant of `psum_reverse_query` incorporates a linear offset (slope) 
 *        into the comparison. It finds the first index where the prefix sum plus 
 *        the index-dependent offset exceeds the target sum.
 * @param psum Pointer to the psum_t structure.
 * @param sum The target cumulative sum (including the offset).
 * @param offset The linear offset factor (slope) applied per index unit.
 * @return The lowest index satisfying the condition.
 * @note Assumes stored values are non-negative.
 * @note If `sum` is negative or no such index exists, returns 0.
 */
size_t psum_reverse_query_with_offset(psum_t *psum, float sum, float offset);

/**
 * @brief Return the total sum of all elements currently in the structure.
 * @param psum Pointer to the psum_t structure.
 * @return The sum of all elements from index 0 to the last defined index.
 * @note Equivalent to calling `psum_query(psum, SIZE_MAX)`.
 */
float psum_total(psum_t *psum);

#endif // PSUM_H_
