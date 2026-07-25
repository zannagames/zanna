//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_convert_coll.h
// Purpose: Collection conversion utilities for transforming between Seq, List, Set, Map, Stack,
// Queue, Deque, Bag, and Ring container types, plus variadic constructors for Seq, List, and Set.
//
// Key invariants:
//   - Conversions produce new independent collections; source collections are not modified.
//   - Element ordering follows each source type's iteration order.
//   - Set conversions deduplicate by pointer equality.
//   - Variadic constructors take a count followed by that many object pointers.
//
// Ownership/Lifetime:
//   - All returned collections are newly allocated and owned by the caller.
//   - Source collections are not consumed or modified by conversion.
//   - Elements are retained in the destination collection.
//
// Links: src/runtime/collections/rt_convert_coll.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares conversions among runtime collection representations.
///
/// Each conversion returns a newly allocated destination and leaves its source
/// unchanged. Ordered sources preserve their natural traversal order; Set,
/// Map, and Bag conversions preserve only those types' unspecified enumeration
/// order. Elements are shared according to destination retain/release rules,
/// except that Bag operations copy string values.
///
/// A null source is treated as an empty source and still produces an allocated
/// destination. The variadic constructors consume exactly the requested number
/// of object-pointer arguments when that number is positive.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Seq Conversions
//=============================================================================

/// @brief Convert a Seq to a List.
/// @param seq Source Seq, or NULL.
/// @return New List with elements in index order.
void *rt_seq_to_list(void *seq);

/// @brief Convert a Seq to a Set.
/// @param seq Source Seq, or NULL.
/// @return New Set with duplicates collapsed by Set equality semantics.
void *rt_seq_to_set(void *seq);

/// @brief Convert a Seq to a Stack (top = last element).
/// @param seq Source Seq, or NULL.
/// @return New owning Stack with the first element at the bottom.
void *rt_seq_to_stack(void *seq);

/// @brief Convert a Seq to a Queue (front = first element).
/// @param seq Source Seq, or NULL.
/// @return New owning Queue with the first element at the front.
void *rt_seq_to_queue(void *seq);

/// @brief Convert a Seq to a Deque.
/// @param seq Source Seq, or NULL.
/// @return New Deque ordered front to back by Seq index.
void *rt_seq_to_deque(void *seq);

/// @brief Convert a Seq to a Bag (string set).
/// @param seq Source Seq, or NULL. Elements may be raw runtime strings or boxed strings;
///            other element types trap.
/// @return New Bag with unique strings.
void *rt_seq_to_bag(void *seq);

//=============================================================================
// List Conversions
//=============================================================================

/// @brief Convert a List to a Seq.
/// @param list Source List, or NULL.
/// @return New owning Seq with elements in List index order.
void *rt_list_to_seq(void *list);

/// @brief Convert a List to a Set.
/// @param list Source List, or NULL.
/// @return New Set with duplicates collapsed by Set equality semantics.
void *rt_list_to_set(void *list);

/// @brief Convert a List to a Stack.
/// @param list Source List, or NULL.
/// @return New owning Stack with the first List element at the bottom.
void *rt_list_to_stack(void *list);

/// @brief Convert a List to a Queue.
/// @param list Source List, or NULL.
/// @return New owning Queue with the first List element at the front.
void *rt_list_to_queue(void *list);

//=============================================================================
// Set Conversions
//=============================================================================

/// @brief Convert a Set to a Seq.
/// @param set Source Set, or NULL.
/// @return New owning Seq in unspecified Set enumeration order.
void *rt_set_to_seq(void *set);

/// @brief Convert a Set to a List.
/// @param set Source Set, or NULL.
/// @return New List in unspecified Set enumeration order.
void *rt_set_to_list(void *set);

//=============================================================================
// Stack Conversions
//=============================================================================

/// @brief Convert a Stack to a Seq (order: bottom to top).
/// @param stack Source Stack, or NULL.
/// @return New owning Seq ordered from bottom to top.
void *rt_stack_to_seq(void *stack);

/// @brief Convert a Stack to a List.
/// @param stack Source Stack, or NULL.
/// @return New List ordered from bottom to top.
void *rt_stack_to_list(void *stack);

//=============================================================================
// Queue Conversions
//=============================================================================

/// @brief Convert a Queue to a Seq (order: front to back).
/// @param queue Source Queue, or NULL.
/// @return New owning Seq ordered from front to back.
void *rt_queue_to_seq(void *queue);

/// @brief Convert a Queue to a List.
/// @param queue Source Queue, or NULL.
/// @return New List ordered from front to back.
void *rt_queue_to_list(void *queue);

//=============================================================================
// Deque Conversions
//=============================================================================

/// @brief Convert a Deque to a Seq (order: front to back).
/// @param deque Source Deque, or NULL.
/// @return New owning Seq ordered from front to back.
void *rt_deque_to_seq(void *deque);

/// @brief Convert a Deque to a List.
/// @param deque Source Deque, or NULL.
/// @return New List ordered from front to back.
void *rt_deque_to_list(void *deque);

//=============================================================================
// Map Conversions
//=============================================================================

/// @brief Get all keys from a Map as a Seq.
/// @param map Source Map, or NULL.
/// @return New owning Seq with all keys in Map enumeration order.
void *rt_map_keys_to_seq(void *map);

/// @brief Get all values from a Map as a Seq.
/// @param map Source Map, or NULL.
/// @return New owning Seq with values in Map enumeration order.
void *rt_map_values_to_seq(void *map);

//=============================================================================
// Bag Conversions
//=============================================================================

/// @brief Convert a unique-string Bag to a Seq.
/// @param bag Source Bag, or NULL.
/// @return New owning Seq containing one copied string per Bag element in
///         unspecified order.
void *rt_bag_to_seq(void *bag);

/// @brief Convert a Bag to a Set (unique elements only, counts discarded).
/// @param bag Source Bag, or NULL.
/// @return New Set containing boxed copies of the Bag's unique strings.
void *rt_bag_to_set(void *bag);

//=============================================================================
// Ring Conversions
//=============================================================================

/// @brief Convert a Ring to a Seq.
/// @param ring Source Ring, or NULL.
/// @return New owning Seq in logical Ring index order.
void *rt_ring_to_seq(void *ring);

//=============================================================================
// Utility Functions
//=============================================================================

/// @brief Create a Seq from a variable number of elements.
/// @param count Number of following object pointers; non-positive values
///        produce an empty Seq.
/// @param ... Exactly @p count elements in desired order.
/// @return New Seq containing the supplied pointers.
void *rt_seq_of(int64_t count, ...);

/// @brief Create a List from a variable number of elements.
/// @param count Number of following object pointers; non-positive values
///        produce an empty List.
/// @param ... Exactly @p count elements in desired order.
/// @return New List containing the supplied pointers.
void *rt_list_of(int64_t count, ...);

/// @brief Create a Set from a variable number of elements.
/// @param count Number of following object pointers; non-positive values
///        produce an empty Set.
/// @param ... Exactly @p count elements to insert.
/// @return New Set with duplicates collapsed by Set equality semantics.
void *rt_set_of(int64_t count, ...);

#ifdef __cplusplus
}
#endif
