//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_dateonly.c
/// @file
/// @brief Implements the GC-managed DateOnly proleptic Gregorian value type.
///
// Purpose: Implements the DateOnly type for the Zanna runtime, representing a
//          calendar date (year, month, day) without a time component. Provides
//          construction from components or Unix day offsets, arithmetic
//          (AddDays, DiffDays), comparison, formatting, and leap-year handling.
//
// Key invariants:
//   - Dates are represented internally as (year, month, day) tuples with no
//     timezone or time information.
//   - Internal conversion between dates and "days since Unix epoch" uses the
//     overflow-checked civil calendar arithmetic for correctness across the
//     proleptic Gregorian calendar.
//   - Month values are 1-based (January = 1, December = 12).
//   - Leap years follow the Gregorian rule: divisible by 4, except centuries
//     unless also divisible by 400.
//   - Out-of-range month/day inputs produce 0 from days_in_month_impl rather
//     than trapping; validation responsibility lies with the caller.
//
// Ownership/Lifetime:
//   - DateOnly instances are heap-allocated via rt_obj_new_i64 and managed by
//     the runtime GC; callers do not free them explicitly.
//   - Formatted strings are newly allocated rt_string values; the caller owns
//     the reference and must call rt_string_unref when done.
//
// Links: src/runtime/core/rt_dateonly.h (public API),
//        src/runtime/core/rt_datetime.c (full DateTime with time components),
//        src/runtime/core/rt_daterange.c (DateRange spanning two timestamps)
//
//===----------------------------------------------------------------------===//

#include "rt_dateonly.h"
#include "rt_datetime.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

//=============================================================================
// Internal Structure
//=============================================================================

/// @brief Internal DateOnly calendar tuple.
typedef struct {
    int64_t year;
    int64_t month;
    int64_t day;
} DateOnly;

//=============================================================================
// Helper Functions
//=============================================================================

/// @brief Test whether @p year is a Gregorian leap year.
/// @details Implements the Gregorian leap-year rule: years divisible by 4 are
///          leap, except century years (`% 100 == 0`) which are leap only
///          when also divisible by 400.
/// @param year Proleptic Gregorian year (negative values are valid).
/// @return 1 if @p year is a leap year, 0 otherwise.
static int8_t is_leap_year(int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/// @brief Return the number of days in @p month (1–12) of @p year.
/// @details February yields 29 in leap years, 28 otherwise; out-of-range months
///          produce 0 so callers detect bad input via the zero return.
/// @param year Proleptic Gregorian year used for February.
/// @param month One-based month number.
/// @return 28–31 for a valid month, otherwise zero.
static int64_t days_in_month_impl(int64_t year, int64_t month) {
    static const int64_t days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12)
        return 0;
    if (month == 2 && is_leap_year(year))
        return 29;
    return days[month];
}

// Overflow-checked signed 64-bit arithmetic used by the civil-calendar day-count
// math. Same triplet as in rt_countdown / rt_duration / rt_datetime — pre-check
// operands before the operation to avoid signed-overflow UB.

/// @brief Overflow-checked signed 64-bit addition. Returns 1 on overflow.
/// @param a Left operand.
/// @param b Right operand.
/// @param out Receives the sum on success.
/// @return One on overflow; zero after writing the exact sum.
static int date_checked_add_i64(int64_t a, int64_t b, int64_t *out) {
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
static int date_checked_sub_i64(int64_t a, int64_t b, int64_t *out) {
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
static int date_checked_mul_i64(int64_t a, int64_t b, int64_t *out) {
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

/// @brief Floor-division of @p value by @p divisor (rounds toward negative infinity).
/// @details C's `/` rounds toward zero; this helper rounds toward `-∞` so the day-of-era
///          math correctly handles BC dates (negative years). The fix-up subtracts 1
///          from the truncated quotient when the remainder has a different sign than
///          the divisor.
/// @param value Dividend.
/// @param divisor Nonzero divisor.
/// @return Mathematical floor of `value / divisor`.
static int64_t date_floor_div(int64_t value, int64_t divisor) {
    int64_t quotient = value / divisor;
    int64_t remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0)))
        quotient--;
    return quotient;
}

/// @brief Locale-independent test for ASCII digits 0-9.
/// @param c Byte value to classify.
/// @return Nonzero only for ASCII `0` through `9`.
static int date_is_digit(char c) {
    return c >= '0' && c <= '9';
}

