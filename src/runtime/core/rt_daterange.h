//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_daterange.h
/// @file
/// @brief Declares the GC-managed closed DateRange timestamp interval API.
///
// Purpose: Date interval type representing a range between two Unix timestamps, with containment
// testing, overlap detection, and duration queries.
//
// Key invariants:
//   - Start and end are Unix timestamps in seconds since epoch (UTC).
//   - Constructor normalizes reversed inputs so Start <= End.
//   - Contains check is inclusive on both ends.
//   - Duration returns end - start in seconds; may be zero for point ranges and traps on overflow.
//
// Ownership/Lifetime:
//   - DateRange objects are GC-managed opaque pointers.
//   - Callers must not free range objects directly.
//   - Null receivers produce the documented zero/false/null sentinel; nonnull
//     objects of another runtime class raise a trap.
//
// Links: src/runtime/core/rt_daterange.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID for DateRange instances.
/// @details Stamped by rt_obj_new_i64 at construction and verified by the shared
///          receiver guard so an explicit receiver of another class traps instead
///          of being reinterpreted as a DateRange payload (VDOC-229).
#define RT_DATERANGE_CLASS_ID INT64_C(-0x430804)

/// @brief Create a date range from start and end timestamps.
/// @param start Start timestamp in seconds.
/// @param end End timestamp in seconds.
/// @return New normalized DateRange, or `NULL` after an allocation trap.
void *rt_daterange_new(int64_t start, int64_t end);

/// @brief Get start timestamp.
/// @param range Date range object.
/// @return Start timestamp in seconds.
/// @note Null returns zero, which is indistinguishable from the Unix epoch.
int64_t rt_daterange_start(void *range);

/// @brief Get end timestamp.
/// @param range Date range object.
/// @return End timestamp in seconds.
/// @note Null returns zero, which is indistinguishable from the Unix epoch.
int64_t rt_daterange_end(void *range);

/// @brief Check if a timestamp falls within the range (inclusive).
/// @param range Date range object.
/// @param timestamp Timestamp to check.
/// @return 1 if contained, 0 otherwise.
int8_t rt_daterange_contains(void *range, int64_t timestamp);

/// @brief Check if two ranges overlap.
/// @param range First date range.
/// @param other Second date range.
/// @return 1 if they overlap, 0 otherwise.
int8_t rt_daterange_overlaps(void *range, void *other);

/// @brief Get the intersection of two ranges.
/// @param range First date range.
/// @param other Second date range.
/// @return Intersection range, or NULL if no overlap.
/// @note Touching endpoints produce a point range.
void *rt_daterange_intersection(void *range, void *other);

/// @brief Get the union of two ranges.
/// @details Returns NULL if ranges are neither overlapping nor adjacent at
///          consecutive integer-second timestamps.
/// @param range First date range.
/// @param other Second date range.
/// @return Union range, or NULL if not contiguous.
void *rt_daterange_union_range(void *range, void *other);

/// @brief Get the number of days in the range.
/// @param range Date range object.
/// @return Whole elapsed 86400-second units. Traps on signed 64-bit overflow.
int64_t rt_daterange_days(void *range);

/// @brief Get the number of hours in the range.
/// @param range Date range object.
/// @return Whole elapsed 3600-second units. Traps on signed 64-bit overflow.
int64_t rt_daterange_hours(void *range);

/// @brief Get the duration of the range in seconds.
/// @param range Date range object.
/// @return Duration in seconds. Traps on signed 64-bit overflow.
int64_t rt_daterange_duration(void *range);

/// @brief Format range as a string.
/// @param range Date range object.
/// @return New UTC string like "2025-01-01 00:00 - 2025-01-31 23:59", or a
///         new empty string if null/unrepresentable by platform time APIs.
/// @note Extremely wide platform years may be truncated to the fixed buffer.
rt_string rt_daterange_to_string(void *range);

#ifdef __cplusplus
}
#endif
