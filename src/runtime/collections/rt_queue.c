//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_queue.c
// Purpose: Implements a FIFO (first-in-first-out) queue backed by a circular
//   buffer. Elements are added (enqueued) at the tail and removed (dequeued)
//   from the head. Both operations are O(1) amortized; the circular buffer
//   avoids element shifting on dequeue.
//
// Key invariants:
//   - Backed by a circular buffer with initial capacity QUEUE_DEFAULT_CAP (16).
//     Growth factor is QUEUE_GROWTH_FACTOR (2); elements are linearized into
//     the new array during resize.
//   - head is the index of the next element to dequeue (oldest element).
//   - tail is computed as (head + count) % capacity (next write position).
//   - When head == (head + count) % capacity the buffer is full and must grow.
//   - Dequeue on an empty queue traps with an error message.
//   - Peek returns the head element without removing it; Peek and Pop trap
//     on an empty queue (TryPop returns None instead).
//   - Owning queues retain pushed values and return retained transfers from
//     Pop; borrowing queues store raw pointers without retain/release.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - Queue objects are GC-managed (rt_obj_new_i64). The items array is
//     realloc-managed and freed by the GC finalizer (queue_finalizer).
//
// Links: src/runtime/collections/rt_queue.h (public API),
//        src/runtime/collections/rt_deque.h (double-ended queue variant)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime FIFO queue as a growable circular buffer.
/// @details Queues start in borrowing mode and may be switched to
///          retained-element ownership while empty. Both modes share FIFO
///          indexing and growth; ownership mode controls GC traversal,
///          retain/release work, and the lifetime of returned elements.

#include "rt_collection_ids.h"

#include "rt_box.h"
#include "rt_collection_ownership.h"
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_platform.h"
#include "rt_queue_internal.h"

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

/// @brief Number of pointer slots allocated for a new queue.
#define QUEUE_DEFAULT_CAP 16
/// @brief Multiplicative capacity increase applied to a full queue.
#define QUEUE_GROWTH_FACTOR 2

/// @brief Internal queue implementation structure (circular buffer).
///
/// The Queue is implemented as a circular buffer (ring buffer) for efficient
/// O(1) add and take operations. Elements are stored in a contiguous array,
/// with head and tail indices that wrap around when they reach the end.
///
/// **Circular buffer concept:**
/// Instead of shifting elements when removing from the front, we just move
/// the head pointer forward. When indices reach the end of the array, they
/// wrap around to the beginning (modulo arithmetic).
///
/// **Memory layout example (capacity=8, 4 elements):**
/// ```
/// Scenario 1 - Contiguous:
///   indices: [0] [1] [2] [3] [4] [5] [6] [7]
///   items:   [ ] [ ] [A] [B] [C] [D] [ ] [ ]
///                     ^           ^
///                   head=2     tail=6
///
/// Scenario 2 - Wrapped around:
///   indices: [0] [1] [2] [3] [4] [5] [6] [7]
///   items:   [C] [D] [ ] [ ] [ ] [ ] [A] [B]
///             ^                       ^
///           tail=2                  head=6
/// ```
///
/// The circular design means:
/// - Push (enqueue) at tail: O(1)
/// - Pop (dequeue) from head: O(1)
/// - No element shifting needed
/// @brief Checked cast of an opaque handle to the Queue implementation;
///        traps with @p what if @p obj is NULL or not a Queue.
/// @param obj Opaque runtime handle to validate.
/// @param what Diagnostic emitted by the trap subsystem on failure.
/// @return Validated queue implementation, or `NULL` after trapping.
static rt_queue_impl *as_queue(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_QUEUE_CLASS_ID, sizeof(rt_queue_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_queue_impl *)obj;
}

