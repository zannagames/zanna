//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_textwrap.c
// Purpose: Implements text word-wrapping and layout utilities for the
//          Zanna.Text.TextWrapper class: Wrap/WrapLines/Fill, Indent/Dedent/
//          Hang, Truncate/TruncateWith/Shorten, AlignLeft/AlignRight/Center,
//          and the LineCount/MaxLineLength measurements.
//
// Key invariants:
//   - Wrapping occurs at whitespace boundaries; a word longer than the width
//     is split at the width (there is no configurable hard-wrap toggle, and
//     no indentation or tab-expansion options are exposed).
//   - Empty input returns an empty string; a non-positive width returns the
//     input unchanged.
//
// Ownership/Lifetime:
//   - The returned wrapped string is a fresh rt_string allocation owned by caller.
//   - Input strings are borrowed for the duration of the call.
//
// Links: src/runtime/text/rt_textwrap.h (public API),
//        src/runtime/core/rt_string.h (runtime string operations),
//        src/runtime/collections/rt_seq.h (WrapLines result)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_textwrap.c
 * @brief Implements byte-width text wrapping and layout utilities.
 * @details Operations wrap at whitespace or safe UTF-8 boundaries, split
 *          wrapped lines, indent, dedent, hang, truncate or shorten with
 *          suffixes, align text, and measure physical lines. Checked size
 *          arithmetic prevents allocation overflow for large inputs.
 */

#include "rt_textwrap.h"
#include "rt_internal.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Convert a nonnegative signed length to `size_t`.
/// @param value Length to convert.
/// @param op Trap message for negative or unrepresentable input.
/// @param out Destination for the converted length.
/// @return One on success, otherwise zero after a recoverable trap.
static int checked_i64_to_size(int64_t value, const char *op, size_t *out) {
    if (!out) {
        rt_trap(op);
        return 0;
    }
    *out = 0;
    if (value < 0) {
        rt_trap(op);
        return 0;
    }
    if ((uint64_t)value > (uint64_t)SIZE_MAX) {
        rt_trap(op);
        return 0;
    }
    *out = (size_t)value;
    return 1;
}

/// @brief Compute `base + count * each` with overflow checking.
/// @param base Fixed byte count.
/// @param count Number of repeated units.
/// @param each Bytes per repeated unit.
/// @param op Trap message used on overflow.
/// @param out Destination for the checked sum.
/// @return One on success, otherwise zero after a recoverable trap.
static int checked_mul_add(size_t base, size_t count, size_t each, const char *op, size_t *out) {
    if (!out) {
        rt_trap(op);
        return 0;
    }
    *out = 0;
    if (each != 0 && count > (SIZE_MAX - base) / each) {
        rt_trap(op);
        return 0;
    }
    *out = base + count * each;
    return 1;
}

/// @brief Allocate a byte buffer and trap on failure.
/// @param size Allocation size in bytes.
/// @param op Trap message used on failure.
/// @return Newly allocated buffer, or null after a recoverable trap.
static char *checked_malloc(size_t size, const char *op) {
    if (size == 0) {
        rt_trap(op);
        return NULL;
    }
    char *ptr = (char *)malloc(size);
    if (!ptr) {
        rt_trap(op);
        return NULL;
    }
    return ptr;
}

/// @brief Add one to a size value with overflow checking.
/// @param value Input size.
/// @param op Trap message used on overflow.
/// @param out Destination for the incremented value.
/// @return `1` on success, otherwise `0` after trapping.
static int checked_add_one(size_t value, const char *op, size_t *out) {
    if (value == SIZE_MAX) {
        rt_trap(op);
        return 0;
    }
    *out = value + 1;
    return 1;
}

/// @brief Validate an optional runtime string handle.
/// @param text Candidate string; null is accepted.
/// @param op Trap message used for an invalid non-null handle.
/// @return `1` for null or a valid string handle, otherwise `0`.
static int textwrap_valid_string(rt_string text, const char *op) {
    if (!text || rt_string_is_handle(text))
        return 1;
    rt_trap(op);
    return 0;
}

/// @brief Allocate an empty runtime string.
/// @return Newly allocated zero-length runtime string.
static rt_string textwrap_empty_string(void) {
    return rt_string_from_bytes("", 0);
}

