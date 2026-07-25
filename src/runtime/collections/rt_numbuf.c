//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_numbuf.c
// Purpose: Implements packed F64Buffer and I64Buffer collections over existing
//          refcounted primitive array payloads.
//
// Key invariants:
//   - Each runtime object owns exactly one hidden-header primitive-array
//     payload whose length is fixed except when CopyFrom resizes it.
//   - Indices are zero-based and checked; slices clamp both endpoints to the
//     source length and never share payload storage.
//   - I64 mutation and reduction detect signed overflow; F64 operations use
//     native IEEE-754 arithmetic.
//   - Seq/List conversions box each packed element into independently retained
//     runtime values.
//
// Ownership/Lifetime:
//   - Buffer wrappers are GC-managed and release their primitive-array payload
//     from a finalizer.
//   - Constructors, slices, and collection conversions return fresh
//     runtime-managed objects.
//
// Links: src/runtime/collections/rt_numbuf.h (public C ABI),
//        src/runtime/arrays/rt_array_f64.h,
//        src/runtime/arrays/rt_array_i64.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements packed mutable buffers for 64-bit floats and integers.
/// @details The wrappers reuse the runtime's refcounted primitive-array
///          payloads to avoid per-element boxing during numeric work. Public
///          adapters validate runtime object kinds, translate signed ABI
///          lengths and indices to native sizes, and box only when exporting
///          to general-purpose `List` or `Seq` collections.

#include "rt_numbuf.h"

#include "rt_array_f64.h"
#include "rt_array_i64.h"
#include "rt_box.h"
#include "rt_collection_ids.h"
#include "rt_internal.h"
#include "rt_list.h"
#include "rt_object.h"
#include "rt_seq.h"

#include <stddef.h>
#include <stdint.h>

/// @brief GC-managed wrapper around one packed double-array payload.
typedef struct rt_f64buf_impl {
    void **vptr;
    double *arr;
} rt_f64buf_impl;

/// @brief GC-managed wrapper around one packed signed-64-bit array payload.
typedef struct rt_i64buf_impl {
    void **vptr;
    int64_t *arr;
} rt_i64buf_impl;

/// @brief Validate and convert a signed public length to `size_t`.
/// @param len Requested element count.
/// @param what Diagnostic emitted when @p len is negative.
/// @return Native nonnegative length, or 0 after trapping.
static size_t checked_len_from_i64(int64_t len, const char *what) {
    if (len < 0) {
        rt_trap(what);
        return 0;
    }
    return (size_t)len;
}

/// @brief Validate and convert a signed public index to `size_t`.
/// @details Upper-bound validation remains the responsibility of the primitive
///          array accessor because it knows the payload length.
/// @param index Requested zero-based index.
/// @param what Diagnostic emitted when @p index is negative.
/// @return Native nonnegative index, or 0 after trapping.
static size_t checked_index_from_i64(int64_t index, const char *what) {
    if (index < 0) {
        rt_trap(what);
        return 0;
    }
    return (size_t)index;
}

/// @brief Release a temporary runtime object and free it when unreferenced.
/// @param obj Runtime-managed object, or `NULL` for a no-op.
static void release_temp_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Finalize an F64Buffer by releasing its primitive-array payload.
/// @param obj Buffer object being finalized; `NULL` is ignored.
static void rt_f64buf_finalize(void *obj) {
    if (!obj)
        return;
    rt_f64buf_impl *buf = (rt_f64buf_impl *)obj;
    rt_arr_f64_release(buf->arr);
    buf->arr = NULL;
}

/// @brief Finalize an I64Buffer by releasing its primitive-array payload.
/// @param obj Buffer object being finalized; `NULL` is ignored.
static void rt_i64buf_finalize(void *obj) {
    if (!obj)
        return;
    rt_i64buf_impl *buf = (rt_i64buf_impl *)obj;
    rt_arr_i64_release(buf->arr);
    buf->arr = NULL;
}

/// @brief Checked cast from an opaque runtime handle to F64Buffer storage.
/// @param obj Handle expected to identify an F64Buffer.
/// @param what Diagnostic emitted when the handle has the wrong runtime kind.
/// @return Validated implementation pointer, or `NULL` after trapping.
static rt_f64buf_impl *as_f64buf(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_F64BUFFER_CLASS_ID, sizeof(rt_f64buf_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_f64buf_impl *)obj;
}

