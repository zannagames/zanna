//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_numeric_conv.c
// Purpose: Implements BASIC scalar conversions, nearest-even rounding,
//          saturating double-to-I64 conversion, and strict error-code-based
//          integer and floating-point parsers.
//
// Key invariants:
//   - CINT and CLNG round to nearest-even independently of the active
//     floating-point rounding mode, then validate the target range.
//   - Null CINT, CLNG, and CSNG status pointers trap rather than dereference.
//   - Strict parsers initialize available output slots to zero and report
//     invalid arguments, syntax errors, overflow, and locale failures
//     separately through runtime error codes.
//   - Banker's rounding is implemented explicitly so it is independent of the
//     process floating-point rounding mode.
//   - Decimal parsing uses fixed ASCII token rules and the C numeric locale;
//     process LC_CTYPE and LC_NUMERIC settings do not change accepted input.
//
// Ownership/Lifetime:
//   - Scalar functions retain no state or references.
//   - Runtime-string parsers borrow string bytes for the duration of the call.
//   - Platform locale objects are temporary and released before return.
//
// Links: src/runtime/core/rt_numeric.h (public API),
//        src/runtime/core/rt_numeric.c (complementary numeric utilities),
//        src/runtime/core/rt_error.h (runtime error and trap definitions)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief BASIC numeric conversions and strict text parsers.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "rt.hpp"
#include "rt_internal.h"
#include "rt_numeric.h"
#include "rt_string.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <xlocale.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Round a floating-point value to the nearest even integer.
/// @details Implements round-half-to-even directly instead of using
///          @c nearbyint, whose result depends on the process floating-point
///          rounding mode. Values with magnitude >= 2^52 already have no
///          fractional part representable in double precision. Non-finite
///          values pass through unchanged and zero retains its sign.
/// @param x Value to round.
/// @return The nearest integral double, choosing an even result at an exact
///         half, or @p x unchanged when it is non-finite or already integral
///         at double precision.
static double rt_round_nearest_even(double x) {
    if (!isfinite(x))
        return x;

    double ax = fabs(x);
    if (ax >= 0x1.0p52)
        return x;

    double lower = floor(ax);
    double frac = ax - lower;
    double rounded = lower;
    if (frac > 0.5) {
        rounded = lower + 1.0;
    } else if (frac == 0.5) {
        double half = fmod(lower, 2.0);
        if (half != 0.0)
            rounded = lower + 1.0;
    }

    return copysign(rounded, x);
}

/// @brief Validate a floating-point value before casting to an integer type.
/// @details Confirms the @p ok pointer is provided, rejects NaN or infinite
///          values, and checks the numeric range against the supplied
///          bounds.  When validation fails the helper records `false` in
///          @p ok and returns `0.0`; otherwise it preserves the original
///          value so the caller can perform the final cast.
/// @param value Floating-point candidate.
/// @param ok Pointer to a flag that receives the success status.
/// @param min_value Inclusive minimum representable value.
/// @param max_value Inclusive maximum representable value.
/// @param null_ok_trap Diagnostic string for null @p ok pointers.
/// @return The original value when it passes validation, otherwise 0.0.
/// @note A null @p ok pointer traps and returns zero if the trap hook returns.
static inline double rt_cast_integer_checked(
    double value, bool *ok, double min_value, double max_value, const char *null_ok_trap) {
    if (!ok) {
        rt_trap(null_ok_trap);
        return 0.0;
    }

    if (!isfinite(value)) {
        *ok = false;
        return 0.0;
    }

    if (value < min_value || value > max_value) {
        *ok = false;
        return 0.0;
    }

    *ok = true;
    return value;
}

#define RT_CAST_INTEGER(value, ok, type, min_value, max_value, null_ok_trap)                       \
    ((type)rt_cast_integer_checked(                                                                \
        (value), (ok), (double)(min_value), (double)(max_value), (null_ok_trap)))

/// @brief Cast a floating-point value to @c int16_t with validation.
/// @details Delegates to @ref rt_cast_integer_checked using the @c int16_t
///          range limits and propagates the @p ok flag.
/// @param value Floating-point value to convert.
/// @param ok Required output flag recording success or failure.
/// @return The value converted with C truncation toward zero when valid;
///         otherwise zero.
/// @note A null @p ok pointer traps and returns zero if the trap hook returns.
static int16_t rt_cast_i16(double value, bool *ok) {
    return RT_CAST_INTEGER(value, ok, int16_t, INT16_MIN, INT16_MAX, "rt_cast_i16: null ok");
}

