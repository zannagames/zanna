//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_list.h
// Purpose: Runtime-backed dynamic list of object references for Zanna.Collections.List, providing
// append, insert, remove, sort, and indexed access with automatic growth.
//
// Key invariants:
//   - Elements are reference-managed: retained on store, released on overwrite or removal.
//   - Indices are 0-based; out-of-bounds access traps at runtime.
//   - rt_list_new returns a new empty list with refcount 1.
//   - Sort operations use a stable algorithm preserving relative order of equal elements.
//
// Ownership/Lifetime:
//   - List owns references to its elements and releases them on removal or destruction.
//   - List lifetime is managed via reference counting; use retain/release to share.
//   - Callers own the initial reference returned by rt_list_new.
//
// Error conventions:
//   - Out-of-bounds index → rt_trap()
//   - Allocation failure → returns NULL
//   - Search not found (Find) → returns -1
//   - Removal not found (Remove) → returns 0
//
// Links: src/runtime/collections/rt_list.c (implementation), src/runtime/core/rt_heap.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares a mutable, retained-element runtime List.
///
/// List provides indexed access, positional insertion and removal, slicing,
/// search, stable sorting, shuffling, and cloning over opaque runtime values.
/// Stored values are retained. Get, first, last, and pop return retained
/// references that callers must release.
///
/// Search and removal use runtime boxed-value equality. List objects and their
/// backing object arrays are runtime-managed and are not safe for
/// unsynchronized concurrent mutation.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Allocate a new List instance (opaque object pointer with vptr at offset 0).
/// @details Provides the concrete runtime backing for Zanna.Collections.List.
///          Allocates internal storage (initial capacity may be > 0) and initializes
///          bookkeeping fields. The returned object is heap-managed and subject to
///          reference counting.
/// @return Opaque pointer to the new List object; NULL on allocation failure.
/// @thread-safety Not thread-safe; caller is responsible for synchronization.
void *rt_list_new(void);

/// @brief Get the number of elements in the list.
/// @details Exposes the List.Len property to the runtime by returning the
///          current logical length stored by the list.
/// @param list Opaque List object pointer.
/// @return Number of elements currently in the list.
int64_t rt_list_len(void *list);

/// @brief Append an element to the end of the list.
/// @details Retains the element (if non-null), grows internal storage
///          geometrically if needed, and writes the element at the end.
///          Supports dynamic growth for collection operations.
/// @param list Opaque List object pointer.
/// @param elem Opaque object element pointer (may be NULL to represent empty slot).
/// @post Count increases by 1 on success.
void rt_list_push(void *list, void *elem);

/// @brief Remove all elements from the list.
/// @details Releases all retained elements and resets the length to zero.
///          The managed backing array is released; a later push allocates
///          fresh storage.
/// @param list Opaque List object pointer.
/// @post Count becomes zero and no backing array remains.
void rt_list_clear(void *list);

/// @brief Remove the element at a specific index.
/// @details Releases the element at the given position and compacts the
///          remaining tail by shifting elements left. Supports positional
///          removal semantics.
/// @param list  Opaque List object pointer.
/// @param index 0-based index; must satisfy 0 <= index < Count.
/// @pre Index must be within bounds; violating may trap at runtime.
void rt_list_remove_at(void *list, int64_t index);

/// @brief Get the element at a specific index (retained).
/// @details Increments the element's refcount before returning to allow
///          safe use of the returned element beyond subsequent list mutations.
/// @param list  Opaque List object pointer.
/// @param index 0-based index; must satisfy 0 <= index < Count.
/// @return The element pointer (may be NULL); caller must release.
void *rt_list_get(void *list, int64_t index);

/// @brief Set the element at a specific index to a new value.
/// @details Retains the new element (if non-null) and releases the previously
///          stored value. Provides indexed update with correct reference management.
/// @param list  Opaque List object pointer.
/// @param index 0-based index; must satisfy 0 <= index < Count.
/// @param elem  Replacement element pointer (may be NULL).
void rt_list_set(void *list, int64_t index, void *elem);

/// @brief Check whether the list contains a specific element.
/// @details Scans the list using runtime boxed-value equality.
/// @param list Opaque List object pointer.
/// @param elem Element to look for (may be NULL).
/// @return 1 if present, 0 otherwise.
int8_t rt_list_has(void *list, void *elem);

