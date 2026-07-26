//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_numfmt.c
// Purpose: Implements the invariant number-formatting operations exposed by
//          Zanna.Text.InvariantNumberFormat: fixed-decimal, grouped-integer,
//          currency, percentage, ordinal, English-word, byte-size, and
//          zero-padded representations.
//
// Key invariants:
//   - Fixed-decimal, currency, and percentage output uses a period as the
//     decimal separator; currency grouping uses commas.
//   - Thousands grouping accepts an arbitrary byte string and defaults to a
//     comma when the supplied separator is null or empty.
//   - Percentage formatting multiplies by 100 and appends '%'.
//   - Ordinal and word formatting follow English conventions.
//   - Signed 64-bit magnitudes, including INT64_MIN, are handled without
//     overflowing a signed negation.
//
// Ownership/Lifetime:
//   - All returned rt_string values are fresh allocations owned by the caller.
//   - No state is retained between calls.
//
// Links: src/runtime/text/rt_numfmt.h (public API),
//        src/runtime/text/rt_numfmt_internal.h (shared grouping helper),
//        src/runtime/core/rt_string_builder.h (formatted-output accumulation)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_numfmt.c
 * @brief Implements invariant numeric display formatting.
 * @details Stateless routines format fixed decimals, grouped integers,
 *          currency, percentages, English ordinals and words, byte sizes, and
 *          zero-padded signed integers. Locale-independent conversion and
 *          unsigned-magnitude helpers preserve the complete int64_t range.
 */

#include "rt_numfmt.h"
#include "rt_format.h"
#include "rt_internal.h"
#include "rt_numfmt_internal.h"
#include "rt_string_builder.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Convert a string-builder failure into a runtime trap.
/// @details Successful statuses return normally. Any other status is treated
///          as an unrecoverable formatting failure and traps with @p op.
/// @param status Status returned by a string-builder operation.
/// @param op Null-terminated trap message identifying the failed operation.
static void numfmt_check_sb(rt_sb_status_t status, const char *op) {
    if (status != RT_SB_OK)
        rt_trap(op);
}

/// @brief Compute the unsigned magnitude of a signed 64-bit integer.
/// @details The two-step negative conversion avoids evaluating `-INT64_MIN`
///          in signed arithmetic.
/// @param n Value whose magnitude is required.
/// @return Absolute magnitude of @p n represented as a `uint64_t`.
static uint64_t abs_i64_magnitude(int64_t n) {
    if (n >= 0)
        return (uint64_t)n;
    return (uint64_t)(-(n + 1)) + 1;
}
/// @brief Render a finite floating-point value with a fixed fractional width.
/// @details Formatting uses the runtime's C-locale helper so the decimal point
///          is independent of the embedding process's `LC_NUMERIC` setting.
///          The caller chooses the precision and handles non-finite values.
/// @param value Finite value to format.
/// @param decimals Number of digits to emit after the decimal point.
/// @return Newly allocated formatted string, or a newly allocated empty string
///         if conversion fails or exceeds the fixed conversion buffer.
static rt_string numfmt_fixed(double value, int decimals) {
    // C-locale conversion: InvariantNumberFormat output must not inherit the
    // embedding process's LC_NUMERIC decimal separator (VDOC-041). A fixed
    // buffer covers the worst case (%f of ~1e308 plus 20 decimals ≈ 335 bytes).
    char buf[512];
    int written = rt_format_snprintf_c_locale(buf, sizeof(buf), "%.*f", decimals, value);
    if (written < 0 || (size_t)written >= sizeof(buf))
        return rt_string_from_bytes("", 0);
    return rt_string_from_bytes(buf, (size_t)written);
}

