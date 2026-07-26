//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_numeric.c
// Purpose: Implements locale-independent VAL-style conversion and
//   caller-buffer formatting for the BASIC runtime. Strict error-code-based
//   parsers and arithmetic conversions live in rt_numeric_conv.c.
//
// Key invariants:
//   - Decimal scanning and formatting use the C locale regardless of the
//     process LC_NUMERIC setting.
//   - VAL accepts only a strict decimal grammar after leading ASCII space.
//   - NaN, Inf, and Infinity prefixes are recognised case-insensitively but
//     deliberately report ok=false because they are not finite VAL results.
//   - Formatting traps on invalid buffers, locale/format failures, or
//     truncation; successful output is always NUL-terminated by snprintf.
//   - All functions are exposed with C linkage (extern "C").
//
// Ownership/Lifetime:
//   - No pointer returned by this file owns storage.
//   - Formatting writes only to caller-owned buffers. Temporary locale objects
//     are created and destroyed within each conversion or formatting call.
//
// Links: src/runtime/core/rt_numeric.h (public API),
//        src/runtime/core/rt_numeric_conv.c (conversions and strict parsers),
//        src/runtime/core/rt_error.h (trap and error definitions)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements locale-independent BASIC numeric parsing and formatting.
 *
 * @details VAL-style conversion accepts the runtime's strict decimal grammar,
 *          recognizes non-finite spellings as unsuccessful conversions, and
 *          reports status through caller-provided storage. Numeric formatting
 *          uses transient C-locale state and caller-owned buffers so behavior
 *          is deterministic across host locale settings and platforms.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "rt_numeric.h"
#include "rt.hpp"

#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <xlocale.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Determine whether a character represents an ASCII digit.
/// @details BASIC numeric parsing only accepts ASCII digits for integral
///          components.  This helper deliberately ignores locale-specific
///          digits so conversions remain deterministic across platforms.
/// @param ch Character to classify.
/// @return Non-zero for bytes `'0'` through `'9'`; otherwise zero.
static int rt_is_digit_char(char ch) {
    return ch >= '0' && ch <= '9';
}

