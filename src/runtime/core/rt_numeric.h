//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_numeric.h
// Purpose: Declares BASIC numeric conversions, locale-independent formatting,
//   VAL-style conversion, and strict integer/floating parsers.
//
// Key invariants:
//   - CINT and CLNG round finite inputs to nearest-even before range checking.
//   - CSNG rejects non-finite inputs and finite values that overflow float.
//   - Text conversion and formatting use ASCII syntax and the C numeric locale.
//   - Strict parsers distinguish invalid arguments, invalid syntax, numeric
//     overflow, and locale setup failures with runtime error codes.
//
// Ownership/Lifetime:
//   - Formatting writes to caller-owned buffers and does not transfer ownership.
//   - Runtime-string parsers borrow the input storage only for the call.
//   - Scalar conversion functions retain no references or persistent state.
//
// Links: src/runtime/core/rt_numeric.c (VAL and formatting implementation),
//        src/runtime/core/rt_numeric_conv.c (conversions and strict parsers),
//        src/runtime/core/rt_error.h (error and trap definitions)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Public numeric conversion, formatting, and parsing API.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rt_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Convert double @p x to INTEGER using round-to-nearest-even.
/// @param x Input value.
/// @param ok Required output flag set to `false` when @p x is non-finite or
///           the rounded result lies outside the signed 16-bit range.
/// @return The rounded 16-bit value on success, or zero on conversion failure.
/// @note A null @p ok pointer traps and returns zero if the trap hook returns.
int16_t rt_cint_from_double(double x, bool *ok);

/// @brief Convert double @p x to LONG using round-to-nearest-even.
/// @param x Input value.
/// @param ok Required output flag set to `false` when @p x is non-finite or
///           the rounded result lies outside the signed 32-bit range.
/// @return The rounded 32-bit value on success, or zero on conversion failure.
/// @note A null @p ok pointer traps and returns zero if the trap hook returns.
int32_t rt_clng_from_double(double x, bool *ok);

/// @brief Convert double @p x to SINGLE.
/// @param x Input value.
/// @param ok Required output flag set to `false` for a non-finite input or
///           when the cast produces a non-finite float.
/// @return The converted float on success, NaN for a non-finite input, or the
///         non-finite cast result when a finite input overflows.
/// @note A null @p ok pointer traps and returns NaN if the trap hook returns.
float rt_csng_from_double(double x, bool *ok);

/// @brief Return an already-double numeric value unchanged.
/// @param x Input value; finite and non-finite values are accepted.
/// @return @p x unchanged.
double rt_cdbl_from_any(double x);

/// @brief Compute INT(x) using floor semantics.
/// @param x Input value.
/// @return `floor(x)`, including the platform libm behavior for signed zero,
///         infinity, and NaN.
double rt_int_floor(double x);

/// @brief Compute FIX(x) using truncation toward zero.
/// @param x Input value.
/// @return `trunc(x)`, including the platform libm behavior for signed zero,
///         infinity, and NaN.
double rt_fix_trunc(double x);

/// @brief Convert DOUBLE to signed 64-bit form with defined saturation.
/// @param x Input floating-point value.
/// @return Zero for NaN; `INT64_MAX` for positive infinity or values at least
///         2^63; `INT64_MIN` for negative infinity or values below -2^63; or
///         the in-range value truncated toward zero.
long long rt_f64_to_i64(double x);

/// @brief Round @p x to @p ndigits decimal places using banker's rounding.
/// @param x Input value.
/// @param ndigits Number of digits after the decimal point (negative for tens, hundreds, ...).
/// @return The nearest-even scaled result. Non-finite inputs and cases where
///         `abs(ndigits) > 308`, the scale is zero/non-finite, or scaling
///         overflows are returned unchanged.
double rt_round_even(double x, int ndigits);

/// @brief Parse NUL-terminated text using the runtime's VAL-style semantics.
/// @details Accepts leading/trailing ASCII whitespace and the strict decimal
///          grammar `[+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?`.
///          Case-insensitive `nan`, `inf`, and `infinity` prefixes produce
///          their IEEE value but are reported as unsuccessful.
/// @param s NUL-terminated input text.
/// @param ok Required output flag set only for a completely consumed finite
///           decimal value.
/// @return The parsed finite value on success, a recognised or converted
///         non-finite value for that failure class, or zero for other failures.
/// @note Null pointers trap; if the trap hook returns, the function returns
///       zero and clears @p ok when that pointer is available.
double rt_val_to_double(const char *s, bool *ok);

