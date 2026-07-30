//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_seq.h
// Purpose: Runtime-backed dynamic sequence for Zanna.Collections.Seq, providing O(1) amortized
// append, O(1) indexed access, and functional operations (map, filter, reduce).
//
// Key invariants:
//   - Indices are 0-based; out-of-bounds access traps at runtime.
//   - Append is amortized O(1) with capacity doubling.
//   - Default sequences borrow elements; ownership can be enabled before inserting elements.
//   - Functional operations (map, filter) return new Seq objects.
//
// Ownership/Lifetime:
//   - Seq objects are runtime/GC-managed opaque pointers.
//   - Borrowing accessors return raw pointers. Owning removals return
//     caller-retained values; borrowing removals do not extend lifetime.
//
// Error conventions:
//   - Out-of-bounds index → rt_trap()
//   - Allocation failure → trap with a NULL fallback return
//
// Links: src/runtime/collections/rt_seq.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for the runtime growable Seq collection.
/// @details Seq stores an indexed dynamic array of runtime value pointers and
///          supports either borrowed or retained-element ownership. The core
///          ABI covers allocation/mutation/search, while sorting and
///          callback-driven functional operations are implemented by the Seq
///          support translation units behind this same interface.

#pragma once

#include <stdint.h>

#include "rt_collection_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new empty sequence with default capacity.
/// @details Creates a borrowing Seq with sixteen reserved slots.
/// @return New runtime-managed Seq.
void *rt_seq_new(void);

/// @brief Create a new public sequence that owns pushed elements.
/// @return New runtime-managed owning Seq with default capacity.
void *rt_seq_new_owned(void);

/// @brief Create a new public sequence with a fixed initial length.
/// @details The result owns later inserted/replaced values and initializes all
///          exposed slots to `NULL`.
/// @param len Initial length. Slots are initialized to null.
/// @return New runtime-managed owning Seq; negative lengths trap.
void *rt_seq_new_sized(int64_t len);

/// @brief Enable or disable element ownership for an empty Seq.
/// @details When enabled, the Seq retains elements on push/insert/set and
///          releases them on clear/finalize. Ownership mode changes trap once
///          the sequence is non-empty.
/// @param obj Opaque Seq object pointer.
/// @param owns 1 to enable ownership, 0 to disable.
void rt_seq_set_owns_elements(void *obj, int8_t owns);

/// @brief Push an element without retaining it, regardless of ownership mode.
/// @details For internal runtime use. On an owning Seq, this transfers one
///          already-owned reference into the sequence; on a borrowing Seq,
///          element lifetime remains managed externally.
/// @param obj Opaque Seq object pointer.
/// @param val Element to add.
void rt_seq_push_raw(void *obj, void *val);

/// @brief Create a new empty sequence with specified initial capacity.
/// @param cap Initial capacity (minimum 1).
/// @return New runtime-managed borrowing Seq.
void *rt_seq_with_capacity(int64_t cap);

/// @brief Create a new public sequence with specified capacity that owns pushed elements.
/// @param cap Initial capacity (minimum 1).
/// @return New runtime-managed owning Seq.
void *rt_seq_with_capacity_owned(int64_t cap);

/// @brief Get the number of elements in the sequence.
/// @param obj Seq handle, or `NULL`.
/// @return Number of live slots, or 0 for `NULL`.
int64_t rt_seq_len(void *obj);

/// @brief Get the current capacity of the sequence.
/// @param obj Seq handle, or `NULL`.
/// @return Reserved pointer-slot capacity, or 0 for `NULL`.
int64_t rt_seq_cap(void *obj);

/// @brief Check if the sequence is empty.
/// @param obj Seq handle, or `NULL`.
/// @return 1 if empty, otherwise 0; `NULL` is empty.
int8_t rt_seq_is_empty(void *obj);

/// @brief Get the element at the specified index.
/// @details Creates no retain; the result is borrowed from the owning Seq or
///          from the producer of a borrowing Seq.
/// @param obj Non-null Seq handle.
/// @param idx Zero-based element index.
/// @return Borrowed element; null/out-of-range handles and indices trap.
void *rt_seq_get(void *obj, int64_t idx);

/// @brief Get a string element at the specified index from a string sequence.
/// @details Accepts raw string elements from runtime snapshots and boxed strings
///          pushed through the public Object ABI. Returns an owned string.
/// @param obj Opaque Seq object pointer.
/// @param idx Index of element to retrieve.
/// @return Caller-owned string handle; wrong element kinds and invalid indices
///         trap.
struct rt_string_impl *rt_seq_get_str(void *obj, int64_t idx);

