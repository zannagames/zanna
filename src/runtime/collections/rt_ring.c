//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_ring.c
// Purpose: Implements a fixed-capacity circular ring buffer. When the ring is
//   full, pushing a new element overwrites the oldest element (the head
//   advances). This gives constant-time push with bounded memory — ideal for
//   sliding windows, input history, audio sample buffers, and recent-event logs.
//
// Key invariants:
//   - Capacity is fixed at construction and never changes.
//   - head is the index of the oldest element (next to be overwritten or popped).
//   - Logical element i maps to physical index (head + i) % capacity.
//   - Get(0) is oldest, Get(count-1) is newest.
//   - Push when full: overwrites the oldest element and advances head (no error).
//   - Push when not full: writes to (head + count) % capacity and increments count.
//   - Pop removes and returns the oldest element (head advances); returns NULL
//     if empty.
//   - By default the Ring retains element references and releases them when
//     overwritten, popped, cleared, or finalized.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - Ring objects are GC-managed (rt_obj_new_i64). The items array is
//     malloc-managed and freed by the GC finalizer (ring_finalizer).
//
// Links: src/runtime/collections/rt_ring.h (public API),
//        src/runtime/collections/rt_queue.h (growing FIFO queue variant)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the fixed-capacity overwrite-on-full Ring collection.
/// @details Logical order is always oldest to newest even when live elements
///          wrap around the native pointer array. Rings default to retained
///          ownership but may be configured as borrowing containers while
///          empty; that mode controls GC traversal and element lifetime.

#include "rt_ring.h"

#include "rt_box.h"

#include "rt_collection_ids.h"
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_object.h"

#include <stdlib.h>
#include <string.h>

/// @brief Ring buffer implementation structure.
/// @details `head` names logical index zero. A logical index is translated by
///          `(head + index) % capacity`; no separate tail field is needed
///          because `count` determines the next write slot.
typedef struct rt_ring_impl {
    void **vptr;          ///< Vtable pointer placeholder.
    void **items;         ///< Array of element pointers.
    size_t capacity;      ///< Maximum number of elements.
    size_t head;          ///< Index of oldest element.
    size_t count;         ///< Number of elements currently stored.
    int8_t owns_elements; ///< Whether stored elements are retained/released.
} rt_ring_impl;

/// @brief Checked cast of an opaque handle to the RingBuffer implementation;
///        traps with @p what if @p obj is NULL or not a RingBuffer.
/// @param obj Opaque runtime handle to validate.
/// @param what Diagnostic emitted by the trap subsystem on failure.
/// @return Validated Ring implementation, or `NULL` after trapping.
static rt_ring_impl *as_ring(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_RING_CLASS_ID, sizeof(rt_ring_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_ring_impl *)obj;
}

/// @brief Drop one GC reference to a stored element and free it at zero.
/// @param value Runtime object reference, or `NULL` for a no-op.
static void ring_release_value(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief GC traversal callback for owned Ring elements.
/// @details The Ring retains stored elements when `owns_elements` is true, so
///          every live slot must be reported to the cycle collector. Borrowed
///          rings skip traversal to match their non-owning lifetime contract.
/// @param obj Ring object being traversed.
/// @param visitor Collector visitor callback.
/// @param ctx Collector-provided callback context.
static void rt_ring_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    if (!ring || !ring->owns_elements || !ring->items || ring->capacity == 0)
        return;
    for (size_t i = 0; i < ring->count; i++) {
        size_t idx = (ring->head + i) % ring->capacity;
        visitor(ring->items[idx], ctx);
    }
}

/// @brief Finalizer callback invoked by the garbage collector when a Ring is collected.
///
/// This function is called automatically by the Zanna runtime's garbage collector
/// when a Ring object becomes unreachable and is about to be freed. The finalizer
/// releases the internal items array that was allocated to store element pointers.
///
/// @param obj Pointer to the Ring object being finalized. May be NULL (no-op).
static void rt_ring_finalize(void *obj) {
    if (!obj)
        return;
    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    if (!ring)
        return;
    if (ring->owns_elements && ring->items) {
        size_t count = ring->count;
        ring->count = 0;
        for (size_t i = 0; i < count; i++) {
            size_t idx = (ring->head + i) % ring->capacity;
            void *value = ring->items[idx];
            ring->items[idx] = NULL;
            ring_release_value(value);
        }
    }
    free(ring->items);
    ring->items = NULL;
    ring->capacity = 0;
    ring->head = 0;
    ring->count = 0;
}

