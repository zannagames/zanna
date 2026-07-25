//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements garbage-collector-aware arrays of strong object references.
/// @details Each populated slot owns one retained object reference. Mutation is
///          permitted only for unique backing storage, copy-on-write resize
///          retains elements into a fresh allocation for shared arrays, and
///          final array release drops all outgoing references exactly once.
///          Growth uses amortized capacity while shrink clears truncated slots
///          under the runtime GC mutator protocol.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/arrays/rt_array_obj.c
// Purpose: Implements dynamic arrays of object references (void* elements) for
//          generic collections. Each element is a runtime-managed object handle;
//          the array retains elements on insertion and releases them on overwrite
//          or teardown.
//
// Key invariants:
//   - The array retains a reference to each stored object; callers keep ownership
//     of, and remain responsible for, their original reference.
//   - Overwriting publishes the retained new object before releasing the old one.
//   - Array teardown releases all held object references exactly once.
//   - The array itself is reference-counted through the heap allocator.
//   - Object arrays are GC-tracked and enumerate every live slot as a strong edge.
//   - Out-of-bounds accesses trigger rt_arr_oob_panic and abort.
//
// Ownership/Lifetime:
//   - The array holds strong references to all stored objects.
//   - The array itself is heap-allocated and reference-counted.
//   - Normal final release drops every element; cycle collection releases only
//     outgoing references that leave the reclaimed cycle.
//
// Links: src/runtime/arrays/rt_array_obj.h (public API),
//        src/runtime/arrays/rt_array.h (int32 base module, oob_panic),
//        src/runtime/oop/rt_object.h (object retain/release)
//
//===----------------------------------------------------------------------===//

#include "rt_array_obj.h"

#include "rt_array.h" // for rt_arr_oob_panic
#include "rt_gc.h"
#include "rt_heap.h"
#include "rt_object.h"
#include "rt_platform.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "rt_trap.h"

/// @brief Return the heap header associated with an object array payload.
/// @details The payload pointer is the first element of the array; the header
///          is stored immediately before it in the heap allocation.
/// @param payload Array payload pointer (element 0) or NULL.
/// @return Heap header pointer, or NULL if @p payload is NULL.
static rt_heap_hdr_t *rt_arr_obj_hdr(void **payload) {
    return payload ? rt_heap_hdr((void *)payload) : NULL;
}

/// @brief Validate that a heap header describes an object array.
/// @details Raises the standard object-array corruption trap when @p hdr is
///          missing or names a different heap allocation kind. The return
///          value lets callers stop immediately when a trap hook recovers
///          instead of continuing to read the corrupted header.
/// @param hdr Heap header to validate.
/// @return Non-zero when @p hdr is a valid object-array header; zero after
///         reporting a validation failure.
static int rt_arr_obj_header_valid(rt_heap_hdr_t *hdr) {
    if (!hdr || hdr->kind != RT_HEAP_ARRAY || hdr->elem_kind != RT_ELEM_OBJ) {
        rt_trap("rt_array_obj: corrupted header");
        return 0;
    }
    return 1;
}

/// @brief Return whether an object-array payload has multiple owners.
/// @details Shared object arrays must not be mutated in place because each
///          slot owns one retained element reference on behalf of the shared
///          backing allocation. Mutating a shared backing would alter aliases
///          without transferring element references correctly.
/// @param hdr Valid object-array heap header.
/// @return Non-zero when the backing allocation has more than one owner.
static int rt_arr_obj_is_shared(const rt_heap_hdr_t *hdr) {
    return hdr && __atomic_load_n(&hdr->refcnt, __ATOMIC_ACQUIRE) > 1;
}

/// @brief Assert that a heap header describes an object array.
/// @details Keeps debug assertions local to this module while using the
///          recoverable validator first, so release builds and trap-recovery
///          tests share the same safety behavior.
/// @param hdr Heap header to validate.
static void rt_arr_obj_assert_header(rt_heap_hdr_t *hdr) {
    if (!hdr || hdr->kind != RT_HEAP_ARRAY || hdr->elem_kind != RT_ELEM_OBJ)
        return;
    assert(hdr);
    assert(hdr->kind == RT_HEAP_ARRAY);
    assert(hdr->elem_kind == RT_ELEM_OBJ);
}

/// @brief Release one object-array element and free it if its refcount reaches zero.
/// @details Centralizes the retain/release handoff used by resize and teardown paths so
///          shrink rollback paths can decide exactly when truncated references are released.
/// @param p Runtime object reference stored in an array slot, or NULL for an empty slot.
static void rt_arr_obj_release_element(void *p) {
    if (p && rt_obj_release_check0(p))
        rt_obj_free(p);
}

/// @brief Copy retained object references into a fresh array.
/// @details Each non-NULL element copied from @p src is retained before being
///          stored in @p dst so the destination array owns independent strong
///          references. If a recovering trap hook returns after a retain error,
///          the partially-copied destination remains valid and can be released
///          normally by the caller.
/// @param dst Destination object-array payload.
/// @param src Source object-array payload.
/// @param count Number of elements to copy.
static void rt_arr_obj_copy_retained(void **dst, void **src, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        void *value = src[i];
        if (value)
            rt_obj_retain_maybe(value);
        dst[i] = value;
    }
}

