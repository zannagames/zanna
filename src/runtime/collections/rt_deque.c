//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_deque.c
// Purpose: Implements a double-ended queue (deque) using a circular buffer.
//   Elements can be pushed and popped from both the front and back in O(1)
//   amortized time. Random access by index is O(1). Provides a richer interface
//   than Stack (LIFO only) or Queue (FIFO only) while retaining their
//   performance characteristics.
//
// Key invariants:
//   - Backed by a circular buffer of initial capacity DEFAULT_CAPACITY (16).
//     Growth doubles the capacity and linearizes the elements into a new array.
//   - `front` is the index of the logical element at position 0 (oldest for
//     queue-like use; the "left" end for deque use).
//   - Physical index of logical element i is (front + i) % cap.
//   - Push-back: writes to (front + len) % cap; push-front: decrements front
//     with wrap-around, then writes at the new front.
//   - Out-of-bounds Get aborts via trap_with_message.
//   - Deque retains pushed values and returns retained transfers from Pop/Peek.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - Deque objects are GC-managed (rt_obj_new_i64). The data array is
//     malloc-managed and freed by the GC finalizer (deque_finalizer).
//
// Links: src/runtime/collections/rt_deque.h (public API)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements a retained-element double-ended queue as a ring buffer.
///
/// Logical index zero is the front element, while physical storage can wrap
/// around the allocated array. Growth doubles capacity and linearizes live
/// elements without changing their logical order. Pushes, pops, and peeks at
/// either end are constant-time amortized; indexed access is constant-time.
///
/// Inserted runtime values are retained. Pop, peek, and get operations return
/// retained references for the caller, including when the underlying mapping
/// remains present. Deque objects and their elements participate in runtime GC
/// traversal. Mutation is not safe without external synchronization.

#include "rt_deque.h"

#include "rt_box.h"
#include "rt_collection_ids.h"
#include "rt_collection_ownership.h"
#include "rt_deque_internal.h"
#include "rt_gc.h"
#include "rt_object.h"
#include "rt_option.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Install a non-local recovery target for the current thread.
/// @param buf Jump buffer that receives control after a runtime trap.
void rt_trap_set_recovery(jmp_buf *buf);
/// @brief Remove the current thread's active legacy recovery target.
void rt_trap_clear_recovery(void);
/// @brief Borrow the most recent runtime trap diagnostic.
/// @return Null-terminated diagnostic text, or NULL when unavailable.
const char *rt_trap_get_error(void);

//=============================================================================
// Internal Structure
//=============================================================================

/// Default number of ring-buffer slots.
#define DEFAULT_CAPACITY 16

//=============================================================================
// Helper Functions
//=============================================================================

#include "rt_trap.h"

/// @brief Thin wrapper that raises a runtime trap with @p msg.
/// @param msg Human-readable runtime trap message.
static void trap_with_message(const char *msg) {
    rt_trap(msg);
}

/// @brief Checked cast of an opaque handle to the Deque struct;
///        traps with @p what if @p obj is NULL or not a Deque.
/// @param obj Opaque runtime object handle to validate.
/// @param what Trap message used on validation failure.
/// @return The validated Deque pointer, or NULL if a returning trap handler
///         resumes after failed validation.
static Deque *as_deque(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_DEQUE_CLASS_ID, sizeof(Deque))) {
        trap_with_message(what);
        return NULL;
    }
    return (Deque *)obj;
}

