//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_dateonly.h
/// @file
/// @brief Declares the GC-managed DateOnly proleptic Gregorian value type.
///
// Purpose: DateOnly type representing a calendar date without time or timezone components,
// providing creation, parsing, arithmetic, and formatting operations.
//
// Key invariants:
//   - Month is 1-indexed (1=January, 12=December); day is 1-indexed.
//   - Days since epoch are counted from 1970-01-01 (Unix epoch, day 0).
//   - Creation and parsing share the year range 0000–9999. Parsing accepts
//     exactly ten bytes in YYYY-MM-DD form, so every constructed value
//     round-trips through the ISO formatter.
//   - Date arithmetic traps on signed 64-bit overflow.
//
// Ownership/Lifetime:
//   - DateOnly objects are heap-allocated runtime objects managed through Zanna's
//     reference-counting/GC lifetime; source callers do not free them explicitly.
//
// Links: src/runtime/core/rt_dateonly.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID for DateOnly instances.
/// @details Stamped by rt_obj_new_i64 at construction and verified by the shared
///          receiver guard so an explicit receiver of another class traps instead
///          of being reinterpreted as a DateOnly payload (VDOC-229).
#define RT_DATEONLY_CLASS_ID INT64_C(-0x430803)

//=========================================================================
// DateOnly Creation
//=========================================================================

/// @brief Create a DateOnly from year, month, day.
/// @param year Four-digit-domain year in [0,9999].
/// @param month Month (1-12).
/// @param day Day (1-31).
/// @return Opaque DateOnly object pointer, or NULL for an invalid month/day.
void *rt_dateonly_create(int64_t year, int64_t month, int64_t day);

/// @brief Get today's date.
/// @return Opaque DateOnly object pointer, or NULL if local-time conversion fails.
void *rt_dateonly_today(void);

/// @brief Parse a date from exact ISO format string (YYYY-MM-DD).
/// @param s Date string with no trailing characters.
/// @return Opaque DateOnly object pointer, or NULL if invalid.
void *rt_dateonly_parse(rt_string s);

/// @brief Create from days since epoch (Jan 1, 1970).
/// @param days Days since epoch.
/// @return New DateOnly, or `NULL` if the date is outside years 0000–9999.
/// @note Traps on signed 64-bit conversion overflow.
void *rt_dateonly_from_days(int64_t days);

//=========================================================================
// Component Access
//=========================================================================

/// @brief Get the year component.
/// @param obj Opaque DateOnly object pointer.
/// @return Year (e.g., 2024).
int64_t rt_dateonly_year(void *obj);

/// @brief Get the month component.
/// @param obj Opaque DateOnly object pointer.
/// @return Month (1-12).
int64_t rt_dateonly_month(void *obj);

/// @brief Get the day component.
/// @param obj Opaque DateOnly object pointer.
/// @return Day (1-31).
int64_t rt_dateonly_day(void *obj);

/// @brief Get the day of week (0=Sunday, 6=Saturday).
/// @param obj Opaque DateOnly object pointer.
/// @return Day of week. Traps on signed 64-bit overflow for extreme years.
int64_t rt_dateonly_day_of_week(void *obj);

/// @brief Get the day of year (1-366).
/// @param obj Opaque DateOnly object pointer.
/// @return Day of year.
int64_t rt_dateonly_day_of_year(void *obj);

/// @brief Get days since epoch (Jan 1, 1970).
/// @param obj Opaque DateOnly object pointer.
/// @return Days since epoch. Traps on signed 64-bit overflow for extreme years.
int64_t rt_dateonly_to_days(void *obj);

//=========================================================================
// Date Arithmetic
//=========================================================================

/// @brief Add days to the date.
/// @param obj Opaque DateOnly object pointer.
/// @param days Number of days to add (can be negative).
/// @return New DateOnly, or `NULL` if the result leaves years 0000–9999.
/// @note Traps on signed 64-bit overflow.
void *rt_dateonly_add_days(void *obj, int64_t days);

/// @brief Add months to the date.
/// @param obj Opaque DateOnly object pointer.
/// @param months Number of months to add (can be negative).
/// @return New DateOnly with day clamped to the target month, or `NULL` if
///         the result leaves years 0000–9999.
/// @note Traps on signed 64-bit overflow.
void *rt_dateonly_add_months(void *obj, int64_t months);

/// @brief Add years to the date.
/// @param obj Opaque DateOnly object pointer.
/// @param years Number of years to add (can be negative).
/// @return New DateOnly, clamping February 29 when necessary, or `NULL` if
///         the result leaves years 0000–9999.
/// @note Traps on signed 64-bit overflow.
void *rt_dateonly_add_years(void *obj, int64_t years);

/// @brief Get the difference in days between two dates.
/// @param a First date.
/// @param b Second date.
/// @return Number of days (a - b). Traps on signed 64-bit overflow.
int64_t rt_dateonly_diff_days(void *a, void *b);

//=========================================================================
// Date Queries
//=========================================================================

/// @brief Check if the year is a leap year.
/// @param obj Opaque DateOnly object pointer.
/// @return 1 if leap year, 0 otherwise.
int8_t rt_dateonly_is_leap_year(void *obj);

/// @brief Get the number of days in the month.
/// @param obj Opaque DateOnly object pointer.
/// @return Number of days (28-31).
int64_t rt_dateonly_days_in_month(void *obj);

/// @brief Get the first day of the month.
/// @param obj Opaque DateOnly object pointer.
/// @return New DateOnly for first of month.
void *rt_dateonly_start_of_month(void *obj);

/// @brief Get the last day of the month.
/// @param obj Opaque DateOnly object pointer.
/// @return New DateOnly for last of month.
void *rt_dateonly_end_of_month(void *obj);

/// @brief Get the first day of the year.
/// @param obj Opaque DateOnly object pointer.
/// @return New DateOnly for Jan 1.
void *rt_dateonly_start_of_year(void *obj);

/// @brief Get the last day of the year.
/// @param obj Opaque DateOnly object pointer.
/// @return New DateOnly for Dec 31.
void *rt_dateonly_end_of_year(void *obj);

//=========================================================================
// Comparison
//=========================================================================

/// @brief Compare two dates.
/// @param a First date.
/// @param b Second date.
/// @return -1 if a < b, 0 if equal, 1 if a > b.
/// @note Null sorts before a valid date; two nulls compare equal.
int64_t rt_dateonly_cmp(void *a, void *b);

/// @brief Check equality of two dates.
/// @param a First date.
/// @param b Second date.
/// @return 1 if equal or both null, 0 otherwise.
int8_t rt_dateonly_equals(void *a, void *b);

//=========================================================================
// Formatting
//=========================================================================

/// @brief Convert to ISO format string (YYYY-MM-DD).
/// @param obj Opaque DateOnly object pointer.
/// @return New ISO string, or the shared empty string for null.
rt_string rt_dateonly_to_string(void *obj);

/// @brief Format using custom format string.
/// @param obj Opaque DateOnly object pointer.
/// @param fmt Format string (supports %Y, %y, %m, %d, %B, %b, %A, %a, %j, and %%).
/// @return New formatted string, shared empty string for null input, or `NULL`
///         after allocation failure.
/// @note Unknown conversion specifiers are preserved literally.
rt_string rt_dateonly_format(void *obj, rt_string fmt);

#ifdef __cplusplus
}
#endif
