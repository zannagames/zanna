//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_reltime.c
// Purpose: Implements relative-time (human-readable elapsed/remaining time)
//          formatting for the Zanna runtime. Converts a pair of Unix timestamps
//          into natural-language descriptions such as "3 minutes ago",
//          "in 2 hours", or "just now".
//
// Key invariants:
//   - Long-form differences smaller than 10 seconds are "just now"; short
//     form uses "now".
//   - Unit thresholds: <60s=seconds, <3600s=minutes, <86400s=hours,
//     <2592000s=days, <31536000s=months, else=years.
//   - Months and years are fixed approximations of 30 and 365 days; displayed
//     unit values are truncated toward zero rather than rounded.
//   - Future timestamps produce "in N <unit>"; past timestamps produce
//     "N <unit> ago"; singular/plural is chosen correctly.
//   - Timestamp differences use unsigned arithmetic across the complete int64
//     domain. Duration magnitude saturates INT64_MIN to INT64_MAX.
//   - The reference timestamp is explicit (not hardcoded to time(NULL)) so
//     callers can pass a fixed reference for deterministic tests.
//
// Ownership/Lifetime:
//   - Returned rt_string values are newly allocated; the caller owns the
//     reference and must call rt_string_unref when done.
//   - Duration assembly uses bounded inline builder storage and releases it on
//     both success and failure; no input storage is borrowed.
//
// Links: src/runtime/core/rt_reltime.h (public API),
//        src/runtime/core/rt_datetime.c (wall-clock time operations),
//        src/runtime/core/rt_string_builder.c (string construction helper)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Human-readable relative timestamp and compact duration formatting.

#include "rt_reltime.h"
#include "rt_datetime.h"
#include "rt_internal.h"
#include "rt_string_builder.h"
#include "rt_trap.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// @brief Return the current Unix time in seconds.
/// @details Uses the shared checked wall-clock read so a genuine `time(NULL)`
///          failure (which returns `(time_t)-1`, aliasing the valid pre-epoch
///          instant) traps rather than being used as a bogus "now" that would
///          produce wildly wrong relative-time strings (VDOC-230).
/// @return Seconds since the Unix epoch, or zero after trapping if the wall
///         clock failure is handled by a returning trap hook.
static int64_t current_unix_seconds(void) {
    int64_t now;
    if (!rt_datetime_wall_seconds(&now)) {
        rt_trap("RelativeTime: system clock unavailable");
        return 0;
    }
    return now;
}

/// @brief Absolute value of @p x, saturating at `INT64_MAX` for `INT64_MIN`.
/// @details Avoids the well-known `-INT64_MIN` undefined-behaviour trap. Used by the
///          relative-time formatter when comparing two timestamps that may straddle
///          zero (e.g. epoch differences across the y2038 boundary on 32-bit hosts).
/// @param x Signed value whose magnitude is required.
/// @return `abs(x)` when representable, otherwise `INT64_MAX`.
static int64_t i64_abs(int64_t x) {
    if (x == INT64_MIN)
        return INT64_MAX; // -INT64_MIN is UB; saturate instead
    return x < 0 ? -x : x;
}

/// @brief Compute the unsigned magnitude and direction of `(timestamp - reference)`.
/// @details Splits the diff into a sign flag (`*in_future` is 1 iff @p timestamp is
///          strictly after @p reference) and a `uint64_t` magnitude. Doing the
///          subtraction in `uint64_t` avoids signed-overflow UB when the two values
///          are far apart in opposite directions of the int64 range.
/// @param timestamp Target Unix timestamp.
/// @param reference Baseline Unix timestamp.
/// @param in_future Required destination set to one only when @p timestamp is
///                  strictly greater than @p reference.
/// @param abs_diff Required destination for the exact unsigned distance.
static void reltime_diff(int64_t timestamp, int64_t reference, int *in_future, uint64_t *abs_diff) {
    if (timestamp >= reference) {
        *in_future = timestamp > reference;
        *abs_diff = (uint64_t)timestamp - (uint64_t)reference;
    } else {
        *in_future = 0;
        *abs_diff = (uint64_t)reference - (uint64_t)timestamp;
    }
}

