//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_diff.c
// Purpose: Implements line-level text diff for the Zanna.Text.Diff class.
//          Computes a dynamic-programming LCS edit script between two
//          multiline strings, producing added/removed/unchanged line records.
//
// Key invariants:
//   - Input strings are split on '\n' into line arrays before diffing.
//   - The diff produces a minimal edit script (fewest insertions + deletions);
//     the LCS table is bounded by RT_DIFF_MAX_LCS_CELLS and traps beyond it.
//   - Each Lines record is prefixed ' ' (unchanged), '+' (added), or
//     '-' (removed); Lines always contains every line of both inputs
//     (context selection happens only in Unified).
//   - Patch validates the diff against its original argument and traps on
//     mismatch (VDOC-061).
//   - Empty input produces an empty diff, not a null result.
//   - All returned strings are fresh allocations; the diff holds no live refs.
//
// Ownership/Lifetime:
//   - Returned Seq of diff records is a fresh allocation owned by the caller.
//   - Input strings are borrowed for the duration of the call; not retained.
//
// Links: src/runtime/text/rt_diff.h (public API),
//        src/runtime/rt_seq.h (Seq container used for diff output),
//        src/runtime/rt_string.h (string split for line arrays)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_diff.c
 * @brief Implements bounded line-oriented text difference and reconstruction.
 * @details Inputs are split into length-aware lines and compared with a capped
 *          dynamic-programming longest-common-subsequence table. The module
 *          emits minimal edit records, compact unified diagnostics, change
 *          statistics, and context-validating patch reconstruction.
 */

#include "rt_diff.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "rt_trap.h"

/// Maximum number of cells permitted in the full LCS matrix.
///
/// The limit bounds both the quadratic work performed by the line diff and
/// the aggregate allocation made for its row arrays.
#define RT_DIFF_MAX_LCS_CELLS (16u * 1024u * 1024u)

// ---------------------------------------------------------------------------
// Line splitting helper
// ---------------------------------------------------------------------------

/// Owned, length-aware representation of newline-delimited input text.
///
/// Each entry is separately allocated and NUL-terminated for convenience,
/// while the parallel length array preserves embedded NUL bytes.
typedef struct {
    char **lines; ///< Individually allocated line byte strings.
    size_t *lens; ///< Byte length of each entry in @c lines.
    int count;    ///< Number of populated entries in both arrays.
} line_array;

static void free_lines(line_array *la);

/// @brief Validate that the LCS table for two line arrays is practical to allocate.
/// @details The current diff implementation stores a full `(m + 1) x (n + 1)`
///          dynamic-programming table. That gives stable minimal edit scripts
///          but is quadratic in line count, so untrusted large inputs need an
///          explicit budget before allocation. This helper performs all size
///          calculations in subtract form and traps before the table can wrap
///          or exceed the configured cell budget.
/// @param m Number of lines in the left input.
/// @param n Number of lines in the right input.
/// @return `true` when the table size is within the runtime budget.
static bool diff_lcs_table_size_ok(int m, int n) {
    if (m < 0 || n < 0)
        return false;
    size_t rows = (size_t)m + 1u;
    size_t cols = (size_t)n + 1u;
    if (rows == 0 || cols == 0 || rows > SIZE_MAX / cols) {
        rt_trap("Diff.Lines: input too large");
        return false;
    }
    if (rows * cols > RT_DIFF_MAX_LCS_CELLS) {
        rt_trap("Diff.Lines: too many line comparisons");
        return false;
    }
    return true;
}

