//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_numbuf.h
// Purpose: Packed numeric collection APIs for F64Buffer and I64Buffer.
// Key invariants:
//   - Buffers are fixed-length, GC-managed objects; indices are 0-based and
//     bounds-checked (out-of-range access traps).
//   - I64 arithmetic traps on signed overflow instead of wrapping; F64
//     arithmetic follows IEEE-754.
// Ownership/Lifetime:
//   - Constructors return GC-managed handles; callers must not free directly.
//   - Slice/conversion results are fresh runtime-managed allocations.
// Links: src/runtime/collections/rt_numbuf.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for packed F64Buffer and I64Buffer collections.
/// @details Numeric buffers store primitive values contiguously without
///          per-element boxing. They provide fixed-index access, resizable
///          whole-buffer copy, clamped slicing, scalar/vector arithmetic,
///          reductions, and explicit conversion to general-purpose boxed
///          runtime collections.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Allocate a zero-filled packed double buffer.
/// @param len Number of elements; negative values trap.
/// @return Fresh runtime-managed F64Buffer.
void *rt_f64buf_new(int64_t len);

/// @brief Build an F64Buffer from boxed numeric Seq elements.
/// @details Accepts boxed F64 values and boxed I64 values converted to double.
///          Other element kinds trap after releasing the partial buffer.
/// @param seq Source Seq; null or invalid handles trap.
/// @return Fresh runtime-managed F64Buffer preserving source order.
void *rt_f64buf_from_seq(void *seq);

/// @brief Get the number of packed double elements.
/// @param buf Valid F64Buffer; null or wrong-kind handles trap.
/// @return Element count.
int64_t rt_f64buf_len(void *buf);

/// @brief Read one packed double.
/// @param buf Valid F64Buffer.
/// @param index Zero-based element index; negative/out-of-range values trap.
/// @return Stored IEEE-754 value.
double rt_f64buf_get(void *buf, int64_t index);

/// @brief Replace one packed double.
/// @param buf F64Buffer to mutate.
/// @param index Zero-based element index; negative/out-of-range values trap.
/// @param value New IEEE-754 value.
void rt_f64buf_set(void *buf, int64_t index, double value);

/// @brief Set every element to @p value.
/// @param buf F64Buffer to mutate.
/// @param value Value copied into each element.
void rt_f64buf_fill(void *buf, double value);

/// @brief Replace @p dst with a packed copy of @p src.
/// @details Resizes the destination to the source length before copying; an
///          allocation failure traps.
/// @param dst Destination F64Buffer.
/// @param src Source F64Buffer.
void rt_f64buf_copy_from(void *dst, void *src);

/// @brief Return a fresh buffer holding elements [start, end) (bounds clamped).
/// @details Endpoints clamp to `[0, Length]`; an end before the normalized
///          start produces an empty non-aliasing buffer.
/// @param buf Source F64Buffer.
/// @param start Requested inclusive index.
/// @param end Requested exclusive index.
/// @return Fresh runtime-managed F64Buffer slice.
void *rt_f64buf_slice(void *buf, int64_t start, int64_t end);

/// @brief Add @p value to every element in place.
/// @details Arithmetic follows native IEEE-754 semantics.
/// @param buf F64Buffer to mutate.
/// @param value Scalar addend.
void rt_f64buf_add_scalar(void *buf, double value);

/// @brief Multiply every element by @p value in place.
/// @details Arithmetic follows native IEEE-754 semantics.
/// @param buf F64Buffer to mutate.
/// @param value Scalar multiplier.
void rt_f64buf_mul_scalar(void *buf, double value);

/// @brief Element-wise add @p other into @p buf (traps on length mismatch).
/// @param buf Destination/left-hand F64Buffer.
/// @param other Source/right-hand F64Buffer of identical length.
void rt_f64buf_add_buffer(void *buf, void *other);

/// @brief Sum of all elements (0.0 for an empty buffer).
/// @details Accumulates left to right using native IEEE-754 addition.
/// @param buf Source F64Buffer.
/// @return Arithmetic sum.
double rt_f64buf_sum(void *buf);

/// @brief Dot product with @p other (traps on length mismatch).
/// @details Uses native IEEE-754 multiply/add operations without compensated
///          summation.
/// @param buf Left-hand F64Buffer.
/// @param other Right-hand F64Buffer of identical length.
/// @return Dot product.
double rt_f64buf_dot(void *buf, void *other);

/// @brief Smallest element (traps on an empty buffer).
/// @details NaN behavior follows direct native `<` comparisons.
/// @param buf Non-empty F64Buffer.
/// @return Selected minimum value.
double rt_f64buf_min(void *buf);

/// @brief Largest element (traps on an empty buffer).
/// @details NaN behavior follows direct native `>` comparisons.
/// @param buf Non-empty F64Buffer.
/// @return Selected maximum value.
double rt_f64buf_max(void *buf);