/// @brief Creates a new Ring buffer with the specified fixed capacity.
///
/// Allocates and initializes a circular buffer that can hold up to `capacity` elements.
/// The Ring is a fixed-size FIFO container - once created, its capacity cannot change.
/// When the buffer is full and a new element is pushed, the oldest element is
/// automatically overwritten (no memory allocation occurs during push operations).
///
/// The Ring is allocated through Zanna's garbage-collected object system via
/// `rt_obj_new_i64`, which means it will be automatically freed when no longer
/// referenced. A finalizer is registered to clean up the internal items array.
///
/// Memory layout after successful creation:
/// ```
/// Ring object (GC-managed):
///   +--------+--------+----------+------+-------+
///   | vptr   | items  | capacity | head | count |
///   | (NULL) | -----> | N        | 0    | 0     |
///   +--------+---|----+----------+------+-------+
///                |
///                v
///   items array (malloc'd):
///   +------+------+------+-----+------+
///   | NULL | NULL | NULL | ... | NULL |
///   +------+------+------+-----+------+
///   (capacity slots, all initially NULL)
/// ```
///
/// @param capacity The maximum number of elements the Ring can hold. If 0,
///                 a minimum capacity of 1 is used. Negative values trap.
///
/// @return A pointer to the newly created Ring, or NULL if the Ring object
///         allocation fails. Items-array allocation failure traps.
///
/// @see rt_ring_push For adding elements to the Ring
/// @see rt_ring_pop For removing elements from the Ring
/// @see rt_ring_finalize For cleanup behavior
void *rt_ring_new(int64_t capacity) {
    if (capacity < 0) {
        rt_trap("Ring: negative capacity");
        return NULL;
    }
    if (capacity == 0)
        capacity = 1; // Minimum capacity of 1

    if ((uint64_t)capacity > SIZE_MAX / sizeof(void *)) {
        rt_trap("Ring: allocation size overflow");
        return NULL;
    }

    rt_ring_impl *ring =
        (rt_ring_impl *)rt_obj_new_i64(RT_RING_CLASS_ID, (int64_t)sizeof(rt_ring_impl));
    if (!ring)
        return NULL;

    ring->vptr = NULL;
    ring->items = (void **)calloc((size_t)capacity, sizeof(void *));
    if (!ring->items) {
        if (rt_obj_release_check0(ring))
            rt_obj_free(ring);
        rt_trap("Ring: memory allocation failed");
        return NULL;
    }
    ring->capacity = (size_t)capacity;
    ring->head = 0;
    ring->count = 0;
    ring->owns_elements = 1;
    rt_obj_set_finalizer(ring, rt_ring_finalize);
    rt_gc_track(ring, rt_ring_traverse);
    return ring;
}

/// @brief Creates a new Ring with default capacity (16).
/// @return New runtime-managed owning Ring with sixteen slots.
void *rt_ring_new_default(void) {
    return rt_ring_new(16);
}

/// @brief Returns the current number of elements stored in the Ring.
///
/// This function returns how many elements are currently in the Ring, which is
/// always between 0 and the Ring's capacity (inclusive). The length increases
/// when elements are pushed (until capacity is reached), decreases when elements
/// are popped, and resets to 0 when the Ring is cleared.
///
/// Note that once the Ring is full, pushing new elements does NOT increase the
/// length - the oldest element is overwritten and the length stays at capacity.
///
/// @param obj Pointer to a Ring object. If NULL, returns 0.
///
/// @return The number of elements currently stored in the Ring (0 to capacity).
///         Returns 0 if obj is NULL.
///
/// @note O(1) time complexity - the count is stored directly in the structure.
///
/// @see rt_ring_cap For the maximum capacity
/// @see rt_ring_is_empty For checking if the Ring is empty
int64_t rt_ring_len(void *obj) {
    if (!obj)
        return 0;
    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    return ring ? (int64_t)ring->count : 0;
}

/// @brief Returns the maximum capacity of the Ring.
///
/// This function returns the fixed maximum number of elements the Ring can hold,
/// as specified when the Ring was created via `rt_ring_new`. The capacity never
/// changes after creation - Rings are fixed-size containers.
///
/// @param obj Pointer to a Ring object. If NULL, returns 0.
///
/// @return The maximum number of elements the Ring can hold. Returns 0 if obj
///         is NULL or if the Ring was created with a failed items allocation.
///
/// @note O(1) time complexity - the capacity is stored directly in the structure.
///
/// @see rt_ring_new For creating a Ring with a specific capacity
/// @see rt_ring_len For the current number of elements
/// @see rt_ring_is_full For checking if the Ring is at capacity
int64_t rt_ring_cap(void *obj) {
    if (!obj)
        return 0;
    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    return ring ? (int64_t)ring->capacity : 0;
}