/// @brief Allocate a null-terminated buffer containing spaces.
/// @param count Nonnegative number of spaces.
/// @param op Trap message used for invalid size or allocation failure.
/// @return Newly allocated C buffer, or null after a recoverable trap.
static char *alloc_spaces(int64_t count, const char *op) {
    size_t n = 0;
    if (!checked_i64_to_size(count, op, &n))
        return NULL;
    size_t alloc_size = 0;
    if (!checked_add_one(n, op, &alloc_size))
        return NULL;
    char *spaces = checked_malloc(alloc_size, op);
    if (!spaces)
        return NULL;
    memset(spaces, ' ', n);
    spaces[n] = '\0';
    return spaces;
}

//=============================================================================
// Basic Text Wrapping
//=============================================================================

/// @brief Return 1 when src[idx] is a UTF-8 continuation byte (0b10xxxxxx).
/// @param src Input byte buffer.
/// @param idx Byte index to inspect.
/// @return Nonzero when the byte has the UTF-8 continuation prefix.
static int textwrap_is_continuation(const char *src, int64_t idx) {
    return ((unsigned char)src[idx] & 0xC0) == 0x80;
}

/// @brief Back a byte index up to the nearest UTF-8 codepoint boundary at or
///        before it, never moving below `floor_idx`. Splitting a multi-byte
///        sequence would emit malformed UTF-8 on both sides (VDOC-046).
/// @param src UTF-8 byte buffer.
/// @param idx Candidate split byte index.
/// @param floor_idx Lowest permitted index.
/// @return Nearest codepoint-boundary byte index at or before @p idx.
static int64_t textwrap_boundary_at_or_before(const char *src, int64_t idx, int64_t floor_idx) {
    while (idx > floor_idx && textwrap_is_continuation(src, idx))
        idx--;
    return idx;
}

/// @brief Greedily word-wrap text to a byte-width target.
/// @details Breaks at the latest space or tab when possible and hard-wraps
///          overlong words otherwise. Hard wraps retreat to UTF-8 codepoint
///          boundaries, so a multibyte codepoint may make a line exceed the
///          byte target. Existing line feeds reset wrapping. Nonpositive width
///          returns a retained reference to valid input.
/// @param text Borrowed runtime text; null becomes empty.
/// @param width Target line width measured in bytes.
/// @return Caller-owned wrapped runtime string.
rt_string rt_textwrap_wrap(rt_string text, int64_t width) {
    if (!text)
        return textwrap_empty_string();
    if (!textwrap_valid_string(text, "TextWrapper.Wrap: invalid string"))
        return textwrap_empty_string();
    if (width <= 0)
        return rt_string_ref(text);

    const char *src = rt_string_cstr(text);
    int64_t src_len = rt_str_len(text);

    size_t src_size = 0;
    size_t alloc_size = 0;
    if (!checked_i64_to_size(src_len, "TextWrapper.Wrap: invalid length", &src_size) ||
        !checked_mul_add(1, src_size, 2, "TextWrapper.Wrap: output length overflow", &alloc_size)) {
        return textwrap_empty_string();
    }
    char *result = checked_malloc(alloc_size, "TextWrapper.Wrap: memory allocation failed");
    if (!result)
        return textwrap_empty_string();

    int64_t result_pos = 0;
    int64_t line_start = 0;
    int64_t last_space = -1;
    int64_t col = 0;

    for (int64_t i = 0; i < src_len; i++) {
        char c = src[i];

        if (c == '\n') {
            // Copy line up to newline
            memcpy(result + result_pos, src + line_start, (size_t)(i - line_start + 1));
            result_pos += i - line_start + 1;
            line_start = i + 1;
            last_space = -1;
            col = 0;
            continue;
        }

        if (c == ' ' || c == '\t') {
            last_space = i;
        }

        col++;

        if (col > width) {
            if (last_space > line_start) {
                // Wrap at last space
                memcpy(result + result_pos, src + line_start, (size_t)(last_space - line_start));
                result_pos += last_space - line_start;
                result[result_pos++] = '\n';
                line_start = last_space + 1;
                col = i - last_space;
                last_space = -1;
            } else {
                // No space found: force a break, but never inside a multi-byte
                // UTF-8 sequence — back up to the codepoint boundary, or emit
                // the whole codepoint when it alone exceeds the width.
                int64_t brk = textwrap_boundary_at_or_before(src, i, line_start);
                if (brk == line_start) {
                    brk = i + 1;
                    while (brk < src_len && textwrap_is_continuation(src, brk))
                        brk++;
                }
                memcpy(result + result_pos, src + line_start, (size_t)(brk - line_start));
                result_pos += brk - line_start;
                if (brk < src_len)
                    result[result_pos++] = '\n';
                line_start = brk;
                if (brk > i) {
                    i = brk - 1; // loop increment lands on the next codepoint
                    col = 0;
                } else {
                    col = i - brk + 1;
                }
            }
        }
    }

    // Copy remaining text
    if (line_start < src_len) {
        memcpy(result + result_pos, src + line_start, (size_t)(src_len - line_start));
        result_pos += src_len - line_start;
    }

    result[result_pos] = '\0';
    rt_string ret = rt_string_from_bytes(result, result_pos);
    free(result);
    return ret;
}