/// @brief Drop one GC reference to a stored element and free it at zero.
/// @param value Runtime object reference, or `NULL` for a no-op.
static void queue_release_value(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Save an active trap diagnostic before clearing its recovery frame.
/// @param buffer Destination for the bounded diagnostic copy.
/// @param buffer_size Capacity of @p buffer including its terminator.
/// @param fallback Message used when no active diagnostic is available.
static void queue_save_trap(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief Finalizer callback invoked when a Queue is garbage collected.
///
/// This function is automatically called by Zanna's garbage collector when a
/// Queue object becomes unreachable. In owning mode it releases every live
/// circular-buffer slot before freeing the internal items array.
///
/// @param obj Pointer to the Queue object being finalized. May be NULL (no-op).
///
/// @note Borrowing queues never retain or release their elements. Owning
///       queues release all still-enqueued values during finalization.
/// @note This function is idempotent - safe to call on already-finalized queues.
///
/// @see rt_queue_clear For removing elements without finalization
static void rt_queue_finalize(void *obj) {
    if (!obj)
        return;
    rt_queue_impl *q = as_queue(obj, "Queue: invalid Queue object");
    if (q->owns_elements && q->items && q->cap > 0) {
        int64_t len = q->len;
        q->len = 0;
        for (int64_t i = 0; i < len; i++) {
            int64_t idx = (q->head + i) % q->cap;
            void *value = q->items[idx];
            q->items[idx] = NULL;
            queue_release_value(value);
        }
    }
    free(q->items);
    q->items = NULL;
    q->len = 0;
    q->cap = 0;
    q->head = 0;
    q->tail = 0;
}

/// @brief GC traversal: visit every live element in the circular buffer
///        (only when the queue owns its elements).
/// @param obj Queue whose live slots are to be traced.
/// @param visitor Collector callback invoked in front-to-back order.
/// @param ctx Opaque collector context forwarded unchanged.
static void rt_queue_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_queue_impl *q = as_queue(obj, "Queue: invalid Queue object");
    if (!q->owns_elements || !q->items || q->cap <= 0)
        return;
    for (int64_t i = 0; i < q->len; i++)
        visitor(q->items[(q->head + i) % q->cap], ctx);
}

/// @brief Grows the queue capacity and linearizes the circular buffer.
///
/// When the queue is full and a new element needs to be added, this function:
/// 1. Allocates a new array with double the capacity
/// 2. Copies elements from the circular buffer to the new array in linear order
/// 3. Resets head to 0 and tail to len
///
/// **Linearization process:**
/// The circular buffer may have elements wrapped around. During growth, we
/// "unwrap" them into a contiguous linear array:
/// ```
/// Before (wrapped):     [C] [D] [ ] [ ] [A] [B]   head=4, tail=2
/// After (linearized):   [A] [B] [C] [D] [ ] [ ] [ ] [ ]  head=0, tail=4
/// ```
///
/// @param q Pointer to the queue implementation. Must not be NULL.
/// @return 1 after publishing the doubled linear allocation, or 0 after
///         trapping while leaving the old allocation intact.
///
/// @note Capacity doubles each time (QUEUE_GROWTH_FACTOR = 2).
/// @note Traps on memory allocation failure.
/// @note O(n) time complexity where n is the number of elements.
static int queue_grow(rt_queue_impl *q) {
    if (q->cap > INT64_MAX / QUEUE_GROWTH_FACTOR) {
        rt_trap("Queue: capacity overflow");
        return 0;
    }
    int64_t new_cap = q->cap * QUEUE_GROWTH_FACTOR;
    if ((uint64_t)new_cap > SIZE_MAX / sizeof(void *)) {
        rt_trap("Queue: allocation size overflow");
        return 0;
    }
    void **new_items = malloc((size_t)new_cap * sizeof(void *));

    if (!new_items) {
        rt_trap("Queue: memory allocation failed");
        return 0;
    }

    // Linearize the circular buffer into the new array
    if (q->len > 0) {
        if (q->head < q->tail) {
            // Contiguous region: head...tail
            memcpy(new_items, &q->items[q->head], (size_t)q->len * sizeof(void *));
        } else {
            // Wrapped around: head...end, then start...tail
            int64_t first_part = q->cap - q->head;
            memcpy(new_items, &q->items[q->head], (size_t)first_part * sizeof(void *));
            memcpy(&new_items[first_part], q->items, (size_t)q->tail * sizeof(void *));
        }
    }

    free(q->items);
    q->items = new_items;
    q->head = 0;
    q->tail = q->len;
    q->cap = new_cap;
    return 1;
}

/// @brief Creates a new empty Queue with default capacity.
///
/// Allocates and initializes a Queue data structure for FIFO (First-In-First-Out)
/// operations. The Queue starts with a default capacity of 16 slots and grows
/// automatically when needed.
///
/// The Queue is implemented as a circular buffer, providing O(1) add and take
/// operations without element shifting.
///
/// **Usage example:**
/// ```
/// Dim queue = Queue.New()
/// queue.Push("first")
/// queue.Push("second")
/// queue.Push("third")
/// PRINT queue.Pop()   ' Outputs: first
/// Print queue.Take()   ' Outputs: second
/// Print queue.Take()   ' Outputs: third
/// ```
///
/// @return A pointer to the newly created Queue object. Traps and does not
///         return if memory allocation fails.
///
/// @note Initial capacity is 16 elements (QUEUE_DEFAULT_CAP).
/// @note A new Queue starts in borrowing mode; call
///       @ref rt_queue_set_owns_elements while it is empty to enable retained
///       ownership.
/// @note Thread safety: Not thread-safe. External synchronization required.
///
/// @see rt_queue_push For adding elements
/// @see rt_queue_pop For removing elements
/// @see rt_queue_finalize For cleanup behavior
void *rt_queue_new(void) {
    rt_queue_impl *q =
        (rt_queue_impl *)rt_obj_new_i64(RT_QUEUE_CLASS_ID, (int64_t)sizeof(rt_queue_impl));
    if (!q) {
        rt_trap("Queue: memory allocation failed");
        return NULL;
    }

    q->len = 0;
    q->cap = QUEUE_DEFAULT_CAP;
    q->head = 0;
    q->tail = 0;
    q->owns_elements = 0;
    q->items = malloc((size_t)QUEUE_DEFAULT_CAP * sizeof(void *));
    rt_obj_set_finalizer(q, rt_queue_finalize);
    rt_gc_track(q, rt_queue_traverse);

    if (!q->items) {
        if (rt_obj_release_check0(q))
            rt_obj_free(q);
        rt_trap("Queue: memory allocation failed");
        return NULL;
    }

    return q;
}

/// @brief Select borrowing or retained-element ownership for an empty queue.
/// @details Ownership cannot change while any value is enqueued because doing
///          so would retroactively alter retain and traversal obligations. A
///          null queue is a no-op.
/// @param obj Queue to configure, or `NULL`.
/// @param owns Nonzero to retain/trace/release elements; zero to borrow them.
void rt_queue_set_owns_elements(void *obj, int8_t owns) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    rt_queue_impl *q = as_queue(obj, "Queue: invalid Queue object");
    if (!q) {
        rt_gc_mutator_exit();
        return;
    }
    owns = owns ? 1 : 0;
    if (q->len != 0 && q->owns_elements != owns) {
        rt_gc_mutator_exit();
        rt_trap("Queue.SetOwnsElements: cannot change ownership mode on non-empty queue");
        return;
    }
    q->owns_elements = owns;
    rt_gc_mutator_exit();
}

