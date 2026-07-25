//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_datetime.c
/// @file
/// @brief Implements Unix-second DateTime queries, formatting, parsing, and arithmetic.
///
// Purpose: Implements the Zanna.DateTime class — wall-clock date/time
//          operations backed by Unix timestamps (seconds since epoch). Provides
//          current time query (NowMs/NowSec), component extraction (Year, Month,
//          Day, Hour, Minute, Second, DayOfWeek), arithmetic (AddDays, AddHours,
//          etc.), and ISO 8601 formatting.
//
// Key invariants:
//   - Timestamps are int64_t seconds since the Unix epoch (1970-01-01 00:00 UTC).
//   - Component extraction (Year, Month, Day, etc.) uses local time zone via
//     localtime_r / rt_localtime_r; ISO format output uses UTC (gmtime_r).
//   - rt_localtime_r and rt_gmtime_r are thread-safe wrappers storing results
//     in caller-provided struct tm buffers.
//   - DayOfWeek is 0-based starting from Sunday (0=Sun ... 6=Sat), matching
//     tm_wday convention.
//   - NowMs returns milliseconds since epoch (wall clock, may jump with NTP).
//
// Ownership/Lifetime:
//   - Formatted strings are newly allocated rt_string values; the caller owns
//     the reference and must call rt_string_unref when done.
//   - No heap allocation is performed by scalar accessors.
//
// Links: src/runtime/core/rt_datetime.h (public API),
//        src/runtime/core/rt_time.c (monotonic sleep and tick helpers),
//        src/runtime/core/rt_stopwatch.c (elapsed time measurement),
//        src/runtime/core/rt_dateonly.c (date-only type)
//
//===----------------------------------------------------------------------===//

#include "rt_datetime.h"
#include "rt_option.h"
#include "rt_platform.h"
#include "rt_trap.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if RT_PLATFORM_MACOS
#include <sys/time.h>
#endif

// Overflow-checked signed 64-bit arithmetic for DateTime epoch math. Same triplet
// found in rt_countdown / rt_duration / rt_dateonly — pre-checks operands before
// performing the operation to avoid signed-overflow UB.

/// @brief Overflow-checked signed 64-bit addition. Returns 1 on overflow.
/// @param a Left operand.
/// @param b Right operand.
/// @param out Receives the sum on success.
/// @return One on overflow; zero after writing the exact sum.
static int dt_checked_add_i64(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return 1;
    *out = a + b;
    return 0;
}

/// @brief Overflow-checked signed 64-bit subtraction. Returns 1 on overflow.
/// @param a Minuend.
/// @param b Subtrahend.
/// @param out Receives the difference on success.
/// @return One on overflow; zero after writing the exact difference.
static int dt_checked_sub_i64(int64_t a, int64_t b, int64_t *out) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
        return 1;
    *out = a - b;
    return 0;
}

/// @brief Overflow-checked signed 64-bit multiplication. Returns 1 on overflow.
/// @param a Left factor.
/// @param b Right factor.
/// @param out Receives the product on success.
/// @return One on overflow; zero after writing the exact product.
static int dt_checked_mul_i64(int64_t a, int64_t b, int64_t *out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, out);
#else
    if (a == 0 || b == 0) {
        *out = 0;
        return 0;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b)
                return 1;
        } else if (b < INT64_MIN / a) {
            return 1;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b)
                return 1;
        } else if (a < INT64_MAX / b) {
            return 1;
        }
    }
    *out = a * b;
    return 0;
#endif
}

/// @brief Narrow @p value into a `int`, returning 0 on out-of-range inputs.
/// @param value Signed 64-bit input.
/// @param out Receives the exactly represented native integer.
/// @return One on exact conversion, otherwise zero.
static int dt_i64_to_int(int64_t value, int *out) {
    if (value < INT_MIN || value > INT_MAX)
        return 0;
    *out = (int)value;
    return 1;
}

/// @brief Reconstructs the full civil year from a C `struct tm`.
/// @param tm Broken-down time whose `tm_year` is relative to 1900.
/// @return Signed full year.
static int64_t dt_tm_year_full(const struct tm *tm) {
    return (int64_t)tm->tm_year + 1900;
}

/// @brief Narrow @p value into a `time_t`, preserving exact representability.
/// @details `time_t` may be 32- or 64-bit depending on ABI; the round-trip cast detects
///          truncation and returns 0 in that case. On success, @p out holds the
///          converted value.
/// @param value Unix-second value to narrow.
/// @param out Receives the exact platform timestamp.
/// @return One on exact conversion, otherwise zero.
static int dt_i64_to_time_t(int64_t value, time_t *out) {
    time_t t = (time_t)value;
    if ((int64_t)t != value)
        return 0;
    *out = t;
    return 1;
}

/// @brief Widen a `time_t` to `int64_t`, preserving exact representability.
/// @details Inverse of `dt_i64_to_time_t`. On a 64-bit `time_t` the conversion is
///          always exact; on a 32-bit `time_t` the round-trip cast confirms the
///          conversion preserved the bit pattern.
/// @param value Platform timestamp to widen.
/// @param out Receives the exact signed 64-bit value.
/// @return One on exact conversion, otherwise zero.
static int dt_time_t_to_i64(time_t value, int64_t *out) {
    int64_t result = (int64_t)value;
    if ((time_t)result != value)
        return 0;
    *out = result;
    return 1;
}

/// @brief Checks a fully decomposed civil datetime for calendar/range validity.
/// @return One only when every field is in range.
static int dt_is_valid_datetime(int64_t year, int month, int day, int hour, int minute, int second);

/// @brief Convert validated local civil fields to epoch seconds without a `-1` collision.
/// @details Most C libraries allow the valid instant one second before the epoch
///          to round-trip through `mktime` and `localtime`. The Windows CRT instead
///          reports that instant as an error because its local-time conversion does
///          not accept negative timestamps. In that case, advancing the same valid
///          civil input by one second must map exactly to epoch zero; this uniquely
///          distinguishes the valid `-1` instant from an unrepresentable input.
/// @param value Mutable `struct tm` populated with the requested local fields.
/// @param year Full input year used for round-trip validation.
/// @param month One-based input month.
/// @param day One-based input day.
/// @param hour Input hour.
/// @param minute Input minute.
/// @param second Input second.
/// @param out Receives epoch seconds on success.
/// @return One when the local civil instant is valid/representable, otherwise zero.
static int dt_mktime_local_checked(struct tm *value,
                                   int64_t year,
                                   int month,
                                   int day,
                                   int hour,
                                   int minute,
                                   int second,
                                   int64_t *out) {
    if (!dt_is_valid_datetime(year, month, day, hour, minute, second))
        return 0;

    struct tm original = *value;
    errno = 0;
    time_t t = mktime(value);
    if (t == (time_t)-1 && errno != 0) {
        struct tm successor = original;
        successor.tm_sec += 1;
        successor.tm_isdst = -1;
        errno = 0;
        if (mktime(&successor) != (time_t)0)
            return 0;
        *out = -1;
        return 1;
    }

    struct tm check_buf;
    struct tm *check = rt_localtime_r(&t, &check_buf);
    if (!check)
        return 0;
    if (check->tm_year != original.tm_year || check->tm_mon != original.tm_mon ||
        check->tm_mday != original.tm_mday || check->tm_hour != original.tm_hour ||
        check->tm_min != original.tm_min || check->tm_sec != original.tm_sec)
        return 0;

    return dt_time_t_to_i64(t, out);
}

