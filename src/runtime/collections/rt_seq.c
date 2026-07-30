//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_seq.c
// Purpose: Core operations for Zanna.Collections.Seq, the primary dynamic growable
//   array for the Zanna runtime. Contains allocation, element access, mutation
//   (push/pop/insert/remove), search, reverse, shuffle, slice, and clone.
//   Sorting and functional operations live in rt_seq_ops.c.
//
// Key invariants:
//   - Initial capacity is SEQ_DEFAULT_CAP (16); grows by SEQ_GROWTH_FACTOR (2).
//   - The items array is a separate malloc allocation; the header is GC-managed.
//   - len is the number of valid elements; cap is the allocated array size.
//     Indexed access outside [0, len) traps.
//   - Borrowing sequences store raw pointers; owning sequences retain inserted
//     elements and release them on replacement, removal, clear, or finalization.
//   - Ownership mode may change only while the sequence is empty.
//   - Core operations live here; stable merge sorting and functional operators
//     are implemented in the linked Seq support translation units.
//   - Not thread-safe; external synchronization required for concurrent writes.
//
// Ownership/Lifetime:
//   - Seq objects are GC-managed (rt_obj_new_i64). The items array is
//     malloc-managed and freed by the GC finalizer (seq_finalizer).
//   - Borrowed accessors return raw stored pointers. Removal from an owning Seq
//     returns a caller-retained reference; removal from a borrowing Seq does
//     not extend element lifetime.
//
// Links: src/runtime/collections/rt_seq_internal.h (shared struct definition),
//        src/runtime/collections/rt_seq_ops.c (sorting and functional operations),
//        src/runtime/collections/rt_seq_functional.c (void* wrapper layer)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements allocation, indexing, mutation, search, and basic order
///        operations for the runtime dynamic Seq.
/// @details Seq is a growable pointer array with selectable borrowed or
///          retained-element ownership. Mutations run inside GC mutator
///          regions, and growth helpers unwind speculative retains if
///          allocation traps before an element can be published.

#include "rt_seq.h"
#include "rt_box.h"
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_random.h"
#include "rt_seq_internal.h"
#include "rt_string.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Pointer slots reserved by the default constructor.
#define SEQ_DEFAULT_CAP 16
/// @brief Multiplicative capacity increase used during automatic growth.
#define SEQ_GROWTH_FACTOR 2

/// @brief Install a non-local trap recovery target for the current thread.
/// @param buf Jump buffer that receives control when a runtime trap occurs.
void rt_trap_set_recovery(jmp_buf *buf);
/// @brief Remove the current thread's non-local trap recovery target.
void rt_trap_clear_recovery(void);
/// @brief Borrow the current thread's most recent trap diagnostic.
/// @return NUL-terminated diagnostic text, or `NULL` when unavailable.
const char *rt_trap_get_error(void);

/// @brief Checked cast of an opaque handle to the Seq implementation;
///        traps with @p what if @p obj is NULL or not a Seq.
/// @param obj Opaque runtime handle to validate.
/// @param what Diagnostic emitted by the trap subsystem on failure.
/// @return Validated Seq implementation, or `NULL` after trapping.
static rt_seq_impl *as_seq(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_SEQ_CLASS_ID, sizeof(rt_seq_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_seq_impl *)obj;
}

/// @brief Release a single element via the object API (safe for strings and objects).
/// @param val Runtime object/string reference, or `NULL` for a no-op.
static void seq_release_element(void *val) {
    if (!val)
        return;
    if (rt_obj_release_check0(val))
        rt_obj_free(val);
}

/// @brief GC traversal: visit every live element (only when the seq owns
///        its elements).
/// @param obj Seq whose live owned slots are to be traced.
/// @param visitor Collector callback invoked in index order.
/// @param ctx Opaque collector context forwarded unchanged.
static void rt_seq_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq->owns_elements || !seq->items)
        return;
    for (int64_t i = 0; i < seq->len; i++)
        visitor(seq->items[i], ctx);
}

/// @brief GC finalizer for `Seq[T]` — releases each owned element and the items array.
///
/// The sequence may or may not own its elements (`owns_elements`
/// flag). Borrowed-element sequences (typed views over a parent
/// container, etc.) skip the per-element release pass to avoid
/// double-free.
/// @param obj Seq object being finalized; `NULL` is ignored.
static void rt_seq_finalize(void *obj) {
    if (!obj)
        return;
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (seq->owns_elements && seq->items) {
        for (int64_t i = 0; i < seq->len; i++)
            seq_release_element(seq->items[i]);
    }
    free(seq->items);
    seq->items = NULL;
    seq->len = 0;
    seq->cap = 0;
}

/// @brief Ensures the sequence has capacity for at least `needed` elements.
///
/// If the current capacity is insufficient, the items array is reallocated
/// to a larger size. Growth is exponential (doubling) to amortize allocation
/// costs over many push operations, giving O(1) amortized push complexity.
///
/// **Growth strategy:**
/// - Capacity doubles each time growth is needed
/// - Starting capacity is 16 (SEQ_DEFAULT_CAP)
/// - Growth sequence: 16 → 32 → 64 → 128 → 256 → ...
///
/// @param seq Pointer to the sequence implementation. Must not be NULL.
/// @param needed Minimum required capacity after this call.
/// @return 1 when capacity is sufficient, or 0 after an overflow/allocation
///         trap.
///
/// @note Traps on memory allocation failure with "Seq: memory allocation failed".
/// @note Never shrinks the capacity - only grows when needed.
///
/// @see rt_seq_push For the primary user of this function
static int seq_ensure_capacity(rt_seq_impl *seq, int64_t needed) {
    if (needed <= seq->cap)
        return 1;

    int64_t new_cap = seq->cap;
    while (new_cap < needed) {
        if (new_cap > INT64_MAX / SEQ_GROWTH_FACTOR) {
            rt_trap("Seq: capacity overflow");
            return 0;
        }
        new_cap *= SEQ_GROWTH_FACTOR;
    }

    if ((uint64_t)new_cap > SIZE_MAX / sizeof(void *)) {
        rt_trap("Seq: allocation size overflow");
        return 0;
    }
    void **new_items = realloc(seq->items, (size_t)new_cap * sizeof(void *));
    if (!new_items) {
        rt_trap("Seq: memory allocation failed");
        return 0;
    }

    seq->items = new_items;
    seq->cap = new_cap;
    return 1;
}

