//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_fmt.c
// Purpose: Implements the Zanna.Text.Fmt namespace — value-to-string formatting
//          helpers for integers, floats, booleans, and radix-based encodings.
//          All functions are defensive: invalid inputs produce empty strings
//          or sensible defaults rather than trapping.
//
// Key invariants:
//   - No function in this file ever calls rt_trap; all error paths return an
//     empty string or a defined fallback value.
//   - Integer formatting uses PRId64/PRIu64 macros for locale-independent
//     decimal output.
//   - Radix formatting (rt_fmt_int_radix) supports bases 2-36; values outside
//     this range return an empty string.
//   - Float formatting uses an isolated C locale; NaN and infinity are
//     formatted as their canonical string representations.
//   - All functions return newly allocated rt_string values.
//
// Ownership/Lifetime:
//   - All returned rt_string values are newly allocated and transfer ownership
//     to the caller; the caller must call rt_string_unref when done.
//   - Intermediate stack buffers (FMT_BUFFER_SIZE = 128) are used for most
//     operations; heap allocation is used when output size depends on caller
//     supplied multi-byte padding, grouping, or currency symbols.
//
// Links: src/runtime/core/rt_fmt.h (public API),
//        src/runtime/core/rt_format.c (floating-point and CSV formatting),
//        src/runtime/core/rt_string.h (rt_string_from_bytes)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements deterministic, allocation-owning value formatters.
/// @details Numeric conversion is isolated from the process locale, and public
///   failure paths return an owned empty runtime string instead of trapping.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "rt_fmt.h"
#include "rt_internal.h"

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

// Buffer size for most formatting operations
/// @brief Default temporary buffer size for formatting operations.
/// @details Chosen to accommodate typical numeric formats without heap
///          allocation while keeping stack usage predictable.
#define FMT_BUFFER_SIZE 128
/// @brief Buffer size for fixed-point double formatting at full double range.
/// @details DBL_MAX has 309 integer digits; with 20 decimals, sign, decimal
///          point, and terminator, 384 bytes leaves headroom without heap use.
#define FMT_FIXED_BUFFER_SIZE 384
// Buffer size for binary formatting (64 bits + null)
/// @brief Buffer size for binary formatting of 64-bit values.
/// @details Holds 64 digits plus a NUL terminator.
#define FMT_BIN_BUFFER_SIZE 65

/// @brief Allocate and return an empty runtime string.
/// @details Used as the failure return for every formatter so callers always
///          receive a valid handle, even when conversion overflowed the
///          internal buffer.
/// @return Newly-allocated empty `rt_string` owned by the caller.
static rt_string rt_fmt_empty(void) {
    return rt_string_from_bytes("", 0);
}

/// @brief Add two `size_t` values reporting overflow without crashing.
/// @details Helper used by the digit-grouping path to avoid integer-overflow
///          UB in buffer-size calculations. Returns 0 on overflow rather than
///          trapping so the caller can fall back to an empty result.
/// @param a   First addend.
/// @param b   Second addend.
/// @param out On success, receives `a + b`.
/// @return 1 when the addition fits in `size_t`, 0 on overflow.
static int rt_fmt_checked_add(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b)
        return 0;
    *out = a + b;
    return 1;
}

/// @brief `vsnprintf` wrapped to run under the C numeric locale.
/// @details Formats numeric output deterministically regardless of the
///          process `setlocale` state: on Windows uses `_vsnprintf_l` with a
///          freshly-created `LC_NUMERIC = "C"` locale; on POSIX swaps in a
///          `newlocale("C")` for the duration of the call via `uselocale`.
/// @param buffer Destination buffer (must be non-null and capacity > 0).
/// @param size   Capacity of @p buffer in bytes including the NUL terminator.
/// @param fmt    `printf`-style format string.
/// @param args   Pre-started `va_list` consumed by the formatter.
/// @return Same as `vsnprintf` — number of bytes that *would* have been
///         written excluding the NUL, or a negative value on failure.
static int rt_fmt_vsnprintf_c_locale(char *buffer, size_t size, const char *fmt, va_list args) {
    if (!buffer || size == 0 || !fmt)
        return -1;

#if defined(_WIN32)
    _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
    if (!c_locale)
        return -1;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    int written = _vsnprintf_l(buffer, size, fmt, c_locale, args);
#if defined(__GNUC__) || defined(__clang__)
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
    int written = vsnprintf(buffer, size, fmt, args);
#pragma GCC diagnostic pop
    uselocale(previous);
    freelocale(c_locale);
    return written;
#endif
}

/// @brief Variadic wrapper over @ref rt_fmt_vsnprintf_c_locale.
/// @details Convenience for one-off format calls. See @ref rt_fmt_vsnprintf_c_locale
///          for the locale-isolation contract.
/// @param buffer Destination buffer.
/// @param size   Capacity in bytes including the NUL terminator.
/// @param fmt    `printf`-style format string.
/// @return Same as `snprintf` — bytes that would have been written, or negative on failure.
static int rt_fmt_snprintf_c_locale(char *buffer, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int written = rt_fmt_vsnprintf_c_locale(buffer, size, fmt, args);
    va_end(args);
    return written;
}