/// @brief Render a finite percentage value and append a percent sign.
/// @details A zero @p decimals value emits no fractional digits; every nonzero
///          value emits exactly one. Formatting uses the runtime's C-locale
///          conversion helper.
/// @param value Already-scaled finite percentage value.
/// @param decimals Zero for integer output, or nonzero for one fractional digit.
/// @return Newly allocated percentage string, or a newly allocated empty string
///         if conversion fails or exceeds the fixed conversion buffer.
static rt_string numfmt_percent_fixed(double value, int decimals) {
    char buf[512];
    int written = decimals == 0
                      ? rt_format_snprintf_c_locale(buf, sizeof(buf), "%.0f%%", value)
                      : rt_format_snprintf_c_locale(buf, sizeof(buf), "%.1f%%", value);
    if (written < 0 || (size_t)written >= sizeof(buf))
        return rt_string_from_bytes("", 0);
    return rt_string_from_bytes(buf, (size_t)written);
}

/// @brief Format a non-finite floating-point value using stable English tokens.
/// @details NaN becomes `"NaN"` and infinities become `"Infinity"` or
///          `"-Infinity"`. A nonempty @p suffix is appended verbatim.
/// @param value Value to inspect.
/// @param suffix Optional null-terminated suffix, such as `"%"`.
/// @return A newly allocated special-value string when @p value is non-finite,
///         or `NULL` when it is finite.
static rt_string numfmt_nonfinite(double value, const char *suffix) {
    const char *base = NULL;
    if (isnan(value))
        base = "NaN";
    else if (isinf(value))
        base = value < 0.0 ? "-Infinity" : "Infinity";
    else
        return NULL;

    if (!suffix || suffix[0] == '\0')
        return rt_string_from_bytes(base, strlen(base));

    rt_string_builder sb;
    rt_sb_init(&sb);
    numfmt_check_sb(rt_sb_append_cstr(&sb, base), "NumberFormat: formatting failed");
    numfmt_check_sb(rt_sb_append_cstr(&sb, suffix), "NumberFormat: formatting failed");
    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;
}

// ---------------------------------------------------------------------------
// rt_numfmt_decimals
// ---------------------------------------------------------------------------

/// @brief Format `n` with exactly `decimals` digits after the decimal point.
/// @details Clamps @p decimals to `[0, 20]`, emits a C-locale decimal point,
///          and delegates rounding to the platform's `printf` implementation.
///          Non-finite inputs produce `"NaN"`, `"Infinity"`, or
///          `"-Infinity"` without a fractional suffix.
/// @param n Floating-point value to format.
/// @param decimals Requested number of fractional digits.
/// @return Newly allocated fixed-decimal string owned by the caller.
rt_string rt_numfmt_decimals(double n, int64_t decimals) {
    if (decimals < 0)
        decimals = 0;
    if (decimals > 20)
        decimals = 20;

    rt_string special = numfmt_nonfinite(n, NULL);
    if (special)
        return special;

    return numfmt_fixed(n, (int)decimals);
}

// ---------------------------------------------------------------------------
// rt_numfmt_thousands
// ---------------------------------------------------------------------------

/// @brief Format an integer with `sep` inserted every three digits from the right.
/// @details @p sep defaults to `","` when it is null, has no backing C
///          string, or is empty. Otherwise all separator bytes are preserved,
///          including embedded null bytes. The sign is emitted before the
///          grouped magnitude, and `INT64_MIN` is supported.
/// @param n Integer to format.
/// @param sep Runtime string inserted between three-digit groups.
/// @return Newly allocated grouped string owned by the caller.
rt_string rt_numfmt_thousands(int64_t n, rt_string sep) {
    const char *sep_bytes = ",";
    size_t sep_len = 1;
    if (sep) {
        const char *candidate = rt_string_cstr(sep);
        int64_t candidate_len = rt_str_len(sep);
        if (candidate && candidate_len > 0) {
            sep_bytes = candidate;
            sep_len = (size_t)candidate_len;
        }
    }

    int negative = n < 0;
    // Handle INT64_MIN
    uint64_t abs_n;
    if (n == INT64_MIN)
        abs_n = (uint64_t)INT64_MAX + 1;
    else
        abs_n = (uint64_t)(negative ? -n : n);

    // Format the absolute number first
    char digits[32];
    int dlen = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)abs_n);
    if (dlen < 0)
        dlen = 0;

    rt_string_builder sb;
    rt_sb_init(&sb);

    if (negative)
        numfmt_check_sb(rt_sb_append_cstr(&sb, "-"), "NumberFormat.Thousands: formatting failed");

    rt_numfmt_group_digits(&sb, digits, dlen, sep_bytes, sep_len, /*group_size=*/3);

    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;
}

