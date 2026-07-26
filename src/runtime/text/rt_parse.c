//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_parse.c
// Purpose: Implements safe integer, double, Boolean, validation, default-value,
//          radix, and Option parsing helpers for the Zanna.Core.Parse namespace.
//          Ordinary invalid input returns false/None/a caller-supplied default
//          instead of trapping.
//
// Key invariants:
//   - TryParse functions report ordinary invalid input with false rather than a
//     parse trap.
//   - NULL output pointers cause immediate false return.
//   - Non-NULL output pointers are always reset before parsing so failed parses
//     cannot leak a caller's previous value.
//   - Empty strings are treated as invalid for all types.
//   - Integer overflow causes false return; the output is not written.
//   - Floating-point parsing is isolated to the C numeric locale, so the
//     decimal separator is always '.'.
//   - Embedded NUL bytes are rejected so hidden suffixes cannot be ignored.
//   - Bool parsing accepts true/yes/1/on and false/no/0/off
//     case-insensitively.
//
// Ownership/Lifetime:
//   - Input strings are borrowed and no parser retains state between calls.
//   - TryParse, predicate, radix, and default-value helpers do not allocate
//     runtime objects; Option wrappers return newly allocated Option objects.
//
// Links: src/runtime/text/rt_parse.h (public API),
//        src/runtime/text/rt_scanner.h (lower-level character scanning)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_parse.c
 * @brief Implements non-trapping strict scalar parsing helpers.
 * @details The parser accepts complete ASCII-whitespace-delimited integer,
 *          C-locale floating, Boolean, and arbitrary-radix representations,
 *          rejects embedded NUL and trailing content, resets output before
 *          failure, and provides predicates, defaults, and Option wrappers.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "rt_parse.h"
#include "rt_internal.h"
#include "rt_option.h"

#include <ctype.h>
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

/// @brief Test whether a byte belongs to the fixed ASCII whitespace set.
/// @param ch Byte to classify.
/// @return Nonzero for space, tab, line feed, carriage return, form feed, or
///         vertical tab; zero otherwise.
static inline int is_ascii_space(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

/// @brief Advance a pointer past ASCII whitespace characters.
/// @param s Null-terminated string cursor to advance.
/// @return Pointer to the first non-whitespace byte or the terminating null.
static inline const char *skip_whitespace(const char *s) {
    while (*s && is_ascii_space((unsigned char)*s))
        ++s;
    return s;
}

/// @brief Check whether a cursor is followed only by ASCII whitespace.
/// @param s Null-terminated cursor to inspect.
/// @return Nonzero when skipping whitespace reaches the string terminator.
static inline int is_end_of_input(const char *s) {
    s = skip_whitespace(s);
    return *s == '\0';
}

/// @brief Compare two null-terminated strings case-insensitively in ASCII.
/// @param a First string.
/// @param b Second string.
/// @return Nonzero when both strings contain the same ASCII letters and bytes.
static inline int str_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

/// @brief Decode an ASCII digit character to its integer value, supporting bases up to 36.
/// @details Accepts decimal digits (`0-9` → 0-9), lowercase letters (`a-z` → 10-35), and
///          uppercase letters (`A-Z` → 10-35). Returns -1 for any other byte so callers
///          can detect "non-digit" without ambiguity. Used by `rt_parse_int_radix`.
/// @param ch Byte to decode.
/// @return Digit value in `[0, 35]`, or `-1` when @p ch is not an ASCII radix digit.
static inline int radix_digit_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9')
        return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'z')
        return (int)(ch - 'a') + 10;
    if (ch >= 'A' && ch <= 'Z')
        return (int)(ch - 'A') + 10;
    return -1;
}

/// @brief Return @p s's byte buffer if it's a live handle with no embedded NUL, else NULL.
/// @details Combines handle validation with the embedded-NUL rejection rule: TryParse
///          callers want false (not a trap) on bad input, but they must not silently
///          accept a string whose declared length exceeds the first NUL — that would
///          let a hidden suffix bypass the parser. NULL handles, invalid handles, and
///          NUL-containing strings all map to NULL so the caller short-circuits.
/// @param s Runtime string handle to validate and inspect.
/// @return Borrowed pointer to the string's null-terminated byte buffer, or
///         `NULL` for a null, invalid, unbacked, or embedded-null string.
static const char *string_cstr_without_embedded_nul(rt_string s) {
    if (!s || !rt_string_is_handle((const void *)s) || !s->data)
        return NULL;

    const char *text = s->data;
    size_t len = (size_t)rt_str_len(s);
    if (memchr(text, '\0', len))
        return NULL;

    return text;
}

