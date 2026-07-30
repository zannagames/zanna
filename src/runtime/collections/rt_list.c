//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_list.c
// Purpose: Implements a dynamic, mutable list of object references backed by
//   rt_arr_obj (a managed object array). Provides push, pop, get, set, remove,
//   insert, reverse, sort, and contains operations. Unlike Seq, List is designed
//   for frequent mutation with stable GC-managed element references.
//
// Key invariants:
//   - The List header contains only a vptr and a strong rt_arr_obj edge; all
//     element storage is delegated to that separately tracked object array.
//   - rt_arr_obj growth strategy: doubles capacity when full, starting from 16.
//   - Elements are retained on insertion (rt_obj_retain_maybe) and released
//     on removal/clear/finalize; Get returns a retained reference the caller
//     must release.
//   - Pop removes and returns the last element (LIFO semantics for stack use).
//   - RemoveAt shifts elements left; Insert shifts elements right — both O(n).
//   - Sort is a stable merge sort. The default comparator orders raw/boxed
//     strings lexicographically and boxed integers numerically; a caller
//     comparator can override it.
//   - Not thread-safe; external synchronization required for concurrent access.
//
// Ownership/Lifetime:
//   - List objects are GC-managed (rt_obj_new_i64). The underlying rt_arr_obj
//     is managed by the GC; no manual free is needed.
//
// Links: src/runtime/collections/rt_list.h (public API),
//        src/runtime/rt_array_obj.h (backing storage implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements a mutable, retained-element runtime List.
///
/// List delegates contiguous element storage and reference management to a
/// tracked `rt_arr_obj`. Indexed reads return retained references, mutations
/// retain replacements before releasing displaced values, and removal
/// operations preserve the previous state if backing-array resizing fails.
///
/// Search uses runtime boxed-value equality. Sorting uses a stable merge sort
/// with the shared total ordering across null, numeric, string, and other
/// values. List objects and their backing arrays participate in runtime GC;
/// mutation is otherwise unsynchronized.

#include "rt_list.h"

#include "rt_array_obj.h"
#include "rt_box.h"
#include "rt_collection_ids.h"
#include "rt_gc.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_random.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Internal List implementation structure.
///
/// The List is a dynamic collection backed by rt_arr_obj (a managed object array).
/// Unlike Seq which manages its own internal array, List delegates storage to
/// the object array system which handles reference counting automatically.
///
/// **Memory layout:**
/// ```
/// List object (GC-managed):
///   +------+-----+
///   | vptr | arr |
///   | NULL | --->|
///   +------+-|---+
///            |
///            v
/// rt_arr_obj (managed array):
///   +---+---+---+...+
///   | A | B | C |   |
///   +---+---+---+...+
/// ```
///
/// **Comparison with Seq:**
/// - List: Uses rt_arr_obj, automatic reference management
/// - Seq: Uses raw malloc'd array, more control
///
/// **Element ownership:**
/// Elements stored in the List are managed by the underlying rt_arr_obj,
/// which handles reference counting automatically.
typedef struct rt_list_impl {
    void **vptr; ///< Vtable pointer placeholder (for OOP compatibility).
    void **arr;  ///< Pointer to the underlying object array (rt_arr_obj).
} rt_list_impl;

/// @brief Finalizer callback invoked when a List is garbage collected.
///
/// This function is automatically called by Zanna's garbage collector when a
/// List object becomes unreachable. It releases the underlying object array,
/// which will in turn release references to all contained elements.
///
/// @param obj Pointer to the List object being finalized. May be NULL (no-op).
///
/// @note The underlying rt_arr_obj handles element reference counting.
/// @note This function is idempotent - safe to call on already-finalized lists.
///
/// @see rt_list_clear For removing elements without finalization
static void rt_list_finalize(void *obj) {
    if (!obj)
        return;
    rt_list_impl *L = (rt_list_impl *)obj;
    if (L->arr) {
        void **arr = L->arr;
        L->arr = NULL;
        rt_arr_obj_release(arr);
    }
}

/// @brief GC traversal: visit the managed backing-array edge.
/// @details The object array is independently tracked and visits its own live
///          elements. Reporting the array here models the actual two-hop
///          ownership graph and avoids trial-decrementing each element twice.
/// @param obj List object to traverse.
/// @param visitor Runtime callback invoked for the managed backing array.
/// @param ctx Opaque visitor context forwarded unchanged.
static void rt_list_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    if (!obj || !visitor)
        return;
    rt_list_impl *L = (rt_list_impl *)obj;
    visitor(L->arr, ctx);
}