/// @brief Parse @p count consecutive ASCII digits from @p s into a non-negative integer.
/// @details Used by the ISO-8601 date parser. Returns -1 on any non-digit byte so the
///          caller can flag malformed input. No bounds check on @p s — the caller must
///          have already verified the buffer has at least @p count bytes.
/// @param s Pointer to at least @p count readable bytes.
/// @param count Number of digits to parse.
/// @return Parsed nonnegative value, or `-1` when any byte is not a digit.
static int date_parse_digits(const char *s, int count) {
    int value = 0;
    for (int i = 0; i < count; i++) {
        if (!date_is_digit(s[i]))
            return -1;
        value = value * 10 + (s[i] - '0');
    }
    return value;
}

/// @brief Ensure a DateOnly formatting buffer can hold at least @p needed bytes.
/// @details Grows the heap buffer geometrically to keep repeated appends amortized
///          O(n). On allocation failure, frees the old buffer, clears the caller's
///          state, and traps so callers do not continue with a dangling pointer.
/// @param buf Pointer to the owned formatting buffer.
/// @param cap Pointer to the current buffer capacity.
/// @param needed Required capacity including the trailing NUL.
/// @return 1 on success, 0 on allocation failure.
static int dateonly_format_reserve(char **buf, size_t *cap, size_t needed) {
    if (needed <= *cap)
        return 1;
    size_t next = *cap ? *cap : 64;
    while (next < needed) {
        if (next > SIZE_MAX / 2) {
            next = needed;
            break;
        }
        next *= 2;
    }
    char *grown = (char *)realloc(*buf, next);
    if (!grown) {
        free(*buf);
        *buf = NULL;
        *cap = 0;
        rt_trap("DateOnly.Format: memory allocation failed");
        return 0;
    }
    *buf = grown;
    *cap = next;
    return 1;
}

/// @brief Append raw bytes to a DateOnly formatting buffer.
/// @details Performs overflow checks before computing the new length and ensures
///          the buffer remains NUL-terminated after the append. On failure, the
///          owned buffer is released and the caller's state is reset.
/// @param buf Pointer to the owned formatting buffer.
/// @param cap Pointer to the current buffer capacity.
/// @param len Pointer to the current byte length excluding the trailing NUL.
/// @param bytes Bytes to append.
/// @param bytes_len Number of bytes to append.
/// @return 1 on success, 0 on overflow or allocation failure.
static int dateonly_format_append_bytes(char **buf,
                                        size_t *cap,
                                        size_t *len,
                                        const char *bytes,
                                        size_t bytes_len) {
    if (!bytes || bytes_len == 0)
        return 1;
    if (*len > SIZE_MAX - bytes_len - 1) {
        free(*buf);
        *buf = NULL;
        *cap = 0;
        *len = 0;
        rt_trap("DateOnly.Format: output too large");
        return 0;
    }
    size_t needed = *len + bytes_len + 1;
    if (!dateonly_format_reserve(buf, cap, needed))
        return 0;
    memcpy(*buf + *len, bytes, bytes_len);
    *len += bytes_len;
    (*buf)[*len] = '\0';
    return 1;
}

/// @brief Append a NUL-terminated C string to a DateOnly formatting buffer.
/// @param buf Pointer to the owned formatting buffer.
/// @param cap Pointer to the current buffer capacity.
/// @param len Pointer to the current byte length excluding the trailing NUL.
/// @param text C string to append; NULL is treated as empty.
/// @return 1 on success, 0 on overflow or allocation failure.
static int dateonly_format_append_cstr(char **buf, size_t *cap, size_t *len, const char *text) {
    return dateonly_format_append_bytes(buf, cap, len, text, text ? strlen(text) : 0);
}

/// @brief Append one character to a DateOnly formatting buffer.
/// @param buf Pointer to the owned formatting buffer.
/// @param cap Pointer to the current buffer capacity.
/// @param len Pointer to the current byte length excluding the trailing NUL.
/// @param ch Character to append.
/// @return 1 on success, 0 on overflow or allocation failure.
static int dateonly_format_append_char(char **buf, size_t *cap, size_t *len, char ch) {
    return dateonly_format_append_bytes(buf, cap, len, &ch, 1);
}