/// @brief Extract the byte view of @p s, falling back to @p fallback on any failure.
/// @details Validates @p s as a live string handle and rejects zero-length
///          payloads. On a hit, writes the byte length through @p out_len and
///          returns the data pointer. On a miss, returns @p fallback and writes
///          `strlen(fallback)` so the caller can use the result uniformly.
/// @param s         Input runtime string (may be `NULL`).
/// @param fallback  C-string returned on rejection (must be valid).
/// @param out_len   Receives the byte length of the returned data (must be non-null).
/// @return Pointer to @p s's bytes on success, or @p fallback on rejection.
static const char *rt_fmt_string_bytes(rt_string s, const char *fallback, size_t *out_len) {
    if (!out_len)
        return fallback;
    if (!s) {
        *out_len = strlen(fallback);
        return fallback;
    }

    if (!rt_string_is_handle((const void *)s) || !s->data) {
        *out_len = strlen(fallback);
        return fallback;
    }

    const char *data = s->data;
    size_t len = (size_t)rt_str_len(s);
    if (!data || len == 0) {
        *out_len = strlen(fallback);
        return fallback;
    }

    *out_len = len;
    return data;
}

/// @brief Same as @ref rt_fmt_string_bytes but accepts an empty `rt_string`.
/// @details Used by paths that want to round-trip an empty input as-is rather
///          than substituting @p fallback for it. Only an invalid handle or
///          a `NULL` `data` pointer triggers the fallback.
/// @param s         Input runtime string (may be `NULL`).
/// @param fallback  C-string returned on rejection.
/// @param out_len   Receives the byte length of the returned data.
/// @return Pointer to @p s's bytes (possibly zero-length) on success, fallback otherwise.
static const char *rt_fmt_string_bytes_allow_empty(rt_string s,
                                                   const char *fallback,
                                                   size_t *out_len) {
    if (!out_len)
        return fallback;
    if (!s || !rt_string_is_handle((const void *)s) || !s->data) {
        *out_len = strlen(fallback);
        return fallback;
    }

    *out_len = (size_t)rt_str_len(s);
    return s->data;
}

/// @brief Test whether @p ch is a UTF-8 continuation byte.
/// @details Continuation bytes have the high two bits set to `10`.
/// @param ch Raw byte to classify.
/// @return Non-zero if @p ch is a continuation byte.
static int rt_fmt_is_utf8_cont(unsigned char ch) {
    return (ch & 0xC0) == 0x80;
}

/// @brief Return the byte length of the first UTF-8 code point in @p data.
/// @details Recognises 1-byte ASCII, 2-byte (`C2..DF`), 3-byte (`E0..EF` with
///          the architectural surrogate / overlong checks for the `E0`/`ED`
///          rows), and 4-byte (`F0..F4` with high-plane bound checks)
///          sequences. Returns 0 for invalid leading bytes, overlong
///          sequences, surrogate pairs, or truncated input.
/// @param data Pointer to UTF-8 bytes.
/// @param len  Number of bytes available at @p data.
/// @return Length in bytes of the first complete code point, or 0 on invalid input.
static size_t rt_fmt_first_utf8_len(const char *data, size_t len) {
    if (!data || len == 0)
        return 0;

    unsigned char first = (unsigned char)data[0];
    if (first < 0x80)
        return 1;

    if (first >= 0xC2 && first <= 0xDF) {
        if (len >= 2 && rt_fmt_is_utf8_cont((unsigned char)data[1]))
            return 2;
        return 0;
    }

    if (first == 0xE0) {
        if (len >= 3 && (unsigned char)data[1] >= 0xA0 && (unsigned char)data[1] <= 0xBF &&
            rt_fmt_is_utf8_cont((unsigned char)data[2]))
            return 3;
        return 0;
    }

    if ((first >= 0xE1 && first <= 0xEC) || (first >= 0xEE && first <= 0xEF)) {
        if (len >= 3 && rt_fmt_is_utf8_cont((unsigned char)data[1]) &&
            rt_fmt_is_utf8_cont((unsigned char)data[2]))
            return 3;
        return 0;
    }

    if (first == 0xED) {
        if (len >= 3 && (unsigned char)data[1] >= 0x80 && (unsigned char)data[1] <= 0x9F &&
            rt_fmt_is_utf8_cont((unsigned char)data[2]))
            return 3;
        return 0;
    }

    if (first == 0xF0) {
        if (len >= 4 && (unsigned char)data[1] >= 0x90 && (unsigned char)data[1] <= 0xBF &&
            rt_fmt_is_utf8_cont((unsigned char)data[2]) &&
            rt_fmt_is_utf8_cont((unsigned char)data[3]))
            return 4;
        return 0;
    }

    if (first >= 0xF1 && first <= 0xF3) {
        if (len >= 4 && rt_fmt_is_utf8_cont((unsigned char)data[1]) &&
            rt_fmt_is_utf8_cont((unsigned char)data[2]) &&
            rt_fmt_is_utf8_cont((unsigned char)data[3]))
            return 4;
        return 0;
    }

    if (first == 0xF4) {
        if (len >= 4 && (unsigned char)data[1] >= 0x80 && (unsigned char)data[1] <= 0x8F &&
            rt_fmt_is_utf8_cont((unsigned char)data[2]) &&
            rt_fmt_is_utf8_cont((unsigned char)data[3]))
            return 4;
        return 0;
    }

    return 0;
}

