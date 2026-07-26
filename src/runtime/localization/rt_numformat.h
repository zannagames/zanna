//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_numformat.h
// Purpose: Public C API for Zanna.Localization.NumberFormat — locale-aware
//          number formatting AND parsing. Wraps a Locale with mutable per-
//          instance options (fraction digit range, grouping on/off, strict
//          mode, rounding policy), locale digit glyphs, primary/secondary
//          grouping, currency patterns, and localized parser tokens.
//
// Key invariants:
//   - Every instance captures the Locale's rt_locale_data_t at construction;
//     subsequent reads never touch the registry (lock-free hot path).
//   - Strict mode rejects inputs where the locale's group separator appears
//     in a position that doesn't match the locale's grouping (e.g., "1,00"
//     under en-US is ambiguous between "1.00" and "100" — rejected). Lenient
//     mode accepts the input by treating the separator as informational.
//   - Supported rounding policies are halfEven (default / banker's), halfUp,
//     halfDown, up, down, ceiling, and floor.
//
// Ownership/Lifetime:
//   - Handles are rt_obj_new_i64-allocated; GC-managed.
//   - Each formatter retains its Locale handle and captured locale-data record.
//   - Formatting methods and rounding-name accessors return owned strings.
//
// Links: src/runtime/localization/rt_numformat.c (implementation),
//        src/runtime/text/rt_numfmt.h (invariant ordinal fallback),
//        src/runtime/localization/rt_locale.h (Locale handle type),
//        docs/zannalib/localization/formatting.md (user documentation).
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt.hpp"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Constructors
//===----------------------------------------------------------------------===//

/// @brief Default constructor; uses the current LocaleManager locale.
/// @return Newly allocated NumberFormat handle, or NULL after allocation
///         failure if the runtime trap hook returns.
void *rt_numformat_new(void);

/// @brief Construct a NumberFormat bound to @p locale.
/// @param locale Locale handle retained by the formatter.
/// @return Newly allocated NumberFormat handle, or NULL after allocation
///         failure if the runtime trap hook returns.
void *rt_numformat_for_locale(void *locale);

//===----------------------------------------------------------------------===//
// Properties
//===----------------------------------------------------------------------===//

/// @brief Return the Locale handle this formatter was built with (borrowed).
/// @param self NumberFormat handle, or NULL.
/// @return Borrowed Locale handle, or NULL when @p self is NULL.
void *rt_numformat_get_locale(void *self);

/// @brief Get the minimum number of fraction digits emitted.
/// @param self NumberFormat handle, or NULL.
/// @return Configured minimum, or zero when @p self is NULL.
int64_t rt_numformat_get_min_frac(void *self);

/// @brief Set the minimum number of fraction digits emitted.
/// @details Clamps to 0 through 20 and raises the maximum if necessary.
/// @param self NumberFormat handle to update, or NULL for a no-op.
/// @param value Requested minimum fraction-digit count.
void rt_numformat_set_min_frac(void *self, int64_t value);

/// @brief Get the maximum number of fraction digits emitted (rounded to fit).
/// @param self NumberFormat handle, or NULL.
/// @return Configured maximum, or three when @p self is NULL.
int64_t rt_numformat_get_max_frac(void *self);

/// @brief Set the maximum number of fraction digits emitted (rounded to fit).
/// @details Clamps to 0 through 20 and lowers the minimum if necessary.
/// @param self NumberFormat handle to update, or NULL for a no-op.
/// @param value Requested maximum fraction-digit count.
void rt_numformat_set_max_frac(void *self, int64_t value);

/// @brief Get whether digit grouping (thousands separators) is enabled (0/1).
/// @param self NumberFormat handle, or NULL.
/// @return 0 or 1; defaults to 1 when @p self is NULL.
int8_t rt_numformat_get_grouping(void *self);

/// @brief Set whether digit grouping (thousands separators) is enabled.
/// @param self NumberFormat handle to update, or NULL for a no-op.
/// @param value Zero disables grouping; non-zero enables it.
void rt_numformat_set_grouping(void *self, int8_t value);

/// @brief Get strict-parsing mode (0/1); strict rejects loose/partial input.
/// @param self NumberFormat handle, or NULL.
/// @return 0 or 1; defaults to 0 when @p self is NULL.
int8_t rt_numformat_get_strict(void *self);

/// @brief Set strict-parsing mode.
/// @param self NumberFormat handle to update, or NULL for a no-op.
/// @param value Zero selects lenient mode; non-zero selects strict mode.
void rt_numformat_set_strict(void *self, int8_t value);

/// @brief Get the rounding mode name (e.g. "halfEven", "halfUp", "floor").
/// @param self NumberFormat handle, or NULL for the half-even default.
/// @return Newly allocated runtime string containing the canonical mode name.
rt_string rt_numformat_get_rounding(void *self);

/// @brief Set the rounding mode by name; unknown names fall back to halfEven.
/// @param self NumberFormat handle to update, or NULL for a no-op.
/// @param mode Runtime string containing the canonical mode name; NULL is ignored.
void rt_numformat_set_rounding(void *self, rt_string mode);

//===----------------------------------------------------------------------===//
// Format methods
//===----------------------------------------------------------------------===//

