//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_diff.h
// Purpose: Line-based text difference computation using a bounded
//          dynamic-programming longest-common-subsequence table.
//
// Key invariants:
//   - Produces a minimal insertion/deletion script in O(m*n) time and space,
//     subject to an implementation-defined LCS-table cell limit.
//   - Diff entries prefix line content with ' ' (equal), '+' (added), or
//     '-' (removed).
//   - Input texts are split on newlines; empty strings produce empty diffs.
//   - The result Seq contains entries in document order.
//   - Patch validates context and removal entries against the complete
//     original text before accepting a reconstruction.
//
// Ownership/Lifetime:
//   - Returned Seq and string objects are newly allocated; caller must release.
//   - Input strings are borrowed for the duration of the call.
//
// Links: src/runtime/text/rt_diff.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_diff.h
 * @brief Declares line-based diff, unified rendering, statistics, and patching.
 * @details Operations compare borrowed text using a bounded minimal
 *          insertion/deletion script, return caller-owned managed records or
 *          diagnostics, and reconstruct modified text only after validating
 *          every unchanged and removed record against the original input.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Compute a minimal line-by-line insertion/deletion script.
/// @details Each returned string is prefixed with `" "` for an unchanged
///          line, `"+"` for an added line, or `"-"` for a removed line.
///          Entries cover both complete inputs in document order. Inputs are
///          split at newline bytes, with a trailing newline represented by a
///          final empty line. Null inputs are treated as empty text.
/// @param a Borrowed original text, or null for empty text.
/// @param b Borrowed modified text, or null for empty text.
/// @return Newly allocated, element-owning Seq of newly allocated diff-line
///         strings; the caller owns the returned reference.
void *rt_diff_lines(rt_string a, rt_string b);

/// @brief Render a compact unified-diff style diagnostic.
/// @details The result begins with fixed `--- a` and `+++ b` headers and
///          includes changes plus nearby unchanged records. It intentionally
///          omits unified-diff hunk headers and is therefore not a conventional
///          patch file.
/// @param a Borrowed original text, or null for empty text.
/// @param b Borrowed modified text, or null for empty text.
/// @param context Number of surrounding records to include; a negative value
///                selects the default of three.
/// @return Newly allocated diagnostic string owned by the caller.
rt_string rt_diff_unified(rt_string a, rt_string b, int64_t context);

/// @brief Count addition and removal records in the line diff.
/// @details Additions and removals are counted independently, so replacing one
///          line normally contributes two changes.
/// @param a Borrowed original text, or null for empty text.
/// @param b Borrowed modified text, or null for empty text.
/// @return Number of added plus removed records.
int64_t rt_diff_count_changes(rt_string a, rt_string b);

/// @brief Apply a sequence of diff lines to the original to reconstruct the modified text.
/// @details Context and removal records must match and completely consume the
///          original text. Additions are inserted, removals are omitted, and
///          context is retained. A mismatch raises a runtime trap. Passing a
///          null sequence returns a newly allocated empty string.
/// @param original Borrowed original text, or null for empty text.
/// @param diff Borrowed Seq of prefixed line strings, normally produced by
///             `rt_diff_lines`.
/// @return Newly allocated reconstructed modified text owned by the caller.
rt_string rt_diff_patch(rt_string original, void *diff);

#ifdef __cplusplus
}
#endif
