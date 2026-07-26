//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file rt_network_time.inc
/// @brief Portable UTC civil-date conversion helpers for network protocols.
/// @details This private include provides an allocation-free replacement for
///          nonstandard `timegm` APIs. It is included only by runtime
///          translation units that parse or format protocol timestamps.
///          Calendar input is interpreted in the proleptic Gregorian calendar.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>
#include <time.h>

/// @brief Convert a Gregorian civil date to days relative to the Unix epoch.
/// @details Uses an era-based integer calendar algorithm that supports years
///          before and after 1970 without consulting local timezone state.
/// @param year Full signed calendar year.
/// @param month One-based month number; callers supply a validated value.
/// @param day One-based day of month; callers supply a validated value.
/// @return Signed day offset from 1970-01-01.
static inline int64_t rt_network_days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/// @brief Convert a UTC broken-down timestamp to portable epoch seconds.
/// @details Validates the representable component ranges, converts through the
///          civil-day helper without applying local timezone or daylight-saving
///          rules, and detects narrowing when @c time_t cannot hold the result.
///          A leap-second value of 60 is accepted.
/// @param tm_val UTC calendar components using standard @c struct tm offsets.
/// @return UTC epoch seconds, or `(time_t)-1` for invalid input or overflow.
static inline time_t rt_network_timegm_utc(const struct tm *tm_val) {
    if (!tm_val || tm_val->tm_mon < 0 || tm_val->tm_mon > 11 || tm_val->tm_mday < 1 ||
        tm_val->tm_mday > 31 || tm_val->tm_hour < 0 || tm_val->tm_hour > 23 || tm_val->tm_min < 0 ||
        tm_val->tm_min > 59 || tm_val->tm_sec < 0 || tm_val->tm_sec > 60) {
        return (time_t)-1;
    }

    int64_t seconds =
        rt_network_days_from_civil(
            tm_val->tm_year + 1900, (unsigned)(tm_val->tm_mon + 1), (unsigned)tm_val->tm_mday) *
            86400 +
        (int64_t)tm_val->tm_hour * 3600 + (int64_t)tm_val->tm_min * 60 + (int64_t)tm_val->tm_sec;
    time_t out = (time_t)seconds;
    return (int64_t)out == seconds ? out : (time_t)-1;
}