/// @brief Save the active trap text before clearing a recovery frame.
/// @param buffer Destination for a bounded NUL-terminated diagnostic copy.
/// @param buffer_size Size of @p buffer in bytes.
/// @param fallback Text used when the trap subsystem has no non-empty message.
static void seq_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Grow a Seq while protecting a retain performed before publication.
/// @details Catches growth traps, re-enters a balanced mutator region to
///          release @p retained_value when needed, then propagates the saved
///          diagnostic. This keeps Push/Insert failure-atomic with respect to
///          element ownership.
/// @param seq Sequence whose allocation may grow.
/// @param needed Required element capacity.
/// @param retained_value Value speculatively retained by the caller.
/// @param retained Nonzero when @p retained_value must be released on failure.
/// @param fallback Diagnostic used if the caught trap supplied none.
/// @return 1 on success, or 0 after cleanup and trap propagation.
static int seq_ensure_capacity_or_release(
    rt_seq_impl *seq, int64_t needed, void *retained_value, int retained, const char *fallback) {
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        seq_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        /* The trap dispatcher unwinds any active mutator scope before
         * transferring control here.  Re-enter while balancing the retained
         * value so collection cannot race the recovery cleanup. */
        rt_gc_mutator_enter();
        if (retained)
            seq_release_element(retained_value);
        rt_gc_mutator_exit();
        rt_trap(saved_error);
        return 0;
    }

    int ok = seq_ensure_capacity(seq, needed);
    rt_trap_clear_recovery();
    if (!ok && retained)
        seq_release_element(retained_value);
    return ok;
}

/// @brief Creates a new empty Seq (sequence) with default capacity.
///
/// Allocates and initializes a Seq data structure for storing a dynamic array
/// of elements. The Seq starts with a default capacity of 16 slots and grows
/// automatically as elements are added.
///
/// The Seq is the most versatile Zanna collection, providing:
/// - O(1) amortized append (Push)
/// - O(1) random access (Get/Set)
/// - O(n) insertion/removal at arbitrary positions
///
/// **Usage example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push("first")
/// seq.Push("second")
/// seq.Push("third")
/// Print seq.Get(0)   ' Outputs: first
/// Print seq.Len()    ' Outputs: 3
/// Print seq.Pop()    ' Outputs: third
/// ```
///
/// @return A pointer to the newly created Seq object. Traps and does not
///         return if memory allocation fails.
///
/// @note Initial capacity is 16 elements (SEQ_DEFAULT_CAP).
/// @note This constructor creates a borrowing Seq. Use
///       @ref rt_seq_new_owned for retained-element ownership.
/// @note Thread safety: Not thread-safe. External synchronization required.
///
/// @see rt_seq_with_capacity For creating with a specific initial capacity
/// @see rt_seq_push For adding elements
/// @see rt_seq_get For accessing elements
/// @see rt_seq_finalize For cleanup behavior
void *rt_seq_new(void) {
    rt_seq_impl *seq = (rt_seq_impl *)rt_obj_new_i64(RT_SEQ_CLASS_ID, (int64_t)sizeof(rt_seq_impl));
    if (!seq) {
        rt_trap("Seq: memory allocation failed");
        return NULL;
    }

    seq->len = 0;
    seq->cap = SEQ_DEFAULT_CAP;
    seq->owns_elements = 0;
    seq->items = malloc((size_t)SEQ_DEFAULT_CAP * sizeof(void *));
    rt_obj_set_finalizer(seq, rt_seq_finalize);
    rt_gc_track(seq, rt_seq_traverse);

    if (!seq->items) {
        if (rt_obj_release_check0(seq))
            rt_obj_free(seq);
        rt_trap("Seq: memory allocation failed");
        return NULL;
    }

    return seq;
}

/// @brief Creates a public Seq that retains pushed elements.
/// @return New runtime-managed empty owning Seq with default capacity.
void *rt_seq_new_owned(void) {
    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    return seq;
}

/// @brief Create an empty seq inheriting @p source's element-ownership mode
///        (defaults to owning when @p source is NULL).
/// @param source Source implementation whose mode should be copied, or `NULL`.
/// @return New runtime-managed empty Seq.
static void *seq_new_empty_like(rt_seq_impl *source) {
    void *seq = rt_seq_new();
    if (!source || source->owns_elements)
        rt_seq_set_owns_elements(seq, 1);
    return seq;
}

/// @brief Creates a public Seq with a fixed initial length.
/// @details The result is owning, has at least one reserved slot, and exposes
///          exactly @p len initialized `NULL` elements.
/// @param len Initial logical length; negative values trap.
/// @return New runtime-managed owning Seq.
void *rt_seq_new_sized(int64_t len) {
    if (len < 0) {
        rt_trap("Seq.NewSized: negative length");
        return NULL;
    }

    void *obj = rt_seq_with_capacity_owned(len > 0 ? len : 1);
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq)
        return NULL;
    for (int64_t i = 0; i < len; i++)
        seq->items[i] = NULL;
    seq->len = len;
    return obj;
}

/// @brief Creates a new empty Seq with a specified initial capacity.
///
/// Allocates a Seq with pre-allocated space for the specified number of elements.
/// This is useful when you know approximately how many elements you'll need,
/// as it avoids the overhead of multiple reallocations during growth.
///
/// **Performance optimization:**
/// If you know you'll be adding 1000 elements, creating a Seq with capacity 1000
/// avoids the growth sequence: 16 → 32 → 64 → 128 → 256 → 512 → 1024, saving
/// 6 reallocations and memory copies.
///
/// **Example:**
/// ```
/// ' Pre-allocate for 100 elements
/// Dim scores = Seq.WithCapacity(100)
/// For i = 1 To 100
///     scores.Push(GetScore(i))  ' No reallocations occur
/// Next
/// ```
///
/// @param cap Initial capacity. Values less than 1 are clamped to 1.
///
/// @return A pointer to the newly created Seq object. Traps and does not
///         return if memory allocation fails.
///
/// @note The Seq is empty after creation (length 0) - capacity is just reserved space.
/// @note This constructor creates a borrowing Seq.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_new For creating with default capacity (16)
/// @see rt_seq_cap For querying the current capacity
void *rt_seq_with_capacity(int64_t cap) {
    if (cap < 1)
        cap = 1;
    if ((uint64_t)cap > SIZE_MAX / sizeof(void *)) {
        rt_trap("Seq: allocation size overflow");
        return NULL;
    }

    rt_seq_impl *seq = (rt_seq_impl *)rt_obj_new_i64(RT_SEQ_CLASS_ID, (int64_t)sizeof(rt_seq_impl));
    if (!seq) {
        rt_trap("Seq: memory allocation failed");
        return NULL;
    }

    seq->len = 0;
    seq->cap = cap;
    seq->owns_elements = 0;
    seq->items = malloc((size_t)cap * sizeof(void *));
    rt_obj_set_finalizer(seq, rt_seq_finalize);
    rt_gc_track(seq, rt_seq_traverse);

    if (!seq->items) {
        if (rt_obj_release_check0(seq))
            rt_obj_free(seq);
        rt_trap("Seq: memory allocation failed");
        return NULL;
    }

    return seq;
}