/// @brief Creates a new empty List.
///
/// Allocates and initializes a List data structure for storing a dynamic
/// collection of objects. The List starts empty with no underlying array
/// allocated until elements are added.
///
/// **Lazy allocation:**
/// The internal array is not allocated until the first element is added.
/// This makes creating empty Lists very lightweight.
///
/// **Usage example:**
/// ```
/// Dim list = List.New()
/// list.Push("first")
/// list.Push("second")
/// list.Push("third")
/// Print list.Count   ' Outputs: 3
/// PRINT list.Get(0) ' Outputs: first
/// ```
///
/// @return A pointer to the newly created List object, or NULL if memory
///         allocation fails.
///
/// @note The List uses rt_arr_obj for storage with automatic reference management.
/// @note Thread safety: Not thread-safe. External synchronization required.
///
/// @see rt_list_push For adding elements
/// @see rt_list_get For accessing elements
/// @see rt_list_finalize For cleanup behavior
void *rt_list_new(void) {
    // Allocate object payload with header via object allocator to match object lifetime rules
    rt_list_impl *list =
        (rt_list_impl *)rt_obj_new_i64(RT_LIST_CLASS_ID, (int64_t)sizeof(rt_list_impl));
    if (!list)
        return NULL;
    list->vptr = NULL;
    list->arr = NULL;
    rt_obj_set_finalizer(list, rt_list_finalize);
    rt_gc_track(list, rt_list_traverse);
    return list;
}

/// @brief Helper to cast a void pointer to a List implementation pointer.
/// @param p Raw pointer to cast.
/// @return Pointer cast to rt_list_impl*.
static inline rt_list_impl *as_list(void *p) {
    if (!rt_obj_is_instance(p, RT_LIST_CLASS_ID, sizeof(rt_list_impl))) {
        rt_trap("List: invalid List object");
        return NULL;
    }
    return (rt_list_impl *)p;
}

/// @brief Drop one GC reference to a transient @p obj and free it at zero.
/// @param obj Temporary retained runtime object, or NULL for a no-op.
static void release_temp_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Returns the number of elements in the List.
///
/// This function returns how many elements are currently stored in the List.
/// The count is maintained by the underlying array and returned in O(1) time.
///
/// @param list Pointer to a List object. If NULL, returns 0.
///
/// @return The number of elements in the List (>= 0). Returns 0 if list is NULL.
///
/// @note O(1) time complexity.
///
/// @see rt_list_push For operations that increase the count
/// @see rt_list_remove_at For operations that decrease the count
int64_t rt_list_len(void *list) {
    if (!list)
        return 0;
    rt_list_impl *L = as_list(list);
    return (int64_t)rt_arr_obj_len(L->arr);
}

/// @brief Removes all elements from the List.
///
/// Clears the List by releasing the underlying array. This also releases
/// references to all contained elements, which may cause them to be freed
/// if no other references exist.
///
/// **After clear:**
/// - Count becomes 0
/// - All element references are released
/// - The List can be reused for new elements
///
/// **Example:**
/// ```
/// Dim list = List.New()
/// list.Push("a")
/// list.Push("b")
/// Print list.Count  ' Outputs: 2
/// list.Clear()
/// Print list.Count  ' Outputs: 0
/// ```
///
/// @param list Pointer to a List object. If NULL, this is a no-op.
///
/// @note The underlying rt_arr_obj and element references are released.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_list_finalize For complete cleanup during garbage collection
void rt_list_clear(void *list) {
    if (!list)
        return;
    rt_gc_mutator_enter();
    rt_list_impl *L = as_list(list);
    if (!L) {
        rt_gc_mutator_exit();
        return;
    }
    if (L->arr) {
        void **arr = L->arr;
        L->arr = NULL;
        rt_arr_obj_release(arr);
    }
    rt_gc_mutator_exit();
}