// ---------------------------------------------------------------------------
// Shared helper: group digits with a caller-selected separator (see
// rt_numfmt_internal.h for the public internal contract).
// ---------------------------------------------------------------------------
/// @brief Append decimal digits grouped from the right.
/// @details When grouping is disabled by a null or empty separator, a
///          nonpositive group size, or a group size at least as large as the
///          input, the digit bytes are appended unchanged. String-builder
///          failures trap through the runtime.
/// @param sb Destination string builder.
/// @param digits Decimal digit buffer to append.
/// @param dlen Number of bytes in @p digits.
/// @param sep Separator byte buffer, or `NULL` to disable grouping.
/// @param sep_len Number of bytes in @p sep.
/// @param group_size Number of digits per group, counted from the right.
void rt_numfmt_group_digits(rt_string_builder *sb,
                            const char *digits,
                            int dlen,
                            const char *sep,
                            size_t sep_len,
                            int group_size) {
    if (dlen <= 0)
        return;

    // Degenerate cases: no separator, zero-length separator, or group size
    // that would swallow the entire input (<=0, or >=dlen). Emit the digits
    // verbatim and skip the grouping machinery.
    if (!sep || sep_len == 0 || group_size <= 0 || group_size >= dlen) {
        numfmt_check_sb(rt_sb_append_bytes(sb, digits, (size_t)dlen),
                        "NumberFormat.Thousands: formatting failed");
        return;
    }

    int first_group = dlen % group_size;
    if (first_group == 0)
        first_group = group_size;

    numfmt_check_sb(rt_sb_append_bytes(sb, digits, (size_t)first_group),
                    "NumberFormat.Thousands: formatting failed");
    int pos = first_group;

    while (pos < dlen) {
        numfmt_check_sb(rt_sb_append_bytes(sb, sep, sep_len),
                        "NumberFormat.Thousands: formatting failed");
        numfmt_check_sb(rt_sb_append_bytes(sb, digits + pos, (size_t)group_size),
                        "NumberFormat.Thousands: formatting failed");
        pos += group_size;
    }
}

// ---------------------------------------------------------------------------
// rt_numfmt_currency
// ---------------------------------------------------------------------------

/// @brief Format `n` as a currency value: `[symbol]X,XXX.XX` (always 2 decimals).
/// @details A null symbol, or one without a backing C string, defaults to
///          `"$"`; an explicitly empty symbol is honored. Finite values use
///          the layout `[-][symbol][comma-grouped integer].[two digits]` with
///          C-locale rounding. Non-finite values omit the currency symbol and
///          produce only their stable English token.
/// @param n Monetary value to format.
/// @param symbol Runtime string to place immediately before the magnitude.
/// @return Newly allocated currency string owned by the caller.
rt_string rt_numfmt_currency(double n, rt_string symbol) {
    rt_string special = numfmt_nonfinite(n, NULL);
    if (special)
        return special;

    const char *sym = "$";
    size_t sym_len = 1;
    if (symbol) {
        sym = rt_string_cstr(symbol);
        sym_len = (size_t)rt_str_len(symbol);
    }
    if (!sym) {
        sym = "$";
        sym_len = 1;
    }

    int negative = n < 0;
    double abs_n = fabs(n);

    // Round to 2 decimal places
    rt_string amount_str = numfmt_fixed(abs_n, 2);
    const char *amount = rt_string_cstr(amount_str);
    int alen = (int)rt_str_len(amount_str);

    // Find the decimal point
    char *dot = (char *)memchr(amount, '.', (size_t)alen);
    int int_len = dot ? (int)(dot - amount) : alen;

    rt_string_builder sb;
    rt_sb_init(&sb);

    if (negative)
        numfmt_check_sb(rt_sb_append_cstr(&sb, "-"), "NumberFormat.Currency: formatting failed");
    numfmt_check_sb(rt_sb_append_bytes(&sb, sym, sym_len),
                    "NumberFormat.Currency: formatting failed");

    // Add thousands separators to integer part
    int first_group = int_len % 3;
    if (first_group == 0)
        first_group = 3;

    numfmt_check_sb(rt_sb_append_bytes(&sb, amount, (size_t)first_group),
                    "NumberFormat.Currency: formatting failed");
    int pos = first_group;

    while (pos < int_len) {
        numfmt_check_sb(rt_sb_append_cstr(&sb, ","), "NumberFormat.Currency: formatting failed");
        numfmt_check_sb(rt_sb_append_bytes(&sb, amount + pos, 3),
                        "NumberFormat.Currency: formatting failed");
        pos += 3;
    }

    // Add decimal part
    if (dot) {
        numfmt_check_sb(rt_sb_append_bytes(&sb, dot, (size_t)(alen - int_len)),
                        "NumberFormat.Currency: formatting failed");
    }

    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    rt_string_unref(amount_str);
    return result;
}