/// @brief Wrap text and return the result split into a Seq of rt_string lines (no trailing LF).
/// @details The returned Seq owns each line. Empty wrapped text produces one
///          empty line element.
/// @param text Borrowed runtime text.
/// @param width Target line width in bytes.
/// @return Caller-owned Seq of caller-independent line strings, or null if Seq
///         allocation fails.
void *rt_textwrap_wrap_lines(rt_string text, int64_t width) {
    rt_string wrapped = rt_textwrap_wrap(text, width);
    void *lines = rt_seq_new();
    if (!lines) {
        rt_string_unref(wrapped);
        return NULL;
    }
    rt_seq_set_owns_elements(lines, 1);

    const char *src = rt_string_cstr(wrapped);
    int64_t len = rt_str_len(wrapped);
    int64_t start = 0;

    for (int64_t i = 0; i <= len; i++) {
        if (i == len || src[i] == '\n') {
            rt_string line = rt_string_from_bytes(src + start, i - start);
            rt_seq_push(lines, (void *)line);
            rt_string_unref(line);
            start = i + 1;
        }
    }

    rt_string_unref(wrapped);
    return lines;
}

/// @brief Alias for `rt_textwrap_wrap` — kept for API parity with Python's `textwrap`.
/// @details In CPython, `wrap()` returns a list of lines and `fill()`
///          returns a single string. This runtime always returns a
///          string from both, so they collapse to the same call.
/// @param text Borrowed runtime text.
/// @param width Target line width in bytes.
/// @return Caller-owned wrapped string from `rt_textwrap_wrap`.
rt_string rt_textwrap_fill(rt_string text, int64_t width) {
    return rt_textwrap_wrap(text, width);
}

//=============================================================================
// Indentation
//=============================================================================

/// @brief Prepend `prefix` to every line of `text` (including empty lines).
/// @details Walks the input one byte at a time, emitting `prefix`
///          before each line and again after every embedded `\n`.
///          Counts lines up-front to size the output buffer exactly:
///          `src_len + line_count * pre_len`.
/// @param text Borrowed runtime text; null is treated as empty.
/// @param prefix Borrowed prefix; null or invalid input becomes empty after a trap.
/// @return Caller-owned indented string.
rt_string rt_textwrap_indent(rt_string text, rt_string prefix) {
    if (!textwrap_valid_string(text, "TextWrapper.Indent: invalid string"))
        return textwrap_empty_string();
    if (!textwrap_valid_string(prefix, "TextWrapper.Indent: invalid prefix"))
        prefix = NULL;
    const char *src = text ? rt_string_cstr(text) : "";
    int64_t src_len = rt_str_len(text);
    const char *pre = prefix ? rt_string_cstr(prefix) : "";
    int64_t pre_len = rt_str_len(prefix);

    if (src_len == 0)
        return rt_string_from_bytes("", 0);

    // Count lines
    int64_t line_count = 1;
    for (int64_t i = 0; i < src_len; i++) {
        if (src[i] == '\n')
            line_count++;
    }

    size_t src_size = 0;
    size_t line_count_size = 0;
    size_t prefix_size = 0;
    size_t base_size = 0;
    size_t alloc_size = 0;
    if (!checked_i64_to_size(src_len, "TextWrapper.Indent: invalid length", &src_size) ||
        !checked_i64_to_size(
            line_count, "TextWrapper.Indent: invalid line count", &line_count_size) ||
        !checked_i64_to_size(pre_len, "TextWrapper.Indent: invalid prefix length", &prefix_size) ||
        !checked_add_one(src_size, "TextWrapper.Indent: output length overflow", &base_size) ||
        !checked_mul_add(base_size,
                         line_count_size,
                         prefix_size,
                         "TextWrapper.Indent: output length overflow",
                         &alloc_size)) {
        return textwrap_empty_string();
    }
    char *result = checked_malloc(alloc_size, "TextWrapper.Indent: memory allocation failed");
    if (!result)
        return textwrap_empty_string();

    int64_t result_pos = 0;
    int at_line_start = 1;

    for (int64_t i = 0; i <= src_len; i++) {
        if (at_line_start) {
            memcpy(result + result_pos, pre, (size_t)pre_len);
            result_pos += pre_len;
            at_line_start = 0;
        }

        if (i < src_len) {
            result[result_pos++] = src[i];
            if (src[i] == '\n') {
                at_line_start = 1;
            }
        }
    }

    result[result_pos] = '\0';
    rt_string ret = rt_string_from_bytes(result, result_pos);
    free(result);
    return ret;
}

