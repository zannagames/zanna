//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_fmt.h
// Purpose: Value formatting functions for the Zanna.Text.Fmt namespace, providing decimal, radix,
// padded, fixed-precision, scientific, percentage, boolean, and human-readable byte-size string
// conversions.
//
// Key invariants:
//   - All formatting functions return newly allocated rt_string objects.
//   - Radix formatting accepts bases 2-36; invalid radix returns an empty string.
//   - rt_fmt_size scales by 1024 and uses B through EB display suffixes.
//   - Boolean formatting produces 'true'/'false' (rt_fmt_bool) or 'yes'/'no' (rt_fmt_bool_yn).
//
// Ownership/Lifetime:
//   - Returned strings are newly allocated with refcount 1; callers must release them.
//   - Input rt_string parameters are not retained; callers retain ownership.
//
// Links: src/runtime/core/rt_fmt.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares deterministic scalar and presentation-oriented formatters.
/// @details Every result is a caller-owned runtime string. Invalid parameters,
///   allocation failures, or internal formatting overflow produce an owned
///   empty string unless the individual function documents another fallback.

#pragma once

#include "rt_string.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Format an integer as decimal string.
/// @param value The integer to format.
/// @return Caller-owned decimal string such as `42` or `-123`; empty if the
///   conversion cannot fit the internal buffer.
rt_string rt_fmt_int(int64_t value);

/// @brief Format an integer in specified radix (base 2-36).
/// @details Negative values retain a minus sign only in base ten. Other bases
///   reinterpret the input as an unsigned 64-bit bit pattern and use lowercase
///   digits.
/// @param value The integer to format.
/// @param radix Base for formatting (2-36).
/// @return Caller-owned formatted string without a radix prefix, or an empty
///   string when @p radix is invalid.
rt_string rt_fmt_int_radix(int64_t value, int64_t radix);

/// @brief Format an integer with minimum width and padding character.
/// @details Repeats the first valid UTF-8 code point in @p pad_char. A missing,
///   empty, or invalid pad string falls back to a space. When padding a
///   negative value with ASCII `0`, the sign remains before the padding.
/// @param value The integer to format.
/// @param width Minimum character width; non-positive or already-satisfied
///   widths leave the decimal representation unchanged.
/// @param pad_char String containing pad character (uses first valid UTF-8 character).
/// @return Caller-owned padded string, or an empty string if sizing or
///   allocation fails.
rt_string rt_fmt_int_pad(int64_t value, int64_t width, rt_string pad_char);

/// @brief Format a number with the historical 15-significant-digit representation.
/// @details Uses the C numeric locale and `%.15g`, strips negative zero, and
///   emits `NaN`, `Infinity`, or `-Infinity` for non-finite values. The
///   representation is stable but is not guaranteed to round-trip every
///   IEEE-754 double exactly.
/// @param value The number to format.
/// @return Caller-owned formatted string such as `2.5`, or an empty string on
///   locale or formatting failure.
rt_string rt_fmt_num(double value);

/// @brief Format a number with fixed decimal places.
/// @details Uses the C numeric locale, clamps @p decimals to [0, 20], and
///   suppresses a negative sign when the rounded text represents zero.
/// @param value The number to format.
/// @param decimals Requested decimal places; values outside [0, 20] are clamped.
/// @return Caller-owned fixed-point string such as `3.14`, a canonical
///   non-finite spelling, or an empty string on formatting failure.
rt_string rt_fmt_num_fixed(double value, int64_t decimals);

/// @brief Format a number in scientific notation.
/// @details Uses the C numeric locale and a lowercase `e`, clamps @p decimals
///   to [0, 20], and suppresses rounded negative zero.
/// @param value The number to format.
/// @param decimals Requested fractional digits in the mantissa; clamped to
///   [0, 20].
/// @return Caller-owned scientific string such as `3.14e+00`, a canonical
///   non-finite spelling, or an empty string on formatting failure.
rt_string rt_fmt_num_sci(double value, int64_t decimals);

/// @brief Format a number as percentage.
/// @details Multiplies @p value by 100, clamps @p decimals to [0, 20], formats
///   in the C numeric locale, suppresses rounded negative zero, and appends `%`.
/// @param value The number to format (0.5 = 50%).
/// @param decimals Requested decimal places; values outside [0, 20] are clamped.
/// @return Caller-owned percentage string such as `50.00%`; non-finite
///   spellings also include `%`. Returns empty on formatting failure.
rt_string rt_fmt_num_pct(double value, int64_t decimals);

