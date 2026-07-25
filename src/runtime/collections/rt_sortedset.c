//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_sortedset.c
// Purpose: Implements a sorted set of strings backed by a sorted dynamic array
//   with binary search. All elements are maintained in lexicographic order at
//   all times; insertion and removal use binary search to find the correct
//   position, followed by memmove to shift elements.
//
// Key invariants:
//   - Backed by a heap-allocated array of rt_string references, sorted in
//     ascending byte-lexicographic order over the full length-aware buffers
//     (embedded NUL bytes participate; shorter strings order first on ties).
//   - Binary search provides O(log n) lookup (Contains, Floor, Ceiling).
//   - Insertion is O(n) due to memmove of the suffix after the insert point.
//   - Removal is O(n) due to memmove of the suffix before the remove point.
//   - Each element is a copied rt_string (via rt_string_from_bytes); the set
//     owns the string objects and frees them on removal or finalizer.
//   - Duplicate keys are rejected: inserting an existing string is a no-op.
//   - Not thread-safe; external synchronization required.
//
// Ownership/Lifetime:
//   - SortedSet objects are GC-managed (rt_obj_new_i64). The data array and
//     all contained rt_string copies are freed by the GC finalizer.
//
// Links: src/runtime/collections/rt_sortedset.h (public API),
//        src/runtime/collections/rt_set.h (unordered set counterpart)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime length-aware lexicographic string SortedSet.
/// @details The Set owns independent string copies in a sorted dynamic array.
///          Binary search supplies lookup/rank boundaries, while insertion and
///          removal shift suffixes to preserve order. Ordered snapshots copy
///          strings again so future mutations cannot affect callers.

#include "rt_sortedset.h"
#include "rt_collection_ids.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_trap.h"

/// @brief Install a non-local trap recovery target for the current thread.
/// @param buf Jump buffer that receives control when a runtime trap occurs.
void rt_trap_set_recovery(jmp_buf *buf);
/// @brief Remove the current thread's non-local trap recovery target.
void rt_trap_clear_recovery(void);
/// @brief Borrow the current thread's most recent trap diagnostic.
/// @return NUL-terminated diagnostic text, or `NULL` when unavailable.
const char *rt_trap_get_error(void);

/// @brief Native sorted-array payload embedded in the GC-managed object.
struct rt_sortedset_impl {
    rt_string *data; // Sorted array of strings
    int64_t len;     // Number of elements
    int64_t cap;     // Capacity
};

/// @brief Internal typed alias for the opaque public SortedSet handle.
typedef struct rt_sortedset_impl *rt_sortedset;

//=============================================================================
// Internal helpers
//=============================================================================

/// @brief Checked cast of an opaque handle to the SortedSet implementation;
///        traps with @p what if @p obj is NULL or not a SortedSet.
/// @param obj Opaque runtime handle to validate.
/// @param what Diagnostic emitted by the trap subsystem on failure.
/// @return Validated SortedSet implementation, or `NULL` after trapping.
static rt_sortedset as_sortedset(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_SORTEDSET_CLASS_ID, sizeof(struct rt_sortedset_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_sortedset)obj;
}

/// @brief Copy a runtime string by its complete length-aware byte sequence.
/// @details Null and unreadable/empty handles normalize to the empty string.
/// @param s Source runtime string, or `NULL`.
/// @return New independent string handle representing the same key.
static rt_string copy_string(rt_string s) {
    if (!s)
        return rt_const_cstr("");
    const char *cstr = rt_string_cstr(s);
    int64_t len = rt_str_len(s);
    if (!cstr || len <= 0)
        return rt_string_from_bytes("", 0);
    return rt_string_from_bytes(cstr, (size_t)len);
}

/// @brief Add one reference to @p s and return it (empty const for NULL);
///        used when handing a stored string back to the caller.
/// @param s Stored string handle, or `NULL`.
/// @return Caller-retained string, or the shared empty-string sentinel.
static rt_string retain_result_string(rt_string s) {
    if (!s)
        return rt_const_cstr("");
    rt_string_ref(s);
    return s;
}