/// @brief Strip the longest leading-whitespace byte prefix common to every non-empty line.
/// @details Two passes:
///          1. Walk the input once to find `min_indent` — the smallest
///             leading-whitespace prefix across all non-empty lines
///             (empty lines are skipped to match Python's
///             `textwrap.dedent` behavior).
///          2. Emit each line with exactly those leading bytes skipped.
///          Tabs and spaces are compared literally so a partial tab is never
///          removed as if it were a run of spaces.
/// @param text Borrowed runtime text; null becomes empty.
/// @return Caller-owned dedented string, possibly a retained input reference
///         when no common indentation exists.
rt_string rt_textwrap_dedent(rt_string text) {
    if (!text)
        return textwrap_empty_string();
    if (!textwrap_valid_string(text, "TextWrapper.Dedent: invalid string"))
        return textwrap_empty_string();

    const char *src = rt_string_cstr(text);
    int64_t src_len = rt_str_len(text);

    int64_t common_start = -1;
    int64_t common_len = -1;
    int64_t line_start = 0;

    while (line_start < src_len) {
        int64_t line_end = line_start;
        while (line_end < src_len && src[line_end] != '\n')
            line_end++;

        int64_t pos = line_start;
        while (pos < line_end && (src[pos] == ' ' || src[pos] == '\t'))
            pos++;

        if (pos < line_end) {
            int64_t indent_len = pos - line_start;
            if (common_len < 0) {
                common_start = line_start;
                common_len = indent_len;
            } else {
                int64_t keep = 0;
                while (keep < common_len && keep < indent_len &&
                       src[common_start + keep] == src[line_start + keep]) {
                    keep++;
                }
                common_len = keep;
            }
        }

        line_start = line_end + 1;
    }

    if (common_len <= 0)
        return rt_string_ref(text);

    // Build result without common indent
    size_t src_size = 0;
    size_t alloc_size = 0;
    if (!checked_i64_to_size(src_len, "TextWrapper.Dedent: invalid length", &src_size) ||
        !checked_add_one(src_size, "TextWrapper.Dedent: output length overflow", &alloc_size)) {
        return textwrap_empty_string();
    }
    char *result = checked_malloc(alloc_size, "TextWrapper.Dedent: memory allocation failed");
    if (!result)
        return textwrap_empty_string();

    int64_t result_pos = 0;
    line_start = 0;
    while (line_start < src_len) {
        int64_t line_end = line_start;
        while (line_end < src_len && src[line_end] != '\n')
            line_end++;

        int64_t pos = line_start;
        while (pos < line_end && (src[pos] == ' ' || src[pos] == '\t'))
            pos++;

        int64_t skip = 0;
        if (pos < line_end) {
            while (skip < common_len && line_start + skip < line_end &&
                   src[common_start + skip] == src[line_start + skip]) {
                skip++;
            }
        }

        int64_t copy_start = line_start + skip;
        if (copy_start < line_end) {
            memcpy(result + result_pos, src + copy_start, (size_t)(line_end - copy_start));
            result_pos += line_end - copy_start;
        }
        if (line_end < src_len && src[line_end] == '\n')
            result[result_pos++] = '\n';

        line_start = line_end + 1;
    }

    result[result_pos] = '\0';
    rt_string ret = rt_string_from_bytes(result, result_pos);
    free(result);
    return ret;
}

