//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_reltime_format.h
// Purpose: Public C API for Zanna.Localization.RelativeTimeFormat — formats
//          durations as human-readable relative-time expressions ("3 days
//          ago", "in 2 hours"). Unit selection is automatic based on the
//          duration magnitude; plural form selection routes through
//          PluralRules for the bound locale.
//
// Key invariants:
//   - Unit thresholds: >=1y -> year, >=30d -> month, >=7d -> week, >=1d -> day,
//     >=1h -> hour, >=1m -> minute, else second. Sign of duration picks past
//     vs. future template.
//   - Duration inputs are int64 (milliseconds, per rt_duration's convention).
//     Positive values render as past ("N units ago"); negative values render
//     as future ("in N units"). This matches the common "elapsed since now"
//     framing used throughout Zanna game/UI code.
//
// Ownership/Lifetime:
//   - Instances are rt_obj_new_i64-allocated; GC-managed.
//   - Each formatter retains its Locale handle and captured locale-data record.
//   - Style access and all formatting methods return owned runtime strings.
//
// Links: src/runtime/localization/rt_reltime_format.c (implementation),
//        src/runtime/localization/rt_plural_rules.h (category selection),
//        src/runtime/core/rt_duration.h (input handle semantics),
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

/// @brief Create a relative-time formatter bound to the current locale.
/// @return Newly allocated formatter, or NULL after allocation failure if the
///         runtime trap hook returns.
void *rt_reltimefmt_new(void);

/// @brief Create a relative-time formatter bound to the given @p locale.
/// @param locale Locale handle retained by the formatter, or NULL for invariant data.
/// @return Newly allocated formatter, or NULL after allocation failure if the
///         runtime trap hook returns.
void *rt_reltimefmt_for_locale(void *locale);

//===----------------------------------------------------------------------===//
// Properties
//===----------------------------------------------------------------------===//

/// @brief Return the Locale handle this formatter was built with (borrowed).
/// @param self RelativeTimeFormat handle, or NULL.
/// @return Borrowed Locale handle, or NULL when @p self is NULL.
void *rt_reltimefmt_get_locale(void *self);

/// @brief Current style identifier ("long" default; "short" for compact form).
/// @param self RelativeTimeFormat handle, or NULL for the long default.
/// @return Newly allocated runtime string containing `"long"` or `"short"`.
rt_string rt_reltimefmt_get_style(void *self);

/// @brief Set the style ("long" or "short"); other values trap.
/// @details Invalid names leave the current style unchanged; NULL arguments
///          are ignored.
/// @param self RelativeTimeFormat handle to update, or NULL.
/// @param style Runtime string naming the style, or NULL.
void rt_reltimefmt_set_style(void *self, rt_string style);

//===----------------------------------------------------------------------===//
// Format methods
//===----------------------------------------------------------------------===//

/// @brief Format @p duration (ms) using the default style. Positive = past;
///        negative = future.
/// @details Magnitudes below one second return the locale's `now` token.
/// @param self RelativeTimeFormat handle, or NULL.
/// @param duration Signed duration in milliseconds.
/// @return Newly allocated relative-time string, or an owned empty string for
///         NULL @p self or a formatting failure.
rt_string rt_reltimefmt_format(void *self, int64_t duration);

/// @brief Format relative time between two Unix timestamps in seconds.
/// @details Computes `now_ts - then_ts`, checks subtraction and millisecond
///          conversion for overflow, and uses the formatter's current style.
/// @param self RelativeTimeFormat handle, or NULL.
/// @param then_ts Reference Unix timestamp in seconds.
/// @param now_ts Current Unix timestamp in seconds.
/// @return Newly allocated relative-time string, or an owned empty string for
///         NULL @p self or after a trapped overflow.
rt_string rt_reltimefmt_format_from(void *self, int64_t then_ts, int64_t now_ts);

/// @brief Short-style format.
/// @details Uses short units/templates when present and falls back to the long
///          template when short template data is absent.
/// @param self RelativeTimeFormat handle, or NULL.
/// @param duration Signed milliseconds; positive is past and negative is future.
/// @return Newly allocated relative-time string, or an owned empty string for
///         NULL @p self or a formatting failure.
rt_string rt_reltimefmt_short(void *self, int64_t duration);

/// @brief Long-style format (default).
/// @param self RelativeTimeFormat handle, or NULL.
/// @param duration Signed milliseconds; positive is past and negative is future.
/// @return Newly allocated relative-time string, or an owned empty string for
///         NULL @p self or a formatting failure.
rt_string rt_reltimefmt_long(void *self, int64_t duration);

/// @brief Format @p value with an explicit @p unit (one of "second"/"minute"/
///        "hour"/"day"/"week"/"month"/"year"). Sign of @p value picks past/future.
/// @details Does not coarsen the explicit unit. Zero returns the locale's
///          `now` token; invalid units trap.
/// @param self RelativeTimeFormat handle.
/// @param value Signed unit count; positive is past and negative is future.
/// @param unit Exact lowercase unit-name runtime string.
/// @return Newly allocated relative-time string, or an owned empty string
///         after invalid input, a trapped unit error, or formatting failure.
rt_string rt_reltimefmt_numeric(void *self, int64_t value, rt_string unit);

#ifdef __cplusplus
}
#endif
