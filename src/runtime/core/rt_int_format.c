//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_int_format.c
// Purpose: Implements locale-independent 64-bit integer-to-string formatting
//          helpers for the BASIC runtime. Routines write into caller-supplied
//          buffers, guarantee null termination even on truncation, and report
//          the number of characters produced so callers can chain buffers.
//
// Key invariants:
//   - Output is always null-terminated, even when the buffer is smaller than
//     the formatted value; the returned length excludes the terminator.
//   - Formatting is locale-independent: PRId64 / PRIu64 macros are used so
//     decimal output is stable across all host environments.
//   - Zero capacity or a NULL buffer causes an early return of 0 without
//     writing; a zero return can also represent truncation to an empty buffer.
//   - rt_snprintf is used instead of snprintf to allow test interposition.
//
// Ownership/Lifetime:
//   - Writes into caller-supplied buffers; no heap allocation is performed.
//   - No state is retained between calls; functions are pure utilities.
//
// Links: src/runtime/core/rt_int_format.h (public API),
//        src/runtime/core/rt_printf_compat.c (rt_snprintf implementation),
//        src/runtime/core/rt_string_format.c (higher-level string conversion)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements bounded decimal formatting for signed and unsigned integers.

#include "rt_int_format.h"
#include "rt_printf_compat.h"

#include <inttypes.h>
#include <stdio.h>

/// @brief Format a signed 64-bit integer into @p buffer.
/// @details Validates the destination buffer, writes the decimal representation
///          using the C locale, and ensures the output is null-terminated even
///          when truncated.  The return value reports the number of characters
///          actually retained excluding the terminator, not the would-have-
///          written length reported by `snprintf`.
/// @param value Signed integer to convert, including the full `int64_t` range.
/// @param buffer Destination byte buffer, or NULL to report failure.
/// @param capacity Total buffer size including the terminator.
/// @return Stored character count. Invalid/zero-capacity destinations and
///   formatter errors return zero; truncation returns @p capacity minus one.
size_t rt_i64_to_cstr(int64_t value, char *buffer, size_t capacity) {
    if (!buffer || capacity == 0)
        return 0;
    int written = rt_snprintf(buffer, capacity, "%" PRId64, (int64_t)value);
    if (written < 0) {
        buffer[0] = '\0';
        return 0;
    }
    if ((size_t)written >= capacity) {
        buffer[capacity - 1] = '\0';
        return capacity - 1;
    }
    return (size_t)written;
}

/// @brief Format an unsigned 64-bit integer into @p buffer.
/// @details Mirrors @ref rt_i64_to_cstr but prints the value using the unsigned
///          conversion specifier.  The function always leaves the buffer
///          null-terminated and reports the number of characters retained.
/// @param value Unsigned integer to convert.
/// @param buffer Destination byte buffer, or NULL to report failure.
/// @param capacity Total buffer size including the terminator.
/// @return Stored character count. Invalid/zero-capacity destinations and
///   formatter errors return zero; truncation returns @p capacity minus one.
size_t rt_u64_to_cstr(uint64_t value, char *buffer, size_t capacity) {
    if (!buffer || capacity == 0)
        return 0;
    int written = rt_snprintf(buffer, capacity, "%" PRIu64, (uint64_t)value);
    if (written < 0) {
        buffer[0] = '\0';
        return 0;
    }
    if ((size_t)written >= capacity) {
        buffer[capacity - 1] = '\0';
        return capacity - 1;
    }
    return (size_t)written;
}
