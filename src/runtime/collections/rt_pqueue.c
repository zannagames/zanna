//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_pqueue.c
// Purpose: Implements a priority queue (min-heap or max-heap) backed by a
//   dynamic array of (priority, value) pairs. The heap property ensures the
//   element with the extreme priority (smallest for min-heap, largest for
//   max-heap) is always at the root and can be peeked or dequeued in O(log n).
//
// Key invariants:
//   - Binary heap stored in a flat array: parent at index i has children at
//     indices 2*i+1 (left) and 2*i+2 (right).
//   - For min-heap: parent.priority <= child.priority (root = minimum).
//     For max-heap: parent.priority >= child.priority (root = maximum).
//   - Initial capacity is HEAP_DEFAULT_CAP (16); grows by HEAP_GROWTH_FACTOR (2).
//   - Each element is a heap_entry { int64_t priority; void* value }.
//     The value pointer is retained while stored in the heap and released on
//     clear/finalize; Pop transfers the heap-owned reference to the caller.
//   - Enqueue is O(log n) via sift-up; dequeue is O(log n) via sift-down.
//   - Peek is O(1); returns NULL if the heap is empty.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - PQueue objects are GC-managed (rt_obj_new_i64). The entries array is
//     realloc-managed and freed by the GC finalizer (heap_finalizer).
//
// Links: src/runtime/collections/rt_pqueue.h (public API)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime integer-priority binary heap.
/// @details The same packed `(priority, value)` representation supports
///          min-heap and max-heap ordering. Values are retained while queued;
///          destructive retrieval transfers that retain to the caller, while
///          non-destructive retrieval creates an additional retain.

#include "rt_pqueue.h"
#include "rt_collection_ids.h"
#include "rt_collection_ownership.h"
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_platform.h"
#include "rt_seq.h"

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

/// @brief Number of entries allocated for a new priority queue.
#define HEAP_DEFAULT_CAP 16
/// @brief Multiplicative capacity increase applied when the heap is full.
#define HEAP_GROWTH_FACTOR 2

/// @brief A single entry in the heap containing priority and value.
/// @details Equal priorities are legal. The heap does not record an insertion
///          sequence number, so their relative removal order is unspecified.
typedef struct heap_entry {
    int64_t priority; ///< Priority value (lower = higher priority for min-heap)
    void *value;      ///< The stored object
} heap_entry;

/// @brief Internal heap implementation structure.
///
/// The Heap is implemented as a binary heap stored in a dynamic array.
/// For a min-heap, the smallest priority value is at the root (index 0).
/// For a max-heap, the largest priority value is at the root.
///
/// **Binary heap property:**
/// - Parent at index i has children at indices 2*i+1 and 2*i+2
/// - Each parent has priority >= (max-heap) or <= (min-heap) its children
///
/// **Memory layout example (min-heap with 5 elements):**
/// ```
/// [0]
/// +-- [1]
/// |   +-- [3]
/// |   +-- [4]
/// +-- [2]
///
/// Array: [(1,A), (3,B), (2,C), (5,D), (4,E)]
///         ^root
/// ```
typedef struct rt_pqueue_impl {
    int64_t len;       ///< Number of elements currently in the heap
    int64_t cap;       ///< Current capacity (allocated slots)
    int8_t is_max;     ///< 1 for max-heap, 0 for min-heap
    heap_entry *items; ///< Array of (priority, value) entries
} rt_pqueue_impl;

/// @brief Checked cast of an opaque handle to the priority-queue impl;
///        traps with @p what if @p obj is NULL or not a PriorityQueue/Heap.
/// @param obj Opaque runtime handle to validate.
/// @param what Diagnostic emitted by the trap subsystem on failure.
/// @return Validated heap implementation, or `NULL` after trapping.
static rt_pqueue_impl *as_pqueue(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_PQUEUE_CLASS_ID, sizeof(rt_pqueue_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_pqueue_impl *)obj;
}

