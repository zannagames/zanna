//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_objpool.h
/// @file
/// @brief Declares a fixed-capacity pool of reusable integer slot IDs.
//
// Purpose: Fixed-capacity object pool for efficient reuse of integer slot IDs, eliminating
// allocation churn for frequently created and destroyed game objects.
//
// Key invariants:
//   - Pool capacity is fixed at creation and cannot exceed RT_OBJPOOL_MAX.
//   - Slot indices are stable across acquire/release cycles.
//   - rt_objpool_acquire returns -1 when the pool is exhausted.
//   - Released slots are immediately available for reacquisition.
//
// Ownership/Lifetime:
//   - The pool is a runtime reference-counted object. rt_objpool_destroy()
//     releases one explicit handle reference.
//   - Slots are logically owned by the caller while acquired; releasing returns them to the pool.
//
// Links: src/runtime/game/rt_objpool.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Maximum number of slots accepted by rt_objpool_new().
#define RT_OBJPOOL_MAX 4096

/// @brief Runtime class ID used to validate ObjectPool handles.
#define RT_OBJPOOL_CLASS_ID INT64_C(-0x51020E)

/// @brief Opaque handle to a runtime reference-counted ObjectPool instance.
typedef struct rt_objpool_impl *rt_objpool;

/// @brief Create a pool containing a fixed number of reusable slots.
/// @param capacity Requested slot count, clamped to `[1, RT_OBJPOOL_MAX]`.
/// @return A new ObjectPool reference, or `NULL` if allocation fails.
/// @details Slots initially contain zero data and are acquired in ascending
///          index order.
rt_objpool rt_objpool_new(int64_t capacity);

/// @brief Release one runtime reference to an ObjectPool.
/// @param pool Pool reference to release; `NULL` is a no-op.
/// @details Storage is reclaimed when the reference count reaches zero. A
///          non-null object of another runtime class raises a trap.
void rt_objpool_destroy(rt_objpool pool);

/// @brief Acquire the slot at the head of the free list.
/// @param pool Pool from which to acquire.
/// @return An index in `[0, capacity)`, or `-1` if @p pool is null or full.
/// @details Acquisition is O(1), resets the slot's data to zero, and prepends
///          the slot to the active list. A non-null wrong-class handle traps.
int64_t rt_objpool_acquire(rt_objpool pool);

/// @brief Release a slot back to the pool and clear its user data.
/// @param pool Pool that owns @p slot.
/// @param slot Active index to release.
/// @return `1` on success, or `0` for a null pool, out-of-range index, or
///         already-free slot.
/// @details Releasing the active-list head is O(1); other slots require an
///          O(active_count) predecessor scan. The released slot becomes the
///          next acquisition candidate. A non-null wrong-class handle traps.
int8_t rt_objpool_release(rt_objpool pool, int64_t slot);

/// @brief Test whether an index denotes a currently acquired slot.
/// @param pool Pool that owns @p slot.
/// @param slot Candidate index.
/// @return `1` if active, or `0` if free, out of range, or @p pool is null.
/// @details A non-null handle of another runtime class raises a trap.
int8_t rt_objpool_is_active(rt_objpool pool, int64_t slot);

/// @brief Count currently acquired slots.
/// @param pool Pool to query.
/// @return Active slot count, or zero for `NULL`.
/// @details A non-null handle of another runtime class raises a trap.
int64_t rt_objpool_active_count(rt_objpool pool);

/// @brief Count slots currently available for acquisition.
/// @param pool Pool to query.
/// @return `capacity - active_count`, or zero for `NULL`.
/// @details A non-null handle of another runtime class raises a trap.
int64_t rt_objpool_free_count(rt_objpool pool);

/// @brief Read the pool's immutable total capacity.
/// @param pool Pool to query.
/// @return Active-plus-free slot count, or zero for `NULL`.
/// @details The returned capacity is the constructor request after clamping.
///          A non-null wrong-class handle raises a trap.
int64_t rt_objpool_capacity(rt_objpool pool);

/// @brief Test whether every pool slot is active.
/// @param pool Pool to query.
/// @return `1` if full, `0` otherwise. A null pool is reported as full.
/// @details A non-null handle of another runtime class raises a trap.
int8_t rt_objpool_is_full(rt_objpool pool);

/// @brief Test whether every pool slot is free.
/// @param pool Pool to query.
/// @return `1` if empty, `0` otherwise. A null pool is reported as empty.
/// @details A non-null handle of another runtime class raises a trap.
int8_t rt_objpool_is_empty(rt_objpool pool);

/// @brief Release all slots and restore the initial free-list order.
/// @param pool Pool to clear; `NULL` is a no-op.
/// @details All user data is erased, capacity is preserved, and the next
///          acquisition returns index zero. A non-null wrong-class handle
///          raises a trap.
void rt_objpool_clear(rt_objpool pool);

/// @brief Start traversing the intrusive active-slot list.
/// @param pool Pool to traverse.
/// @return The most recently acquired active slot, or `-1` if none.
/// @details This operation is O(1). A null pool returns `-1`; a non-null
///          wrong-class handle raises a trap.
int64_t rt_objpool_first_active(rt_objpool pool);

/// @brief Advance an active-slot traversal in constant time.
/// @param pool Pool being traversed.
/// @param after Current in-range traversal index.
/// @return The next linked active slot, or `-1` at the tail, for an invalid
///         index, or for a null pool.
/// @details Callers should not release or clear traversal slots between steps.
///          A non-null wrong-class handle raises a trap.
int64_t rt_objpool_next_active(rt_objpool pool, int64_t after);

/// @brief Associate one opaque 64-bit value with an active slot.
/// @param pool Pool that owns @p slot.
/// @param slot Active slot index.
/// @param data Value to store.
/// @return `1` on success, or `0` for a null pool, invalid index, or free slot.
/// @details Stored data is reset to zero by acquire, release, and clear. A
///          non-null wrong-class handle raises a trap.
int8_t rt_objpool_set_data(rt_objpool pool, int64_t slot, int64_t data);

/// @brief Retrieve the opaque value associated with an active slot.
/// @param pool Pool that owns @p slot.
/// @param slot Active slot index.
/// @return Stored data, or zero for a null pool, invalid index, or free slot.
/// @details Zero is also valid data; use rt_objpool_is_active() when the
///          sentinel must be disambiguated. A non-null wrong-class handle
///          raises a trap.
int64_t rt_objpool_get_data(rt_objpool pool, int64_t slot);

#ifdef __cplusplus
}
#endif