/// @brief Indent every line *except* the first with `prefix` (hanging-indent style).
/// @details Useful for "labelled paragraph" output where the first line
///          carries the label and continuation lines are indented to
///          align under the body. The first physical line is left
///          alone; every subsequent line gets `prefix` prepended.
/// @param text Borrowed runtime text; null becomes empty.
/// @param prefix Borrowed continuation prefix; null is treated as empty.
/// @return Caller-owned hanging-indent string.
rt_string rt_textwrap_hang(rt_string text, rt_string prefix) {
    if (!text)
        return textwrap_empty_string();
    if (!textwrap_valid_string(text, "TextWrapper.Hang: invalid string"))
        return textwrap_empty_string();
    if (!textwrap_valid_string(prefix, "TextWrapper.Hang: invalid prefix"))
        prefix = NULL;

    const char *src = rt_string_cstr(text);
    int64_t src_len = rt_str_len(text);
    const char *pre = prefix ? rt_string_cstr(prefix) : "";
    int64_t pre_len = rt_str_len(prefix);

    // Count lines
    int64_t line_count = 0;
    for (int64_t i = 0; i < src_len; i++) {
        if (src[i] == '\n')
            line_count++;
    }

    size_t src_size = 0;
    size_t line_count_size = 0;
    size_t prefix_size = 0;
    size_t base_size = 0;
    size_t alloc_size = 0;
    if (!checked_i64_to_size(src_len, "TextWrapper.Hang: invalid length", &src_size) ||
        !checked_i64_to_size(
            line_count, "TextWrapper.Hang: invalid line count", &line_count_size) ||
        !checked_i64_to_size(pre_len, "TextWrapper.Hang: invalid prefix length", &prefix_size) ||
        !checked_add_one(src_size, "TextWrapper.Hang: output length overflow", &base_size) ||
        !checked_mul_add(base_size,
                         line_count_size,
                         prefix_size,
                         "TextWrapper.Hang: output length overflow",
                         &alloc_size)) {
        return textwrap_empty_string();
    }
    char *result = checked_malloc(alloc_size, "TextWrapper.Hang: memory allocation failed");
    if (!result)
        return textwrap_empty_string();

    int64_t result_pos = 0;
    int first_line = 1;

    for (int64_t i = 0; i < src_len; i++) {
        if (!first_line && (i == 0 || src[i - 1] == '\n')) {
            memcpy(result + result_pos, pre, (size_t)pre_len);
            result_pos += pre_len;
        }

        result[result_pos++] = src[i];

        if (src[i] == '\n') {
            first_line = 0;
        }
    }

    result[result_pos] = '\0';
    rt_string ret = rt_string_from_bytes(result, result_pos);
    free(result);
    return ret;
}

//=============================================================================
// Truncation
//=============================================================================

/// @brief Truncate to `width` characters total, appending `"..."` if it didn't fit.
/// @param text Borrowed runtime text.
/// @param width Maximum output byte budget.
/// @return Caller-owned truncated string with a default ellipsis when shortened.
rt_string rt_textwrap_truncate(rt_string text, int64_t width) {
    rt_string suffix = rt_const_cstr("...");
    rt_string result = rt_textwrap_truncate_with(text, width, suffix);
    rt_string_unref(suffix);
    return result;
}

