//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_datetime.h
/// @file
/// @brief Declares Unix-second DateTime queries, formatting, parsing, and arithmetic.
///
// Purpose: Runtime date/time functions providing Unix timestamp queries, component extraction
// (year, month, day, hour, minute, second), formatting, and arithmetic operations for the
// Zanna.DateTime runtime class.
//
// Key invariants:
//   - All timestamps are Unix timestamps in seconds since the UTC epoch (1970-01-01 00:00:00).
//   - Component extraction functions use local time; rt_datetime_to_iso is the
//     fixed UTC formatter and rt_datetime_to_local uses local civil time.
//   - rt_datetime_now_ms returns wall-clock milliseconds since epoch; use Clock
//     or Stopwatch for elapsed-time measurement.
//   - rt_datetime_format delegates to host strftime; the explicit ToISO/ToLocal
//     helpers provide fixed layouts.
//
// Ownership/Lifetime:
//   - Returned strings are newly allocated and must be released by the caller.
//   - Integer timestamps are value types; no heap allocation for arithmetic.
//
// Links: src/runtime/core/rt_datetime.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Read the wall clock in whole seconds, distinguishing genuine failure.
/// @details Internal helper shared by DateTime.Now, DateOnly.Today, and the
/// RelativeTime current-time path so a `time(NULL)` failure (which returns
/// `(time_t)-1`, aliasing the valid pre-epoch instant) is detected via `errno`
/// rather than surfaced as a plausible 1969 timestamp (VDOC-230). Not a registered
/// runtime surface symbol.
/// @param out Receives current epoch seconds on success; untouched on failure.
/// @return 1 on success, 0 when the wall clock is unavailable.
/// @pre @p out is nonnull.
int rt_datetime_wall_seconds(int64_t *out);

/// @brief Get current Unix timestamp in seconds.
/// @return Seconds since Unix epoch (1970-01-01 00:00:00 UTC). Traps if the system
/// clock is unavailable rather than returning the ambiguous `-1` sentinel.
int64_t rt_datetime_now(void);

/// @brief Get current time in milliseconds.
/// @return Milliseconds since Unix epoch, or 0 on a platform clock-read failure.
/// @note Zero is also the valid Unix epoch; overflow raises a trap.
int64_t rt_datetime_now_ms(void);

/// @brief Extract year from timestamp.
/// @param timestamp Unix timestamp in seconds.
/// @return Year (e.g., 2025).
int64_t rt_datetime_year(int64_t timestamp);

/// @brief Extract month from timestamp.
/// @param timestamp Unix timestamp in seconds.
/// @return Month (1-12).
int64_t rt_datetime_month(int64_t timestamp);

/// @brief Extract day of month from timestamp.
/// @param timestamp Unix timestamp in seconds.
/// @return Day (1-31).
int64_t rt_datetime_day(int64_t timestamp);

/// @brief Extract hour from timestamp.
/// @param timestamp Unix timestamp in seconds.
/// @return Hour (0-23).
int64_t rt_datetime_hour(int64_t timestamp);

/// @brief Extract minute from timestamp.
/// @param timestamp Unix timestamp in seconds.
/// @return Minute (0-59).
int64_t rt_datetime_minute(int64_t timestamp);

/// @brief Extract second from timestamp.
/// @param timestamp Unix timestamp in seconds.
/// @return Second (0-59).
int64_t rt_datetime_second(int64_t timestamp);

/// @brief Get day of week from timestamp.
/// @param timestamp Unix timestamp in seconds.
/// @return Day of week (0=Sunday, 1=Monday, ..., 6=Saturday).
int64_t rt_datetime_day_of_week(int64_t timestamp);

/// @brief Format timestamp using strftime format string.
/// @param timestamp Unix timestamp in seconds.
/// @param format strftime-compatible format string.
/// @return New local-time formatted string, or a new empty string for invalid
///         timestamp/format, embedded null, or output-buffer exhaustion.
rt_string rt_datetime_format(int64_t timestamp, rt_string format);

/// @brief Convert timestamp to ISO 8601 format (UTC).
/// @param timestamp Unix timestamp in seconds.
/// @return ISO 8601 formatted string (e.g., "2025-12-05T14:30:00Z").
rt_string rt_datetime_to_iso(int64_t timestamp);