/// @brief Set the element at the specified index.
/// @details Owning Seqs retain the replacement before releasing the old value;
///          borrowing Seqs store it raw. Length is unchanged.
/// @param obj Non-null Seq handle.
/// @param idx Index of element to set.
/// @param val Value to store; may be `NULL`.
void rt_seq_set(void *obj, int64_t idx, void *val);

/// @brief Set an element without retaining the new value.
/// @details If the sequence owns elements, the previous element is still
///          released. This is for runtime code that already owns a reference
///          and wants to transfer it into the sequence. Replacing a slot with
///          the identical pointer still consumes the transferred reference.
/// @param obj Opaque Seq object pointer.
/// @param idx Existing zero-based slot index.
/// @param val Value whose current reference is transferred to the sequence.
void rt_seq_set_raw(void *obj, int64_t idx, void *val);

/// @brief Add an element to the end of the sequence.
/// @details Runs in amortized O(1). Owning Seqs retain @p val; borrowing Seqs
///          append it raw.
/// @param obj Non-null Seq handle.
/// @param val Element to add; may be `NULL`.
void rt_seq_push(void *obj, void *val);

/// @brief Append all elements of @p other onto @p obj.
/// @details Preserves element ordering. Self-appends are supported: when @p obj == @p other,
///          the operation doubles the original sequence contents without looping indefinitely.
/// @param obj Opaque Seq object pointer.
/// @param other Opaque Seq object pointer whose elements will be appended (treated as empty
/// when NULL).
void rt_seq_push_all(void *obj, void *other);

/// @brief Remove and return the last element from the sequence.
/// @details Owning Seqs return a caller-retained value; borrowing Seqs return
///          the raw stored pointer. Null/empty Seqs trap.
/// @param obj Non-null Seq handle.
/// @return Removed value, which may be a stored `NULL`.
void *rt_seq_pop(void *obj);

/// @brief Get the last element without removing it.
/// @details Creates no retain.
/// @param obj Non-null Seq handle.
/// @return Borrowed last value; null/empty Seqs trap.
void *rt_seq_peek(void *obj);

/// @brief Get the first element.
/// @details Creates no retain.
/// @param obj Non-null Seq handle.
/// @return Borrowed first value; null/empty Seqs trap.
void *rt_seq_first(void *obj);

/// @brief Get the last element.
/// @details Creates no retain and is equivalent to @ref rt_seq_peek.
/// @param obj Non-null Seq handle.
/// @return Borrowed last value; null/empty Seqs trap.
void *rt_seq_last(void *obj);

/// @brief Insert an element at the specified position.
/// @details Shifts the suffix right in O(n). Owning Seqs retain @p val;
///          borrowing Seqs store it raw.
/// @param obj Non-null Seq handle.
/// @param idx Position to insert at (0 to len inclusive).
/// @param val Element to insert; may be `NULL`.
void rt_seq_insert(void *obj, int64_t idx, void *val);

/// @brief Remove and return the element at the specified position.
/// @details Shifts the suffix left in O(n). Owning Seqs return a
///          caller-retained value; borrowing Seqs return the raw pointer.
/// @param obj Non-null Seq handle.
/// @param idx Position to remove from.
/// @return Removed element; invalid indices trap.
void *rt_seq_remove(void *obj, int64_t idx);

/// @brief Remove all elements from the sequence.
/// @details Releases live values in owning mode, clears all active slots, and
///          preserves capacity/ownership mode. A null Seq is a no-op.
/// @param obj Seq handle, or `NULL`.
void rt_seq_clear(void *obj);

/// @brief Find the index of an element in the sequence.
/// @details Boxed numbers/strings compare by content; ordinary objects compare
///          by identity.
/// @param obj Seq handle, or `NULL`.
/// @param val Element to find; may be `NULL`.
/// @return Index of element, or -1 if not found.
int64_t rt_seq_find(void *obj, void *val);

/// @brief Find the first index of an element as an Option index.
/// @details Returns `SomeI64(index)` when @p val is present and `None` when it
///          is absent or @p obj is NULL.
/// @param obj Opaque Seq object pointer, or NULL.
/// @param val Element compared with boxed-value equality.
/// @return Opaque Zanna.Option containing the first index, or None.
void *rt_seq_find_option(void *obj, void *val);

/// @brief Check if the sequence contains an element.
/// @param obj Seq handle, or `NULL`.
/// @param val Element compared with boxed-value equality.
/// @return 1 if found, 0 otherwise.
int8_t rt_seq_has(void *obj, void *val);

/// @brief Reverse the elements in the sequence in place.
/// @param obj Opaque Seq object pointer.
void rt_seq_reverse(void *obj);