/// @brief Adds an element to the end of the List.
///
/// Appends a new element after the current last element. The underlying array
/// automatically grows to accommodate the new element.
///
/// **Visual example:**
/// ```
/// Before Add(D):  [A, B, C]      count=3
/// After Add(D):   [A, B, C, D]   count=4
/// ```
///
/// **Example:**
/// ```
/// Dim list = List.New()
/// list.Push("first")
/// list.Push("second")
/// list.Push("third")
/// Print list.Count  ' Outputs: 3
/// ```
///
/// @param list Pointer to a List object. If NULL, this is a no-op.
/// @param elem The element to add. Reference is retained by the List.
///
/// @note O(1) amortized time complexity. Occasional O(n) when resizing.
/// @note The List retains a reference to elem via rt_arr_obj.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_list_insert For inserting at arbitrary positions
/// @see rt_list_remove_at For removing elements
void rt_list_push(void *list, void *elem) {
    if (!list)
        return;
    rt_gc_mutator_enter();
    rt_list_impl *L = as_list(list);
    if (!L) {
        rt_gc_mutator_exit();
        return;
    }
    size_t len = rt_arr_obj_len(L->arr);

    rt_obj_retain_maybe(elem);
    void **arr2 = rt_arr_obj_resize(L->arr, len + 1);
    if (!arr2) {
        release_temp_obj(elem);
        rt_gc_mutator_exit();
        rt_trap("List.Push: memory allocation failed");
        return;
    }
    L->arr = arr2;
    L->arr[len] = elem;
    rt_gc_mutator_exit();
}

/// @brief Returns the element at the specified index.
///
/// Provides O(1) random access to any element in the List. Indices are
/// zero-based, so valid indices range from 0 to count-1.
///
/// **Example:**
/// ```
/// Dim list = List.New()
/// list.Push("a")
/// list.Push("b")
/// list.Add("c")
/// Print list.Item(0)  ' Outputs: a
/// Print list.Item(1)  ' Outputs: b
/// Print list.Item(2)  ' Outputs: c
/// ```
///
/// @param list Pointer to a List object. Must not be NULL.
/// @param index Zero-based index of the element to retrieve (0 to count-1).
///
/// @return The element at the specified index.
///
/// @note O(1) time complexity.
/// @note The returned element is retained and must be released by the caller.
/// @note Traps with "rt_list_get: null list" if list is NULL.
/// @note Traps with "rt_list_get: negative index" if index < 0.
/// @note Traps with "rt_list_get: index out of bounds" if index >= count.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_list_set For modifying an element
/// @see rt_list_len For getting the valid index range
void *rt_list_get(void *list, int64_t index) {
    if (!list) {
        rt_trap("rt_list_get: null list");
        return NULL;
    }
    if (index < 0) {
        rt_trap("rt_list_get: negative index");
        return NULL;
    }
    rt_list_impl *L = as_list(list);
    if (!L)
        return NULL;
    size_t len = rt_arr_obj_len(L->arr);
    if ((uint64_t)index >= (uint64_t)len) {
        char msg[128];
        snprintf(msg,
                 sizeof(msg),
                 "rt_list_get: index out of bounds (index=%lld, count=%llu)",
                 (long long)index,
                 (unsigned long long)len);
        rt_trap(msg);
        return NULL;
    }
    return rt_arr_obj_get(L->arr, (size_t)index);
}

/// @brief Replaces the element at the specified index.
///
/// Sets a new value at the given index. The previous element's reference is
/// released, and a reference to the new element is retained. The index must
/// refer to an existing element - this function cannot extend the List.
///
/// **Example:**
/// ```
/// Dim list = List.New()
/// list.Push("a")
/// list.Push("b")
/// list.SetItem(0, "x")
/// Print list.Item(0)  ' Outputs: x
/// Print list.Item(1)  ' Outputs: b
/// ```
///
/// @param list Pointer to a List object. Must not be NULL.
/// @param index Zero-based index of the element to modify (0 to count-1).
/// @param elem The new element to store at this index.
///
/// @note O(1) time complexity.
/// @note The old element's reference is released, the new one is retained.
/// @note Traps with "rt_list_set: null list" if list is NULL.
/// @note Traps with "rt_list_set: negative index" if index < 0.
/// @note Traps with "rt_list_set: index out of bounds" if index >= count.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_list_get For reading an element
/// @see rt_list_push For adding new elements
void rt_list_set(void *list, int64_t index, void *elem) {
    if (!list) {
        rt_trap("rt_list_set: null list");
        return;
    }
    if (index < 0) {
        rt_trap("rt_list_set: negative index");
        return;
    }
    rt_list_impl *L = as_list(list);
    if (!L)
        return;
    size_t len = rt_arr_obj_len(L->arr);
    if ((uint64_t)index >= (uint64_t)len) {
        rt_trap("rt_list_set: index out of bounds");
        return;
    }
    rt_arr_obj_put(L->arr, (size_t)index, elem);
}

