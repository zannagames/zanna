//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_ring.h
// Purpose: Fixed-capacity circular buffer (Ring) with FIFO ordering that overwrites the oldest
// entry when full, useful for rolling logs and event histories.
//
// Key invariants:
//   - Fixed capacity set at creation; cannot be resized.
//   - When full, push overwrites the oldest element (no trap).
//   - Peek returns the oldest (front) element without removing it.
//   - Elements are retained when pushed and released when overwritten, popped,
//     cleared, or finalized while ownership mode is enabled.
//
// Ownership/Lifetime:
//   - Ring objects are heap-allocated and runtime-managed.
//   - Popped elements are returned with a caller-owned reference in ownership mode.
//
// Error conventions:
//   - Allocation failure → trap with a NULL fallback return
//   - Pop on empty → returns NULL
//   - Membership (Has) → returns 1 (found) or 0 (not found)
//
// Links: src/runtime/collections/rt_ring.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for a fixed-capacity circular Ring collection.
/// @details The Ring preserves logical oldest-to-newest order while reusing a
///          fixed native allocation. A push to a full Ring discards the oldest
///          value. Retained ownership is enabled by default and can be changed
///          only while empty.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new ring buffer with specified capacity.
/// @param capacity Maximum number of elements. Zero maps to 1; negative traps.
/// @return New runtime-managed owning Ring, or `NULL` after allocation failure.
void *rt_ring_new(int64_t capacity);

/// @brief Create a new ring buffer with default capacity (16).
/// @return New runtime-managed owning Ring.
void *rt_ring_new_default(void);

/// @brief Get number of elements in ring.
/// @param obj Ring handle, or `NULL`.
/// @return Current live-element count, or 0 for `NULL`.
int64_t rt_ring_len(void *obj);

/// @brief Get capacity of ring.
/// @param obj Ring handle, or `NULL`.
/// @return Fixed maximum capacity, or 0 for `NULL`.
int64_t rt_ring_cap(void *obj);

/// @brief Check if ring is empty.
/// @param obj Ring handle, or `NULL`.
/// @return 1 if empty, otherwise 0; `NULL` is empty.
int8_t rt_ring_is_empty(void *obj);

/// @brief Check if ring is full.
/// @param obj Ring handle, or `NULL`.
/// @return 1 when length equals capacity, otherwise 0.
int8_t rt_ring_is_full(void *obj);

/// @brief Set whether the ring retains/releases stored runtime objects.
/// @details The default for new rings is enabled. Ownership mode can only be
/// changed while the ring is empty.
/// @param obj Empty Ring; null or invalid handles trap.
/// @param owns Nonzero to enable ownership, zero to store borrowed pointers.
void rt_ring_set_owns_elements(void *obj, int8_t owns);

/// @brief Return whether the ring owns stored elements.
/// @param obj Ring handle, or `NULL`.
/// @return 1 when owning mode is enabled, otherwise 0.
int8_t rt_ring_owns_elements(void *obj);

/// @brief Push element to ring (overwrites oldest if full).
/// @details Owning Rings retain @p elem and release any overwritten value;
///          borrowing Rings store/discard raw pointers. A null Ring is a no-op.
/// @param obj Ring handle, or `NULL`.
/// @param elem Element to push; may be `NULL`.
void rt_ring_push(void *obj, void *elem);

/// @brief Pop oldest element from ring.
/// @details Owning Rings return a caller-retained value; borrowing Rings return
///          the raw pointer. A stored null is ambiguous with absence.
/// @param obj Ring handle, or `NULL`.
/// @return Oldest element, or `NULL` if empty/null/null-valued.
void *rt_ring_pop(void *obj);

/// @brief Peek at oldest element without removing.
/// @details Creates no retain; the result is borrowed from the Ring/producer.
/// @param obj Ring handle, or `NULL`.
/// @return Borrowed oldest value, or `NULL` if empty/null/null-valued.
void *rt_ring_peek(void *obj);

/// @brief Get element by logical index (0 = oldest).
/// @details Creates no retain; the result is borrowed from the Ring/producer.
/// @param obj Ring handle, or `NULL`.
/// @param index Logical index (0 to len-1).
/// @return Borrowed element, or `NULL` if out of bounds/null-valued.
void *rt_ring_get(void *obj, int64_t index);

/// @brief Remove all elements from ring.
/// @details Releases live values in owning mode, clears slots, and preserves
///          capacity/ownership mode. A null Ring is a no-op.
/// @param obj Ring handle, or `NULL`.
void rt_ring_clear(void *obj);

/// @brief Check if the ring contains an equal element.
/// @details Uses boxed-value equality for boxed numeric/string values and
///          identity for ordinary runtime objects.
/// @param obj Ring handle, or `NULL`.
/// @param elem Element to search for; may be `NULL`.
/// @return 1 if found, 0 otherwise.
int8_t rt_ring_has(void *obj, void *elem);

/// @brief Get the oldest element (same as peek).
/// @param obj Ring handle, or `NULL`.
/// @return Borrowed oldest value, or `NULL` if empty/null/null-valued.
void *rt_ring_first(void *obj);

/// @brief Get the newest element.
/// @param obj Ring handle, or `NULL`.
/// @return Borrowed newest value, or `NULL` if empty/null/null-valued.
void *rt_ring_last(void *obj);

/// @brief Reverse the elements in the ring in place.
/// @details Reverses logical order without changing capacity, head, count,
///          ownership mode, or retain counts. A null Ring is a no-op.
/// @param obj Ring handle, or `NULL`.
void rt_ring_reverse(void *obj);

/// @brief Create a shallow copy of the ring.
/// @details Preserves capacity, order, and ownership mode; owning clones
///          independently retain values. A null source yields a minimum-size
///          empty owning Ring.
/// @param obj Source Ring handle, or `NULL`.
/// @return New runtime-managed Ring.
void *rt_ring_clone(void *obj);

#ifdef __cplusplus
}
#endif
