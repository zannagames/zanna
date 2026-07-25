//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_bitset.h
// Purpose: Arbitrary-size bit array (BitSet) with 0-based indexing that auto-grows when bits are
// set beyond the current size, supporting population count, set/clear/toggle, and bitwise
// operations.
//
// Key invariants:
//   - Bit indices are 0-based; the bitset auto-grows when setting beyond current size.
//   - All bits start as 0 on allocation.
//   - Bitwise AND/OR/XOR operations return a result sized to the larger operand.
//   - rt_bitset_is_empty tests whether any bits are set.
//
// Ownership/Lifetime:
//   - BitSet objects are GC-managed (rt_obj_new_i64) with a runtime
//     finalizer; callers must not free them directly.
//   - Internal backing array may be reallocated on auto-grow.
//
// Links: src/runtime/collections/rt_bitset.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the runtime API for arbitrary-length packed bit sets.
///
/// Bit indices are zero-based. A BitSet has a logical length independent of
/// its reserved word capacity: set and toggle grow the logical length when
/// necessary, while get and clear never grow it. Whole-set operations return
/// new independent BitSets and do not modify their operands.
///
/// BitSet objects are runtime-managed opaque handles. Invalid non-null handles
/// raise a runtime trap; null and negative-index behavior is documented for
/// each operation below.

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new BitSet with room for at least nbits bits.
/// @param nbits Initial capacity in bits (all bits start as 0). Zero uses the
///              default capacity; negative values trap.
/// @return A new runtime-managed BitSet, or NULL after a size or allocation
///         failure.
void *rt_bitset_new(int64_t nbits);

/// @brief Get the current logical bit length.
/// @param obj BitSet handle, or NULL to query a zero-length set.
/// @return Highest valid logical index plus one, or zero for NULL.
int64_t rt_bitset_len(void *obj);

/// @brief Count the number of set bits.
/// @param obj BitSet handle, or NULL to count an empty set.
/// @return Population count across logical bits.
int64_t rt_bitset_count(void *obj);

/// @brief Check if all bits are zero.
/// @param obj BitSet handle, or NULL to test an empty set.
/// @return 1 if every logical bit is zero; otherwise 0.
int8_t rt_bitset_is_empty(void *obj);

/// @brief Get the value of a bit at the given index.
/// @param obj BitSet handle, or NULL to read an empty set.
/// @param idx 0-based bit index.
/// @return 1 if set, or 0 if clear, negative, out of range, or @p obj is NULL.
int8_t rt_bitset_get(void *obj, int64_t idx);

/// @brief Set a bit to 1 at the given index. Auto-grows if needed.
/// @param obj BitSet handle, or NULL for a no-op.
/// @param idx 0-based bit index; negative values are ignored.
void rt_bitset_set(void *obj, int64_t idx);

/// @brief Clear a bit to 0 at the given index.
/// @param obj BitSet handle, or NULL for a no-op.
/// @param idx 0-based bit index; negative and out-of-range values are ignored.
void rt_bitset_clear(void *obj, int64_t idx);

/// @brief Toggle a bit at the given index. Auto-grows if needed.
/// @param obj BitSet handle, or NULL for a no-op.
/// @param idx 0-based bit index; negative values are ignored.
void rt_bitset_toggle(void *obj, int64_t idx);

/// @brief Set all bits to 1.
/// @param obj BitSet handle, or NULL for a no-op.
/// @note Unused high bits in the final storage word remain zero.
void rt_bitset_set_all(void *obj);

/// @brief Clear all bits to 0.
/// @param obj BitSet handle, or NULL for a no-op.
/// @note Logical length and reserved capacity are preserved.
void rt_bitset_clear_all(void *obj);

/// @brief Bitwise AND of two BitSets. Returns a new BitSet.
/// @param a First BitSet handle.
/// @param b Second BitSet handle.
/// @return New BitSet with size `max(a.len, b.len)`. If either operand is
///         NULL, returns a new empty default-length BitSet.
void *rt_bitset_and(void *a, void *b);

/// @brief Bitwise OR of two BitSets. Returns a new BitSet.
/// @param a First BitSet handle.
/// @param b Second BitSet handle.
/// @return New BitSet with size `max(a.len, b.len)`. If either operand is
///         NULL, returns a new empty default-length BitSet.
void *rt_bitset_or(void *a, void *b);

/// @brief Bitwise XOR of two BitSets. Returns a new BitSet.
/// @param a First BitSet handle.
/// @param b Second BitSet handle.
/// @return New BitSet with size `max(a.len, b.len)`. If either operand is
///         NULL, returns a new empty default-length BitSet.
void *rt_bitset_xor(void *a, void *b);

/// @brief Bitwise NOT of a BitSet. Returns a new BitSet.
/// @param obj Source BitSet handle.
/// @return New same-length BitSet with all logical bits flipped. NULL returns
///         a new empty default-length BitSet.
void *rt_bitset_not(void *obj);

/// @brief Convert the BitSet to a binary string representation.
/// @param obj BitSet handle, or NULL to render an empty set.
/// @return Newly created MSB-first string with leading zeroes suppressed;
///         zero-valued and null sets produce `"0"`.
rt_string rt_bitset_to_string(void *obj);

#ifdef __cplusplus
}
#endif
