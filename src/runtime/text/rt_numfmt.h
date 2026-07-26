//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_numfmt.h
// Purpose: Declares invariant numeric-display helpers for fixed decimals,
// grouped integers, currency, percentages, English ordinals and words,
// human-readable byte counts, and zero-padded integers.
//
// Key invariants:
//   - All returned strings are newly allocated; caller must release.
//   - Decimal and currency output uses a period as the decimal separator;
//     currency uses comma grouping, while integer grouping accepts a separator.
//   - Percentage inputs are ratios and are multiplied by 100.
//   - Ordinal and word conversion follows US English conventions.
//   - All signed-integer operations support the complete int64_t range.
//
// Ownership/Lifetime:
//   - Returned strings are newly allocated; caller must release.
//   - Input values are passed by value; no ownership transfer.
//
// Links: src/runtime/text/rt_numfmt.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_numfmt.h
 * @brief Declares invariant numeric presentation helpers.
 * @details Value-only operations return caller-owned Strings for decimal,
 *          grouped, monetary, percentage, ordinal, English-word, human byte,
 *          and padded integer representations with documented US-English and
 *          C-locale conventions.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Format a floating-point value with a fixed number of decimal places.
/// @details Clamps @p decimals to `[0, 20]` and uses a C-locale decimal point.
///          Non-finite values become `"NaN"` or signed `"Infinity"`.
/// @param n Value to format.
/// @param decimals Requested number of fractional digits.
/// @return Newly allocated formatted string, for example `"1234.57"`.
rt_string rt_numfmt_decimals(double n, int64_t decimals);

/// @brief Format an integer with three-digit grouping.
/// @details Uses a comma when @p sep is null or empty. A nonempty separator is
///          copied byte-for-byte between groups.
/// @param n Integer to format.
/// @param sep Separator string, such as `","` or `"_"`.
/// @return Newly allocated grouped string, for example `"1,234,567"`.
rt_string rt_numfmt_thousands(int64_t n, rt_string sep);

/// @brief Format a number as currency.
/// @details Places the sign before the symbol, groups the magnitude with
///          commas, and emits exactly two fractional digits. A null symbol
///          defaults to `"$"`; a deliberately empty symbol is preserved.
/// @param n Monetary value to format.
/// @param symbol Currency-symbol string.
/// @return Newly allocated string such as `"$1,234.56"`.
rt_string rt_numfmt_currency(double n, rt_string symbol);

/// @brief Format a number as a percentage.
/// @details Multiplies @p n by 100, rounds to one fractional digit, and removes
///          a trailing `.0`. Non-finite results retain the percent suffix.
/// @param n Ratio to format, for example `0.756`.
/// @return Newly allocated percentage string, for example `"75.6%"`.
rt_string rt_numfmt_percent(double n);

/// @brief Format a signed integer with its English ordinal suffix.
/// @details Applies the 11th/12th/13th exceptions before selecting `st`, `nd`,
///          `rd`, or `th` from the final digit.
/// @param n Integer to format.
/// @return Newly allocated ordinal such as `"1st"` or `"-22nd"`.
rt_string rt_numfmt_ordinal(int64_t n);

/// @brief Convert a signed 64-bit integer to US English words.
/// @details Supports the full `int64_t` range through quintillions, omits
///          `"and"` after hundreds, and hyphenates compound tens.
/// @param n Integer to convert.
/// @return Newly allocated English text such as `"forty-two"`.
rt_string rt_numfmt_to_words(int64_t n);

/// @brief Format a signed byte count with a scaled unit suffix.
/// @details Scales by powers of 1024 through `EB`. Byte values are integral;
///          scaled values use one or two fractional digits according to size.
/// @param bytes Signed number of bytes.
/// @return Newly allocated string such as `"1.50 KB"` or `"12.0 MB"`.
rt_string rt_numfmt_bytes(int64_t bytes);

/// @brief Zero-pad an integer to a minimum character width.
/// @details Clamps @p width to `[1, 64]`; a negative sign counts toward the
///          width, and longer values are never truncated.
/// @param n Integer to format.
/// @param width Requested minimum output width.
/// @return Newly allocated padded string, for example `"00042"`.
rt_string rt_numfmt_pad(int64_t n, int64_t width);

#ifdef __cplusplus
}
#endif