/// @brief Checked cast from an opaque runtime handle to I64Buffer storage.
/// @param obj Handle expected to identify an I64Buffer.
/// @param what Diagnostic emitted when the handle has the wrong runtime kind.
/// @return Validated implementation pointer, or `NULL` after trapping.
static rt_i64buf_impl *as_i64buf(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_I64BUFFER_CLASS_ID, sizeof(rt_i64buf_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_i64buf_impl *)obj;
}

/// @brief Allocate a zero-filled F64Buffer with a validated native length.
/// @details Installs the payload finalizer before reporting any primitive-array
///          allocation failure, so the partial wrapper can be released safely.
/// @param len Number of packed doubles to allocate.
/// @return New runtime-managed buffer, or `NULL` after an allocation trap.
static void *alloc_f64buf_with_len(size_t len) {
    rt_f64buf_impl *buf =
        (rt_f64buf_impl *)rt_obj_new_i64(RT_F64BUFFER_CLASS_ID, (int64_t)sizeof(rt_f64buf_impl));
    if (!buf) {
        rt_trap("F64Buffer.New: memory allocation failed");
        return NULL;
    }
    buf->vptr = NULL;
    buf->arr = rt_arr_f64_new(len);
    rt_obj_set_finalizer(buf, rt_f64buf_finalize);
    if (!buf->arr) {
        release_temp_obj(buf);
        rt_trap("F64Buffer.New: memory allocation failed");
        return NULL;
    }
    return buf;
}

/// @brief Allocate a zero-filled I64Buffer with a validated native length.
/// @details Installs the payload finalizer before reporting any primitive-array
///          allocation failure, so the partial wrapper can be released safely.
/// @param len Number of packed signed integers to allocate.
/// @return New runtime-managed buffer, or `NULL` after an allocation trap.
static void *alloc_i64buf_with_len(size_t len) {
    rt_i64buf_impl *buf =
        (rt_i64buf_impl *)rt_obj_new_i64(RT_I64BUFFER_CLASS_ID, (int64_t)sizeof(rt_i64buf_impl));
    if (!buf) {
        rt_trap("I64Buffer.New: memory allocation failed");
        return NULL;
    }
    buf->vptr = NULL;
    buf->arr = rt_arr_i64_new(len);
    rt_obj_set_finalizer(buf, rt_i64buf_finalize);
    if (!buf->arr) {
        release_temp_obj(buf);
        rt_trap("I64Buffer.New: memory allocation failed");
        return NULL;
    }
    return buf;
}

/// @brief Normalize a half-open slice range to valid payload offsets.
/// @details Each endpoint is clamped independently to `[0, len]`; when the
///          normalized end precedes the start, the end is raised to the start
///          so the result is empty rather than reversed.
/// @param len Source payload length.
/// @param start Requested inclusive start index.
/// @param end Requested exclusive end index.
/// @param out_start Receives the normalized inclusive offset.
/// @param out_end Receives the normalized exclusive offset.
static void clamp_slice(
    size_t len, int64_t start, int64_t end, size_t *out_start, size_t *out_end) {
    int64_t clamped_start = start < 0 ? 0 : start;
    int64_t clamped_end = end < 0 ? 0 : end;
    if ((uint64_t)clamped_start > len)
        clamped_start = (int64_t)len;
    if ((uint64_t)clamped_end > len)
        clamped_end = (int64_t)len;
    if (clamped_end < clamped_start)
        clamped_end = clamped_start;
    *out_start = (size_t)clamped_start;
    *out_end = (size_t)clamped_end;
}

/// @brief Allocate a zero-filled packed double buffer.
/// @param len Requested element count; negative values trap.
/// @return New runtime-managed F64Buffer.
void *rt_f64buf_new(int64_t len) {
    return alloc_f64buf_with_len(checked_len_from_i64(len, "F64Buffer.New: negative length"));
}