/// @brief Drop one GC reference to a stored element and free it at zero.
/// @param value Retained runtime value, or NULL for a no-op.
static void deque_release_value(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Save an active trap diagnostic before clearing its recovery frame.
/// @param buffer Destination for the bounded diagnostic copy.
/// @param buffer_size Capacity of @p buffer including its terminator.
/// @param fallback Message used when no active diagnostic is available.
static void deque_save_trap(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief Grow the ring buffer to hold at least @p required elements.
/// @details Doubles capacity (or jumps to @p required), then linearizes the
///          wrapped contents so @c front becomes 0. Traps on overflow/OOM.
/// @param d Deque whose backing array is to grow.
/// @param required Minimum number of element slots required.
/// @return 1 when sufficient capacity is available; otherwise 0 after a trap
///         with the original allocation and logical order intact.
static int ensure_capacity(Deque *d, int64_t required) {
    if (required <= d->cap)
        return 1;

    if (d->cap > INT64_MAX / 2) {
        trap_with_message("Deque: capacity overflow");
        return 0;
    }
    int64_t new_cap = d->cap * 2;
    if (new_cap < required)
        new_cap = required;

    if ((uint64_t)new_cap > SIZE_MAX / sizeof(void *)) {
        trap_with_message("Deque: allocation size overflow");
        return 0;
    }
    void **new_data = (void **)malloc((size_t)new_cap * sizeof(void *));
    if (!new_data) {
        trap_with_message("Failed to allocate memory for deque");
        return 0;
    }
    if (d->len > new_cap) {
        free(new_data);
        trap_with_message("Deque: corrupted length exceeds capacity");
        return 0;
    }

    // Copy elements to new array, starting at index 0
    for (int64_t i = 0; i < d->len; i++) {
        int64_t idx = (d->front + i) % d->cap;
        new_data[i] = d->data[idx];
    }

    free(d->data);
    d->data = new_data;
    d->cap = new_cap;
    d->front = 0;
    return 1;
}

//=============================================================================
// Deque Creation
//=============================================================================

/// @brief Creates an empty Deque with the default capacity.
/// @return A new runtime-managed Deque, or NULL after an allocation trap.
void *rt_deque_new(void) {
    return rt_deque_with_capacity(DEFAULT_CAPACITY);
}

/// @brief GC finalizer: release every live element and free the ring buffer.
/// @param obj Deque object being finalized.
static void deque_finalize(void *obj) {
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (d && d->data) {
        for (int64_t i = 0; i < d->len; i++) {
            int64_t idx = (d->front + i) % d->cap;
            deque_release_value(d->data[idx]);
            d->data[idx] = NULL;
        }
        free(d->data);
        d->data = NULL;
        d->len = 0;
        d->cap = 0;
        d->front = 0;
    }
}

/// @brief GC traversal: visit every live element in logical (front→back) order.
/// @param obj Deque object to traverse.
/// @param visitor Runtime callback invoked for each retained element.
/// @param ctx Opaque visitor context forwarded unchanged.
static void deque_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d)
        return;
    if (!d->data || d->cap <= 0)
        return;
    for (int64_t i = 0; i < d->len; i++) {
        int64_t idx = (d->front + i) % d->cap;
        visitor(d->data[idx], ctx);
    }
}

/// @brief Creates an empty Deque with a requested initial capacity.
/// @param cap Requested slots; values below one are normalized to one.
/// @return A new runtime-managed Deque, or NULL after a size or allocation trap.
void *rt_deque_with_capacity(int64_t cap) {
    if (cap < 1)
        cap = 1;
    if ((uint64_t)cap > SIZE_MAX / sizeof(void *)) {
        trap_with_message("Deque: allocation size overflow");
        return NULL;
    }

    Deque *d = (Deque *)rt_obj_new_i64(RT_DEQUE_CLASS_ID, (int64_t)sizeof(Deque));
    if (!d) {
        trap_with_message("Deque: memory allocation failed");
        return NULL;
    }

    d->data = (void **)malloc((size_t)cap * sizeof(void *));
    if (!d->data) {
        if (rt_obj_release_check0(d))
            rt_obj_free(d);
        trap_with_message("Deque: memory allocation failed");
        return NULL;
    }

    d->cap = cap;
    d->len = 0;
    d->front = 0;
    rt_obj_set_finalizer(d, deque_finalize);
    rt_gc_track(d, deque_traverse);
    return d;
}

//=============================================================================
// Size Operations
//=============================================================================

/// @brief Returns the number of elements in the deque.
/// @param obj Pointer to the Deque object, or NULL.
/// @return Element count, or 0 if obj is NULL.
int64_t rt_deque_len(void *obj) {
    if (!obj)
        return 0;
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d)
        return 0;
    return d->len;
}

/// @brief Returns the current capacity of the deque's backing buffer.
/// @param obj Pointer to the Deque object, or NULL.
/// @return Buffer capacity, or 0 if obj is NULL.
int64_t rt_deque_cap(void *obj) {
    if (!obj)
        return 0;
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d)
        return 0;
    return d->cap;
}

/// @brief Returns whether the deque contains no elements.
/// @param obj Pointer to the Deque object, or NULL.
/// @return 1 if empty or NULL, 0 otherwise.
int8_t rt_deque_is_empty(void *obj) {
    if (!obj)
        return 1;
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d)
        return 1;
    return d->len == 0 ? 1 : 0;
}

//=============================================================================
// Front Operations
//=============================================================================