/// @brief Test whether @p text encodes decimal zero in any of the formatter's textual forms.
/// @details Ignores sign characters (`+`, `-`), a decimal point, an exponent
///          marker (`e`/`E`), and a trailing percent sign — anything else is
///          counted as a "digit". Returns true only when every counted digit
///          is `'0'`. Used to suppress the `-` prefix on `-0` / `-0.00%`.
/// @param text Pointer to decimal text (may be `NULL`, treated as zero).
/// @param len  Number of bytes in @p text.
/// @return 1 if every non-format character is `'0'`; 0 otherwise.
static int rt_fmt_decimal_string_is_zero(const char *text, size_t len) {
    if (!text)
        return 1;
    for (size_t i = 0; i < len; ++i) {
        char ch = text[i];
        if (ch == '.' || ch == '+' || ch == '-' || ch == 'e' || ch == 'E' || ch == '%')
            continue;
        if (ch != '0')
            return 0;
    }
    return 1;
}

/// @brief Allocate a new C buffer that groups @p digits into thousands separated by @p sep.
/// @details Lays out the first 1-3 digits, then alternating `sep`+three-digit
///          chunks, optionally prefixed by `-`. Overflow-checks every size
///          contribution before allocating so an absurdly long digit string
///          cannot wrap the buffer length.
/// @param digits     Pointer to ASCII digit characters (no sign / decimal).
/// @param digits_len Number of digits.
/// @param sep        Grouping separator (e.g. `","` or `"."`).
/// @param sep_len    Length of @p sep in bytes.
/// @param negative   Non-zero to prepend `-` to the result.
/// @param out_len    On success, receives the byte length of the buffer
///                   (not counting the NUL terminator). May be `NULL`.
/// @return Newly-allocated NUL-terminated buffer (caller-owned), or `NULL`
///         on allocation failure / overflow / invalid arguments.
static char *rt_fmt_group_digits_alloc(const char *digits,
                                       size_t digits_len,
                                       const char *sep,
                                       size_t sep_len,
                                       int negative,
                                       size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!digits || digits_len == 0 || !sep)
        return NULL;

    size_t groups = (digits_len - 1) / 3;
    size_t sep_total = 0;
    if (groups != 0) {
        if (sep_len > SIZE_MAX / groups)
            return NULL;
        sep_total = groups * sep_len;
    }

    size_t total = digits_len;
    if (!rt_fmt_checked_add(total, sep_total, &total))
        return NULL;
    if (negative && !rt_fmt_checked_add(total, 1, &total))
        return NULL;
    if (total == SIZE_MAX)
        return NULL;

    char *buf = (char *)malloc(total + 1);
    if (!buf)
        return NULL;

    char *dst = buf;
    if (negative)
        *dst++ = '-';

    size_t first_group = digits_len % 3;
    if (first_group == 0)
        first_group = 3;

    memcpy(dst, digits, first_group);
    dst += first_group;
    size_t pos = first_group;
    while (pos < digits_len) {
        memcpy(dst, sep, sep_len);
        dst += sep_len;
        memcpy(dst, digits + pos, 3);
        dst += 3;
        pos += 3;
    }
    *dst = '\0';

    if (out_len)
        *out_len = (size_t)(dst - buf);
    return buf;
}

/// @brief Append @p len bytes from @p text to @p buf at offset @p *pos.
/// @details Returns 0 (without writing) when the destination would overflow
///          or any pointer is NULL. On success, advances `*pos` by @p len.
/// @param buf   Destination buffer.
/// @param cap   Capacity of @p buf in bytes.
/// @param pos   Current write offset; receives the new offset on success.
/// @param text  Source bytes.
/// @param len   Number of bytes to copy.
/// @return 1 on success, 0 on overflow / invalid input.
static int rt_fmt_append_bytes(char *buf, size_t cap, size_t *pos, const char *text, size_t len) {
    if (!buf || !pos || !text || *pos > cap)
        return 0;
    if (len > cap - *pos)
        return 0;
    memcpy(buf + *pos, text, len);
    *pos += len;
    return 1;
}

/// @brief Append a NUL-terminated string to @p buf at offset @p *pos.
/// @details Convenience wrapper around @ref rt_fmt_append_bytes that computes
///          the length via `strlen`.
/// @param buf   Destination buffer.
/// @param cap   Capacity of @p buf in bytes.
/// @param pos   Current write offset; receives the new offset on success.
/// @param text  NUL-terminated source string.
/// @return 1 on success, 0 on overflow / invalid input.
static int rt_fmt_append_cstr(char *buf, size_t cap, size_t *pos, const char *text) {
    return rt_fmt_append_bytes(buf, cap, pos, text, strlen(text));
}

/// @brief Format a signed 64-bit integer in decimal.
/// @details Uses `snprintf` and returns an empty string on formatting failure
///          or truncation. The result owns its bytes and must be released by
///          the caller.
/// @param value Integer value to format.
/// @return Newly allocated runtime string with decimal text.
rt_string rt_fmt_int(int64_t value) {
    char buffer[FMT_BUFFER_SIZE];
    int len = snprintf(buffer, sizeof(buffer), "%" PRId64, value);
    if (len < 0 || (size_t)len >= sizeof(buffer))
        return rt_fmt_empty();
    return rt_string_from_bytes(buffer, (size_t)len);
}