/// @brief Allocate a zero-filled packed signed-integer buffer.
/// @param len Requested element count; negative values trap.
/// @return New runtime-managed I64Buffer.
void *rt_i64buf_new(int64_t len) {
    return alloc_i64buf_with_len(checked_len_from_i64(len, "I64Buffer.New: negative length"));
}

/// @brief Convert a general `Seq` of boxed numeric values to packed doubles.
/// @details Boxed F64 values are copied directly and boxed I64 values are
///          converted with the language's native integer-to-double cast.
///          Any other element kind releases the partial result and traps.
/// @param seq Source `Seq`; null or invalid handles trap through the Seq API.
/// @return Fresh runtime-managed F64Buffer with the same length and order.
void *rt_f64buf_from_seq(void *seq) {
    int64_t len_i64 = rt_seq_len(seq);
    void *obj = rt_f64buf_new(len_i64);
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.FromSeq: invalid buffer object");
    size_t len = rt_arr_f64_len(buf->arr);
    for (size_t i = 0; i < len; i++) {
        void *item = rt_seq_get(seq, (int64_t)i);
        double value = 0.0;
        int64_t i64_value = 0;
        if (!rt_box_try_to_f64(item, &value)) {
            if (rt_box_try_to_i64(item, &i64_value))
                value = (double)i64_value;
            else {
                release_temp_obj(obj);
                rt_trap("F64Buffer.FromSeq: value is not numeric");
                return NULL;
            }
        }
        rt_arr_f64_set_fast(buf->arr, i, value);
    }
    return obj;
}

/// @brief Convert a general `Seq` of boxed integers to packed I64 values.
/// @details Every element must be exactly convertible through
///          `rt_box_try_to_i64`; floating-point values are not narrowed. A
///          mismatch releases the partial result before trapping.
/// @param seq Source `Seq`; null or invalid handles trap through the Seq API.
/// @return Fresh runtime-managed I64Buffer with the same length and order.
void *rt_i64buf_from_seq(void *seq) {
    int64_t len_i64 = rt_seq_len(seq);
    void *obj = rt_i64buf_new(len_i64);
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.FromSeq: invalid buffer object");
    size_t len = rt_arr_i64_len(buf->arr);
    for (size_t i = 0; i < len; i++) {
        void *item = rt_seq_get(seq, (int64_t)i);
        int64_t value = 0;
        if (!rt_box_try_to_i64(item, &value)) {
            release_temp_obj(obj);
            rt_trap("I64Buffer.FromSeq: value is not i64");
            return NULL;
        }
        rt_arr_i64_set_fast(buf->arr, i, value);
    }
    return obj;
}

/// @brief Return the number of packed doubles in a buffer.
/// @param obj Valid F64Buffer handle; null or wrong-kind handles trap.
/// @return Element count.
int64_t rt_f64buf_len(void *obj) {
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.Length: invalid buffer object");
    return (int64_t)rt_arr_f64_len(buf->arr);
}

/// @brief Return the number of packed signed integers in a buffer.
/// @param obj Valid I64Buffer handle; null or wrong-kind handles trap.
/// @return Element count.
int64_t rt_i64buf_len(void *obj) {
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.Length: invalid buffer object");
    return (int64_t)rt_arr_i64_len(buf->arr);
}

/// @brief Read one packed double.
/// @param obj Valid F64Buffer handle.
/// @param index Zero-based index; negative or out-of-range values trap.
/// @return Stored IEEE-754 value.
double rt_f64buf_get(void *obj, int64_t index) {
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.Get: invalid buffer object");
    return rt_arr_f64_get(buf->arr, checked_index_from_i64(index, "F64Buffer.Get: negative index"));
}

/// @brief Read one packed signed integer.
/// @param obj Valid I64Buffer handle.
/// @param index Zero-based index; negative or out-of-range values trap.
/// @return Stored signed 64-bit value.
int64_t rt_i64buf_get(void *obj, int64_t index) {
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.Get: invalid buffer object");
    return rt_arr_i64_get(buf->arr, checked_index_from_i64(index, "I64Buffer.Get: negative index"));
}

/// @brief Replace one packed double in place.
/// @param obj Valid F64Buffer handle.
/// @param index Zero-based index; negative or out-of-range values trap.
/// @param value New IEEE-754 value.
void rt_f64buf_set(void *obj, int64_t index, double value) {
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.Set: invalid buffer object");
    rt_arr_f64_set(buf->arr, checked_index_from_i64(index, "F64Buffer.Set: negative index"), value);
}