/// @brief Pushes an element onto the front of the deque.
/// @param obj Pointer to the Deque object. No-op if NULL.
/// @param elem Element to push. May be NULL.
/// @note Grows the backing buffer if needed. O(1) amortized.
void rt_deque_push_front(void *obj, void *elem) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");

    if (!d) {
        rt_gc_mutator_exit();
        return;
    }

    if (d->len >= INT64_MAX) {
        rt_gc_mutator_exit();
        trap_with_message("Deque: maximum capacity reached");
        return;
    }
    if (!ensure_capacity(d, d->len + 1)) {
        rt_gc_mutator_exit();
        return;
    }

    if (!rt_collection_retain_checked(elem, "Deque.PushFront: value retain failed")) {
        rt_gc_mutator_exit();
        return;
    }
    // Move front pointer backward (with wrap-around)
    d->front = (d->front - 1 + d->cap) % d->cap;
    d->data[d->front] = elem;
    d->len++;
    rt_gc_mutator_exit();
}

/// @brief Removes and returns the front element.
/// @param obj Pointer to the Deque object. Must not be NULL.
/// @return The removed element as a retained reference owned by the caller.
/// @note Traps if the deque is NULL or empty.
void *rt_deque_pop_front(void *obj) {
    if (!obj) {
        trap_with_message("PopFront called on NULL deque");
        return NULL;
    }
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d) {
        rt_gc_mutator_exit();
        return NULL;
    }
    if (d->len == 0) {
        rt_gc_mutator_exit();
        trap_with_message("PopFront called on empty deque");
        return NULL;
    }

    void *val = d->data[d->front];
    d->data[d->front] = NULL;
    d->front = (d->front + 1) % d->cap;
    d->len--;
    // Move the deque's stored reference to the caller.
    rt_gc_mutator_exit();
    return val;
}

/// @brief Returns the front element without removing it.
/// @param obj Pointer to the Deque object. Must not be NULL.
/// @return The front element as a retained reference owned by the caller.
/// @note Traps if the deque is NULL or empty.
void *rt_deque_peek_front(void *obj) {
    if (!obj) {
        trap_with_message("PeekFront called on NULL deque");
        return NULL;
    }
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d) {
        rt_gc_mutator_exit();
        return NULL;
    }
    if (d->len == 0) {
        rt_gc_mutator_exit();
        trap_with_message("PeekFront called on empty deque");
        return NULL;
    }

    void *val = d->data[d->front];
    if (!rt_collection_retain_checked(val, "Deque.PeekFront: result retain failed")) {
        rt_gc_mutator_exit();
        return NULL;
    }
    rt_gc_mutator_exit();
    return val;
}

//=============================================================================
// Back Operations
//=============================================================================

/// @brief Pushes an element onto the back of the deque.
/// @param obj Pointer to the Deque object. No-op if NULL.
/// @param elem Element to push. May be NULL.
/// @note Grows the backing buffer if needed. O(1) amortized.
void rt_deque_push_back(void *obj, void *elem) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");

    if (!d) {
        rt_gc_mutator_exit();
        return;
    }

    if (d->len >= INT64_MAX) {
        rt_gc_mutator_exit();
        trap_with_message("Deque: maximum capacity reached");
        return;
    }
    if (!ensure_capacity(d, d->len + 1)) {
        rt_gc_mutator_exit();
        return;
    }

    int64_t back = (d->front + d->len) % d->cap;
    if (!rt_collection_retain_checked(elem, "Deque.PushBack: value retain failed")) {
        rt_gc_mutator_exit();
        return;
    }
    d->data[back] = elem;
    d->len++;
    rt_gc_mutator_exit();
}

/// @brief Removes and returns the back element.
/// @param obj Pointer to the Deque object. Must not be NULL.
/// @return The removed element as a retained reference owned by the caller.
/// @note Traps if the deque is NULL or empty.
void *rt_deque_pop_back(void *obj) {
    if (!obj) {
        trap_with_message("PopBack called on NULL deque");
        return NULL;
    }
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d) {
        rt_gc_mutator_exit();
        return NULL;
    }
    if (d->len == 0) {
        rt_gc_mutator_exit();
        trap_with_message("PopBack called on empty deque");
        return NULL;
    }

    int64_t back = (d->front + d->len - 1) % d->cap;
    void *val = d->data[back];
    d->data[back] = NULL;
    d->len--;
    // Move the deque's stored reference to the caller.
    rt_gc_mutator_exit();
    return val;
}