/// @brief Convert a Gregorian date to days since Unix epoch (1970-01-01).
/// @details Uses overflow-checked civil calendar arithmetic. The calendar is
///          adjusted so March is month 0, then reduced into 400-year Gregorian
///          eras before subtracting the Unix epoch day offset.
/// @param year Proleptic Gregorian year.
/// @param month One-based month.
/// @param day One-based day of month.
/// @param out Receives the signed epoch-day offset.
/// @return Nonzero on success; zero if intermediate arithmetic overflows.
static int to_days_since_epoch_checked(int64_t year, int64_t month, int64_t day, int64_t *out) {
    int64_t adjusted_year = year;
    if (month <= 2 && date_checked_sub_i64(adjusted_year, 1, &adjusted_year))
        return 0;

    int64_t era_numerator = adjusted_year;
    if (adjusted_year < 0 && date_checked_sub_i64(adjusted_year, 399, &era_numerator))
        return 0;

    int64_t era = era_numerator / 400;
    int64_t era_years;
    int64_t yoe_i64;
    if (date_checked_mul_i64(era, 400, &era_years) ||
        date_checked_sub_i64(adjusted_year, era_years, &yoe_i64))
        return 0;

    uint64_t yoe = (uint64_t)yoe_i64;
    uint64_t month_term = (uint64_t)(month + (month > 2 ? -3 : 9));
    uint64_t doy = (153u * month_term + 2u) / 5u + (uint64_t)day - 1u;
    uint64_t doe_u = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    if (doe_u > INT64_MAX)
        return 0;

    int64_t era_days;
    int64_t total;
    if (date_checked_mul_i64(era, 146097, &era_days) ||
        date_checked_add_i64(era_days, (int64_t)doe_u, &total) ||
        date_checked_sub_i64(total, 719468, out))
        return 0;
    return 1;
}