/// @brief Replace one packed signed integer in place.
/// @param obj Valid I64Buffer handle.
/// @param index Zero-based index; negative or out-of-range values trap.
/// @param value New signed 64-bit value.
void rt_i64buf_set(void *obj, int64_t index, int64_t value) {
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.Set: invalid buffer object");
    rt_arr_i64_set(buf->arr, checked_index_from_i64(index, "I64Buffer.Set: negative index"), value);
}

/// @brief Fill every F64Buffer element with one value.
/// @param obj Valid F64Buffer handle.
/// @param value IEEE-754 bit pattern/value copied to every slot.
void rt_f64buf_fill(void *obj, double value) {
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.Fill: invalid buffer object");
    size_t len = rt_arr_f64_len(buf->arr);
    for (size_t i = 0; i < len; i++)
        rt_arr_f64_set_fast(buf->arr, i, value);
}

/// @brief Fill every I64Buffer element with one value.
/// @param obj Valid I64Buffer handle.
/// @param value Signed value copied to every slot.
void rt_i64buf_fill(void *obj, int64_t value) {
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.Fill: invalid buffer object");
    size_t len = rt_arr_i64_len(buf->arr);
    for (size_t i = 0; i < len; i++)
        rt_arr_i64_set_fast(buf->arr, i, value);
}

/// @brief Replace an F64Buffer with a copy of another buffer.
/// @details Resizes @p dst to the source length before copying the packed
///          payload. Source and destination must both be F64Buffer objects.
/// @param dst_obj Destination buffer mutated in place.
/// @param src_obj Source buffer whose length and values are copied.
void rt_f64buf_copy_from(void *dst_obj, void *src_obj) {
    rt_f64buf_impl *dst = as_f64buf(dst_obj, "F64Buffer.CopyFrom: invalid destination buffer");
    rt_f64buf_impl *src = as_f64buf(src_obj, "F64Buffer.CopyFrom: invalid source buffer");
    size_t len = rt_arr_f64_len(src->arr);
    if (rt_arr_f64_resize(&dst->arr, len) != 0) {
        rt_trap("F64Buffer.CopyFrom: memory allocation failed");
        return;
    }
    rt_arr_f64_copy_payload(dst->arr, src->arr, len);
}

/// @brief Replace an I64Buffer with a copy of another buffer.
/// @details Resizes @p dst to the source length before copying the packed
///          payload. Source and destination must both be I64Buffer objects.
/// @param dst_obj Destination buffer mutated in place.
/// @param src_obj Source buffer whose length and values are copied.
void rt_i64buf_copy_from(void *dst_obj, void *src_obj) {
    rt_i64buf_impl *dst = as_i64buf(dst_obj, "I64Buffer.CopyFrom: invalid destination buffer");
    rt_i64buf_impl *src = as_i64buf(src_obj, "I64Buffer.CopyFrom: invalid source buffer");
    size_t len = rt_arr_i64_len(src->arr);
    if (rt_arr_i64_resize(&dst->arr, len) != 0) {
        rt_trap("I64Buffer.CopyFrom: memory allocation failed");
        return;
    }
    rt_arr_i64_copy_payload(dst->arr, src->arr, len);
}

/// @brief Copy a clamped half-open range into a fresh F64Buffer.
/// @details Negative endpoints clamp to zero, endpoints beyond the source
///          clamp to its length, and an end before the start yields an empty
///          buffer. The result never aliases source storage.
/// @param obj Source F64Buffer.
/// @param start Requested inclusive index.
/// @param end Requested exclusive index.
/// @return Fresh runtime-managed F64Buffer containing `[start, end)`.
void *rt_f64buf_slice(void *obj, int64_t start, int64_t end) {
    rt_f64buf_impl *src = as_f64buf(obj, "F64Buffer.Slice: invalid buffer object");
    size_t from = 0;
    size_t to = 0;
    clamp_slice(rt_arr_f64_len(src->arr), start, end, &from, &to);
    void *slice_obj = rt_f64buf_new((int64_t)(to - from));
    rt_f64buf_impl *slice = as_f64buf(slice_obj, "F64Buffer.Slice: invalid slice object");
    rt_arr_f64_copy_payload(slice->arr, src->arr + from, to - from);
    return slice_obj;
}