/// @brief Creates a public capacity-reserved Seq that retains pushed elements.
/// @param cap Initial reserved capacity, clamped to at least one.
/// @return New runtime-managed empty owning Seq.
void *rt_seq_with_capacity_owned(int64_t cap) {
    void *seq = rt_seq_with_capacity(cap);
    if (!seq)
        return NULL;
    rt_seq_set_owns_elements(seq, 1);
    return seq;
}

/// @brief Enable or disable element ownership for a Seq.
///
/// When owns_elements=1, the Seq retains elements on push/set and releases
/// them on clear/finalize. When owns_elements=0 (default), the Seq stores
/// raw pointers and the caller manages element lifetime.
///
/// @param obj Pointer to a Seq object. Must not be NULL.
/// @param owns 1 to enable ownership, 0 to disable.
///
/// @note Must be called before any elements are pushed. A requested mode
///       change on a non-empty Seq traps; requesting its current mode is
///       accepted.
void rt_seq_set_owns_elements(void *obj, int8_t owns) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq) {
        rt_gc_mutator_exit();
        return;
    }
    owns = owns ? 1 : 0;
    if (seq->len != 0 && seq->owns_elements != owns) {
        rt_gc_mutator_exit();
        rt_trap("Seq.SetOwnsElements: cannot change ownership mode on non-empty sequence");
        return;
    }
    seq->owns_elements = owns;
    rt_gc_mutator_exit();
}

/// @brief Returns the number of elements currently in the Seq.
///
/// This function returns how many elements have been added and not yet removed.
/// The count is maintained internally and returned in O(1) time.
///
/// @param obj Pointer to a Seq object. If NULL, returns 0.
///
/// @return The number of elements in the Seq (>= 0). Returns 0 if obj is NULL.
///
/// @note O(1) time complexity.
///
/// @see rt_seq_cap For the allocated capacity
/// @see rt_seq_is_empty For a boolean check
int64_t rt_seq_len(void *obj) {
    if (!obj)
        return 0;
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    return seq ? seq->len : 0;
}

/// @brief Returns the current allocated capacity of the Seq.
///
/// Capacity is the number of elements the Seq can hold without reallocating.
/// This is always >= the current length. When length exceeds capacity during
/// a push, the Seq automatically grows (capacity doubles).
///
/// **Capacity vs Length:**
/// - Length: How many elements are currently stored
/// - Capacity: How many elements can be stored without reallocation
///
/// @param obj Pointer to a Seq object. If NULL, returns 0.
///
/// @return The current capacity (>= 0). Returns 0 if obj is NULL.
///
/// @note O(1) time complexity.
///
/// @see rt_seq_len For the number of actual elements
/// @see rt_seq_with_capacity For pre-allocating capacity
int64_t rt_seq_cap(void *obj) {
    if (!obj)
        return 0;
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    return seq ? seq->cap : 0;
}

/// @brief Checks whether the Seq contains no elements.
///
/// A Seq is considered empty when its length is 0, which occurs:
/// - Immediately after creation
/// - After all elements have been popped/removed
/// - After calling rt_seq_clear
///
/// @param obj Pointer to a Seq object. If NULL, returns true (1).
///
/// @return 1 (true) if the Seq is empty or obj is NULL, 0 (false) otherwise.
///
/// @note O(1) time complexity.
///
/// @see rt_seq_len For the exact count
/// @see rt_seq_clear For removing all elements
int8_t rt_seq_is_empty(void *obj) {
    if (!obj)
        return 1;
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    return !seq || seq->len == 0 ? 1 : 0;
}

/// @brief Returns the element at the specified index.
///
/// Provides O(1) random access to any element in the Seq. Indices are
/// zero-based, so valid indices range from 0 to len-1.
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push("a")
/// seq.Push("b")
/// seq.Push("c")
/// Print seq.Get(0)  ' Outputs: a
/// Print seq.Get(1)  ' Outputs: b
/// Print seq.Get(2)  ' Outputs: c
/// ```
///
/// @param obj Pointer to a Seq object. Must not be NULL.
/// @param idx Zero-based index of the element to retrieve (0 to len-1).
///
/// @return The element at the specified index.
///
/// @note O(1) time complexity.
/// @note Traps with "Seq.Get: null sequence" if obj is NULL.
/// @note Traps with "Seq.Get: index out of bounds" if idx < 0 or idx >= len.
/// @note No retain is created. In owning mode the result is borrowed from the
///       Seq and remains valid only while its stored retain survives; in
///       borrowing mode the producer controls lifetime.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_set For modifying an element
/// @see rt_seq_first For getting the first element
/// @see rt_seq_last For getting the last element
void *rt_seq_get(void *obj, int64_t idx) {
    if (!obj) {
        rt_trap("Seq.Get: null sequence");
        return NULL;
    }

    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq)
        return NULL;

    if (idx < 0 || idx >= seq->len) {
        rt_trap("Seq.Get: index out of bounds");
        return NULL;
    }

    return seq->items[idx];
}

/// @brief Get string element at index from a string sequence.
///
/// Runtime string snapshots store raw rt_string pointers, while general Zia
/// calls to Seq.Push(String) pass boxed strings through the object ABI. This
/// helper accepts both representations and returns an owned string handle.
///
/// @param obj Opaque Seq object pointer.
/// @param idx Index of element to retrieve.
/// @return String element at the index (raw rt_string pointer).
/// @note The returned string handle is owned by the caller.
struct rt_string_impl *rt_seq_get_str(void *obj, int64_t idx) {
    void *val = rt_seq_get(obj, idx);
    if (rt_string_is_handle(val))
        return rt_string_ref((rt_string)val);
    if (rt_box_type(val) == RT_BOX_STR)
        return rt_unbox_str(val);
    rt_trap("Seq.GetStr: value is not a string");
    return NULL;
}