/// @brief Cast a floating-point value to @c int32_t with validation.
/// @details Applies the @ref rt_cast_integer_checked guard using @c int32_t
///          limits so that overflow and NaN conditions surface through the
///          @p ok flag.
/// @param value Floating-point value to convert.
/// @param ok Required output flag recording success or failure.
/// @return The value converted with C truncation toward zero when valid;
///         otherwise zero.
/// @note A null @p ok pointer traps and returns zero if the trap hook returns.
static int32_t rt_cast_i32(double value, bool *ok) {
    return RT_CAST_INTEGER(value, ok, int32_t, INT32_MIN, INT32_MAX, "rt_cast_i32: null ok");
}

/// @brief Convert a double to BASIC's CINT result with banker's rounding.
/// @details Applies nearest-even rounding via @ref rt_round_nearest_even
///          before casting to @c int16_t.  The @p ok flag communicates
///          whether the rounded value is finite and fits the target range.
/// @param x Input double.
/// @param ok Required output flag updated with the conversion status.
/// @return The converted 16-bit integer, or zero if the cast failed.
/// @note A null @p ok pointer traps and returns zero if the trap hook returns.
int16_t rt_cint_from_double(double x, bool *ok) {
    const double rounded = rt_round_nearest_even(x);
    return rt_cast_i16(rounded, ok);
}

/// @brief Convert a double to BASIC's CLNG result with banker's rounding.
/// @details Mirrors @ref rt_cint_from_double but targets @c int32_t,
///          allowing larger integer ranges while still respecting the
///          nearest-even rounding rule.
/// @param x Input double.
/// @param ok Required output flag updated with the conversion status.
/// @return The converted 32-bit integer, or zero if the cast failed.
/// @note A null @p ok pointer traps and returns zero if the trap hook returns.
int32_t rt_clng_from_double(double x, bool *ok) {
    const double rounded = rt_round_nearest_even(x);
    return rt_cast_i32(rounded, ok);
}

/// @brief Convert a double to BASIC's CSNG single-precision result.
/// @details Validates the @p ok pointer, rejects non-finite inputs, casts to
///          @c float, and ensures the result remains finite.  The helper
///          sets @p ok to `true` only when all checks succeed.
/// @param x Input double.
/// @param ok Required output flag updated with the conversion status.
/// @return The converted single-precision value on success, NaN for a
///         non-finite input, or the non-finite float produced when a finite
///         double overflows the target type.
/// @note A null @p ok pointer traps and returns NaN if the trap hook returns.
float rt_csng_from_double(double x, bool *ok) {
    if (!ok) {
        rt_trap("rt_csng_from_double: null ok");
        return NAN;
    }

    if (!isfinite(x)) {
        *ok = false;
        return NAN;
    }

    const float result = (float)x;
    if (!isfinite(result)) {
        *ok = false;
        return result;
    }

    *ok = true;
    return result;
}

/// @brief Return the provided value unchanged for CDbl conversions.
/// @details This ABI entry already receives a double, so it performs no
///          finiteness check or other transformation.
/// @param x Input numeric value.
/// @return @p x unchanged, including NaN, infinity, and signed zero.
double rt_cdbl_from_any(double x) {
    return x;
}

/// @brief Compute BASIC's INT result by flooring the argument.
/// @details Delegates to @ref floor to obtain the greatest integer less than
///          or equal to @p x; no explicit domain checks are added.
/// @param x Input double.
/// @return `floor(x)`, including the platform libm result for NaN, infinity,
///         and signed zero.
double rt_int_floor(double x) {
    return floor(x);
}

/// @brief Compute BASIC's FIX result by truncating towards zero.
/// @details Uses @ref trunc so positive and negative numbers move towards
///          zero, matching BASIC semantics; no explicit domain checks are
///          added.
/// @param x Input double.
/// @return `trunc(x)`, including the platform libm result for NaN, infinity,
///         and signed zero.
double rt_fix_trunc(double x) {
    return trunc(x);
}

