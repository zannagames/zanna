//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_int_format.h
// Purpose: Portable, locale-independent 64-bit integer to decimal string formatting, providing
// signed and unsigned variants that write into caller-supplied buffers.
//
// Key invariants:
//   - Always null-terminates the output buffer when capacity > 0.
//   - Undersized buffers receive a truncated prefix and the return value is the
//     number of characters retained, at most capacity - 1.
//   - Output is identical across platforms regardless of locale settings.
//   - Minimum buffer size for int64 is 21 bytes (20 digits + NUL).
//
// Ownership/Lifetime:
//   - Callers provide and own the destination buffer.
//   - No heap allocation or pointer retention occurs.
//
// Links: src/runtime/core/rt_int_format.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares bounded decimal conversion into caller-owned C buffers.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Format a signed 64-bit integer into the supplied buffer using the C locale.
/// @details Uses the portable `PRId64` conversion. When the representation does
///   not fit, the buffer contains the longest NUL-terminated prefix that fits.
///   A capacity of at least 21 bytes is sufficient for every signed value.
/// @param value Signed integer value to format.
/// @param buffer Destination buffer, or NULL to return zero without writing.
/// @param capacity Size of @p buffer in bytes, including space for the null terminator.
/// @return Number of characters retained excluding the terminator. Invalid
///   destinations and formatting errors return zero; truncation returns
///   @p capacity minus one.
size_t rt_i64_to_cstr(int64_t value, char *buffer, size_t capacity);

/// @brief Format an unsigned 64-bit integer into the supplied buffer using the C locale.
/// @details Uses the portable `PRIu64` conversion. When the representation does
///   not fit, the buffer contains the longest NUL-terminated prefix that fits.
///   A capacity of at least 21 bytes is sufficient for every unsigned value.
/// @param value Unsigned integer value to format.
/// @param buffer Destination buffer, or NULL to return zero without writing.
/// @param capacity Size of @p buffer in bytes, including space for the null terminator.
/// @return Number of characters retained excluding the terminator. Invalid
///   destinations and formatting errors return zero; truncation returns
///   @p capacity minus one.
size_t rt_u64_to_cstr(uint64_t value, char *buffer, size_t capacity);

#ifdef __cplusplus
}
#endif