/// @brief Release a temporary runtime object and free it at zero references.
/// @param obj Runtime-managed object, or `NULL` for a no-op.
static void sortedset_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Save active trap text before clearing a local recovery frame.
/// @param buffer Destination for a bounded NUL-terminated diagnostic.
/// @param buffer_size Size of @p buffer in bytes.
/// @param fallback Text used when the trap subsystem has no non-empty message.
static void sortedset_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Push an independent copy of @p s onto @p seq (balances the copy's
///        reference so the seq holds the only owning ref).
/// @details String allocation and Seq growth are trap-protected. Failure
///          releases both the temporary string and the complete partial
///          snapshot before propagating the saved diagnostic.
/// @param seq Owning snapshot Seq under construction.
/// @param s Stored source string to copy.
static void seq_push_string_copy(void *seq, rt_string s) {
    rt_string volatile copy = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        sortedset_save_trap_error(
            saved_error, sizeof(saved_error), "SortedSet: snapshot append failed");
        rt_trap_clear_recovery();
        if (copy)
            rt_str_release_maybe((rt_string)copy);
        sortedset_release_object(seq);
        rt_trap(saved_error);
        return;
    }

    copy = copy_string(s);
    if (!copy) {
        rt_trap("SortedSet: string allocation failed");
        rt_trap_clear_recovery();
        return;
    }
    rt_seq_push(seq, (void *)copy);
    rt_str_release_maybe((rt_string)copy);
    copy = NULL;
    rt_trap_clear_recovery();
}

/// @brief Lexicographic byte comparison of two strings (NULL treated as "");
///        returns <0, 0, >0 — the SortedSet ordering primitive.
/// @details Compares the common byte prefix with `memcmp`, then places the
///          shorter string first when one is a prefix of the other. Embedded
///          NUL bytes participate normally.
/// @param a First runtime string, or `NULL` as empty.
/// @param b Second runtime string, or `NULL` as empty.
/// @return Negative, zero, or positive according to ascending byte order.
static int compare_strings(rt_string a, rt_string b) {
    const char *sa = a ? rt_string_cstr(a) : "";
    const char *sb = b ? rt_string_cstr(b) : "";
    int64_t len_a = a && sa ? rt_str_len(a) : 0;
    int64_t len_b = b && sb ? rt_str_len(b) : 0;
    size_t la = len_a > 0 ? (size_t)len_a : 0;
    size_t lb = len_b > 0 ? (size_t)len_b : 0;
    size_t minlen = la < lb ? la : lb;
    int cmp = memcmp(sa ? sa : "", sb ? sb : "", minlen);
    if (cmp != 0)
        return cmp;
    if (la < lb)
        return -1;
    if (la > lb)
        return 1;
    return 0;
}

/// @brief Find an existing string or its lower-bound insertion position.
/// @param set SortedSet to search.
/// @param str Query string; `NULL` denotes the empty string.
/// @param found Optional output set to 1 for exact equality and 0 otherwise.
/// @return Existing index when found, otherwise the first valid insertion
///         index in `[0, set->len]`.
static int64_t binary_search(rt_sortedset set, rt_string str, int8_t *found) {
    if (!set || set->len == 0) {
        if (found)
            *found = 0;
        return 0;
    }

    int64_t lo = 0;
    int64_t hi = set->len - 1;

    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        int cmp = compare_strings(str, set->data[mid]);

        if (cmp == 0) {
            if (found)
                *found = 1;
            return mid;
        } else if (cmp < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }

    if (found)
        *found = 0;
    return lo;
}

/// @brief Grow the sorted-element array to hold at least @p needed entries
///        (geometric growth; traps on overflow/OOM).
/// @param set SortedSet whose capacity may grow.
/// @param needed Minimum required string slots.
/// @return 1 when capacity is sufficient, or 0 after trapping.
static int ensure_capacity(rt_sortedset set, int64_t needed) {
    if (set->cap >= needed)
        return 1;

    int64_t new_cap = set->cap == 0 ? 8 : set->cap * 2;
    while (new_cap < needed) {
        if (new_cap > INT64_MAX / 2) {
            rt_trap("SortedSet: capacity overflow");
            return 0;
        }
        new_cap *= 2;
    }

    if ((uint64_t)new_cap > SIZE_MAX / sizeof(rt_string)) {
        rt_trap("SortedSet: allocation size overflow");
        return 0;
    }
    rt_string *new_data = realloc(set->data, sizeof(rt_string) * new_cap);
    if (!new_data) {
        rt_trap("rt_sortedset: memory allocation failed");
        return 0;
    }
    set->data = new_data;
    set->cap = new_cap;
    return 1;
}