/// @brief Shuffle the elements in the sequence in place.
/// @details Uses an in-place Fisher–Yates shuffle driven by the same deterministic RNG as
///          Zanna.Random.NextInt (so Zanna.Random.Seed influences the result).
/// @param obj Opaque Seq object pointer.
void rt_seq_shuffle(void *obj);

/// @brief Create a new sequence containing elements from [start, end).
/// @details Preserves source ownership mode and independently retains shared
///          values when owning. A null source returns an empty owning Seq.
/// @param obj Source Seq handle, or `NULL`.
/// @param start Start index (inclusive, clamped to 0).
/// @param end End index (exclusive, clamped to len).
/// @return New sequence containing the slice.
void *rt_seq_slice(void *obj, int64_t start, int64_t end);

/// @brief Create a shallow copy of the sequence.
/// @details Preserves source ownership mode and independently retains shared
///          values when owning. A null source returns an empty owning Seq.
/// @param obj Source Seq handle, or `NULL`.
/// @return New runtime-managed Seq with the same element pointers.
void *rt_seq_clone(void *obj);

/// @brief Sort the elements in the sequence in ascending order.
/// @details Uses the shared total boxed-value order so strings and integers
///          sort consistently with other runtime collections. Ordinary object
///          handles fall back to the shared deterministic order. For custom
///          ordering, use SortBy.
/// @param obj Opaque Seq object pointer. If NULL, this is a no-op.
/// @note O(n log n) time complexity.
/// @note Modifies the Seq in place and allocates O(n) temporary merge storage.
/// @note Thread safety: Not thread-safe.
/// @see rt_seq_sort_by For custom comparison functions
void rt_seq_sort(void *obj);

/// @brief Sort the elements using a custom comparison function.
/// @details Uses a stable merge sort. The comparison function should
///          return negative if a < b, zero if equal, positive if a > b.
/// @param obj Opaque Seq object pointer. If NULL, this is a no-op.
/// @param cmp Comparison function receiving two element pointers.
/// @note O(n log n) time complexity.
/// @note Modifies the Seq in place and allocates O(n) temporary merge storage.
/// @note Thread safety: Not thread-safe.
void rt_seq_sort_by(void *obj, int64_t (*cmp)(void *, void *));

/// @brief Sort the elements in descending order.
/// @details Uses the reverse of the shared total boxed-value order directly,
///          preserving stability among elements that compare equal.
/// @param obj Opaque Seq object pointer. If NULL, this is a no-op.
/// @note O(n log n) time complexity.
/// @note Modifies the Seq in place and allocates O(n) temporary merge storage.
/// @note Thread safety: Not thread-safe.
void rt_seq_sort_desc(void *obj);

//=========================================================================
// Functional Operations - Keep, Reject, Apply, All, Any, etc.
//=========================================================================

/// @brief Create a new Seq containing only elements matching a predicate.
/// @details Iterates through the Seq and includes elements for which the
///          predicate function returns non-zero (true).
/// @param obj Opaque Seq object pointer. If NULL, returns empty Seq.
/// @param pred Predicate function that takes an element and returns non-zero
///             to include it. If NULL, returns a clone of the original.
/// @return New Seq containing only matching elements.
/// @note O(n) time complexity.
/// @note Creates a new Seq; original is not modified.
void *rt_seq_keep(void *obj, int8_t (*pred)(void *));

/// @brief Create a new Seq excluding elements matching a predicate.
/// @details Inverse of Keep. Includes elements for which the predicate
///          returns zero (false).
/// @param obj Opaque Seq object pointer. If NULL, returns empty Seq.
/// @param pred Predicate function. If NULL, returns a clone of the original.
/// @return New Seq containing only non-matching elements.
/// @note O(n) time complexity.
void *rt_seq_reject(void *obj, int8_t (*pred)(void *));

/// @brief Create a new Seq by transforming each element with a function.
/// @details Applies the transform function to each element and collects
///          the results into a new Seq.
/// @param obj Opaque Seq object pointer. If NULL, returns empty Seq.
/// @param fn Transform function that takes an element and returns a new
///           element. If NULL, returns a clone of the original.
/// @return New Seq containing transformed elements.
/// @note O(n) time complexity.
void *rt_seq_apply(void *obj, void *(*fn)(void *));

/// @brief Check if all elements satisfy a predicate.
/// @details Returns true if the predicate returns non-zero for every element.
///          Returns true for empty sequences (vacuous truth).
/// @param obj Opaque Seq object pointer. If NULL, returns 1 (true).
/// @param pred Predicate function. If NULL, returns 1 (true).
/// @return 1 if all elements match, 0 otherwise.
/// @note O(n) worst case, but short-circuits on first non-match.
int8_t rt_seq_all(void *obj, int8_t (*pred)(void *));