/// @brief Compute an amortized-growth capacity for a resizable object array.
/// @details Capacities start at sixteen slots and double until @p required_len
///          fits. If doubling would overflow `size_t`, the exact required
///          length is used after validating that its byte representation fits
///          in a heap allocation. This keeps repeated List append operations
///          amortized O(1) without weakening the heap allocator's overflow
///          guarantees.
/// @param current_cap Existing backing capacity, or zero for a new array.
/// @param required_len Minimum logical length the result must hold.
/// @param out_cap Receives the validated capacity on success.
/// @return Non-zero on success; zero when the requested capacity cannot be
///         represented safely.
static int rt_arr_obj_growth_capacity(size_t current_cap, size_t required_len, size_t *out_cap) {
    const size_t min_capacity = 16;
    size_t cap = current_cap;

    if (!out_cap)
        return 0;
    if (required_len == 0) {
        *out_cap = 0;
        return 1;
    }
    if (cap < min_capacity)
        cap = min_capacity;
    while (cap < required_len) {
        if (cap > SIZE_MAX / 2) {
            cap = required_len;
            break;
        }
        cap *= 2;
    }
    if (cap > (SIZE_MAX - sizeof(rt_heap_hdr_t)) / sizeof(void *))
        return 0;
    *out_cap = cap;
    return 1;
}

/// @brief Allocate a zero-filled object array with independent length and capacity.
/// @details This internal constructor is used by the resizable collection path,
///          while @ref rt_arr_obj_new preserves its exact-capacity constructor
///          contract. The allocation is registered as an object-reference array
///          by the heap and GC layers.
/// @param len Initial logical length.
/// @param cap Backing capacity, which must be at least @p len.
/// @return Array payload on success, or NULL after an invalid size or allocation
///         failure.
static void **rt_arr_obj_alloc_with_capacity(size_t len, size_t cap) {
    if (cap < len || cap > (SIZE_MAX - sizeof(rt_heap_hdr_t)) / sizeof(void *))
        return NULL;
    void **arr = (void **)rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_OBJ, sizeof(void *), len, cap);
    if (arr && cap)
        memset(arr, 0, cap * sizeof(void *));
    return arr;
}

/// @brief Allocate a new object array with logical length @p len.
/// @details The payload is zero-initialized so all elements start as NULL.
///          The returned pointer is the payload (element 0), not the header.
/// @param len Number of elements to allocate.
/// @return Payload pointer for the new array, or NULL on allocation failure.
void **rt_arr_obj_new(size_t len) {
    return rt_arr_obj_alloc_with_capacity(len, len);
}

/// @brief Return the logical length of the object array.
/// @details A NULL array is treated as length zero for convenience.
/// @param arr Object array payload pointer (may be NULL).
/// @return Number of elements in the array, or 0 if @p arr is NULL.
size_t rt_arr_obj_len(void **arr) {
    if (!arr)
        return 0;
    rt_heap_hdr_t *hdr = rt_arr_obj_hdr(arr);
    if (!rt_arr_obj_header_valid(hdr))
        return 0;
    rt_arr_obj_assert_header(hdr);
    return hdr->len;
}

/// @brief Retrieve an element and retain it for the caller.
/// @details The returned object reference is retained so the caller owns a
///          reference independent of subsequent array mutations. The function
///          asserts that @p arr is non-NULL and @p idx is in range.
/// @param arr Object array payload pointer (must be non-NULL).
/// @param idx Element index to read.
/// @return Retained object pointer (may be NULL if the slot is empty).
void *rt_arr_obj_get(void **arr, size_t idx) {
    if (!arr) {
        rt_trap("rt_arr_obj_get: null array");
        return NULL;
    }
    rt_heap_hdr_t *hdr = rt_arr_obj_hdr(arr);
    if (!rt_arr_obj_header_valid(hdr))
        return NULL;
    rt_arr_obj_assert_header(hdr);
    if (idx >= hdr->len)
        rt_arr_oob_panic(idx, hdr->len);
    if (idx >= hdr->len)
        return NULL;
    void *p = arr[idx];
    rt_obj_retain_maybe(p);
    return p;
}