/// @brief Split an explicit byte range into a heap-allocated array of line copies.
/// @details Counts `\n` characters first to size the array, then walks
///          again to copy each line (without the terminator) into its
///          own malloc'd null-terminated string. The final segment after
///          the last `\n` is always emitted, even when empty — that
///          way `"a\nb\n"` produces `["a", "b", ""]` and `"a\nb"`
///          produces `["a", "b"]`, mirroring how unified-diff tools
///          treat trailing newlines. A null pointer or zero length produces
///          an empty array. Allocation and line-count failures trap and
///          return an empty, safely releasable aggregate.
/// @param text Borrowed start of the byte range; may contain embedded NULs.
/// @param text_len Number of readable bytes at @p text.
/// @return An owning line array that must be released with `free_lines`.
static line_array split_lines(const char *text, size_t text_len) {
    line_array la = {NULL, NULL, 0};
    if (!text || text_len == 0)
        return la;

    // Count lines
    int count = 1;
    for (size_t i = 0; i < text_len; i++)
        if (text[i] == '\n') {
            if (count == INT_MAX) {
                rt_trap("rt_diff: too many lines");
                return la;
            }
            count++;
        }

    la.lines = (char **)malloc((size_t)count * sizeof(char *));
    if (!la.lines) {
        rt_trap("rt_diff: memory allocation failed");
        return la;
    }
    la.lens = (size_t *)malloc((size_t)count * sizeof(size_t));
    if (!la.lens) {
        free(la.lines);
        la.lines = NULL;
        rt_trap("rt_diff: memory allocation failed");
        return la;
    }
    la.count = 0;

    size_t start = 0;
    for (size_t i = 0; i <= text_len; i++) {
        if (i == text_len || text[i] == '\n') {
            size_t len = i - start;
            la.lines[la.count] = (char *)malloc(len + 1);
            if (!la.lines[la.count]) {
                free_lines(&la);
                rt_trap("rt_diff: memory allocation failed");
                return la;
            }
            memcpy(la.lines[la.count], text + start, len);
            la.lines[la.count][len] = '\0';
            la.lens[la.count] = len;
            la.count++;
            if (i == text_len)
                break;
            start = i + 1;
        }
    }
    return la;
}

/// @brief Free every line copy and the line-pointer array, leaving an empty `line_array`.
/// @details Setting both `lines` and `count` to zero/NULL after free
///          makes the helper idempotent — a second call is a no-op
///          rather than a double-free.
/// @param la Non-null line array whose owned storage is to be released.
static void free_lines(line_array *la) {
    for (int i = 0; i < la->count; i++)
        free(la->lines[i]);
    free(la->lines);
    free(la->lens);
    la->lines = NULL;
    la->lens = NULL;
    la->count = 0;
}

/// @brief Compare one line from each array for exact byte equality.
/// @details The stored lengths are checked before `memcmp`, so embedded NUL
///          bytes participate in the comparison.
/// @param a Left line array.
/// @param i Valid line index in @p a.
/// @param b Right line array.
/// @param j Valid line index in @p b.
/// @return Non-zero when the selected lines have identical lengths and bytes.
static int lines_equal(const line_array *a, int i, const line_array *b, int j) {
    return a->lens[i] == b->lens[j] && memcmp(a->lines[i], b->lines[j], a->lens[i]) == 0;
}

/// @brief Release a locally owned runtime object reference.
/// @details Frees the object only when dropping this reference reduces its
///          runtime reference count to zero. A null pointer is ignored.
/// @param obj Runtime object reference owned by the current function.
static void release_local_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Convert a string-builder failure into the runtime diff trap.
/// @details Successful operations leave the builder untouched. On failure,
///          the builder is released before trapping. Callers rely on the
///          runtime trap being terminating and must not reuse the builder
///          after an error.
/// @param sb Builder associated with @p status.
/// @param status Status returned by the most recent builder operation.
static void diff_check_sb(rt_string_builder *sb, rt_sb_status_t status) {
    if (status == RT_SB_OK)
        return;
    rt_sb_free(sb);
    rt_trap("rt_diff: string builder allocation failed");
}

/// @brief Allocate a runtime string from a byte range or raise a diff trap.
/// @param bytes Borrowed byte range to copy; follows `rt_string_from_bytes`
///              rules for an empty range.
/// @param len Number of bytes to copy.
/// @return Newly allocated runtime string, or null only if the trap hook returns.
static rt_string diff_string_from_bytes_or_trap(const char *bytes, size_t len) {
    rt_string result = rt_string_from_bytes(bytes, len);
    if (!result)
        rt_trap("rt_diff: string allocation failed");
    return result;
}

// ---------------------------------------------------------------------------
// Simple LCS-based diff (O(nm) space - sufficient for typical text)
// ---------------------------------------------------------------------------