/// @brief Replaces the element at the specified index.
///
/// Provides O(1) random modification of any element in the Seq. The index
/// must refer to an existing element - this function cannot extend the Seq.
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push("a")
/// seq.Push("b")
/// seq.Set(0, "x")
/// Print seq.Get(0)  ' Outputs: x
/// Print seq.Get(1)  ' Outputs: b
/// ```
///
/// @param obj Pointer to a Seq object. Must not be NULL.
/// @param idx Zero-based index of the element to modify (0 to len-1).
/// @param val The new value to store at this index. May be NULL.
///
/// @note O(1) time complexity.
/// @note Owning Seqs retain @p val before releasing the replaced slot;
///       borrowing Seqs store the pointer raw. Retain-before-release makes
///       self-replacement safe.
/// @note Traps with "Seq.Set: null sequence" if obj is NULL.
/// @note Traps with "Seq.Set: index out of bounds" if idx < 0 or idx >= len.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_get For reading an element
/// @see rt_seq_push For adding new elements
void rt_seq_set(void *obj, int64_t idx, void *val) {
    if (!obj) {
        rt_trap("Seq.Set: null sequence");
        return;
    }

    rt_gc_mutator_enter();
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq) {
        rt_gc_mutator_exit();
        return;
    }

    if (idx < 0 || idx >= seq->len) {
        rt_gc_mutator_exit();
        rt_trap("Seq.Set: index out of bounds");
        return;
    }

    void *old = seq->items[idx];
    if (seq->owns_elements) {
        if (val)
            rt_obj_retain_maybe(val);
    }
    seq->items[idx] = val;
    if (seq->owns_elements)
        seq_release_element(old);
    rt_gc_mutator_exit();
}

/// @brief Replace an element by transferring the caller's reference.
/// @details Stores @p val without retaining it. For an owning sequence, the
///          previous slot reference is released after the store. This release
///          also occurs when old and new pointers are identical: in that case
///          the caller's transferred reference replaces the sequence's prior
///          reference, so one of the two indistinguishable retains must still
///          be consumed to keep the net count unchanged.
/// @param obj Owning or borrowing Seq object; must not be NULL.
/// @param idx Existing zero-based slot index.
/// @param val Value whose existing reference is transferred into the slot.
void rt_seq_set_raw(void *obj, int64_t idx, void *val) {
    if (!obj) {
        rt_trap("Seq.Set: null sequence");
        return;
    }

    rt_gc_mutator_enter();
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq) {
        rt_gc_mutator_exit();
        return;
    }

    if (idx < 0 || idx >= seq->len) {
        rt_gc_mutator_exit();
        rt_trap("Seq.Set: index out of bounds");
        return;
    }

    void *old = seq->items[idx];
    seq->items[idx] = val;
    if (seq->owns_elements)
        seq_release_element(old);
    rt_gc_mutator_exit();
}

/// @brief Adds an element to the end of the Seq.
///
/// Appends a new element after the current last element. This is the primary
/// way to grow a Seq. If capacity is exceeded, the Seq automatically doubles
/// its internal storage.
///
/// **Visual example:**
/// ```
/// Before Push(D):  [A, B, C]      len=3
/// After Push(D):   [A, B, C, D]   len=4
/// ```
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push("first")
/// seq.Push("second")
/// seq.Push("third")
/// Print seq.Len()  ' Outputs: 3
/// ```
///
/// @param obj Pointer to a Seq object. Must not be NULL.
/// @param val The element to add. May be NULL (NULL is a valid element).
///
/// @note O(1) amortized time complexity. Occasional O(n) when resizing occurs.
/// @note Owning Seqs retain @p val before publication; borrowing Seqs store
///       the pointer raw.
/// @note Traps with "Seq.Push: null sequence" if obj is NULL.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_pop For removing from the end
/// @see rt_seq_insert For inserting at arbitrary positions
/// @see rt_seq_push_all For appending multiple elements
void rt_seq_push(void *obj, void *val) {
    if (!obj) {
        rt_trap("Seq.Push: null sequence");
        return;
    }

    rt_gc_mutator_enter();
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq) {
        rt_gc_mutator_exit();
        return;
    }

    if (seq->len >= INT64_MAX) {
        rt_gc_mutator_exit();
        rt_trap("Seq: maximum length reached");
        return;
    }
    int retained = 0;
    if (seq->owns_elements && val) {
        rt_obj_retain_maybe(val);
        retained = 1;
    }
    if (!seq_ensure_capacity_or_release(
            seq, seq->len + 1, val, retained, "Seq.Push: capacity failed")) {
        rt_gc_mutator_exit();
        return;
    }
    seq->items[seq->len] = val;
    seq->len++;
    rt_gc_mutator_exit();
}

/// @brief Push without retaining the element — for sequences that don't own their values.
///
/// Used by typed-view paths (e.g. `Seq[Int]` over a packed int64
/// array) where the underlying storage is not GC-managed and a
/// retain would be a noop. Public-facing code should use `rt_seq_push`.
/// @param obj Non-null Seq to mutate.
/// @param val Raw pointer appended without a retain.
void rt_seq_push_raw(void *obj, void *val) {
    if (!obj) {
        rt_trap("Seq.Push: null sequence");
        return;
    }

    rt_gc_mutator_enter();
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq) {
        rt_gc_mutator_exit();
        return;
    }

    if (seq->len >= INT64_MAX) {
        rt_gc_mutator_exit();
        rt_trap("Seq: maximum length reached");
        return;
    }
    if (!seq_ensure_capacity(seq, seq->len + 1)) {
        rt_gc_mutator_exit();
        return;
    }
    seq->items[seq->len] = val;
    seq->len++;
    rt_gc_mutator_exit();
}