/// @brief Copy a clamped half-open range into a fresh I64Buffer.
/// @details Negative endpoints clamp to zero, endpoints beyond the source
///          clamp to its length, and an end before the start yields an empty
///          buffer. The result never aliases source storage.
/// @param obj Source I64Buffer.
/// @param start Requested inclusive index.
/// @param end Requested exclusive index.
/// @return Fresh runtime-managed I64Buffer containing `[start, end)`.
void *rt_i64buf_slice(void *obj, int64_t start, int64_t end) {
    rt_i64buf_impl *src = as_i64buf(obj, "I64Buffer.Slice: invalid buffer object");
    size_t from = 0;
    size_t to = 0;
    clamp_slice(rt_arr_i64_len(src->arr), start, end, &from, &to);
    void *slice_obj = rt_i64buf_new((int64_t)(to - from));
    rt_i64buf_impl *slice = as_i64buf(slice_obj, "I64Buffer.Slice: invalid slice object");
    rt_arr_i64_copy_payload(slice->arr, src->arr + from, to - from);
    return slice_obj;
}

/// @brief Add one scalar to every packed double in place.
/// @details Evaluation follows native IEEE-754 addition, including NaN,
///          infinity, rounding, and signed-zero behavior.
/// @param obj F64Buffer to mutate.
/// @param value Scalar added to every element.
void rt_f64buf_add_scalar(void *obj, double value) {
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.AddScalar: invalid buffer object");
    size_t len = rt_arr_f64_len(buf->arr);
    for (size_t i = 0; i < len; i++)
        rt_arr_f64_set_fast(buf->arr, i, rt_arr_f64_get_fast(buf->arr, i) + value);
}

/// @brief Compute signed 64-bit addition with explicit overflow detection.
/// @param a Left operand.
/// @param b Right operand.
/// @param out Receives the exact sum when representable.
/// @return 1 on overflow, otherwise 0.
static int numbuf_checked_add_i64(int64_t a, int64_t b, int64_t *out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, out);
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return 1;
    *out = a + b;
    return 0;
#endif
}

/// @brief Compute signed 64-bit multiplication with explicit overflow detection.
/// @param a Left operand.
/// @param b Right operand.
/// @param out Receives the exact product when representable.
/// @return 1 on overflow, otherwise 0.
static int numbuf_checked_mul_i64(int64_t a, int64_t b, int64_t *out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, out);
#else
    if (a == 0 || b == 0) {
        *out = 0;
        return 0;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b)
                return 1;
        } else if (b < INT64_MIN / a) {
            return 1;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b)
                return 1;
        } else if (a < INT64_MAX / b) {
            return 1;
        }
    }
    *out = a * b;
    return 0;
#endif
}

/// @brief Add one scalar to every packed integer in place.
/// @details Traps at the first unrepresentable sum. Elements preceding that
///          position have already been updated, so overflow is not an atomic
///          rollback boundary.
/// @param obj I64Buffer to mutate.
/// @param value Scalar added to every element.
void rt_i64buf_add_scalar(void *obj, int64_t value) {
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.AddScalar: invalid buffer object");
    size_t len = rt_arr_i64_len(buf->arr);
    for (size_t i = 0; i < len; i++) {
        int64_t out;
        if (numbuf_checked_add_i64(rt_arr_i64_get_fast(buf->arr, i), value, &out)) {
            rt_trap("I64Buffer.AddScalar: integer overflow");
            return;
        }
        rt_arr_i64_set_fast(buf->arr, i, out);
    }
}

/// @brief Multiply every packed double by one scalar in place.
/// @details Evaluation follows native IEEE-754 multiplication.
/// @param obj F64Buffer to mutate.
/// @param value Scalar multiplied into every element.
void rt_f64buf_mul_scalar(void *obj, double value) {
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.MulScalar: invalid buffer object");
    size_t len = rt_arr_f64_len(buf->arr);
    for (size_t i = 0; i < len; i++)
        rt_arr_f64_set_fast(buf->arr, i, rt_arr_f64_get_fast(buf->arr, i) * value);
}