/// @brief Trapping wrapper around `to_days_since_epoch_checked`.
/// @details Returns the day count or traps with `rt_trap_ovf()` on overflow. Used by
///          the public Date API entries that prefer a fail-loud contract over a
///          two-return-value pattern.
/// @param year Proleptic Gregorian year.
/// @param month One-based month.
/// @param day One-based day of month.
/// @return Signed day offset from 1970-01-01, or zero after an overflow trap.
static int64_t to_days_since_epoch(int64_t year, int64_t month, int64_t day) {
    int64_t result;
    if (!to_days_since_epoch_checked(year, month, day, &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

/// @brief Convert days since Unix epoch back to Gregorian year/month/day.
/// @details Inverse of to_days_since_epoch. Adds the epoch day offset, then
///          reverses the Gregorian era/month transform to recover year, month,
///          and day. The constants come from Gregorian cycle lengths: 400-year
///          cycle = 146097 days, and 5-month group = 153 days.
/// @param days Signed offset from 1970-01-01.
/// @param year Receives the proleptic Gregorian year.
/// @param month Receives the one-based month.
/// @param day Receives the one-based day of month.
/// @return Nonzero on success; zero if intermediate arithmetic overflows.
static int from_days_since_epoch_checked(int64_t days,
                                         int64_t *year,
                                         int64_t *month,
                                         int64_t *day) {
    int64_t z;
    if (date_checked_add_i64(days, 719468, &z))
        return 0;

    int64_t era = date_floor_div(z, 146097);
    int64_t era_days;
    int64_t doe;
    if (date_checked_mul_i64(era, 146097, &era_days) || date_checked_sub_i64(z, era_days, &doe))
        return 0;

    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yoe_days = 365 * yoe + yoe / 4 - yoe / 100;
    int64_t doy = doe - yoe_days;
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);

    int64_t era_years;
    int64_t y;
    if (date_checked_mul_i64(era, 400, &era_years) || date_checked_add_i64(yoe, era_years, &y) ||
        (m <= 2 && date_checked_add_i64(y, 1, &y)))
        return 0;

    *year = y;
    *month = m;
    *day = d;
    return 1;
}

//=============================================================================
// DateOnly Creation
//=============================================================================

/// @brief Create a DateOnly from explicit year, month, and day components.
/// @details Validates that year is in [0,9999], month is in [1,12], and day is in
///          [1, days-in-month]. Returns NULL for invalid inputs rather than
///          trapping, allowing callers to provide their own error handling. The
///          year domain is the four-digit ISO 8601 range so that every constructed
///          value round-trips through `ToString`/`Parse`; years outside it (e.g.
///          negative or five-digit) are rejected rather than producing a string
///          the exact parser cannot read back (VDOC-231).
/// @param year Gregorian year in [0,9999] (e.g. 2026).
/// @param month Month number (1=January, 12=December).
/// @param day Day of month (1-based).
/// @return New GC-managed DateOnly, or NULL if inputs are out of range.
void *rt_dateonly_create(int64_t year, int64_t month, int64_t day) {
    // Validate inputs. The year domain matches the four-digit serializer/parser
    // domain so construction and round-trip agree (VDOC-231).
    if (year < 0 || year > 9999)
        return NULL;
    if (month < 1 || month > 12)
        return NULL;
    int64_t max_day = days_in_month_impl(year, month);
    if (day < 1 || day > max_day)
        return NULL;

    DateOnly *d = (DateOnly *)rt_obj_new_i64(RT_DATEONLY_CLASS_ID, (int64_t)sizeof(DateOnly));
    if (!d) {
        rt_trap("DateOnly.New: memory allocation failed");
        return NULL;
    }

    d->year = year;
    d->month = month;
    d->day = day;
    return d;
}

/// @brief Return a DateOnly representing today's date in local time.
/// @details Uses the platform's localtime_r to convert the current Unix
///          timestamp to a calendar date. The result reflects the system's
///          local timezone setting (not UTC). A genuine wall-clock failure is
///          detected via the shared checked read rather than being converted into
///          a plausible 1969 date (VDOC-230).
/// @return New DateOnly for today, or NULL if the system clock fails.
void *rt_dateonly_today(void) {
    int64_t seconds;
    if (!rt_datetime_wall_seconds(&seconds))
        return NULL;
    // seconds was produced from a live time_t, so the round-trip cast is exact.
    time_t now = (time_t)seconds;
    struct tm tm_buf;
    struct tm *tm = rt_localtime_r(&now, &tm_buf);
    if (!tm)
        return NULL;
    return rt_dateonly_create(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
}

/// @brief Parse a DateOnly from an ISO 8601 date string (YYYY-MM-DD).
/// @details Validates exactly ten bytes with four ASCII year digits and two-digit
///          month/day fields. Delegates calendar validation to rt_dateonly_create.
/// @param s Runtime string containing the date text.
/// @return New DateOnly, or NULL if the string is malformed or out of range.
void *rt_dateonly_parse(rt_string s) {
    int64_t len = rt_str_len(s);
    if (len != 10)
        return NULL;
    const char *str = rt_string_cstr(s);
    if (!str)
        return NULL;
    if (str[4] != '-' || str[7] != '-')
        return NULL;

    int year = date_parse_digits(str, 4);
    int month = date_parse_digits(str + 5, 2);
    int day = date_parse_digits(str + 8, 2);
    if (year < 0 || month < 0 || day < 0)
        return NULL;

    return rt_dateonly_create(year, month, day);
}

/// @brief Create a DateOnly from a days-since-epoch count.
/// @details Converts the signed day offset back to year/month/day using the
///          checked Gregorian civil-date inverse. Day 0 = January 1, 1970.
///          Negative values represent dates before the Unix epoch.
/// @param days Signed offset from 1970-01-01.
/// @return New DateOnly for the corresponding calendar date, or `NULL` if the
///         result falls outside the supported year range.
/// @note Arithmetic overflow raises a trap.
void *rt_dateonly_from_days(int64_t days) {
    int64_t year, month, day;
    if (!from_days_since_epoch_checked(days, &year, &month, &day)) {
        rt_trap_ovf();
        return NULL;
    }
    return rt_dateonly_create(year, month, day);
}

//=============================================================================
// Component Access
//=============================================================================

/// @brief Validate an explicit DateOnly receiver, trapping on a wrong class.
/// @details Returns NULL for a NULL receiver so callers keep their existing
///          null-sentinel contract, but a non-NULL object of another class (e.g.
///          a Seq handed to the static compatibility form) traps rather than
///          being reinterpreted as a DateOnly payload (VDOC-229). The heap kind,
///          class ID, and payload size are all checked by rt_obj_is_instance.
/// @param obj Candidate receiver pointer.
/// @return The validated DateOnly, or NULL when @p obj is NULL.
static DateOnly *as_dateonly(void *obj) {
    if (!obj)
        return NULL;
    if (!rt_obj_is_instance(obj, RT_DATEONLY_CLASS_ID, sizeof(DateOnly))) {
        rt_trap("DateOnly: invalid receiver");
        return NULL;
    }
    return (DateOnly *)obj;
}

/// @brief Return the year component of a DateOnly (e.g. 2026).
/// @param obj DateOnly object pointer; returns 0 if NULL.
/// @return Four-digit year.
int64_t rt_dateonly_year(void *obj) {
    if (!obj)
        return 0;
    DateOnly *d = as_dateonly(obj);
    return d->year;
}

/// @brief Return the month component (1=January, 12=December).
/// @param obj DateOnly object pointer; returns 0 if NULL.
/// @return Month number in the range 1-12.
int64_t rt_dateonly_month(void *obj) {
    if (!obj)
        return 0;
    DateOnly *d = as_dateonly(obj);
    return d->month;
}

/// @brief Return the day-of-month component (1-31).
/// @param obj DateOnly object pointer; returns 0 if NULL.
/// @return Day number in the range 1-31.
int64_t rt_dateonly_day(void *obj) {
    if (!obj)
        return 0;
    DateOnly *d = as_dateonly(obj);
    return d->day;
}

/// @brief Return the day of week (0=Sunday through 6=Saturday).
/// @details Converts the date to days-since-epoch, then offsets by 4 because
///          January 1, 1970 was a Thursday (day index 4 in a Sunday-start week).
///          The modulo 7 produces the correct weekday for any date.
/// @param obj DateOnly object pointer; returns 0 if NULL.
/// @return Day-of-week index: 0=Sunday, 1=Monday, ..., 6=Saturday.
int64_t rt_dateonly_day_of_week(void *obj) {
    if (!obj)
        return 0;
    DateOnly *d = as_dateonly(obj);

    // Zeller's formula (modified for Sunday = 0)
    int64_t days = to_days_since_epoch(d->year, d->month, d->day);
    // Jan 1, 1970 was Thursday (day 4)
    int64_t dow = (days % 7 + 4) % 7;
    return dow < 0 ? dow + 7 : dow;
}

/// @brief Return the 1-based day-of-year (1-366).
/// @details Sums the number of days in all preceding months (accounting for
///          leap years in February) then adds the current day. January 1 = 1.
/// @param obj DateOnly object pointer; returns 0 if NULL.
/// @return Day-of-year in the range 1-366.
int64_t rt_dateonly_day_of_year(void *obj) {
    if (!obj)
        return 0;
    DateOnly *d = as_dateonly(obj);

    int64_t doy = 0;
    for (int64_t m = 1; m < d->month; m++) {
        doy += days_in_month_impl(d->year, m);
    }
    doy += d->day;
    return doy;
}

/// @brief Convert the date to days since Unix epoch (1970-01-01 = day 0).
/// @details Delegates to checked civil-date conversion. Useful for
///          serialization, date arithmetic, and comparing dates numerically.
/// @param obj DateOnly object pointer; returns 0 if NULL.
/// @return Signed day offset (negative for dates before 1970).
int64_t rt_dateonly_to_days(void *obj) {
    if (!obj)
        return 0;
    DateOnly *d = as_dateonly(obj);
    return to_days_since_epoch(d->year, d->month, d->day);
}

//=============================================================================
// Date Arithmetic
//=============================================================================

/// @brief Return a new DateOnly shifted by the given number of days.
/// @details Converts to days-since-epoch, adds the offset, and converts back.
///          Handles month/year boundary crossings automatically via the epoch
///          round-trip. Negative values move backward in time.
/// @param obj Source DateOnly (not modified).
/// @param days Signed number of days to add.
/// @return New DateOnly for the resulting calendar date, or `NULL` if it leaves
///         the supported year range.
void *rt_dateonly_add_days(void *obj, int64_t days) {
    if (!obj)
        return NULL;
    DateOnly *d = as_dateonly(obj);
    int64_t base = to_days_since_epoch(d->year, d->month, d->day);
    int64_t total;
    if (date_checked_add_i64(base, days, &total)) {
        rt_trap_ovf();
        return NULL;
    }
    return rt_dateonly_from_days(total);
}

/// @brief Return a new DateOnly shifted by the given number of months.
/// @details Adds the month offset then normalizes year/month. If the resulting
///          day exceeds the new month's length (e.g. Jan 31 + 1 month → Feb 28),
///          it clamps to the last day. Special case: Feb 29 → Feb 28 when the
///          target year is not a leap year.
/// @param obj Source DateOnly (not modified).
/// @param months Signed number of months to add (negative = subtract).
/// @return New DateOnly with clamped day-of-month, or `NULL` outside the
///         supported year range.
void *rt_dateonly_add_months(void *obj, int64_t months) {
    if (!obj)
        return NULL;
    DateOnly *d = as_dateonly(obj);

    int64_t month_index;
    if (date_checked_add_i64(d->month - 1, months, &month_index)) {
        rt_trap_ovf();
        return NULL;
    }

    int64_t year_delta = month_index / 12;
    int64_t month_zero = month_index % 12;
    if (month_zero < 0) {
        month_zero += 12;
        year_delta--;
    }

    int64_t year;
    if (date_checked_add_i64(d->year, year_delta, &year)) {
        rt_trap_ovf();
        return NULL;
    }
    int64_t month = month_zero + 1;

    // Clamp day to valid range for new month
    int64_t max_day = days_in_month_impl(year, month);
    int64_t day = d->day;
    if (day > max_day)
        day = max_day;

    return rt_dateonly_create(year, month, day);
}

/// @brief Return a new DateOnly shifted by the given number of years.
/// @details Delegates to add_months(obj, years * 12). This handles the Feb 29
///          leap-year edge case correctly — adding 1 year to Feb 29 gives Feb 28
///          in a non-leap year.
/// @param obj Source DateOnly (not modified).
/// @param years Signed number of years to add.
/// @return New DateOnly, or `NULL` outside the supported year range.
void *rt_dateonly_add_years(void *obj, int64_t years) {
    if (!obj)
        return NULL;
    DateOnly *d = as_dateonly(obj);

    int64_t year;
    if (date_checked_add_i64(d->year, years, &year)) {
        rt_trap_ovf();
        return NULL;
    }
    int64_t month = d->month;
    int64_t day = d->day;

    // Handle Feb 29 in non-leap years
    if (month == 2 && day == 29 && !is_leap_year(year)) {
        day = 28;
    }

    return rt_dateonly_create(year, month, day);
}

/// @brief Return the signed difference in days between two dates (a - b).
/// @details Converts both dates to days-since-epoch and subtracts. Positive
///          result means a is later than b; negative means a is earlier.
/// @param a First DateOnly (the "later" date in positive-result convention).
/// @param b Second DateOnly (subtracted from a).
/// @return Signed day difference; 0 if either input is NULL.
int64_t rt_dateonly_diff_days(void *a, void *b) {
    if (!a || !b)
        return 0;
    int64_t result;
    if (date_checked_sub_i64(rt_dateonly_to_days(a), rt_dateonly_to_days(b), &result)) {
        rt_trap_ovf();
        return 0;
    }
    return result;
}

//=============================================================================
// Date Queries
//=============================================================================

/// @brief Check if the date's year is a Gregorian leap year.
/// @details Leap year rule: divisible by 4, except centuries unless also
///          divisible by 400. So 2000 is a leap year but 1900 is not.
/// @param obj DateOnly object pointer; returns 0 if NULL.
/// @return 1 if the year is a leap year, 0 otherwise.
int8_t rt_dateonly_is_leap_year(void *obj) {
    if (!obj)
        return 0;
    DateOnly *d = as_dateonly(obj);
    return is_leap_year(d->year);
}

/// @brief Return the number of days in the date's month (28-31).
/// @details Accounts for leap years when the month is February.
/// @param obj DateOnly object pointer; returns 0 if NULL.
/// @return Day count: 28 or 29 for Feb, 30 or 31 for other months.
int64_t rt_dateonly_days_in_month(void *obj) {
    if (!obj)
        return 0;
    DateOnly *d = as_dateonly(obj);
    return days_in_month_impl(d->year, d->month);
}

/// @brief Return a new DateOnly for the first day of the same month.
/// @param obj Source DateOnly; null returns `NULL`.
/// @return New DateOnly with day one.
void *rt_dateonly_start_of_month(void *obj) {
    if (!obj)
        return NULL;
    DateOnly *d = as_dateonly(obj);
    return rt_dateonly_create(d->year, d->month, 1);
}

/// @brief Return a new DateOnly for the last day of the same month.
/// @param obj Source DateOnly; null returns `NULL`.
/// @return New DateOnly at the month's Gregorian final day.
void *rt_dateonly_end_of_month(void *obj) {
    if (!obj)
        return NULL;
    DateOnly *d = as_dateonly(obj);
    return rt_dateonly_create(d->year, d->month, days_in_month_impl(d->year, d->month));
}

/// @brief Return a new DateOnly for January 1 of the same year.
/// @param obj Source DateOnly; null returns `NULL`.
/// @return New DateOnly for January 1.
void *rt_dateonly_start_of_year(void *obj) {
    if (!obj)
        return NULL;
    DateOnly *d = as_dateonly(obj);
    return rt_dateonly_create(d->year, 1, 1);
}

/// @brief Return a new DateOnly for December 31 of the same year.
/// @param obj Source DateOnly; null returns `NULL`.
/// @return New DateOnly for December 31.
void *rt_dateonly_end_of_year(void *obj) {
    if (!obj)
        return NULL;
    DateOnly *d = as_dateonly(obj);
    return rt_dateonly_create(d->year, 12, 31);
}

//=============================================================================
// Comparison
//=============================================================================

/// @brief Compare two dates chronologically.
/// @details Converts both to days-since-epoch for a single integer comparison.
///          NULL is treated as "less than" any valid date, so NULL < any date.
/// @param a First DateOnly.
/// @param b Second DateOnly.
/// @return -1 if a is earlier, 0 if equal, 1 if a is later.
int64_t rt_dateonly_cmp(void *a, void *b) {
    if (!a && !b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;

    int64_t days_a = rt_dateonly_to_days(a);
    int64_t days_b = rt_dateonly_to_days(b);

    if (days_a < days_b)
        return -1;
    if (days_a > days_b)
        return 1;
    return 0;
}

/// @brief Check if two dates represent the same calendar day.
/// @param a First DateOnly.
/// @param b Second DateOnly.
/// @return 1 if both represent the same date or both are null, 0 otherwise.
int8_t rt_dateonly_equals(void *a, void *b) {
    return rt_dateonly_cmp(a, b) == 0 ? 1 : 0;
}

//=============================================================================
// Formatting
//=============================================================================

/// @brief Format the date as an ISO 8601 string (YYYY-MM-DD).
/// @details Emits exactly four year digits (e.g. "2026-03-29"). Every DateOnly is
///          constructed with a year in [0,9999] (rt_dateonly_create enforces that
///          domain), so the output is always a fixed ten-byte string that
///          `Parse` reads back — construction, formatting, and parsing share one
///          year domain (VDOC-231).
/// @param obj DateOnly object pointer; returns "" if NULL.
/// @return Newly allocated ISO string, or the shared empty-string constant for null.
rt_string rt_dateonly_to_string(void *obj) {
    if (!obj)
        return rt_const_cstr("");

    DateOnly *d = as_dateonly(obj);
    char buf[32];
    int len = snprintf(buf,
                       sizeof(buf),
                       "%04lld-%02lld-%02lld",
                       (long long)d->year,
                       (long long)d->month,
                       (long long)d->day);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        rt_trap("DateOnly.ToString: formatted output truncated");
        return rt_const_cstr("");
    }
    return rt_string_from_bytes(buf, (size_t)len);
}

/// @brief Format the date using a custom format string.
/// @details Supports strftime-style specifiers: %Y/%y (4-/2-digit year), %m
///          (2-digit month), %d (2-digit day), %B/%b (full/abbreviated month),
///          %A/%a (full/abbreviated weekday), and %j (3-digit day of year).
///          `%%` writes a literal percent; unknown specifiers and a trailing
///          percent are preserved literally.
/// @param obj DateOnly object pointer; returns "" if NULL.
/// @param fmt Format string containing specifiers.
/// @return Newly allocated formatted string, the shared empty-string constant
///         for a null receiver/format, or `NULL` after allocation failure.
rt_string rt_dateonly_format(void *obj, rt_string fmt) {
    if (!obj)
        return rt_const_cstr("");

    DateOnly *d = as_dateonly(obj);
    if (!fmt)
        return rt_const_cstr("");
    int64_t fmt_len = rt_str_len(fmt);
    if (fmt_len < 0)
        return rt_const_cstr("");
    const char *fmt_str = rt_string_cstr(fmt);
    if (!fmt_str && fmt_len > 0)
        return rt_const_cstr("");

    static const char *month_names[] = {"",
                                        "January",
                                        "February",
                                        "March",
                                        "April",
                                        "May",
                                        "June",
                                        "July",
                                        "August",
                                        "September",
                                        "October",
                                        "November",
                                        "December"};
    static const char *month_abbr[] = {
        "", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char *day_names[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *day_abbr[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    char *buf = NULL;
    size_t buf_cap = 0;
    size_t buf_len = 0;
    if (!dateonly_format_reserve(&buf, &buf_cap, 64))
        return NULL;
    buf[0] = '\0';

    for (int64_t i = 0; i < fmt_len; i++) {
        if (fmt_str[i] == '%' && i + 1 < fmt_len) {
            i++;
            char spec = fmt_str[i];
            char tmp[64];
            switch (spec) {
                case 'Y': // 4-digit year
                    snprintf(tmp, sizeof(tmp), "%04lld", (long long)d->year);
                    if (!dateonly_format_append_cstr(&buf, &buf_cap, &buf_len, tmp))
                        return NULL;
                    break;
                case 'y': // 2-digit year
                    snprintf(tmp, sizeof(tmp), "%02lld", (long long)(d->year % 100));
                    if (!dateonly_format_append_cstr(&buf, &buf_cap, &buf_len, tmp))
                        return NULL;
                    break;
                case 'm': // 2-digit month
                    snprintf(tmp, sizeof(tmp), "%02lld", (long long)d->month);
                    if (!dateonly_format_append_cstr(&buf, &buf_cap, &buf_len, tmp))
                        return NULL;
                    break;
                case 'd': // 2-digit day
                    snprintf(tmp, sizeof(tmp), "%02lld", (long long)d->day);
                    if (!dateonly_format_append_cstr(&buf, &buf_cap, &buf_len, tmp))
                        return NULL;
                    break;
                case 'B': // Full month name
                    if (d->month >= 1 && d->month <= 12 &&
                        !dateonly_format_append_cstr(
                            &buf, &buf_cap, &buf_len, month_names[d->month]))
                        return NULL;
                    break;
                case 'b': // Abbreviated month name
                    if (d->month >= 1 && d->month <= 12 &&
                        !dateonly_format_append_cstr(
                            &buf, &buf_cap, &buf_len, month_abbr[d->month]))
                        return NULL;
                    break;
                case 'A': // Full day name
                {
                    int64_t dow = rt_dateonly_day_of_week(obj);
                    if (dow >= 0 && dow <= 6 &&
                        !dateonly_format_append_cstr(&buf, &buf_cap, &buf_len, day_names[dow]))
                        return NULL;
                    break;
                }
                case 'a': // Abbreviated day name
                {
                    int64_t dow = rt_dateonly_day_of_week(obj);
                    if (dow >= 0 && dow <= 6 &&
                        !dateonly_format_append_cstr(&buf, &buf_cap, &buf_len, day_abbr[dow]))
                        return NULL;
                    break;
                }
                case 'j': // Day of year
                    snprintf(tmp, sizeof(tmp), "%03lld", (long long)rt_dateonly_day_of_year(obj));
                    if (!dateonly_format_append_cstr(&buf, &buf_cap, &buf_len, tmp))
                        return NULL;
                    break;
                case '%': // Literal %
                    if (!dateonly_format_append_char(&buf, &buf_cap, &buf_len, '%'))
                        return NULL;
                    break;
                default:
                    if (!dateonly_format_append_char(&buf, &buf_cap, &buf_len, '%') ||
                        !dateonly_format_append_char(&buf, &buf_cap, &buf_len, spec))
                        return NULL;
                    break;
            }
        } else {
            if (!dateonly_format_append_char(&buf, &buf_cap, &buf_len, fmt_str[i]))
                return NULL;
        }
    }

    rt_string out = rt_string_from_bytes(buf, buf_len);
    free(buf);
    return out;
}