/// @brief Appends all elements from another Seq to the end of this Seq.
///
/// Copies all elements from the source Seq and appends them to the destination
/// Seq, preserving their order. This is more efficient than pushing elements
/// one by one as it performs a single capacity check and memory copy.
///
/// **Example:**
/// ```
/// Dim seq1 = Seq.New()
/// seq1.Push("a")
/// seq1.Push("b")
///
/// Dim seq2 = Seq.New()
/// seq2.Push("c")
/// seq2.Push("d")
///
/// seq1.PushAll(seq2)
/// ' seq1 is now: [a, b, c, d]
/// ' seq2 is unchanged: [c, d]
/// ```
///
/// **Self-append behavior:**
/// When pushing a Seq onto itself (obj == other), the Seq doubles its contents.
/// This is handled specially to avoid infinite loops:
/// ```
/// Dim seq = Seq.New()
/// seq.Push("x")
/// seq.PushAll(seq)  ' seq becomes: [x, x]
/// ```
///
/// @param obj Destination Seq to append to. Must not be NULL.
/// @param other Source Seq whose elements will be copied. If NULL, no-op.
///
/// @note O(n) time complexity where n is the length of other.
/// @note The source Seq is not modified (elements are copied, not moved).
/// @note An owning destination independently retains each appended element;
///       a borrowing destination copies pointers raw. Destination ownership,
///       not source ownership, controls the result.
/// @note Traps with "Seq.PushAll: null sequence" if obj is NULL.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_push For adding single elements
/// @see rt_seq_clone For creating a copy of a Seq
void rt_seq_push_all(void *obj, void *other) {
    if (!obj) {
        rt_trap("Seq.PushAll: null sequence");
        return;
    }
    if (!other)
        return;

    rt_gc_mutator_enter();
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    rt_seq_impl *src = as_seq(other, "Seq: invalid Seq object");
    if (!seq || !src) {
        rt_gc_mutator_exit();
        return;
    }

    if (src->len <= 0) {
        rt_gc_mutator_exit();
        return;
    }

    if (seq == src) {
        int64_t original_len = seq->len;
        if (original_len > INT64_MAX - original_len) {
            rt_gc_mutator_exit();
            rt_trap("Seq.PushAll: length overflow");
            return;
        }
        if (seq->owns_elements) {
            if (!seq_ensure_capacity(seq, original_len + original_len)) {
                rt_gc_mutator_exit();
                return;
            }
            for (int64_t i = 0; i < original_len; i++) {
                void *item = seq->items[i];
                if (item)
                    rt_obj_retain_maybe(item);
                seq->items[seq->len] = item;
                seq->len++;
            }
            rt_gc_mutator_exit();
            return;
        }
        if (!seq_ensure_capacity(seq, original_len + original_len)) {
            rt_gc_mutator_exit();
            return;
        }
        memcpy(&seq->items[original_len], seq->items, (size_t)original_len * sizeof(void *));
        seq->len = original_len + original_len;
        rt_gc_mutator_exit();
        return;
    }

    if (src->len > INT64_MAX - seq->len) {
        rt_gc_mutator_exit();
        rt_trap("Seq.PushAll: length overflow");
        return;
    }
    if (seq->owns_elements) {
        if (!seq_ensure_capacity(seq, seq->len + src->len)) {
            rt_gc_mutator_exit();
            return;
        }
        for (int64_t i = 0; i < src->len; i++) {
            void *item = src->items[i];
            if (item)
                rt_obj_retain_maybe(item);
            seq->items[seq->len] = item;
            seq->len++;
        }
        rt_gc_mutator_exit();
        return;
    }
    if (!seq_ensure_capacity(seq, seq->len + src->len)) {
        rt_gc_mutator_exit();
        return;
    }
    memcpy(&seq->items[seq->len], src->items, (size_t)src->len * sizeof(void *));
    seq->len += src->len;
    rt_gc_mutator_exit();
}

/// @brief Removes and returns the last element from the Seq.
///
/// Removes the element at the end of the Seq and returns it. This is the
/// inverse of Push and provides O(1) removal from the end.
///
/// **Visual example:**
/// ```
/// Before Pop():  [A, B, C, D]   len=4
/// After Pop():   [A, B, C]      len=3
/// Returns: D
/// ```
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push("first")
/// seq.Push("second")
/// seq.Push("third")
/// Print seq.Pop()  ' Outputs: third
/// Print seq.Pop()  ' Outputs: second
/// Print seq.Len()  ' Outputs: 1
/// ```
///
/// @param obj Pointer to a Seq object. Must not be NULL.
///
/// @return The element that was at the end of the Seq.
///
/// @note O(1) time complexity.
/// @note Owning Seqs return a caller-retained value. Borrowing Seqs return the
///       raw stored pointer without extending its lifetime.
/// @note Traps with "Seq.Pop: null sequence" if obj is NULL.
/// @note Traps with "Seq.Pop: sequence is empty" if the Seq is empty.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_push For the inverse operation
/// @see rt_seq_peek For viewing without removing
/// @see rt_seq_is_empty For checking before pop
void *rt_seq_pop(void *obj) {
    if (!obj) {
        rt_trap("Seq.Pop: null sequence");
        return NULL;
    }

    rt_gc_mutator_enter();
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq) {
        rt_gc_mutator_exit();
        return NULL;
    }

    if (seq->len == 0) {
        rt_gc_mutator_exit();
        rt_trap("Seq.Pop: sequence is empty");
        return NULL;
    }

    void *val = seq->items[seq->len - 1];
    if (seq->owns_elements)
        rt_obj_retain_maybe(val);
    seq->len--;
    seq->items[seq->len] = NULL; // Clear slot to prevent stale pointer access
    if (seq->owns_elements)
        seq_release_element(val);
    rt_gc_mutator_exit();
    return val;
}

/// @brief Returns the last element without removing it.
///
/// Peeks at the element at the end of the Seq without modifying the Seq.
/// This is equivalent to Get(Len() - 1) but more convenient and descriptive.
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push("a")
/// seq.Push("b")
/// Print seq.Peek()  ' Outputs: b
/// Print seq.Peek()  ' Outputs: b (still there)
/// Print seq.Pop()   ' Outputs: b (now removed)
/// Print seq.Peek()  ' Outputs: a
/// ```
///
/// @param obj Pointer to a Seq object. Must not be NULL.
///
/// @return The element at the end of the Seq (not removed).
///
/// @note O(1) time complexity.
/// @note No retain is created. The result is borrowed from an owning Seq or
///       from the original producer for a borrowing Seq.
/// @note Traps with "Seq.Peek: null sequence" if obj is NULL.
/// @note Traps with "Seq.Peek: sequence is empty" if the Seq is empty.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_pop For removing while retrieving
/// @see rt_seq_last Alias for this function
/// @see rt_seq_first For viewing the first element
void *rt_seq_peek(void *obj) {
    if (!obj) {
        rt_trap("Seq.Peek: null sequence");
        return NULL;
    }

    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq)
        return NULL;

    if (seq->len == 0) {
        rt_trap("Seq.Peek: sequence is empty");
        return NULL;
    }

    return seq->items[seq->len - 1];
}

