//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_objpool.c
/// @file
/// @brief Implements a fixed-capacity integer-slot pool with intrusive free
///        and active lists.
//
// Purpose: Fixed-capacity object pool for Zanna games. Eliminates per-frame
//   allocation churn for frequently created and destroyed game objects such as
//   bullets, enemies, particles, and projectiles. Slots are acquired from an
//   embedded free list in O(1); releasing a non-head active slot scans the
//   active list. Active slots are traversable in O(1) per step using an
//   intrusive singly-linked list maintained by acquire and release.
//
// Key invariants:
//   - The pool owns a single calloc'd slot array of fixed capacity. Capacity is
//     clamped to RT_OBJPOOL_MAX (4096) at creation and never changes.
//   - Slot indices are stable across the pool's lifetime: an acquired slot keeps
//     the same integer index from acquire until release. Games may store extra
//     data per-slot via rt_objpool_set_data / rt_objpool_get_data (one int64).
//   - The free list is a singly-linked chain through pool_slot.next_free.
//     free_head is the index of the next available slot (-1 when pool is full).
//   - The active list is a singly-linked chain through pool_slot.next_active.
//     active_head is the index of the first acquired slot (-1 when empty).
//     rt_objpool_first_active / rt_objpool_next_active run in O(1).
//   - Release is O(active_count) in the worst case because the active list is
//     singly-linked and removing a non-head element requires scanning for the
//     predecessor. For typical game pool sizes (<= 512) this is negligible.
//   - Releasing an already-free slot (double-release) is a safe no-op.
//
// Ownership/Lifetime:
//   - The pool struct is GC-managed (rt_obj_new_i64). The slot array is
//     malloc'd separately; the GC finalizer (objpool_finalizer) frees it when
//     the pool is collected. rt_objpool_destroy() releases one object
//     reference and frees the object when it was the last reference.
//
// Links: src/runtime/game/rt_objpool.h (public API),
//        docs/zannalib/game.md (ObjectPool section)
//
//===----------------------------------------------------------------------===//

#include "rt_objpool.h"
#include "rt_object.h"
#include "rt_trap.h"

#include <stdlib.h>
#include <string.h>

/// @brief Internal record shared by the free and active intrusive lists.
/// @details A slot belongs to exactly one list. Acquiring moves the free-list
///          head to the active-list head; releasing performs the inverse move.
struct pool_slot {
    int64_t data;        ///< User data.
    int64_t next_free;   ///< Next free slot index (-1 if end).
    int64_t next_active; ///< Next active slot index (-1 if tail of active list).
    int8_t active;       ///< 1 if acquired, 0 if free.
};

/// @brief Private state for a fixed-capacity object pool.
/// @details The separately allocated slot array has stable addresses and
///          indices for the lifetime of the pool.
struct rt_objpool_impl {
    struct pool_slot *slots; ///< Slot array.
    int64_t capacity;        ///< Total capacity.
    int64_t active_count;    ///< Number of active slots.
    int64_t free_head;       ///< Head of free list.
    int64_t active_head;     ///< Head of active list (-1 if none). O(1) iteration.
};