/// @brief Report whether a queue retains and traces its elements.
/// @param obj Queue handle, or `NULL`.
/// @return 1 for retained-element ownership, otherwise 0; `NULL` reports 0.
int8_t rt_queue_owns_elements(void *obj) {
    if (!obj)
        return 0;
    rt_queue_impl *queue = as_queue(obj, "Queue: invalid Queue object");
    return queue && queue->owns_elements ? 1 : 0;
}

/// @brief Returns the number of elements currently in the Queue.
///
/// This function returns how many elements have been added and not yet taken.
/// The count is maintained internally and returned in O(1) time.
///
/// @param obj Pointer to a Queue object. If NULL, returns 0.
///
/// @return The number of elements in the Queue (>= 0). Returns 0 if obj is NULL.
///
/// @note O(1) time complexity.
///
/// @see rt_queue_is_empty For a boolean check
/// @see rt_queue_push For operations that increase the count
/// @see rt_queue_pop For operations that decrease the count
int64_t rt_queue_len(void *obj) {
    if (!obj)
        return 0;
    rt_queue_impl *queue = as_queue(obj, "Queue: invalid Queue object");
    return queue ? queue->len : 0;
}

/// @brief Checks whether the Queue contains no elements.
///
/// A Queue is considered empty when its length is 0, which occurs:
/// - Immediately after creation
/// - After all elements have been taken
/// - After calling rt_queue_clear
///
/// Calling Pop or Peek on an empty Queue will trap with an error.
///
/// @param obj Pointer to a Queue object. If NULL, returns true (1).
///
/// @return 1 (true) if the Queue is empty or obj is NULL, 0 (false) otherwise.
///
/// @note O(1) time complexity.
///
/// @see rt_queue_len For the exact count
/// @see rt_queue_pop For removing elements (traps if empty)
/// @see rt_queue_peek For viewing front element (traps if empty)
int8_t rt_queue_is_empty(void *obj) {
    if (!obj)
        return 1;
    rt_queue_impl *queue = as_queue(obj, "Queue: invalid Queue object");
    return !queue || queue->len == 0 ? 1 : 0;
}