/// @brief Format a signed integer using a specified radix (base 2-36).
/// @details Radix values outside 2-36 return an empty string. For radix 10,
///          negative values are emitted with a leading '-' and INT64_MIN is
///          handled safely. For non-decimal radices, the value is treated
///          as unsigned so the bit pattern is preserved.
/// @param value Integer to format.
/// @param radix Base to use (2-36).
/// @return Newly allocated string containing the formatted number.
rt_string rt_fmt_int_radix(int64_t value, int64_t radix) {
    // Validate radix
    if (radix < 2 || radix > 36)
        return rt_fmt_empty();

    // Handle zero specially
    if (value == 0)
        return rt_string_from_bytes("0", 1);

    // Handle negative numbers for non-decimal bases
    bool negative = false;
    uint64_t uval;
    if (value < 0 && radix == 10) {
        negative = true;
        uval = (uint64_t)(-(value + 1)) + 1; // Handle INT64_MIN
    } else {
        // For non-decimal bases, treat as unsigned
        uval = (uint64_t)value;
    }

    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char buffer[FMT_BUFFER_SIZE];
    char *p = buffer + sizeof(buffer) - 1;
    *p = '\0';

    while (uval > 0) {
        --p;
        *p = digits[uval % (uint64_t)radix];
        uval /= (uint64_t)radix;
    }

    if (negative) {
        --p;
        *p = '-';
    }

    size_t len = (size_t)(buffer + sizeof(buffer) - 1 - p);
    return rt_string_from_bytes(p, len);
}

/// @brief Format an integer with a minimum width and pad character.
/// @details The output is left-padded to @p width using the first valid UTF-8
///          code point of @p pad_char (defaulting to space if empty or invalid). When padding with
///          '0' and the value is negative, the sign is emitted before the
///          zeros to match typical numeric formatting conventions. Very large
///          widths that cannot be allocated return an empty string.
/// @param value Integer to format.
/// @param width Minimum output width in characters.
/// @param pad_char Runtime string containing the pad character.
/// @return Newly allocated string containing the padded number.
rt_string rt_fmt_int_pad(int64_t value, int64_t width, rt_string pad_char) {
    const char *pad_data = " ";
    size_t pad_len = 1;
    if (pad_char && rt_string_is_handle((const void *)pad_char) && pad_char->data) {
        size_t candidate_len = (size_t)rt_str_len(pad_char);
        size_t first_len = rt_fmt_first_utf8_len(pad_char->data, candidate_len);
        if (first_len > 0) {
            pad_data = pad_char->data;
            pad_len = first_len;
        }
    }

    // Format the number
    char num_buf[FMT_BUFFER_SIZE];
    int num_len = snprintf(num_buf, sizeof(num_buf), "%" PRId64, value);
    if (num_len < 0 || (size_t)num_len >= sizeof(num_buf))
        return rt_fmt_empty();

    // If width <= number length, return as-is
    if (width <= 0 || width <= num_len)
        return rt_string_from_bytes(num_buf, (size_t)num_len);

    if ((uint64_t)width > SIZE_MAX)
        return rt_fmt_empty();

    size_t pad_count = (size_t)width - (size_t)num_len;
    size_t pad_bytes = 0;
    if (pad_count > SIZE_MAX / pad_len)
        return rt_fmt_empty();
    pad_bytes = pad_count * pad_len;

    size_t out_len = 0;
    if (!rt_fmt_checked_add((size_t)num_len, pad_bytes, &out_len) || out_len == SIZE_MAX)
        return rt_fmt_empty();

    char *buffer = (char *)malloc(out_len + 1);
    if (!buffer)
        return rt_fmt_empty();

    // Handle negative numbers: put sign before padding if pad is '0'
    size_t pos = 0;
    if (value < 0 && pad_len == 1 && pad_data[0] == '0') {
        buffer[pos++] = '-';
        // Skip the minus sign in num_buf
        for (size_t i = 0; i < pad_count; ++i)
            buffer[pos++] = pad_data[0];
        memcpy(buffer + pos, num_buf + 1, (size_t)num_len - 1);
        pos += (size_t)num_len - 1;
    } else {
        for (size_t i = 0; i < pad_count; ++i) {
            memcpy(buffer + pos, pad_data, pad_len);
            pos += pad_len;
        }
        memcpy(buffer + pos, num_buf, (size_t)num_len);
        pos += (size_t)num_len;
    }
    buffer[pos] = '\0';

    rt_string result = rt_string_from_bytes(buffer, pos);
    free(buffer);
    return result;
}

/// @brief Format a floating-point number with default precision.
/// @details Uses `%.15g` — 15 significant digits, trailing zeros stripped.
///          Matches the historical Zanna / BASIC golden format; not a strict
///          IEEE-754 round-trip (values like 1.0/3.0 lose precision). NaN and
///          infinity map to "NaN" / "Infinity" / "-Infinity".
/// @param value Floating-point value to format.
/// @return Newly allocated runtime string for the formatted number.
rt_string rt_fmt_num(double value) {
    // Handle special cases
    if (isnan(value))
        return rt_string_from_bytes("NaN", 3);
    if (isinf(value))
        return value > 0 ? rt_string_from_bytes("Infinity", 8)
                         : rt_string_from_bytes("-Infinity", 9);
    if (value == 0.0)
        value = 0.0;

    char buffer[FMT_BUFFER_SIZE];
    // %.15g is the historical Zanna default — 15 significant digits matches
    // Python repr()-style output, strips trailing zeros via %g, and produces
    // the text that golden tests (including basic_random_repro and the
    // comprehensive_control_flow_strings suite) were recorded against.
    int len = rt_fmt_snprintf_c_locale(buffer, sizeof(buffer), "%.15g", value);
    if (len < 0 || (size_t)len >= sizeof(buffer))
        return rt_fmt_empty();
    return rt_string_from_bytes(buffer, (size_t)len);
}