/// @brief Drop one GC reference to a heap element and free it at zero.
/// @param value Runtime object reference, or `NULL` for a no-op.
static void heap_release_value(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Save an active trap diagnostic before clearing its recovery frame.
/// @param buffer Destination for the bounded diagnostic copy.
/// @param buffer_size Capacity of @p buffer including its terminator.
/// @param fallback Message used when no active diagnostic is available.
static void heap_save_trap(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief GC traversal: visit every value currently stored in the heap array.
/// @param obj Priority queue whose stored values are to be traced.
/// @param visitor Collector callback invoked for each occupied entry.
/// @param ctx Opaque collector context forwarded unchanged.
static void rt_pqueue_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    if (!h)
        return;
    if (!h->items)
        return;
    for (int64_t i = 0; i < h->len; i++)
        visitor(h->items[i].value, ctx);
}

/// @brief Finalizer callback invoked when a Heap is garbage collected.
/// @details Releases all still-queued values, clears occupied slots, and frees
///          the native entry allocation.
/// @param obj Priority queue being finalized; `NULL` is ignored.
static void rt_pqueue_finalize(void *obj) {
    if (!obj)
        return;
    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    if (!h)
        return;
    int64_t len = h->len;
    h->len = 0;
    for (int64_t i = 0; i < len; i++) {
        void *value = h->items[i].value;
        h->items[i].value = NULL;
        heap_release_value(value);
    }
    free(h->items);
    h->items = NULL;
    h->cap = 0;
}

/// @brief Grows the heap capacity.
/// @details Allocates and copies into a new array before freeing the old one,
///          leaving the original storage unchanged on allocation failure.
/// @param h Full heap whose positive capacity is to be doubled.
/// @return 1 after publishing the larger allocation, or 0 after trapping.
static int heap_grow(rt_pqueue_impl *h) {
    if (h->cap > INT64_MAX / HEAP_GROWTH_FACTOR) {
        rt_trap("Heap: capacity overflow");
        return 0;
    }
    int64_t new_cap = h->cap * HEAP_GROWTH_FACTOR;
    if ((uint64_t)new_cap > SIZE_MAX / sizeof(heap_entry)) {
        rt_trap("Heap: allocation size overflow");
        return 0;
    }
    heap_entry *new_items = malloc((size_t)new_cap * sizeof(heap_entry));

    if (!new_items) {
        rt_trap("Heap: memory allocation failed");
        return 0;
    }
    if (h->len > new_cap) {
        free(new_items);
        rt_trap("Heap: corrupted length exceeds capacity");
        return 0;
    }

    if (h->len > 0) {
        memcpy(new_items, h->items, (size_t)h->len * sizeof(heap_entry));
    }

    free(h->items);
    h->items = new_items;
    h->cap = new_cap;
    return 1;
}

/// @brief Compare two priorities based on heap type.
/// @details Uses strict comparison: equal priorities are equivalent and
///          therefore receive no stable FIFO/LIFO guarantee.
/// @param h Heap whose min/max mode selects the comparison direction.
/// @param a Candidate priority.
/// @param b Reference priority.
/// @return Nonzero when @p a belongs closer to the root than @p b.
static inline int heap_compare(rt_pqueue_impl *h, int64_t a, int64_t b) {
    if (h->is_max)
        return a > b; // Max-heap: larger values go up
    else
        return a < b; // Min-heap: smaller values go up
}

/// @brief Swap two entries in the heap.
/// @param items Heap entry allocation.
/// @param i First valid array index.
/// @param j Second valid array index.
static inline void heap_swap(heap_entry *items, int64_t i, int64_t j) {
    heap_entry tmp = items[i];
    items[i] = items[j];
    items[j] = tmp;
}

/// @brief Restore heap property by moving an element up.
/// @details Repeatedly exchanges @p k with its parent while the child's
///          priority is more extreme for the configured min/max mode.
/// @param h Heap containing the newly appended entry.
/// @param k Valid index from which to begin the sift-up.
static void heap_swim(rt_pqueue_impl *h, int64_t k) {
    while (k > 0) {
        int64_t parent = (k - 1) / 2;
        if (!heap_compare(h, h->items[k].priority, h->items[parent].priority))
            break;
        heap_swap(h->items, k, parent);
        k = parent;
    }
}

/// @brief Restore heap property by moving an element down.
/// @details Selects the more extreme child at each level, exchanging until the
///          parent dominates both children. Arithmetic is guarded by the loop
///          bound so child-index calculation cannot overflow.
/// @param h Heap whose root/replacement may violate ordering.
/// @param k Valid index from which to begin the sift-down.
static void heap_sink(rt_pqueue_impl *h, int64_t k) {
    while (h->len >= 2 && k <= (h->len - 2) / 2) {
        int64_t child = k * 2 + 1; // Left child. Safe because of the loop guard.
        // Pick the child with higher priority
        if (child + 1 < h->len &&
            heap_compare(h, h->items[child + 1].priority, h->items[child].priority)) {
            child++; // Right child has higher priority
        }
        // If parent already has higher priority, stop
        if (!heap_compare(h, h->items[child].priority, h->items[k].priority))
            break;
        heap_swap(h->items, k, child);
        k = child;
    }
}

/// @brief Construct a min-heap priority queue (smallest priority extracted first). Default
/// behavior — call `_new_max` for a max-heap.
/// @return New runtime-managed empty min-heap.
void *rt_pqueue_new(void) {
    return rt_pqueue_new_max(0); // Default to min-heap
}

/// @brief Construct a priority queue. `is_max=1` makes it a max-heap (largest priority first);
/// `is_max=0` makes it a min-heap. Internal storage is a binary heap on a dynamic array.
/// @param is_max Nonzero for maximum-first order; zero for minimum-first.
/// @return New runtime-managed empty heap, or `NULL` after an allocation trap.
void *rt_pqueue_new_max(int8_t is_max) {
    rt_pqueue_impl *h =
        (rt_pqueue_impl *)rt_obj_new_i64(RT_PQUEUE_CLASS_ID, (int64_t)sizeof(rt_pqueue_impl));
    if (!h) {
        rt_trap("Heap: memory allocation failed");
        return NULL;
    }

    h->len = 0;
    h->cap = HEAP_DEFAULT_CAP;
    h->is_max = is_max ? 1 : 0;
    h->items = malloc((size_t)HEAP_DEFAULT_CAP * sizeof(heap_entry));
    rt_obj_set_finalizer(h, rt_pqueue_finalize);
    rt_gc_track(h, rt_pqueue_traverse);

    if (!h->items) {
        if (rt_obj_release_check0(h))
            rt_obj_free(h);
        rt_trap("Heap: memory allocation failed");
        return NULL;
    }

    return h;
}

/// @brief Number of items currently in the queue.
/// @details A null handle is treated as an empty heap; an invalid non-null
///          handle traps.
/// @param obj Opaque priority-queue handle, or `NULL`.
/// @return Occupied entry count, or 0 for `NULL`.
int64_t rt_pqueue_len(void *obj) {
    if (!obj)
        return 0;
    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    return h ? h->len : 0;
}

/// @brief Returns 1 if the queue has no items.
/// @param obj Opaque priority-queue handle, or `NULL`.
/// @return 1 when empty, otherwise 0; `NULL` is empty.
int8_t rt_pqueue_is_empty(void *obj) {
    if (!obj)
        return 1;
    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    return !h || h->len == 0 ? 1 : 0;
}

/// @brief Returns 1 if the queue is a max-heap, 0 if min-heap.
/// @param obj Opaque priority-queue handle, or `NULL`.
/// @return 1 for maximum-first order and 0 for minimum-first order; `NULL`
///         reports 0.
int8_t rt_pqueue_is_max(void *obj) {
    if (!obj)
        return 0;
    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    return h ? h->is_max : 0;
}

/// @brief Insert `val` with the given `priority`. O(log n) — sift-up to restore heap order.
/// Auto-grows internal storage when capacity is reached.
/// @details The queue retains @p val before publication. Equal priorities are
///          accepted but do not have stable removal order. Null handles trap.
/// @param obj Priority queue to mutate.
/// @param priority Signed ordering key.
/// @param val Runtime value to retain; `NULL` is a valid queued value.
void rt_pqueue_push(void *obj, int64_t priority, void *val) {
    if (!obj) {
        rt_trap("Heap.Push: null heap");
        return;
    }

    rt_gc_mutator_enter();
    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    if (!h) {
        rt_gc_mutator_exit();
        return;
    }

    if (h->len >= INT64_MAX) {
        rt_gc_mutator_exit();
        rt_trap("Heap: maximum length reached");
        return;
    }
    if (h->len >= h->cap) {
        if (!heap_grow(h)) {
            rt_gc_mutator_exit();
            return;
        }
    }

    if (!rt_collection_retain_checked(val, "Heap.Push: value retain failed")) {
        rt_gc_mutator_exit();
        return;
    }
    // Add at the end
    h->items[h->len].priority = priority;
    h->items[h->len].value = val;
    h->len++;

    // Restore heap property
    heap_swim(h, h->len - 1);
    rt_gc_mutator_exit();
}

/// @brief Remove and return the highest-priority item. O(log n) — replace root with last
/// element then sift-down. Traps if the queue is empty (use `_try_pop` for safe variant).
/// @details "Highest" means the smallest integer in a min-heap and the largest
///          in a max-heap. The queue-owned retain is transferred to the caller;
///          no additional retain or release occurs.
/// @param obj Non-null priority queue.
/// @return Caller-owned transferred value; may be `NULL` when a null value was
///         queued.
void *rt_pqueue_pop(void *obj) {
    if (!obj) {
        rt_trap("Heap.Pop: null heap");
        return NULL;
    }

    rt_gc_mutator_enter();
    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    if (!h) {
        rt_gc_mutator_exit();
        return NULL;
    }

    if (h->len == 0) {
        rt_gc_mutator_exit();
        rt_trap("Heap.Pop: heap is empty");
        return NULL;
    }

    void *val = h->items[0].value;

    // Move last element to root and shrink
    h->len--;
    if (h->len > 0) {
        h->items[0] = h->items[h->len];
        heap_sink(h, 0);
    }
    h->items[h->len].value = NULL;

    rt_gc_mutator_exit();
    return val;
}

/// @brief Look at the highest-priority item without removing it. Traps on empty queue.
/// @details The stored association remains unchanged and the returned runtime
///          value is retained for the caller.
/// @param obj Non-null, non-empty priority queue.
/// @return Caller-owned retained top value; may be a stored `NULL`.
void *rt_pqueue_peek(void *obj) {
    if (!obj) {
        rt_trap("Heap.Peek: null heap");
        return NULL;
    }

    rt_gc_mutator_enter();
    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    if (!h) {
        rt_gc_mutator_exit();
        return NULL;
    }

    if (h->len == 0) {
        rt_gc_mutator_exit();
        rt_trap("Heap.Peek: heap is empty");
        return NULL;
    }

    void *val = h->items[0].value;
    if (!rt_collection_retain_checked(val, "Heap.Peek: result retain failed")) {
        rt_gc_mutator_exit();
        return NULL;
    }
    rt_gc_mutator_exit();
    return val;
}

/// @brief Like `_pop` but returns NULL on an empty queue instead of trapping.
/// @details On success transfers the heap-owned reference exactly like
///          @ref rt_pqueue_pop. Since `NULL` is a valid queued value, use the
///          Option variant when absence must be distinguished.
/// @param obj Priority queue, or `NULL`.
/// @return Caller-owned transferred top value, or `NULL` for empty/null queue
///         or a stored null value.
void *rt_pqueue_try_pop(void *obj) {
    if (!obj)
        return NULL;

    rt_gc_mutator_enter();
    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    if (!h) {
        rt_gc_mutator_exit();
        return NULL;
    }

    if (h->len == 0) {
        rt_gc_mutator_exit();
        return NULL;
    }

    void *val = h->items[0].value;

    h->len--;
    if (h->len > 0) {
        h->items[0] = h->items[h->len];
        heap_sink(h, 0);
    }
    h->items[h->len].value = NULL;

    rt_gc_mutator_exit();
    return val;
}

/// @brief Remove the highest-priority item as an Option.
/// @details Returns `None` when the heap is empty and `Some(value)` otherwise.
///          The Option is constructed before the heap is mutated, so a failed
///          retain leaves the highest-priority item in place.
/// @param obj Opaque Heap object pointer.
/// @return New runtime-managed `Some(value)` when an item is removed,
///         otherwise `None`.
void *rt_pqueue_try_pop_option(void *obj) {
    if (!obj)
        return rt_option_none();

    rt_gc_mutator_enter();
    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    if (!h) {
        rt_gc_mutator_exit();
        return NULL;
    }
    if (h->len == 0) {
        rt_gc_mutator_exit();
        return rt_option_none();
    }

    void *value = h->items[0].value;
    void *option = rt_option_some(value);
    if (!option) {
        rt_gc_mutator_exit();
        return NULL;
    }

    h->len--;
    if (h->len > 0) {
        h->items[0] = h->items[h->len];
        heap_sink(h, 0);
    }
    h->items[h->len].value = NULL;
    heap_release_value(value);
    rt_gc_mutator_exit();
    return option;
}

/// @brief Like `_peek` but returns NULL on an empty queue instead of trapping.
/// @details Retains the returned value without removing it. Since `NULL` is a
///          valid queued value, use the Option variant when absence must be
///          distinguished.
/// @param obj Priority queue, or `NULL`.
/// @return Caller-owned retained top value, or `NULL` for empty/null queue or
///         a stored null value.
void *rt_pqueue_try_peek(void *obj) {
    if (!obj)
        return NULL;

    rt_gc_mutator_enter();
    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    if (!h) {
        rt_gc_mutator_exit();
        return NULL;
    }

    if (h->len == 0) {
        rt_gc_mutator_exit();
        return NULL;
    }

    void *val = h->items[0].value;
    if (!rt_collection_retain_checked(val, "Heap.TryPeek: result retain failed")) {
        rt_gc_mutator_exit();
        return NULL;
    }
    rt_gc_mutator_exit();
    return val;
}

/// @brief Return the highest-priority item as an Option without removing it.
/// @details Returns `None` when the heap is empty and `Some(value)` otherwise.
///          The Option directly retains the stored value, avoiding an extra
///          temporary retain/release pair.
/// @param obj Opaque Heap object pointer.
/// @return New runtime-managed `Some(value)` when an item exists, otherwise
///         `None`.
void *rt_pqueue_try_peek_option(void *obj) {
    if (!obj)
        return rt_option_none();

    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    if (!h)
        return NULL;
    if (h->len == 0)
        return rt_option_none();

    return rt_option_some(h->items[0].value);
}

/// @brief Reset the queue to empty (length 0). Capacity is preserved.
/// @details Releases every queued value and clears occupied slots. The min/max
///          mode and current entry allocation remain available for reuse. A
///          null handle is a no-op.
/// @param obj Priority queue to clear, or `NULL`.
void rt_pqueue_clear(void *obj) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    if (!h) {
        rt_gc_mutator_exit();
        return;
    }
    int64_t len = h->len;
    h->len = 0;
    for (int64_t i = 0; i < len; i++) {
        void *value = h->items[i].value;
        h->items[i].value = NULL;
        h->items[i].priority = 0;
        heap_release_value(value);
    }
    rt_gc_mutator_exit();
}