/// @brief Walk past a well-formed decimal float at @p cursor, returning the byte after it.
/// @details Recognises the grammar `[+|-]?([0-9]+('.'[0-9]*)?|'.'[0-9]+)
///          ([eE][+|-]?[0-9]+)?`.
///          Returns NULL when no recognisable number is found at the cursor (zero
///          mantissa digits, or a missing exponent body after `e`/`E`). Otherwise
///          returns a pointer one byte past the last consumed character — callers use
///          this for trailing-suffix validation (`Try*` parsers reject any non-empty
///          tail to keep the contract strict).
/// @param cursor Null-terminated cursor positioned at the potential literal.
/// @return Pointer immediately after a valid decimal literal, or `NULL` when
///         the prefix does not satisfy the decimal grammar.
static const char *scan_decimal_float(const char *cursor) {
    if (!cursor)
        return NULL;

    const char *p = cursor;
    if (*p == '+' || *p == '-')
        ++p;

    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        ++digits;
        ++p;
    }

    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            ++digits;
            ++p;
        }
    }

    if (digits == 0)
        return NULL;

    if (*p == 'e' || *p == 'E') {
        const char *exp = p + 1;
        if (*exp == '+' || *exp == '-')
            ++exp;
        int exp_digits = 0;
        while (*exp >= '0' && *exp <= '9') {
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
/// @details Accepts the formatter's `NaN`, `Inf`, and `-Inf` spellings, the long
///          `Infinity`/`-Infinity` form emitted by `Fmt.Num`, a leading `+`, and
///          case-insensitive input so Convert.ToString_Double, Fmt.Num, and
///          Parse/Convert.ToDouble round-trip through the public APIs.
/// @param cursor Null-terminated cursor positioned at the potential literal.
/// @param out_value Destination for the decoded NaN or infinity.
/// @param out_end Destination for the first byte after the recognized literal.
/// @return `1` when a complete special-value prefix is recognized, otherwise `0`.
static int scan_nonfinite_float(const char *cursor, double *out_value, const char **out_end) {
    if (!cursor || !out_value || !out_end)
        return 0;
    const char *p = cursor;
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
        const char *end = p + 3;
        // Consume the long "Infinity" spelling when present (case-insensitive).
        static const char kInityLower[] = "inity";
        const char *tail = p + 3;
        int matchesLong = 1;
        for (int i = 0; i < 5; ++i) {
            const char ch = tail[i];
            if (ch == '\0' ||
                (char)tolower((unsigned char)ch) != kInityLower[i]) {
                matchesLong = 0;
                break;
            }
        }
        if (matchesLong)
            end = p + 8;
        *out_value = negative ? -INFINITY : INFINITY;
        *out_end = end;
        return 1;
    }
    return 0;
}

/// @brief Try to parse a runtime string as a signed 64-bit decimal integer.
/// @details Leading and trailing ASCII whitespace is allowed. Empty input,
///          embedded nulls, overflow, and any non-whitespace suffix fail.
///          @p out_value is reset to zero before input validation.
/// @param s Borrowed runtime string to parse.
/// @param out_value Destination for the parsed integer; must be non-null.
/// @return `1` on success, or `0` on failure.
int8_t rt_parse_try_int(rt_string s, int64_t *out_value) {
    if (!out_value)
        return 0;
    *out_value = 0;
    if (!s)
        return 0;

    const char *text = string_cstr_without_embedded_nul(s);
    if (!text)
        return 0;

    const char *cursor = skip_whitespace(text);
    if (*cursor == '\0')
        return 0;

    errno = 0;
    char *endptr = NULL;
    long long parsed = strtoll(cursor, &endptr, 10);

    if (errno == ERANGE)
        return 0;
    if (!endptr || endptr == cursor)
        return 0;
    if (!is_end_of_input(endptr))
        return 0;

    *out_value = (int64_t)parsed;
    return 1;
}

/// @brief Try to parse a runtime string as a double-precision number.
/// @details Decimal syntax is checked before conversion and always uses `.` as
///          the decimal point. Conversion runs in a C numeric locale without
///          changing the process-global locale. Case-insensitive `NaN`, `Inf`,
///          and `Infinity` spellings accept an optional sign; a decimal
///          conversion that becomes non-finite fails. @p out_value is reset to
///          zero before input validation.
/// @param s Borrowed runtime string to parse.
/// @param out_value Destination for the parsed number; must be non-null.
/// @return `1` on success, or `0` on invalid input or locale setup failure.
int8_t rt_parse_try_num(rt_string s, double *out_value) {
    if (!out_value)
        return 0;
    *out_value = 0.0;
    if (!s)
        return 0;

    const char *text = string_cstr_without_embedded_nul(s);
    if (!text)
        return 0;

    const char *cursor = skip_whitespace(text);
    if (*cursor == '\0')
        return 0;

    const char *nonfinite_end = NULL;
    double nonfinite = 0.0;
    if (scan_nonfinite_float(cursor, &nonfinite, &nonfinite_end)) {
        if (!is_end_of_input(nonfinite_end))
            return 0;
        *out_value = nonfinite;
        return 1;
    }

    const char *literal_end = scan_decimal_float(cursor);
    if (!literal_end || !is_end_of_input(literal_end))
        return 0;

    errno = 0;
    char *endptr = NULL;
    double value = 0.0;

#if defined(_WIN32)
    _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
    if (!c_locale)
        return 0;
    value = _strtod_l(cursor, &endptr, c_locale);
    _free_locale(c_locale);
#else
    locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (!c_locale)
        return 0;
    locale_t previous = uselocale(c_locale);
    if (previous == (locale_t)0) {
        freelocale(c_locale);
        return 0;
    }
    value = strtod(cursor, &endptr);
    uselocale(previous);
    freelocale(c_locale);
#endif

    if (!isfinite(value))
        return 0;
    if (!endptr || endptr == cursor)
        return 0;
    if (!is_end_of_input(endptr))
        return 0;

    *out_value = value;
    return 1;
}

/// @brief Try to parse a runtime string as a Boolean.
/// @details Accepts `true`, `yes`, `1`, and `on` as true, and `false`, `no`,
///          `0`, and `off` as false, all case-insensitively. Leading and
///          trailing ASCII whitespace is allowed; embedded nulls, extra words,
///          and all other tokens fail. @p out_value is reset to zero first.
/// @param s Borrowed runtime string to parse.
/// @param out_value Destination for normalized zero or one; must be non-null.
/// @return `1` on success, or `0` on failure.
int8_t rt_parse_try_bool(rt_string s, int8_t *out_value) {
    if (!out_value)
        return 0;
    *out_value = 0;
    if (!s)
        return 0;

    const char *text = string_cstr_without_embedded_nul(s);
    if (!text)
        return 0;

    const char *cursor = skip_whitespace(text);
    if (*cursor == '\0')
        return 0;

    // Extract the word (until whitespace or end)
    char word[16];
    size_t i = 0;
    while (*cursor && !is_ascii_space((unsigned char)*cursor) && i < sizeof(word) - 1)
        word[i++] = *cursor++;
    word[i] = '\0';

    // Check for trailing non-whitespace
    if (!is_end_of_input(cursor))
        return 0;

    // Check for true values
    if (str_eq_ci(word, "true") || str_eq_ci(word, "yes") || str_eq_ci(word, "1") ||
        str_eq_ci(word, "on")) {
        *out_value = 1;
        return 1;
    }

    // Check for false values
    if (str_eq_ci(word, "false") || str_eq_ci(word, "no") || str_eq_ci(word, "0") ||
        str_eq_ci(word, "off")) {
        *out_value = 0;
        return 1;
    }

    return 0;
}

/// @brief Parse a decimal integer or return a caller-selected fallback.
/// @details Applies the same strict syntax and whitespace rules as
///          `rt_parse_try_int`.
/// @param s Borrowed runtime string to parse.
/// @param default_value Value returned when parsing fails.
/// @return Parsed signed integer, or @p default_value.
int64_t rt_parse_int_or(rt_string s, int64_t default_value) {
    int64_t result;
    if (rt_parse_try_int(s, &result))
        return result;
    return default_value;
}

/// @brief Parse-or-default convenience for doubles.
/// @details Applies the same decimal, non-finite, whitespace, and locale rules
///          as `rt_parse_try_num`.
/// @param s Borrowed runtime string to parse.
/// @param default_value Value returned when parsing fails.
/// @return Parsed number, or @p default_value.
double rt_parse_num_or(rt_string s, double default_value) {
    double result;
    if (rt_parse_try_num(s, &result))
        return result;
    return default_value;
}

/// @brief Parse-or-default convenience for booleans.
/// @details Applies the same accepted tokens and whitespace rules as
///          `rt_parse_try_bool`.
/// @param s Borrowed runtime string to parse.
/// @param default_value Value returned when parsing fails.
/// @return Parsed normalized Boolean, or @p default_value unchanged.
int8_t rt_parse_bool_or(rt_string s, int8_t default_value) {
    int8_t result;
    if (rt_parse_try_bool(s, &result))
        return result;
    return default_value;
}

/// @brief Returns 1 if `s` parses as a valid integer (no value extracted). Equivalent to a
/// `try_int` call that discards the result — handy for input validation gates.
/// @param s Borrowed runtime string to validate.
/// @return `1` when @p s satisfies `rt_parse_try_int`, otherwise `0`.
int8_t rt_parse_is_int(rt_string s) {
    int64_t dummy;
    return rt_parse_try_int(s, &dummy);
}

/// @brief Returns 1 if `s` parses as a valid double (locale-isolated, like `try_num`).
/// @param s Borrowed runtime string to validate.
/// @return `1` when @p s satisfies `rt_parse_try_num`, otherwise `0`.
int8_t rt_parse_is_num(rt_string s) {
    double dummy;
    return rt_parse_try_num(s, &dummy);
}

/// @brief Parse an integer in any radix from 2 to 36 (covers binary, octal,
/// decimal, hex, base32, base36). Returns `default_value` for out-of-range
/// radix or any parse failure. Alphabetic digits beyond 9 use `a-z`/`A-Z`
/// case-insensitively. Decimal input may use a leading '+' or '-' so decimal
/// Fmt.IntRadix output round-trips; non-decimal input rejects signs and parses
/// the full unsigned 64-bit bit pattern before casting it back to int64_t.
/// Leading and trailing ASCII whitespace is accepted, but radix prefixes and
/// embedded null bytes are rejected.
/// @param s Borrowed runtime string to parse.
/// @param radix Numeric base in the inclusive range `[2, 36]`.
/// @param default_value Value returned for invalid radix, syntax, or overflow.
/// @return Parsed signed integer or non-decimal 64-bit bit pattern, or
///         @p default_value on failure.
int64_t rt_parse_int_radix(rt_string s, int64_t radix, int64_t default_value) {
    // Validate radix range
    if (radix < 2 || radix > 36)
        return default_value;
    if (!s)
        return default_value;

    const char *text = string_cstr_without_embedded_nul(s);
    if (!text)
        return default_value;

    const char *cursor = skip_whitespace(text);
    if (*cursor == '\0')
        return default_value;

    int negative = 0;
    if (*cursor == '-') {
        if (radix != 10)
            return default_value;
        negative = 1;
        ++cursor;
    } else if (*cursor == '+') {
        if (radix != 10)
            return default_value;
        ++cursor;
    }

    if (*cursor == '\0')
        return default_value;

    uint64_t value = 0;
    uint64_t limit = UINT64_MAX;
    if (radix == 10)
        limit = negative ? ((uint64_t)INT64_MAX + 1ULL) : (uint64_t)INT64_MAX;

    const char *p = cursor;
    for (; *p && !is_ascii_space((unsigned char)*p); ++p) {
        int digit = radix_digit_value((unsigned char)*p);
        if (digit < 0 || digit >= radix)
            return default_value;
        uint64_t udigit = (uint64_t)digit;
        uint64_t uradix = (uint64_t)radix;
        if (value > (limit - udigit) / uradix)
            return default_value;
        value = value * uradix + udigit;
    }

    if (p == cursor || !is_end_of_input(p))
        return default_value;

    if (radix == 10) {
        if (negative) {
            if (value == ((uint64_t)INT64_MAX + 1ULL))
                return INT64_MIN;
            return -(int64_t)value;
        }
        return (int64_t)value;
    }

    int64_t result = 0;
    memcpy(&result, &value, sizeof(result));
    return result;
}

/// @brief Parse a number and box the result in an Option.
/// @details Uses `rt_parse_try_num`; failure produces `None`, while success
///          produces a newly allocated `Some(f64)`. Option construction may
///          trap if allocation fails.
/// @param s Borrowed runtime string to parse.
/// @return Caller-owned `Some(f64)` on success or caller-owned `None` on failure.
void *rt_parse_double_option(rt_string s) {
    double value = 0.0;
    if (!rt_parse_try_num(s, &value))
        return rt_option_none();
    return rt_option_some_f64(value);
}

/// @brief Parse a decimal integer and box the result in an Option.
/// @details Uses `rt_parse_try_int`; failure produces `None`, while success
///          produces a newly allocated `Some(i64)`. Option construction may
///          trap if allocation fails.
/// @param s Borrowed runtime string to parse.
/// @return Caller-owned `Some(i64)` on success or caller-owned `None` on failure.
void *rt_parse_int64_option(rt_string s) {
    int64_t value = 0;
    if (!rt_parse_try_int(s, &value))
        return rt_option_none();
    return rt_option_some_i64(value);
}

/// @brief Parse a Boolean and box the normalized result in an Option.
/// @details Uses `rt_parse_try_bool`; failure produces `None`, while success
///          produces a newly allocated integer-backed `Some(i1)`. Option
///          construction may trap if allocation fails.
/// @param s Borrowed runtime string to parse.
/// @return Caller-owned `Some(i1)` on success or caller-owned `None` on failure.
void *rt_parse_bool_option(rt_string s) {
    int8_t value = 0;
    if (!rt_parse_try_bool(s, &value))
        return rt_option_none();
    return rt_option_some_i1(value);
}

#ifdef __cplusplus
}
#endif