/// @brief Removes the element at the specified index.
///
/// Removes the element at the given index and shifts all subsequent elements
/// one position to the left to fill the gap. The removed element's reference
/// is released.
///
/// **Visual example:**
/// ```
/// Before RemoveAt(1):  [A, B, C, D]   count=4
/// After RemoveAt(1):   [A, C, D]      count=3
/// ```
///
/// **Example:**
/// ```
/// Dim list = List.New()
/// list.Push("a")
/// list.Push("b")
/// list.Add("c")
/// list.RemoveAt(1)
/// ' list is now: [a, c]
/// Print list.Count  ' Outputs: 2
/// ```
///
/// @param list Pointer to a List object. Must not be NULL.
/// @param index Zero-based index of the element to remove (0 to count-1).
///
/// @note O(n) time complexity due to element shifting.
/// @note The removed element's reference is released.
/// @note Traps with "rt_list_remove_at: null list" if list is NULL.
/// @note Traps with "rt_list_remove_at: negative index" if index < 0.
/// @note Traps with "rt_list_remove_at: index out of bounds" if index >= count.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_list_remove For removing by element value
/// @see rt_list_push For adding elements
void rt_list_remove_at(void *list, int64_t index) {
    if (!list) {
        rt_trap("rt_list_remove_at: null list");
        return;
    }
    if (index < 0) {
        rt_trap("rt_list_remove_at: negative index");
        return;
    }
    rt_gc_mutator_enter();
    rt_list_impl *L = as_list(list);
    if (!L) {
        rt_gc_mutator_exit();
        return;
    }
    size_t len = rt_arr_obj_len(L->arr);
    if ((uint64_t)index >= (uint64_t)len) {
        rt_gc_mutator_exit();
        rt_trap("rt_list_remove_at: index out of bounds");
        return;
    }
    if (len == 0) {
        rt_gc_mutator_exit();
        return;
    }

    void *removed = L->arr[index];
    if ((size_t)index + 1 < len) {
        memmove(
            &L->arr[index], &L->arr[(size_t)index + 1], (len - (size_t)index - 1) * sizeof(void *));
    }
    L->arr[len - 1] = NULL;

    // Shrink storage. Resize-to-zero releases the backing array and returns
    // NULL, which is the empty-list state.
    void **shrunk = rt_arr_obj_resize(L->arr, len - 1);
    if (len - 1 > 0 && !shrunk) {
        if ((size_t)index + 1 < len) {
            memmove(&L->arr[(size_t)index + 1],
                    &L->arr[index],
                    (len - (size_t)index - 1) * sizeof(void *));
        }
        L->arr[index] = removed;
        rt_gc_mutator_exit();
        rt_trap("List.RemoveAt: memory allocation failed");
        return;
    }
    L->arr = shrunk;
    release_temp_obj(removed);
    rt_gc_mutator_exit();
}

/// @brief Finds the first occurrence of an element in the List.
///
/// Searches for an element using runtime boxed-value equality. Returns the
/// index of the first match, or -1 if not found.
///
/// **Comparison semantics:**
/// Boxed numeric and string values compare by content; other objects compare
/// according to `rt_box_equal()`.
///
/// **Example:**
/// ```
/// Dim list = List.New()
/// Dim obj1 = SomeObject.New()
/// Dim obj2 = SomeObject.New()
/// list.Add(obj1)
/// list.Add(obj2)
/// Print list.Find(obj1)   ' Outputs: 0
/// Print list.Find(obj2)   ' Outputs: 1
/// ```
///
/// @param list Pointer to a List object. If NULL, returns -1.
/// @param elem The element to search for (compared by content for boxed values).
///
/// @return The zero-based index of the first occurrence, or -1 if not found
///         or list is NULL.
///
/// @note O(n) time complexity - linear search from the beginning.
/// @note Boxed values are compared by content; non-boxed by pointer identity.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_list_has For boolean membership check
int64_t rt_list_find(void *list, void *elem) {
    if (!list)
        return -1;

    rt_list_impl *L = as_list(list);
    size_t len = rt_arr_obj_len(L->arr);

    for (size_t i = 0; i < len; ++i) {
        if (rt_box_equal(L->arr[i], elem))
            return (int64_t)i;
    }

    return -1;
}

