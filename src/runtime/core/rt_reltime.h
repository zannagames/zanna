//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_reltime.h
// Purpose: Declares English human-readable relative timestamp and compact
// whole-second duration formatting.
//
// Key invariants:
//   - Input timestamps are Unix timestamps in seconds since epoch (UTC).
//   - rt_reltime_format computes relative to the current wall-clock time.
//   - rt_reltime_format_from uses an explicit reference timestamp for deterministic output.
//   - Short variants produce compact representations suitable for UI labels.
//   - Relative units use fixed 30-day months and 365-day years and floor the
//     selected unit; durations discard sub-second remainders.
//
// Ownership/Lifetime:
//   - Returned strings carry an owned initial reference and must be released
//     with rt_string_unref.
//   - Input timestamps are plain int64_t values with no ownership transfer.
//
// Links: src/runtime/core/rt_reltime.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Relative timestamp and compact duration formatting API.
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Format timestamp relative to current time.
/// @details Delegates to @ref rt_reltime_format_from after a checked wall-clock
///          read. A clock failure traps and uses epoch zero if the trap hook
///          returns.
/// @param timestamp Unix timestamp in seconds.
/// @return Owned string such as `2 hours ago`, `in 3 days`, or `just now`.
rt_string rt_reltime_format(int64_t timestamp);

/// @brief Format timestamp relative to a reference time.
/// @details Uses absolute thresholds of 10 seconds, 60 seconds, 60 minutes,
///          24 hours, 30 days, and 365 days. Values are floored in the selected
///          unit. Future values use `in N units`; equal/past values use
///          `N units ago`, except sub-ten-second differences use `just now`.
/// @param timestamp Unix timestamp in seconds.
/// @param reference Reference Unix timestamp in seconds.
/// @return Owned human-readable relative-time string.
rt_string rt_reltime_format_from(int64_t timestamp, int64_t reference);

/// @brief Format a duration in milliseconds as human-readable.
/// @details Produces compact nonzero day/hour/minute/second components such as
///          `2h 30m`, `1d 5h 20m`, or `45s`. Sub-second remainders are
///          discarded, zero components are omitted, and negative values with
///          a nonzero whole-second magnitude receive one leading minus sign.
///          Zero and sub-second magnitudes render as `0s`.
/// @param duration_ms Duration in milliseconds.
/// @return Owned duration string, or an owned empty string on local formatting
///         or builder failure.
rt_string rt_reltime_format_duration(int64_t duration_ms);

/// @brief Format timestamp relative to now in short form.
/// @details Uses the long formatter's thresholds with the suffixes `s`, `m`,
///          `h`, `d`, `mo`, and `y`. Differences below ten seconds render as
///          `now`. A checked clock-read failure has the same trap/fallback
///          behavior as @ref rt_reltime_format.
/// @param timestamp Unix timestamp in seconds.
/// @return Owned short relative-time string such as `2h ago` or `in 3d`.
rt_string rt_reltime_format_short(int64_t timestamp);

#ifdef __cplusplus
}
#endif
