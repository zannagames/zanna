//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_duration.c
/// @file
/// @brief Implements scalar millisecond Duration construction, arithmetic, and formatting.
///
// Purpose: Implements the Duration/TimeSpan type for the Zanna runtime.
//          A Duration is a signed 64-bit integer representing a time span in
//          milliseconds. Provides factory functions (FromMillis, FromSeconds,
//          FromMinutes, FromHours, FromDays), total-unit accessors, component
//          extraction (Days/Hours/Minutes/Seconds/Millis parts), and formatting.
//
// Key invariants:
//   - A Duration is represented as a plain int64_t (milliseconds); there is no
//     wrapper struct or heap object — values are passed by value.
//   - Factories and arithmetic functions trap on signed 64-bit overflow.
//   - Component extraction (e.g., rt_duration_get_hours) returns the whole-
//     unit component after subtracting larger units, analogous to .NET TimeSpan.
//   - Negative durations represent intervals in the past; component extraction
//     uses the unsigned magnitude so INT64_MIN is handled without negation UB.
//
// Ownership/Lifetime:
//   - Duration values are scalar int64_t; no heap allocation is performed.
//   - Formatted strings returned by rt_duration_to_string are newly allocated
//     rt_string values; the caller owns the reference and must unref when done.
//
// Links: src/runtime/core/rt_duration.h (public API),
//        src/runtime/core/rt_daterange.c (uses Duration for span computation),
//        src/runtime/core/rt_stopwatch.c (produces Duration-valued elapsed time)
//
//===----------------------------------------------------------------------===//

#include "rt_duration.h"

#include "rt_string.h"
#include "rt_trap.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Milliseconds in one second.
#define MS_PER_SECOND 1000LL
/// @brief Milliseconds in one 60-second minute.
#define MS_PER_MINUTE (60LL * MS_PER_SECOND)
/// @brief Milliseconds in one 60-minute hour.
#define MS_PER_HOUR (60LL * MS_PER_MINUTE)
/// @brief Milliseconds in one 24-hour day.
#define MS_PER_DAY (24LL * MS_PER_HOUR)

// Overflow-checked signed 64-bit arithmetic used by Duration math so a malformed
// duration calculation surfaces as a trap rather than silently wrapping. The same
// triplet appears across the time-related runtime files (Countdown, Stopwatch,
// DateTime, Duration); each carries its own copy to keep the helpers static.

/// @brief Overflow-checked signed 64-bit addition. Returns 1 on overflow.
/// @param a Left operand.
/// @param b Right operand.
/// @param out Receives the sum on success.
/// @return One on overflow; zero after writing the exact sum.
static int dur_checked_add_i64(int64_t a, int64_t b, int64_t *out) {
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
static int dur_checked_sub_i64(int64_t a, int64_t b, int64_t *out) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
        return 1;
    *out = a - b;
    return 0;
}