/// @brief Find the first index of an element as an Option index.
/// @details Sentinel-free companion to @ref rt_list_find. A match returns
///          `SomeI64(index)` and absence returns `None`, so callers do not need
///          to reserve `-1` as an out-of-band value.
/// @param list Opaque List object pointer, or NULL.
/// @param elem Element to search for; may be NULL.
/// @return Opaque Zanna.Option containing the first index, or None.
void *rt_list_find_option(void *list, void *elem) {
    int64_t index = rt_list_find(list, elem);
    return index >= 0 ? rt_option_some_i64(index) : rt_option_none();
}

/// @brief Checks whether the List contains a specific element.
///
/// Tests if the element is present in the List using content-aware equality.
/// This is a convenience wrapper around rt_list_find that returns a boolean.
///
/// **Example:**
/// ```
/// Dim list = List.New()
/// Dim obj = SomeObject.New()
/// list.Add(obj)
/// Print list.Has(obj)     ' Outputs: True
/// Print list.Has(Nothing) ' Outputs: False
/// ```
///
/// @param list Pointer to a List object. If NULL, returns 0 (false).
/// @param elem The element to search for (compared by content for boxed values).
///
/// @return 1 (true) if the element is found, 0 (false) otherwise.
///
/// @note O(n) time complexity - linear search.
/// @note Boxed values are compared by content; non-boxed by pointer identity.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_list_find For getting the index of the element
int8_t rt_list_has(void *list, void *elem) {
    return rt_list_find(list, elem) >= 0 ? 1 : 0;
}

/// @brief Inserts an element at the specified position.
///
/// Inserts a new element at the given index, shifting all subsequent elements
/// one position to the right. Unlike SetItem, Insert grows the List by one.
///
/// **Visual example:**
/// ```
/// Before Insert(1, X):  [A, B, C]      count=3
/// After Insert(1, X):   [A, X, B, C]   count=4
/// ```
///
/// **Valid indices:**
/// - 0: Insert at the beginning (before all elements)
/// - count: Insert at the end (equivalent to Add)
/// - Any value from 0 to count (inclusive)
///
/// **Example:**
/// ```
/// Dim list = List.New()
/// list.Add("a")
/// list.Add("c")
/// list.Insert(1, "b")    ' Insert between a and c
/// ' list is now: [a, b, c]
/// ```
///
/// @param list Pointer to a List object. Must not be NULL.
/// @param index Position to insert at (0 to count inclusive).
/// @param elem The element to insert. Reference is retained by the List.
///
/// @note O(n) time complexity due to element shifting.
/// @note The List retains a reference to elem via rt_arr_obj.
/// @note Traps with "List.Insert: null list" if list is NULL.
/// @note Traps with "List.Insert: negative index" if index < 0.
/// @note Traps with "List.Insert: index out of bounds" if index > count.
/// @note Traps with "List.Insert: memory allocation failed" on allocation failure.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_list_push For appending to the end (O(1))
/// @see rt_list_remove_at For removing at an index
void rt_list_insert(void *list, int64_t index, void *elem) {
    if (!list) {
        rt_trap("List.Insert: null list");
        return;
    }
    if (index < 0) {
        rt_trap("List.Insert: negative index");
        return;
    }

    rt_gc_mutator_enter();
    rt_list_impl *L = as_list(list);
    if (!L) {
        rt_gc_mutator_exit();
        return;
    }
    size_t len = rt_arr_obj_len(L->arr);
    if ((uint64_t)index > (uint64_t)len) {
        rt_gc_mutator_exit();
        rt_trap("List.Insert: index out of bounds");
        return;
    }

    rt_obj_retain_maybe(elem);
    void **arr2 = rt_arr_obj_resize(L->arr, len + 1);
    if (!arr2) {
        release_temp_obj(elem);
        rt_gc_mutator_exit();
        rt_trap("List.Insert: memory allocation failed");
        return;
    }
    L->arr = arr2;

    if ((size_t)index < len) {
        memmove(&L->arr[(size_t)index + 1],
                &L->arr[(size_t)index],
                (len - (size_t)index) * sizeof(void *));
    }
    L->arr[(size_t)index] = elem;
    rt_gc_mutator_exit();
}

