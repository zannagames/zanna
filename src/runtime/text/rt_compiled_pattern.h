//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_compiled_pattern.h
// Purpose: Pre-compiled regular expression pattern for efficient repeated matching without per-call
// recompilation overhead, supporting find, capture, replace, and split.
//
// Key invariants:
//   - The constructor traps on invalid syntax; TryNew returns a diagnostic Result.
//   - Compiled patterns are opaque objects managed by the runtime object system.
//   - Match results use Seq containers for multi-capture returns.
//   - Replace and ReplaceFirst perform literal replacement through separate APIs.
//   - Positions and start arguments are byte offsets, not Unicode code-point indices.
//
// Ownership/Lifetime:
//   - CompiledPattern objects are runtime-managed; caller does not need to free them.
//   - Returned strings, Seqs, and Option objects are caller-owned runtime values.
//
// Links: src/runtime/text/rt_compiled_pattern.c (implementation),
// src/runtime/text/rt_regex_internal.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_compiled_pattern.h
 * @brief Declares reusable managed compiled-regex operations.
 * @details The opaque pattern retains immutable compiled state and supports
 *          byte-offset matching, search, captures, complete match enumeration,
 *          literal replacement, and splitting. Returned Strings, Options, and
 *          sequences are caller-owned managed values.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Creation and Lifecycle
//=============================================================================

/// @brief Compile a regex pattern for repeated use.
/// @param pattern Required regex source string without embedded NUL bytes.
/// @return New GC-managed opaque CompiledPattern object, or NULL after a
///         validation, allocation, or syntax trap.
void *rt_compiled_pattern_new(rt_string pattern);

/// @brief Compile a regex without trapping for malformed user syntax.
/// @param pattern Required regex source string without embedded NUL bytes.
/// @param case_insensitive Nonzero enables deterministic ASCII case folding.
/// @return Caller-owned Result containing a CompiledPattern or an error string.
void *rt_compiled_pattern_try_new(rt_string pattern, int8_t case_insensitive);

/// @brief Get the original pattern string.
/// @param obj CompiledPattern object, or NULL.
/// @return Newly allocated copy of the source pattern, or an empty constant
///         string for NULL.
rt_string rt_compiled_pattern_get_pattern(void *obj);

//=============================================================================
// Matching Operations
//=============================================================================

/// @brief Test if this pattern matches anywhere in text.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @return 1 if the pattern matches anywhere, otherwise 0.
int8_t rt_compiled_pattern_is_match(void *obj, rt_string text);

/// @brief Find first match of this pattern in text.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @return Newly allocated first matching substring, or empty when no match.
/// @note Use rt_compiled_pattern_find_option() to distinguish absence from a
///       valid zero-width match.
rt_string rt_compiled_pattern_find(void *obj, rt_string text);

/// @brief Find first match as an Option string.
/// @details Returns `SomeStr(match)` for any match, including an empty-string
///          match, and `None` when no match exists.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @return Caller-owned Zanna.Option containing the first match, or None.
void *rt_compiled_pattern_find_option(void *obj, rt_string text);

/// @brief Find first match starting at or after given position.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @param start Starting byte offset; negative values clamp to zero.
/// @return Newly allocated first match at/after the offset, or empty when absent.
rt_string rt_compiled_pattern_find_from(void *obj, rt_string text, int64_t start);

/// @brief Find first match at or after given position as an Option string.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @param start Starting byte offset; negative values clamp to zero.
/// @return Caller-owned Zanna.Option containing the first match, or None.
void *rt_compiled_pattern_find_from_option(void *obj, rt_string text, int64_t start);

/// @brief Find position of first match.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @return Start byte offset of the first match, or -1 if none.
int64_t rt_compiled_pattern_find_pos(void *obj, rt_string text);

/// @brief Find position of first match as an Option index.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @return Caller-owned Zanna.Option containing the first byte offset, or None.
void *rt_compiled_pattern_find_pos_option(void *obj, rt_string text);