/// @brief Format a floating-point number with fixed decimal places.
/// @details Decimal places are clamped to [0, 20]. The fixed buffer is sized
///          for the largest finite double at the maximum precision. NaN and
///          infinity are mapped to their textual forms.
/// @param value Floating-point value to format.
/// @param decimals Requested number of fractional digits.
/// @return Newly allocated runtime string for the formatted number.
rt_string rt_fmt_num_fixed(double value, int64_t decimals) {
    // Handle special cases
    if (isnan(value))
        return rt_string_from_bytes("NaN", 3);
    if (isinf(value))
        return value > 0 ? rt_string_from_bytes("Infinity", 8)
                         : rt_string_from_bytes("-Infinity", 9);

    // Clamp decimals
    if (decimals < 0)
        decimals = 0;
    if (decimals > 20)
        decimals = 20;
    if (value == 0.0)
        value = 0.0;

    char buffer[FMT_FIXED_BUFFER_SIZE];
    int len = rt_fmt_snprintf_c_locale(buffer, sizeof(buffer), "%.*f", (int)decimals, value);
    if (len < 0 || (size_t)len >= sizeof(buffer))
        return rt_fmt_empty();
    if (buffer[0] == '-' && rt_fmt_decimal_string_is_zero(buffer, (size_t)len)) {
        memmove(buffer, buffer + 1, (size_t)len);
        --len;
    }
    len = (int)strlen(buffer);
    return rt_string_from_bytes(buffer, (size_t)len);
}

/// @brief Format a floating-point number in scientific notation.
/// @details Decimal places are clamped to [0, 20]. NaN and infinity are
///          mapped to their textual forms. Uses `%e` for exponent output.
/// @param value Floating-point value to format.
/// @param decimals Requested number of fractional digits.
/// @return Newly allocated runtime string in scientific notation.
rt_string rt_fmt_num_sci(double value, int64_t decimals) {
    // Handle special cases
    if (isnan(value))
        return rt_string_from_bytes("NaN", 3);
    if (isinf(value))
        return value > 0 ? rt_string_from_bytes("Infinity", 8)
                         : rt_string_from_bytes("-Infinity", 9);

    // Clamp decimals
    if (decimals < 0)
        decimals = 0;
    if (decimals > 20)
        decimals = 20;
    if (value == 0.0)
        value = 0.0;

    char buffer[FMT_BUFFER_SIZE];
    int len = rt_fmt_snprintf_c_locale(buffer, sizeof(buffer), "%.*e", (int)decimals, value);
    if (len < 0 || (size_t)len >= sizeof(buffer))
        return rt_fmt_empty();
    if (buffer[0] == '-' && rt_fmt_decimal_string_is_zero(buffer, (size_t)len)) {
        memmove(buffer, buffer + 1, (size_t)len);
        --len;
    }
    len = (int)strlen(buffer);
    return rt_string_from_bytes(buffer, (size_t)len);
}

/// @brief Format a floating-point number as a percentage.
/// @details Multiplies the input by 100, formats with fixed decimals, and
///          appends a '%' suffix. NaN and infinity are mapped to textual
///          forms that include the suffix.
/// @param value Input value where 1.0 means 100%.
/// @param decimals Number of fractional digits to emit.
/// @return Newly allocated runtime string with percent formatting.
rt_string rt_fmt_num_pct(double value, int64_t decimals) {
    // Handle special cases
    if (isnan(value))
        return rt_string_from_bytes("NaN%", 4);
    if (isinf(value))
        return value > 0 ? rt_string_from_bytes("Infinity%", 9)
                         : rt_string_from_bytes("-Infinity%", 10);

    // Clamp decimals
    if (decimals < 0)
        decimals = 0;
    if (decimals > 20)
        decimals = 20;

    double pct = value * 100.0;
    if (isnan(pct))
        return rt_string_from_bytes("NaN%", 4);
    if (isinf(pct))
        return pct > 0 ? rt_string_from_bytes("Infinity%", 9)
                       : rt_string_from_bytes("-Infinity%", 10);
    if (pct == 0.0)
        pct = 0.0;

    char buffer[FMT_BUFFER_SIZE];
    int len = rt_fmt_snprintf_c_locale(buffer, sizeof(buffer), "%.*f%%", (int)decimals, pct);
    if (len < 0 || (size_t)len >= sizeof(buffer))
        return rt_fmt_empty();
    if (buffer[0] == '-' && rt_fmt_decimal_string_is_zero(buffer, (size_t)len)) {
        memmove(buffer, buffer + 1, (size_t)len);
        --len;
    }
    len = (int)strlen(buffer);
    return rt_string_from_bytes(buffer, (size_t)len);
}

/// @brief Format a boolean as lowercase "true"/"false".
/// @details This mirrors the canonical Zanna boolean spelling used in
///          diagnostics and string conversions.
/// @param value Boolean value to format.
/// @return Newly allocated runtime string with "true" or "false".
rt_string rt_fmt_bool(bool value) {
    return value ? rt_string_from_bytes("true", 4) : rt_string_from_bytes("false", 5);
}