/// @brief Multiply every packed integer by one scalar in place.
/// @details Traps at the first unrepresentable product. Earlier elements
///          remain updated if a later element overflows.
/// @param obj I64Buffer to mutate.
/// @param value Scalar multiplied into every element.
void rt_i64buf_mul_scalar(void *obj, int64_t value) {
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.MulScalar: invalid buffer object");
    size_t len = rt_arr_i64_len(buf->arr);
    for (size_t i = 0; i < len; i++) {
        int64_t out;
        if (numbuf_checked_mul_i64(rt_arr_i64_get_fast(buf->arr, i), value, &out)) {
            rt_trap("I64Buffer.MulScalar: integer overflow");
            return;
        }
        rt_arr_i64_set_fast(buf->arr, i, out);
    }
}

/// @brief Add another F64Buffer element by element.
/// @details Both buffers must have identical lengths. IEEE-754 addition is
///          performed directly into @p obj; @p other is not modified.
/// @param obj Destination/left-hand F64Buffer.
/// @param other_obj Source/right-hand F64Buffer.
void rt_f64buf_add_buffer(void *obj, void *other_obj) {
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.AddBuffer: invalid buffer object");
    rt_f64buf_impl *other = as_f64buf(other_obj, "F64Buffer.AddBuffer: invalid source buffer");
    size_t len = rt_arr_f64_len(buf->arr);
    if (rt_arr_f64_len(other->arr) != len) {
        rt_trap("F64Buffer.AddBuffer: length mismatch");
        return;
    }
    for (size_t i = 0; i < len; i++) {
        rt_arr_f64_set_fast(
            buf->arr, i, rt_arr_f64_get_fast(buf->arr, i) + rt_arr_f64_get_fast(other->arr, i));
    }
}

/// @brief Add another I64Buffer element by element.
/// @details Length mismatch traps before mutation. Signed overflow traps at
///          the first failing element, after any earlier sums were stored.
/// @param obj Destination/left-hand I64Buffer.
/// @param other_obj Source/right-hand I64Buffer.
void rt_i64buf_add_buffer(void *obj, void *other_obj) {
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.AddBuffer: invalid buffer object");
    rt_i64buf_impl *other = as_i64buf(other_obj, "I64Buffer.AddBuffer: invalid source buffer");
    size_t len = rt_arr_i64_len(buf->arr);
    if (rt_arr_i64_len(other->arr) != len) {
        rt_trap("I64Buffer.AddBuffer: length mismatch");
        return;
    }
    for (size_t i = 0; i < len; i++) {
        int64_t out;
        if (numbuf_checked_add_i64(
                rt_arr_i64_get_fast(buf->arr, i), rt_arr_i64_get_fast(other->arr, i), &out)) {
            rt_trap("I64Buffer.AddBuffer: integer overflow");
            return;
        }
        rt_arr_i64_set_fast(buf->arr, i, out);
    }
}

/// @brief Accumulate all F64Buffer elements from left to right.
/// @details Returns positive zero for an empty buffer and otherwise follows
///          native IEEE-754 addition order without compensated summation.
/// @param obj Source F64Buffer.
/// @return Arithmetic sum.
double rt_f64buf_sum(void *obj) {
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.Sum: invalid buffer object");
    double sum = 0.0;
    size_t len = rt_arr_f64_len(buf->arr);
    for (size_t i = 0; i < len; i++)
        sum += rt_arr_f64_get_fast(buf->arr, i);
    return sum;
}

/// @brief Accumulate all I64Buffer elements with overflow checking.
/// @param obj Source I64Buffer.
/// @return Exact signed sum, or 0 after trapping on overflow; an empty buffer
///         returns 0.
int64_t rt_i64buf_sum(void *obj) {
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.Sum: invalid buffer object");
    int64_t sum = 0;
    size_t len = rt_arr_i64_len(buf->arr);
    for (size_t i = 0; i < len; i++) {
        if (numbuf_checked_add_i64(sum, rt_arr_i64_get_fast(buf->arr, i), &sum)) {
            rt_trap("I64Buffer.Sum: integer overflow");
            return 0;
        }
    }
    return sum;
}