// ---------------------------------------------------------------------------
// rt_numfmt_percent
// ---------------------------------------------------------------------------

/// @brief Format `n` as a percentage (multiplies by 100, appends `%`).
/// @details The scaled value is rounded to one decimal place with `round`,
///          then a trailing `.0` is removed. Thus `0.5` becomes `"50%"`
///          and `0.123` becomes `"12.3%"`. Non-finite input, including
///          overflow while scaling, produces a stable English token followed
///          by `%`.
/// @param n Ratio to multiply by 100 and format.
/// @return Newly allocated percentage string owned by the caller.
rt_string rt_numfmt_percent(double n) {
    double pct = n * 100.0;

    rt_string special = numfmt_nonfinite(pct, "%");
    if (special)
        return special;

    // Use at most 1 decimal place, but omit trailing .0
    double rounded = round(pct * 10.0) / 10.0;
    if (!isfinite(rounded))
        return numfmt_nonfinite(rounded, "%");

    rt_string formatted = numfmt_percent_fixed(rounded, 1);
    const char *text = rt_string_cstr(formatted);
    int64_t len = rt_str_len(formatted);

    if (len >= 3 && text[len - 3] == '.' && text[len - 2] == '0' && text[len - 1] == '%') {
        rt_string_builder sb;
        rt_sb_init(&sb);
        numfmt_check_sb(rt_sb_append_bytes(&sb, text, (size_t)(len - 3)),
                        "NumberFormat.Percent: formatting failed");
        numfmt_check_sb(rt_sb_append_cstr(&sb, "%"), "NumberFormat.Percent: formatting failed");
        rt_string trimmed = rt_string_from_bytes(sb.data, sb.len);
        rt_sb_free(&sb);
        rt_string_unref(formatted);
        return trimmed;
    }

    return formatted;
}

// ---------------------------------------------------------------------------
// rt_numfmt_ordinal
// ---------------------------------------------------------------------------

/// @brief Append the English ordinal suffix to `n` (1 → "1st", 22 → "22nd", 113 → "113th").
/// @details Standard English rules:
///          - Numbers ending in 11/12/13 always take `th` (so "11th",
///            not "11st").
///          - Otherwise: 1 → `st`, 2 → `nd`, 3 → `rd`, anything else → `th`.
///          Sign is preserved on the number portion (so `-1 → "-1st"`).
/// @param n Integer to format, including any signed 64-bit value.
/// @return Newly allocated English ordinal string owned by the caller.
rt_string rt_numfmt_ordinal(int64_t n) {
    const char *suffix;
    uint64_t abs_n = abs_i64_magnitude(n);
    uint64_t mod100 = abs_n % 100;
    uint64_t mod10 = abs_n % 10;

    if (mod100 >= 11 && mod100 <= 13)
        suffix = "th";
    else if (mod10 == 1)
        suffix = "st";
    else if (mod10 == 2)
        suffix = "nd";
    else if (mod10 == 3)
        suffix = "rd";
    else
        suffix = "th";

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld%s", (long long)n, suffix);
    if (len < 0)
        len = 0;
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;
    return rt_string_from_bytes(buf, (size_t)len);
}