/// @brief Convert timestamp to local ISO 8601 format (no Z suffix).
/// @param timestamp Unix timestamp in seconds.
/// @return Local ISO 8601 formatted string (e.g., "2025-12-05T14:30:00").
rt_string rt_datetime_to_local(int64_t timestamp);

/// @brief Create timestamp from date/time components.
/// @param year Year (e.g., 2025).
/// @param month Month (1-12).
/// @param day Day (1-31).
/// @param hour Hour (0-23).
/// @param minute Minute (0-59).
/// @param second Second (0-59).
/// @return Unix timestamp in seconds, or -1 if components are invalid or the local time is not
/// representable. The sentinel collides with the valid instant one second before the epoch; use
/// @ref rt_datetime_create_option when failure must be distinguished from a pre-epoch result.
/// @note Local skipped-DST times are rejected; repeated-DST times follow the host's choice.
int64_t rt_datetime_create(
    int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute, int64_t second);

/// @brief Create timestamp from date/time components, returning an Option.
/// @details Unambiguous replacement for @ref rt_datetime_create: `Some(i64)` for any valid,
/// representable instant (including `-1`, one second before the epoch) and `None` on failure.
/// @param year Year (e.g., 2025).
/// @param month Month (1-12).
/// @param day Day (1-31).
/// @param hour Hour (0-23).
/// @param minute Minute (0-59).
/// @param second Second (0-59).
/// @return Opaque Zanna.Option holding the timestamp, or None when components are invalid or the
/// local time is not representable.
void *rt_datetime_create_option(
    int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute, int64_t second);

/// @brief Add seconds to timestamp.
/// @param timestamp Base timestamp in seconds.
/// @param seconds Seconds to add (can be negative).
/// @return New timestamp. Traps on signed 64-bit overflow.
int64_t rt_datetime_add_seconds(int64_t timestamp, int64_t seconds);

/// @brief Add days to timestamp.
/// @param timestamp Base timestamp in seconds.
/// @param days Days to add (can be negative).
/// @return New timestamp. Traps on signed 64-bit overflow.
/// @note Adds exactly 86400 seconds per day; this is not local-calendar arithmetic.
int64_t rt_datetime_add_days(int64_t timestamp, int64_t days);

/// @brief Calculate difference between two timestamps.
/// @param ts1 First timestamp in seconds.
/// @param ts2 Second timestamp in seconds.
/// @return Difference (ts1 - ts2) in seconds. Traps on signed 64-bit overflow.
int64_t rt_datetime_diff(int64_t ts1, int64_t ts2);

//=========================================================================
// Parsing Functions
//=========================================================================

/// @brief Parse an ISO 8601 datetime string to timestamp.
/// @param s Exact string like "2024-01-15T10:30:00", "2024-01-15T10:30:00Z",
///          or "2024-01-15T10:30:00.123+02:00".
/// @return Unix timestamp, or 0 on parse failure.
/// @note Fractions are truncated; a missing zone suffix means local time.
int64_t rt_datetime_parse_iso(rt_string s);

/// @brief Parse a date string to timestamp (midnight).
/// @param s Exact string like "2024-01-15".
/// @return Unix timestamp at midnight, or 0 on parse failure.
int64_t rt_datetime_parse_date(rt_string s);

/// @brief Parse a time string to seconds since midnight.
/// @param s Exact string like "10:30", "10:30:00", or "10:30:00.123".
/// @return Seconds since midnight, or -1 on parse failure.
/// @note Fractional seconds are accepted only after an explicit seconds field
///       and are truncated.
int64_t rt_datetime_parse_time(rt_string s);

/// @brief Try to parse a datetime string in any supported format.
/// @param s Input string (ISO, date-only, or time-only).
/// @return Epoch seconds for datetime/date input, seconds since midnight for
///         time-only input, or 0 on parse failure.
int64_t rt_datetime_try_parse(rt_string s);

/// @brief Try to parse a datetime string in any supported format as an Option.
/// @details Returns `Some(i64)` on success and `None` on failure, preserving a
///          valid Unix epoch timestamp (`0`) as a successful parse.
/// @param s Input string (ISO, date-only, or time-only).
/// @return New Option containing epoch seconds for datetime/date input or
///         seconds since midnight for time-only input; `None` on failure.
void *rt_datetime_try_parse_option(rt_string s);

#ifdef __cplusplus
}
#endif
