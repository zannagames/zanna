//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_sparsearray.h
// Purpose: Memory-efficient sparse array mapping arbitrary int64_t indices to object pointer
// values, allocating storage only for non-null entries via a hash map.
//
// Key invariants:
//   - Indices are arbitrary signed 64-bit integers; no range restriction.
//   - Only non-null entries consume memory.
//   - Setting a NULL value removes the entry for that index.
//   - All operations are O(1) average-case via internal hash map.
//   - rt_sparse_get returns NULL for indices not explicitly set.
//
// Ownership/Lifetime:
//   - SparseArray objects are runtime/GC-managed.
//   - Stored values are retained while in the array.
//   - Get returns a borrowed value; enumeration results independently retain
//     their boxed indices or stored values.
//
// Links: src/runtime/collections/rt_sparsearray.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for a sparse signed-integer-indexed value array.
/// @details Non-null values occupy an open-addressed table and are retained by
///          the collection. Every signed 64-bit index is valid. Null is the
///          deletion sentinel and therefore cannot be stored as a present
///          value.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty sparse array.
/// @return New runtime-managed SparseArray.
void *rt_sparse_new(void);

/// @brief Get number of non-null entries.
/// @param obj SparseArray handle, or `NULL`.
/// @return Occupied entry count, or 0 for `NULL`.
int64_t rt_sparse_len(void *obj);

/// @brief Get value at index, or NULL if not set.
/// @details Creates no retain; the result is borrowed from the SparseArray.
/// @param obj SparseArray handle, or `NULL`.
/// @param index Arbitrary integer index.
/// @return Borrowed value at index, or `NULL` when absent.
void *rt_sparse_get(void *obj, int64_t index);

/// @brief Set value at index, or remove the entry when value is NULL.
/// @details Retains a non-null replacement before releasing the old value.
/// @param obj SparseArray handle, or `NULL` for a no-op.
/// @param index Arbitrary integer index.
/// @param value Value to retain, or `NULL` to remove.
void rt_sparse_set(void *obj, int64_t index, void *value);

/// @brief Check if index has a value.
/// @param obj SparseArray handle, or `NULL`.
/// @param index Index to check.
/// @return 1 if set, 0 otherwise.
int8_t rt_sparse_has(void *obj, int64_t index);

/// @brief Remove value at index.
/// @details Releases the stored value and repairs the following probe cluster.
/// @param obj SparseArray handle, or `NULL`.
/// @param index Index to remove.
/// @return 1 if removed, 0 if not found.
int8_t rt_sparse_remove(void *obj, int64_t index);

/// @brief Get all indices that have values.
/// @details Order follows physical slots and may change after mutation.
/// @param obj SparseArray handle, or `NULL`.
/// @return New runtime-managed owning Seq of boxed i64 indices.
void *rt_sparse_indices(void *obj);

/// @brief Get all values.
/// @details The owning snapshot independently retains values in the same
///          current slot order used by @ref rt_sparse_indices.
/// @param obj SparseArray handle, or `NULL`.
/// @return New runtime-managed owning Seq of values.
void *rt_sparse_values(void *obj);

/// @brief Remove all entries.
/// @details Releases values and preserves table capacity for reuse.
/// @param obj SparseArray handle, or `NULL`.
void rt_sparse_clear(void *obj);

#ifdef __cplusplus
}
#endif