/// @brief Compute the dot product of two equal-length F64Buffers.
/// @details Products and the left-to-right accumulation follow native
///          IEEE-754 semantics without compensated summation.
/// @param obj Left-hand source buffer.
/// @param other_obj Right-hand source buffer of identical length.
/// @return Dot product, or 0.0 after a length-mismatch trap.
double rt_f64buf_dot(void *obj, void *other_obj) {
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.Dot: invalid buffer object");
    rt_f64buf_impl *other = as_f64buf(other_obj, "F64Buffer.Dot: invalid source buffer");
    size_t len = rt_arr_f64_len(buf->arr);
    if (rt_arr_f64_len(other->arr) != len) {
        rt_trap("F64Buffer.Dot: length mismatch");
        return 0.0;
    }
    double sum = 0.0;
    for (size_t i = 0; i < len; i++)
        sum += rt_arr_f64_get_fast(buf->arr, i) * rt_arr_f64_get_fast(other->arr, i);
    return sum;
}

/// @brief Compute the exact checked dot product of two I64Buffers.
/// @details Each product and running sum is overflow-checked; neither input is
///          modified. Length mismatch or overflow traps.
/// @param obj Left-hand source buffer.
/// @param other_obj Right-hand source buffer of identical length.
/// @return Dot product, or 0 after trapping.
int64_t rt_i64buf_dot(void *obj, void *other_obj) {
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.Dot: invalid buffer object");
    rt_i64buf_impl *other = as_i64buf(other_obj, "I64Buffer.Dot: invalid source buffer");
    size_t len = rt_arr_i64_len(buf->arr);
    if (rt_arr_i64_len(other->arr) != len) {
        rt_trap("I64Buffer.Dot: length mismatch");
        return 0;
    }
    int64_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        int64_t prod;
        if (numbuf_checked_mul_i64(
                rt_arr_i64_get_fast(buf->arr, i), rt_arr_i64_get_fast(other->arr, i), &prod) ||
            numbuf_checked_add_i64(sum, prod, &sum)) {
            rt_trap("I64Buffer.Dot: integer overflow");
            return 0;
        }
    }
    return sum;
}

/// @brief Find the smallest F64Buffer element by native comparison.
/// @details Empty buffers trap. NaN behavior follows the direct `<`
///          comparisons used by the implementation.
/// @param obj Non-empty source F64Buffer.
/// @return Minimum selected value, or 0.0 after an empty-buffer trap.
double rt_f64buf_min(void *obj) {
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.Min: invalid buffer object");
    size_t len = rt_arr_f64_len(buf->arr);
    if (len == 0) {
        rt_trap("F64Buffer.Min: empty buffer");
        return 0.0;
    }
    double value = rt_arr_f64_get_fast(buf->arr, 0);
    for (size_t i = 1; i < len; i++) {
        double next = rt_arr_f64_get_fast(buf->arr, i);
        if (next < value)
            value = next;
    }
    return value;
}

/// @brief Find the smallest I64Buffer element.
/// @param obj Non-empty source I64Buffer.
/// @return Minimum value, or 0 after an empty-buffer trap.
int64_t rt_i64buf_min(void *obj) {
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.Min: invalid buffer object");
    size_t len = rt_arr_i64_len(buf->arr);
    if (len == 0) {
        rt_trap("I64Buffer.Min: empty buffer");
        return 0;
    }
    int64_t value = rt_arr_i64_get_fast(buf->arr, 0);
    for (size_t i = 1; i < len; i++) {
        int64_t next = rt_arr_i64_get_fast(buf->arr, i);
        if (next < value)
            value = next;
    }
    return value;
}

/// @brief Find the largest F64Buffer element by native comparison.
/// @details Empty buffers trap. NaN behavior follows the direct `>`
///          comparisons used by the implementation.
/// @param obj Non-empty source F64Buffer.
/// @return Maximum selected value, or 0.0 after an empty-buffer trap.
double rt_f64buf_max(void *obj) {
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.Max: invalid buffer object");
    size_t len = rt_arr_f64_len(buf->arr);
    if (len == 0) {
        rt_trap("F64Buffer.Max: empty buffer");
        return 0.0;
    }
    double value = rt_arr_f64_get_fast(buf->arr, 0);
    for (size_t i = 1; i < len; i++) {
        double next = rt_arr_f64_get_fast(buf->arr, i);
        if (next > value)
            value = next;
    }
    return value;
}