//=============================================================================
// Creation and Lifecycle
//=============================================================================

/// @brief GC finalizer: release every stored string and free the element array.
/// @param obj SortedSet object being finalized; `NULL` is ignored.
static void sortedset_finalizer(void *obj) {
    if (!obj)
        return;
    rt_sortedset set = as_sortedset(obj, "SortedSet: invalid SortedSet object");
    if (!set)
        return;
    for (int64_t i = 0; i < set->len; i++)
        rt_str_release_maybe(set->data[i]);
    free(set->data);
    set->data = NULL;
    set->len = 0;
    set->cap = 0;
}

/// @brief Construct an empty sorted set. Internally a sorted dynamic array — `_add` is O(log n)
/// search + O(n) shift; `_has` is O(log n). Use FrozenSet for read-only sets, regular Set for
/// hash-based unordered storage.
/// @return New runtime-managed SortedSet, or `NULL` on allocation failure.
void *rt_sortedset_new(void) {
    rt_sortedset set = (rt_sortedset)rt_obj_new_i64(RT_SORTEDSET_CLASS_ID,
                                                    (int64_t)sizeof(struct rt_sortedset_impl));
    if (set)
        rt_obj_set_finalizer(set, sortedset_finalizer);
    return set;
}

/// @brief Return the number of elements in the sorted set.
/// @param obj SortedSet handle, or `NULL`.
/// @return Unique-string count, or 0 for `NULL`.
int64_t rt_sortedset_len(void *obj) {
    rt_sortedset set = obj ? as_sortedset(obj, "SortedSet.Len: invalid SortedSet object") : NULL;
    return set ? set->len : 0;
}

/// @brief Check whether the sorted set is empty.
/// @details Equivalent to checking if the sorted array has no elements.
/// @param obj SortedSet handle, or `NULL`.
/// @return 1 when empty, otherwise 0.
int8_t rt_sortedset_is_empty(void *obj) {
    return rt_sortedset_len(obj) == 0 ? 1 : 0;
}

//=============================================================================
// Basic Operations
//=============================================================================

/// @brief Insert an element into the sorted set maintaining sorted order.
/// @details Performs a binary search to locate the insertion point, then shifts
///          subsequent elements right to open a gap. Duplicates (where the binary
///          search finds an exact match) are silently ignored. O(log n) search
///          plus O(n) shift for the insertion. Null input denotes the empty
///          string and the inserted key is copied before publication.
/// @param obj SortedSet to mutate, or `NULL`.
/// @param str String value to copy; `NULL` denotes empty.
/// @return 1 when inserted, otherwise 0 for duplicate/null Set.
int8_t rt_sortedset_add(void *obj, rt_string str) {
    if (!obj)
        return 0;
    rt_sortedset set = as_sortedset(obj, "SortedSet.Add: invalid SortedSet object");
    if (!set)
        return 0;

    int8_t found;
    int64_t idx = binary_search(set, str, &found);

    if (found)
        return 0; // Already present

    if (!ensure_capacity(set, set->len + 1))
        return 0;
    rt_string copy = copy_string(str);
    if (!copy) {
        rt_trap("SortedSet.Add: string allocation failed");
        return 0;
    }

    // Shift elements to make room
    if (idx < set->len) {
        memmove(&set->data[idx + 1], &set->data[idx], (size_t)(set->len - idx) * sizeof(rt_string));
    }

    // Insert copy of string
    set->data[idx] = copy;
    set->len++;
    return 1;
}