/// @brief Checks whether the Ring contains no elements.
///
/// A Ring is considered empty when its length is 0, which occurs:
/// - Immediately after creation (before any push operations)
/// - After all elements have been popped
/// - After calling rt_ring_clear
///
/// An empty Ring will return NULL from rt_ring_pop, rt_ring_peek, and rt_ring_get.
///
/// @param obj Pointer to a Ring object. If NULL, returns true (1) since a
///            non-existent Ring conceptually has no elements.
///
/// @return 1 (true) if the Ring is empty or obj is NULL, 0 (false) otherwise.
///
/// @note O(1) time complexity.
///
/// @see rt_ring_is_full For the opposite check
/// @see rt_ring_len For the exact count of elements
int8_t rt_ring_is_empty(void *obj) {
    return rt_ring_len(obj) == 0;
}

/// @brief Checks whether the Ring is at maximum capacity.
///
/// A Ring is considered full when its current length equals its capacity.
/// When a Ring is full:
/// - Pushing a new element will overwrite the oldest element (no error occurs)
/// - The head pointer advances to maintain FIFO ordering
/// - The length stays the same (still at capacity)
///
/// This is useful for callers who want to know if a push will discard data,
/// or who want to implement a non-overwriting policy by checking before push.
///
/// @param obj Pointer to a Ring object. If NULL, returns false (0).
///
/// @return 1 (true) if the Ring is full, 0 (false) if not full or obj is NULL.
///
/// @note O(1) time complexity.
/// @note A Ring with capacity=0 (failed allocation) is never considered full.
///
/// @see rt_ring_is_empty For the opposite check
/// @see rt_ring_push For the behavior when pushing to a full Ring
int8_t rt_ring_is_full(void *obj) {
    if (!obj)
        return 0;
    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    if (!ring)
        return 0;
    return ring->count == ring->capacity;
}

/// @brief Select retained or borrowed element storage for an empty Ring.
/// @details Mode changes are forbidden while elements are present because
///          existing slots were inserted under the current retain contract.
///          Null or invalid handles trap.
/// @param obj Empty Ring to configure.
/// @param owns Nonzero for retain/trace/release behavior; zero for raw
///             borrowed pointers.
void rt_ring_set_owns_elements(void *obj, int8_t owns) {
    rt_gc_mutator_enter();
    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    if (!ring) {
        rt_gc_mutator_exit();
        return;
    }
    owns = owns ? 1 : 0;
    if (ring->count != 0 && ring->owns_elements != owns) {
        rt_gc_mutator_exit();
        rt_trap("Ring: cannot change ownership mode while non-empty");
        return;
    }
    ring->owns_elements = owns;
    rt_gc_mutator_exit();
}

/// @brief Report whether a Ring retains and traces stored elements.
/// @param obj Ring handle, or `NULL`.
/// @return 1 in owning mode, otherwise 0; `NULL` reports 0.
int8_t rt_ring_owns_elements(void *obj) {
    if (!obj)
        return 0;
    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    return ring && ring->owns_elements ? 1 : 0;
}

