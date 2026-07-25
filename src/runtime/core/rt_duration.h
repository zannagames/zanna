//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_duration.h
/// @file
/// @brief Declares scalar millisecond Duration construction, arithmetic, and formatting.
///
// Purpose: Duration/TimeSpan type for representing time intervals stored as milliseconds, with
// creation helpers for common units and arithmetic/formatting operations.
//
// Key invariants:
//   - Duration is stored as a signed 64-bit integer in milliseconds.
//   - Negative durations are valid and represent time in the past.
//   - Conversion helpers and arithmetic methods trap on signed 64-bit overflow.
//   - Arithmetic operations on durations return value types, not heap objects.
//
// Ownership/Lifetime:
//   - Duration values are plain int64_t; no heap allocation or lifetime management.
//   - Formatted string results are newly allocated and must be released by the caller.
//
// Links: src/runtime/core/rt_duration.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Duration Creation
//=========================================================================

/// @brief Create a Duration from milliseconds.
/// @param ms Milliseconds.
/// @return Duration value (milliseconds).
int64_t rt_duration_from_millis(int64_t ms);

/// @brief Create a Duration from seconds.
/// @param seconds Seconds.
/// @return Duration value (milliseconds).
int64_t rt_duration_from_seconds(int64_t seconds);

/// @brief Create a Duration from minutes.
/// @param minutes Minutes.
/// @return Duration value (milliseconds).
int64_t rt_duration_from_minutes(int64_t minutes);

/// @brief Create a Duration from hours.
/// @param hours Hours.
/// @return Duration value (milliseconds).
int64_t rt_duration_from_hours(int64_t hours);

/// @brief Create a Duration from days.
/// @param days Days.
/// @return Duration value (milliseconds).
int64_t rt_duration_from_days(int64_t days);

/// @brief Create a Duration from components.
/// @param days Days component.
/// @param hours Hours contribution; values are not restricted to 0-23.
/// @param minutes Minutes contribution; values are not restricted to 0-59.
/// @param seconds Seconds contribution; values are not restricted to 0-59.
/// @param millis Milliseconds contribution; values are not restricted to 0-999.
/// @return Duration value (milliseconds).
int64_t rt_duration_create(
    int64_t days, int64_t hours, int64_t minutes, int64_t seconds, int64_t millis);

//=========================================================================
// Duration Total Conversions
//=========================================================================

/// @brief Get total milliseconds in the duration.
/// @param duration Duration value (milliseconds).
/// @return Total milliseconds.
int64_t rt_duration_total_millis(int64_t duration);

/// @brief Get total seconds in the duration (truncated).
/// @param duration Duration value (milliseconds).
/// @return Total seconds truncated toward zero.
int64_t rt_duration_total_seconds(int64_t duration);

/// @brief Get total minutes in the duration (truncated).
/// @param duration Duration value (milliseconds).
/// @return Total minutes truncated toward zero.
int64_t rt_duration_total_minutes(int64_t duration);

/// @brief Get total hours in the duration (truncated).
/// @param duration Duration value (milliseconds).
/// @return Total hours truncated toward zero.
int64_t rt_duration_total_hours(int64_t duration);

/// @brief Get total days in the duration (truncated).
/// @param duration Duration value (milliseconds).
/// @return Total days truncated toward zero.
int64_t rt_duration_total_days(int64_t duration);

/// @brief Get total seconds as a double (with fractional part).
/// @param duration Duration value (milliseconds).
/// @return Total seconds as double.
double rt_duration_total_seconds_f(int64_t duration);

//=========================================================================
// Duration Components
//=========================================================================

/// @brief Get the days component of the duration.
/// @param duration Duration value (milliseconds).
/// @return Nonnegative whole-day component of the absolute magnitude.
int64_t rt_duration_get_days(int64_t duration);

/// @brief Get the hours component (0-23) after extracting days.
/// @param duration Duration value (milliseconds).
/// @return Absolute-magnitude hours component (0-23).
int64_t rt_duration_get_hours(int64_t duration);

/// @brief Get the minutes component (0-59) after extracting hours.
/// @param duration Duration value (milliseconds).
/// @return Absolute-magnitude minutes component (0-59).
int64_t rt_duration_get_minutes(int64_t duration);

/// @brief Get the seconds component (0-59) after extracting minutes.
/// @param duration Duration value (milliseconds).
/// @return Absolute-magnitude seconds component (0-59).
int64_t rt_duration_get_seconds(int64_t duration);

/// @brief Get the milliseconds component (0-999) after extracting seconds.
/// @param duration Duration value (milliseconds).
/// @return Absolute-magnitude milliseconds component (0-999).
int64_t rt_duration_get_millis(int64_t duration);

//=========================================================================
// Duration Operations
//=========================================================================

/// @brief Add two durations.
/// @param d1 First duration (milliseconds).
/// @param d2 Second duration (milliseconds).
/// @return Sum of durations (milliseconds).
int64_t rt_duration_add(int64_t d1, int64_t d2);

/// @brief Subtract two durations.
/// @param d1 First duration (milliseconds).
/// @param d2 Second duration (milliseconds).
/// @return Difference (d1 - d2) in milliseconds.
int64_t rt_duration_sub(int64_t d1, int64_t d2);

/// @brief Multiply a duration by a scalar.
/// @param duration Duration value (milliseconds).
/// @param factor Multiplication factor.
/// @return Scaled duration (milliseconds).
int64_t rt_duration_mul(int64_t duration, int64_t factor);

/// @brief Divide a duration by a scalar.
/// @param duration Duration value (milliseconds).
/// @param divisor Division factor (must not be 0).
/// @return Divided duration truncated toward zero.
/// @note Traps on zero or the overflowing `INT64_MIN / -1` case.
int64_t rt_duration_div(int64_t duration, int64_t divisor);

/// @brief Get the absolute value of a duration.
/// @param duration Duration value (milliseconds).
/// @return Absolute duration (milliseconds). Traps for INT64_MIN.
int64_t rt_duration_abs(int64_t duration);

/// @brief Negate a duration.
/// @param duration Duration value (milliseconds).
/// @return Negated duration (milliseconds). Traps for INT64_MIN.
int64_t rt_duration_neg(int64_t duration);

//=========================================================================
// Duration Comparison
//=========================================================================

/// @brief Compare two durations.
/// @param d1 First duration (milliseconds).
/// @param d2 Second duration (milliseconds).
/// @return -1 if d1 < d2, 0 if equal, 1 if d1 > d2.
int64_t rt_duration_cmp(int64_t d1, int64_t d2);

//=========================================================================
// Duration Formatting
//=========================================================================

/// @brief Format a duration as a human-readable string.
/// @details Format: "[-]d.hh:mm:ss.fff" or shorter if components are zero.
/// @param duration Duration value (milliseconds).
/// @return Formatted string (e.g., "1.02:30:45.500" or "02:30:45").
rt_string rt_duration_to_string(int64_t duration);

/// @brief Format a duration in ISO 8601 duration format.
/// @details Format: `[-]P[n]DT[n]H[n]M[n[.fff]]S`; zero is `PT0S` and
///          fractional-second trailing zeroes are omitted.
/// @param duration Duration value (milliseconds).
/// @return ISO 8601 duration string.
rt_string rt_duration_to_iso(int64_t duration);

//=========================================================================
// Constants
//=========================================================================

/// @brief Zero duration constant.
/// @return Duration of zero milliseconds.
int64_t rt_duration_zero(void);

#ifdef __cplusplus
}
#endif