/// @brief Format a boolean as "true" or "false".
/// @param value The boolean to format.
/// @return Caller-owned runtime string containing `true` or `false`.
rt_string rt_fmt_bool(bool value);

/// @brief Format a boolean as "yes" or "no".
/// @param value The boolean to format.
/// @return Caller-owned runtime string containing lowercase `yes` or `no`.
rt_string rt_fmt_bool_yn(bool value);

/// @brief Format a byte size as human-readable string.
/// @details Scales by powers of 1024 from bytes through exbibyte magnitude,
///   labels the units `B` through `EB`, and preserves a negative sign. Bytes
///   have no fractional digit; scaled units always have one.
/// @param bytes The size in bytes.
/// @return Caller-owned size string such as `512 B` or `1.5 KB`, or empty on
///   formatting failure.
rt_string rt_fmt_size(int64_t bytes);

/// @brief Format an integer as hexadecimal (lowercase).
/// @details Reinterprets @p value as unsigned, preserving the full 64-bit bit
///   pattern for negative inputs.
/// @param value The integer to format.
/// @return Caller-owned lowercase hexadecimal string without a prefix, or
///   empty on formatting failure.
rt_string rt_fmt_hex(int64_t value);

/// @brief Format an integer as padded hexadecimal.
/// @details Reinterprets @p value as unsigned and clamps @p width to [1, 16].
///   Width is a minimum and never truncates a longer representation.
/// @param value The integer to format.
/// @param width Requested minimum digit width, padded with ASCII zero.
/// @return Caller-owned lowercase hexadecimal string without a prefix, or
///   empty on formatting failure.
rt_string rt_fmt_hex_pad(int64_t value, int64_t width);

/// @brief Format an integer as binary.
/// @details Reinterprets @p value as unsigned and emits the shortest
///   representation, except that zero is represented by one digit.
/// @param value The integer to format.
/// @return Caller-owned binary string without a prefix.
rt_string rt_fmt_bin(int64_t value);

/// @brief Format an integer as octal.
/// @details Reinterprets @p value as unsigned so negative inputs expose their
///   64-bit representation.
/// @param value The integer to format.
/// @return Caller-owned octal string without a prefix, or empty on formatting
///   failure.
rt_string rt_fmt_oct(int64_t value);

/// @brief Format an integer with thousands separators.
/// @details Inserts the entire byte sequence of @p sep between three-digit
///   decimal groups. A NULL, invalid, or empty separator falls back to `,`.
/// @param value The integer to format.
/// @param sep Separator string (e.g., "," or ".").
/// @return Caller-owned grouped string such as `1,234,567`, or an empty
///   string on sizing, allocation, or formatting failure.
rt_string rt_fmt_int_grouped(int64_t value, rt_string sep);

/// @brief Format a number as currency.
/// @details Uses comma grouping and C-locale fixed-point formatting. Decimal
///   precision is clamped to [0, 20]. The sign precedes the symbol, and
///   rounded negative zero loses its sign. A NULL or invalid symbol defaults
///   to `$`, while a valid empty symbol is preserved. Non-finite values are
///   returned without a symbol.
/// @param value The amount.
/// @param decimals Requested decimal places, typically 2.
/// @param symbol Currency symbol (e.g., "$", "€").
/// @return Caller-owned string such as `$1,234.56`, a canonical non-finite
///   spelling, or an empty string on failure.
rt_string rt_fmt_currency(double value, int64_t decimals, rt_string symbol);

/// @brief Convert an integer to English words.
/// @details Uses lowercase American-style scale names through quintillion,
///   hyphenates compound tens, omits `and`, and prefixes negative values with
///   `negative`.
/// @param value The number, covering the full signed 64-bit range.
/// @return Caller-owned English text such as `one hundred twenty-three`, or
///   an empty string if the internal bounded composition fails.
rt_string rt_fmt_to_words(int64_t value);

/// @brief Convert an integer to ordinal string.
/// @details Chooses the suffix from the absolute value, including the
///   11th/12th/13th exception, while retaining the original decimal sign.
/// @param value The number.
/// @return Caller-owned ordinal string such as `1st`, `-2nd`, or `13th`, or
///   an empty string on formatting failure.
rt_string rt_fmt_ordinal(int64_t value);

#ifdef __cplusplus
}
#endif