/// @brief Remove an element from the sorted set.
/// @details Performs a binary search to find the element, releases its string
///          reference, then shifts subsequent elements left to close the gap.
///          O(log n) search plus O(n) shift for the removal.
/// @param obj SortedSet to mutate, or `NULL`.
/// @param str Equal string to remove; `NULL` denotes empty.
/// @return 1 when removed, otherwise 0.
int8_t rt_sortedset_remove(void *obj, rt_string str) {
    if (!obj)
        return 0;
    rt_sortedset set = as_sortedset(obj, "SortedSet.Remove: invalid SortedSet object");
    if (!set)
        return 0;

    int8_t found;
    int64_t idx = binary_search(set, str, &found);

    if (!found)
        return 0;

    // Release the string
    rt_str_release_maybe(set->data[idx]);

    // Shift elements down
    if (idx + 1 < set->len) {
        memmove(
            &set->data[idx], &set->data[idx + 1], (size_t)(set->len - idx - 1) * sizeof(rt_string));
    }

    set->data[set->len - 1] = NULL;
    set->len--;
    return 1;
}

/// @brief Check whether an element exists in the sorted set.
/// @details Uses binary search over the sorted array for O(log n) lookup.
/// @param obj SortedSet handle, or `NULL`.
/// @param str Query string; `NULL` denotes empty.
/// @return 1 when present, otherwise 0.
int8_t rt_sortedset_has(void *obj, rt_string str) {
    if (!obj)
        return 0;
    rt_sortedset set = as_sortedset(obj, "SortedSet.Has: invalid SortedSet object");
    if (!set)
        return 0;

    int8_t found;
    binary_search(set, str, &found);
    return found;
}

/// @brief Remove all elements from the sorted set.
/// @details Releases all retained string references and resets the length to
///          zero. The backing array capacity is preserved for potential reuse.
/// @param obj SortedSet to clear, or `NULL` for a no-op.
void rt_sortedset_clear(void *obj) {
    if (!obj)
        return;
    rt_sortedset set = as_sortedset(obj, "SortedSet.Clear: invalid SortedSet object");
    if (!set)
        return;

    for (int64_t i = 0; i < set->len; i++) {
        rt_str_release_maybe(set->data[i]);
        set->data[i] = NULL;
    }
    set->len = 0;
}

//=============================================================================
// Ordered Access
//=============================================================================

/// @brief Return the smallest element in the sorted set.
/// @details Returns data[0] since the backing array is always sorted ascending.
/// @param obj SortedSet handle, or `NULL`.
/// @return Caller-retained smallest string, or an empty-string sentinel when
///         no element exists.
rt_string rt_sortedset_first(void *obj) {
    rt_sortedset set = obj ? as_sortedset(obj, "SortedSet.First: invalid SortedSet object") : NULL;
    if (!set || set->len == 0)
        return rt_const_cstr("");
    return retain_result_string(set->data[0]);
}

/// @brief Return the largest element in the sorted set.
/// @details Returns data[len-1] since the backing array is always sorted ascending.
/// @param obj SortedSet handle, or `NULL`.
/// @return Caller-retained largest string, or an empty-string sentinel.
rt_string rt_sortedset_last(void *obj) {
    rt_sortedset set = obj ? as_sortedset(obj, "SortedSet.Last: invalid SortedSet object") : NULL;
    if (!set || set->len == 0)
        return rt_const_cstr("");
    return retain_result_string(set->data[set->len - 1]);
}

/// @brief Return the greatest element less than or equal to the given value.
/// @details Uses binary search to find the insertion point; if an exact match
///          exists it is returned, otherwise the element just before the
///          insertion point is the floor.
/// @param obj SortedSet handle, or `NULL`.
/// @param str Inclusive upper-bound query; `NULL` denotes empty.
/// @return Caller-retained floor string, or an empty-string sentinel when none
///         exists.
rt_string rt_sortedset_floor(void *obj, rt_string str) {
    rt_sortedset set = obj ? as_sortedset(obj, "SortedSet.Floor: invalid SortedSet object") : NULL;
    if (!set || set->len == 0)
        return rt_const_cstr("");

    int8_t found;
    int64_t idx = binary_search(set, str, &found);

    if (found)
        return retain_result_string(set->data[idx]);
    if (idx == 0)
        return rt_const_cstr("");
    return retain_result_string(set->data[idx - 1]);
}

