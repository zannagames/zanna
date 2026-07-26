//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/system/rt_machine_linux_helpers.h
// Purpose: Testable parsing helpers for Linux machine and cgroup controls.
//
// Key invariants:
//   - Unsigned controls reject signs, overflow, and trailing non-whitespace.
//   - CPU sets are ordered, non-overlapping, non-empty ranges.
//
// Ownership/Lifetime:
//   - Header-only parsing over caller-owned immutable strings.
//
// Links: src/runtime/system/rt_machine.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_machine_linux_helpers.h
 * @brief Provides header-only parsers for Linux machine resource controls.
 * @details These pure helpers parse strict unsigned control values and
 *          canonical ordered CPU-set lists from borrowed immutable strings,
 *          rejecting signs, trailing syntax, overlaps, empty ranges, and
 *          arithmetic overflow before publishing results.
 */

#pragma once

#include <limits.h>
#include <stdint.h>

/// @brief Parse one nonempty unsigned decimal component and advance a cursor.
/// @details Consumes consecutive ASCII digits only, rejects arithmetic
///          overflow, and publishes neither cursor nor value on failure.
/// @param cursor_inout Address of the current borrowed input pointer; advanced
///        to the first nondigit on success.
/// @param out Receives the parsed unsigned value on success.
/// @return 1 when at least one digit was parsed without overflow, otherwise 0.
static inline int rt_machine_linux_parse_decimal_component(const char **cursor_inout,
                                                           unsigned long long *out) {
    const char *cursor = cursor_inout ? *cursor_inout : NULL;
    if (!cursor || !out || *cursor < '0' || *cursor > '9')
        return 0;
    unsigned long long value = 0;
    do {
        unsigned int digit = (unsigned int)(*cursor - '0');
        if (value > (ULLONG_MAX - digit) / 10u)
            return 0;
        value = value * 10u + digit;
        cursor++;
    } while (*cursor >= '0' && *cursor <= '9');
    *cursor_inout = cursor;
    *out = value;
    return 1;
}

/// @brief Parse a complete unsigned decimal Linux control value.
/// @details Accepts trailing spaces or horizontal tabs but rejects signs,
///          leading whitespace, overflow, and every other trailing character.
/// @param text Borrowed NUL-terminated control text.
/// @param out Receives the parsed value on success.
/// @return 1 when all significant input is a representable decimal integer,
///         otherwise 0 without publishing @p out.
static inline int rt_machine_linux_parse_u64(const char *text, unsigned long long *out) {
    const char *cursor = text;
    unsigned long long value = 0;
    if (!rt_machine_linux_parse_decimal_component(&cursor, &value))
        return 0;
    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    if (*cursor != '\0')
        return 0;
    *out = value;
    return 1;
}

/// @brief Count CPUs described by a canonical Linux cpuset list.
/// @details Accepts comma-separated decimal indices and inclusive ranges. The
///          entries must be nonempty, strictly ordered, nonoverlapping, and
///          representable; whitespace is not accepted.
/// @param text Borrowed NUL-terminated cpuset expression.
/// @return Positive CPU count when valid and representable as int64_t, otherwise
///         0 for empty, malformed, overlapping, unordered, or overflowing input.
static inline int64_t rt_machine_linux_count_cpuset(const char *text) {
    if (!text || text[0] == '\0')
        return 0;
    unsigned long long count = 0;
    unsigned long long previous_last = 0;
    int have_previous = 0;
    const char *cursor = text;
    while (*cursor) {
        unsigned long long first = 0;
        if (!rt_machine_linux_parse_decimal_component(&cursor, &first))
            return 0;

        unsigned long long last = first;
        if (*cursor == '-') {
            cursor++;
            if (!rt_machine_linux_parse_decimal_component(&cursor, &last) || last < first)
                return 0;
        }
        if (have_previous && first <= previous_last)
            return 0;
        unsigned long long width = last - first;
        if (width == ULLONG_MAX)
            return 0;
        unsigned long long span = width + 1u;
        if (ULLONG_MAX - count < span)
            return 0;
        count += span;
        previous_last = last;
        have_previous = 1;
        if (*cursor == '\0')
            break;
        if (*cursor != ',')
            return 0;
        cursor++;
        if (*cursor == '\0')
            return 0;
    }
    return count > 0 && count <= (unsigned long long)INT64_MAX ? (int64_t)count : 0;
}

/// @brief Convert a cgroup CPU quota and period to a logical-CPU ceiling.
/// @details Computes the mathematical ceiling of quota divided by period so a
///          positive fractional CPU allocation still permits one logical CPU.
/// @param quota Positive quota duration from a cgroup CPU control.
/// @param period Positive scheduling period from the matching control.
/// @return Rounded-up logical-CPU count, or 0 when either input is zero or the
///         result cannot be represented as int64_t.
static inline int64_t rt_machine_linux_cpu_quota(unsigned long long quota,
                                                unsigned long long period) {
    if (quota == 0 || period == 0)
        return 0;
    unsigned long long rounded = quota / period + (quota % period != 0);
    return rounded <= (unsigned long long)INT64_MAX ? (int64_t)rounded : 0;
}