/// @brief Truncate to `width` characters total, appending `suffix` if shortened.
/// @details If `text` already fits, returns it unchanged. Otherwise
///          keeps `width - suffix_len` characters and appends `suffix`.
///          Edge case: if `width <= suffix_len`, returns just the
///          suffix truncated to a UTF-8 boundary.
/// @param text Borrowed runtime text.
/// @param width Maximum output byte budget.
/// @param suffix Borrowed truncation suffix; null becomes empty.
/// @return Caller-owned truncated string or retained input when it already fits.
rt_string rt_textwrap_truncate_with(rt_string text, int64_t width, rt_string suffix) {
    if (!text)
        return textwrap_empty_string();
    if (!textwrap_valid_string(text, "TextWrapper.Truncate: invalid string"))
        return textwrap_empty_string();
    rt_string suffix_eff = suffix;
    int release_suffix = 0;
    if (!suffix_eff) {
        suffix_eff = rt_const_cstr("");
        release_suffix = 1;
    } else if (!textwrap_valid_string(suffix_eff, "TextWrapper.Truncate: invalid suffix")) {
        suffix_eff = rt_const_cstr("");
        release_suffix = 1;
    }

    int64_t text_len = rt_str_len(text);
    int64_t suffix_len = rt_str_len(suffix_eff);

    if (text_len <= width) {
        if (release_suffix)
            rt_string_unref(suffix_eff);
        return rt_string_ref(text);
    }

    if (width <= suffix_len) {
        const char *sfx = rt_string_cstr(suffix_eff);
        int64_t cut = textwrap_boundary_at_or_before(sfx, width, 0);
        rt_string result = rt_str_substr(suffix_eff, 0, cut);
        if (release_suffix)
            rt_string_unref(suffix_eff);
        return result;
    }

    int64_t keep = width - suffix_len;
    keep = textwrap_boundary_at_or_before(rt_string_cstr(text), keep, 0);
    rt_string kept = rt_str_substr(text, 0, keep);
    rt_string result = rt_str_concat(kept, rt_string_ref(suffix_eff));
    if (release_suffix)
        rt_string_unref(suffix_eff);
    return result;
}

/// @brief Shorten by replacing the middle with `"..."` while preserving start and end.
/// @details Useful for paths and identifiers where keeping both ends
///          helps readability. Splits the keep budget roughly in half
///          (right side gets the extra char on odd widths). Falls
///          back to a head-truncate when `width < 5` (no room for
///          even three dots plus one char on each side).
/// @param text Borrowed runtime text.
/// @param width Maximum output byte budget.
/// @return Caller-owned shortened string or retained input when it fits.
rt_string rt_textwrap_shorten(rt_string text, int64_t width) {
    if (!text)
        return textwrap_empty_string();
    if (!textwrap_valid_string(text, "TextWrapper.Shorten: invalid string"))
        return textwrap_empty_string();

    int64_t text_len = rt_str_len(text);

    if (text_len <= width)
        return rt_string_ref(text);

    const char *src = rt_string_cstr(text);
    if (width < 5)
        return rt_str_substr(text, 0, textwrap_boundary_at_or_before(src, width, 0));

    int64_t left = (width - 3) / 2;
    int64_t right = width - 3 - left;

    left = textwrap_boundary_at_or_before(src, left, 0);
    int64_t right_start = text_len - right;
    while (right_start < text_len && textwrap_is_continuation(src, right_start))
        right_start++; // never begin the tail slice inside a codepoint
    rt_string left_part = rt_str_substr(text, 0, left);
    rt_string right_part = rt_str_substr(text, right_start, text_len - right_start);

    rt_string result = rt_str_concat(left_part, rt_const_cstr("..."));
    return rt_str_concat(result, right_part);
}

//=============================================================================
// Alignment
//=============================================================================

/// @brief Left-justify `text` in a `width`-column field, padding with trailing spaces.
/// @details If `text` is already as wide or wider than `width`, it's
///          returned unchanged.
/// @param text Borrowed runtime text; null is treated as zero bytes.
/// @param width Target byte width.
/// @return Caller-owned retained or space-padded string.
rt_string rt_textwrap_left(rt_string text, int64_t width) {
    if (!text) {
        if (width <= 0)
            return textwrap_empty_string();
        char *spaces = alloc_spaces(width, "TextWrapper.Left: memory allocation failed");
        if (!spaces)
            return textwrap_empty_string();
        rt_string result = rt_string_from_bytes(spaces, width);
        free(spaces);
        return result;
    }
    if (!textwrap_valid_string(text, "TextWrapper.Left: invalid string"))
        return textwrap_empty_string();

    int64_t text_len = rt_str_len(text);
    if (text_len >= width)
        return rt_string_ref(text);

    int64_t pad = width - text_len;
    char *spaces = alloc_spaces(pad, "TextWrapper.Left: memory allocation failed");
    if (!spaces)
        return textwrap_empty_string();

    rt_string padding = rt_string_from_bytes(spaces, pad);
    free(spaces);

    return rt_str_concat(rt_string_ref(text), padding);
}