/// @brief Append bytes to a relative-time builder and report append failure.
/// @details Relative-time formatting is intended to be allocation-light, but
///          builder growth can still fail for very large or memory-constrained
///          processes. This helper centralizes status checking so callers never
///          cast a failed `snprintf` length or ignored builder error into an
///          invalid output size.
/// @param sb Destination builder.
/// @param bytes Bytes to append.
/// @param len Number of bytes in @p bytes.
/// @return 1 on success, 0 when the builder rejected the append.
static int reltime_append_bytes_checked(rt_string_builder *sb, const char *bytes, size_t len) {
    return rt_sb_append_bytes(sb, bytes, len) == RT_SB_OK;
}

// ---------------------------------------------------------------------------
// rt_reltime_format_from
// ---------------------------------------------------------------------------

/// @brief Format the relative time between two timestamps as a human string.
/// @details Computes (timestamp - reference) and picks the best unit to express
///          the offset. Magnitudes below ten seconds return `just now`.
///          Thereafter thresholds are 60 seconds, 60 minutes, 24 hours, fixed
///          30-day months, and fixed 365-day years. The selected unit is floored.
///          Formatting failure produces an empty string; defensive truncation
///          preserves at most the first 127 bytes, although every valid int64
///          timestamp pair fits the local buffer.
/// @param timestamp The target Unix timestamp (seconds).
/// @param reference The baseline Unix timestamp to compare against.
/// @return Owned runtime string such as `3 minutes ago`, `in 2 hours`, or
///         `just now`; allocation failure follows @ref rt_string_from_bytes.
rt_string rt_reltime_format_from(int64_t timestamp, int64_t reference) {
    uint64_t abs_diff;
    int in_future;
    reltime_diff(timestamp, reference, &in_future, &abs_diff);

    const char *unit = NULL;
    uint64_t value = 0;

    if (abs_diff < 10) {
        return rt_string_from_bytes("just now", 8);
    } else if (abs_diff < 60) {
        value = abs_diff;
        unit = "seconds";
    } else if (abs_diff < 3600) {
        value = abs_diff / 60;
        unit = value == 1 ? "minute" : "minutes";
    } else if (abs_diff < 86400) {
        value = abs_diff / 3600;
        unit = value == 1 ? "hour" : "hours";
    } else if (abs_diff < 2592000) // ~30 days
    {
        value = abs_diff / 86400;
        unit = value == 1 ? "day" : "days";
    } else if (abs_diff < 31536000) // ~365 days
    {
        value = abs_diff / 2592000;
        unit = value == 1 ? "month" : "months";
    } else {
        value = abs_diff / 31536000;
        unit = value == 1 ? "year" : "years";
    }

    char buf[128];
    int len;
    if (in_future)
        len = snprintf(buf, sizeof(buf), "in %llu %s", (unsigned long long)value, unit);
    else
        len = snprintf(buf, sizeof(buf), "%llu %s ago", (unsigned long long)value, unit);

    if (len < 0)
        len = 0;
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;
    return rt_string_from_bytes(buf, (size_t)len);
}

// ---------------------------------------------------------------------------
// rt_reltime_format
// ---------------------------------------------------------------------------

/// @brief Format a Unix timestamp as a relative time string from now.
/// @details Computes the difference from the current time and delegates to
///          @ref rt_reltime_format_from. A wall-clock read failure traps; if the
///          trap hook returns, epoch zero is used as the reference fallback.
/// @param timestamp Unix timestamp in seconds.
/// @return Owned relative-time string.
rt_string rt_reltime_format(int64_t timestamp) {
    return rt_reltime_format_from(timestamp, current_unix_seconds());
}

// ---------------------------------------------------------------------------
// rt_reltime_format_duration
// ---------------------------------------------------------------------------