/// @brief Returns the back element without removing it.
/// @param obj Pointer to the Deque object. Must not be NULL.
/// @return The back element as a retained reference owned by the caller.
/// @note Traps if the deque is NULL or empty.
void *rt_deque_peek_back(void *obj) {
    if (!obj) {
        trap_with_message("PeekBack called on NULL deque");
        return NULL;
    }
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d) {
        rt_gc_mutator_exit();
        return NULL;
    }
    if (d->len == 0) {
        rt_gc_mutator_exit();
        trap_with_message("PeekBack called on empty deque");
        return NULL;
    }

    int64_t back = (d->front + d->len - 1) % d->cap;
    void *val = d->data[back];
    if (!rt_collection_retain_checked(val, "Deque.PeekBack: result retain failed")) {
        rt_gc_mutator_exit();
        return NULL;
    }
    rt_gc_mutator_exit();
    return val;
}

//=============================================================================
// Random Access
//=============================================================================

/// @brief Returns the element at the given logical index.
/// @param obj Pointer to the Deque object. Must not be NULL.
/// @param idx Zero-based index. Must be in range [0, len).
/// @return The element at the index as a retained reference owned by the caller.
/// @note Traps if obj is NULL or idx is out of bounds.
void *rt_deque_get(void *obj, int64_t idx) {
    if (!obj) {
        trap_with_message("Get called on NULL deque");
        return NULL;
    }
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d) {
        rt_gc_mutator_exit();
        return NULL;
    }
    if (idx < 0 || idx >= d->len) {
        rt_gc_mutator_exit();
        trap_with_message("Index out of bounds");
        return NULL;
    }

    int64_t actual = (d->front + idx) % d->cap;
    void *val = d->data[actual];
    if (!rt_collection_retain_checked(val, "Deque.Get: result retain failed")) {
        rt_gc_mutator_exit();
        return NULL;
    }
    rt_gc_mutator_exit();
    return val;
}

/// @brief Replaces the element at the given logical index.
/// @param obj Pointer to the Deque object. Must not be NULL.
/// @param idx Zero-based index. Must be in range [0, len).
/// @param elem New value to store. May be NULL.
/// @note Traps if obj is NULL or idx is out of bounds.
void rt_deque_set(void *obj, int64_t idx, void *elem) {
    if (!obj) {
        trap_with_message("Set called on NULL deque");
        return;
    }
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d) {
        rt_gc_mutator_exit();
        return;
    }
    if (idx < 0 || idx >= d->len) {
        rt_gc_mutator_exit();
        trap_with_message("Index out of bounds");
        return;
    }

    int64_t actual = (d->front + idx) % d->cap;
    if (!rt_collection_retain_checked(elem, "Deque.Set: value retain failed")) {
        rt_gc_mutator_exit();
        return;
    }
    void *old_elem = d->data[actual];
    d->data[actual] = elem;
    deque_release_value(old_elem);
    rt_gc_mutator_exit();
}

//=============================================================================
// Utility
//=============================================================================

/// @brief Removes all elements from the deque, resetting length to 0.
/// @param obj Pointer to the Deque object. No-op if NULL.
/// @note Does not shrink the backing buffer.
void rt_deque_clear(void *obj) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d) {
        rt_gc_mutator_exit();
        return;
    }
    for (int64_t i = 0; i < d->len; i++) {
        int64_t idx = (d->front + i) % d->cap;
        void *old_elem = d->data[idx];
        d->data[idx] = NULL;
        deque_release_value(old_elem);
    }
    d->len = 0;
    d->front = 0;
    rt_gc_mutator_exit();
}

/// @brief Checks whether the deque contains an equal runtime value.
/// @param obj Pointer to the Deque object, or NULL.
/// @param elem Value to compare with `rt_box_equal()`.
/// @return 1 if found, 0 otherwise. Returns 0 if obj is NULL.
/// @note O(n) linear scan.
int8_t rt_deque_has(void *obj, void *elem) {
    if (!obj)
        return 0;
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d)
        return 0;

    for (int64_t i = 0; i < d->len; i++) {
        int64_t idx = (d->front + i) % d->cap;
        if (rt_box_equal(d->data[idx], elem))
            return 1;
    }
    return 0;
}

/// @brief Reverses the elements of the deque in place.
/// @param obj Pointer to the Deque object. No-op if NULL or fewer than 2 elements.
/// @note O(n) where n is the length of the deque.
void rt_deque_reverse(void *obj) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d) {
        rt_gc_mutator_exit();
        return;
    }
    if (d->len < 2) {
        rt_gc_mutator_exit();
        return;
    }

    for (int64_t i = 0; i < d->len / 2; i++) {
        int64_t front_idx = (d->front + i) % d->cap;
        int64_t back_idx = (d->front + d->len - 1 - i) % d->cap;

        void *tmp = d->data[front_idx];
        d->data[front_idx] = d->data[back_idx];
        d->data[back_idx] = tmp;
    }
    rt_gc_mutator_exit();
}