/// @brief Drain a copy of the queue into a Seq, ordered by priority. The original queue is
/// preserved (operates on a temporary clone).
/// @details The result contains values only, not their integer priorities.
///          Min-heaps produce ascending priorities and max-heaps descending
///          priorities; equal-priority order is unspecified. The fresh owning
///          `Seq` independently retains every value. A null heap traps.
/// @param obj Source priority queue.
/// @return New runtime-managed owning `Seq` in extraction order.
void *rt_pqueue_to_seq(void *obj) {
    if (!obj) {
        rt_trap("Heap.ToSeq: null heap");
        return NULL;
    }

    rt_pqueue_impl *h = as_pqueue(obj, "Heap: invalid Heap object");
    if (!h)
        return NULL;

    void *volatile seq = NULL;
    rt_pqueue_impl *volatile copy = NULL;
    void *volatile transferred = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        char saved_error[256];
        heap_save_trap(saved_error, sizeof(saved_error), "Heap.ToSeq: snapshot failed");
        rt_trap_clear_recovery();
        heap_release_value((void *)transferred);
        heap_release_value((void *)copy);
        heap_release_value((void *)seq);
        rt_trap(saved_error);
        return NULL;
    }

    seq = rt_seq_with_capacity_owned(h->len > 0 ? h->len : 1);
    if (!seq) {
        rt_trap_clear_recovery();
        return NULL;
    }

    // Pop all elements in priority order and add to Seq. Work on a retained
    // copy so the original heap remains unchanged.
    copy = (rt_pqueue_impl *)rt_pqueue_new_max(h->is_max);
    if (!copy) {
        rt_trap_clear_recovery();
        heap_release_value((void *)seq);
        return NULL;
    }

    // Copy all entries
    for (int64_t i = 0; i < h->len; i++) {
        rt_pqueue_push((void *)copy, h->items[i].priority, h->items[i].value);
    }

    // Pop from copy in priority order
    while (copy->len > 0) {
        transferred = rt_pqueue_pop((void *)copy);
        // The pop already moved the copy's owned reference to this frame.
        // Move it again into the owning Seq without an avoidable retain/release
        // pair (which would also reject a saturated but otherwise valid ref).
        rt_seq_push_raw((void *)seq, (void *)transferred);
        transferred = NULL;
    }

    heap_release_value((void *)copy);
    copy = NULL;
    rt_trap_clear_recovery();
    return (void *)seq;
}