/// @brief Validate and cast an opaque ObjectPool handle.
/// @param pool Candidate handle; `NULL` is accepted.
/// @param api Trap message used when a non-null object has the wrong class ID.
/// @return @p pool when valid, or `NULL` when the input is null or invalid.
/// @details A class mismatch raises a runtime trap before returning `NULL`.
static rt_objpool checked_objpool(rt_objpool pool, const char *api) {
    if (!pool)
        return NULL;
    if (rt_obj_class_id(pool) != RT_OBJPOOL_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return pool;
}

/// @brief Release the separately allocated slot array during object teardown.
/// @param obj ObjectPool implementation supplied by the runtime object system;
///        `NULL` is a no-op.
/// @details The pointer is cleared after `free()` so subsequent teardown code
///          cannot retain the stale allocation address.
static void objpool_finalizer(void *obj) {
    if (!obj)
        return;
    struct rt_objpool_impl *pool = (struct rt_objpool_impl *)obj;
    free(pool->slots);
    pool->slots = NULL;
}

/// @brief Allocate and initialize a fixed-capacity ObjectPool.
/// @param capacity Requested slot count, clamped to `[1, RT_OBJPOOL_MAX]`.
/// @return A new runtime-managed pool reference, or `NULL` if either the
///         object or its slot array cannot be allocated.
/// @details All slots initially contain zero data and form an ascending free
///          list, so the first acquisitions return indices 0, 1, 2, and so on.
///          A finalizer owns the separately allocated slot array.
rt_objpool rt_objpool_new(int64_t capacity) {
    if (capacity < 1)
        capacity = 1;
    if (capacity > RT_OBJPOOL_MAX)
        capacity = RT_OBJPOOL_MAX;

    struct rt_objpool_impl *pool =
        rt_obj_new_i64(RT_OBJPOOL_CLASS_ID, sizeof(struct rt_objpool_impl));
    if (!pool)
        return NULL;

    pool->slots = calloc((size_t)capacity, sizeof(struct pool_slot));
    if (!pool->slots) {
        if (rt_obj_release_check0(pool))
            rt_obj_free(pool);
        return NULL;
    }

    pool->capacity = capacity;
    pool->active_count = 0;
    pool->free_head = 0;
    pool->active_head = -1;

    // Initialize free list
    for (int64_t i = 0; i < capacity; i++) {
        pool->slots[i].data = 0;
        pool->slots[i].active = 0;
        pool->slots[i].next_free = (i + 1 < capacity) ? i + 1 : -1;
        pool->slots[i].next_active = -1;
    }

    rt_obj_set_finalizer(pool, objpool_finalizer);
    return pool;
}

/// @brief Release one runtime reference to an ObjectPool.
/// @param pool Handle to release; `NULL` is a no-op.
/// @details The runtime frees the object and invokes its slot-array finalizer
///          when this was the last reference. A non-null wrong-class handle
///          raises a runtime trap.
void rt_objpool_destroy(rt_objpool pool) {
    pool = checked_objpool(pool, "ObjectPool.Destroy: expected Zanna.Game.ObjectPool");
    if (pool && rt_obj_release_check0(pool))
        rt_obj_free(pool);
}

/// @brief Acquire the next free slot in constant time.
/// @param pool Pool from which to acquire a slot.
/// @return A stable index in `[0, capacity)`, or `-1` for a null or full pool.
/// @details The slot is removed from the free-list head, reset to data zero,
///          and prepended to the active list. A non-null wrong-class handle
///          raises a runtime trap.
int64_t rt_objpool_acquire(rt_objpool pool) {
    pool = checked_objpool(pool, "ObjectPool.Acquire: expected Zanna.Game.ObjectPool");
    if (!pool)
        return -1;
    if (pool->free_head < 0)
        return -1; // Pool is full

    int64_t slot = pool->free_head;
    pool->free_head = pool->slots[slot].next_free;
    pool->slots[slot].active = 1;
    pool->slots[slot].next_free = -1;
    pool->slots[slot].data = 0;
    // Prepend to active list (O(1))
    pool->slots[slot].next_active = pool->active_head;
    pool->active_head = slot;
    pool->active_count++;

    return slot;
}

/// @brief Return an active slot to the pool and erase its user data.
/// @param pool Pool that owns @p slot.
/// @param slot Index previously returned by rt_objpool_acquire().
/// @return `1` when an active slot was released; `0` for a null pool, an
///         out-of-range index, or an already-free slot.
/// @details Removing the active-list head is constant time. Removing another
///          active slot scans for its predecessor and is O(active_count). The
///          released index is prepended to the free list and will therefore be
///          the next slot acquired. A non-null wrong-class handle traps.
int8_t rt_objpool_release(rt_objpool pool, int64_t slot) {
    pool = checked_objpool(pool, "ObjectPool.Release: expected Zanna.Game.ObjectPool");
    if (!pool)
        return 0;
    if (slot < 0 || slot >= pool->capacity)
        return 0;
    if (!pool->slots[slot].active)
        return 0; // Already free

    pool->slots[slot].active = 0;
    pool->slots[slot].data = 0;
    // Remove from active list (O(active_count))
    if (pool->active_head == slot) {
        pool->active_head = pool->slots[slot].next_active;
    } else {
        int64_t prev = pool->active_head;
        while (prev >= 0 && pool->slots[prev].next_active != slot)
            prev = pool->slots[prev].next_active;
        if (prev >= 0)
            pool->slots[prev].next_active = pool->slots[slot].next_active;
    }
    pool->slots[slot].next_active = -1;
    // Return to free list
    pool->slots[slot].next_free = pool->free_head;
    pool->free_head = slot;
    pool->active_count--;

    return 1;
}

/// @brief Test whether a slot is currently acquired.
/// @param pool Pool that owns @p slot.
/// @param slot Candidate index.
/// @return `1` for an active in-range slot, or `0` for a free/out-of-range
///         slot or null pool.
/// @details A non-null handle of another runtime class raises a trap.
int8_t rt_objpool_is_active(rt_objpool pool, int64_t slot) {
    pool = checked_objpool(pool, "ObjectPool.IsActive: expected Zanna.Game.ObjectPool");
    if (!pool)
        return 0;
    if (slot < 0 || slot >= pool->capacity)
        return 0;
    return pool->slots[slot].active;
}

/// @brief Count the pool's currently acquired slots.
/// @param pool Pool to query.
/// @return Active slot count, or zero for `NULL`.
/// @details A non-null handle of another runtime class raises a trap.
int64_t rt_objpool_active_count(rt_objpool pool) {
    pool = checked_objpool(pool, "ObjectPool.ActiveCount: expected Zanna.Game.ObjectPool");
    return pool ? pool->active_count : 0;
}

/// @brief Count the slots currently available for acquisition.
/// @param pool Pool to query.
/// @return `capacity - active_count`, or zero for `NULL`.
/// @details A non-null handle of another runtime class raises a trap.
int64_t rt_objpool_free_count(rt_objpool pool) {
    pool = checked_objpool(pool, "ObjectPool.FreeCount: expected Zanna.Game.ObjectPool");
    return pool ? pool->capacity - pool->active_count : 0;
}

/// @brief Read the pool's immutable slot capacity.
/// @param pool Pool to query.
/// @return Total active-plus-free slots, or zero for `NULL`.
/// @details A non-null handle of another runtime class raises a trap.
int64_t rt_objpool_capacity(rt_objpool pool) {
    pool = checked_objpool(pool, "ObjectPool.Capacity: expected Zanna.Game.ObjectPool");
    return pool ? pool->capacity : 0;
}

/// @brief Test whether no slot remains available for acquisition.
/// @param pool Pool to query.
/// @return `1` when every slot is active; `0` otherwise. A null pool also
///         returns `1`.
/// @details A non-null handle of another runtime class raises a trap.
int8_t rt_objpool_is_full(rt_objpool pool) {
    pool = checked_objpool(pool, "ObjectPool.IsFull: expected Zanna.Game.ObjectPool");
    return pool ? (pool->active_count >= pool->capacity ? 1 : 0) : 1;
}

/// @brief Test whether the pool has no active slots.
/// @param pool Pool to query.
/// @return `1` when all slots are free. A null pool also returns `1`.
/// @details A non-null handle of another runtime class raises a trap.
int8_t rt_objpool_is_empty(rt_objpool pool) {
    pool = checked_objpool(pool, "ObjectPool.IsEmpty: expected Zanna.Game.ObjectPool");
    return pool ? (pool->active_count == 0 ? 1 : 0) : 1;
}

/// @brief Release every slot and rebuild the initial free list.
/// @param pool Pool to clear; `NULL` is a no-op.
/// @details All user data is reset to zero. Subsequent acquisition restarts at
///          index zero. Capacity is unchanged. A non-null wrong-class handle
///          raises a runtime trap.
void rt_objpool_clear(rt_objpool pool) {
    pool = checked_objpool(pool, "ObjectPool.Clear: expected Zanna.Game.ObjectPool");
    if (!pool)
        return;

    pool->active_count = 0;
    pool->free_head = 0;
    pool->active_head = -1;

    for (int64_t i = 0; i < pool->capacity; i++) {
        pool->slots[i].data = 0;
        pool->slots[i].active = 0;
        pool->slots[i].next_free = (i + 1 < pool->capacity) ? i + 1 : -1;
        pool->slots[i].next_active = -1;
    }
}

/// @brief Begin constant-time traversal of the active-slot list.
/// @param pool Pool to traverse.
/// @return The most recently acquired active slot, or `-1` for an empty or
///         null pool.
/// @details The active list is maintained in LIFO acquisition order. A
///          non-null wrong-class handle raises a runtime trap.
int64_t rt_objpool_first_active(rt_objpool pool) {
    // O(1): return head of the maintained active list
    pool = checked_objpool(pool, "ObjectPool.FirstActive: expected Zanna.Game.ObjectPool");
    return pool ? pool->active_head : -1;
}

/// @brief Continue constant-time traversal of the active-slot list.
/// @param pool Pool being traversed.
/// @param after In-range slot returned by rt_objpool_first_active() or an
///        earlier call to this function.
/// @return The following active-list index, or `-1` at the tail, for an
///         out-of-range index, or for a null pool.
/// @details The function follows the supplied slot's intrusive link; callers
///          should not release or clear traversal slots between iterator
///          steps. A non-null wrong-class handle raises a runtime trap.
int64_t rt_objpool_next_active(rt_objpool pool, int64_t after) {
    // O(1): follow the intrusive next_active pointer
    pool = checked_objpool(pool, "ObjectPool.NextActive: expected Zanna.Game.ObjectPool");
    if (!pool || after < 0 || after >= pool->capacity)
        return -1;
    return pool->slots[after].next_active;
}

/// @brief Store one caller-defined 64-bit value in an active slot.
/// @param pool Pool that owns @p slot.
/// @param slot Active slot index.
/// @param data Value to associate with the slot.
/// @return `1` on success, or `0` for a null pool, invalid index, or free slot.
/// @details The pool treats the value as opaque. It is reset to zero on
///          acquire, release, and clear. A non-null wrong-class handle traps.
int8_t rt_objpool_set_data(rt_objpool pool, int64_t slot, int64_t data) {
    pool = checked_objpool(pool, "ObjectPool.SetData: expected Zanna.Game.ObjectPool");
    if (!pool)
        return 0;
    if (slot < 0 || slot >= pool->capacity)
        return 0;
    if (!pool->slots[slot].active)
        return 0;
    pool->slots[slot].data = data;
    return 1;
}

/// @brief Read the caller-defined value associated with an active slot.
/// @param pool Pool that owns @p slot.
/// @param slot Active slot index.
/// @return The stored value, or zero for a null pool, invalid index, or free
///         slot.
/// @details Because zero is valid user data, use rt_objpool_is_active() when
///          the failure sentinel must be distinguished. A non-null
///          wrong-class handle raises a runtime trap.
int64_t rt_objpool_get_data(rt_objpool pool, int64_t slot) {
    pool = checked_objpool(pool, "ObjectPool.GetData: expected Zanna.Game.ObjectPool");
    if (!pool)
        return 0;
    if (slot < 0 || slot >= pool->capacity)
        return 0;
    if (!pool->slots[slot].active)
        return 0;
    return pool->slots[slot].data;
}