/// @brief Adds an element to the back of the Queue.
///
/// Enqueues a new element at the tail of the Queue. This is the primary
/// insertion operation for FIFO behavior - elements are added at the back
/// and removed from the front.
///
/// If the Queue's capacity is exceeded, it automatically grows (doubles)
/// to accommodate the new element.
///
/// **Visual example:**
/// ```
/// Before Add(D):  front->[A, B, C]<-back
/// After Add(D):   front->[A, B, C, D]<-back
/// ```
///
/// @param obj Pointer to a Queue object. Must not be NULL.
/// @param elem The element to add. May be NULL (NULL is a valid element).
///
/// @note O(1) amortized time complexity. Occasional O(n) when resizing occurs.
/// @note Borrowing queues store @p elem without retaining it. Owning queues
///       retain non-null elements until pop, clear, or finalization.
/// @note Traps with "Queue.Add: null queue" if obj is NULL.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_queue_pop For the removal operation
/// @see rt_queue_peek For viewing without removing
void rt_queue_push(void *obj, void *elem) {
    if (!obj) {
        rt_trap("Queue.Add: null queue");
        return;
    }

    rt_gc_mutator_enter();
    rt_queue_impl *q = as_queue(obj, "Queue: invalid Queue object");
    if (!q) {
        rt_gc_mutator_exit();
        return;
    }

    if (q->len >= INT64_MAX) {
        rt_gc_mutator_exit();
        rt_trap("Queue: maximum length reached");
        return;
    }
    if (q->len >= q->cap) {
        if (!queue_grow(q)) {
            rt_gc_mutator_exit();
            return;
        }
    }

    if (q->owns_elements &&
        !rt_collection_retain_checked(elem, "Queue.Push: value retain failed")) {
        rt_gc_mutator_exit();
        return;
    }
    q->items[q->tail] = elem;
    q->tail = (q->tail + 1) % q->cap;
    q->len++;
    rt_gc_mutator_exit();
}

/// @brief Removes and returns the front element from the Queue.
///
/// Dequeues the element at the front of the Queue (the oldest element).
/// This is the primary retrieval operation for FIFO behavior.
///
/// **Visual example:**
/// ```
/// Before Take():  front->[A, B, C, D]<-back
/// After Take():   front->[B, C, D]<-back
/// Returns: A
/// ```
///
/// **Error handling:**
/// Calling Take on an empty Queue is a programming error and traps with
/// "Queue.Take: queue is empty". Always check rt_queue_is_empty before
/// taking, or use a try-catch pattern if available.
///
/// @param obj Pointer to a Queue object. Must not be NULL.
///
/// @return The element that was at the front of the Queue.
///
/// @note O(1) time complexity.
/// @note For an owning queue, the returned value carries a caller-owned retain.
///       For a borrowing queue, it is the same raw borrowed pointer that was
///       pushed and receives no lifetime extension.
/// @note Traps if the Queue is empty or obj is NULL.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_queue_push For the insertion operation
/// @see rt_queue_peek For viewing without removing
/// @see rt_queue_is_empty For checking before take
void *rt_queue_pop(void *obj) {
    if (!obj) {
        rt_trap("Queue.Take: null queue");
        return NULL;
    }

    rt_gc_mutator_enter();
    rt_queue_impl *q = as_queue(obj, "Queue: invalid Queue object");
    if (!q) {
        rt_gc_mutator_exit();
        return NULL;
    }

    if (q->len == 0) {
        rt_gc_mutator_exit();
        rt_trap("Queue.Take: queue is empty");
        return NULL;
    }

    void *val = q->items[q->head];
    q->items[q->head] = NULL;
    q->head = (q->head + 1) % q->cap;
    q->len--;
    // Owning queues move their stored reference to the caller. Borrowing
    // queues continue to return the raw pointer without changing ownership.

    rt_gc_mutator_exit();
    return val;
}