/// @brief Convert a double to a 64-bit signed integer by truncating toward zero.
/// @details Provides a direct conversion from floating-point to integer,
///          suitable for ZannaLang's Number to Integer conversion. NaN becomes
///          0; infinities and finite out-of-range values clamp to the nearest
///          signed 64-bit endpoint.
/// @param x Input double.
/// @return Zero for NaN; `INT64_MAX` for values at least 2^63; `INT64_MIN`
///         for values below -2^63; otherwise @p x truncated toward zero.
long long rt_f64_to_i64(double x) {
    if (isnan(x))
        return 0;
    if (x >= 0x1.0p63)
        return INT64_MAX;
    if (x < -0x1.0p63)
        return INT64_MIN;
    return (long long)trunc(x);
}

/// @brief Round a double to a specified number of digits using banker's rounding.
/// @details Applies nearest-even rounding with optional scaling so callers
///          can request decimal precision. Positive @p ndigits selects digits
///          after the decimal point and negative values select powers of ten
///          before it. Non-finite inputs, digit magnitudes above 308, unusable
///          scale factors, and scaling overflow return @p x unchanged.
/// @param x Input double to round.
/// @param ndigits Decimal-place position at which to round.
/// @return The scaled nearest-even result, or @p x for a documented
///         short-circuit condition.
double rt_round_even(double x, int ndigits) {
    if (!isfinite(x))
        return x;

    if (ndigits == 0)
        return rt_round_nearest_even(x);

    const double absDigits = fabs((double)ndigits);
    if (absDigits > 308.0)
        return x;

    const double factor = pow(10.0, (double)ndigits);
    if (!isfinite(factor) || factor == 0.0)
        return x;

    const double scaled = x * factor;
    if (!isfinite(scaled))
        return x;

    const double rounded = rt_round_nearest_even(scaled);
    return rounded / factor;
}