/// @brief Removes the first occurrence of an element from the List.
///
/// Searches for the element by content-aware equality and removes the first match.
/// If the element is not found, the List remains unchanged and false is returned.
///
/// **Example:**
/// ```
/// Dim list = List.New()
/// Dim obj = SomeObject.New()
/// list.Add(obj)
/// list.Add("other")
/// Print list.Remove(obj)   ' Outputs: True (removed)
/// Print list.Remove(obj)   ' Outputs: False (already removed)
/// ```
///
/// @param list Pointer to a List object. If NULL, returns 0 (false).
/// @param elem The element to remove (compared by content for boxed values).
///
/// @return 1 (true) if the element was found and removed, 0 (false) otherwise.
///
/// @note O(n) time complexity for search + O(n) for removal = O(n) total.
/// @note Only removes the first occurrence if duplicates exist.
/// @note The removed element's reference is released.
/// @note Thread safety: Not thread-safe.
///
/// @see rt_list_remove_at For removing by index
/// @see rt_list_find For finding without removing
int8_t rt_list_remove(void *list, void *elem) {
    int64_t idx = rt_list_find(list, elem);
    if (idx < 0)
        return 0;
    rt_list_remove_at(list, idx);
    return 1;
}

/// @brief Creates a new List containing a slice of elements.
///
/// Returns a new List containing elements from the specified range.
/// The original List is not modified.
///
/// **Example:**
/// ```
/// Dim list = List.New()
/// list.Push("a")
/// list.Push("b")
/// list.Add("c")
/// list.Add("d")
/// Dim slice = list.Slice(1, 3)
/// ' slice contains ["b", "c"]
/// ```
///
/// @param list  Pointer to a List object.
/// @param start Start index (inclusive, clamped to 0).
/// @param end   End index (exclusive, clamped to Count).
///
/// @return New List containing the elements in the range.
///
/// @note O(k) time where k = end - start.
/// @note Thread safety: Not thread-safe.
void *rt_list_slice(void *list, int64_t start, int64_t end) {
    void *result = rt_list_new();
    if (!result)
        return NULL;

    if (!list)
        return result;

    rt_list_impl *L = as_list(list);
    size_t len = rt_arr_obj_len(L->arr);

    // Clamp indices
    if (start < 0)
        start = 0;
    if (end < 0)
        end = 0;
    if ((size_t)start > len)
        start = (int64_t)len;
    if ((size_t)end > len)
        end = (int64_t)len;
    if (start >= end)
        return result;

    // Copy elements
    for (int64_t i = start; i < end; i++) {
        void *elem = rt_arr_obj_get(L->arr, (size_t)i);
        rt_list_push(result, elem);
        release_temp_obj(elem);
    }

    return result;
}

/// @brief Reverses the order of elements in the List in place.
///
/// Swaps elements from both ends toward the center, modifying
/// the original List.
///
/// **Example:**
/// ```
/// Dim list = List.New()
/// list.Push("a")
/// list.Push("b")
/// list.Add("c")
/// list.Reverse()
/// ' list now contains ["c", "b", "a"]
/// ```
///
/// @param list Pointer to a List object. If NULL, this is a no-op.
///
/// @note O(n) time complexity.
/// @note Thread safety: Not thread-safe.
void rt_list_reverse(void *list) {
    if (!list)
        return;

    rt_gc_mutator_enter();
    rt_list_impl *L = as_list(list);
    if (!L) {
        rt_gc_mutator_exit();
        return;
    }
    size_t len = rt_arr_obj_len(L->arr);
    if (len < 2) {
        rt_gc_mutator_exit();
        return;
    }

    // Swap elements from both ends toward center
    for (size_t i = 0; i < len / 2; i++) {
        size_t j = len - 1 - i;
        void *a = L->arr[i];
        void *b = L->arr[j];
        // Direct swap without reference counting (elements stay in list)
        L->arr[i] = b;
        L->arr[j] = a;
    }
    rt_gc_mutator_exit();
}