/// @brief Returns the first element without removing it.
///
/// Provides convenient access to the element at index 0. This is equivalent
/// to Get(0) but more descriptive.
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push("a")
/// seq.Push("b")
/// seq.Push("c")
/// Print seq.First()  ' Outputs: a
/// Print seq.Last()   ' Outputs: c
/// ```
///
/// @param obj Pointer to a Seq object. Must not be NULL.
///
/// @return The element at index 0 (not removed).
///
/// @note O(1) time complexity.
/// @note No retain is created. The result is borrowed from an owning Seq or
///       from the original producer for a borrowing Seq.
/// @note Traps with "Seq.First: null sequence" if obj is NULL.
/// @note Traps with "Seq.First: sequence is empty" if the Seq is empty.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_last For viewing the last element
/// @see rt_seq_get For accessing by arbitrary index
void *rt_seq_first(void *obj) {
    if (!obj) {
        rt_trap("Seq.First: null sequence");
        return NULL;
    }

    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq)
        return NULL;

    if (seq->len == 0) {
        rt_trap("Seq.First: sequence is empty");
        return NULL;
    }

    return seq->items[0];
}

/// @brief Returns the last element without removing it.
///
/// Provides convenient access to the element at index (len - 1). This is
/// equivalent to Get(Len() - 1) and Peek() but with a more descriptive name.
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push("a")
/// seq.Push("b")
/// seq.Push("c")
/// Print seq.Last()   ' Outputs: c
/// Print seq.First()  ' Outputs: a
/// ```
///
/// @param obj Pointer to a Seq object. Must not be NULL.
///
/// @return The element at index (len - 1) (not removed).
///
/// @note O(1) time complexity.
/// @note No retain is created. The result is borrowed from an owning Seq or
///       from the original producer for a borrowing Seq.
/// @note Traps with "Seq.Last: null sequence" if obj is NULL.
/// @note Traps with "Seq.Last: sequence is empty" if the Seq is empty.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_first For viewing the first element
/// @see rt_seq_peek Alias for this function
/// @see rt_seq_get For accessing by arbitrary index
void *rt_seq_last(void *obj) {
    if (!obj) {
        rt_trap("Seq.Last: null sequence");
        return NULL;
    }

    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq)
        return NULL;

    if (seq->len == 0) {
        rt_trap("Seq.Last: sequence is empty");
        return NULL;
    }

    return seq->items[seq->len - 1];
}

/// @brief Inserts an element at the specified position.
///
/// Inserts a new element at the given index, shifting all subsequent elements
/// one position to the right. Unlike Set, Insert grows the Seq by one element.
///
/// **Visual example:**
/// ```
/// Before Insert(1, X):  [A, B, C]      len=3
/// After Insert(1, X):   [A, X, B, C]   len=4
/// ```
///
/// **Valid indices:**
/// - 0: Insert at the beginning (before all elements)
/// - len: Insert at the end (equivalent to Push)
/// - Any value from 0 to len (inclusive)
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push("a")
/// seq.Push("c")
/// seq.Insert(1, "b")    ' Insert between a and c
/// ' seq is now: [a, b, c]
/// seq.Insert(0, "start") ' Insert at beginning
/// ' seq is now: [start, a, b, c]
/// ```
///
/// @param obj Pointer to a Seq object. Must not be NULL.
/// @param idx Position to insert at (0 to len inclusive).
/// @param val The element to insert. May be NULL.
///
/// @note O(n) time complexity due to element shifting.
/// @note Owning Seqs retain @p val before any reallocating growth; borrowing
///       Seqs store it raw.
/// @note Traps with "Seq.Insert: null sequence" if obj is NULL.
/// @note Traps with "Seq.Insert: index out of bounds" if idx < 0 or idx > len.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_push For appending to the end (O(1))
/// @see rt_seq_remove For removing at an index
void rt_seq_insert(void *obj, int64_t idx, void *val) {
    if (!obj) {
        rt_trap("Seq.Insert: null sequence");
        return;
    }

    rt_gc_mutator_enter();
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq) {
        rt_gc_mutator_exit();
        return;
    }

    if (idx < 0 || idx > seq->len) {
        rt_gc_mutator_exit();
        rt_trap("Seq.Insert: index out of bounds");
        return;
    }
    if (seq->len >= INT64_MAX) {
        rt_gc_mutator_exit();
        rt_trap("Seq: maximum length reached");
        return;
    }

    int retained = 0;
    if (seq->owns_elements && val) {
        rt_obj_retain_maybe(val);
        retained = 1;
    }
    if (!seq_ensure_capacity_or_release(
            seq, seq->len + 1, val, retained, "Seq.Insert: capacity failed")) {
        rt_gc_mutator_exit();
        return;
    }

    // Shift elements to the right
    if (idx < seq->len) {
        memmove(&seq->items[idx + 1], &seq->items[idx], (size_t)(seq->len - idx) * sizeof(void *));
    }

    seq->items[idx] = val;
    seq->len++;
    rt_gc_mutator_exit();
}