/// @brief Returns the front element without removing it from the Queue.
///
/// Peeks at the element at the front of the Queue (the next one to be taken)
/// without modifying the Queue. Useful for:
/// - Inspecting the next element before deciding to take it
/// - Implementing conditional dequeue logic
/// - Debugging or logging
///
/// **Example:**
/// ```
/// queue.Add("A")
/// queue.Add("B")
/// Print queue.Peek()  ' Outputs: A
/// Print queue.Peek()  ' Outputs: A (still there)
/// Print queue.Take()  ' Outputs: A (now removed)
/// Print queue.Peek()  ' Outputs: B
/// ```
///
/// @param obj Pointer to a Queue object. Must not be NULL.
///
/// @return The element at the front of the Queue (not removed).
///
/// @note O(1) time complexity.
/// @note No additional retain is created in either mode. In an owning queue,
///       the queue's stored retain keeps the pointer valid only while the
///       element remains enqueued; in borrowing mode the producer controls
///       lifetime throughout.
/// @note Traps if the Queue is empty or obj is NULL.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_queue_pop For removing while retrieving
/// @see rt_queue_is_empty For checking before peek
void *rt_queue_peek(void *obj) {
    if (!obj) {
        rt_trap("Queue.Peek: null queue");
        return NULL;
    }

    rt_queue_impl *q = as_queue(obj, "Queue: invalid Queue object");
    if (!q)
        return NULL;

    if (q->len == 0) {
        rt_trap("Queue.Peek: queue is empty");
        return NULL;
    }

    return q->items[q->head];
}

/// @brief Removes all elements from the Queue.
///
/// Clears the Queue by resetting its length, head, and tail to 0.
/// The capacity remains unchanged (no memory is freed), allowing the
/// Queue to be efficiently reused.
///
/// **After clear:**
/// - Length becomes 0
/// - Head and tail reset to 0
/// - is_empty returns true
/// - Capacity unchanged (no reallocation)
/// - Owning queues release all live elements; borrowing queues forget pointers
///
/// @param obj Pointer to a Queue object. If NULL, this is a no-op.
///
/// @note O(1) for borrowing queues and O(n) for owning queues because owned
///       references must be released.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_queue_finalize For complete cleanup
/// @see rt_queue_is_empty For checking if empty
void rt_queue_clear(void *obj) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    rt_queue_impl *q = as_queue(obj, "Queue: invalid Queue object");
    if (!q) {
        rt_gc_mutator_exit();
        return;
    }
    if (q->owns_elements && q->items && q->cap > 0) {
        int64_t len = q->len;
        q->len = 0;
        for (int64_t i = 0; i < len; i++) {
            int64_t idx = (q->head + i) % q->cap;
            void *value = q->items[idx];
            q->items[idx] = NULL;
            queue_release_value(value);
        }
    }
    q->len = 0;
    q->head = 0;
    q->tail = 0;
    rt_gc_mutator_exit();
}