/// @brief Return the smallest element greater than or equal to the given value.
/// @details Uses binary search to find the insertion point; if an exact match
///          exists it is returned, otherwise the element at the insertion point
///          (the first element greater than the value) is the ceiling.
/// @param obj SortedSet handle, or `NULL`.
/// @param str Inclusive lower-bound query; `NULL` denotes empty.
/// @return Caller-retained ceiling string, or an empty-string sentinel.
rt_string rt_sortedset_ceil(void *obj, rt_string str) {
    rt_sortedset set = obj ? as_sortedset(obj, "SortedSet.Ceil: invalid SortedSet object") : NULL;
    if (!set || set->len == 0)
        return rt_const_cstr("");

    int8_t found;
    int64_t idx = binary_search(set, str, &found);

    if (found)
        return retain_result_string(set->data[idx]);
    if (idx >= set->len)
        return rt_const_cstr("");
    return retain_result_string(set->data[idx]);
}

/// @brief Return the greatest element strictly less than the given value.
/// @details Similar to floor but excludes exact matches.
/// @param obj SortedSet handle, or `NULL`.
/// @param str Exclusive upper-bound query; `NULL` denotes empty.
/// @return Caller-retained lower string, or an empty-string sentinel.
rt_string rt_sortedset_lower(void *obj, rt_string str) {
    rt_sortedset set = obj ? as_sortedset(obj, "SortedSet.Lower: invalid SortedSet object") : NULL;
    if (!set || set->len == 0)
        return rt_const_cstr("");

    int8_t found;
    int64_t idx = binary_search(set, str, &found);

    (void)found;
    // For lower, we want strictly less than the match or insertion point.
    idx--;

    if (idx < 0)
        return rt_const_cstr("");
    return retain_result_string(set->data[idx]);
}

/// @brief Return the smallest element strictly greater than the given value.
/// @details Similar to ceil but excludes exact matches.
/// @param obj SortedSet handle, or `NULL`.
/// @param str Exclusive lower-bound query; `NULL` denotes empty.
/// @return Caller-retained higher string, or an empty-string sentinel.
rt_string rt_sortedset_higher(void *obj, rt_string str) {
    rt_sortedset set = obj ? as_sortedset(obj, "SortedSet.Higher: invalid SortedSet object") : NULL;
    if (!set || set->len == 0)
        return rt_const_cstr("");

    int8_t found;
    int64_t idx = binary_search(set, str, &found);

    // For higher, we want strictly greater than
    if (found)
        idx++;

    if (idx >= set->len)
        return rt_const_cstr("");
    return retain_result_string(set->data[idx]);
}

/// @brief Return the element at the given rank (0-based index in sorted order).
/// @details Direct array access: data[index]. O(1) since the array is sorted.
/// @param obj SortedSet handle, or `NULL`.
/// @param index Zero-based sorted rank.
/// @return Caller-retained string at @p index, or an empty-string sentinel.
rt_string rt_sortedset_at(void *obj, int64_t index) {
    rt_sortedset set = obj ? as_sortedset(obj, "SortedSet.At: invalid SortedSet object") : NULL;
    if (!set || index < 0 || index >= set->len)
        return rt_const_cstr("");
    return retain_result_string(set->data[index]);
}

/// @brief Return the 0-based rank of an element in sorted order.
/// @details Returns -1 if the element is not in the set.
/// @param obj SortedSet handle, or `NULL`.
/// @param str Query string; `NULL` denotes empty.
/// @return Sorted rank, or -1 when absent.
int64_t rt_sortedset_index_of(void *obj, rt_string str) {
    if (!obj)
        return -1;
    rt_sortedset set = as_sortedset(obj, "SortedSet.IndexOf: invalid SortedSet object");
    if (!set)
        return -1;

    int8_t found;
    int64_t idx = binary_search(set, str, &found);
    return found ? idx : -1;
}

//=============================================================================
// Range Operations
//=============================================================================