/// @brief Returns the first element in the List.
///
/// **Example:**
/// ```
/// Dim list = List.New()
/// list.Push("a")
/// list.Push("b")
/// Print list.First()  ' Outputs: a
/// ```
///
/// @param list Pointer to a List object.
///
/// @return The first element, or NULL if the List is empty or NULL.
///
/// @note O(1) time complexity.
/// @note A non-null result is retained and must be released by the caller.
/// @note Thread safety: Not thread-safe.
void *rt_list_first(void *list) {
    if (!list)
        return NULL;

    rt_list_impl *L = as_list(list);
    size_t len = rt_arr_obj_len(L->arr);
    if (len == 0)
        return NULL;

    return rt_arr_obj_get(L->arr, 0);
}

/// @brief Returns the last element in the List.
///
/// **Example:**
/// ```
/// Dim list = List.New()
/// list.Push("a")
/// list.Push("b")
/// Print list.Last()  ' Outputs: b
/// ```
///
/// @param list Pointer to a List object.
///
/// @return The last element, or NULL if the List is empty or NULL.
///
/// @note O(1) time complexity.
/// @note A non-null result is retained and must be released by the caller.
/// @note Thread safety: Not thread-safe.
void *rt_list_last(void *list) {
    if (!list)
        return NULL;

    rt_list_impl *L = as_list(list);
    size_t len = rt_arr_obj_len(L->arr);
    if (len == 0)
        return NULL;

    return rt_arr_obj_get(L->arr, len - 1);
}

/// @brief Tests whether a List contains no elements.
/// @param list List handle, or NULL.
/// @return 1 if empty or NULL; otherwise 0.
int8_t rt_list_is_empty(void *list) {
    if (!list)
        return 1;
    return rt_list_len(list) == 0 ? 1 : 0;
}

/// @brief Removes and returns the final List element.
/// @param list Non-null List handle.
/// @return Removed element as a retained caller-owned reference.
/// @note A null or empty List traps. If shrinking storage fails, the element is
///       restored and the operation traps without changing the List.
void *rt_list_pop(void *list) {
    if (!list) {
        rt_trap("List.Pop: null list");
        return NULL;
    }

    rt_gc_mutator_enter();
    rt_list_impl *L = as_list(list);
    if (!L) {
        rt_gc_mutator_exit();
        return NULL;
    }
    size_t len = rt_arr_obj_len(L->arr);
    if (len == 0) {
        rt_gc_mutator_exit();
        rt_trap("List.Pop: list is empty");
        return NULL;
    }

    void *elem = rt_arr_obj_get(L->arr, len - 1);
    rt_arr_obj_put(L->arr, len - 1, NULL);
    void **shrunk = rt_arr_obj_resize(L->arr, len - 1);
    if (len - 1 > 0 && !shrunk) {
        rt_arr_obj_put(L->arr, len - 1, elem);
        release_temp_obj(elem);
        rt_gc_mutator_exit();
        rt_trap("List.Pop: memory allocation failed");
        return NULL;
    }
    L->arr = shrunk;
    rt_gc_mutator_exit();
    return elem;
}

//=============================================================================
// Sorting
//=============================================================================

/// @brief Default comparison for list elements.
/// @details Delegates to the shared collection order (VDOC-089): type-class
///          rank first (NULL < numeric < string < other), then value order
///          within class, so the relation is total and transitive.
/// @param a First element.
/// @param b Second element.
/// @return Negative, zero, or positive according to the shared ascending order.
static int64_t list_default_compare(void *a, void *b) {
    return rt_box_default_sort_compare(a, b);
}

/// @brief Merge two sorted halves of a temp array.
/// @param items Array being sorted.
/// @param temp Scratch array with at least the same extent.
/// @param left Inclusive first index of the left half.
/// @param mid Inclusive final index of the left half.
/// @param right Inclusive final index of the right half.
/// @param cmp Comparator defining ascending order.
static void list_merge(void **items,
                       void **temp,
                       size_t left,
                       size_t mid,
                       size_t right,
                       int64_t (*cmp)(void *, void *)) {
    size_t i = left, j = mid + 1, k = left;

    while (i <= mid && j <= right) {
        if (cmp(items[i], items[j]) <= 0)
            temp[k++] = items[i++];
        else
            temp[k++] = items[j++];
    }
    while (i <= mid)
        temp[k++] = items[i++];
    while (j <= right)
        temp[k++] = items[j++];

    memcpy(items + left, temp + left, (right - left + 1) * sizeof(void *));
}

