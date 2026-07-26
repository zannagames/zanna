//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_textwrap.h
// Purpose: Declares byte-width wrapping, indentation, truncation, alignment,
//          and line-measurement utilities for Zanna.Text.TextWrapper.
//
// Key invariants:
//   - Word wrapping prefers spaces/tabs and hard-wraps words longer than width.
//   - rt_textwrap_fill returns a wrapped string with newlines inserted at word boundaries.
//   - rt_textwrap_indent prepends a prefix string to each line.
//   - Width is measured in bytes, but hard wrapping and truncation avoid
//     splitting a UTF-8 continuation sequence.
//
// Ownership/Lifetime:
//   - Returned strings are newly allocated; caller must release.
//   - Input strings are borrowed for the duration of wrapping.
//
// Links: src/runtime/text/rt_textwrap.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_textwrap.h
 * @brief Declares wrapping, indentation, truncation, alignment, and measurement.
 * @details The byte-width API borrows runtime text, preserves explicit line
 *          breaks, avoids splitting UTF-8 continuation bytes during hard
 *          boundaries, and returns caller-owned Strings or line sequences for
 *          layout and inspection operations.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Basic Text Wrapping
//=========================================================================

/// @brief Wrap text to specified width.
/// @details Existing LF boundaries are preserved. Nonpositive width disables
///          wrapping; overlong words are hard-wrapped at UTF-8 boundaries.
/// @param text Borrowed text; null becomes empty.
/// @param width Target line width in bytes.
/// @return Caller-owned wrapped text with line feeds inserted.
rt_string rt_textwrap_wrap(rt_string text, int64_t width);

/// @brief Wrap text and return as sequence of lines.
/// @param text Borrowed text.
/// @param width Target line width in bytes.
/// @return Caller-owned Seq that owns its line strings.
void *rt_textwrap_wrap_lines(rt_string text, int64_t width);

/// @brief Fill text by joining wrapped lines with single newline.
/// @details This runtime aliases Fill directly to `rt_textwrap_wrap`.
/// @param text Borrowed text.
/// @param width Target line width in bytes.
/// @return Caller-owned wrapped text.
rt_string rt_textwrap_fill(rt_string text, int64_t width);

//=========================================================================
// Indentation
//=========================================================================

/// @brief Indent text with prefix.
/// @details Prefixes every physical line, including empty lines and the empty
///          line after a trailing LF.
/// @param text Borrowed text.
/// @param prefix Borrowed line prefix; null is empty.
/// @return Caller-owned indented text.
rt_string rt_textwrap_indent(rt_string text, rt_string prefix);

/// @brief Remove common leading whitespace from all lines.
/// @details Removes the longest identical space/tab byte prefix shared by
///          every nonempty line.
/// @param text Borrowed text.
/// @return Caller-owned dedented text.
rt_string rt_textwrap_dedent(rt_string text);

/// @brief Indent text except first line.
/// @param text Borrowed text.
/// @param prefix Borrowed prefix for every physical line after the first.
/// @return Caller-owned hanging-indent text.
rt_string rt_textwrap_hang(rt_string text, rt_string prefix);

//=========================================================================
// Truncation
//=========================================================================

/// @brief Truncate text to max length with ellipsis.
/// @param text Borrowed text.
/// @param width Maximum output bytes.
/// @return Caller-owned text truncated with `"..."` when needed.
rt_string rt_textwrap_truncate(rt_string text, int64_t width);

/// @brief Truncate text with custom suffix.
/// @param text Borrowed text.
/// @param width Maximum output bytes.
/// @param suffix Borrowed suffix appended when shortening; null is empty.
/// @return Caller-owned truncated text.
rt_string rt_textwrap_truncate_with(rt_string text, int64_t width, rt_string suffix);

/// @brief Shorten text in middle with ellipsis.
/// @details Widths below five use head truncation; wider output preserves both ends.
/// @param text Borrowed text.
/// @param width Maximum output bytes.
/// @return Caller-owned text with the middle replaced by `"..."`.
rt_string rt_textwrap_shorten(rt_string text, int64_t width);

//=========================================================================
// Alignment
//=========================================================================

/// @brief Left-align text in specified width.
/// @param text Borrowed text; null is treated as empty.
/// @param width Target byte width.
/// @return Caller-owned text padded with trailing spaces when needed.
rt_string rt_textwrap_left(rt_string text, int64_t width);

/// @brief Right-align text in specified width.
/// @param text Borrowed text; null is treated as empty.
/// @param width Target byte width.
/// @return Caller-owned text padded with leading spaces when needed.
rt_string rt_textwrap_right(rt_string text, int64_t width);

/// @brief Center text in specified width.
/// @details An odd extra padding byte is placed on the right.
/// @param text Borrowed text; null is treated as empty.
/// @param width Target byte width.
/// @return Caller-owned space-padded text.
rt_string rt_textwrap_center(rt_string text, int64_t width);

//=========================================================================
// Utility
//=========================================================================

/// @brief Count lines in text.
/// @details Empty text counts as one; a trailing LF does not add an empty line.
/// @param text Borrowed text.
/// @return Physical line count.
int64_t rt_textwrap_line_count(rt_string text);

/// @brief Get the maximum line length.
/// @param text Borrowed text.
/// @return Longest line length in bytes.
int64_t rt_textwrap_max_line_len(rt_string text);

#ifdef __cplusplus
}
#endif