/// @brief Test for the fixed ASCII whitespace set.
/// @param ch Byte to classify.
/// @return Non-zero for space, tab, line feed, carriage return, form feed, or
///         vertical tab; otherwise zero.
static inline int rt_is_ascii_space(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

/// @brief Locale-independent test for ASCII digits 0-9.
/// @details Used by the parsers in this file instead of `isdigit` so the result is
///          deterministic regardless of the process's `LC_CTYPE` setting.
/// @param ch Byte to classify.
/// @return Non-zero for bytes `'0'` through `'9'`; otherwise zero.
static inline int rt_is_ascii_digit(unsigned char ch) {
    return ch >= '0' && ch <= '9';
}

/// @brief Advance a pointer past ASCII whitespace characters.
/// @details Uses a fixed byte set instead of the process locale so numeric
///          parsing remains deterministic.
/// @param cursor Non-null position within a NUL-terminated byte buffer.
/// @return Pointer to the first non-whitespace byte (or the terminator).
static inline const unsigned char *rt_skip_ascii_space(const unsigned char *cursor) {
    while (*cursor && rt_is_ascii_space(*cursor))
        ++cursor;
    return cursor;
}

/// @brief Return the end of a strict decimal floating literal.
/// @details Accepts `[+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?`.
///          Hexadecimal C float syntax is intentionally rejected so the
///          public parser matches the documented decimal grammar.
/// @param cursor Start of a NUL-terminated byte sequence.
/// @return The first byte after a complete literal, or `NULL` if @p cursor is
///         null or does not begin with one.
static const unsigned char *rt_scan_decimal_float(const unsigned char *cursor) {
    if (!cursor)
        return NULL;

    const unsigned char *p = cursor;
    if (*p == '+' || *p == '-')
        ++p;

    int digits = 0;
    while (rt_is_ascii_digit(*p)) {
        ++digits;
        ++p;
    }

    if (*p == '.') {
        ++p;
        while (rt_is_ascii_digit(*p)) {
            ++digits;
            ++p;
        }
    }

    if (digits == 0)
        return NULL;

    if (*p == 'e' || *p == 'E') {
        const unsigned char *exp = p + 1;
        if (*exp == '+' || *exp == '-')
            ++exp;
        int exp_digits = 0;
        while (rt_is_ascii_digit(*exp)) {
            ++exp_digits;
            ++exp;
        }
        if (exp_digits == 0)
            return NULL;
        p = exp;
    }

    return p;
}

/// @brief Recognize canonical non-finite floating literals.
/// @details Accepts `NaN` and `Inf` case-insensitively after an optional sign.
///          The sign controls infinity; NaN is written using the `NAN`
///          constant. Only the three-letter token is consumed, allowing the
///          caller to enforce trailing-whitespace and end-of-input rules.
/// @param cursor Candidate NUL-terminated byte sequence.
/// @param out_value Destination for NaN or signed infinity.
/// @param out_end Destination for the first byte after the token.
/// @return Non-zero when a token was recognised; otherwise zero, including
///         for any null argument.
static int rt_scan_nonfinite_float(const unsigned char *cursor,
                                   double *out_value,
                                   const unsigned char **out_end) {
    if (!cursor || !out_value || !out_end)
        return 0;
    const unsigned char *p = cursor;
    int negative = 0;
    if (*p == '+' || *p == '-') {
        negative = *p == '-';
        ++p;
    }
    if ((p[0] == 'n' || p[0] == 'N') && (p[1] == 'a' || p[1] == 'A') &&
        (p[2] == 'n' || p[2] == 'N')) {
        *out_value = NAN;
        *out_end = p + 3;
        return 1;
    }
    if ((p[0] == 'i' || p[0] == 'I') && (p[1] == 'n' || p[1] == 'N') &&
        (p[2] == 'f' || p[2] == 'F')) {
        *out_value = negative ? -INFINITY : INFINITY;
        *out_end = p + 3;
        return 1;
    }
    return 0;
}

/// @brief Parse a signed 64-bit integer from ASCII text.
/// @details Skips leading whitespace, invokes @ref strtoll using base 10,
///          validates that the entire string was consumed, and reports
///          overflow or invalid input through BASIC error codes. Trailing
///          ASCII whitespace is permitted. If supplied, @p out_value is
///          initialized to zero before any validation.
/// @param text NUL-terminated string containing the number.
/// @param out_value Required destination for the parsed integer.
/// @return `Err_None` on success, `Err_InvalidOperation` for a null argument,
///         `Err_InvalidCast` for empty, partially consumed, or malformed
///         input, or `Err_Overflow` when `strtoll` reports `ERANGE`.
int32_t rt_parse_int64(const char *text, int64_t *out_value) {
    if (out_value)
        *out_value = 0;
    if (!text || !out_value)
        return (int32_t)Err_InvalidOperation;

    const unsigned char *cursor = rt_skip_ascii_space((const unsigned char *)text);
    if (*cursor == '\0')
        return (int32_t)Err_InvalidCast;

    errno = 0;
    char *endptr = NULL;
    long long parsed = strtoll((const char *)cursor, &endptr, 10);
    if (errno == ERANGE)
        return (int32_t)Err_Overflow;

    if (!endptr || endptr == (const char *)cursor)
        return (int32_t)Err_InvalidCast;

    const unsigned char *rest = (const unsigned char *)endptr;
    rest = rt_skip_ascii_space(rest);
    if (*rest != '\0')
        return (int32_t)Err_InvalidCast;

    *out_value = (int64_t)parsed;
    return (int32_t)Err_None;
}

/// @brief Borrow a runtime string's C-compatible byte buffer.
/// @details Rejects null or invalid handles, null data, and a NUL byte within
///          the string's recorded length. The runtime-owned terminator after
///          that length remains available to the C parsers.
/// @param text Runtime string handle to inspect.
/// @return A borrowed NUL-terminated pointer, or `NULL` when the handle cannot
///         safely be parsed as C text.
static const char *rt_parse_string_text(rt_string text) {
    if (!text || !rt_string_is_handle((const void *)text) || !text->data)
        return NULL;
    size_t len = (size_t)rt_str_len(text);
    if (memchr(text->data, '\0', len))
        return NULL;
    return text->data;
}

/// @brief Parse a signed 64-bit integer from a runtime string.
/// @details Initializes an available output to zero, obtains a C-compatible
///          view with @ref rt_parse_string_text, and delegates successful
///          views to @ref rt_parse_int64.
/// @param text Runtime string handle.
/// @param out_value Required destination for the parsed integer.
/// @return `Err_InvalidOperation` for a null output pointer,
///         `Err_InvalidCast` for an unusable string, or the result of
///         @ref rt_parse_int64.
int32_t rt_parse_int64_str(rt_string text, int64_t *out_value) {
    if (out_value)
        *out_value = 0;
    if (!out_value)
        return (int32_t)Err_InvalidOperation;
    const char *cstr = rt_parse_string_text(text);
    if (!cstr)
        return (int32_t)Err_InvalidCast;
    return rt_parse_int64(cstr, out_value);
}

/// @brief Implementation helper that parses a double using the C locale.
/// @details After leading ASCII whitespace, accepts either a signed
///          case-insensitive three-letter `nan`/`inf` token or the strict
///          decimal grammar scanned by @ref rt_scan_decimal_float. Trailing
///          ASCII whitespace is allowed and all other trailing bytes are
///          rejected. Explicit non-finite tokens are successful; a decimal
///          conversion that becomes non-finite is overflow. Finite underflow
///          to zero or a subnormal is accepted.
/// @param text Non-null NUL-terminated input text.
/// @param out_value Non-null destination written only on success.
/// @return `Err_None` on success, `Err_InvalidCast` for invalid syntax,
///         `Err_Overflow` for a non-finite decimal result, or
///         `Err_RuntimeError` when the C locale cannot be installed.
static int32_t rt_parse_double_impl(const char *text, double *out_value) {
    const unsigned char *cursor = rt_skip_ascii_space((const unsigned char *)text);
    if (*cursor == '\0')
        return (int32_t)Err_InvalidCast;

    const unsigned char *nonfinite_end = NULL;
    double nonfinite = 0.0;
    if (rt_scan_nonfinite_float(cursor, &nonfinite, &nonfinite_end)) {
        const unsigned char *literal_tail = rt_skip_ascii_space(nonfinite_end);
        if (*literal_tail != '\0')
            return (int32_t)Err_InvalidCast;
        *out_value = nonfinite;
        return (int32_t)Err_None;
    }

    const unsigned char *literal_end = rt_scan_decimal_float(cursor);
    if (!literal_end)
        return (int32_t)Err_InvalidCast;
    const unsigned char *literal_tail = rt_skip_ascii_space(literal_end);
    if (*literal_tail != '\0')
        return (int32_t)Err_InvalidCast;

    errno = 0;
    char *endptr = NULL;
    double value = 0.0;

#if defined(_WIN32)
    _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
    if (!c_locale)
        return (int32_t)Err_RuntimeError;
    value = _strtod_l((const char *)cursor, &endptr, c_locale);
    _free_locale(c_locale);
#else
    locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (!c_locale)
        return (int32_t)Err_RuntimeError;
    locale_t previous = uselocale(c_locale);
    if (previous == (locale_t)0) {
        freelocale(c_locale);
        return (int32_t)Err_RuntimeError;
    }
    value = strtod((const char *)cursor, &endptr);
    uselocale(previous);
    freelocale(c_locale);
#endif

    if (endptr == (const char *)cursor)
        return (int32_t)Err_InvalidCast;

    if (!isfinite(value))
        return (int32_t)Err_Overflow;

    const unsigned char *rest = rt_skip_ascii_space((const unsigned char *)endptr);
    if (*rest != '\0')
        return (int32_t)Err_InvalidCast;

    *out_value = value;
    return (int32_t)Err_None;
}

/// @brief Parse a double from ASCII text respecting BASIC error codes.
/// @details Validates parameters before delegating to
///          @ref rt_parse_double_impl so the public API consistently rejects
///          null pointers with @ref Err_InvalidOperation. If supplied,
///          @p out_value is initialized to zero before validation.
/// @param text NUL-terminated string containing the number.
/// @param out_value Required destination for the parsed double.
/// @return `Err_None` on success, `Err_InvalidOperation` for a null argument,
///         or the syntax, overflow, or locale error from
///         @ref rt_parse_double_impl.
int32_t rt_parse_double(const char *text, double *out_value) {
    if (out_value)
        *out_value = 0.0;
    if (!text || !out_value)
        return (int32_t)Err_InvalidOperation;
    return rt_parse_double_impl(text, out_value);
}

/// @brief Parse a double from a runtime string.
/// @details Initializes an available output to zero, obtains a C-compatible
///          view with @ref rt_parse_string_text, and delegates successful
///          views to @ref rt_parse_double.
/// @param text Runtime string handle.
/// @param out_value Required destination for the parsed double.
/// @return `Err_InvalidOperation` for a null output pointer,
///         `Err_InvalidCast` for an unusable string, or the result of
///         @ref rt_parse_double.
int32_t rt_parse_double_str(rt_string text, double *out_value) {
    if (out_value)
        *out_value = 0.0;
    if (!out_value)
        return (int32_t)Err_InvalidOperation;
    const char *cstr = rt_parse_string_text(text);
    if (!cstr)
        return (int32_t)Err_InvalidCast;
    return rt_parse_double(cstr, out_value);
}

#ifdef __cplusplus
}
#endif
