//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_parse.h
// Purpose: Declares strict parsing functions for Zanna.Core.Parse, including
//          TryXxx, XxxOr, IsXxx, arbitrary-radix, and Option-returning variants.
//
// Key invariants:
//   - TryXxx functions store the parsed value at a pointer and return true on success.
//   - XxxOr functions return the parsed value or a caller-specified default on failure.
//   - IsXxx functions return true when the string is a valid representation of the type.
//   - Ordinary invalid input is reported without a parsing trap.
//   - Decimal floating-point syntax always uses a period, independent of the
//     embedding process's numeric locale.
//   - Embedded null bytes and non-whitespace trailing content are rejected.
//
// Ownership/Lifetime:
//   - Input strings are borrowed; callers retain ownership.
//   - Option-returning functions allocate caller-owned Option objects and may
//     trap on allocation failure; other functions allocate no runtime objects.
//
// Links: src/runtime/text/rt_parse.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_parse.h
 * @brief Declares strict Try, defaulting, predicate, radix, and Option parsers.
 * @details Borrowed runtime Strings are parsed without ordinary syntax traps;
 *          Try functions initialize caller outputs and return status, default
 *          and predicate forms allocate nothing, and Option forms return
 *          caller-owned managed presence values.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Try to parse a signed 64-bit decimal integer.
/// @details Allows leading and trailing ASCII whitespace but rejects empty
///          input, embedded nulls, overflow, and non-whitespace suffixes.
///          Resets @p out_value to zero before validating @p s.
/// @param s Borrowed runtime string to parse.
/// @param out_value Destination for the parsed integer; must be non-null.
/// @return `1` if parsing succeeds, otherwise `0`.
int8_t rt_parse_try_int(rt_string s, int64_t *out_value);

/// @brief Try to parse a number from string, storing result at out_value.
/// @details Accepts finite decimal values plus explicit NaN/Inf spellings.
///          Decimal overflow and non-finite decimal results fail; finite
///          underflow to zero/subnormal values is accepted. Syntax and
///          conversion use a C numeric locale, and @p out_value is reset to
///          zero before validation.
/// @param s Borrowed runtime string to parse.
/// @param out_value Destination for the parsed double; must be non-null.
/// @return `1` if parsing succeeds, otherwise `0`.
int8_t rt_parse_try_num(rt_string s, double *out_value);

/// @brief Try to parse a case-insensitive Boolean token.
/// @details Accepts `true`, `yes`, `1`, or `on` for true and `false`, `no`,
///          `0`, or `off` for false, surrounded only by ASCII whitespace.
///          Resets @p out_value to zero before validation.
/// @param s Borrowed runtime string to parse.
/// @param out_value Destination for normalized zero or one; must be non-null.
/// @return `1` if parsing succeeds, otherwise `0`.
int8_t rt_parse_try_bool(rt_string s, int8_t *out_value);

/// @brief Parse an integer from string, returning default on failure.
/// @details Uses the same strict decimal syntax as `rt_parse_try_int`.
/// @param s Borrowed runtime string to parse.
/// @param default_value Value to return if parsing fails.
/// @return Parsed integer or @p default_value.
int64_t rt_parse_int_or(rt_string s, int64_t default_value);

/// @brief Parse a number from string, returning default on failure.
/// @details Uses the same syntax, special values, and C-locale conversion as
///          `rt_parse_try_num`.
/// @param s Borrowed runtime string to parse.
/// @param default_value Value to return if parsing fails.
/// @return Parsed double or @p default_value.
double rt_parse_num_or(rt_string s, double default_value);

/// @brief Parse a boolean from string, returning default on failure.
/// @details Uses the same token set as `rt_parse_try_bool`.
/// @param s Borrowed runtime string to parse.
/// @param default_value Value to return if parsing fails.
/// @return Parsed normalized Boolean or @p default_value unchanged.
int8_t rt_parse_bool_or(rt_string s, int8_t default_value);

/// @brief Check if string represents a valid integer.
/// @details Performs the same validation as `rt_parse_try_int` and discards the
///          parsed value.
/// @param s Borrowed runtime string to check.
/// @return `1` if @p s can be parsed as an integer, otherwise `0`.
int8_t rt_parse_is_int(rt_string s);

/// @brief Check if string represents a valid number.
/// @details Performs the same validation as `rt_parse_try_num` and discards the
///          parsed value.
/// @param s Borrowed runtime string to check.
/// @return `1` if @p s can be parsed as a number, otherwise `0`.
int8_t rt_parse_is_num(rt_string s);

/// @brief Parse an integer with specified radix, returning default on failure.
/// @details Leading `+` and `-` signs are accepted only for decimal input.
///          Non-decimal radices parse unsigned 64-bit bit patterns so
///          Fmt.Hex/Fmt.Bin negative outputs round-trip when reinterpreted as
///          `int64_t`. Prefixes such as `0x` are rejected.
/// @param s Borrowed runtime string to parse.
/// @param radix Base in the inclusive range `[2, 36]`.
/// @param default_value Value to return if parsing fails or radix is invalid.
/// @return Parsed integer or @p default_value.
int64_t rt_parse_int_radix(rt_string s, int64_t radix, int64_t default_value);

/// @brief Parse a number into Option<f64>; returns None on failure.
/// @details Option construction allocates and may trap on allocation failure.
/// @param s Borrowed runtime string to parse.
/// @return Caller-owned `Zanna.Option.SomeF64(parsed)` or `Zanna.Option.None()`.
void *rt_parse_double_option(rt_string s);

/// @brief Parse an integer into Option<i64>; returns None on failure.
/// @details Option construction allocates and may trap on allocation failure.
/// @param s Borrowed runtime string to parse.
/// @return Caller-owned `Zanna.Option.SomeI64(parsed)` or `Zanna.Option.None()`.
void *rt_parse_int64_option(rt_string s);

/// @brief Parse a boolean into Option<i1>; returns None on failure.
/// @details Option construction allocates and may trap on allocation failure.
/// @param s Borrowed runtime string to parse.
/// @return Caller-owned `Zanna.Option.SomeI1(parsed)` or `Zanna.Option.None()`.
void *rt_parse_bool_option(rt_string s);

#ifdef __cplusplus
}
#endif