/// @brief Removes and returns the element at the specified position.
///
/// Removes the element at the given index and shifts all subsequent elements
/// one position to the left to fill the gap.
///
/// **Visual example:**
/// ```
/// Before Remove(1):  [A, B, C, D]   len=4
/// After Remove(1):   [A, C, D]      len=3
/// Returns: B
/// ```
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push("a")
/// seq.Push("b")
/// seq.Push("c")
/// Print seq.Remove(1)  ' Outputs: b
/// ' seq is now: [a, c]
/// Print seq.Remove(0)  ' Outputs: a
/// ' seq is now: [c]
/// ```
///
/// @param obj Pointer to a Seq object. Must not be NULL.
/// @param idx Zero-based index of the element to remove (0 to len-1).
///
/// @return The element that was removed.
///
/// @note O(n) time complexity due to element shifting.
/// @note Owning Seqs return a caller-retained value. Borrowing Seqs return the
///       raw stored pointer without extending its lifetime.
/// @note Traps with "Seq.Remove: null sequence" if obj is NULL.
/// @note Traps with "Seq.Remove: index out of bounds" if idx < 0 or idx >= len.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_pop For removing from the end (O(1))
/// @see rt_seq_insert For inserting at an index
void *rt_seq_remove(void *obj, int64_t idx) {
    if (!obj) {
        rt_trap("Seq.Remove: null sequence");
        return NULL;
    }

    rt_gc_mutator_enter();
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq) {
        rt_gc_mutator_exit();
        return NULL;
    }

    if (idx < 0 || idx >= seq->len) {
        rt_gc_mutator_exit();
        rt_trap("Seq.Remove: index out of bounds");
        return NULL;
    }

    void *val = seq->items[idx];
    if (seq->owns_elements)
        rt_obj_retain_maybe(val);

    // Shift elements to the left
    if (idx < seq->len - 1) {
        memmove(
            &seq->items[idx], &seq->items[idx + 1], (size_t)(seq->len - idx - 1) * sizeof(void *));
    }

    seq->items[seq->len - 1] = NULL;
    seq->len--;
    if (seq->owns_elements)
        seq_release_element(val);
    rt_gc_mutator_exit();
    return val;
}

/// @brief Removes all elements from the Seq.
///
/// Clears the Seq by resetting its length to 0. The capacity remains unchanged
/// (no memory is freed), allowing the Seq to be efficiently reused for new
/// elements.
///
/// **After clear:**
/// - Length becomes 0
/// - is_empty returns true
/// - Capacity unchanged (no reallocation)
/// - Borrowed element references are forgotten
/// - Owned element references are released
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push("a")
/// seq.Push("b")
/// Print seq.Len()    ' Outputs: 2
/// seq.Clear()
/// Print seq.Len()    ' Outputs: 0
/// Print seq.IsEmpty  ' Outputs: True
/// ```
///
/// @param obj Pointer to a Seq object. If NULL, this is a no-op.
///
/// @note O(n) for owned sequences because each live element is released; O(n)
///       for borrowed sequences to clear reusable slots.
/// @note Borrowed elements are not freed. Owned elements are released and may be
///       freed when their reference count drops to zero.
/// @note The active portion of the internal array is cleared so released
///       handles do not remain in reusable storage.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_finalize For complete cleanup (including the array)
/// @see rt_seq_is_empty For checking if empty
void rt_seq_clear(void *obj) {
    if (!obj)
        return;
    rt_gc_mutator_enter();
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq) {
        rt_gc_mutator_exit();
        return;
    }
    if (seq->owns_elements && seq->items) {
        for (int64_t i = 0; i < seq->len; i++) {
            seq_release_element(seq->items[i]);
            seq->items[i] = NULL;
        }
    } else if (seq->items) {
        for (int64_t i = 0; i < seq->len; i++)
            seq->items[i] = NULL;
    }
    seq->len = 0;
    rt_gc_mutator_exit();
}

/// @brief Finds the first occurrence of an element in the Seq.
///
/// Searches with boxed-value equality (rt_box_equal): boxed integers,
/// booleans, floats, and strings compare by content; non-boxed objects
/// compare by identity. Returns the index of the first match, or -1 if
/// not found.
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// Dim obj1 = SomeObject.New()
/// Dim obj2 = SomeObject.New()
/// seq.Push(obj1)
/// seq.Push(obj2)
/// Print seq.Find(obj1)   ' Outputs: 0
/// Print seq.Find(obj2)   ' Outputs: 1
/// Print seq.Find(Nothing) ' Outputs: -1 (not found)
/// ```
///
/// @param obj Pointer to a Seq object. If NULL, returns -1.
/// @param val The element to search for (compared by content for boxed values).
///
/// @return The zero-based index of the first occurrence, or -1 if not found
///         or obj is NULL.
///
/// @note O(n) time complexity - linear search from the beginning.
/// @note Boxed values are compared by content; non-boxed by pointer identity.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_has For boolean membership check
int64_t rt_seq_find(void *obj, void *val) {
    if (!obj)
        return -1;

    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");

    for (int64_t i = 0; i < seq->len; i++) {
        if (rt_box_equal(seq->items[i], val)) {
            return i;
        }
    }

    return -1;
}

/// @brief Find the first index of an element as an Option index.
/// @details Sentinel-free companion to @ref rt_seq_find. A match returns
///          `SomeI64(index)` and absence returns `None`.
/// @param obj Opaque Seq object pointer, or NULL.
/// @param val Element to search for; may be NULL.
/// @return Opaque Zanna.Option containing the first index, or None.
void *rt_seq_find_option(void *obj, void *val) {
    int64_t index = rt_seq_find(obj, val);
    return index >= 0 ? rt_option_some_i64(index) : rt_option_none();
}

/// @brief Checks whether the Seq contains a specific element.
///
/// Tests if the element is present in the Seq using content-aware equality.
/// This is a convenience wrapper around rt_seq_find that returns a boolean.
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// Dim obj = SomeObject.New()
/// seq.Push(obj)
/// Print seq.Has(obj)     ' Outputs: True
/// Print seq.Has(Nothing) ' Outputs: False
/// ```
///
/// @param obj Pointer to a Seq object. If NULL, returns 0 (false).
/// @param val The element to search for (compared by content for boxed values).
///
/// @return 1 (true) if the element is found, 0 (false) otherwise.
///
/// @note O(n) time complexity - linear search.
/// @note Boxed values are compared by content; non-boxed by pointer identity.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_find For getting the index of the element
int8_t rt_seq_has(void *obj, void *val) {
    return rt_seq_find(obj, val) >= 0 ? 1 : 0;
}

/// @brief Reverses the order of elements in the Seq in place.
///
/// Modifies the Seq so that elements appear in reverse order. The first
/// element becomes the last, the second becomes second-to-last, and so on.
///
/// **Visual example:**
/// ```
/// Before Reverse():  [A, B, C, D]
/// After Reverse():   [D, C, B, A]
/// ```
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push(1)
/// seq.Push(2)
/// seq.Push(3)
/// seq.Reverse()
/// ' seq is now: [3, 2, 1]
/// For i = 0 To seq.Len() - 1
///     Print seq.Get(i)  ' Outputs: 3, 2, 1
/// Next
/// ```
///
/// @param obj Pointer to a Seq object. If NULL, this is a no-op.
///
/// @note O(n/2) time complexity - swaps pairs from ends toward middle.
/// @note Modifies the Seq in place (no new allocation).
/// @note Safe to call on empty or single-element Seqs (no-op).
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_shuffle For randomizing order
void rt_seq_reverse(void *obj) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq) {
        rt_gc_mutator_exit();
        return;
    }

    for (int64_t i = 0; i < seq->len / 2; i++) {
        int64_t j = seq->len - 1 - i;
        void *tmp = seq->items[i];
        seq->items[i] = seq->items[j];
        seq->items[j] = tmp;
    }
    rt_gc_mutator_exit();
}