/// @brief Format a boolean as lowercase "yes"/"no".
/// @details Lowercase per the BUG-015 regression contract — the prompted-for
///          output matches the literal tokens users type back in "yes/no"
///          prompt flows. Callers wanting title-case can substitute
///          `value ? "Yes" : "No"` at the call site.
/// @param value Boolean value to format.
/// @return Newly allocated runtime string with "yes" or "no".
rt_string rt_fmt_bool_yn(bool value) {
    return value ? rt_string_from_bytes("yes", 3) : rt_string_from_bytes("no", 2);
}

/// @brief Format a byte count into a human-readable size string.
/// @details Converts the absolute byte count into units of 1024, selecting
///          the largest unit where the magnitude remains >= 1. For bytes,
///          the output is an integer (e.g., "512 B"); for larger units, one
///          decimal place is emitted (e.g., "1.5 MB"). Negative sizes keep
///          a leading '-' sign.
/// @param bytes Byte count to format.
/// @return Newly allocated runtime string with size and unit suffix.
rt_string rt_fmt_size(int64_t bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    static const int num_units = sizeof(units) / sizeof(units[0]);

    // Handle negative sizes
    bool negative = bytes < 0;
    uint64_t ubytes;
    if (bytes < 0)
        ubytes = (uint64_t)(-(bytes + 1)) + 1; // Handle INT64_MIN
    else
        ubytes = (uint64_t)bytes;

    int unit_idx = 0;
    uint64_t divisor = 1;

    while (unit_idx < num_units - 1 && ubytes / divisor >= 1024ULL) {
        divisor *= 1024ULL;
        ++unit_idx;
    }
    double size = (double)ubytes / (double)divisor;
    if (unit_idx > 0 && unit_idx < num_units - 1 && size >= 1023.95) {
        divisor *= 1024ULL;
        ++unit_idx;
        size = (double)ubytes / (double)divisor;
    }

    char buffer[FMT_BUFFER_SIZE];
    int len;

    if (unit_idx == 0) {
        // Bytes - show as integer
        len = snprintf(buffer,
                       sizeof(buffer),
                       "%s%" PRIu64 " %s",
                       negative ? "-" : "",
                       ubytes,
                       units[unit_idx]);
    } else {
        // Units >= KB always show one decimal digit (e.g., 1.0 KB).
        len = rt_fmt_snprintf_c_locale(
            buffer, sizeof(buffer), "%s%.1f %s", negative ? "-" : "", size, units[unit_idx]);
    }

    if (len < 0 || (size_t)len >= sizeof(buffer))
        return rt_fmt_empty();
    len = (int)strlen(buffer);
    return rt_string_from_bytes(buffer, (size_t)len);
}

/// @brief Format an integer as lowercase hexadecimal.
/// @details Treats the input as unsigned so the full 64-bit pattern is
///          preserved, then formats without a "0x" prefix.
/// @param value Integer value to format.
/// @return Newly allocated runtime string with hex digits.
rt_string rt_fmt_hex(int64_t value) {
    char buffer[FMT_BUFFER_SIZE];
    int len = snprintf(buffer, sizeof(buffer), "%" PRIx64, (uint64_t)value);
    if (len < 0 || (size_t)len >= sizeof(buffer))
        return rt_fmt_empty();
    return rt_string_from_bytes(buffer, (size_t)len);
}

/// @brief Format an integer as zero-padded hexadecimal.
/// @details The width is clamped to [1, 16] to match 64-bit output size.
///          Padding uses '0' and no prefix is emitted.
/// @param value Integer value to format.
/// @param width Minimum width in hex digits.
/// @return Newly allocated runtime string with padded hex digits.
rt_string rt_fmt_hex_pad(int64_t value, int64_t width) {
    if (width <= 0)
        width = 1;
    if (width > 16)
        width = 16;

    char buffer[FMT_BUFFER_SIZE];
    int len = snprintf(buffer, sizeof(buffer), "%0*" PRIx64, (int)width, (uint64_t)value);
    if (len < 0 || (size_t)len >= sizeof(buffer))
        return rt_fmt_empty();
    return rt_string_from_bytes(buffer, (size_t)len);
}

/// @brief Format an integer as a binary string.
/// @details Treats the input as unsigned and emits a minimal-length binary
///          representation without any prefix. Zero is returned as "0".
/// @param value Integer value to format.
/// @return Newly allocated runtime string containing binary digits.
rt_string rt_fmt_bin(int64_t value) {
    // Handle zero specially
    if (value == 0)
        return rt_string_from_bytes("0", 1);

    char buffer[FMT_BIN_BUFFER_SIZE];
    char *p = buffer + sizeof(buffer) - 1;
    *p = '\0';

    uint64_t uval = (uint64_t)value;
    while (uval > 0) {
        --p;
        *p = (uval & 1) ? '1' : '0';
        uval >>= 1;
    }

    size_t len = (size_t)(buffer + sizeof(buffer) - 1 - p);
    return rt_string_from_bytes(p, len);
}

/// @brief Format an integer as lowercase octal.
/// @details Treats the input as unsigned and emits an octal string without
///          any prefix.
/// @param value Integer value to format.
/// @return Newly allocated runtime string containing octal digits.
rt_string rt_fmt_oct(int64_t value) {
    char buffer[FMT_BUFFER_SIZE];
    int len = snprintf(buffer, sizeof(buffer), "%" PRIo64, (uint64_t)value);
    if (len < 0 || (size_t)len >= sizeof(buffer))
        return rt_fmt_empty();
    return rt_string_from_bytes(buffer, (size_t)len);
}

