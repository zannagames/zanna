//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements an unboxed append-only-stack/list surface for i64 values.
/// @details Logical length and reserved capacity live in the shared array heap
///          header. Push writes in place while capacity remains and otherwise
///          allocates a doubled backing array, copies elements, releases the old
///          handle, and rebinds the caller's pointer. Pop and peek trap on an
///          empty list and never shrink capacity.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/arrays/rt_list_i64.c
// Purpose: Implements a dynamic-append list of 64-bit integers without boxing
//          (P2-3.7 optimization). Provides push, get, set, len, and clear
//          operations backed by the runtime heap with amortized O(1) push.
//
// Key invariants:
//   - len <= cap at all times; the heap header tracks both independently.
//   - Push is amortized O(1): capacity doubles when exhausted.
//   - Minimum initial capacity is RT_LIST_I64_MIN_CAP (8) elements.
//   - Refcount is exactly 1 immediately after rt_list_i64_new.
//   - No boxing overhead: values are stored as raw int64_t, not rt_object.
//   - Out-of-bounds get/set delegate to rt_arr_oob_panic and abort.
//
// Ownership/Lifetime:
//   - Caller owns the initial reference returned by rt_list_i64_new.
//   - Push may reallocate the backing buffer; callers must use the pointer
//     returned by push (or re-query after push) rather than caching the old one.
//   - The heap allocator manages deallocation when the refcount reaches zero.
//
// Links: src/runtime/arrays/rt_list_i64.h (public API),
//        src/runtime/arrays/rt_array.h (oob_panic helper),
//        src/runtime/rt_heap.h (heap alloc, set_len, hdr)
//
//===----------------------------------------------------------------------===//

#include "rt_list_i64.h"

#include "rt_array.h" // rt_arr_oob_panic
#include "rt_heap.h"  // rt_heap_alloc, rt_heap_hdr, rt_heap_set_len

#include <assert.h>
#include <string.h>

/// Minimum allocation capacity when init_cap is zero.
#define RT_LIST_I64_MIN_CAP 8

/// @brief Allocate an empty int64 list with pre-reserved capacity.
/// @details Allocates via the runtime heap with len=0 and cap=max(init_cap,8).
///          The first push will not allocate as long as the initial capacity
///          has not been exhausted.
/// @param init_cap Desired initial capacity in elements; 0 defaults to 8.
/// @return List payload pointer (element 0 address), or NULL on failure.
int64_t *rt_list_i64_new(size_t init_cap) {
    size_t cap = init_cap ? init_cap : RT_LIST_I64_MIN_CAP;
    return (int64_t *)rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_I64, sizeof(int64_t), 0, cap);
}

/// @brief Append @p val to the list, growing the buffer if needed.
/// @details Fast path: len < cap — write directly and bump len in the header.
///          Slow path: cap exhausted — allocate a new buffer with 2× capacity,
///          copy existing elements, write @p val, release the old buffer, and
///          update @p *list_inout with the new address.
/// @param list_inout Non-null pointer to the list payload; updated on realloc.
/// @param val Value to append.
/// @return 0 on success; -1 when the slow-path allocation fails.
int rt_list_i64_push(int64_t **list_inout, int64_t val) {
    assert(list_inout && *list_inout);

    int64_t *arr = *list_inout;
    rt_heap_hdr_t *hdr = rt_heap_hdr(arr);
    size_t len = hdr->len;
    size_t cap = hdr->cap;

    if (len < cap) {
        // Fast path: capacity available — no allocation needed.
        arr[len] = val;
        rt_heap_set_len(arr, len + 1);
        return 0;
    }

    // Slow path: double the capacity (or bootstrap to the minimum).
    size_t new_cap = cap ? cap * 2 : RT_LIST_I64_MIN_CAP;
    int64_t *new_arr =
        (int64_t *)rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_I64, sizeof(int64_t), 0, new_cap);
    if (!new_arr)
        return -1;

    if (len)
        memcpy(new_arr, arr, len * sizeof(int64_t));

    new_arr[len] = val;
    rt_heap_set_len(new_arr, len + 1);

    // Release old buffer and redirect the caller's pointer.
    rt_arr_i64_release(arr);
    *list_inout = new_arr;
    return 0;
}

/// @brief Remove and return the last element.
/// @details Traps via rt_arr_oob_panic when the list is empty. Decrements len
///          in the header; does not release capacity.
/// @param list_inout Non-null pointer to the list payload.
/// @return Value of the removed element.
int64_t rt_list_i64_pop(int64_t **list_inout) {
    assert(list_inout && *list_inout);

    int64_t *arr = *list_inout;
    rt_heap_hdr_t *hdr = rt_heap_hdr(arr);
    size_t len = hdr->len;

    if (len == 0)
        rt_arr_oob_panic(0, 0);

    int64_t val = arr[len - 1];
    rt_heap_set_len(arr, len - 1);
    return val;
}

/// @brief Return the last element without removing it.
/// @details Traps via rt_arr_oob_panic when the list is empty.
/// @param list Non-null list payload pointer.
/// @return Value of the last element.
int64_t rt_list_i64_peek(int64_t *list) {
    assert(list);

    rt_heap_hdr_t *hdr = rt_heap_hdr(list);
    size_t len = hdr->len;

    if (len == 0)
        rt_arr_oob_panic(0, 0);

    return list[len - 1];
}