/// @brief Creates a shallow copy of the deque.
/// @param obj Pointer to the Deque object, or NULL.
/// @return A new Deque containing the same elements in the same order.
///         Returns an empty deque if obj is NULL.
/// @note Elements are not deep-copied; both deques retain the same pointers.
void *rt_deque_clone(void *obj) {
    if (!obj)
        return rt_deque_new();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d)
        return NULL;

    void *volatile new_d = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        deque_save_trap(saved_error, sizeof(saved_error), "Deque.Clone: copy failed");
        rt_trap_clear_recovery();
        deque_release_value((void *)new_d);
        rt_trap(saved_error);
        return NULL;
    }

    new_d = rt_deque_with_capacity(d->cap);
    if (!new_d) {
        rt_trap_clear_recovery();
        return NULL;
    }

    for (int64_t i = 0; i < d->len; i++) {
        int64_t idx = (d->front + i) % d->cap;
        rt_deque_push_back((void *)new_d, d->data[idx]);
    }

    rt_trap_clear_recovery();
    return (void *)new_d;
}

/// @brief Pop the front element, or return NULL if empty (no trap).
/// @param obj Opaque Deque object pointer.
/// @return The removed element as a retained caller-owned reference, or NULL
///         if empty. A stored NULL is indistinguishable from emptiness.
void *rt_deque_try_pop_front(void *obj) {
    if (!obj)
        return NULL;
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d) {
        rt_gc_mutator_exit();
        return NULL;
    }
    if (d->len == 0) {
        rt_gc_mutator_exit();
        return NULL;
    }

    void *val = d->data[d->front];
    d->data[d->front] = NULL;
    d->front = (d->front + 1) % d->cap;
    d->len--;
    // Move the deque's stored reference to the caller.
    rt_gc_mutator_exit();
    return val;
}

/// @brief Pop the front element as an Option, preserving NULL as a present value.
/// @details Returns `None` only when the deque is empty. The Option is
///          constructed before the deque is mutated, so a failed retain leaves
///          the front element in place.
/// @param obj Opaque Deque object pointer.
/// @return `Some(value)` when an element is removed, otherwise `None`.
void *rt_deque_try_pop_front_option(void *obj) {
    if (!obj)
        return rt_option_none();
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d) {
        rt_gc_mutator_exit();
        return NULL;
    }
    if (d->len == 0) {
        rt_gc_mutator_exit();
        return rt_option_none();
    }

    void *value = d->data[d->front];
    void *option = rt_option_some(value);
    if (!option) {
        rt_gc_mutator_exit();
        return NULL;
    }
    d->data[d->front] = NULL;
    d->front = (d->front + 1) % d->cap;
    d->len--;
    deque_release_value(value);
    rt_gc_mutator_exit();
    return option;
}

/// @brief Pop the back element, or return NULL if empty (no trap).
/// @param obj Opaque Deque object pointer.
/// @return The removed element as a retained caller-owned reference, or NULL
///         if empty. A stored NULL is indistinguishable from emptiness.
void *rt_deque_try_pop_back(void *obj) {
    if (!obj)
        return NULL;
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d) {
        rt_gc_mutator_exit();
        return NULL;
    }
    if (d->len == 0) {
        rt_gc_mutator_exit();
        return NULL;
    }

    int64_t back = (d->front + d->len - 1) % d->cap;
    void *val = d->data[back];
    d->data[back] = NULL;
    d->len--;
    // Move the deque's stored reference to the caller.
    rt_gc_mutator_exit();
    return val;
}

/// @brief Pop the back element as an Option, preserving NULL as a present value.
/// @details Returns `None` only when the deque is empty. The Option is
///          constructed before the deque is mutated, so a failed retain leaves
///          the back element in place.
/// @param obj Opaque Deque object pointer.
/// @return `Some(value)` when an element is removed, otherwise `None`.
void *rt_deque_try_pop_back_option(void *obj) {
    if (!obj)
        return rt_option_none();
    rt_gc_mutator_enter();
    Deque *d = as_deque(obj, "Deque: invalid Deque object");
    if (!d) {
        rt_gc_mutator_exit();
        return NULL;
    }
    if (d->len == 0) {
        rt_gc_mutator_exit();
        return rt_option_none();
    }

    int64_t back = (d->front + d->len - 1) % d->cap;
    void *value = d->data[back];
    void *option = rt_option_some(value);
    if (!option) {
        rt_gc_mutator_exit();
        return NULL;
    }
    d->data[back] = NULL;
    d->len--;
    deque_release_value(value);
    rt_gc_mutator_exit();
    return option;
}