/// @brief Adds an element to the Ring buffer.
///
/// Pushes a new element to the "tail" (newest end) of the Ring. The behavior
/// depends on whether the Ring is full:
///
/// **When the Ring has space (count < capacity):**
/// - The element is stored at the tail position
/// - The count increases by 1
/// - No existing elements are affected
///
/// **When the Ring is full (count == capacity):**
/// - The element overwrites the oldest element (at head position)
/// - The head advances to the next-oldest element
/// - The count stays the same (still full)
/// - In owning mode, the overwritten element is released.
///
/// Visual example with capacity=3:
/// ```
/// Initial (empty):     [_, _, _]  head=0, count=0
/// Push(A):             [A, _, _]  head=0, count=1
/// Push(B):             [A, B, _]  head=0, count=2
/// Push(C):             [A, B, C]  head=0, count=3 (FULL)
/// Push(D):             [D, B, C]  head=1, count=3 (A overwritten, D at old head position)
/// Push(E):             [D, E, C]  head=2, count=3 (B overwritten)
/// ```
///
/// @param obj Pointer to a Ring object. If NULL, this function is a no-op.
/// @param elem The element pointer to add. May be NULL (NULL is a valid element).
///
/// @note O(1) time complexity - no memory allocation or copying occurs.
/// @note In owning mode, the Ring retains elem before releasing an overwritten
///       value, so overwriting a slot with the same object is safe. Borrowing
///       mode stores and discards raw pointers without lifetime operations.
/// @note Thread safety: Not thread-safe. External synchronization required for
///       concurrent access.
///
/// @warning When pushing to a full Ring, the oldest element is silently discarded.
///          Use rt_ring_is_full to check before pushing if data loss is unacceptable.
///
/// @see rt_ring_pop For removing and returning the oldest element
/// @see rt_ring_is_full For checking if push will overwrite
void rt_ring_push(void *obj, void *elem) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    if (!ring) {
        rt_gc_mutator_exit();
        return;
    }
    if (ring->capacity == 0 || !ring->items) {
        rt_gc_mutator_exit();
        return;
    }

    if (ring->owns_elements)
        rt_obj_retain_maybe(elem);

    // Calculate tail position (where new element goes)
    size_t tail = (ring->head + ring->count) % ring->capacity;

    if (ring->count == ring->capacity) {
        // Ring is full - overwrite oldest element. Data loss is by design for ring buffers.
        void *old = ring->items[ring->head];
        ring->items[ring->head] = elem;
        if (ring->owns_elements)
            ring_release_value(old);
        // Advance head to next oldest
        ring->head = (ring->head + 1) % ring->capacity;
        // count stays the same (still full)
    } else {
        // Ring has space - add to tail
        ring->items[tail] = elem;
        ring->count++;
    }
    rt_gc_mutator_exit();
}

/// @brief Removes and returns the oldest element from the Ring.
///
/// Pops the element at the "head" (oldest end) of the Ring in FIFO order.
/// This is the element that was pushed earliest and hasn't been overwritten
/// or previously popped.
///
/// After a successful pop:
/// - The head index advances to the next-oldest element
/// - The count decreases by 1
/// - The slot is cleared to NULL (for safety/debugging)
///
/// Visual example with capacity=3:
/// ```
/// State before:  [A, B, C]  head=0, count=3
/// Pop() -> A:    [_, B, C]  head=1, count=2
/// Pop() -> B:    [_, _, C]  head=2, count=1
/// Pop() -> C:    [_, _, _]  head=0, count=0
/// Pop() -> NULL: [_, _, _]  (empty, nothing to pop)
/// ```
///
/// @param obj Pointer to a Ring object. If NULL, returns NULL.
///
/// @return The oldest element in the Ring, or NULL if the Ring is empty or
///         obj is NULL. Note that NULL may also be a valid stored element,
///         so use rt_ring_is_empty to distinguish between "empty" and
///         "contains NULL".
///
/// @note O(1) time complexity.
/// @note Ownership transfer: In owning mode, the returned value is retained for
///       the caller before the Ring drops its stored reference.
/// @note Thread safety: Not thread-safe. External synchronization required.
///
/// @see rt_ring_peek For reading the oldest element without removing it
/// @see rt_ring_push For adding elements
/// @see rt_ring_is_empty For checking if the Ring has elements to pop
void *rt_ring_pop(void *obj) {
    if (!obj)
        return NULL;

    rt_gc_mutator_enter();
    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    if (!ring) {
        rt_gc_mutator_exit();
        return NULL;
    }
    if (ring->count == 0 || !ring->items) {
        rt_gc_mutator_exit();
        return NULL;
    }

    // Get oldest element (at head)
    void *item = ring->items[ring->head];
    if (ring->owns_elements)
        rt_obj_retain_maybe(item);
    ring->items[ring->head] = NULL;

    // Advance head
    ring->head = (ring->head + 1) % ring->capacity;
    ring->count--;

    if (ring->owns_elements)
        ring_release_value(item);
    rt_gc_mutator_exit();
    return item;
}