/// @brief Combine `(seconds, millis_part)` into a single epoch-millis `int64_t`.
/// @details Multiplies seconds by 1000 (checked) and adds the millis remainder
///          (checked). Either overflow path traps with `rt_trap_ovf()` rather than
///          silently wrapping. Used wherever DateTime input arrives as a split
///          seconds/millis pair (e.g. interop with system epoch APIs).
/// @param seconds Whole epoch seconds.
/// @param millis_part Millisecond remainder to add.
/// @return Combined epoch milliseconds, or zero after an overflow trap.
static int64_t dt_epoch_millis_from_parts(int64_t seconds, int64_t millis_part) {
    int64_t millis;
    int64_t result;
    if (dt_checked_mul_i64(seconds, 1000, &millis) ||
        dt_checked_add_i64(millis, millis_part, &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

/// @brief Read the wall clock in whole seconds, distinguishing genuine failure.
/// @details `time(NULL)` returns `(time_t)-1` on error, but `-1` is also the valid
///          instant one second before the Unix epoch, so a bare cast cannot tell a
///          clock failure from a 1969 timestamp (VDOC-230). POSIX sets `errno` on
///          failure, so this helper clears `errno` first and only treats `-1` as a
///          failure when `errno` was set. Shared by every current-time entry point
///          (DateTime.Now, DateOnly.Today, RelativeTime) so they all detect the
///          failure the same way instead of aliasing it to a plausible date.
/// @param out Receives the current epoch seconds on success; untouched on failure.
/// @return 1 on success, 0 when the wall clock is unavailable.
/// @pre @p out is nonnull.
int rt_datetime_wall_seconds(int64_t *out) {
    errno = 0;
    time_t t = time(NULL);
    if (t == (time_t)-1 && errno != 0)
        return 0;
    *out = (int64_t)t;
    return 1;
}

/// @brief Gets the current date/time as a Unix timestamp.
///
/// Returns the current time as the number of seconds elapsed since the Unix
/// epoch (January 1, 1970, 00:00:00 UTC). This timestamp can be used with
/// other DateTime functions to extract components or perform arithmetic.
///
/// **Usage example:**
/// ```
/// Dim now = DateTime.Now()
/// Print "Current timestamp: " & now
/// Print "Year: " & DateTime.Year(now)
/// Print "Month: " & DateTime.Month(now)
/// Print "Day: " & DateTime.Day(now)
/// ```
///
/// @return The current Unix timestamp in seconds. Traps if the system clock is
///         unavailable rather than returning `-1`, which would be indistinguishable
///         from the valid instant one second before the epoch (VDOC-230).
///
/// @note O(1) time complexity - single system call.
/// @note Resolution is seconds. Use rt_datetime_now_ms for milliseconds.
/// @note The timestamp is timezone-independent (represents an instant in time).
///
/// @see rt_datetime_now_ms For millisecond precision
/// @see rt_datetime_year For extracting the year component
int64_t rt_datetime_now(void) {
    int64_t now;
    if (!rt_datetime_wall_seconds(&now)) {
        rt_trap("DateTime.Now: system clock unavailable");
        return 0;
    }
    return now;
}

/// @brief Gets the current date/time as milliseconds since the Unix epoch.
///
/// Returns the current time with millisecond precision - the number of
/// milliseconds elapsed since the Unix epoch (January 1, 1970, 00:00:00 UTC).
/// This provides higher precision than rt_datetime_now() which only has
/// second resolution.
///
/// **Precision comparison:**
/// ```
/// DateTime.Now()   = 1703001600       (second precision)
/// DateTime.NowMs() = 1703001600123    (millisecond precision)
///                              ^^^
///                          milliseconds
/// ```
///
/// **Usage example:**
/// ```
/// Dim startMs = DateTime.NowMs()
/// ' ... do some work ...
/// Dim endMs = DateTime.NowMs()
/// Print "Operation took " & (endMs - startMs) & " milliseconds"
/// ```
///
/// **Note:** For performance timing, consider using Stopwatch instead, which
/// uses a monotonic clock that isn't affected by system time changes.
///
/// @return The current time in milliseconds since the Unix epoch.
///
/// @note O(1) time complexity - single system call.
/// @note Uses gettimeofday on macOS, clock_gettime on other platforms.
/// @note Unlike Stopwatch, this is wall-clock time and can be affected by
///       system time adjustments (NTP, manual changes, etc.).
/// @note macOS/POSIX clock-read failure returns zero, which is indistinguishable
///       from the Unix epoch; arithmetic overflow raises a trap.
///
/// @see rt_datetime_now For second-precision timestamps
/// @see rt_stopwatch.c For monotonic elapsed time measurement
int64_t rt_datetime_now_ms(void) {
#if RT_PLATFORM_WINDOWS
    return rt_windows_time_ms();
#elif RT_PLATFORM_MACOS
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
        return 0;
    return dt_epoch_millis_from_parts((int64_t)tv.tv_sec, (int64_t)tv.tv_usec / 1000);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return dt_epoch_millis_from_parts((int64_t)ts.tv_sec, (int64_t)ts.tv_nsec / 1000000);
#endif
}

/// @brief Extracts the year component from a timestamp.
///
/// Returns the four-digit year (e.g., 2023) from a Unix timestamp, interpreted
/// in the local time zone.
///
/// **Example:**
/// ```
/// Dim now = DateTime.Now()
/// Dim year = DateTime.Year(now)  ' e.g., 2023
/// Print "Current year: " & year
/// ```
///
/// @param timestamp Unix timestamp (seconds since epoch).
///
/// @return The year (e.g., 2023), or 0 if the timestamp is invalid.
///
/// @note Uses local time zone for conversion.
/// @note O(1) time complexity.
///
/// @see rt_datetime_month For extracting the month
/// @see rt_datetime_day For extracting the day
int64_t rt_datetime_year(int64_t timestamp) {
    time_t t;
    if (!dt_i64_to_time_t(timestamp, &t))
        return 0;
    struct tm tm_buf;
    struct tm *tm = rt_localtime_r(&t, &tm_buf);
    return tm ? dt_tm_year_full(tm) : 0;
}

/// @brief Extracts the month component from a timestamp.
///
/// Returns the month (1-12) from a Unix timestamp, interpreted in the local
/// time zone.
///
/// **Month values:**
/// | Value | Month     |
/// |-------|-----------|
/// | 1     | January   |
/// | 2     | February  |
/// | 3     | March     |
/// | ...   | ...       |
/// | 12    | December  |
///
/// **Example:**
/// ```
/// Dim now = DateTime.Now()
/// Dim month = DateTime.Month(now)  ' 1-12
/// Dim names() = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"}
/// Print "Current month: " & names(month - 1)
/// ```
///
/// @param timestamp Unix timestamp (seconds since epoch).
///
/// @return The month (1-12), or 0 if the timestamp is invalid.
///
/// @note Uses local time zone for conversion.
/// @note Returns 1-based month (January = 1), unlike C's tm_mon which is 0-based.
/// @note O(1) time complexity.
///
/// @see rt_datetime_year For extracting the year
/// @see rt_datetime_day For extracting the day
int64_t rt_datetime_month(int64_t timestamp) {
    time_t t;
    if (!dt_i64_to_time_t(timestamp, &t))
        return 0;
    struct tm tm_buf;
    struct tm *tm = rt_localtime_r(&t, &tm_buf);
    return tm ? (int64_t)(tm->tm_mon + 1) : 0;
}

/// @brief Extracts the day of month component from a timestamp.
///
/// Returns the day of the month (1-31) from a Unix timestamp, interpreted in
/// the local time zone.
///
/// **Example:**
/// ```
/// Dim now = DateTime.Now()
/// Dim day = DateTime.Day(now)  ' 1-31
/// Print "Today is day " & day & " of the month"
/// ```
///
/// @param timestamp Unix timestamp (seconds since epoch).
///
/// @return The day of month (1-31), or 0 if the timestamp is invalid.
///
/// @note Uses local time zone for conversion.
/// @note Valid range depends on the month (28-31 days).
/// @note O(1) time complexity.
///
/// @see rt_datetime_month For extracting the month
/// @see rt_datetime_day_of_week For getting the weekday
int64_t rt_datetime_day(int64_t timestamp) {
    time_t t;
    if (!dt_i64_to_time_t(timestamp, &t))
        return 0;
    struct tm tm_buf;
    struct tm *tm = rt_localtime_r(&t, &tm_buf);
    return tm ? (int64_t)tm->tm_mday : 0;
}

/// @brief Extracts the hour component from a timestamp.
///
/// Returns the hour (0-23) in 24-hour format from a Unix timestamp, interpreted
/// in the local time zone.
///
/// **Example:**
/// ```
/// Dim now = DateTime.Now()
/// Dim hour = DateTime.Hour(now)  ' 0-23
/// If hour < 12 Then
///     Print "Good morning!"
/// ElseIf hour < 17 Then
///     Print "Good afternoon!"
/// Else
///     Print "Good evening!"
/// End If
/// ```
///
/// @param timestamp Unix timestamp (seconds since epoch).
///
/// @return The hour (0-23), or 0 if the timestamp is invalid.
///
/// @note Uses local time zone for conversion.
/// @note Returns 24-hour format (0 = midnight, 12 = noon, 23 = 11 PM).
/// @note O(1) time complexity.
///
/// @see rt_datetime_minute For extracting minutes
/// @see rt_datetime_second For extracting seconds
int64_t rt_datetime_hour(int64_t timestamp) {
    time_t t;
    if (!dt_i64_to_time_t(timestamp, &t))
        return 0;
    struct tm tm_buf;
    struct tm *tm = rt_localtime_r(&t, &tm_buf);
    return tm ? (int64_t)tm->tm_hour : 0;
}

/// @brief Extracts the minute component from a timestamp.
///
/// Returns the minute (0-59) from a Unix timestamp, interpreted in the local
/// time zone.
///
/// **Example:**
/// ```
/// Dim now = DateTime.Now()
/// Dim h = DateTime.Hour(now)
/// Dim m = DateTime.Minute(now)
/// Print "Current time: " & h & ":" & Format(m, "00")
/// ```
///
/// @param timestamp Unix timestamp (seconds since epoch).
///
/// @return The minute (0-59), or 0 if the timestamp is invalid.
///
/// @note Uses local time zone for conversion.
/// @note O(1) time complexity.
///
/// @see rt_datetime_hour For extracting the hour
/// @see rt_datetime_second For extracting seconds
int64_t rt_datetime_minute(int64_t timestamp) {
    time_t t;
    if (!dt_i64_to_time_t(timestamp, &t))
        return 0;
    struct tm tm_buf;
    struct tm *tm = rt_localtime_r(&t, &tm_buf);
    return tm ? (int64_t)tm->tm_min : 0;
}

/// @brief Extracts the second component from a timestamp.
///
/// Returns the second (0-59) from a Unix timestamp, interpreted in the local
/// time zone. In rare cases during leap seconds, the value might be 60.
///
/// **Example:**
/// ```
/// Dim now = DateTime.Now()
/// Dim s = DateTime.Second(now)  ' 0-59
/// Print "Seconds: " & s
/// ```
///
/// @param timestamp Unix timestamp (seconds since epoch).
///
/// @return The second (0-59, rarely 60 for leap seconds), or 0 if invalid.
///
/// @note Uses local time zone for conversion.
/// @note O(1) time complexity.
///
/// @see rt_datetime_minute For extracting minutes
/// @see rt_datetime_hour For extracting the hour
int64_t rt_datetime_second(int64_t timestamp) {
    time_t t;
    if (!dt_i64_to_time_t(timestamp, &t))
        return 0;
    struct tm tm_buf;
    struct tm *tm = rt_localtime_r(&t, &tm_buf);
    return tm ? (int64_t)tm->tm_sec : 0;
}

/// @brief Extracts the day of week from a timestamp.
///
/// Returns the day of the week (0-6) from a Unix timestamp, interpreted in
/// the local time zone. Sunday is 0, Saturday is 6.
///
/// **Day of week values:**
/// | Value | Day       |
/// |-------|-----------|
/// | 0     | Sunday    |
/// | 1     | Monday    |
/// | 2     | Tuesday   |
/// | 3     | Wednesday |
/// | 4     | Thursday  |
/// | 5     | Friday    |
/// | 6     | Saturday  |
///
/// **Example:**
/// ```
/// Dim now = DateTime.Now()
/// Dim dow = DateTime.DayOfWeek(now)
/// Dim names() = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"}
/// Print "Today is " & names(dow)
///
/// If dow = 0 Or dow = 6 Then
///     Print "It's the weekend!"
/// Else
///     Print "It's a weekday."
/// End If
/// ```
///
/// @param timestamp Unix timestamp (seconds since epoch).
///
/// @return The day of week (0 = Sunday, 6 = Saturday), or 0 if invalid.
///
/// @note Uses local time zone for conversion.
/// @note O(1) time complexity.
///
/// @see rt_datetime_day For the day of month
int64_t rt_datetime_day_of_week(int64_t timestamp) {
    time_t t;
    if (!dt_i64_to_time_t(timestamp, &t))
        return 0;
    struct tm tm_buf;
    struct tm *tm = rt_localtime_r(&t, &tm_buf);
    return tm ? (int64_t)tm->tm_wday : 0;
}

/// @brief Formats a timestamp using a strftime format string.
///
/// Converts a Unix timestamp to a formatted string representation using the
/// C strftime() format specifiers. This provides flexible, locale-aware date
/// and time formatting.
///
/// **Common format specifiers:**
/// | Specifier | Description              | Example    |
/// |-----------|--------------------------|------------|
/// | %Y        | 4-digit year             | 2023       |
/// | %m        | Month (01-12)            | 12         |
/// | %d        | Day of month (01-31)     | 19         |
/// | %H        | Hour 24h (00-23)         | 14         |
/// | %M        | Minute (00-59)           | 30         |
/// | %S        | Second (00-59)           | 45         |
/// | %I        | Hour 12h (01-12)         | 02         |
/// | %p        | AM/PM                    | PM         |
/// | %A        | Full weekday name        | Tuesday    |
/// | %a        | Abbreviated weekday      | Tue        |
/// | %B        | Full month name          | December   |
/// | %b        | Abbreviated month        | Dec        |
/// | %%        | Literal %                | %          |
///
/// **Example:**
/// ```
/// Dim now = DateTime.Now()
///
/// ' Standard formats
/// Print DateTime.Format(now, "%Y-%m-%d")           ' 2023-12-19
/// Print DateTime.Format(now, "%H:%M:%S")           ' 14:30:45
/// Print DateTime.Format(now, "%Y-%m-%d %H:%M:%S")  ' 2023-12-19 14:30:45
///
/// ' Human-readable
/// Print DateTime.Format(now, "%A, %B %d, %Y")      ' Tuesday, December 19, 2023
/// Print DateTime.Format(now, "%I:%M %p")           ' 02:30 PM
/// ```
///
/// @param timestamp Unix timestamp (seconds since epoch).
/// @param format Zanna string containing strftime format specifiers.
///
/// @return Formatted date/time string, or empty string if:
///         - The timestamp is invalid
///         - The format string is NULL or empty
///         - The formatted output exceeds 256 characters
///
/// @note Uses local time zone for conversion.
/// @note Format specifiers are locale-dependent (month/day names).
/// @note Maximum output length is 256 characters.
/// @note O(1) time complexity.
///
/// @see rt_datetime_to_iso For ISO 8601 format (UTC)
rt_string rt_datetime_format(int64_t timestamp, rt_string format) {
    time_t t;
    if (!dt_i64_to_time_t(timestamp, &t))
        return rt_string_from_bytes("", 0);
    struct tm tm_buf;
    struct tm *tm = rt_localtime_r(&t, &tm_buf);
    if (!tm) {
        return rt_string_from_bytes("", 0);
    }

    if (!format)
        return rt_string_from_bytes("", 0);
    int64_t fmt_len64 = rt_str_len(format);
    if (fmt_len64 <= 0 || (uint64_t)fmt_len64 > (uint64_t)SIZE_MAX)
        return rt_string_from_bytes("", 0);

    const char *fmt_cstr = rt_string_cstr(format);
    if (!fmt_cstr || memchr(fmt_cstr, '\0', (size_t)fmt_len64) != NULL) {
        return rt_string_from_bytes("", 0);
    }

    // Format the time
    char buffer[256];
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    size_t len = strftime(buffer, sizeof(buffer), fmt_cstr, tm);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

    if (len == 0) {
        return rt_string_from_bytes("", 0);
    }

    return rt_string_from_bytes(buffer, len);
}

/// @brief Converts a timestamp to ISO 8601 format (UTC).
///
/// Formats a Unix timestamp as an ISO 8601 date/time string in UTC. This
/// format is widely used for data interchange, APIs, and logging because
/// it's unambiguous and machine-parseable.
///
/// **Format:** `YYYY-MM-DDTHH:MM:SSZ`
///
/// Where:
/// - `YYYY`: 4-digit year
/// - `MM`: 2-digit month (01-12)
/// - `DD`: 2-digit day (01-31)
/// - `T`: Literal separator between date and time
/// - `HH`: 2-digit hour in 24h format (00-23)
/// - `MM`: 2-digit minute (00-59)
/// - `SS`: 2-digit second (00-59)
/// - `Z`: Indicates UTC (Zulu time)
///
/// **Example:**
/// ```
/// Dim now = DateTime.Now()
/// Dim iso = DateTime.ToISO(now)
/// Print iso  ' "2023-12-19T14:30:45Z"
///
/// ' Use for API calls
/// Dim json = "{""timestamp"": """ & iso & """}"
/// ```
///
/// @param timestamp Unix timestamp (seconds since epoch).
///
/// @return ISO 8601 formatted string in UTC, or empty string if invalid.
///
/// @note Always uses UTC (not local time) - the 'Z' suffix indicates this.
/// @note Does not include milliseconds (only second precision).
/// @note O(1) time complexity.
///
/// @see rt_datetime_format For custom formatting in local time
/// @see rt_datetime_create For creating timestamps from components
rt_string rt_datetime_to_iso(int64_t timestamp) {
    time_t t;
    if (!dt_i64_to_time_t(timestamp, &t))
        return rt_string_from_bytes("", 0);
    struct tm tm_buf;
    struct tm *tm = rt_gmtime_r(&t, &tm_buf); // Use UTC for ISO format
    if (!tm) {
        return rt_string_from_bytes("", 0);
    }

    char buffer[64];
    int len = snprintf(buffer,
                       sizeof(buffer),
                       "%04lld-%02d-%02dT%02d:%02d:%02dZ",
                       (long long)dt_tm_year_full(tm),
                       tm->tm_mon + 1,
                       tm->tm_mday,
                       tm->tm_hour,
                       tm->tm_min,
                       tm->tm_sec);

    if (len < 0) {
        return rt_string_from_bytes("", 0);
    }
    if ((size_t)len >= sizeof(buffer)) {
        rt_trap("DateTime.ToISO: formatted output truncated");
        return rt_string_from_bytes("", 0);
    }
    return rt_string_from_bytes(buffer, (size_t)len);
}

/// @brief Converts a timestamp to local ISO 8601 format (no Z suffix).
///
/// Like rt_datetime_to_iso but uses local time instead of UTC.
/// Format: YYYY-MM-DDTHH:MM:SS
///
/// @param timestamp Unix timestamp (seconds since epoch).
/// @return Local ISO 8601 formatted string, or empty string if invalid.
rt_string rt_datetime_to_local(int64_t timestamp) {
    time_t t;
    if (!dt_i64_to_time_t(timestamp, &t))
        return rt_string_from_bytes("", 0);
    struct tm tm_buf;
    struct tm *tm = rt_localtime_r(&t, &tm_buf);
    if (!tm) {
        return rt_string_from_bytes("", 0);
    }

    char buffer[64];
    int len = snprintf(buffer,
                       sizeof(buffer),
                       "%04lld-%02d-%02dT%02d:%02d:%02d",
                       (long long)dt_tm_year_full(tm),
                       tm->tm_mon + 1,
                       tm->tm_mday,
                       tm->tm_hour,
                       tm->tm_min,
                       tm->tm_sec);

    if (len < 0) {
        return rt_string_from_bytes("", 0);
    }
    if ((size_t)len >= sizeof(buffer)) {
        rt_trap("DateTime.ToLocal: formatted output truncated");
        return rt_string_from_bytes("", 0);
    }
    return rt_string_from_bytes(buffer, (size_t)len);
}

/// @brief Creates a Unix timestamp from date/time components.
///
/// Combines year, month, day, hour, minute, and second components into a
/// Unix timestamp. The components are interpreted in the local time zone.
///
/// **Parameter ranges:**
/// | Parameter | Range       | Notes                    |
/// |-----------|-------------|--------------------------|
/// | year      | 1970-2038+  | Full 4-digit year        |
/// | month     | 1-12        | January = 1              |
/// | day       | 1-31        | Depends on month         |
/// | hour      | 0-23        | 24-hour format           |
/// | minute    | 0-59        |                          |
/// | second    | 0-59        |                          |
///
/// **Example:**
/// ```
/// ' Create timestamp for Christmas 2023 at noon
/// Dim christmas = DateTime.Create(2023, 12, 25, 12, 0, 0)
/// Print DateTime.ToISO(christmas)  ' "2023-12-25T..."
///
/// ' Calculate days until an event
/// Dim now = DateTime.Now()
/// Dim diff = DateTime.Diff(christmas, now)
/// Print "Days until Christmas: " & (diff / 86400)
/// ```
///
/// **Validation handling:**
/// The function rejects out-of-range or normalized component values.
/// Examples rejected with -1 include month 13, day 32, invalid leap days,
/// hour 24, minute 60, and second 60.
///
/// @param year The year (e.g., 2023).
/// @param month The month (1-12, where 1 = January).
/// @param day The day of month (1-31).
/// @param hour The hour in 24-hour format (0-23).
/// @param minute The minute (0-59).
/// @param second The second (0-59).
///
/// @return Unix timestamp representing the specified date/time, or -1 if
///         the date cannot be represented.
///
/// @note Uses local time zone for interpretation.
/// @note Automatically handles daylight saving time transitions.
/// @note O(1) time complexity.
///
/// @brief Shared core for the sentinel and Option `Create` forms.
/// @details Performs the full range check, `mktime` conversion, and round-trip
///          field verification, writing the resulting instant to @p out. Kept
///          separate from the public entry points so both the legacy sentinel
///          form and the unambiguous Option form apply identical validation and
///          neither has to encode success in the returned instant (VDOC-225).
/// @param out Receives the Unix timestamp on success; untouched on failure.
/// @return 1 when the civil input is valid and representable, 0 otherwise.
static int dt_create_impl(int64_t year,
                          int64_t month,
                          int64_t day,
                          int64_t hour,
                          int64_t minute,
                          int64_t second,
                          int64_t *out) {
    int64_t year_offset;
    int64_t month_offset;
    if (dt_checked_sub_i64(year, 1900, &year_offset))
        return 0;
    if (dt_checked_sub_i64(month, 1, &month_offset))
        return 0;

    int tm_year;
    int tm_mon;
    int tm_mday;
    int tm_hour;
    int tm_min;
    int tm_sec;
    if (!dt_i64_to_int(year_offset, &tm_year) || !dt_i64_to_int(month_offset, &tm_mon) ||
        !dt_i64_to_int(day, &tm_mday) || !dt_i64_to_int(hour, &tm_hour) ||
        !dt_i64_to_int(minute, &tm_min) || !dt_i64_to_int(second, &tm_sec))
        return 0;

    struct tm tm = {0};
    tm.tm_year = tm_year;
    tm.tm_mon = tm_mon;
    tm.tm_mday = tm_mday;
    tm.tm_hour = tm_hour;
    tm.tm_min = tm_min;
    tm.tm_sec = tm_sec;
    tm.tm_isdst = -1; // Let the system determine DST

    return dt_mktime_local_checked(&tm, year, tm_mon + 1, tm_mday, tm_hour, tm_min, tm_sec, out);
}

/// @see rt_datetime_year For extracting components from a timestamp
/// @see rt_datetime_to_iso For formatting timestamps
/// @see rt_datetime_create_option For the unambiguous Option-returning form
/// @brief Creates a local-time Unix timestamp from validated civil components.
/// @param year Full local civil year.
/// @param month One-based month in 1–12.
/// @param day Valid day for @p year and @p month.
/// @param hour Hour in 0–23.
/// @param minute Minute in 0–59.
/// @param second Second in 0–59; leap-second value 60 is rejected.
/// @return Representable Unix seconds, or `-1` on failure.
/// @note Legacy sentinel form: `-1` marks failure but is also a valid instant
///       (one second before the Unix epoch), so callers that must distinguish
///       failure from a pre-epoch result should use @ref rt_datetime_create_option.
int64_t rt_datetime_create(
    int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute, int64_t second) {
    int64_t result;
    return dt_create_impl(year, month, day, hour, minute, second, &result) ? result : -1;
}

/// @brief Create a Unix timestamp from civil components, returning an Option.
/// @details Unambiguous replacement for the legacy sentinel @ref rt_datetime_create:
///          returns `Some(i64)` for any valid, representable instant — including
///          `-1`, the valid instant one second before the Unix epoch — and `None`
///          for out-of-range, normalized, skipped-DST, or unrepresentable input,
///          so callers can distinguish failure from a genuine pre-epoch result
///          (VDOC-225). Components are interpreted in the local time zone,
///          exactly as `Create`.
/// @param year Full local civil year.
/// @param month One-based month in 1–12.
/// @param day Valid day for @p year and @p month.
/// @param hour Hour in 0–23.
/// @param minute Minute in 0–59.
/// @param second Second in 0–59.
/// @return Opaque Zanna.Option holding the timestamp, or None on failure.
void *rt_datetime_create_option(
    int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute, int64_t second) {
    int64_t result;
    return dt_create_impl(year, month, day, hour, minute, second, &result)
               ? rt_option_some_i64(result)
               : rt_option_none();
}

/// @brief Adds seconds to a timestamp.
///
/// Returns a new timestamp that is the specified number of seconds later
/// (or earlier, if seconds is negative) than the input timestamp.
///
/// **Example:**
/// ```
/// Dim now = DateTime.Now()
///
/// ' 1 hour later
/// Dim later = DateTime.AddSeconds(now, 3600)
///
/// ' 30 minutes earlier
/// Dim earlier = DateTime.AddSeconds(now, -1800)
///
/// ' 1 week later
/// Dim nextWeek = DateTime.AddSeconds(now, 7 * 24 * 60 * 60)
/// ```
///
/// **Common time intervals in seconds:**
/// | Duration  | Seconds    |
/// |-----------|------------|
/// | 1 minute  | 60         |
/// | 1 hour    | 3,600      |
/// | 1 day     | 86,400     |
/// | 1 week    | 604,800    |
///
/// @param timestamp Unix timestamp to add to.
/// @param seconds Number of seconds to add (can be negative).
///
/// @return New timestamp offset by the specified seconds.
///
/// @note O(1) time complexity - simple addition.
/// @note Traps on signed 64-bit overflow.
///
/// @see rt_datetime_add_days For adding days
/// @see rt_datetime_diff For calculating time differences
int64_t rt_datetime_add_seconds(int64_t timestamp, int64_t seconds) {
    int64_t result;
    if (dt_checked_add_i64(timestamp, seconds, &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

/// @brief Adds days to a timestamp.
///
/// Returns a new timestamp that is the specified number of days later
/// (or earlier, if days is negative) than the input timestamp. One day
/// is exactly 86,400 seconds.
///
/// **Example:**
/// ```
/// Dim today = DateTime.Now()
///
/// ' Tomorrow
/// Dim tomorrow = DateTime.AddDays(today, 1)
///
/// ' Yesterday
/// Dim yesterday = DateTime.AddDays(today, -1)
///
/// ' One month from now (approximately)
/// Dim nextMonth = DateTime.AddDays(today, 30)
/// ```
///
/// @param timestamp Unix timestamp to add to.
/// @param days Number of days to add (can be negative).
///
/// @return New timestamp offset by the specified days.
///
/// @note O(1) time complexity - simple multiplication and addition.
/// @note Does not account for daylight saving time transitions (always adds
///       exactly 86,400 seconds per day). For calendar-aware day arithmetic,
///       use DateTime.Create with adjusted day values.
/// @note Traps on signed 64-bit overflow.
///
/// @see rt_datetime_add_seconds For adding arbitrary time intervals
/// @see rt_datetime_diff For calculating time differences
int64_t rt_datetime_add_days(int64_t timestamp, int64_t days) {
    int64_t seconds;
    int64_t result;
    if (dt_checked_mul_i64(days, 86400, &seconds) ||
        dt_checked_add_i64(timestamp, seconds, &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

/// @brief Calculates the difference between two timestamps in seconds.
///
/// Returns the number of seconds between two timestamps (ts1 - ts2). A positive
/// result means ts1 is later than ts2; negative means ts1 is earlier.
///
///
/// **Example:**
/// ```
/// Dim start = DateTime.Now()
/// ' ... do some work ...
/// Dim finish = DateTime.Now()
///
/// Dim elapsed = DateTime.Diff(finish, start)
/// Print "Operation took " & elapsed & " seconds"
///
/// ' Convert to other units
/// Dim minutes = elapsed / 60
/// Dim hours = elapsed / 3600
/// Dim days = elapsed / 86400
/// ```
///
/// **Age calculation example:**
/// ```
/// Dim birthdate = DateTime.Create(1990, 6, 15, 0, 0, 0)
/// Dim now = DateTime.Now()
/// Dim ageSeconds = DateTime.Diff(now, birthdate)
/// Dim ageYears = ageSeconds / (365.25 * 86400)
/// Print "Age: " & Int(ageYears) & " years"
/// ```
///
/// @param ts1 First timestamp (minuend).
/// @param ts2 Second timestamp (subtrahend).
///
/// @return Difference in seconds (ts1 - ts2). Positive if ts1 > ts2,
///         negative if ts1 < ts2, zero if equal.
///
/// @note O(1) time complexity - simple subtraction.
/// @note The order of parameters matters: Diff(later, earlier) gives positive.
///
/// @see rt_datetime_add_seconds For the inverse operation
/// @see rt_datetime_add_days For adding day intervals
int64_t rt_datetime_diff(int64_t ts1, int64_t ts2) {
    int64_t result;
    if (dt_checked_sub_i64(ts1, ts2, &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

//=============================================================================
// Parsing Functions
//=============================================================================

/// @brief Helper to check if a character is a digit.
/// @param c Byte to classify.
/// @return Nonzero only for ASCII `0` through `9`.
static int dt_is_digit(char c) {
    return c >= '0' && c <= '9';
}

/// @brief Borrow a runtime string as bytes, rejecting embedded NULs for C-style parsers.
/// @param s Runtime string to inspect.
/// @param len_out Receives the byte length on success.
/// @return Borrowed null-terminated data, or `NULL` for invalid arguments,
///         unrepresentable length, unavailable data, or an embedded null byte.
static const char *dt_cstr_without_embedded_nul(rt_string s, size_t *len_out) {
    if (!s || !len_out)
        return NULL;
    int64_t len64 = rt_str_len(s);
    if (len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX)
        return NULL;
    const char *str = rt_string_cstr(s);
    if (!str && len64 > 0)
        return NULL;
    size_t len = (size_t)len64;
    if (str && memchr(str, '\0', len) != NULL)
        return NULL;
    *len_out = len;
    return str ? str : "";
}

/// @brief Helper to parse exactly N digits from a string.
/// @param s First candidate digit.
/// @param limit One-past-end byte bound.
/// @param n Exact number of digits required.
/// @param end Receives the first byte after the parsed digits.
/// @return Parsed nonnegative integer, or -1 for insufficient/non-digit input.
static int dt_parse_digits(const char *s, const char *limit, int n, const char **end) {
    int val = 0;
    for (int i = 0; i < n; ++i) {
        if (s + i >= limit || !dt_is_digit(s[i]))
            return -1;
        val = val * 10 + (s[i] - '0');
    }
    *end = s + n;
    return val;
}

/// @brief Consume an optional fractional seconds suffix. If '.' is present, at least one digit is
/// required. Fractions are truncated because DateTime stores whole seconds.
/// @param p Address of the current parser cursor, advanced past the fraction.
/// @param limit One-past-end byte bound.
/// @return One if no fraction exists or a valid fraction is consumed; otherwise zero.
static int dt_parse_optional_fraction(const char **p, const char *limit) {
    if (*p >= limit || **p != '.')
        return 1;
    (*p)++;
    if (*p >= limit || !dt_is_digit(**p))
        return 0;
    while (*p < limit && dt_is_digit(**p))
        (*p)++;
    return 1;
}

/// @brief Gregorian leap-year predicate.
/// @param year Proleptic Gregorian year.
/// @return Nonzero for a leap year.
static int dt_is_leap_year(int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/// @brief Number of days in @p month (1–12) of @p year, with leap-year adjustment.
/// @details Out-of-range months return 0 so callers can detect bad input via the
///          zero return.
/// @param year Gregorian year used for February.
/// @param month One-based month number.
/// @return 28–31 for a valid month, otherwise zero.
static int dt_days_in_month(int64_t year, int month) {
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12)
        return 0;
    if (month == 2 && dt_is_leap_year(year))
        return 29;
    return days[month];
}

/// @brief Validate a fully-decomposed civil datetime against calendar bounds.
/// @details Checks month-aware day-of-month upper bounds and the standard 0-23 / 0-59
///          / 0-59 ranges for hour/minute/second. Returns 1 only when every component
///          is in range. Note: leap seconds (`second == 60`) are not accepted.
/// @param year Gregorian year.
/// @param month One-based month.
/// @param day One-based day of month.
/// @param hour Hour in 0–23.
/// @param minute Minute in 0–59.
/// @param second Second in 0–59.
/// @return One for valid components, otherwise zero.
static int dt_is_valid_datetime(
    int64_t year, int month, int day, int hour, int minute, int second) {
    int max_day = dt_days_in_month(year, month);
    return max_day > 0 && day >= 1 && day <= max_day && hour >= 0 && hour <= 23 && minute >= 0 &&
           minute <= 59 && second >= 0 && second <= 59;
}

/// @brief Convert civil (year, month, day) UTC to days-since-Unix-epoch with overflow check.
/// @details Implements Howard Hinnant's `days_from_civil` algorithm with checked arithmetic.
///          Returns 1 with the result in @p out on success, 0 on overflow without writing.
/// @param year Proleptic Gregorian year.
/// @param month One-based month.
/// @param day One-based day.
/// @param out Receives the signed epoch-day offset.
/// @return One on success, otherwise zero.
static int dt_days_from_civil_utc(int64_t year, int64_t month, int64_t day, int64_t *out) {
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    uint64_t yoe = (uint64_t)(year - era * 400);
    uint64_t doy = (uint64_t)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    int64_t era_days;
    int64_t total;
    if (dt_checked_mul_i64(era, 146097, &era_days) ||
        dt_checked_add_i64(era_days, (int64_t)doe, &total) ||
        dt_checked_sub_i64(total, 719468, out))
        return 0;
    return 1;
}

/// @brief Compose a civil datetime into a UTC Unix-epoch second count, with overflow check.
/// @details Resolves the date through `dt_days_from_civil_utc`, multiplies up to seconds
///          (`* 86400`), then adds hour/minute/second offsets. Each step uses checked
///          arithmetic so overflow is reported via the 0 return rather than wrapping.
/// @param year Gregorian year.
/// @param month One-based month.
/// @param day One-based day.
/// @param hour Hour in 0–23.
/// @param minute Minute in 0–59.
/// @param second Second in 0–59.
/// @param out Receives Unix epoch seconds.
/// @return One on success, zero on arithmetic overflow.
static int dt_make_utc_timestamp(
    int year, int month, int day, int hour, int minute, int second, int64_t *out) {
    int64_t days;
    if (!dt_days_from_civil_utc(year, month, day, &days))
        return 0;

    int64_t day_seconds;
    int64_t hour_seconds;
    int64_t minute_seconds;
    int64_t total;
    if (dt_checked_mul_i64(days, 86400, &day_seconds) ||
        dt_checked_mul_i64(hour, 3600, &hour_seconds) ||
        dt_checked_add_i64(day_seconds, hour_seconds, &total) ||
        dt_checked_mul_i64(minute, 60, &minute_seconds) ||
        dt_checked_add_i64(total, minute_seconds, &total) || dt_checked_add_i64(total, second, out))
        return 0;
    return 1;
}

/// @brief Compose a civil datetime into a local-zone Unix-epoch second count.
/// @details Routes through `mktime` so DST/timezone rules apply automatically. The
///          round-trip through `localtime_r` validates that every output field matches
///          the input, rejecting skipped local hours that `mktime` normalizes. A repeated
///          hour can round-trip through either occurrence, so `tm_isdst = -1` leaves that
///          choice to the host implementation (VDOC-226).
/// @param year Local civil year.
/// @param month One-based local month.
/// @param day One-based local day.
/// @param hour Local hour.
/// @param minute Local minute.
/// @param second Local second.
/// @param out Receives Unix epoch seconds.
/// @return One when the local civil instant is valid and representable.
static int dt_make_local_timestamp(
    int year, int month, int day, int hour, int minute, int second, int64_t *out) {
    struct tm tm = {0};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;

    return dt_mktime_local_checked(&tm, year, month, day, hour, minute, second, out);
}

/// @brief Parse an ISO-8601-like datetime string into an epoch-seconds `int64_t`.
/// @details Accepts `YYYY-MM-DD[T| ]HH:MM:SS[.fff][±HH:MM]` and `Z` UTC marker. Returns
///          1 with @p out populated on success, 0 on any malformed input. Used by the
///          DateTime parser entry points.
/// @param s Exact input string with no embedded nulls or trailing bytes.
/// @param out Receives Unix epoch seconds.
/// @return One on success, otherwise zero.
static int dt_parse_iso_impl(rt_string s, int64_t *out) {
    size_t len = 0;
    const char *str = dt_cstr_without_embedded_nul(s, &len);
    if (!str || !out)
        return 0;

    const char *p = str;
    const char *limit = str + len;
    const char *end;

    int year = dt_parse_digits(p, limit, 4, &end);
    if (year < 0 || end >= limit || *end != '-')
        return 0;
    p = end + 1;

    int month = dt_parse_digits(p, limit, 2, &end);
    if (month < 0 || end >= limit || *end != '-')
        return 0;
    p = end + 1;

    int day = dt_parse_digits(p, limit, 2, &end);
    if (day < 0)
        return 0;
    p = end;

    if (p >= limit || (*p != 'T' && *p != 't' && *p != ' '))
        return 0;
    p++;

    int hour = dt_parse_digits(p, limit, 2, &end);
    if (hour < 0 || end >= limit || *end != ':')
        return 0;
    p = end + 1;

    int minute = dt_parse_digits(p, limit, 2, &end);
    if (minute < 0 || end >= limit || *end != ':')
        return 0;
    p = end + 1;

    int second = dt_parse_digits(p, limit, 2, &end);
    if (second < 0)
        return 0;
    p = end;

    if (!dt_parse_optional_fraction(&p, limit))
        return 0;

    int is_utc = 0;
    int offset_seconds = 0;
    if (p < limit && (*p == 'Z' || *p == 'z')) {
        is_utc = 1;
        p++;
    } else if (p < limit && (*p == '+' || *p == '-')) {
        int sign = *p == '-' ? -1 : 1;
        int offset_hour;
        int offset_minute;
        p++;
        offset_hour = dt_parse_digits(p, limit, 2, &end);
        if (offset_hour < 0 || end >= limit || *end != ':')
            return 0;
        p = end + 1;
        offset_minute = dt_parse_digits(p, limit, 2, &end);
        if (offset_minute < 0)
            return 0;
        if (offset_hour > 14 || (offset_hour == 14 && offset_minute != 0) || offset_minute > 59)
            return 0;
        p = end;
        offset_seconds = sign * (offset_hour * 3600 + offset_minute * 60);
        is_utc = 1;
    }
    if (p != limit)
        return 0;

    if (!dt_is_valid_datetime(year, month, day, hour, minute, second))
        return 0;

    if (is_utc) {
        int64_t utc_value;
        if (!dt_make_utc_timestamp(year, month, day, hour, minute, second, &utc_value))
            return 0;
        if (offset_seconds != 0 && dt_checked_sub_i64(utc_value, offset_seconds, &utc_value))
            return 0;
        *out = utc_value;
        return 1;
    }
    return dt_make_local_timestamp(year, month, day, hour, minute, second, out);
}

/// @brief Parse `YYYY-MM-DD` into an epoch-seconds value (local midnight).
/// @details Returns 1 with @p out populated on success, 0 on malformed input.
/// @param s Exact ten-byte date string.
/// @param out Receives local-midnight Unix epoch seconds.
/// @return One on success, otherwise zero.
static int dt_parse_date_impl(rt_string s, int64_t *out) {
    size_t len = 0;
    const char *str = dt_cstr_without_embedded_nul(s, &len);
    if (!str || !out)
        return 0;

    const char *p = str;
    const char *limit = str + len;
    const char *end;

    int year = dt_parse_digits(p, limit, 4, &end);
    if (year < 0 || end >= limit || *end != '-')
        return 0;
    p = end + 1;

    int month = dt_parse_digits(p, limit, 2, &end);
    if (month < 0 || end >= limit || *end != '-')
        return 0;
    p = end + 1;

    int day = dt_parse_digits(p, limit, 2, &end);
    if (day < 0)
        return 0;
    p = end;
    if (p != limit)
        return 0;

    if (!dt_is_valid_datetime(year, month, day, 0, 0, 0))
        return 0;
    return dt_make_local_timestamp(year, month, day, 0, 0, 0, out);
}

/// @brief Parse `HH:MM[:SS[.fff]]` into seconds since midnight.
/// @details Returns 1 with @p out populated on success, 0 on malformed input.
/// @param s Exact time string.
/// @param out Receives a value in 0–86399.
/// @return One on success, otherwise zero.
static int dt_parse_time_impl(rt_string s, int64_t *out) {
    size_t len = 0;
    const char *str = dt_cstr_without_embedded_nul(s, &len);
    if (!str || !out)
        return 0;

    const char *p = str;
    const char *limit = str + len;
    const char *end;

    int hour = dt_parse_digits(p, limit, 2, &end);
    if (hour < 0 || end >= limit || *end != ':')
        return 0;
    p = end + 1;

    int minute = dt_parse_digits(p, limit, 2, &end);
    if (minute < 0)
        return 0;
    p = end;

    int second = 0;
    if (p < limit && *p == ':') {
        p++;
        second = dt_parse_digits(p, limit, 2, &end);
        if (second < 0)
            return 0;
        p = end;
        if (!dt_parse_optional_fraction(&p, limit))
            return 0;
    }

    if (p != limit)
        return 0;
    if (hour > 23 || minute > 59 || second > 59)
        return 0;

    *out = (int64_t)(hour * 3600 + minute * 60 + second);
    return 1;
}

/// @brief Parse an ISO 8601 datetime string to a Unix timestamp.
/// @details Accepts an exact four-digit-year datetime with `T`, `t`, or space
///          separator, optional fractional seconds (truncated), and optional
///          `Z`/`z` or numeric `±HH:MM` offset. Without a zone suffix, the time
///          is interpreted locally. Returns 0 on parse failure, colliding with
///          the actual epoch timestamp.
/// @param s Runtime string containing the ISO datetime.
/// @return Unix timestamp in seconds, or 0 on parse failure.
int64_t rt_datetime_parse_iso(rt_string s) {
    int64_t result;
    return dt_parse_iso_impl(s, &result) ? result : 0;
}

/// @brief Parse a date-only string (YYYY-MM-DD) to a Unix timestamp at midnight.
/// @details Interprets the date as local midnight (00:00:00) and converts via
///          mktime. Returns 0 on parse failure.
/// @param s Runtime string containing the date in ISO format.
/// @return Unix timestamp at midnight on the given date, or 0 on failure.
int64_t rt_datetime_parse_date(rt_string s) {
    int64_t result;
    return dt_parse_date_impl(s, &result) ? result : 0;
}

/// @brief Parse a time string (HH:MM or HH:MM:SS) to seconds since midnight.
/// @details Accepts 24-hour format with optional seconds and a fractional
///          suffix only when seconds are present. Fractions are discarded. Validates ranges
///          (hour 0-23, minute 0-59, second 0-59). Returns -1 on any parse
///          error, allowing callers to distinguish "midnight" (0) from failure.
/// @param s Runtime string containing the time text.
/// @return Seconds since midnight (0-86399), or -1 on parse failure.
int64_t rt_datetime_parse_time(rt_string s) {
    int64_t result;
    return dt_parse_time_impl(s, &result) ? result : -1;
}

/// @brief Shared implementation for parsing any supported DateTime input.
/// @details Tries formats in order of specificity: ISO 8601
///          (YYYY-MM-DDTHH:MM:SS), date-only (YYYY-MM-DD), then time-only
///          (HH:MM or HH:MM:SS). The boolean return lets modern Option APIs
///          distinguish a valid Unix epoch timestamp (`0`) from failure.
/// @param s Runtime string to parse.
/// @param out Receives the parsed timestamp or seconds-since-midnight on success.
/// @return 1 when parsing succeeds, 0 when the input is empty or unsupported.
static int8_t dt_try_parse_any(rt_string s, int64_t *out) {
    if (out)
        *out = 0;
    size_t len = 0;
    const char *str = dt_cstr_without_embedded_nul(s, &len);
    if (!str || len == 0)
        return 0;

    if (len >= 19) {
        int64_t result;
        if (dt_parse_iso_impl(s, &result)) {
            if (out)
                *out = result;
            return 1;
        }
    }

    if (len == 10 && str[4] == '-' && str[7] == '-') {
        int64_t result;
        if (dt_parse_date_impl(s, &result)) {
            if (out)
                *out = result;
            return 1;
        }
    }

    if (len >= 5 && str[2] == ':') {
        int64_t result;
        if (dt_parse_time_impl(s, &result)) {
            if (out)
                *out = result;
            return 1;
        }
    }

    return 0;
}

/// @brief Attempt to parse a datetime string in any supported format.
/// @details Legacy sentinel-returning form. Returns the first successful parse,
///          but still returns `0` on failure, so it cannot distinguish failure
///          from a valid Unix epoch timestamp. Prefer
///          @ref rt_datetime_try_parse_option for new public APIs.
/// @param s Runtime string to parse.
/// @return Epoch seconds for datetime/date input, seconds since midnight for
///         time-only input, or 0 on failure.
int64_t rt_datetime_try_parse(rt_string s) {
    int64_t result = 0;
    return dt_try_parse_any(s, &result) ? result : 0;
}

/// @brief Attempt to parse a datetime string and return an Option.
/// @details Returns `Some(i64)` for any supported input, including the Unix
///          epoch timestamp `0`, and `None` for empty, malformed, or
///          embedded-NUL input. This is the non-ambiguous replacement for the
///          legacy sentinel-returning @ref rt_datetime_try_parse helper.
/// @param s Runtime string to parse.
/// @return New Option containing epoch seconds for datetime/date input or
///         seconds since midnight for time-only input; `None` on failure.
void *rt_datetime_try_parse_option(rt_string s) {
    int64_t result = 0;
    return dt_try_parse_any(s, &result) ? rt_option_some_i64(result) : rt_option_none();
}