/// @brief Check if any element satisfies a predicate.
/// @details Returns true if the predicate returns non-zero for at least
///          one element. Returns false for empty sequences.
/// @param obj Opaque Seq object pointer. If NULL, returns 0 (false).
/// @param pred Predicate function. If NULL, returns 0 (false).
/// @return 1 if any element matches, 0 otherwise.
/// @note O(n) worst case, but short-circuits on first match.
int8_t rt_seq_any(void *obj, int8_t (*pred)(void *));

/// @brief Check if no elements satisfy a predicate.
/// @details Returns true if the predicate returns zero for every element.
///          Returns true for empty sequences.
/// @param obj Opaque Seq object pointer. If NULL, returns 1 (true).
/// @param pred Predicate function. If NULL, returns 1 (true).
/// @return 1 if no elements match, 0 otherwise.
/// @note O(n) worst case, but short-circuits on first match.
int8_t rt_seq_none(void *obj, int8_t (*pred)(void *));

/// @brief Count elements that satisfy a predicate.
/// @param obj Opaque Seq object pointer. If NULL, returns 0.
/// @param pred Predicate function. If NULL, returns total length.
/// @return Number of elements for which predicate returns non-zero.
/// @note O(n) time complexity.
int64_t rt_seq_count_where(void *obj, int8_t (*pred)(void *));

/// @brief Find the first element satisfying a predicate.
/// @param obj Opaque Seq object pointer. If NULL, returns NULL.
/// @param pred Predicate function. If NULL, returns first element or NULL.
/// @return First matching element, or NULL if none found.
/// @note O(n) worst case, but short-circuits on first match.
void *rt_seq_find_where(void *obj, int8_t (*pred)(void *));

/// @brief Find the first element satisfying a predicate as an Option.
/// @details Preserves a found NULL element as `Some(NULL)` instead of
///          conflating it with absence.
/// @param obj Opaque Seq object pointer. If NULL, returns None.
/// @param pred Predicate function. If NULL, returns first element as Some or None when empty.
/// @return Opaque Zanna.Option containing the first matching element, or None.
void *rt_seq_find_where_option(void *obj, int8_t (*pred)(void *));

/// @brief Create a new Seq with the first N elements.
/// @param obj Opaque Seq object pointer. If NULL, returns empty Seq.
/// @param n Number of elements to take. Clamped to [0, len].
/// @return New Seq containing at most n elements from the start.
/// @note O(n) time complexity.
void *rt_seq_take(void *obj, int64_t n);

/// @brief Create a new Seq skipping the first N elements.
/// @param obj Opaque Seq object pointer. If NULL, returns empty Seq.
/// @param n Number of elements to skip. Clamped to [0, len].
/// @return New Seq containing elements after the first n.
/// @note O(n) time complexity.
void *rt_seq_drop(void *obj, int64_t n);

/// @brief Create a new Seq with elements taken while predicate is true.
/// @details Takes elements from the start while the predicate returns
///          non-zero. Stops at the first element where predicate is false.
/// @param obj Opaque Seq object pointer. If NULL, returns empty Seq.
/// @param pred Predicate function. If NULL, returns clone.
/// @return New Seq with leading elements matching predicate.
/// @note O(n) time complexity.
void *rt_seq_take_while(void *obj, int8_t (*pred)(void *));

/// @brief Create a new Seq skipping elements while predicate is true.
/// @details Skips elements from the start while the predicate returns
///          non-zero. Includes all elements from the first non-match.
/// @param obj Opaque Seq object pointer. If NULL, returns empty Seq.
/// @param pred Predicate function. If NULL, returns empty Seq.
/// @return New Seq with elements after the leading matching ones.
/// @note O(n) time complexity.
void *rt_seq_drop_while(void *obj, int8_t (*pred)(void *));

/// @brief Reduce the sequence to a single value using an accumulator.
/// @details Applies the reducer function to each element and an accumulator,
///          threading the result through as the new accumulator.
/// @param obj Opaque Seq object pointer. If NULL, returns init.
/// @param init Initial accumulator value.
/// @param fn Reducer function: fn(accumulator, element) -> new accumulator.
///           If NULL, returns init.
/// @return Final accumulated value.
/// @note O(n) time complexity.
void *rt_seq_fold(void *obj, void *init, void *(*fn)(void *, void *));

#ifdef __cplusplus
}
#endif