/// @brief Format @p value as a decimal using the formatter's settings.
/// @param self NumberFormat handle, or NULL.
/// @param value Floating-point value to format.
/// @return Newly allocated localized string, or an owned empty string for NULL
///         @p self or a formatting failure.
rt_string rt_numformat_decimal(void *self, double value);

/// @brief Format @p value as a decimal with exactly @p digits fraction places.
/// @details Clamps @p digits to 0 through 20.
/// @param self NumberFormat handle, or NULL.
/// @param value Floating-point value to format.
/// @param digits Requested exact fraction-digit count.
/// @return Newly allocated localized string, or an owned empty string for NULL
///         @p self or a formatting failure.
rt_string rt_numformat_decimal_n(void *self, double value, int64_t digits);

/// @brief Format @p value as an exact integer (no float round-trip).
/// @param self NumberFormat handle, or NULL.
/// @param value Signed 64-bit integer to format.
/// @return Newly allocated localized string, or an owned empty string for NULL
///         @p self or a formatting failure.
rt_string rt_numformat_integer(void *self, int64_t value);

/// @brief Format @p value as a percentage (scaled ×100, locale percent sign).
/// @param self NumberFormat handle, or NULL.
/// @param value Ratio to scale and format.
/// @return Newly allocated percentage string, or an owned empty string for
///         NULL @p self or a formatting failure.
rt_string rt_numformat_percent(void *self, double value);

/// @brief Format @p value as currency using the locale's default symbol.
/// @details Uses the locale currency precision and positive/negative pattern.
/// @param self NumberFormat handle, or NULL.
/// @param value Monetary value to format.
/// @return Newly allocated currency string, or an owned empty string for NULL
///         @p self or a formatting failure.
rt_string rt_numformat_currency(void *self, double value);

/// @brief Format @p value as currency using the given ISO 4217 @p code.
/// @details Requires three uppercase ASCII letters. Non-default codes are
///          emitted literally in the locale pattern's symbol position.
/// @param self NumberFormat handle, or NULL.
/// @param value Monetary value to format.
/// @param code Three-uppercase-letter runtime string.
/// @return Newly allocated currency string, or an owned empty string after
///         invalid input, NULL @p self, or a formatting failure.
rt_string rt_numformat_currency_of(void *self, double value, rt_string code);

/// @brief Format @p value in scientific notation with @p digits mantissa frac.
/// @details Clamps @p digits to 0 through 20 and localizes decimal, exponent,
///          sign, digit, NaN, and infinity tokens.
/// @param self NumberFormat handle, or NULL.
/// @param value Floating-point value to format.
/// @param digits Requested mantissa fraction-digit count.
/// @return Newly allocated scientific string, or an owned empty string for
///         NULL @p self or a formatting failure.
rt_string rt_numformat_scientific(void *self, double value, int64_t digits);

/// @brief Format @p value with invariant English ordinal suffixes.
/// @details Locale-specific ordinal rendering is not yet implemented; @p self
///          is currently unused.
/// @param self NumberFormat handle, currently permitted to be NULL.
/// @param value Signed integer to format.
/// @return Newly allocated invariant ordinal string.
rt_string rt_numformat_ordinal(void *self, int64_t value);

//===----------------------------------------------------------------------===//
// Parse methods
//===----------------------------------------------------------------------===//

/// @brief Strict-or-lenient parse of a locale-formatted decimal string.
/// @details Accepts locale NaN and infinity tokens. Traps on invalid input; use
///          the Try* variant for an Option result.
/// @param self NumberFormat handle.
/// @param input Runtime string to parse.
/// @return Parsed double, or zero after a trapped error.
double rt_numformat_parse_decimal(void *self, rt_string input);

/// @brief Try-parse variant: returns Option<f64> (Some on success, None on failure).
/// @param self NumberFormat handle, or NULL.
/// @param input Runtime string to parse, or NULL.
/// @return Newly allocated `Some<f64>` on success; otherwise `None`.
void *rt_numformat_try_parse_decimal(void *self, rt_string input);

/// @brief Integer parse; rejects fractional content.
/// @details Traps on malformed or out-of-range input.
/// @param self NumberFormat handle.
/// @param input Runtime string to parse.
/// @return Exact parsed integer, or zero after a trapped error.
int64_t rt_numformat_parse_integer(void *self, rt_string input);

/// @brief Try-parse integer variant: returns Option<i64> (Some/None).
/// @param self NumberFormat handle, or NULL.
/// @param input Runtime string to parse, or NULL.
/// @return Newly allocated `Some<i64>` on success; otherwise `None`.
void *rt_numformat_try_parse_integer(void *self, rt_string input);

/// @brief Currency parse; accepts an optional leading / trailing currency
///        symbol, default code, or three-uppercase-letter ISO code.
/// @details Also recognizes locale currency patterns and accounting parentheses.
///          Traps when the interior numeric text is invalid.
/// @param self NumberFormat handle.
/// @param input Runtime string to parse.
/// @return Parsed monetary value, or zero after a trapped error.
double rt_numformat_parse_currency(void *self, rt_string input);

/// @brief Try-parse currency variant: returns Option<f64> (Some/None).
/// @param self NumberFormat handle, or NULL.
/// @param input Runtime string to parse, or NULL.
/// @return Newly allocated `Some<f64>` on success; otherwise `None`.
void *rt_numformat_try_parse_currency(void *self, rt_string input);

#ifdef __cplusplus
}
#endif