/// @brief Fill the longest-common-subsequence DP table for two line arrays.
/// @details Standard `O(m*n)` LCS recurrence run *backward* from
///          `(m, n)` to `(0, 0)` so `table[i][j]` holds the LCS length
///          of the suffixes `a[i..m]` and `b[j..n]`. The traceback in
///          `rt_diff_lines` then walks forward from `(0, 0)` greedily,
///          choosing the direction that preserves the LCS. The resulting
///          insertion/deletion script is minimal, although this is a full
///          dynamic-programming LCS implementation rather than Myers'
///          algorithm. The caller has already bounded the `O(m*n)` storage
///          and work through `diff_lcs_table_size_ok`.
/// @param a Left line array.
/// @param b Right line array.
/// @param table Preallocated `(a->count + 1)` by `(b->count + 1)` matrix.
static void compute_lcs_table(const line_array *a, const line_array *b, int **table) {
    int m = a->count;
    int n = b->count;

    for (int i = m; i >= 0; i--) {
        for (int j = n; j >= 0; j--) {
            if (i == m || j == n)
                table[i][j] = 0;
            else if (lines_equal(a, i, b, j))
                table[i][j] = table[i + 1][j + 1] + 1;
            else
                table[i][j] = table[i + 1][j] > table[i][j + 1] ? table[i + 1][j] : table[i][j + 1];
        }
    }
}

// ---------------------------------------------------------------------------
// rt_diff_lines
// ---------------------------------------------------------------------------

/// @brief Compute a minimal line-oriented insertion/deletion script.
/// @details Splits both inputs at newline bytes, computes a bounded full LCS
///          matrix, and traces it forward into strings prefixed with `' '`
///          for an unchanged line, `'+'` for an addition, or `'-'` for a
///          removal. Every input line appears in document order. Equal-length
///          traceback alternatives favor an addition before a removal. Null
///          inputs are treated as empty text. Oversized inputs trap; an LCS
///          allocation failure returns the already-created empty sequence.
/// @param a Borrowed original text, or null for empty text.
/// @param b Borrowed modified text, or null for empty text.
/// @return Newly allocated, element-owning runtime Seq of newly allocated
///         prefixed strings; the caller owns the returned reference.
void *rt_diff_lines(rt_string a, rt_string b) {
    void *result = rt_seq_new();
    rt_seq_set_owns_elements(result, 1);

    const char *astr = a ? rt_string_cstr(a) : "";
    const char *bstr = b ? rt_string_cstr(b) : "";
    size_t astr_len = a ? (size_t)rt_str_len(a) : 0;
    size_t bstr_len = b ? (size_t)rt_str_len(b) : 0;

    line_array la = split_lines(astr, astr_len);
    line_array lb = split_lines(bstr, bstr_len);

    int m = la.count;
    int n = lb.count;
    if (!diff_lcs_table_size_ok(m, n)) {
        free_lines(&la);
        free_lines(&lb);
        return result;
    }

    // Build LCS table
    int **table = (int **)malloc((size_t)(m + 1) * sizeof(int *));
    if (!table) {
        free_lines(&la);
        free_lines(&lb);
        return result;
    }
    for (int i = 0; i <= m; i++) {
        table[i] = (int *)calloc((size_t)(n + 1), sizeof(int));
        if (!table[i]) {
            for (int k = 0; k < i; k++)
                free(table[k]);
            free(table);
            free_lines(&la);
            free_lines(&lb);
            return result;
        }
    }

    compute_lcs_table(&la, &lb, table);

    // Trace back to produce diff
    int i = 0, j = 0;
    while (i < m || j < n) {
        rt_string_builder sb;
        rt_sb_init(&sb);

        if (i < m && j < n && lines_equal(&la, i, &lb, j)) {
            diff_check_sb(&sb, rt_sb_append_bytes(&sb, " ", 1));
            diff_check_sb(&sb, rt_sb_append_bytes(&sb, la.lines[i], la.lens[i]));
            i++;
            j++;
        } else if (j < n && (i >= m || table[i][j + 1] >= table[i + 1][j])) {
            diff_check_sb(&sb, rt_sb_append_bytes(&sb, "+", 1));
            diff_check_sb(&sb, rt_sb_append_bytes(&sb, lb.lines[j], lb.lens[j]));
            j++;
        } else {
            diff_check_sb(&sb, rt_sb_append_bytes(&sb, "-", 1));
            diff_check_sb(&sb, rt_sb_append_bytes(&sb, la.lines[i], la.lens[i]));
            i++;
        }

        rt_string line = diff_string_from_bytes_or_trap(sb.data, sb.len);
        rt_seq_push(result, line);
        rt_string_unref(line);
        rt_sb_free(&sb);
    }

    // Cleanup
    for (int k = 0; k <= m; k++)
        free(table[k]);
    free(table);
    free_lines(&la);
    free_lines(&lb);

    return result;
}