/// @brief Determine whether a byte is ASCII whitespace.
/// @details Keeps numeric parsing independent of the active process locale.
/// @param ch Byte to classify.
/// @return Non-zero for space, tab, line feed, carriage return, form feed, or
///         vertical tab; otherwise zero.
static int rt_is_ascii_space(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

/// @brief Convert an ASCII letter to lowercase without invoking locale APIs.
/// @details The runtime uses ASCII-only case folding when matching special
///          constants such as "INF".  Using a bespoke helper avoids linking
///          against locale facilities that could diverge between hosts.
/// @param ch Byte value to fold.
/// @return The lowercase ASCII code for an uppercase ASCII letter, or @p ch
///         unchanged for every other value.
static int rt_ascii_tolower(int ch) {
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A' + 'a';
    return ch;
}

/// @brief Compare @p start against @p token case-insensitively.
/// @details Iterates over @p token and ensures each byte in @p start matches
///          after ASCII case folding.  When the token matches, @p out_end is
///          updated to point immediately past the consumed characters.
/// @param start Candidate byte sequence; must be NUL-terminated when non-null.
/// @param token Lowercase ASCII token to match; must be NUL-terminated.
/// @param out_end Optional destination for the first byte following a match.
/// @return `true` when the complete token matches; `false` for null arguments,
///         an early terminator, or a differing byte.
static bool rt_match_token_ci(const char *start, const char *token, const char **out_end) {
    if (!start || !token)
        return false;

    size_t index = 0;
    while (token[index] != '\0') {
        const unsigned char ch = (unsigned char)start[index];
        if (ch == '\0')
            return false;
        if (rt_ascii_tolower((int)ch) != token[index])
            return false;
        ++index;
    }

    if (out_end)
        *out_end = start + index;
    return true;
}

/// @brief Parse a textual non-finite constant prefix.
/// @details Handles an optional sign and the case-insensitive spellings
///          `nan`, `inf`, and `infinity`. This helper consumes only the matched
///          prefix; it does not require the following byte to terminate the
///          input. A leading `#` is not accepted.
/// @param start Candidate NUL-terminated text.
/// @param out_end Optional destination for the first unconsumed byte.
/// @param out_value Destination for the corresponding IEEE NaN or infinity.
/// @return `true` when a supported prefix was consumed; otherwise `false`.
static bool rt_parse_special_constant(const char *start, char **out_end, double *out_value) {
    if (!start || !out_value)
        return false;

    const char *cursor = start;
    bool is_negative = false;
    if (*cursor == '+' || *cursor == '-') {
        is_negative = (*cursor == '-');
        ++cursor;
    }

    const char *matched_end = NULL;
    if (rt_match_token_ci(cursor, "nan", &matched_end)) {
        cursor = matched_end;
        double value = nan("");
        if (is_negative)
            value = -value;
        if (out_end)
            *out_end = (char *)(uintptr_t)cursor;
        *out_value = value;
        return true;
    }

    if (rt_match_token_ci(cursor, "inf", &matched_end)) {
        cursor = matched_end;
        const char *extended = NULL;
        if (rt_match_token_ci(cursor, "inity", &extended))
            cursor = extended;

        double value = is_negative ? -INFINITY : INFINITY;
        if (out_end)
            *out_end = (char *)(uintptr_t)cursor;
        *out_value = value;
        return true;
    }

    return false;
}

#if defined(_WIN32)
/// @brief Parse a double using the C locale on Windows.
/// @details Materialises a per-call `_locale_t`, calls `_strtod_l`, and then
///          tears the locale down. Per-call allocation avoids unsynchronised
///          lazy global state during first-use races.
/// @param input NUL-terminated text to convert.
/// @param out_end Optional destination for `_strtod_l`'s first unconsumed byte.
/// @param out_value Destination for the converted value.
/// @return `true` when a locale was created and at least one byte was
///         consumed; otherwise `false`.
static bool rt_strtod_c_locale(const char *input, char **out_end, double *out_value) {
    if (!input || !out_value)
        return false;

    _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
    if (!c_locale)
        return false;

    errno = 0;
    char *endptr = NULL;
    double value = _strtod_l(input, &endptr, c_locale);
    _free_locale(c_locale);

    if (endptr == input)
        return false;

    if (out_end)
        *out_end = endptr;
    *out_value = value;
    return true;
}
#else
/// @brief Parse a double using the C locale on POSIX platforms.
/// @details Creates a transient locale object with `newlocale`, installs it
///          with @c uselocale for the duration of the conversion, and then
///          restores the previous locale.  Mirrors the Windows behaviour so
///          callers can rely on consistent locale-independent parsing.
/// @param input NUL-terminated text to convert.
/// @param out_end Optional destination for `strtod`'s first unconsumed byte.
/// @param out_value Destination for the converted value.
/// @return `true` when the locale operations succeeded and at least one byte
///         was consumed; otherwise `false`.
static bool rt_strtod_c_locale(const char *input, char **out_end, double *out_value) {
    if (!input || !out_value)
        return false;

    locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", NULL);
    if (!c_locale)
        return false;

    locale_t previous = uselocale(c_locale);
    if (previous == (locale_t)0) {
        freelocale(c_locale);
        return false;
    }
    errno = 0;
    char *endptr = NULL;
    double value = strtod(input, &endptr);
    uselocale(previous);
    freelocale(c_locale);

    if (endptr == input)
        return false;

    if (out_end)
        *out_end = endptr;
    *out_value = value;
    return true;
}
#endif

/// @brief Return the end of a strict decimal floating literal.
/// @details Accepts `[+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?`.
///          Hexadecimal C float syntax is intentionally rejected.
/// @param cursor Start of a NUL-terminated byte sequence.
/// @return The first byte after the literal, or `NULL` when @p cursor is null
///         or does not begin with a complete literal.
static const unsigned char *rt_scan_decimal_float(const unsigned char *cursor) {
    if (!cursor)
        return NULL;

    const unsigned char *p = cursor;
    if (*p == '+' || *p == '-')
        ++p;

    int digits = 0;
    while (rt_is_digit_char((char)*p)) {
        ++digits;
        ++p;
    }

    if (*p == '.') {
        ++p;
        while (rt_is_digit_char((char)*p)) {
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
        while (rt_is_digit_char((char)*exp)) {
            ++exp_digits;
            ++exp;
        }
        if (exp_digits == 0)
            return NULL;
        p = exp;
    }

    return p;
}

/// @brief Convert NUL-terminated text using the runtime's VAL-style grammar.
/// @details Skips leading ASCII whitespace, scans a signed decimal literal,
///          permits trailing ASCII whitespace, and requires no other trailing
///          bytes. Conversion uses the C locale. Case-insensitive `nan`,
///          `inf`, and `infinity` prefixes, including an optional sign, return
///          their IEEE value with @p ok set to `false`; the special-token path
///          does not validate bytes following the recognised prefix. Empty,
///          malformed, partially consumed, overflowing, and other non-finite
///          inputs also clear @p ok. A null input traps after clearing
///          @p ok; a null @p ok pointer traps without dereferencing it.
/// @param s NUL-terminated input text.
/// @param ok Required destination for the finite-conversion status.
/// @return The finite parsed value on success, the recognised or converted
///         non-finite value for that failure class, or `0.0` for other
///         failures. If a trap hook returns, the documented fallback is used.
double rt_val_to_double(const char *s, bool *ok) {
    if (!ok) {
        rt_trap("rt_val_to_double: null ok");
        return 0.0;
    }
    if (!s) {
        *ok = false;
        rt_trap("rt_val_to_double: null string");
        return 0.0;
    }

    const unsigned char *p = (const unsigned char *)s;
    while (*p && rt_is_ascii_space(*p))
        ++p;

    const char *parse = (const char *)p;
    if (*parse == '\0') {
        *ok = false;
        return 0.0;
    }

    double special_value = 0.0;
    if (rt_parse_special_constant(parse, NULL, &special_value)) {
        *ok = false;
        return special_value;
    }

    const unsigned char *literal_end = rt_scan_decimal_float((const unsigned char *)parse);
    if (!literal_end) {
        *ok = false;
        return 0.0;
    }

    const unsigned char *literal_tail = literal_end;
    while (*literal_tail != '\0' && rt_is_ascii_space(*literal_tail))
        ++literal_tail;
    if (*literal_tail != '\0') {
        *ok = false;
        return 0.0;
    }

    char *end = NULL;
    double value = 0.0;
    if (!rt_strtod_c_locale(parse, &end, &value)) {
        *ok = false;
        return 0.0;
    }

    if ((const unsigned char *)end != literal_end) {
        *ok = false;
        return 0.0;
    }

    if (!isfinite(value)) {
        *ok = false;
        return value;
    }

    *ok = true;
    return value;
}

/// @brief Run `vsnprintf` with the C numeric locale.
/// @details Windows passes a transient `_locale_t` to `_vsnprintf_l`. POSIX
///          temporarily installs a transient locale for the current thread,
///          restores the previous locale, and then frees the temporary object.
/// @param out Destination buffer forwarded to the platform formatter.
/// @param cap Capacity of @p out in bytes.
/// @param fmt `printf`-style format string.
/// @param args Arguments corresponding to @p fmt.
/// @return The platform formatter result, or `-1` when locale acquisition or
///         installation fails.
static int rt_vsnprintf_c_locale(char *out, size_t cap, const char *fmt, va_list args) {
#if defined(_WIN32)
    _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
    if (!c_locale)
        return -1;
#if !defined(_MSC_VER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    int written = _vsnprintf_l(out, cap, fmt, c_locale, args);
#if !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif
    _free_locale(c_locale);
    return written;
#else
    locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", NULL);
    if (!c_locale)
        return -1;
    locale_t previous = uselocale(c_locale);
    if (previous == (locale_t)0) {
        freelocale(c_locale);
        return -1;
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    int written = vsnprintf(out, cap, fmt, args);
#pragma GCC diagnostic pop
    uselocale(previous);
    freelocale(c_locale);
    return written;
#endif
}

/// @brief Format a floating-point or integer value into a caller buffer.
/// @details Invokes `vsnprintf` with the provided format string, trapping on
///          a null/zero-capacity buffer, locale or formatting failure, or
///          output that does not fit including its terminator. The buffer
///          contents after a returning trap hook are unspecified.
/// @param out Destination buffer.
/// @param cap Capacity of @p out in bytes.
/// @param fmt `printf`-style format string.
/// @param ... Values corresponding to @p fmt.
static void rt_format(char *out, size_t cap, const char *fmt, ...) {
    if (!out || cap == 0) {
        rt_trap("rt_format: invalid buffer");
        return;
    }

    va_list args;
    va_start(args, fmt);
#if !defined(_MSC_VER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    int written = rt_vsnprintf_c_locale(out, cap, fmt, args);
#if !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif
    va_end(args);

    if (written < 0) {
        rt_trap("rt_format: format error");
        return;
    }
    if ((size_t)written >= cap) {
        rt_trap("rt_format: truncated");
        return;
    }
}

/// @brief Serialize a double with 17 significant decimal digits.
/// @details Uses the C locale and the `%.17g` conversion. After @ref rt_format
///          returns, @p out_err receives `RT_ERROR_NONE` when supplied,
///          including when an installed trap hook returned from a formatting
///          failure.
/// @param x Value to format, including supported non-finite values.
/// @param out Caller-owned destination buffer.
/// @param cap Capacity of @p out in bytes, including the terminator.
/// @param out_err Optional destination overwritten with `RT_ERROR_NONE`.
void rt_str_from_double(double x, char *out, size_t cap, RtError *out_err) {
    rt_format(out, cap, "%.17g", x);
    if (out_err)
        *out_err = RT_ERROR_NONE;
}

/// @brief Serialize a float with nine significant decimal digits.
/// @details Promotes @p x to double and uses the C-locale `%.9g` conversion.
///          After @ref rt_format returns, an optional @p out_err is overwritten
///          with `RT_ERROR_NONE`.
/// @param x Value to format, including supported non-finite values.
/// @param out Caller-owned destination buffer.
/// @param cap Capacity of @p out in bytes, including the terminator.
/// @param out_err Optional destination overwritten with `RT_ERROR_NONE`.
void rt_str_from_float(float x, char *out, size_t cap, RtError *out_err) {
    rt_format(out, cap, "%.9g", (double)x);
    if (out_err)
        *out_err = RT_ERROR_NONE;
}

/// @brief Format a 32-bit signed integer using the C locale.
/// @details Uses `PRId32` without padding or an explicit sign. After
///          @ref rt_format returns, an optional @p out_err is overwritten with
///          `RT_ERROR_NONE`.
/// @param x Value to format.
/// @param out Caller-owned destination buffer.
/// @param cap Capacity of @p out in bytes, including the terminator.
/// @param out_err Optional destination overwritten with `RT_ERROR_NONE`.
void rt_str_from_i32(int32_t x, char *out, size_t cap, RtError *out_err) {
    rt_format(out, cap, "%" PRId32, x);
    if (out_err)
        *out_err = RT_ERROR_NONE;
}

/// @brief Format a 16-bit signed integer into a caller buffer.
/// @details Uses `PRId16` without padding or an explicit sign. After
///          @ref rt_format returns, an optional @p out_err is overwritten with
///          `RT_ERROR_NONE`.
/// @param x Value to format.
/// @param out Caller-owned destination buffer.
/// @param cap Capacity of @p out in bytes, including the terminator.
/// @param out_err Optional destination overwritten with `RT_ERROR_NONE`.
void rt_str_from_i16(int16_t x, char *out, size_t cap, RtError *out_err) {
    rt_format(out, cap, "%" PRId16, x);
    if (out_err)
        *out_err = RT_ERROR_NONE;
}

#ifdef __cplusplus
}
#endif
