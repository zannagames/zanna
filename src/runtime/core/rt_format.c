#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_format.c
// Purpose: Implements numeric and CSV formatting helpers that mirror BASIC
//          runtime semantics. Provides deterministic double-to-string
//          display and exact round-trip conversion with C-locale decimal
//          separators, special-value handling (NaN, infinity), and CSV string
//          quoting.
//
// Key invariants:
//   - Caller-provided output buffers must be non-NULL and non-zero in capacity;
//     invalid parameters cause an immediate trap via rt_trap.
//   - Floating-point formatting is performed under the C numeric locale so
//     output is stable across host environments.
//   - Truncation during formatting is treated as a fatal error; callers must
//     provide buffers large enough for the expected value range.
//   - NaN and infinity are formatted as their canonical string representations
//     ("NaN", "Inf", "-Inf") rather than locale-dependent variants.
//   - BASIC display formatting intentionally uses 15 significant digits for
//     compatibility, while conversion formatting uses enough digits to
//     round-trip exactly.
//   - CSV quoting doubles internal double-quotes and wraps the result in
//     double-quote delimiters.
//
// Ownership/Lifetime:
//   - CSV helpers (rt_format_csv_string) return newly allocated rt_string
//     values that transfer ownership to the caller; caller must unref when done.
//   - Buffer-based helpers (rt_format_f64) write into caller-supplied storage
//     and do not retain any pointers.
//
// Links: src/runtime/core/rt_format.h (public API),
//        src/runtime/core/rt_fmt.c (higher-level Zanna.Text.Fmt namespace),
//        src/runtime/core/rt_string_format.c (numeric parsing and conversion)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements C-locale floating-point and CSV string formatting.

#include "rt_format.h"
#include "rt_internal.h"

#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <xlocale.h>
#endif