/// @brief Return the slice of elements with `from ≤ x ≤ to` as a Seq. Both endpoints inclusive;
/// either may be NULL to mean "open end". Useful for range queries on ordered keys.
/// @details Every selected string is copied into an owning snapshot. If the
///          lower bound sorts after the upper bound, the result is empty.
/// @param obj SortedSet handle, or `NULL`.
/// @param from Inclusive lower bound, or `NULL` for no lower bound.
/// @param to Inclusive upper bound, or `NULL` for no upper bound.
/// @return New runtime-managed owning Seq of copied strings.
void *rt_sortedset_range(void *obj, rt_string from, rt_string to) {
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    rt_seq_set_owns_elements(seq, 1);
    if (!obj)
        return seq;
    rt_sortedset set = as_sortedset(obj, "SortedSet.Range: invalid SortedSet object");
    if (!set)
        return seq;

    int8_t found;
    int64_t start = from ? binary_search(set, from, &found) : 0;

    for (int64_t i = start; i < set->len; i++) {
        if (to && compare_strings(set->data[i], to) > 0)
            break;
        seq_push_string_copy(seq, set->data[i]);
    }

    return seq;
}

/// @brief Return a Seq of every element in sorted ascending order. Snapshot — independent of
/// future mutations.
/// @param obj SortedSet handle, or `NULL`.
/// @return New runtime-managed owning Seq of independent string copies.
void *rt_sortedset_items(void *obj) {
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    rt_seq_set_owns_elements(seq, 1);
    if (!obj)
        return seq;
    rt_sortedset set = as_sortedset(obj, "SortedSet.Items: invalid SortedSet object");
    if (!set)
        return seq;

    for (int64_t i = 0; i < set->len; i++) {
        seq_push_string_copy(seq, set->data[i]);
    }

    return seq;
}

/// @brief Return a Seq of the first `n` elements in sorted order (the smallest values).
/// @param obj SortedSet handle, or `NULL`.
/// @param n Maximum number of leading elements; nonpositive values select none.
/// @return New runtime-managed owning Seq of independent string copies.
void *rt_sortedset_take(void *obj, int64_t n) {
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    rt_seq_set_owns_elements(seq, 1);
    if (!obj || n <= 0)
        return seq;
    rt_sortedset set = as_sortedset(obj, "SortedSet.Take: invalid SortedSet object");
    if (!set)
        return seq;

    int64_t count = n < set->len ? n : set->len;
    for (int64_t i = 0; i < count; i++) {
        seq_push_string_copy(seq, set->data[i]);
    }

    return seq;
}

/// @brief Return a Seq containing all but the first `n` elements (in sorted order).
/// @param obj SortedSet handle, or `NULL`.
/// @param n Number of elements to skip; negative values skip zero.
/// @return New runtime-managed owning Seq of independent string copies.
void *rt_sortedset_skip(void *obj, int64_t n) {
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    rt_seq_set_owns_elements(seq, 1);
    if (!obj)
        return seq;
    rt_sortedset set = as_sortedset(obj, "SortedSet.Skip: invalid SortedSet object");
    if (!set)
        return seq;
    if (n >= set->len)
        return seq;

    int64_t start = n < 0 ? 0 : n;
    for (int64_t i = start; i < set->len; i++) {
        seq_push_string_copy(seq, set->data[i]);
    }

    return seq;
}

//=============================================================================
// Set Operations
//=============================================================================

/// @brief Return a fresh sorted set containing every element from either operand.
/// @details Null operands denote empty sets. Every result string is copied
///          independently through ordinary SortedSet insertion.
/// @param obj First SortedSet, or `NULL`.
/// @param other Second SortedSet, or `NULL`.
/// @return New runtime-managed union.
void *rt_sortedset_union(void *obj, void *other) {
    void *result = rt_sortedset_new();
    if (!result)
        return NULL;
    rt_sortedset a = obj ? as_sortedset(obj, "SortedSet.Union: invalid SortedSet object") : NULL;
    rt_sortedset b =
        other ? as_sortedset(other, "SortedSet.Union: invalid SortedSet object") : NULL;

    if (a) {
        for (int64_t i = 0; i < a->len; i++) {
            rt_sortedset_add(result, a->data[i]);
        }
    }

    if (b) {
        for (int64_t i = 0; i < b->len; i++) {
            rt_sortedset_add(result, b->data[i]);
        }
    }

    return result;
}