/// @brief Returns the oldest element without removing it from the Ring.
///
/// Peeks at the element at the "head" (oldest end) of the Ring without
/// modifying the Ring's state. This is equivalent to what rt_ring_pop would
/// return, but the element remains in the Ring for future access.
///
/// This function is useful for:
/// - Inspecting the next element to be popped without committing
/// - Implementing conditional pop logic ("peek then pop if condition met")
/// - Observing elements in a producer-consumer pattern
///
/// @param obj Pointer to a Ring object. If NULL, returns NULL.
///
/// @return The oldest element in the Ring, or NULL if the Ring is empty or
///         obj is NULL. Note that NULL may also be a valid stored element,
///         so use rt_ring_is_empty to distinguish between "empty" and
///         "contains NULL".
///
/// @note O(1) time complexity.
/// @note The Ring retains ownership of the element. The returned pointer
///       receives no new retain and is only valid as long as its producer or
///       the owning Ring keeps it alive (i.e., until it is popped, overwritten,
///       cleared, or the Ring is collected).
/// @note Thread safety: Not thread-safe. The returned pointer may become
///       invalid if another thread modifies the Ring.
///
/// @see rt_ring_pop For removing and returning the oldest element
/// @see rt_ring_get For accessing elements by logical index
void *rt_ring_peek(void *obj) {
    if (!obj)
        return NULL;

    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    if (!ring)
        return NULL;
    if (ring->count == 0 || !ring->items)
        return NULL;

    // Return oldest element without removing
    return ring->items[ring->head];
}

/// @brief Retrieves an element by its logical index within the Ring.
///
/// Provides random access to Ring elements using logical indexing where:
/// - Index 0 is the oldest element (the head, what rt_ring_peek returns)
/// - Index (len-1) is the newest element (the most recently pushed)
///
/// The logical index is translated to the physical array position using
/// circular arithmetic: `actual = (head + index) % capacity`
///
/// Visual example with capacity=5, after Push(A), Push(B), Push(C):
/// ```
/// Physical array: [A, B, C, _, _]  head=0, count=3
/// Logical indices: Get(0)=A, Get(1)=B, Get(2)=C, Get(3)=NULL (out of bounds)
///
/// After Pop() (removes A):
/// Physical array: [_, B, C, _, _]  head=1, count=2
/// Logical indices: Get(0)=B, Get(1)=C, Get(2)=NULL (out of bounds)
///
/// After Push(D), Push(E), Push(F) (wraps around, overwrites B):
/// Physical array: [F, _, C, D, E]  head=2, count=4
/// Logical indices: Get(0)=C, Get(1)=D, Get(2)=E, Get(3)=F
/// ```
///
/// @param obj Pointer to a Ring object. If NULL, returns NULL.
/// @param index Logical index into the Ring (0 = oldest, len-1 = newest).
///              Must be in range [0, len-1] or NULL is returned.
///
/// @return The element at the specified logical index, or NULL if:
///         - obj is NULL
///         - index is negative
///         - index >= current length (out of bounds)
///         - Ring has no items array (allocation failed)
///
/// @note O(1) time complexity - direct array access with modular arithmetic.
/// @note The Ring retains ownership of the element. The returned pointer
///       receives no new retain and is only valid as long as its producer or
///       the owning Ring keeps it alive.
/// @note Thread safety: Not thread-safe. Index validity may change if another
///       thread modifies the Ring.
///
/// @see rt_ring_peek For accessing just the oldest element (index 0)
/// @see rt_ring_len For determining valid index range
void *rt_ring_get(void *obj, int64_t index) {
    if (!obj)
        return NULL;

    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    if (!ring)
        return NULL;
    if (index < 0 || (size_t)index >= ring->count || !ring->items)
        return NULL;

    // Calculate actual index: logical 0 = head (oldest)
    size_t actual = (ring->head + (size_t)index) % ring->capacity;
    return ring->items[actual];
}

/// @brief Removes all elements from the Ring without deallocating memory.
///
/// Clears the Ring by resetting it to an empty state. After this call:
/// - The count becomes 0
/// - The head resets to 0
/// - All element slots are set to NULL
/// - The Ring can be reused with push operations
/// - The capacity remains unchanged (no reallocation)
///
/// This function iterates through all stored elements and sets their slots
/// to NULL for safety, preventing dangling pointer access through get/peek
/// operations that might occur due to bugs. This is a defensive measure
/// rather than a strict requirement.
///
/// @param obj Pointer to a Ring object. If NULL, this function is a no-op.
///
/// @note O(n) time complexity where n is the current number of elements,
///       due to the NULL-clearing loop. This could be optimized to O(1) if
///       the NULL-clearing is not needed, but the defensive safety is preferred.
///
/// @note Memory behavior: No memory is freed. The items array remains allocated
///       at its original capacity. To fully free a Ring, let the garbage
///       collector reclaim it (which triggers rt_ring_finalize).
///
/// @note Ownership: In owning mode, each stored element is released.
///
/// @note Thread safety: Not thread-safe. External synchronization required.
///
/// @see rt_ring_pop For removing elements one at a time with retrieval
/// @see rt_ring_finalize For the destructor behavior
void rt_ring_clear(void *obj) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    if (!ring) {
        rt_gc_mutator_exit();
        return;
    }
    if (!ring->items) {
        rt_gc_mutator_exit();
        return;
    }

    size_t count = ring->count;
    size_t head = ring->head;
    ring->head = 0;
    ring->count = 0;
    for (size_t i = 0; i < count; i++) {
        size_t idx = (head + i) % ring->capacity;
        void *value = ring->items[idx];
        ring->items[idx] = NULL;
        if (ring->owns_elements)
            ring_release_value(value);
    }
    rt_gc_mutator_exit();
}