/// @brief Randomly shuffles the elements in the Seq in place.
///
/// Rearranges the elements into a random permutation using the Fisher-Yates
/// (also known as Knuth) shuffle algorithm. Each possible permutation has
/// equal probability.
///
/// **Fisher-Yates algorithm:**
/// For each position i from len-1 down to 1:
/// 1. Pick a random index j from 0 to i (inclusive)
/// 2. Swap elements at positions i and j
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push(1)
/// seq.Push(2)
/// seq.Push(3)
/// seq.Push(4)
/// seq.Shuffle()
/// ' seq might now be: [3, 1, 4, 2] (random order)
/// ```
///
/// **Deterministic shuffles:**
/// To get reproducible shuffles, seed the random number generator before
/// calling Shuffle:
/// ```
/// Random.Seed(12345)
/// seq.Shuffle()  ' Same seed = same shuffle result
/// ```
///
/// @param obj Pointer to a Seq object. If NULL, this is a no-op.
///
/// @note O(n) time complexity.
/// @note Modifies the Seq in place (no new allocation).
/// @note Uses Zanna.Random.NextInt for randomness - seed for reproducibility.
/// @note Safe to call on empty or single-element Seqs (no-op).
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_reverse For reversing order
void rt_seq_shuffle(void *obj) {
    if (!obj)
        return;

    rt_gc_mutator_enter();
    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    if (!seq) {
        rt_gc_mutator_exit();
        return;
    }
    if (seq->len <= 1) {
        rt_gc_mutator_exit();
        return;
    }

    for (int64_t i = seq->len - 1; i > 0; --i) {
        int64_t j = (int64_t)rt_rand_int((long long)(i + 1));
        void *tmp = seq->items[i];
        seq->items[i] = seq->items[j];
        seq->items[j] = tmp;
    }
    rt_gc_mutator_exit();
}

/// @brief Creates a new Seq containing a subset of elements from [start, end).
///
/// Extracts a portion of the Seq into a new Seq. The range is half-open:
/// start is inclusive, end is exclusive. Out-of-bounds indices are clamped
/// to valid ranges rather than causing errors.
///
/// **Visual example:**
/// ```
/// Original:            [A, B, C, D, E]
/// Slice(1, 4):         [B, C, D]
/// Slice(0, 2):         [A, B]
/// Slice(3, 100):       [D, E]  (end clamped to 5)
/// ```
///
/// **Index clamping:**
/// - start < 0 is treated as 0
/// - end > len is treated as len
/// - start >= end returns an empty Seq
///
/// **Example:**
/// ```
/// Dim seq = Seq.New()
/// seq.Push("a")
/// seq.Push("b")
/// seq.Push("c")
/// seq.Push("d")
/// Dim sub = seq.Slice(1, 3)
/// ' sub is: [b, c]
/// ' original seq is unchanged
/// ```
///
/// @param obj Source Seq to slice from. If NULL, returns an empty Seq.
/// @param start Start index (inclusive). Clamped to 0 if negative.
/// @param end End index (exclusive). Clamped to len if greater.
///
/// @return A new Seq containing elements from indices [start, end).
///         Returns empty Seq if start >= end or obj is NULL.
///
/// @note O(n) time complexity where n is the slice length.
/// @note The source Seq is not modified.
/// @note Elements are shallow-copied (pointers, not deep copies).
/// @note The result preserves the source ownership mode and independently
///       retains copied elements when that mode is owning. A null source
///       produces an empty owning Seq.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_clone For copying the entire Seq
void *rt_seq_slice(void *obj, int64_t start, int64_t end) {
    if (!obj)
        return seq_new_empty_like(NULL);

    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");

    // Clamp bounds
    if (start < 0)
        start = 0;
    if (end > seq->len)
        end = seq->len;
    if (start >= end) {
        return seq_new_empty_like(seq);
    }

    int64_t new_len = end - start;
    rt_seq_impl *result = rt_seq_with_capacity(new_len);
    if (seq->owns_elements)
        rt_seq_set_owns_elements(result, 1);

    for (int64_t i = start; i < end; i++)
        rt_seq_push(result, seq->items[i]);

    return result;
}

/// @brief Creates a shallow copy of the Seq.
///
/// Returns a new Seq containing all elements from the original. This is a
/// shallow copy: the element pointers are copied, but the elements themselves
/// are not duplicated. Both Seqs will point to the same underlying objects.
///
/// **Shallow vs Deep copy:**
/// - Shallow (this function): Copies pointers, shares objects
/// - Deep: Would copy objects too (not provided)
///
/// **Example:**
/// ```
/// Dim original = Seq.New()
/// original.Push("a")
/// original.Push("b")
/// original.Push("c")
///
/// Dim copy = original.Clone()
/// ' copy is: [a, b, c]
///
/// copy.Push("d")
/// ' copy is: [a, b, c, d]
/// ' original is: [a, b, c] (unchanged)
/// ```
///
/// **Use cases:**
/// - Creating a backup before modifications
/// - Passing a copy to a function that might modify it
/// - Testing with a duplicate while preserving the original
///
/// @param obj Source Seq to copy. If NULL, returns an empty Seq.
///
/// @return A new Seq containing the same elements as the original.
///
/// @note O(n) time complexity where n is the length.
/// @note The source Seq is not modified.
/// @note Elements are shallow-copied (same pointers as original).
/// @note The result preserves the source ownership mode and independently
///       retains shared objects when that mode is owning. A null source
///       produces an empty owning Seq.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_seq_slice For copying a subset
void *rt_seq_clone(void *obj) {
    if (!obj)
        return seq_new_empty_like(NULL);

    rt_seq_impl *seq = as_seq(obj, "Seq: invalid Seq object");
    return rt_seq_slice(obj, 0, seq->len);
}