// ---------------------------------------------------------------------------
// rt_numfmt_to_words
// ---------------------------------------------------------------------------

/// English names for the integers from zero through nineteen.
static const char *const ones[] = {"",        "one",     "two",       "three",    "four",
                                   "five",    "six",     "seven",     "eight",    "nine",
                                   "ten",     "eleven",  "twelve",    "thirteen", "fourteen",
                                   "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};

/// English decade names indexed by the tens digit.
static const char *const tens[] = {
    "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};

/// @brief Append the English-words form of `n` (`0..999`) to the builder.
/// @details Implements the standard 100s/tens/ones decomposition:
///          - `>= 100` → "X hundred" + remainder.
///          - `>= 20`  → tens word + optional `-` + ones (e.g. "thirty-two").
///          - `< 20`   → direct lookup in the `ones[]` table (which
///            also covers the irregular teen forms like "fifteen").
///          `has_prev` is set to 1 on first emission so callers can
///          insert a space before subsequent chunks (used by
///          `rt_numfmt_to_words` to separate scale words).
/// @param sb Destination string builder.
/// @param n Chunk value in the inclusive range `[0, 999]`.
/// @param has_prev In/out flag indicating whether an earlier chunk was emitted.
static void append_chunk(rt_string_builder *sb, int64_t n, int *has_prev) {
    if (n == 0)
        return;
    if (n < 0 || n > 999) {
        rt_trap("NumberFormat.ToWords: internal chunk out of range");
        return;
    }

    if (*has_prev)
        numfmt_check_sb(rt_sb_append_cstr(sb, " "), "NumberFormat.ToWords: formatting failed");

    if (n >= 100) {
        numfmt_check_sb(rt_sb_append_cstr(sb, ones[n / 100]),
                        "NumberFormat.ToWords: formatting failed");
        numfmt_check_sb(rt_sb_append_cstr(sb, " hundred"),
                        "NumberFormat.ToWords: formatting failed");
        n %= 100;
        if (n > 0)
            numfmt_check_sb(rt_sb_append_cstr(sb, " "), "NumberFormat.ToWords: formatting failed");
    }

    if (n >= 20) {
        numfmt_check_sb(rt_sb_append_cstr(sb, tens[n / 10]),
                        "NumberFormat.ToWords: formatting failed");
        int ones_idx = (int)(n % 10);
        if (ones_idx > 0) {
            numfmt_check_sb(rt_sb_append_cstr(sb, "-"), "NumberFormat.ToWords: formatting failed");
            numfmt_check_sb(rt_sb_append_cstr(sb, ones[ones_idx]),
                            "NumberFormat.ToWords: formatting failed");
        }
    } else if (n > 0 && n < 20) {
        numfmt_check_sb(rt_sb_append_cstr(sb, ones[n]), "NumberFormat.ToWords: formatting failed");
    }

    *has_prev = 1;
}

/// @brief Convert a signed integer to US English words.
/// @details Handles the complete `int64_t` range by decomposing the unsigned
///          magnitude into three-digit groups through quintillions. Negative
///          values begin with `"negative"`. Hundreds omit `"and"`, while
///          compound tens use a hyphen.
/// @param n Integer to convert.
/// @return Newly allocated English word string owned by the caller.
rt_string rt_numfmt_to_words(int64_t n) {
    if (n == 0)
        return rt_string_from_bytes("zero", 4);

    rt_string_builder sb;
    rt_sb_init(&sb);

    int negative = n < 0;
    uint64_t abs_n;
    if (n == INT64_MIN)
        abs_n = (uint64_t)INT64_MAX + 1;
    else
        abs_n = (uint64_t)(negative ? -n : n);

    if (negative)
        numfmt_check_sb(rt_sb_append_cstr(&sb, "negative "),
                        "NumberFormat.ToWords: formatting failed");

    // Break into groups of three
    static const char *const scale[] = {"",
                                        "thousand",
                                        "million",
                                        "billion",
                                        "trillion",
                                        "quadrillion",
                                        "quintillion",
                                        "sextillion"};
    int groups[8] = {0};
    int num_groups = 0;

    uint64_t temp = abs_n;
    while (temp > 0 && num_groups < 8) {
        groups[num_groups++] = (int)(temp % 1000);
        temp /= 1000;
    }

    int has_prev = 0;
    for (int i = num_groups - 1; i >= 0; i--) {
        if (groups[i] == 0)
            continue;
        append_chunk(&sb, groups[i], &has_prev);
        if (i > 0 && scale[i][0] != '\0') {
            numfmt_check_sb(rt_sb_append_cstr(&sb, " "), "NumberFormat.ToWords: formatting failed");
            numfmt_check_sb(rt_sb_append_cstr(&sb, scale[i]),
                            "NumberFormat.ToWords: formatting failed");
        }
    }

    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;
}

// ---------------------------------------------------------------------------
// rt_numfmt_bytes
// ---------------------------------------------------------------------------

/// @brief Format a byte count with a human-readable unit suffix (`B`, `KB`, `MB`, ...).
/// @details Steps the value down by factors of 1024 (binary, not
///          decimal) and caps the unit at exabytes (`EB`). Bytes are emitted
///          as integers; larger units use one fractional digit at magnitudes
///          of at least ten and two below ten. Fractional punctuation follows
///          the process numeric locale because this operation uses `snprintf`
///          directly.
/// @param bytes Signed byte count to format.
/// @return Newly allocated size string owned by the caller.
rt_string rt_numfmt_bytes(int64_t bytes) {
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    uint64_t magnitude = abs_i64_magnitude(bytes);
    double val = (double)magnitude;
    int unit_idx = 0;

    while (val >= 1024.0 && unit_idx < 6) {
        val /= 1024.0;
        unit_idx++;
    }

    char buf[64];
    int len;

    if (unit_idx == 0) {
        len = snprintf(
            buf, sizeof(buf), "%s%" PRIu64 " %s", bytes < 0 ? "-" : "", magnitude, units[0]);
    } else {
        if (bytes < 0)
            val = -val;
        // Use 1 decimal place for values >= 10, 2 for smaller
        if (fabs(val) >= 10.0)
            len = snprintf(buf, sizeof(buf), "%.1f %s", val, units[unit_idx]);
        else
            len = snprintf(buf, sizeof(buf), "%.2f %s", val, units[unit_idx]);
    }

    if (len < 0)
        len = 0;
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;
    return rt_string_from_bytes(buf, (size_t)len);
}

// ---------------------------------------------------------------------------
// rt_numfmt_pad
// ---------------------------------------------------------------------------

/// @brief Zero-pad an integer to the specified character width.
/// @details `width` is clamped to `[1, 64]`. For positive values,
///          uses printf's `%0*lld` directly. For negatives, the
///          width budget includes the leading `-` so `pad(-5, 4)`
///          produces `"-005"` (4 chars total, not 5). Values wider than the
///          requested minimum are never truncated.
/// @param n Integer to format.
/// @param width Requested minimum character width, including any sign.
/// @return Newly allocated zero-padded string owned by the caller.
rt_string rt_numfmt_pad(int64_t n, int64_t width) {
    if (width < 1)
        width = 1;
    if (width > 64)
        width = 64;

    char buf[128];
    int len;

    if (n >= 0)
        len = snprintf(buf, sizeof(buf), "%0*lld", (int)width, (long long)n);
    else {
        uint64_t magnitude = abs_i64_magnitude(n);
        len = snprintf(buf, sizeof(buf), "-%0*" PRIu64, (int)(width - 1), magnitude);
    }

    if (len < 0)
        len = 0;
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;
    return rt_string_from_bytes(buf, (size_t)len);
}