/// @brief Store an object reference at the specified index.
/// @details Retains the new object before overwriting the slot so self-assignment
///          is safe. Releases the previous object reference and frees it if its
///          reference count drops to zero. The function asserts that @p arr is
///          non-NULL and @p idx is in range.
/// @param arr Object array payload pointer (must be non-NULL).
/// @param idx Element index to update.
/// @param obj Object reference to store (may be NULL).
void rt_arr_obj_put(void **arr, size_t idx, void *obj) {
    if (!arr) {
        rt_trap("rt_arr_obj_put: null array");
        return;
    }
    rt_gc_mutator_enter();
    rt_heap_hdr_t *hdr = rt_arr_obj_hdr(arr);
    if (!rt_arr_obj_header_valid(hdr)) {
        rt_gc_mutator_exit();
        return;
    }
    rt_arr_obj_assert_header(hdr);
    if (rt_arr_obj_is_shared(hdr)) {
        rt_gc_mutator_exit();
        rt_trap("rt_arr_obj_put: cannot mutate shared array");
        return;
    }
    if (idx >= hdr->len)
        rt_arr_oob_panic(idx, hdr->len);
    if (idx >= hdr->len) {
        rt_gc_mutator_exit();
        return;
    }

    // Retain new first to handle self-assignment safely
    rt_obj_retain_maybe(obj);

    void *old = arr[idx];
    arr[idx] = obj;
    if (old) {
        if (rt_obj_release_check0(old))
            rt_obj_free(old);
    }
    rt_gc_mutator_exit();
}

/// @brief Resize an object array to the requested length.
/// @details Growth uses a minimum sixteen-slot, doubling capacity policy so
///          repeated appends are amortized O(1); new logical slots are always
///          zero-initialized. Shrinking clears and releases truncated elements
///          but retains the backing capacity, avoiding an allocation on every
///          List removal. Resizing to zero preserves the historical contract of
///          releasing the array and returning NULL. A shared backing allocation
///          is copied before mutation so aliases retain their original contents.
/// @param arr Existing array payload pointer (may be NULL).
/// @param len New logical length.
/// @return Payload pointer for the resized array; `NULL` represents either a
///         successful resize to zero or failure.
void **rt_arr_obj_resize(void **arr, size_t len) {
    if (!arr) {
        size_t cap = 0;
        if (!rt_arr_obj_growth_capacity(0, len, &cap))
            return NULL;
        return rt_arr_obj_alloc_with_capacity(len, cap);
    }

    rt_gc_mutator_enter();
    rt_heap_hdr_t *hdr = rt_arr_obj_hdr(arr);
    if (!rt_arr_obj_header_valid(hdr)) {
        rt_gc_mutator_exit();
        return NULL;
    }
    rt_arr_obj_assert_header(hdr);

    size_t old_len = hdr->len;
    if (len == 0) {
        rt_arr_obj_release(arr);
        rt_gc_mutator_exit();
        return NULL;
    }

    if (rt_arr_obj_is_shared(hdr)) {
        size_t fresh_cap = hdr->cap;
        if (fresh_cap < len && !rt_arr_obj_growth_capacity(fresh_cap, len, &fresh_cap)) {
            rt_gc_mutator_exit();
            return NULL;
        }
        if (fresh_cap < len)
            fresh_cap = len;
        void **fresh = rt_arr_obj_alloc_with_capacity(len, fresh_cap);
        if (!fresh) {
            rt_gc_mutator_exit();
            return NULL;
        }
        size_t copy_len = old_len < len ? old_len : len;
        rt_arr_obj_copy_retained(fresh, arr, copy_len);
        rt_arr_obj_release(arr);
        rt_gc_mutator_exit();
        return fresh;
    }

    if (len < old_len) {
        for (size_t i = len; i < old_len; ++i) {
            void *truncated = arr[i];
            arr[i] = NULL;
            rt_arr_obj_release_element(truncated);
        }
        rt_heap_set_len(arr, len);
        rt_gc_mutator_exit();
        return arr;
    }

    if (len == old_len) {
        rt_gc_mutator_exit();
        return arr;
    }

    if (len <= hdr->cap) {
        memset(arr + old_len, 0, (len - old_len) * sizeof(void *));
        rt_heap_set_len(arr, len);
        rt_gc_mutator_exit();
        return arr;
    }

    size_t new_cap = 0;
    if (!rt_arr_obj_growth_capacity(hdr->cap, len, &new_cap)) {
        rt_gc_mutator_exit();
        return NULL;
    }

    void **payload = (void **)rt_heap_realloc(arr, sizeof(void *), len, new_cap);
    rt_gc_mutator_exit();
    return payload;
}

/// @brief Release all elements and the array itself.
/// @details Each non-NULL element is released and freed when its reference count
///          drops to zero. The array payload is then released via the heap API.
/// @param arr Object array payload pointer (may be NULL).
void rt_arr_obj_release(void **arr) {
    if (!arr)
        return;
    rt_gc_mutator_enter();
    rt_heap_hdr_t *hdr = rt_arr_obj_hdr(arr);
    if (!rt_arr_obj_header_valid(hdr)) {
        rt_gc_mutator_exit();
        return;
    }
    rt_arr_obj_assert_header(hdr);

    size_t n = hdr->len;
    size_t refs = rt_heap_release_deferred(arr);
    if (refs != 0) {
        rt_gc_mutator_exit();
        return;
    }

    for (size_t i = 0; i < n; ++i) {
        void *p = arr[i];
        if (p) {
            rt_arr_obj_release_element(p);
            arr[i] = NULL;
        }
    }
    rt_heap_free_zero_ref(arr);
    rt_gc_mutator_exit();
}
