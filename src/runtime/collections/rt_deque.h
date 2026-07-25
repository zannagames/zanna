//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_deque.h
// Purpose: Double-ended queue (deque) providing O(1) push/pop at both front and back with O(1)
// random access by index, implemented as a ring buffer.
//
// Key invariants:
//   - Indices are 0-based with front at index 0.
//   - Pop and peek operations on an empty deque trap immediately.
//   - Capacity is always >= 1; the ring buffer doubles on overflow.
//   - Element equality for rt_deque_has uses runtime boxed-value equality.
//
// Ownership/Lifetime:
//   - Deque objects are GC-managed; elements are retained on insertion and released on removal.
//   - Callers should not free deque objects directly.
//
// Error conventions:
//   - Out-of-bounds index → rt_trap()
//   - Allocation failure → returns NULL
//   - Pop/Peek on empty → rt_trap()
//   - TryPopFront/TryPopBack on empty → returns NULL
//
// Links: src/runtime/collections/rt_deque.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares retained-element double-ended queue operations.
///
/// Deque exposes a circular buffer in logical front-to-back order. Pushes and
/// pops at either end are constant-time amortized, indexed access is
/// constant-time, and capacity grows without changing logical order.
///
/// Inserted values are retained. Successful pop, peek, get, and non-Option
/// try-pop operations return retained references owned by the caller. Option
/// try-pop variants preserve the distinction between an empty deque and a
/// stored NULL. Deque objects are runtime-managed opaque handles.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Deque Creation
//=========================================================================

/// @brief Create a new empty deque with default capacity.
/// @return New runtime-managed Deque, or NULL after an allocation trap.
void *rt_deque_new(void);

/// @brief Create a new empty deque with specified initial capacity.
/// @param cap Initial capacity; values below one are normalized to one.
/// @return New runtime-managed Deque, or NULL after a size or allocation trap.
void *rt_deque_with_capacity(int64_t cap);

//=========================================================================
// Size Operations
//=========================================================================

/// @brief Get the number of elements in the deque.
/// @param obj Deque handle, or NULL.
/// @return Number of elements, or zero for NULL.
int64_t rt_deque_len(void *obj);

/// @brief Get the current capacity of the deque.
/// @param obj Deque handle, or NULL.
/// @return Current capacity, or zero for NULL.
int64_t rt_deque_cap(void *obj);

/// @brief Check if the deque is empty.
/// @param obj Deque handle, or NULL.
/// @return 1 if empty or NULL; otherwise 0.
int8_t rt_deque_is_empty(void *obj);

//=========================================================================
// Front Operations
//=========================================================================

/// @brief Add an element to the front of the deque.
/// @param obj Deque handle, or NULL for a no-op.
/// @param elem Element to retain and add; may be NULL.
void rt_deque_push_front(void *obj, void *elem);

/// @brief Remove and return the element at the front.
/// @param obj Non-null Deque handle.
/// @return Removed element as a retained caller-owned reference; traps if empty.
void *rt_deque_pop_front(void *obj);

/// @brief Get the element at the front without removing it.
/// @param obj Non-null Deque handle.
/// @return Front element as a retained caller-owned reference; traps if empty.
void *rt_deque_peek_front(void *obj);

//=========================================================================
// Back Operations
//=========================================================================

/// @brief Add an element to the back of the deque.
/// @param obj Deque handle, or NULL for a no-op.
/// @param elem Element to retain and add; may be NULL.
void rt_deque_push_back(void *obj, void *elem);

/// @brief Remove and return the element at the back.
/// @param obj Non-null Deque handle.
/// @return Removed element as a retained caller-owned reference; traps if empty.
void *rt_deque_pop_back(void *obj);

/// @brief Get the element at the back without removing it.
/// @param obj Non-null Deque handle.
/// @return Back element as a retained caller-owned reference; traps if empty.
void *rt_deque_peek_back(void *obj);

//=========================================================================
// Random Access
//=========================================================================

/// @brief Get the element at the specified index.
/// @param obj Non-null Deque handle.
/// @param idx Index of element to retrieve (0 is front).
/// @return Element as a retained caller-owned reference; traps if out of bounds.
void *rt_deque_get(void *obj, int64_t idx);

/// @brief Set the element at the specified index.
/// @param obj Non-null Deque handle.
/// @param idx Index of element to set.
/// @param val Value to retain and store; may be NULL.
void rt_deque_set(void *obj, int64_t idx, void *val);

//=========================================================================
// Utility
//=========================================================================

/// @brief Remove all elements from the deque.
/// @param obj Deque handle, or NULL for a no-op.
/// @note Capacity is retained.
void rt_deque_clear(void *obj);

/// @brief Check if the deque contains an element.
/// @param obj Deque handle, or NULL.
/// @param elem Element compared using runtime boxed-value equality.
/// @return 1 if found, 0 otherwise.
int8_t rt_deque_has(void *obj, void *elem);

/// @brief Reverse the elements in the deque in place.
/// @param obj Deque handle, or NULL for a no-op.
void rt_deque_reverse(void *obj);

/// @brief Pop the front element, or return NULL if empty (no trap).
/// @param obj Deque handle, or NULL.
/// @return Removed element as a retained caller-owned reference, or NULL if
///         empty. A stored NULL is ambiguous.
void *rt_deque_try_pop_front(void *obj);

/// @brief Pop the front element as an Option.
/// @details Returns `None` when the deque is empty and `Some(value)` when an
///          element is removed. A stored NULL value is represented as
///          `Some(NULL)`, unlike @ref rt_deque_try_pop_front.
/// @param obj Deque handle, or NULL.
/// @return Opaque Zanna.Option object.
void *rt_deque_try_pop_front_option(void *obj);

/// @brief Pop the back element, or return NULL if empty (no trap).
/// @param obj Deque handle, or NULL.
/// @return Removed element as a retained caller-owned reference, or NULL if
///         empty. A stored NULL is ambiguous.
void *rt_deque_try_pop_back(void *obj);

/// @brief Pop the back element as an Option.
/// @details Returns `None` when the deque is empty and `Some(value)` when an
///          element is removed. A stored NULL value is represented as
///          `Some(NULL)`, unlike @ref rt_deque_try_pop_back.
/// @param obj Deque handle, or NULL.
/// @return Opaque Zanna.Option object.
void *rt_deque_try_pop_back_option(void *obj);

/// @brief Create a shallow copy of the deque.
/// @param obj Source Deque handle, or NULL.
/// @return New Deque retaining the same elements in logical order; NULL source
///         produces an empty Deque.
void *rt_deque_clone(void *obj);

#ifdef __cplusplus
}
#endif