// ---------------------------------------------------------------------------
// rt_diff_unified
// ---------------------------------------------------------------------------

/// @brief Return whether a rendered diff line represents an addition or deletion.
///
/// Diff records are strings prefixed with one of `' '`, `'+'`, or `'-'`.
/// Unified context filtering only treats additions and deletions as anchors;
/// unchanged lines are included when they are close enough to one of those
/// anchors.
///
/// @param line Rendered diff line from `rt_diff_lines`.
/// @return Non-zero for added/removed lines.
static int diff_line_is_change(rt_string line) {
    const char *cstr = rt_string_cstr(line);
    return cstr && (cstr[0] == '+' || cstr[0] == '-');
}

/// @brief Test whether a diff line falls within a context window of a change.
///
/// @param diff    Sequence returned by `rt_diff_lines`.
/// @param len     Number of records in @p diff.
/// @param index   Candidate line index.
/// @param context Number of surrounding records to include around changes.
/// @return Non-zero when the candidate should be rendered.
static int diff_line_within_context(void *diff, int64_t len, int64_t index, int64_t context) {
    int64_t start = index > context ? index - context : 0;
    int64_t end = len - index > context ? index + context : len - 1;
    for (int64_t i = start; i <= end; i++) {
        if (diff_line_is_change((rt_string)rt_seq_get(diff, i)))
            return 1;
    }
    return 0;
}

/// @brief Render an `a → b` diff as a plain unified-diff style string.
/// @details Emits the canonical `--- a` / `+++ b` header, then dumps
///          changed lines plus up to @p context unchanged records around
///          those changes. Lines are already prefixed with one of ` `,
///          `+`, or `-`. This remains a compact diagnostic format rather
///          than a patch-applicable unified diff with hunk headers. A
///          negative context selects the default of three records; zero
///          suppresses unchanged context.
/// @param a Borrowed original text, or null for empty text.
/// @param b Borrowed modified text, or null for empty text.
/// @param context Number of surrounding diff records, or a negative value
///                to use the default of three.
/// @return Newly allocated diagnostic diff string owned by the caller.
rt_string rt_diff_unified(rt_string a, rt_string b, int64_t context) {
    if (context < 0)
        context = 3;

    void *diff = rt_diff_lines(a, b);
    int64_t len = rt_seq_len(diff);

    rt_string_builder sb;
    rt_sb_init(&sb);

    // Simple unified format: output selected lines with proper prefixes.
    diff_check_sb(&sb, rt_sb_append_cstr(&sb, "--- a\n"));
    diff_check_sb(&sb, rt_sb_append_cstr(&sb, "+++ b\n"));

    for (int64_t i = 0; i < len; i++) {
        rt_string line = (rt_string)rt_seq_get(diff, i);
        if (!diff_line_is_change(line) && !diff_line_within_context(diff, len, i, context))
            continue;
        const char *cstr = rt_string_cstr(line);
        if (cstr) {
            diff_check_sb(&sb, rt_sb_append_bytes(&sb, cstr, (size_t)rt_str_len(line)));
            diff_check_sb(&sb, rt_sb_append_cstr(&sb, "\n"));
        }
    }

    rt_string result = diff_string_from_bytes_or_trap(sb.data, sb.len);
    rt_sb_free(&sb);
    release_local_obj(diff);
    return result;
}

// ---------------------------------------------------------------------------
// rt_diff_count_changes
// ---------------------------------------------------------------------------