/// @brief Copy the elements into a fresh List of boxed floats.
/// @param buf Source F64Buffer.
/// @return New runtime-managed List whose owning elements preserve source
///         order.
void *rt_f64buf_to_list(void *buf);

/// @brief Copy the elements into a fresh owning Seq of boxed floats.
/// @param buf Source F64Buffer.
/// @return New runtime-managed owning Seq preserving source order.
void *rt_f64buf_to_seq(void *buf);

/// @brief Allocate a zero-filled packed signed-integer buffer.
/// @param len Number of elements; negative values trap.
/// @return Fresh runtime-managed I64Buffer.
void *rt_i64buf_new(int64_t len);

/// @brief Build an I64Buffer from boxed integer Seq elements.
/// @details Floating-point and nonnumeric values are rejected rather than
///          narrowed; a mismatch releases the partial buffer before trapping.
/// @param seq Source Seq; null or invalid handles trap.
/// @return Fresh runtime-managed I64Buffer preserving source order.
void *rt_i64buf_from_seq(void *seq);

/// @brief Get the number of packed signed-integer elements.
/// @param buf Valid I64Buffer; null or wrong-kind handles trap.
/// @return Element count.
int64_t rt_i64buf_len(void *buf);

/// @brief Read one packed signed integer.
/// @param buf Valid I64Buffer.
/// @param index Zero-based element index; negative/out-of-range values trap.
/// @return Stored signed 64-bit value.
int64_t rt_i64buf_get(void *buf, int64_t index);

/// @brief Replace one packed signed integer.
/// @param buf I64Buffer to mutate.
/// @param index Zero-based element index; negative/out-of-range values trap.
/// @param value New signed 64-bit value.
void rt_i64buf_set(void *buf, int64_t index, int64_t value);

/// @brief Set every element to @p value.
/// @param buf I64Buffer to mutate.
/// @param value Value copied into each element.
void rt_i64buf_fill(void *buf, int64_t value);

/// @brief Replace @p dst with a packed copy of @p src.
/// @details Resizes the destination to the source length before copying; an
///          allocation failure traps.
/// @param dst Destination I64Buffer.
/// @param src Source I64Buffer.
void rt_i64buf_copy_from(void *dst, void *src);

/// @brief Return a fresh buffer holding elements [start, end) (bounds clamped).
/// @details Endpoints clamp to `[0, Length]`; an end before the normalized
///          start produces an empty non-aliasing buffer.
/// @param buf Source I64Buffer.
/// @param start Requested inclusive index.
/// @param end Requested exclusive index.
/// @return Fresh runtime-managed I64Buffer slice.
void *rt_i64buf_slice(void *buf, int64_t start, int64_t end);

/// @brief Add @p value to every element in place (traps on signed overflow).
/// @details Mutation stops at the first overflow; earlier elements remain
///          updated.
/// @param buf I64Buffer to mutate.
/// @param value Scalar addend.
void rt_i64buf_add_scalar(void *buf, int64_t value);

/// @brief Multiply every element by @p value in place (traps on signed overflow).
/// @details Mutation stops at the first overflow; earlier elements remain
///          updated.
/// @param buf I64Buffer to mutate.
/// @param value Scalar multiplier.
void rt_i64buf_mul_scalar(void *buf, int64_t value);

/// @brief Element-wise add @p other into @p buf (traps on length mismatch or overflow).
/// @details Length mismatch traps before mutation. Overflow can leave earlier
///          elements updated.
/// @param buf Destination/left-hand I64Buffer.
/// @param other Source/right-hand I64Buffer of identical length.
void rt_i64buf_add_buffer(void *buf, void *other);

/// @brief Sum of all elements (traps on accumulation overflow; 0 when empty).
/// @param buf Source I64Buffer.
/// @return Exact signed sum.
int64_t rt_i64buf_sum(void *buf);

/// @brief Dot product with @p other (traps on length mismatch or overflow).
/// @param buf Left-hand I64Buffer.
/// @param other Right-hand I64Buffer of identical length.
/// @return Exact signed dot product.
int64_t rt_i64buf_dot(void *buf, void *other);

/// @brief Smallest element (traps on an empty buffer).
/// @param buf Non-empty I64Buffer.
/// @return Minimum signed value.
int64_t rt_i64buf_min(void *buf);

/// @brief Largest element (traps on an empty buffer).
/// @param buf Non-empty I64Buffer.
/// @return Maximum signed value.
int64_t rt_i64buf_max(void *buf);

/// @brief Copy the elements into a fresh List of boxed integers.
/// @param buf Source I64Buffer.
/// @return New runtime-managed List whose owning elements preserve source
///         order.
void *rt_i64buf_to_list(void *buf);

/// @brief Copy the elements into a fresh owning Seq of boxed integers.
/// @param buf Source I64Buffer.
/// @return New runtime-managed owning Seq preserving source order.
void *rt_i64buf_to_seq(void *buf);

#ifdef __cplusplus
}
#endif