/// @brief Overflow-checked signed 64-bit multiplication. Returns 1 on overflow.
/// @details Uses `__builtin_mul_overflow` on GCC/Clang and a manual divide-bound
///          check on MSVC.
/// @param a Left factor.
/// @param b Right factor.
/// @param out Receives the product on success.
/// @return One on overflow; zero after writing the exact product.
static int dur_checked_mul_i64(int64_t a, int64_t b, int64_t *out) {
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

/// @brief Absolute value of @p duration as `uint64_t`, safe for `INT64_MIN`.
/// @details The trick `(uint64_t)(-(d+1)) + 1u` avoids the `-INT64_MIN` UB that a naïve
///          `(uint64_t)-d` would hit. Used by the duration formatter to render the
///          sign separately from the magnitude.
/// @param duration Signed millisecond duration.
/// @return Exact unsigned magnitude, including `2^63` for `INT64_MIN`.
static uint64_t dur_abs_u64(int64_t duration) {
    if (duration >= 0)
        return (uint64_t)duration;
    return (uint64_t)(-(duration + 1)) + 1u;
}

/// @brief Advance a snprintf cursor by @p len bytes, clamping at @p end.
/// @details Used by the multi-fragment duration formatter to chain `snprintf` calls
///          into a fixed-size buffer. Negative @p len (snprintf format error) is a no-op;
///          a successful len that would overrun @p end clamps the cursor at @p end so
///          subsequent appends become silent no-ops.
/// @param p Address of the next-write cursor.
/// @param end One-past-end buffer pointer.
/// @param len Character count returned by `snprintf`.
static void dur_append_snprintf(char **p, char *end, int len) {
    if (len < 0)
        return;
    if (len >= end - *p) {
        *p = end;
        return;
    }
    *p += len;
}

/// @brief Appends the ISO seconds component with optional trimmed milliseconds.
///
/// Milliseconds are emitted as one to three fractional digits after removing
/// trailing zeroes; an all-zero fraction emits integer seconds only.
///
/// @param p Address of the next-write cursor.
/// @param end One-past-end buffer pointer.
/// @param seconds Whole seconds component.
/// @param millis Fractional milliseconds in 0–999.
static void dur_append_iso_seconds(char **p,
                                   char *end,
                                   uint64_t seconds,
                                   uint64_t millis) {
    if (*p >= end)
        return;
    if (millis == 0) {
        dur_append_snprintf(
            p, end, snprintf(*p, (size_t)(end - *p), "%lluS", (unsigned long long)seconds));
        return;
    }

    char frac[4];
    // Callers pass 0-999; the modulo makes that bound local so the three-digit
    // field provably fits the buffer.
    snprintf(frac, sizeof(frac), "%03u", (unsigned int)(millis % 1000u));
    size_t frac_len = 3;
    while (frac_len > 0 && frac[frac_len - 1] == '0')
        frac_len--;
    frac[frac_len] = '\0';

    dur_append_snprintf(p,
                        end,
                        snprintf(*p,
                                 (size_t)(end - *p),
                                 "%llu.%sS",
                                 (unsigned long long)seconds,
                                 frac));
}

//=============================================================================
// Duration Creation
//=============================================================================

/// @brief Create a Duration from a raw millisecond count (identity).
/// @param ms Milliseconds.
/// @return The same value (Durations are stored as milliseconds internally).
int64_t rt_duration_from_millis(int64_t ms) {
    return ms;
}

/// @brief Create a Duration from a count of seconds.
/// @param seconds Number of seconds.
/// @return Duration in milliseconds (seconds * 1000).
int64_t rt_duration_from_seconds(int64_t seconds) {
    int64_t result;
    if (dur_checked_mul_i64(seconds, MS_PER_SECOND, &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

/// @brief Create a Duration from a count of minutes.
/// @param minutes Number of minutes.
/// @return Duration in milliseconds (minutes * 60000).
int64_t rt_duration_from_minutes(int64_t minutes) {
    int64_t result;
    if (dur_checked_mul_i64(minutes, MS_PER_MINUTE, &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

/// @brief Create a Duration from a count of hours.
/// @param hours Number of hours.
/// @return Duration in milliseconds (hours * 3600000).
int64_t rt_duration_from_hours(int64_t hours) {
    int64_t result;
    if (dur_checked_mul_i64(hours, MS_PER_HOUR, &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

/// @brief Create a Duration from a count of days.
/// @param days Number of days.
/// @return Duration in milliseconds (days * 86400000).
int64_t rt_duration_from_days(int64_t days) {
    int64_t result;
    if (dur_checked_mul_i64(days, MS_PER_DAY, &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

/// @brief Creates a duration by summing unrestricted unit contributions.
/// @param days Signed 24-hour-day contribution.
/// @param hours Signed hour contribution, not restricted to a component range.
/// @param minutes Signed minute contribution, not restricted to a component range.
/// @param seconds Signed second contribution, not restricted to a component range.
/// @param millis Signed millisecond contribution, not restricted to a component range.
/// @return Exact combined milliseconds.
/// @note Each multiplication and addition is checked; overflow raises a trap.
int64_t rt_duration_create(
    int64_t days, int64_t hours, int64_t minutes, int64_t seconds, int64_t millis) {
    int64_t total = 0;
    int64_t part;
    if (dur_checked_mul_i64(days, MS_PER_DAY, &part) || dur_checked_add_i64(total, part, &total) ||
        dur_checked_mul_i64(hours, MS_PER_HOUR, &part) ||
        dur_checked_add_i64(total, part, &total) ||
        dur_checked_mul_i64(minutes, MS_PER_MINUTE, &part) ||
        dur_checked_add_i64(total, part, &total) ||
        dur_checked_mul_i64(seconds, MS_PER_SECOND, &part) ||
        dur_checked_add_i64(total, part, &total) || dur_checked_add_i64(total, millis, &total)) {
        rt_trap_ovf();
        return 0;
    }
    return total;
}

//=============================================================================
// Duration Total Conversions
//=============================================================================

/// @brief Return the total milliseconds in the duration (identity).
/// @param duration Duration in milliseconds.
/// @return Same value.
int64_t rt_duration_total_millis(int64_t duration) {
    return duration;
}

/// @brief Return the total whole seconds in the duration.
/// @param duration Duration in milliseconds.
/// @return Seconds truncated toward zero (`duration / 1000`).
int64_t rt_duration_total_seconds(int64_t duration) {
    return duration / MS_PER_SECOND;
}

/// @brief Return the total whole minutes in the duration.
/// @param duration Duration in milliseconds.
/// @return Minutes truncated toward zero (`duration / 60000`).
int64_t rt_duration_total_minutes(int64_t duration) {
    return duration / MS_PER_MINUTE;
}

/// @brief Return the total whole hours in the duration.
/// @param duration Duration in milliseconds.
/// @return Hours truncated toward zero (`duration / 3600000`).
int64_t rt_duration_total_hours(int64_t duration) {
    return duration / MS_PER_HOUR;
}

/// @brief Return the total whole days in the duration.
/// @param duration Duration in milliseconds.
/// @return Days truncated toward zero (`duration / 86400000`).
int64_t rt_duration_total_days(int64_t duration) {
    return duration / MS_PER_DAY;
}

/// @brief Return the total seconds as a floating-point value.
/// @details Useful for sub-second precision without truncation.
/// @param duration Duration in milliseconds.
/// @return Fractional seconds (e.g. 1500ms → 1.5).
double rt_duration_total_seconds_f(int64_t duration) {
    return (double)duration / (double)MS_PER_SECOND;
}

//=============================================================================
// Duration Components
//=============================================================================

/// @brief Extract the days component (largest unit).
/// @details Returns abs(duration) / MS_PER_DAY — the whole days portion.
/// @param duration Duration in milliseconds.
/// @return Nonnegative day count from the absolute magnitude.
int64_t rt_duration_get_days(int64_t duration) {
    uint64_t abs_dur = dur_abs_u64(duration);
    return (int64_t)(abs_dur / MS_PER_DAY);
}

/// @brief Extract the hours component (0-23 after removing days).
/// @param duration Duration in milliseconds.
/// @return Nonnegative hours remaining after whole absolute-magnitude days.
int64_t rt_duration_get_hours(int64_t duration) {
    uint64_t abs_dur = dur_abs_u64(duration);
    return (int64_t)((abs_dur % MS_PER_DAY) / MS_PER_HOUR);
}

/// @brief Extract the minutes component (0-59 after removing hours).
/// @param duration Duration in milliseconds.
/// @return Nonnegative minutes remaining after whole absolute-magnitude hours.
int64_t rt_duration_get_minutes(int64_t duration) {
    uint64_t abs_dur = dur_abs_u64(duration);
    return (int64_t)((abs_dur % MS_PER_HOUR) / MS_PER_MINUTE);
}

/// @brief Extract the seconds component (0-59 after removing minutes).
/// @param duration Duration in milliseconds.
/// @return Nonnegative seconds remaining after whole absolute-magnitude minutes.
int64_t rt_duration_get_seconds(int64_t duration) {
    uint64_t abs_dur = dur_abs_u64(duration);
    return (int64_t)((abs_dur % MS_PER_MINUTE) / MS_PER_SECOND);
}

/// @brief Extract the milliseconds component (0-999 after removing seconds).
/// @param duration Duration in milliseconds.
/// @return Nonnegative milliseconds remaining after whole absolute-magnitude seconds.
int64_t rt_duration_get_millis(int64_t duration) {
    uint64_t abs_dur = dur_abs_u64(duration);
    return (int64_t)(abs_dur % MS_PER_SECOND);
}

//=============================================================================
// Duration Operations
//=============================================================================

/// @brief Add two durations.
/// @param d1 First duration in milliseconds.
/// @param d2 Second duration in milliseconds.
/// @return Sum (d1 + d2) in milliseconds.
int64_t rt_duration_add(int64_t d1, int64_t d2) {
    int64_t result;
    if (dur_checked_add_i64(d1, d2, &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

/// @brief Subtract two durations.
/// @param d1 First duration in milliseconds.
/// @param d2 Second duration in milliseconds.
/// @return Difference (d1 - d2) in milliseconds.
int64_t rt_duration_sub(int64_t d1, int64_t d2) {
    int64_t result;
    if (dur_checked_sub_i64(d1, d2, &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

/// @brief Scale a duration by an integer factor.
/// @param duration Duration in milliseconds.
/// @param factor Multiplier.
/// @return Scaled duration (duration * factor).
int64_t rt_duration_mul(int64_t duration, int64_t factor) {
    int64_t result;
    if (dur_checked_mul_i64(duration, factor, &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

/// @brief Divide a duration by an integer divisor.
/// @details Traps on zero divisor to match runtime error conventions.
/// @param duration Duration in milliseconds.
/// @param divisor Divisor (must be non-zero).
/// @return Quotient truncated toward zero.
/// @note Division by zero and `INT64_MIN / -1` raise traps.
int64_t rt_duration_div(int64_t duration, int64_t divisor) {
    if (divisor == 0) {
        rt_trap_div0();
        return 0;
    }
    if (duration == INT64_MIN && divisor == -1) {
        rt_trap_ovf();
        return 0;
    }
    return duration / divisor;
}

/// @brief Return the absolute value of a duration.
/// @param duration Duration in milliseconds (may be negative).
/// @return Non-negative magnitude.
int64_t rt_duration_abs(int64_t duration) {
    if (duration == INT64_MIN) {
        rt_trap_ovf();
        return 0;
    }
    return duration >= 0 ? duration : -duration;
}

/// @brief Negate a duration.
/// @param duration Duration in milliseconds.
/// @return Negated value (-duration).
int64_t rt_duration_neg(int64_t duration) {
    if (duration == INT64_MIN) {
        rt_trap_ovf();
        return 0;
    }
    return -duration;
}

//=============================================================================
// Duration Comparison
//=============================================================================

/// @brief Compare two durations.
/// @param d1 First duration in milliseconds.
/// @param d2 Second duration in milliseconds.
/// @return -1 if d1 < d2, 0 if equal, 1 if d1 > d2.
int64_t rt_duration_cmp(int64_t d1, int64_t d2) {
    if (d1 < d2)
        return -1;
    if (d1 > d2)
        return 1;
    return 0;
}

//=============================================================================
// Duration Formatting
//=============================================================================

/// @brief Format a duration as a fixed clock-style string.
/// @details Produces `[-]d.HH:MM:SS[.fff]` when days are nonzero and
///          `[-]HH:MM:SS[.fff]` otherwise. Negative values carry one leading sign.
/// @param duration Duration in milliseconds.
/// @return Newly allocated runtime string.
rt_string rt_duration_to_string(int64_t duration) {
    char buffer[64];

    int negative = duration < 0;
    uint64_t abs_dur = dur_abs_u64(duration);

    uint64_t days = abs_dur / MS_PER_DAY;
    uint64_t hours = (abs_dur % MS_PER_DAY) / MS_PER_HOUR;
    uint64_t minutes = (abs_dur % MS_PER_HOUR) / MS_PER_MINUTE;
    uint64_t seconds = (abs_dur % MS_PER_MINUTE) / MS_PER_SECOND;
    uint64_t millis = abs_dur % MS_PER_SECOND;

    const char *sign = negative ? "-" : "";

    if (days > 0) {
        if (millis > 0) {
            snprintf(buffer,
                     sizeof(buffer),
                     "%s%llu.%02llu:%02llu:%02llu.%03llu",
                     sign,
                     (unsigned long long)days,
                     (unsigned long long)hours,
                     (unsigned long long)minutes,
                     (unsigned long long)seconds,
                     (unsigned long long)millis);
        } else {
            snprintf(buffer,
                     sizeof(buffer),
                     "%s%llu.%02llu:%02llu:%02llu",
                     sign,
                     (unsigned long long)days,
                     (unsigned long long)hours,
                     (unsigned long long)minutes,
                     (unsigned long long)seconds);
        }
    } else {
        if (millis > 0) {
            snprintf(buffer,
                     sizeof(buffer),
                     "%s%02llu:%02llu:%02llu.%03llu",
                     sign,
                     (unsigned long long)hours,
                     (unsigned long long)minutes,
                     (unsigned long long)seconds,
                     (unsigned long long)millis);
        } else {
            snprintf(buffer,
                     sizeof(buffer),
                     "%s%02llu:%02llu:%02llu",
                     sign,
                     (unsigned long long)hours,
                     (unsigned long long)minutes,
                     (unsigned long long)seconds);
        }
    }

    return rt_string_from_bytes(buffer, strlen(buffer));
}

/// @brief Format a duration as an ISO 8601 duration string.
/// @details Produces output like `P2DT3H15M30.5S`; negative values use a
///          leading `-P`, zero is `PT0S`, and millisecond fractions omit
///          trailing zeroes.
/// @param duration Duration in milliseconds.
/// @return Newly allocated runtime string.
rt_string rt_duration_to_iso(int64_t duration) {
    char buffer[64];
    char *p = buffer;
    char *end = buffer + sizeof(buffer);

    int negative = duration < 0;
    uint64_t abs_dur = dur_abs_u64(duration);

    uint64_t days = abs_dur / MS_PER_DAY;
    uint64_t hours = (abs_dur % MS_PER_DAY) / MS_PER_HOUR;
    uint64_t minutes = (abs_dur % MS_PER_HOUR) / MS_PER_MINUTE;
    uint64_t seconds = (abs_dur % MS_PER_MINUTE) / MS_PER_SECOND;
    uint64_t millis = abs_dur % MS_PER_SECOND;

    if (negative)
        *p++ = '-';
    *p++ = 'P';

    if (days > 0) {
        if (p < end)
            dur_append_snprintf(
                &p, end, snprintf(p, (size_t)(end - p), "%lluD", (unsigned long long)days));
    }

    if (hours > 0 || minutes > 0 || seconds > 0 || millis > 0) {
        if (p < end)
            *p++ = 'T';
        if (hours > 0) {
            if (p < end)
                dur_append_snprintf(
                    &p, end, snprintf(p, (size_t)(end - p), "%lluH", (unsigned long long)hours));
        }
        if (minutes > 0) {
            if (p < end)
                dur_append_snprintf(
                    &p, end, snprintf(p, (size_t)(end - p), "%lluM", (unsigned long long)minutes));
        }
        if (seconds > 0 || millis > 0) {
            if (p < end)
                dur_append_iso_seconds(&p, end, seconds, millis);
        }
    }

    // Handle zero duration
    if ((p == buffer + 1 || (p == buffer + 2 && negative)) && p + 3 < end) {
        *p++ = 'T';
        *p++ = '0';
        *p++ = 'S';
    }

    if (p >= end)
        p = end - 1;
    *p = '\0';
    return rt_string_from_bytes(buffer, strlen(buffer));
}

//=============================================================================
// Constants
//=============================================================================

/// @brief Return the zero duration (0 milliseconds).
/// @return Result value.
int64_t rt_duration_zero(void) {
    return 0;
}