/// @brief Check whether any live Ring element equals @p elem.
/// @details Scans oldest to newest with `rt_box_equal`: boxed numeric/string
///          values compare by content and ordinary runtime objects by identity.
///          A null Ring reports absence.
/// @param obj Ring handle, or `NULL`.
/// @param elem Value to compare; may be `NULL`.
/// @return 1 on the first match, otherwise 0.
int8_t rt_ring_has(void *obj, void *elem) {
    if (!obj)
        return 0;

    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    if (!ring)
        return 0;
    if (!ring->items)
        return 0;

    for (size_t i = 0; i < ring->count; i++) {
        size_t idx = (ring->head + i) % ring->capacity;
        if (rt_box_equal(ring->items[idx], elem))
            return 1;
    }
    return 0;
}

/// @brief Borrow the oldest Ring element.
/// @details Equivalent to @ref rt_ring_peek; no element is removed or retained.
/// @param obj Ring handle, or `NULL`.
/// @return Borrowed oldest value, or `NULL` when empty/null/null-valued.
void *rt_ring_first(void *obj) {
    return rt_ring_peek(obj);
}

/// @brief Borrow the newest Ring element.
/// @details Computes logical index `count - 1` without modifying or retaining
///          the stored value.
/// @param obj Ring handle, or `NULL`.
/// @return Borrowed newest value, or `NULL` when empty/null/null-valued.
void *rt_ring_last(void *obj) {
    if (!obj)
        return NULL;

    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    if (!ring)
        return NULL;
    if (ring->count == 0 || !ring->items)
        return NULL;

    size_t idx = (ring->head + ring->count - 1) % ring->capacity;
    return ring->items[idx];
}

/// @brief Reverse the logical oldest-to-newest order in place.
/// @details Swaps symmetric live slots through circular index translation.
///          Capacity, head, count, ownership mode, and retain counts are
///          unchanged. A null Ring or fewer than two values is a no-op.
/// @param obj Ring to mutate, or `NULL`.
void rt_ring_reverse(void *obj) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    if (!ring) {
        rt_gc_mutator_exit();
        return;
    }
    if (ring->count < 2 || !ring->items) {
        rt_gc_mutator_exit();
        return;
    }

    for (size_t i = 0; i < ring->count / 2; i++) {
        size_t front_idx = (ring->head + i) % ring->capacity;
        size_t back_idx = (ring->head + ring->count - 1 - i) % ring->capacity;

        void *tmp = ring->items[front_idx];
        ring->items[front_idx] = ring->items[back_idx];
        ring->items[back_idx] = tmp;
    }
    rt_gc_mutator_exit();
}

/// @brief Create a shallow Ring clone with independent storage.
/// @details Preserves capacity, logical order, and ownership mode. An owning
///          clone independently retains every value; a borrowing clone copies
///          raw pointers. A null source produces an empty owning Ring of the
///          minimum one-element capacity.
/// @param obj Source Ring handle, or `NULL`.
/// @return New runtime-managed Ring clone.
void *rt_ring_clone(void *obj) {
    if (!obj)
        return rt_ring_new(1);

    rt_ring_impl *ring = as_ring(obj, "Ring: invalid Ring object");
    if (!ring)
        return NULL;

    void *new_ring = rt_ring_new((int64_t)ring->capacity);
    if (!new_ring)
        return NULL;
    if (!ring->owns_elements)
        rt_ring_set_owns_elements(new_ring, 0);

    for (size_t i = 0; i < ring->count; i++) {
        size_t idx = (ring->head + i) % ring->capacity;
        rt_ring_push(new_ring, ring->items[idx]);
    }

    return new_ring;
}