/// @brief Locale-isolated `vsnprintf` that always uses the C locale's numeric formatting.
/// @details Wraps the platform-specific locale APIs (`_create_locale` + `_vsnprintf_l` on
///          Win32; `newlocale` + `uselocale` + `vsnprintf` on POSIX) so the runtime emits
///          deterministic decimal points regardless of the process's `LC_NUMERIC` setting.
///          Returns the number of bytes that would have been written (per `vsnprintf`
///          semantics), or -1 on locale-acquisition failure.
/// @param buffer Destination storage accepted by the platform formatter.
/// @param capacity Size of @p buffer in bytes.
/// @param fmt Non-NULL `printf`-style format string.
/// @param args Initialized argument list matching @p fmt.
/// @return Result reported by the platform locale-aware formatter, or -1 when
///   a C numeric locale cannot be installed.
static int rt_format_vsnprintf_c_locale(char *buffer,
                                        size_t capacity,
                                        const char *fmt,
                                        va_list args) {
#if defined(_WIN32)
    _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
    if (!c_locale)
        return -1;
#if !defined(_MSC_VER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    int written = _vsnprintf_l(buffer, capacity, fmt, c_locale, args);
#if !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif
    _free_locale(c_locale);
    return written;
#else
    locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (!c_locale)
        return -1;
    locale_t previous = uselocale(c_locale);
    if (previous == (locale_t)0) {
        freelocale(c_locale);
        return -1;
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    int written = vsnprintf(buffer, capacity, fmt, args);
#pragma GCC diagnostic pop
    uselocale(previous);
    freelocale(c_locale);
    return written;
#endif
}

/// @brief Variadic wrapper around `rt_format_vsnprintf_c_locale`.
/// @details Creates and finalizes the argument list while preserving the
///   underlying formatter's return convention.
/// @param buffer Destination storage accepted by the platform formatter.
/// @param capacity Size of @p buffer in bytes.
/// @param fmt Non-NULL `printf`-style format string followed by matching arguments.
/// @return Result reported by @ref rt_format_vsnprintf_c_locale.
int rt_format_snprintf_c_locale(char *buffer, size_t capacity, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int written = rt_format_vsnprintf_c_locale(buffer, capacity, fmt, args);
    va_end(args);
    return written;
}

/// @brief Parse a formatter candidate under the C numeric locale.
/// @details Requires the complete NUL-terminated input to be consumed. Locale
///   creation or switching failure, invalid trailing text, and invalid
///   arguments are reported uniformly as failure.
/// @param text Non-NULL NUL-terminated candidate representation.
/// @param out Non-NULL destination receiving the parsed value on success.
/// @return 1 when the entire candidate parses, otherwise 0; @p out is changed
///   only on success.
static int rt_format_strtod_c_locale(const char *text, double *out) {
    if (!text || !out)
        return 0;
    char *endptr = NULL;
#if defined(_WIN32)
    _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
    if (!c_locale)
        return 0;
    double parsed = _strtod_l(text, &endptr, c_locale);
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
    double parsed = strtod(text, &endptr);
    uselocale(previous);
    freelocale(c_locale);
#endif
    if (!endptr || *endptr != '\0')
        return 0;
    *out = parsed;
    return 1;
}

/// @brief Compare two doubles by exact IEEE-754 representation.
/// @details Copies both values into unsigned integers so the comparison
///   distinguishes signed zero and preserves NaN payload distinctions without
///   violating strict aliasing.
/// @param a First double to compare.
/// @param b Second double to compare.
/// @return 1 when all 64 representation bits match, otherwise 0.
static int rt_format_f64_bits_equal(double a, double b) {
    uint64_t aa = 0;
    uint64_t bb = 0;
    memcpy(&aa, &a, sizeof(aa));
    memcpy(&bb, &b, sizeof(bb));
    return aa == bb;
}

/// @brief Copy formatted text into a caller-provided buffer.
/// @details Validates buffer arguments, traps on truncation, and performs a
///          full copy including the null terminator.  Keeping the check in a
///          helper avoids repeating guard logic across the formatting functions.
/// @param text Null-terminated string to write.
/// @param buffer Destination buffer supplied by the caller.
/// @param capacity Size of the destination buffer in bytes.
/// @note Invalid or undersized destinations raise a trap. If trap dispatch
///   returns after truncation, the destination is set to an empty C string.
static void rt_format_write(const char *text, char *buffer, size_t capacity) {
    if (!buffer || capacity == 0) {
        rt_trap("rt_format_f64: invalid buffer");
        return;
    }
    size_t len = strlen(text);
    if (len + 1 > capacity) {
        rt_trap("rt_format_f64: truncated");
        buffer[0] = '\0';
        return;
    }
    memcpy(buffer, text, len + 1);
}

/// @brief Handle NaN / ±infinity printing for both formatter entry points.
/// @details Writes the canonical spellings (`"NaN"`, `"Inf"`, `"-Inf"`) into
///          @p buffer and returns 1 when @p value is non-finite; otherwise
///          leaves the buffer untouched and returns 0 so the caller proceeds
///          to its decimal formatter.
/// @param value    Value to classify.
/// @param buffer   Destination buffer for the special-value string.
/// @param capacity Capacity of @p buffer in bytes (must include the NUL).
/// @return 1 if a special-value string was written, 0 if @p value is finite.
static int rt_format_f64_write_special(double value, char *buffer, size_t capacity) {
    if (isnan(value)) {
        rt_format_write("NaN", buffer, capacity);
        return 1;
    }
    if (isinf(value)) {
        if (signbit(value))
            rt_format_write("-Inf", buffer, capacity);
        else
            rt_format_write("Inf", buffer, capacity);
        return 1;
    }
    return 0;
}

/// @brief Format a double according to BASIC display rules.
/// @details Handles NaN and infinity explicitly. Finite values use the legacy
///          15-significant-digit `%g` display form used by BASIC `PRINT`
///          output. This is intentionally not an exact-round-trip formatter;
///          use @ref rt_format_f64_roundtrip for conversion APIs that require
///          exact reparsing.
/// @param value Floating-point value to format.
/// @param buffer Destination buffer supplied by the caller.
/// @param capacity Size of the destination buffer in bytes.
/// @note Invalid buffers and formatting failures raise a trap. When dispatch
///   returns and a writable destination exists, the buffer is cleared.
void rt_format_f64(double value, char *buffer, size_t capacity) {
    if (!buffer || capacity == 0) {
        rt_trap("rt_format_f64: invalid buffer");
        return;
    }

    if (rt_format_f64_write_special(value, buffer, capacity))
        return;

    int written = rt_format_snprintf_c_locale(buffer, capacity, "%.15g", value);
    if (written < 0) {
        rt_trap("rt_format_f64: format error");
        buffer[0] = '\0';
        return;
    }
    if ((size_t)written >= capacity) {
        rt_trap("rt_format_f64: truncated");
        buffer[0] = '\0';
        return;
    }
}

/// @brief Compare two candidate round-trip strings and decide whether to replace the current best.
/// @details The shortest exact-reparse representation wins. When two
///          candidates are the same length, the fixed-notation form is
///          preferred over scientific notation so integer-valued doubles
///          remain readable without extra bytes.
/// @param candidate Newly-generated formatted representation.
/// @param best      The current best representation (must be non-null and non-empty).
/// @return Non-zero if @p candidate should replace @p best.
static int rt_format_should_replace_roundtrip(const char *candidate, const char *best) {
    size_t candidate_len = strlen(candidate);
    size_t best_len = strlen(best);
    if (candidate_len != best_len)
        return candidate_len < best_len;

    int candidate_exp = strchr(candidate, 'e') != NULL || strchr(candidate, 'E') != NULL;
    int best_exp = strchr(best, 'e') != NULL || strchr(best, 'E') != NULL;
    return best_exp && !candidate_exp;
}

/// @brief Format a double according to exact round-trip conversion rules.
/// @details Handles NaN and infinity explicitly. For finite values uses a
///          shortest-round-trip search: tries `%.*g` at precision 1 through 17,
///          keeps candidates whose `strtod` recovers the original double, and
///          chooses the shortest text. Ties prefer fixed notation over
///          exponent notation so integer-valued doubles remain readable when
///          that costs no additional bytes.
/// @param value Floating-point value to format.
/// @param buffer Destination buffer supplied by the caller.
/// @param capacity Size of the destination buffer in bytes.
/// @note Invalid buffers and formatting failures raise a trap. When dispatch
///   returns and a writable destination exists, the buffer is cleared.
void rt_format_f64_roundtrip(double value, char *buffer, size_t capacity) {
    if (!buffer || capacity == 0) {
        rt_trap("rt_format_f64_roundtrip: invalid buffer");
        return;
    }

    if (rt_format_f64_write_special(value, buffer, capacity))
        return;

    char candidate[64];
    char best[64];
    best[0] = '\0';
    for (int precision = 1; precision <= 17; ++precision) {
        int n = rt_format_snprintf_c_locale(candidate, sizeof(candidate), "%.*g", precision, value);
        if (n < 0 || (size_t)n >= sizeof(candidate))
            continue;
        double reparsed = 0.0;
        if (rt_format_strtod_c_locale(candidate, &reparsed) &&
            rt_format_f64_bits_equal(value, reparsed)) {
            if (best[0] == '\0' || rt_format_should_replace_roundtrip(candidate, best))
                memcpy(best, candidate, (size_t)n + 1);
        }
    }

    if (best[0] != '\0') {
        rt_format_write(best, buffer, capacity);
        return;
    }

    int written = rt_format_snprintf_c_locale(candidate, sizeof(candidate), "%.17g", value);
    if (written < 0 || (size_t)written >= sizeof(candidate)) {
        rt_trap("rt_format_f64_roundtrip: format error");
        buffer[0] = '\0';
        return;
    }
    rt_format_write(candidate, buffer, capacity);
}

/// @brief Allocate a freshly quoted CSV string from a runtime string handle.
/// @details Duplicates the incoming text, doubles embedded quotes, wraps the
///          content in leading and trailing quotes, and returns a new
///          @ref rt_string that owns the allocated buffer.  The function traps
///          on allocation failure to match runtime expectations.
/// @param value Runtime string handle to quote. May be null for an empty string.
/// @return Caller-owned runtime string containing the CSV-safe text. If an
///   error trap returns locally, an owned empty string is returned.
rt_string rt_csv_quote_alloc(rt_string value) {
    const char *data = "";
    size_t len = 0;
    if (value) {
        data = rt_string_cstr(value);
        int64_t raw_len = rt_str_len(value);
        if (raw_len < 0) {
            rt_trap("rt_csv_quote_alloc: invalid string length");
            return rt_string_from_bytes("", 0);
        }
        len = (size_t)raw_len;
    }

    size_t extra = 0;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '"') {
            if (extra == SIZE_MAX) {
                rt_trap("rt_csv_quote_alloc: string too large");
                return rt_string_from_bytes("", 0);
            }
            ++extra;
        }
    }

    if (len > SIZE_MAX - extra || len + extra > SIZE_MAX - 2) {
        rt_trap("rt_csv_quote_alloc: string too large");
        return rt_string_from_bytes("", 0);
    }
    size_t total = len + extra + 2; // leading and trailing quotes
    char *buffer = (char *)malloc(total + 1);
    if (!buffer) {
        rt_trap("rt_csv_quote_alloc: out of memory");
        return rt_string_from_bytes("", 0);
    }

    size_t pos = 0;
    buffer[pos++] = '"';
    for (size_t i = 0; i < len; ++i) {
        char ch = data[i];
        buffer[pos++] = ch;
        if (ch == '"') {
            buffer[pos++] = '"';
        }
    }
    buffer[pos++] = '"';
    buffer[pos] = '\0';

    rt_string result = rt_string_from_bytes(buffer, total);
    free(buffer);
    return result;
}