/// @brief Recursive merge sort.
/// @param items Array being sorted.
/// @param temp Scratch array with at least the same extent.
/// @param left Inclusive first index.
/// @param right Inclusive final index.
/// @param cmp Comparator defining ascending order.
static void list_merge_sort(
    void **items, void **temp, size_t left, size_t right, int64_t (*cmp)(void *, void *)) {
    if (left >= right)
        return;
    size_t mid = left + (right - left) / 2;
    list_merge_sort(items, temp, left, mid, cmp);
    list_merge_sort(items, temp, mid + 1, right, cmp);
    list_merge(items, temp, left, mid, right, cmp);
}

/// @brief Sort a list in-place using a comparison function.
/// @details Sorts the backing array directly (like Seq.Sort) to avoid ref
///          counting side effects from rt_arr_obj_get/put during rearrangement.
/// @param list List handle, or NULL for a no-op.
/// @param cmp Comparator defining the desired order.
/// @note Comparator traps are recovered long enough to free scratch storage,
///       then raised again with the original message.
static void list_sort_impl(void *list, int64_t (*cmp)(void *, void *)) {
    if (!list)
        return;

    rt_gc_mutator_enter();
    rt_list_impl *L = as_list(list);
    if (!L) {
        rt_gc_mutator_exit();
        return;
    }
    size_t len = L->arr ? rt_arr_obj_len(L->arr) : 0;
    if (len <= 1) {
        rt_gc_mutator_exit();
        return;
    }

    // Allocate temporary buffer for merge sort
    if (len > SIZE_MAX / sizeof(void *)) {
        rt_gc_mutator_exit();
        rt_trap("List.Sort: scratch size overflow");
        return;
    }
    void **temp = (void **)malloc(len * sizeof(void *));
    if (!temp) {
        rt_gc_mutator_exit();
        rt_trap("List.Sort: memory allocation failed");
        return;
    }
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        const char *error = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 error && error[0] ? error : "List.Sort: comparator trapped");
        rt_trap_clear_recovery();
        free(temp);
        rt_gc_mutator_exit();
        rt_trap(saved_error);
        return;
    }

    // Sort the backing array in-place (same approach as Seq.Sort)
    list_merge_sort(L->arr, temp, 0, len - 1, cmp);

    rt_trap_clear_recovery();
    free(temp);
    rt_gc_mutator_exit();
}

/// @brief Stably sorts a List using the shared ascending runtime order.
/// @param list List handle, or NULL for a no-op.
void rt_list_sort(void *list) {
    list_sort_impl(list, list_default_compare);
}

/// @brief Descending comparison wrapper.
/// @param a First element.
/// @param b Second element.
/// @return The inverse of the shared ascending comparison.
static int64_t list_compare_desc(void *a, void *b) {
    return -list_default_compare(a, b);
}

/// @brief Stably sorts a List using the shared descending runtime order.
/// @param list List handle, or NULL for a no-op.
void rt_list_sort_desc(void *list) {
    list_sort_impl(list, list_compare_desc);
}

/// @brief Randomly permutes List elements in place with Fisher-Yates.
/// @param list List handle, or NULL for a no-op.
/// @note Uses the active runtime random-number generator.
void rt_list_shuffle(void *list) {
    if (!list)
        return;

    rt_gc_mutator_enter();
    rt_list_impl *L = as_list(list);
    if (!L) {
        rt_gc_mutator_exit();
        return;
    }
    size_t len = L->arr ? rt_arr_obj_len(L->arr) : 0;
    if (len < 2) {
        rt_gc_mutator_exit();
        return;
    }

    // Fisher-Yates shuffle using the active runtime RNG.
    for (size_t i = len - 1; i > 0; --i) {
        size_t j = (size_t)rt_rand_range(0, (long long)i);
        void *a = L->arr[i];
        void *b = L->arr[j];
        L->arr[i] = b;
        L->arr[j] = a;
    }
    rt_gc_mutator_exit();
}

/// @brief Creates a shallow, independently mutable copy of a List.
/// @param list Source List, or NULL.
/// @return New List retaining the same element pointers in order; NULL source
///         produces an empty List.
void *rt_list_clone(void *list) {
    void *result = rt_list_new();
    if (!result || !list)
        return result;

    rt_list_impl *L = as_list(list);
    size_t len = L->arr ? rt_arr_obj_len(L->arr) : 0;

    for (size_t i = 0; i < len; ++i) {
        void *elem = rt_arr_obj_get(L->arr, i);
        rt_list_push(result, elem);
        release_temp_obj(elem);
    }

    return result;
}