// -----------------------------------------------------------------------
// Thousands separator, currency, words, ordinal
// -----------------------------------------------------------------------

/// @brief Format an integer with thousands grouping (e.g., "1,234,567").
/// @details Converts the integer to its unsigned decimal representation, then
///          inserts the separator string every three digits from the right.
///          INT64_MIN is handled safely via the `-(value+1)+1` trick. A heap
///          buffer is used because the separator may be multi-byte (e.g., a
///          thin space), making the output longer than the fixed-size stack buf.
/// @param value Integer to format.
/// @param sep Separator string; defaults to "," if NULL or empty.
/// @return Newly allocated string with grouped digits, or empty on OOM.
rt_string rt_fmt_int_grouped(int64_t value, rt_string sep) {
    size_t sep_len = 0;
    const char *sep_str = rt_fmt_string_bytes(sep, ",", &sep_len);

    // Format the number without grouping first
    char raw[FMT_BUFFER_SIZE];
    int rlen;
    int negative = 0;
    uint64_t uval;
    if (value < 0) {
        negative = 1;
        uval = (uint64_t)(-(value + 1)) + 1ULL;
    } else {
        uval = (uint64_t)value;
    }
    rlen = snprintf(raw, sizeof(raw), "%" PRIu64, uval);
    if (rlen < 0 || (size_t)rlen >= sizeof(raw))
        return rt_fmt_empty();

    size_t out_len = 0;
    char *buf = rt_fmt_group_digits_alloc(raw, (size_t)rlen, sep_str, sep_len, negative, &out_len);
    if (!buf)
        return rt_fmt_empty();

    rt_string result = rt_string_from_bytes(buf, out_len);
    free(buf);
    return result;
}

/// @brief Format a floating-point number as currency with symbol and grouping.
/// @details Splits the value into integer and fractional parts, groups the
///          integer with commas via rt_fmt_int_grouped, and appends the
///          fractional portion formatted to the requested decimal precision.
///          The currency symbol (e.g., "$", "EUR") is prepended after the
///          optional negative sign, matching common accounting conventions.
///          Decimal places are clamped to [0, 20].
/// @param value Amount to format (negative values get a leading '-').
/// @param decimals Number of fractional digits (typically 2 for currency).
/// @param symbol Currency symbol string; defaults to "$" if NULL.
/// @return Newly allocated string like "-$1,234.56", or empty on OOM.
rt_string rt_fmt_currency(double value, int64_t decimals, rt_string symbol) {
    if (decimals < 0)
        decimals = 0;
    if (decimals > 20)
        decimals = 20;

    if (isnan(value))
        return rt_string_from_bytes("NaN", 3);
    if (isinf(value))
        return value > 0 ? rt_string_from_bytes("Infinity", 8)
                         : rt_string_from_bytes("-Infinity", 9);

    size_t sym_len = 0;
    const char *sym = rt_fmt_string_bytes_allow_empty(symbol, "$", &sym_len);

    double abs_val = fabs(value);

    char fixed[384];
    int fixed_len = rt_fmt_snprintf_c_locale(fixed, sizeof(fixed), "%.*f", (int)decimals, abs_val);
    if (fixed_len < 0 || (size_t)fixed_len >= sizeof(fixed))
        return rt_fmt_empty();
    fixed_len = (int)strlen(fixed);
    int negative = signbit(value) && !rt_fmt_decimal_string_is_zero(fixed, (size_t)fixed_len);

    const char *dot = memchr(fixed, '.', (size_t)fixed_len);
    size_t integer_len = dot ? (size_t)(dot - fixed) : (size_t)fixed_len;
    size_t frac_len = dot ? (size_t)fixed_len - integer_len : 0;

    size_t grp_len = 0;
    char *grouped = rt_fmt_group_digits_alloc(fixed, integer_len, ",", 1, 0, &grp_len);
    if (!grouped)
        return rt_fmt_empty();

    // Build final: [-]symbol + grouped + decimals
    size_t total = negative ? 1 : 0;
    if (!rt_fmt_checked_add(total, sym_len, &total) ||
        !rt_fmt_checked_add(total, grp_len, &total) ||
        !rt_fmt_checked_add(total, frac_len, &total)) {
        free(grouped);
        return rt_fmt_empty();
    }
    if (total == SIZE_MAX) {
        free(grouped);
        return rt_fmt_empty();
    }

    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        free(grouped);
        return rt_fmt_empty();
    }

    char *dst = buf;
    if (negative)
        *dst++ = '-';
    memcpy(dst, sym, sym_len);
    dst += sym_len;
    memcpy(dst, grouped, grp_len);
    dst += grp_len;
    if (frac_len > 0) {
        memcpy(dst, dot, frac_len);
        dst += frac_len;
    }
    *dst = '\0';

    rt_string result = rt_string_from_bytes(buf, (size_t)(dst - buf));
    free(buf);
    free(grouped);
    return result;
}