/// @brief Check whether any queued element equals @p elem.
/// @details Scans front to back with `rt_box_equal`: boxed numeric/string
///          values compare by content and ordinary object handles by identity.
///          A null queue reports absence.
/// @param obj Queue handle, or `NULL`.
/// @param elem Value to compare; may be `NULL`.
/// @return 1 if found, 0 otherwise.
int8_t rt_queue_has(void *obj, void *elem) {
    if (!obj)
        return 0;

    rt_queue_impl *q = as_queue(obj, "Queue: invalid Queue object");
    if (!q)
        return 0;
    for (int64_t i = 0; i < q->len; i++) {
        if (rt_box_equal(q->items[(q->head + i) % q->cap], elem))
            return 1;
    }
    return 0;
}

/// @brief Pop the front element, or return NULL if empty (no trap).
/// @details Result ownership matches @ref rt_queue_pop: owning queues return a
///          caller-retained value, borrowing queues return the raw stored
///          pointer. A stored `NULL` is ambiguous with an empty queue.
/// @param obj Queue handle, or `NULL`.
/// @return Removed front value, or `NULL` if empty/null/null-valued.
void *rt_queue_try_pop(void *obj) {
    if (!obj)
        return NULL;

    rt_gc_mutator_enter();
    rt_queue_impl *q = as_queue(obj, "Queue: invalid Queue object");
    if (!q || q->len == 0) {
        rt_gc_mutator_exit();
        return NULL;
    }

    void *val = q->items[q->head];
    q->items[q->head] = NULL;
    q->head = (q->head + 1) % q->cap;
    q->len--;
    // Transfer the owning queue's stored reference; borrowing mode remains a
    // raw-pointer removal.
    rt_gc_mutator_exit();
    return val;
}

/// @brief Pop the front element as an Option, preserving NULL as a present value.
/// @details This is the explicit optional variant of @ref rt_queue_try_pop. It
///          returns `None` only when the queue has no element to pop; if the
///          queue contains a literal NULL value, the result is `Some(NULL)`.
///          The Option is constructed before the queue is mutated, so a failed
///          retain leaves the queued element in place.
/// @param obj Queue handle, or `NULL`.
/// @return New runtime-managed `Some(value)` when an element is removed,
///         otherwise `None`.
void *rt_queue_try_pop_option(void *obj) {
    if (!obj)
        return rt_option_none();

    rt_gc_mutator_enter();
    rt_queue_impl *q = as_queue(obj, "Queue: invalid Queue object");
    if (!q) {
        rt_gc_mutator_exit();
        return NULL;
    }
    if (q->len == 0) {
        rt_gc_mutator_exit();
        return rt_option_none();
    }

    void *value = q->items[q->head];
    void *option = rt_option_some(value);
    if (!option) {
        rt_gc_mutator_exit();
        return NULL;
    }

    q->items[q->head] = NULL;
    q->head = (q->head + 1) % q->cap;
    q->len--;
    if (q->owns_elements)
        queue_release_value(value);
    rt_gc_mutator_exit();
    return option;
}

/// @brief Create a shallow copy of the queue.
///
/// Allocates a new Queue and pushes all elements from the source in
/// front-to-back order, preserving the original queue ordering.
/// The clone preserves ownership mode: an owning clone independently retains
/// every value, while a borrowing clone copies only raw pointers.
///
/// @param obj Source Queue handle, or `NULL`.
/// @return New runtime-managed Queue with the same mode/order, or an empty
///         borrowing queue for `NULL`.
void *rt_queue_clone(void *obj) {
    if (!obj)
        return rt_queue_new();

    rt_queue_impl *q = as_queue(obj, "Queue: invalid Queue object");
    if (!q)
        return NULL;

    void *volatile result = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        queue_save_trap(saved_error, sizeof(saved_error), "Queue.Clone: copy failed");
        rt_trap_clear_recovery();
        queue_release_value((void *)result);
        rt_trap(saved_error);
        return NULL;
    }

    result = rt_queue_new();
    if (!result) {
        rt_trap_clear_recovery();
        return NULL;
    }
    if (q->owns_elements)
        rt_queue_set_owns_elements((void *)result, 1);
    for (int64_t i = 0; i < q->len; i++) {
        rt_queue_push((void *)result, q->items[(q->head + i) % q->cap]);
    }
    rt_trap_clear_recovery();
    return (void *)result;
}
