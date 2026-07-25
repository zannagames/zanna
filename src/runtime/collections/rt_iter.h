//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_iter.h
// Purpose: Unified stateful iterator protocol for all collection types, providing a common
// next/has_next interface over Seq, List, Deque, Ring, Map keys/values, Set, and Stack.
//
// Key invariants:
//   - Iterators are lightweight handles wrapping a collection reference and a position.
//   - Live Seq/List/Ring sources must not be structurally modified during iteration.
//   - Deque/Map/Set/Stack iterators own snapshots unaffected by source mutation.
//   - rt_iter_has_next returns 1 while elements remain; 0 when exhausted.
//
// Ownership/Lifetime:
//   - Iterator objects are runtime-managed; callers do not free them directly.
//   - Iterators retain the live source collection (or own a snapshot Seq)
//     and release it when the iterator is collected.
//
// Links: src/runtime/collections/rt_iter.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares a unified stateful iterator protocol for collections.
///
/// Seq, List, and Ring factories create live indexed iterators with cached
/// lengths. Deque, Map, Set, and Stack factories instead capture snapshots.
/// Structural mutation of a live source during traversal is unsupported,
/// whereas later changes to a snapshotted source are never observed.
///
/// Next and peek return retained references owned by the caller. Because NULL
/// also signals exhaustion, use `rt_iter_has_next()` when stored NULL elements
/// must be distinguished. Iterator objects are runtime-managed opaque handles.

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create an iterator over a Seq.
/// @param seq Non-null Seq retained by the iterator.
/// @return New iterator with the current Seq length cached, or NULL.
void *rt_iter_from_seq(void *seq);

/// @brief Create an iterator over a List.
/// @param list Non-null List retained by the iterator.
/// @return New iterator with the current List length cached, or NULL.
void *rt_iter_from_list(void *list);

/// @brief Create an iterator over a Deque.
/// @param deque Non-null Deque snapshotted front to back.
/// @return New snapshot iterator, or NULL on failure.
void *rt_iter_from_deque(void *deque);

/// @brief Create an iterator over a Map's keys.
/// @param map Non-null Map to snapshot.
/// @return New snapshot iterator producing retained key strings in insertion order.
void *rt_iter_from_map_keys(void *map);

/// @brief Create an iterator over a Map's values.
/// @param map Non-null Map to snapshot.
/// @return New snapshot iterator producing retained values in insertion order.
void *rt_iter_from_map_values(void *map);

/// @brief Create an iterator over a Set's items.
/// @param set Non-null Set to snapshot.
/// @return New snapshot iterator in unspecified Set enumeration order.
void *rt_iter_from_set(void *set);

/// @brief Create a snapshot iterator from a Stack (bottom to top).
/// @param stack Non-null Stack to snapshot without modifying it.
/// @return New snapshot iterator, or NULL on failure.
void *rt_iter_from_stack(void *stack);

/// @brief Create an iterator from a Ring buffer.
/// @param ring Non-null Ring retained by the iterator.
/// @return New iterator with the current Ring length cached, or NULL.
void *rt_iter_from_ring(void *ring);

/// @brief Check if more elements are available.
/// @param iter Iterator handle, or NULL.
/// @return 1 if a cached position remains; otherwise 0.
int8_t rt_iter_has_next(void *iter);

/// @brief Get the current element and advance the iterator.
/// @param iter Iterator handle, or NULL.
/// @return Current element as a retained caller-owned reference, or NULL if
///         exhausted; advances one position on success.
void *rt_iter_next(void *iter);

/// @brief Peek at the current element without advancing.
/// @param iter Iterator handle, or NULL.
/// @return Current element as a retained caller-owned reference, or NULL if
///         exhausted; does not advance.
void *rt_iter_peek(void *iter);

/// @brief Reset the iterator to the beginning.
/// @param iter Iterator handle, or NULL for a no-op.
void rt_iter_reset(void *iter);

/// @brief Get the current zero-based position.
/// @param iter Iterator handle, or NULL.
/// @return Index of the next element, starting at zero and reaching count at
///         exhaustion; zero for NULL.
int64_t rt_iter_index(void *iter);

/// @brief Get the total number of elements in the underlying collection.
/// @param iter Iterator handle, or NULL.
/// @return Length cached at construction, or zero for NULL.
int64_t rt_iter_count(void *iter);

/// @brief Collect remaining elements into a new Seq.
/// @param iter Iterator handle, or NULL.
/// @return New owning Seq containing remaining elements; drains @p iter to its
///         cached end. NULL produces an empty Seq.
void *rt_iter_to_seq(void *iter);

/// @brief Skip N elements.
/// @param iter Iterator handle, or NULL.
/// @param n Maximum positions to skip; non-positive values do nothing.
/// @return Number of elements actually skipped.
int64_t rt_iter_skip(void *iter, int64_t n);

#ifdef __cplusplus
}
#endif