/// @brief Find one match range at or after a byte offset.
/// @details The returned sequence is empty for no match, otherwise contains
///          `[start, end, resume]` as boxed byte offsets. `resume` equals end
///          for consuming matches and advances one complete UTF-8 scalar for a
///          zero-width match. Whole-word checks use deterministic Unicode 16.0
///          letter/mark/number/connector rules.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @param start Zero-based starting byte offset; negatives clamp to zero.
/// @param whole_word Nonzero rejects matches touching Unicode word scalars.
/// @return Caller-owned owning Seq of zero or three boxed integers.
void *rt_compiled_pattern_find_range_from(void *obj,
                                          rt_string text,
                                          int64_t start,
                                          int8_t whole_word);

/// @brief Find all non-overlapping matches.
/// @details Zero-width matches advance one byte to guarantee progress.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @return Caller-owned element-owning Seq of newly allocated nonoverlapping matches.
void *rt_compiled_pattern_find_all(void *obj, rt_string text);

//=============================================================================
// Capture Groups
//=============================================================================

/// @brief Find first match and return capture groups.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @return Caller-owned Seq with full match at index zero followed by capture
///         groups, or an empty Seq if no match.
void *rt_compiled_pattern_captures(void *obj, rt_string text);

/// @brief Find first match at/after start and return capture groups.
/// @details A capture that did not participate is represented by an empty string.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @param start Starting byte offset; negative values clamp to zero.
/// @return Caller-owned Seq with full match then capture groups, or an empty Seq.
void *rt_compiled_pattern_captures_from(void *obj, rt_string text, int64_t start);

//=============================================================================
// Replacement Operations
//=============================================================================

/// @brief Replace all matches with replacement string.
/// @details Replacement bytes are literal; capture-expansion syntax is not interpreted.
/// @param obj Required CompiledPattern object.
/// @param text Input text; NULL is treated as empty.
/// @param replacement Literal replacement; NULL is treated as empty.
/// @return Newly allocated string with all nonoverlapping matches replaced.
rt_string rt_compiled_pattern_replace(void *obj, rt_string text, rt_string replacement);

/// @brief Replace first match only.
/// @param obj Required CompiledPattern object.
/// @param text Input text; NULL is treated as empty.
/// @param replacement Literal replacement; NULL is treated as empty.
/// @return Newly allocated string with the first match replaced, or a fresh copy
///         of the input when no match exists.
rt_string rt_compiled_pattern_replace_first(void *obj, rt_string text, rt_string replacement);

/// @brief Expand capture references for the exact match beginning at a byte offset.
/// @details Supports `$$`, `$&`, `$0`, `$1`, and `${1}` syntax. A missing exact
///          match returns Err rather than searching later text.
/// @param obj Required CompiledPattern object.
/// @param text Full source text; NULL is treated as empty.
/// @param start Expected zero-based full-match start.
/// @param replacement Replacement template; NULL is treated as empty.
/// @return Caller-owned Result containing the expanded String or an error String.
void *rt_compiled_pattern_expand_replacement_at(void *obj,
                                                rt_string text,
                                                int64_t start,
                                                rt_string replacement);

//=============================================================================
// Split Operation
//=============================================================================

/// @brief Split text by pattern matches.
/// @param obj Required CompiledPattern object.
/// @param text Text to split; NULL is treated as empty.
/// @return Caller-owned element-owning Seq of substrings between matches.
void *rt_compiled_pattern_split(void *obj, rt_string text);

/// @brief Split text by pattern matches with limit.
/// @details Zero-width matches are handled without dropping input bytes.
/// @param obj Required CompiledPattern object.
/// @param text Text to split; NULL is treated as empty.
/// @param limit Maximum number of result pieces when positive; nonpositive is unlimited.
/// @return Caller-owned element-owning Seq of substring strings.
void *rt_compiled_pattern_split_n(void *obj, rt_string text, int64_t limit);

#ifdef __cplusplus
}
#endif
