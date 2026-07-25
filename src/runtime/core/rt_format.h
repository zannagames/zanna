//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_format.h
// Purpose: Deterministic, locale-independent formatting utilities providing f64-to-string
// conversion and CSV quoting for BASIC PRINT, WRITE#, and conversion helpers.
//
// Key invariants:
//   - rt_format_f64 output is identical across platforms and locales.
//   - rt_format_f64_roundtrip output parses back to the same IEEE-754 value.
//   - The caller supplies the output buffer and must ensure sufficient capacity.
//   - rt_csv_quote_alloc always surrounds the value with double-quotes and doubles any internal
//   quotes.
//   - NULL input to rt_csv_quote_alloc is treated as empty string.
//
// Ownership/Lifetime:
//   - rt_format_f64 and rt_format_f64_roundtrip write into caller-managed buffers with no
//     heap allocation.
//   - rt_csv_quote_alloc returns a new rt_string owned by the caller (must release).
//
// Links: src/runtime/core/rt_format.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares locale-stable double formatting and CSV quoting helpers.

#pragma once

#include <stddef.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief snprintf that always uses the C numeric locale for the conversion.
/// @details Deterministic decimal separators regardless of the embedding
///          process's LC_NUMERIC. Shared by the text-format emitters so their
///          numeric output is identical across locales (VDOC-041). Arguments
///          must obey the same validity and type requirements as `snprintf`.
/// @param buffer Destination storage accepted by the platform formatter.
/// @param capacity Size of @p buffer in bytes.
/// @param fmt Non-NULL `printf`-style format string followed by matching arguments.
/// @return Result from the platform locale-aware formatter; locale setup or
///   formatting errors return a negative value.
int rt_format_snprintf_c_locale(char *buffer, size_t capacity, const char *fmt, ...);

/// @brief Format a double-precision value into a deterministic display string.
///
/// @details This is the BASIC PRINT/WRITE# display form. It keeps historical
/// 15-significant-digit output for finite values and canonical spellings for
/// NaN/Inf. Use @ref rt_format_f64_roundtrip when the string must reparse to
/// the exact same double.
///
/// @param value Value to format.
/// @param buffer Non-NULL destination buffer for the textual representation.
/// @param capacity Size of @p buffer in bytes, including space for the null terminator.
/// @note Invalid arguments, formatting failure, or truncation raise a runtime
///   trap. If trap dispatch returns and @p buffer is writable, failure paths
///   leave it as an empty C string.
void rt_format_f64(double value, char *buffer, size_t capacity);

/// @brief Format a double-precision value into an exact round-trip string.
/// @details Finite values are searched across `%g` precisions 1 through 17,
///   accepting only candidates that reparse to the identical 64-bit value.
///   The shortest candidate wins, with fixed notation preferred over exponent
///   notation on equal length. NaN and infinities use `NaN`, `Inf`, and `-Inf`.
/// @param value Value to format.
/// @param buffer Non-NULL destination buffer for the textual representation.
/// @param capacity Size of @p buffer in bytes, including space for the null terminator.
/// @note Invalid arguments, formatting failure, or truncation raise a runtime
///   trap. If trap dispatch returns and @p buffer is writable, failure paths
///   leave it as an empty C string.
void rt_format_f64_roundtrip(double value, char *buffer, size_t capacity);

/// @brief Produce a CSV-escaped string literal for WRITE # statements.
/// @details The successful result always has leading and trailing double
///   quotes, with every embedded quote duplicated. Embedded NUL bytes are
///   processed according to the runtime string's explicit byte length.
/// @param value Borrowed source string to escape; NULL is treated as empty.
/// @return Caller-owned quoted runtime string. Size, allocation, or invalid
///   string failures trap and return an owned empty string if dispatch returns.
rt_string rt_csv_quote_alloc(rt_string value);

#ifdef __cplusplus
}
#endif