/// @brief Find the first index of an element in the list.
/// @details Scans the list left-to-right using runtime boxed-value equality.
///          Enables search and removal patterns without manual iteration.
/// @param list Opaque List object pointer.
/// @param elem Element to look for (may be NULL).
/// @return Index of the first matching element, or -1 when not found.
int64_t rt_list_find(void *list, void *elem);

/// @brief Find the first index of an element as an Option index.
/// @details Returns `SomeI64(index)` when the element is present and `None`
///          when it is absent or @p list is NULL.
/// @param list Opaque List object pointer, or NULL.
/// @param elem Element to look for (may be NULL).
/// @return Opaque Zanna.Option containing the first index, or None.
void *rt_list_find_option(void *list, void *elem);

/// @brief Insert an element at a specific index, shifting elements right.
/// @details Grows the backing storage by one and shifts elements at and after
///          the index to the right. Retains the inserted value. Supports
///          positional insertion for dynamic list operations.
/// @param list  Opaque List object pointer.
/// @param index 0-based insert position; must satisfy 0 <= index <= Count.
/// @param elem  Element to insert (may be NULL).
/// @pre Index must be within bounds; violating traps at runtime.
void rt_list_insert(void *list, int64_t index, void *elem);

/// @brief Remove the first occurrence of an element from the list.
/// @details Searches for the element using reference equality and removes it
///          when found. Provides a common removal helper with boolean success
///          reporting.
/// @param list Opaque List object pointer.
/// @param elem Element to remove (may be NULL).
/// @return 1 when an element was removed, 0 otherwise.
int8_t rt_list_remove(void *list, void *elem);

/// @brief Create a new List containing elements from a range.
/// @details Creates a new List and copies elements in the specified range.
///          Supports sub-list extraction for common list operations.
/// @param list  Opaque List object pointer.
/// @param start 0-based start index (inclusive, clamped to 0).
/// @param end   0-based end index (exclusive, clamped to Count).
/// @return New List containing the slice; empty List if range is invalid.
void *rt_list_slice(void *list, int64_t start, int64_t end);

/// @brief Reverse the order of elements in the list in place.
/// @details Swaps elements from both ends toward the center. Supports common
///          list transformation without creating a new list.
/// @param list Opaque List object pointer.
void rt_list_reverse(void *list);

/// @brief Get the first element in the list.
/// @details Convenience method for the common head/first access pattern.
///          Returns a retained reference to the element at index 0.
/// @param list Opaque List object pointer.
/// @return Retained first element, or NULL if the list is empty.
void *rt_list_first(void *list);

/// @brief Get the last element in the list.
/// @details Convenience method for the common tail/last access pattern.
///          Returns a retained reference to the element at index Count-1.
/// @param list Opaque List object pointer.
/// @return Retained last element, or NULL if the list is empty.
void *rt_list_last(void *list);

/// @brief Check whether the list is empty.
/// @param list Opaque List object pointer.
/// @return 1 if empty (or NULL), 0 otherwise.
int8_t rt_list_is_empty(void *list);

/// @brief Remove and return the last element from the list.
/// @param list Opaque List object pointer. Must not be NULL.
/// @return Removed element as a retained caller-owned reference; traps if empty.
void *rt_list_pop(void *list);

/// @brief Sort the list in ascending order (default comparison).
/// @param list Opaque List object pointer.
/// @note Uses a stable merge sort and the shared total runtime order.
void rt_list_sort(void *list);

/// @brief Sort the list in descending order.
/// @param list Opaque List object pointer.
/// @note Uses a stable merge sort.
void rt_list_sort_desc(void *list);

/// @brief Randomly shuffle the list in place (Fisher-Yates).
/// @param list Opaque List object pointer.
/// @note Uses the active runtime random-number generator.
void rt_list_shuffle(void *list);

/// @brief Create a shallow copy of the list.
/// @param list Source List, or NULL.
/// @return New independently mutable List retaining the same element pointers;
///         NULL source produces an empty List.
void *rt_list_clone(void *list);

#ifdef __cplusplus
}
#endif
