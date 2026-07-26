//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_regex.h
// Purpose: Declares the static Zanna.Text.Pattern byte-oriented regex API for
//          searching, collecting, replacing, splitting, and literal escaping.
//
// Key invariants:
//   - Patterns are compiled and cached internally for repeated use.
//   - Supported syntax includes ., ^, $, [...], \d/\w/\s and complements,
//     *, +, ?, lazy *?/+?/??, capture groups, and alternation.
//   - Bounded quantifiers, backreferences, lookaround, and named groups are
//     unsupported.
//   - Matching and returned positions operate on bytes, not Unicode codepoints.
//   - Required patterns reject embedded null bytes; subject and replacement
//     strings use their declared runtime byte lengths.
//
// Ownership/Lifetime:
//   - Returned Seqs and Options are caller-owned. Match and transformation
//     strings follow normal runtime-string ownership; no-match string APIs use
//     an empty-string sentinel.
//   - Input strings are borrowed for the duration of matching.
//
// Links: src/runtime/text/rt_regex.c (implementation), src/runtime/text/rt_regex_internal.h,
// src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_regex.h
 * @brief Declares the static byte-oriented regular-expression operations.
 * @details Borrowed subject and pattern Strings support matching, leftmost
 *          search, captures, complete nonoverlapping enumeration, literal
 *          replacement, splitting, and regex escaping. Returned Options,
 *          sequences, and transformation Strings follow caller-owned runtime
 *          conventions.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Test whether a pattern matches anywhere in a byte string.
/// @details A null subject is treated as empty. Compilation is cached.
/// @param text Borrowed subject text.
/// @param pattern Borrowed required regex pattern.
/// @return `1` when any match exists, otherwise `0`.
/// @note Traps on a null, embedded-null, or syntactically invalid pattern.
int8_t rt_pattern_is_match(rt_string text, rt_string pattern);

/// @brief Find the first leftmost match.
/// @details The empty return value is ambiguous with a successful zero-width
///          match; use `rt_pattern_find_option` when absence matters.
/// @param text Borrowed subject text.
/// @param pattern Borrowed required regex pattern.
/// @return First matching substring, or an empty-string sentinel if absent.
/// @note Traps on an invalid pattern.
rt_string rt_pattern_find(rt_string text, rt_string pattern);

/// @brief Find first match of pattern in text as an Option string.
/// @details Returns `SomeStr(match)` for any match, including an empty-string
///          match, and `None` when no match exists.
/// @param text Borrowed subject text.
/// @param pattern Borrowed required regex pattern.
/// @return Caller-owned Option containing the first match, or `None`.
/// @note Traps on an invalid pattern or Option allocation failure.
void *rt_pattern_find_option(rt_string text, rt_string pattern);

/// @brief Find the first match at or after a byte offset.
/// @details Negative offsets clamp to zero; offsets beyond the subject return
///          the empty no-match sentinel.
/// @param text Borrowed subject text.
/// @param pattern Borrowed required regex pattern.
/// @param start Zero-based starting byte offset.
/// @return First matching substring at or after @p start, or an empty sentinel.
/// @note Traps on an invalid pattern.
rt_string rt_pattern_find_from(rt_string text, rt_string pattern, int64_t start);

/// @brief Find first match at or after a position as an Option string.
/// @details Negative offsets clamp to zero and offsets beyond the subject
///          produce `None`; zero-width matches remain representable.
/// @param text Borrowed subject text.
/// @param pattern Borrowed required regex pattern.
/// @param start Zero-based starting byte offset.
/// @return Caller-owned Option containing the first match, or `None`.
/// @note Traps on an invalid pattern or Option allocation failure.
void *rt_pattern_find_from_option(rt_string text, rt_string pattern, int64_t start);

/// @brief Find the byte position of the first match.
/// @param text Borrowed subject text.
/// @param pattern Borrowed required regex pattern.
/// @return Starting byte offset, or `-1` if no match exists.
/// @note Traps on an invalid pattern.
int64_t rt_pattern_find_pos(rt_string text, rt_string pattern);

/// @brief Find position of first match as an Option index.
/// @param text Borrowed subject text.
/// @param pattern Borrowed required regex pattern.
/// @return Caller-owned Option containing the starting byte offset, or `None`.
/// @note Traps on an invalid pattern or Option allocation failure.
void *rt_pattern_find_pos_option(rt_string text, rt_string pattern);

/// @brief Find all non-overlapping matches from left to right.
/// @details Search advances one byte after zero-width matches to ensure
///          progress, while preserving a possible end-of-text match.
/// @param text Borrowed subject text.
/// @param pattern Borrowed required regex pattern.
/// @return Caller-owned Seq that owns its matching substring elements.
/// @note Traps on an invalid pattern or allocation failure.
void *rt_pattern_find_all(rt_string text, rt_string pattern);

/// @brief Replace every non-overlapping match with literal bytes.
/// @details Capture-group substitutions are not interpreted. Zero-width
///          replacement preserves the source byte stepped over for progress.
/// @param text Borrowed subject text.
/// @param pattern Borrowed required regex pattern.
/// @param replacement Borrowed literal replacement text.
/// @return Newly allocated transformed string.
/// @note Traps on an invalid pattern, size overflow, or allocation failure.
rt_string rt_pattern_replace(rt_string text, rt_string pattern, rt_string replacement);

/// @brief Replace only the first match with literal bytes.
/// @param text Borrowed subject text.
/// @param pattern Borrowed required regex pattern.
/// @param replacement Borrowed literal replacement text.
/// @return Newly allocated transformed string, or a new copy of @p text when
///         no match exists.
/// @note Traps on an invalid pattern, size overflow, or allocation failure.
rt_string rt_pattern_replace_first(rt_string text, rt_string pattern, rt_string replacement);

/// @brief Split text at non-overlapping pattern matches.
/// @details A delimiter at the end produces an empty trailing segment.
///          Zero-width delimiters are handled without losing source bytes.
/// @param text Borrowed subject text.
/// @param pattern Borrowed required delimiter pattern.
/// @return Caller-owned Seq that owns the substring segments.
/// @note Traps on an invalid pattern or allocation failure.
void *rt_pattern_split(rt_string text, rt_string pattern);

/// @brief Escape regex metacharacters so text can be matched literally.
/// @param text Borrowed byte string to escape; null is treated as empty.
/// @return Newly allocated escaped pattern text.
rt_string rt_pattern_escape(rt_string text);

#ifdef __cplusplus
}
#endif