/// @brief Return a fresh sorted set containing only elements present in both operands. Uses a
/// merge-style two-pointer walk over the sorted arrays for O(|a| + |b|) time.
/// @param obj First SortedSet, or `NULL` as empty.
/// @param other Second SortedSet, or `NULL` as empty.
/// @return New runtime-managed intersection containing independent copies.
void *rt_sortedset_intersect(void *obj, void *other) {
    void *result = rt_sortedset_new();
    if (!result)
        return NULL;
    rt_sortedset a =
        obj ? as_sortedset(obj, "SortedSet.Intersect: invalid SortedSet object") : NULL;
    rt_sortedset b =
        other ? as_sortedset(other, "SortedSet.Intersect: invalid SortedSet object") : NULL;

    if (!a || !b)
        return result;

    // Use merge-style intersection for sorted sets
    int64_t i = 0, j = 0;
    while (i < a->len && j < b->len) {
        int cmp = compare_strings(a->data[i], b->data[j]);
        if (cmp == 0) {
            rt_sortedset_add(result, a->data[i]);
            i++;
            j++;
        } else if (cmp < 0) {
            i++;
        } else {
            j++;
        }
    }

    return result;
}

/// @brief Return a fresh sorted set containing elements present in `obj` but not in `other`.
/// @param obj Minuend SortedSet, or `NULL` as empty.
/// @param other Subtrahend SortedSet, or `NULL` as empty.
/// @return New runtime-managed difference containing independent copies.
void *rt_sortedset_diff(void *obj, void *other) {
    void *result = rt_sortedset_new();
    if (!result)
        return NULL;
    rt_sortedset a = obj ? as_sortedset(obj, "SortedSet.Diff: invalid SortedSet object") : NULL;
    rt_sortedset b = other ? as_sortedset(other, "SortedSet.Diff: invalid SortedSet object") : NULL;

    if (!a)
        return result;
    if (!b) {
        // All elements in a
        for (int64_t i = 0; i < a->len; i++) {
            rt_sortedset_add(result, a->data[i]);
        }
        return result;
    }

    // Use merge-style difference for sorted sets
    int64_t i = 0, j = 0;
    while (i < a->len) {
        if (j >= b->len) {
            rt_sortedset_add(result, a->data[i]);
            i++;
        } else {
            int cmp = compare_strings(a->data[i], b->data[j]);
            if (cmp < 0) {
                rt_sortedset_add(result, a->data[i]);
                i++;
            } else if (cmp > 0) {
                j++;
            } else {
                i++;
                j++;
            }
        }
    }

    return result;
}

/// @brief Check whether this set is a subset of another sorted set.
/// @details Every element in this set must also be present in the other set.
///          Null handles denote empty operands.
/// @param obj Candidate subset, or `NULL`.
/// @param other Candidate superset, or `NULL`.
/// @return 1 when the subset relation holds, otherwise 0.
int8_t rt_sortedset_is_subset(void *obj, void *other) {
    if (!obj)
        return 1; // Empty is subset of everything
    rt_sortedset a = as_sortedset(obj, "SortedSet.IsSubset: invalid SortedSet object");
    if (!a)
        return 0;
    if (a->len == 0)
        return 1;
    if (!other)
        return 0; // Non-empty can't be subset of empty
    rt_sortedset b = as_sortedset(other, "SortedSet.IsSubset: invalid SortedSet object");
    if (!b)
        return 0;

    // Check all elements of a are in b
    int64_t j = 0;
    for (int64_t i = 0; i < a->len; i++) {
        // Find a->data[i] in b
        while (j < b->len && compare_strings(b->data[j], a->data[i]) < 0) {
            j++;
        }
        if (j >= b->len || compare_strings(b->data[j], a->data[i]) != 0) {
            return 0; // Not found
        }
    }

    return 1;
}