/// @brief English words for values from zero through nineteen.
/// @details Index zero is empty because callers emit it only as a placeholder
///   while composing a nonzero sub-thousand chunk.
static const char *ones[] = {"",        "one",     "two",       "three",    "four",
                             "five",    "six",     "seven",     "eight",    "nine",
                             "ten",     "eleven",  "twelve",    "thirteen", "fourteen",
                             "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
/// @brief English tens words indexed by the tens digit.
/// @details Indices zero and one are empty because units and teens use
///   @ref ones instead.
static const char *tens_words[] = {
    "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};

/// @brief Format the sub-thousand portion of a number into English words.
/// @details Handles values 0-999 by breaking into hundreds, tens, and ones.
///          Teens (11-19) are handled as a special case because English has
///          unique words for them rather than composing from tens+ones.
/// @param buf Output buffer to write into.
/// @param cap Remaining capacity of buf.
/// @param n Value in range [0, 999].
/// @return Number of characters written (not including NUL terminator).
static int words_chunk(char *buf, size_t cap, size_t *pos, uint64_t n) {
    if (n == 0)
        return 1;
    if (n >= 100) {
        int h = (int)(n / 100);
        if (!rt_fmt_append_cstr(buf, cap, pos, ones[h]) ||
            !rt_fmt_append_cstr(buf, cap, pos, " hundred"))
            return 0;
        n %= 100;
        if (n > 0 && !rt_fmt_append_cstr(buf, cap, pos, " "))
            return 0;
    }
    if (n >= 20) {
        int t = (int)(n / 10);
        if (!rt_fmt_append_cstr(buf, cap, pos, tens_words[t]))
            return 0;
        n %= 10;
        if (n > 0 && (!rt_fmt_append_cstr(buf, cap, pos, "-") ||
                      !rt_fmt_append_cstr(buf, cap, pos, ones[n])))
            return 0;
    } else if (n > 0) {
        if (!rt_fmt_append_cstr(buf, cap, pos, ones[n]))
            return 0;
    }
    return 1;
}

/// @brief Convert an integer to its English word representation.
/// @details Decomposes the number into groups of three digits (ones, thousands,
///          millions, billions, trillions, quadrillions, and quintillions) and
///          converts each group to words via words_chunk. Groups are joined with
///          scale labels. Supports the full signed 64-bit integer range.
///          Negative numbers are prefixed with "negative ".
///          Zero is special-cased to return "zero" directly.
/// @param value Integer to convert.
/// @return Newly allocated string such as "one hundred twenty-three thousand".
rt_string rt_fmt_to_words(int64_t value) {
    if (value == 0)
        return rt_string_from_bytes("zero", 4);

    char buf[1024];
    size_t pos = 0;
    int negative = 0;
    uint64_t uvalue;

    if (value < 0) {
        negative = 1;
        uvalue = 0 - (uint64_t)value;
    } else {
        uvalue = (uint64_t)value;
    }

    if (negative && !rt_fmt_append_cstr(buf, sizeof(buf), &pos, "negative "))
        return rt_fmt_empty();

    static const char *scale[] = {
        "", " thousand", " million", " billion", " trillion", " quadrillion", " quintillion"};
    uint64_t parts[sizeof(scale) / sizeof(scale[0])] = {0};
    int part_count = 0;

    uint64_t temp = uvalue;
    while (temp > 0 && part_count < (int)(sizeof(parts) / sizeof(parts[0]))) {
        parts[part_count++] = temp % 1000;
        temp /= 1000;
    }
    if (temp > 0)
        return rt_fmt_empty();

    int first = 1;
    for (int i = part_count - 1; i >= 0; --i) {
        if (parts[i] == 0)
            continue;
        if (!first && !rt_fmt_append_cstr(buf, sizeof(buf), &pos, " "))
            return rt_fmt_empty();
        if (!words_chunk(buf, sizeof(buf), &pos, parts[i]) ||
            !rt_fmt_append_cstr(buf, sizeof(buf), &pos, scale[i]))
            return rt_fmt_empty();
        first = 0;
    }

    return rt_string_from_bytes(buf, pos);
}

/// @brief Convert an integer to its ordinal string (e.g., "1st", "2nd", "3rd").
/// @details Appends the English ordinal suffix to the decimal representation.
///          The suffix selection follows the English exception rules: 11th, 12th,
///          and 13th use "th" despite ending in 1/2/3, because the teens always
///          use "th". All other numbers ending in 1 get "st", 2 gets "nd", 3 gets
///          "rd", and everything else gets "th".
/// @param value Integer to format as ordinal.
/// @return Newly allocated string like "42nd" or "111th".
rt_string rt_fmt_ordinal(int64_t value) {
    char buf[FMT_BUFFER_SIZE];
    const char *suffix;
    uint64_t abs_val = value < 0 ? 0 - (uint64_t)value : (uint64_t)value;
    uint64_t last_two = abs_val % 100;
    uint64_t last_one = abs_val % 10;

    if (last_two >= 11 && last_two <= 13)
        suffix = "th";
    else if (last_one == 1)
        suffix = "st";
    else if (last_one == 2)
        suffix = "nd";
    else if (last_one == 3)
        suffix = "rd";
    else
        suffix = "th";

    int len = snprintf(buf, sizeof(buf), "%" PRId64 "%s", value, suffix);
    if (len < 0 || (size_t)len >= sizeof(buf))
        return rt_fmt_empty();
    return rt_string_from_bytes(buf, (size_t)len);
}

#ifdef __cplusplus
}
#endif