/// @brief Count the number of added and removed records between two strings.
/// @details Computes the same minimal script as `rt_diff_lines` and counts
///          each `'+'` and `'-'` record independently; a replacement commonly
///          contributes two changes.
/// @param a Borrowed original text, or null for empty text.
/// @param b Borrowed modified text, or null for empty text.
/// @return Total number of addition and removal records.
int64_t rt_diff_count_changes(rt_string a, rt_string b) {
    void *diff = rt_diff_lines(a, b);
    int64_t len = rt_seq_len(diff);
    int64_t changes = 0;

    for (int64_t i = 0; i < len; i++) {
        rt_string line = (rt_string)rt_seq_get(diff, i);
        const char *cstr = rt_string_cstr(line);
        if (cstr && (cstr[0] == '+' || cstr[0] == '-'))
            changes++;
    }

    release_local_obj(diff);
    return changes;
}

// ---------------------------------------------------------------------------
// rt_diff_patch
// ---------------------------------------------------------------------------

/// @brief Reconstruct the "after" text by applying a diff sequence.
/// @details Walks the diff's line entries (each prefixed with ` `, `+`,
///          or `-`) and emits the appropriate content into a builder:
///          - ` ` (context) → output the line as-is.
///          - `+` (added)   → output the line.
///          - `-` (removed) → skip.
///          Context and removed entries must exactly consume the corresponding
///          lines of @p original, including explicit byte lengths, and the
///          complete original must be consumed. A mismatch traps and returns
///          an empty fallback string if the trap hook returns. Output records
///          are joined with `\n`; the trailing empty record produced by
///          `rt_diff_lines` preserves a final newline. Null @p diff produces
///          a newly allocated empty string.
/// @param original Borrowed source text against which context and removals
///                 are validated; null denotes empty text.
/// @param diff Borrowed Seq of prefixed line strings, normally returned by
///             `rt_diff_lines`.
/// @return Newly allocated reconstructed modified text owned by the caller.
rt_string rt_diff_patch(rt_string original, void *diff) {
    if (!diff)
        return rt_string_from_bytes("", 0);

    // Validate the diff against `original` (VDOC-061): every context (' ')
    // and removed ('-') line must match the corresponding original line,
    // and the diff must consume the original exactly.
    const char *orig_str = original ? rt_string_cstr(original) : "";
    size_t orig_len = original ? (size_t)rt_str_len(original) : 0;
    line_array orig_lines = split_lines(orig_str ? orig_str : "", orig_len);
    size_t orig_index = 0;

    rt_string_builder sb;
    rt_sb_init(&sb);

    int64_t len = rt_seq_len(diff);
    int first = 1;

    for (int64_t i = 0; i < len; i++) {
        rt_string line = (rt_string)rt_seq_get(diff, i);
        const char *cstr = rt_string_cstr(line);
        if (!cstr)
            continue;
        int64_t line_len = rt_str_len(line);
        if (line_len < 1)
            continue;

        if (cstr[0] == ' ' || cstr[0] == '-') {
            // Context/removed lines consume one original line and must match.
            size_t content_len = (size_t)(line_len - 1);
            if (orig_index >= (size_t)orig_lines.count ||
                orig_lines.lens[orig_index] != content_len ||
                memcmp(orig_lines.lines[orig_index], cstr + 1, content_len) != 0) {
                rt_sb_free(&sb);
                free_lines(&orig_lines);
                rt_trap("Diff.Patch: diff does not apply to the original text");
                return rt_string_from_bytes("", 0);
            }
            orig_index++;
        }

        // Include lines that are same (' ') or added ('+')
        if (cstr[0] == ' ' || cstr[0] == '+') {
            if (!first)
                diff_check_sb(&sb, rt_sb_append_cstr(&sb, "\n"));
            if (line_len > 1)
                diff_check_sb(
                    &sb, rt_sb_append_bytes(&sb, cstr + 1, (size_t)(line_len - 1))); // Skip prefix
            first = 0;
        }
    }

    if (orig_index != (size_t)orig_lines.count) {
        rt_sb_free(&sb);
        free_lines(&orig_lines);
        rt_trap("Diff.Patch: diff does not apply to the original text");
        return rt_string_from_bytes("", 0);
    }
    free_lines(&orig_lines);

    rt_string result = diff_string_from_bytes_or_trap(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;
}