/// @brief Find the largest I64Buffer element.
/// @param obj Non-empty source I64Buffer.
/// @return Maximum value, or 0 after an empty-buffer trap.
int64_t rt_i64buf_max(void *obj) {
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.Max: invalid buffer object");
    size_t len = rt_arr_i64_len(buf->arr);
    if (len == 0) {
        rt_trap("I64Buffer.Max: empty buffer");
        return 0;
    }
    int64_t value = rt_arr_i64_get_fast(buf->arr, 0);
    for (size_t i = 1; i < len; i++) {
        int64_t next = rt_arr_i64_get_fast(buf->arr, i);
        if (next > value)
            value = next;
    }
    return value;
}

/// @brief Export packed doubles to a fresh general-purpose List.
/// @details Values are boxed in source order. The owning list retains each
///          box before the local temporary reference is released.
/// @param obj Source F64Buffer.
/// @return New runtime-managed List of boxed F64 values.
void *rt_f64buf_to_list(void *obj) {
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.ToList: invalid buffer object");
    void *list = rt_list_new();
    size_t len = rt_arr_f64_len(buf->arr);
    for (size_t i = 0; i < len; i++) {
        void *boxed = rt_box_f64(rt_arr_f64_get_fast(buf->arr, i));
        rt_list_push(list, boxed);
        release_temp_obj(boxed);
    }
    return list;
}

/// @brief Export packed integers to a fresh general-purpose List.
/// @details Values are boxed in source order. The owning list retains each
///          box before the local temporary reference is released.
/// @param obj Source I64Buffer.
/// @return New runtime-managed List of boxed I64 values.
void *rt_i64buf_to_list(void *obj) {
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.ToList: invalid buffer object");
    void *list = rt_list_new();
    size_t len = rt_arr_i64_len(buf->arr);
    for (size_t i = 0; i < len; i++) {
        void *boxed = rt_box_i64(rt_arr_i64_get_fast(buf->arr, i));
        rt_list_push(list, boxed);
        release_temp_obj(boxed);
    }
    return list;
}

/// @brief Export packed doubles to a fresh owning Seq.
/// @details Capacity is preallocated from the source length and each boxed
///          value is retained by the result sequence.
/// @param obj Source F64Buffer.
/// @return New runtime-managed owning `Seq` of boxed F64 values.
void *rt_f64buf_to_seq(void *obj) {
    rt_f64buf_impl *buf = as_f64buf(obj, "F64Buffer.ToSeq: invalid buffer object");
    void *seq = rt_seq_with_capacity_owned(rt_f64buf_len(obj) > 0 ? rt_f64buf_len(obj) : 1);
    size_t len = rt_arr_f64_len(buf->arr);
    for (size_t i = 0; i < len; i++) {
        void *boxed = rt_box_f64(rt_arr_f64_get_fast(buf->arr, i));
        rt_seq_push(seq, boxed);
        release_temp_obj(boxed);
    }
    return seq;
}

/// @brief Export packed integers to a fresh owning Seq.
/// @details Capacity is preallocated from the source length and each boxed
///          value is retained by the result sequence.
/// @param obj Source I64Buffer.
/// @return New runtime-managed owning `Seq` of boxed I64 values.
void *rt_i64buf_to_seq(void *obj) {
    rt_i64buf_impl *buf = as_i64buf(obj, "I64Buffer.ToSeq: invalid buffer object");
    void *seq = rt_seq_with_capacity_owned(rt_i64buf_len(obj) > 0 ? rt_i64buf_len(obj) : 1);
    size_t len = rt_arr_i64_len(buf->arr);
    for (size_t i = 0; i < len; i++) {
        void *boxed = rt_box_i64(rt_arr_i64_get_fast(buf->arr, i));
        rt_seq_push(seq, boxed);
        release_temp_obj(boxed);
    }
    return seq;
}