/// @brief Format DOUBLE @p x into @p out using round-trip precision.
/// @details Uses the C-locale `%.17g` representation and traps if the buffer
///          is invalid, formatting fails, or the output is truncated.
/// @param x Value to format.
/// @param out Destination buffer; must not be NULL.
/// @param cap Capacity of @p out in bytes including the terminator; must be non-zero.
/// @param out_err Optional error record overwritten with `RT_ERROR_NONE` after
///                the formatting helper returns.
void rt_str_from_double(double x, char *out, size_t cap, RtError *out_err);

/// @brief Format SINGLE @p x into @p out using round-trip precision.
/// @details Promotes @p x and uses the C-locale `%.9g` representation, trapping
///          for an invalid buffer, formatting failure, or truncation.
/// @param x Value to format.
/// @param out Destination buffer; must not be NULL.
/// @param cap Capacity of @p out in bytes including the terminator; must be non-zero.
/// @param out_err Optional error record overwritten with `RT_ERROR_NONE` after
///                the formatting helper returns.
void rt_str_from_float(float x, char *out, size_t cap, RtError *out_err);

/// @brief Format LONG @p x into @p out as minimal decimal digits.
/// @details Uses the C locale and traps for an invalid buffer, formatting
///          failure, or truncation.
/// @param x Value to format.
/// @param out Destination buffer; must not be NULL.
/// @param cap Capacity of @p out in bytes including the terminator; must be non-zero.
/// @param out_err Optional error record overwritten with `RT_ERROR_NONE` after
///                the formatting helper returns.
void rt_str_from_i32(int32_t x, char *out, size_t cap, RtError *out_err);

/// @brief Format INTEGER @p x into @p out as minimal decimal digits.
/// @details Uses the C locale and traps for an invalid buffer, formatting
///          failure, or truncation.
/// @param x Value to format.
/// @param out Destination buffer; must not be NULL.
/// @param cap Capacity of @p out in bytes including the terminator; must be non-zero.
/// @param out_err Optional error record overwritten with `RT_ERROR_NONE` after
///                the formatting helper returns.
void rt_str_from_i16(int16_t x, char *out, size_t cap, RtError *out_err);

/// @brief Parse signed 64-bit integer from trimmed C string @p text.
/// @details Accepts a base-10 `strtoll` token surrounded by ASCII whitespace
///          and requires complete consumption.
/// @param text NUL-terminated input text.
/// @param out_value Destination initialized to zero before validation.
/// @return `Err_None` on success, `Err_InvalidOperation` for a null argument,
///         `Err_InvalidCast` for empty or malformed text, or `Err_Overflow`
///         when `strtoll` reports range overflow.
int32_t rt_parse_int64(const char *text, int64_t *out_value);

/// @brief Parse signed 64-bit integer from a runtime string @p text.
/// @details Borrows the runtime string's NUL-terminated data, rejects invalid
///          handles, null data, and embedded NUL bytes, then delegates to
///          @ref rt_parse_int64.
/// @param text Runtime string handle.
/// @param out_value Destination initialized to zero before validation.
/// @return `Err_None` on success, `Err_InvalidOperation` for a null output
///         pointer, `Err_InvalidCast` for an unusable string, or a parser error
///         returned by @ref rt_parse_int64.
int32_t rt_parse_int64_str(rt_string text, int64_t *out_value);

/// @brief Parse double-precision floating point from trimmed C string @p text.
/// @details Accepts the strict decimal grammar documented by
///          @ref rt_val_to_double or a case-insensitive signed `nan`/`inf`
///          token, surrounded by ASCII whitespace. Conversion uses the C
///          locale. Decimal overflow and non-finite decimal results are
///          rejected, while explicitly spelled non-finite tokens and finite
///          underflow are accepted.
/// @param text NUL-terminated input text.
/// @param out_value Destination initialized to zero before validation.
/// @return `Err_None` on success, `Err_InvalidOperation` for a null argument,
///         `Err_InvalidCast` for malformed text, `Err_Overflow` for a
///         non-finite decimal result, or `Err_RuntimeError` if locale setup
///         fails.
int32_t rt_parse_double(const char *text, double *out_value);

/// @brief Parse double-precision floating point from a runtime string @p text.
/// @details Rejects invalid handles, null data, and embedded NUL bytes before
///          delegating to @ref rt_parse_double.
/// @param text Runtime string handle.
/// @param out_value Destination initialized to zero before validation.
/// @return `Err_None` on success, `Err_InvalidOperation` for a null output
///         pointer, `Err_InvalidCast` for an unusable string, or a parser error
///         returned by @ref rt_parse_double.
int32_t rt_parse_double_str(rt_string text, double *out_value);

#ifdef __cplusplus
}
#endif