/// @brief Format a millisecond duration as compact whole-second components.
/// @details Emits nonzero day/hour/minute/second fields such as `1d 5h 20m`.
///          The millisecond remainder is discarded, so a negative magnitude that
///          truncates to zero whole seconds renders as plain `0s` — the sign is
///          only emitted when the displayed whole-second magnitude is nonzero, so
///          there is no `-0s` (VDOC-227). Zero intermediate and trailing
///          components are omitted, while a wholly sub-second duration emits
///          `0s`. `INT64_MIN` magnitude is saturated by one millisecond to avoid
///          signed overflow. Any builder or local-format failure releases
///          temporary storage and returns an owned empty string.
/// @param duration_ms Duration in milliseconds.
/// @return Owned compact duration string.
rt_string rt_reltime_format_duration(int64_t duration_ms) {
    int64_t abs_ms = i64_abs(duration_ms);

    int64_t total_secs = abs_ms / 1000;
    // Only sign a nonzero displayed magnitude; a sub-second negative input
    // truncates to "0s", which must not carry a leading "-" (VDOC-227).
    int negative = duration_ms < 0 && total_secs > 0;
    int64_t days = total_secs / 86400;
    int64_t hours = (total_secs % 86400) / 3600;
    int64_t minutes = (total_secs % 3600) / 60;
    int64_t seconds = total_secs % 60;

    rt_string_builder sb;
    rt_sb_init(&sb);

    if (negative && rt_sb_append_cstr(&sb, "-") != RT_SB_OK)
        goto format_error;

    int has_prev = 0;
    if (days > 0) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%lldd", (long long)days);
        if (len < 0 || (size_t)len >= sizeof(buf) ||
            !reltime_append_bytes_checked(&sb, buf, (size_t)len))
            goto format_error;
        has_prev = 1;
    }
    if (hours > 0) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%s%lldh", has_prev ? " " : "", (long long)hours);
        if (len < 0 || (size_t)len >= sizeof(buf) ||
            !reltime_append_bytes_checked(&sb, buf, (size_t)len))
            goto format_error;
        has_prev = 1;
    }
    if (minutes > 0) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%s%lldm", has_prev ? " " : "", (long long)minutes);
        if (len < 0 || (size_t)len >= sizeof(buf) ||
            !reltime_append_bytes_checked(&sb, buf, (size_t)len))
            goto format_error;
        has_prev = 1;
    }
    if (seconds > 0 || !has_prev) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%s%llds", has_prev ? " " : "", (long long)seconds);
        if (len < 0 || (size_t)len >= sizeof(buf) ||
            !reltime_append_bytes_checked(&sb, buf, (size_t)len))
            goto format_error;
    }

    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;

format_error:
    rt_sb_free(&sb);
    return rt_string_from_bytes("", 0);
}

// ---------------------------------------------------------------------------
// rt_reltime_format_short
// ---------------------------------------------------------------------------

/// @brief Format a timestamp as a short relative time from now.
/// @details Uses the same unsigned difference and fixed thresholds as the long
///          formatter, but emits `s`, `m`, `h`, `d`, `mo`, or `y` suffixes and
///          `now` below ten seconds. The selected value is floored. A wall-clock
///          failure traps and falls back to epoch zero if the hook returns.
///          Local formatting errors return an owned empty string; defensive
///          buffer truncation retains a valid prefix.
/// @param timestamp Unix timestamp in seconds.
/// @return Owned abbreviated relative-time string.
rt_string rt_reltime_format_short(int64_t timestamp) {
    int64_t now = current_unix_seconds();
    uint64_t abs_diff;
    int in_future;
    reltime_diff(timestamp, now, &in_future, &abs_diff);

    char amount[32];
    char buf[64];
    int len;

    if (abs_diff < 10) {
        return rt_string_from_bytes("now", 3);
    } else if (abs_diff < 60) {
        len = snprintf(amount, sizeof(amount), "%llus", (unsigned long long)abs_diff);
    } else if (abs_diff < 3600) {
        len = snprintf(amount, sizeof(amount), "%llum", (unsigned long long)(abs_diff / 60));
    } else if (abs_diff < 86400) {
        len = snprintf(amount, sizeof(amount), "%lluh", (unsigned long long)(abs_diff / 3600));
    } else if (abs_diff < 2592000) {
        len = snprintf(amount, sizeof(amount), "%llud", (unsigned long long)(abs_diff / 86400));
    } else if (abs_diff < 31536000) {
        len = snprintf(amount, sizeof(amount), "%llumo", (unsigned long long)(abs_diff / 2592000));
    } else {
        len = snprintf(amount, sizeof(amount), "%lluy", (unsigned long long)(abs_diff / 31536000));
    }

    if (len < 0)
        return rt_string_from_bytes("", 0);
    if ((size_t)len >= sizeof(amount))
        amount[sizeof(amount) - 1] = '\0';

    if (in_future) {
        len = snprintf(buf, sizeof(buf), "in %s", amount);
    } else {
        len = snprintf(buf, sizeof(buf), "%s ago", amount);
    }

    if (len < 0)
        len = 0;
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;
    return rt_string_from_bytes(buf, (size_t)len);
}