/// @brief Right-justify `text` in a `width`-column field, padding with leading spaces.
/// @param text Borrowed runtime text; null is treated as zero bytes.
/// @param width Target byte width.
/// @return Caller-owned retained or space-padded string.
rt_string rt_textwrap_right(rt_string text, int64_t width) {
    if (!text) {
        if (width <= 0)
            return textwrap_empty_string();
        char *spaces = alloc_spaces(width, "TextWrapper.Right: memory allocation failed");
        if (!spaces)
            return textwrap_empty_string();
        rt_string result = rt_string_from_bytes(spaces, width);
        free(spaces);
        return result;
    }
    if (!textwrap_valid_string(text, "TextWrapper.Right: invalid string"))
        return textwrap_empty_string();

    int64_t text_len = rt_str_len(text);
    if (text_len >= width)
        return rt_string_ref(text);

    int64_t pad = width - text_len;
    char *spaces = alloc_spaces(pad, "TextWrapper.Right: memory allocation failed");
    if (!spaces)
        return textwrap_empty_string();

    rt_string padding = rt_string_from_bytes(spaces, pad);
    free(spaces);

    return rt_str_concat(padding, rt_string_ref(text));
}

/// @brief Center `text` in a `width`-column field with balanced space padding.
/// @details For odd-padding cases the extra space goes on the right
///          (matching Python's `str.center`).
/// @param text Borrowed runtime text; null is treated as zero bytes.
/// @param width Target byte width.
/// @return Caller-owned retained or space-padded string.
rt_string rt_textwrap_center(rt_string text, int64_t width) {
    if (!text) {
        if (width <= 0)
            return textwrap_empty_string();
        char *spaces = alloc_spaces(width, "TextWrapper.Center: memory allocation failed");
        if (!spaces)
            return textwrap_empty_string();
        rt_string result = rt_string_from_bytes(spaces, width);
        free(spaces);
        return result;
    }
    if (!textwrap_valid_string(text, "TextWrapper.Center: invalid string"))
        return textwrap_empty_string();

    int64_t text_len = rt_str_len(text);
    if (text_len >= width)
        return rt_string_ref(text);

    int64_t total_pad = width - text_len;
    int64_t left_pad = total_pad / 2;
    int64_t right_pad = total_pad - left_pad;

    char *left_spaces = alloc_spaces(left_pad, "TextWrapper.Center: memory allocation failed");
    char *right_spaces = alloc_spaces(right_pad, "TextWrapper.Center: memory allocation failed");
    if (!left_spaces || !right_spaces) {
        free(left_spaces);
        free(right_spaces);
        return textwrap_empty_string();
    }

    rt_string left_padding = rt_string_from_bytes(left_spaces, left_pad);
    rt_string right_padding = rt_string_from_bytes(right_spaces, right_pad);
    free(left_spaces);
    free(right_spaces);

    rt_string result = rt_str_concat(left_padding, rt_string_ref(text));
    return rt_str_concat(result, right_padding);
}

//=============================================================================
// Utility
//=============================================================================

/// @brief Count physical lines without adding one for a trailing line feed.
/// @details Empty input still counts as 1 line. Trailing newline does
///          not add an extra empty line.
/// @param text Borrowed runtime text; null counts as one line.
/// @return Physical line count under the trailing-newline rule.
int64_t rt_textwrap_line_count(rt_string text) {
    if (!text)
        return 1;

    const char *src = rt_string_cstr(text);
    int64_t len = rt_str_len(text);
    int64_t count = 1;

    for (int64_t i = 0; i < len; i++) {
        if (src[i] == '\n' && i + 1 < len)
            count++;
    }

    return count;
}

/// @brief Return the byte length of the longest line in `text`.
/// @details Walks the input once, tracking the running line length
///          and resetting it on each newline.
/// @param text Borrowed runtime text; null has length zero.
/// @return Maximum line length measured in bytes.
int64_t rt_textwrap_max_line_len(rt_string text) {
    if (!text)
        return 0;

    const char *src = rt_string_cstr(text);
    int64_t len = rt_str_len(text);
    int64_t max_len = 0;
    int64_t current_len = 0;

    for (int64_t i = 0; i < len; i++) {
        if (src[i] == '\n') {
            if (current_len > max_len)
                max_len = current_len;
            current_len = 0;
        } else {
            current_len++;
        }
    }

    if (current_len > max_len)
        max_len = current_len;

    return max_len;
}
