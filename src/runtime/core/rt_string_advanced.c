//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_string_advanced.c
// Purpose: Implements extended byte-string search, replacement, padding,
//   splitting/joining, repetition, character classification, strict UTF-8
//   traversal, reversal, and three-way comparison for the runtime.
//
// Key invariants:
//   - General search, replacement, padding, splitting, and comparison operate
//     on stored byte lengths and therefore preserve embedded NUL bytes.
//   - Returned strings own one reference but may be a retained input or the
//     immortal empty singleton instead of a fresh allocation.
//   - Split/Lines return owned-element Seq objects. Join only borrows its Seq,
//     separator, and element handles.
//   - UTF-8-aware helpers reject overlong encodings, surrogate code points,
//     truncated sequences, and scalars above U+10FFFF.
//   - Three-way comparisons return exactly -1, 0, or 1 and order NULL before
//     every non-null string.
//
// Ownership/Lifetime:
//   - Every returned rt_string transfers one owned reference to the caller.
//   - String and Seq inputs are borrowed unless a function explicitly retains
//     an input for its returned result.
//   - Sequence containers returned by Split/Lines own references to their
//     string elements and must be released through the Seq API.
//
// Links: src/runtime/core/rt_string_internal.h (shared helpers),
//        src/runtime/core/rt_string_ops.c (core operations),
//        src/runtime/core/rt_string.h (public API)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Extended runtime byte-string and strict UTF-8 operations.

#include "rt_ascii.h"
#include "rt_internal.h"
#include "rt_regex_internal.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_string_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Extended String Functions (Zanna.String expansion)
//===----------------------------------------------------------------------===//

/// @brief Validate an optional runtime string before reading its wrapper.
/// @param str Candidate borrowed string; null is accepted.
/// @param diagnostic Operation-specific invalid-handle message.
/// @return One for null or a registered live string, otherwise zero after a
///         recoverable trap.
static int rt_string_arg_valid_(rt_string str, const char *diagnostic) {
    if (!str || rt_string_is_handle(str))
        return 1;
    rt_trap(diagnostic);
    return 0;
}

/// @brief Replace every non-overlapping occurrence of @p needle.
/// @details Scans the stored bytes once, using the first needle byte to locate
///          candidates before verifying the complete match. A null haystack
///          returns the empty singleton. A null replacement, null/empty needle,
///          overlong needle, or absence of matches retains the original
///          haystack instead of allocating a copy.
/// @param haystack Borrowed source byte string; may be NULL.
/// @param needle Borrowed non-empty byte sequence to find; may be NULL.
/// @param replacement Borrowed replacement bytes; may be NULL.
/// @return Owned transformed string, retained haystack, empty singleton, or
///         `NULL` after a builder/allocation trap.
rt_string rt_str_replace(rt_string haystack, rt_string needle, rt_string replacement) {
    if (!rt_string_arg_valid_(haystack, "String.Replace: invalid source") ||
        !rt_string_arg_valid_(needle, "String.Replace: invalid needle") ||
        !rt_string_arg_valid_(replacement, "String.Replace: invalid replacement")) {
        return NULL;
    }
    if (!haystack)
        return rt_empty_string();
    if (!needle || !replacement)
        return rt_string_ref(haystack);

    size_t hay_len = rt_string_len_bytes(haystack);
    size_t needle_len = rt_string_len_bytes(needle);
    size_t repl_len = rt_string_len_bytes(replacement);

    // Empty needle: return original string
    if (needle_len == 0)
        return rt_string_ref(haystack);
    if (needle_len > hay_len)
        return rt_string_ref(haystack);

    // Single-pass algorithm using string builder.
    // This eliminates the double-scan (count + build) that was O(2*n*m).
    // Instead we scan once, building the result as we go.
    rt_string_builder sb;
    rt_sb_init(&sb);

    const char *p = haystack->data;
    const char *end = p + hay_len;
    const char *prev = p;
    const char first = needle->data[0];
    int found_any = 0;

    // Use memchr for fast first-character scanning (SIMD-optimized)
    while (p <= end - needle_len) {
        // Fast scan for first character of needle
        const char *match = memchr(p, first, (size_t)(end - needle_len - p + 1));
        if (!match)
            break;

        p = match;
        if (memcmp(p, needle->data, needle_len) == 0) {
            found_any = 1;
            // Append chunk before match
            size_t chunk = (size_t)(p - prev);
            if (chunk > 0) {
                if (rt_sb_append_bytes(&sb, prev, chunk) != RT_SB_OK) {
                    rt_sb_free(&sb);
                    rt_trap("rt_str_replace: allocation failed");
                    return NULL;
                }
            }
            // Append replacement
            if (repl_len > 0) {
                if (rt_sb_append_bytes(&sb, replacement->data, repl_len) != RT_SB_OK) {
                    rt_sb_free(&sb);
                    rt_trap("rt_str_replace: allocation failed");
                    return NULL;
                }
            }
            p += needle_len;
            prev = p;
        } else {
            p++;
        }
    }

    // No matches found - return original string (avoid allocation)
    if (!found_any) {
        rt_sb_free(&sb);
        return rt_string_ref(haystack);
    }

    // Append remainder after last match
    size_t remainder = (size_t)(end - prev);
    if (remainder > 0) {
        if (rt_sb_append_bytes(&sb, prev, remainder) != RT_SB_OK) {
            rt_sb_free(&sb);
            rt_trap("rt_str_replace: allocation failed");
            return NULL;
        }
    }

    // Create result string from builder
    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);

    if (!result) {
        rt_trap("rt_str_replace: allocation failed");
        return NULL;
    }
    return result;
}

/// @brief Test whether @p str begins with @p prefix.
/// @details Compares stored bytes, so embedded NULs are significant. An empty
///          prefix matches every non-null string.
/// @param str Borrowed source string; NULL never matches.
/// @param prefix Borrowed prefix; NULL never matches.
/// @return One when the complete prefix matches; otherwise zero.
int64_t rt_str_starts_with(rt_string str, rt_string prefix) {
    if (!rt_string_arg_valid_(str, "String.StartsWith: invalid source") ||
        !rt_string_arg_valid_(prefix, "String.StartsWith: invalid prefix")) {
        return 0;
    }
    if (!str || !prefix)
        return 0;

    size_t str_len = rt_string_len_bytes(str);
    size_t prefix_len = rt_string_len_bytes(prefix);

    if (prefix_len > str_len)
        return 0;
    if (prefix_len == 0)
        return 1;

    return memcmp(str->data, prefix->data, prefix_len) == 0;
}

/// @brief Test whether @p str ends with @p suffix.
/// @details Compares stored bytes, so embedded NULs are significant. An empty
///          suffix matches every non-null string.
/// @param str Borrowed source string; NULL never matches.
/// @param suffix Borrowed suffix; NULL never matches.
/// @return One when the complete suffix matches; otherwise zero.
int64_t rt_str_ends_with(rt_string str, rt_string suffix) {
    if (!rt_string_arg_valid_(str, "String.EndsWith: invalid source") ||
        !rt_string_arg_valid_(suffix, "String.EndsWith: invalid suffix")) {
        return 0;
    }
    if (!str || !suffix)
        return 0;

    size_t str_len = rt_string_len_bytes(str);
    size_t suffix_len = rt_string_len_bytes(suffix);

    if (suffix_len > str_len)
        return 0;
    if (suffix_len == 0)
        return 1;

    return memcmp(str->data + str_len - suffix_len, suffix->data, suffix_len) == 0;
}

/// @brief Test whether @p str contains @p needle.
/// @details Performs a byte-wise substring search. An empty needle matches
///          every non-null source, including an empty source.
/// @param str Borrowed source string; NULL never matches.
/// @param needle Borrowed byte sequence; NULL never matches.
/// @return One when a match exists; otherwise zero.
int64_t rt_str_has(rt_string str, rt_string needle) {
    if (!rt_string_arg_valid_(str, "String.Has: invalid source") ||
        !rt_string_arg_valid_(needle, "String.Has: invalid needle")) {
        return 0;
    }
    if (!str || !needle)
        return 0;

    size_t str_len = rt_string_len_bytes(str);
    size_t needle_len = rt_string_len_bytes(needle);

    if (needle_len == 0)
        return 1;
    if (needle_len > str_len)
        return 0;

    // Simple substring search
    for (size_t i = 0; i + needle_len <= str_len; i++) {
        if (memcmp(str->data + i, needle->data, needle_len) == 0)
            return 1;
    }
    return 0;
}

/// @brief Count non-overlapping byte occurrences of @p needle in @p str.
/// @details Advances by the complete needle length after a match and by one
///          byte after a miss. Null operands and an empty or overlong needle
///          yield zero. Result overflow traps before returning `INT64_MAX` if
///          the trap handler returns.
/// @param str Borrowed source string; may be NULL.
/// @param needle Borrowed byte sequence; may be NULL.
/// @return Number of non-overlapping matches, or zero for invalid/no matches.
int64_t rt_str_count(rt_string str, rt_string needle) {
    if (!rt_string_arg_valid_(str, "String.Count: invalid source") ||
        !rt_string_arg_valid_(needle, "String.Count: invalid needle")) {
        return 0;
    }
    if (!str || !needle)
        return 0;

    size_t str_len = rt_string_len_bytes(str);
    size_t needle_len = rt_string_len_bytes(needle);

    if (needle_len == 0)
        return 0;
    if (needle_len > str_len)
        return 0;

    int64_t count = 0;
    const char *p = str->data;
    const char *end = p + str_len;

    while (p <= end - needle_len) {
        if (memcmp(p, needle->data, needle_len) == 0) {
            if (count == INT64_MAX) {
                rt_trap("String.Count: result too large");
                return INT64_MAX;
            }
            count++;
            p += needle_len; // Non-overlapping
        } else {
            p++;
        }
    }

    return count;
}

/// @brief Pad @p str on the left to the requested byte width.
/// @details The padding string must contain exactly one byte when padding is
///          required; this prevents repetition of part of a multibyte UTF-8
///          sequence. Null input returns the empty singleton. Non-positive or
///          already-satisfied widths and null/empty padding retain @p str.
///          Invalid or unrepresentable widths trap.
/// @param str Borrowed source string; may be NULL.
/// @param width Target stored-byte length.
/// @param pad_str Borrowed string supplying the single padding byte.
/// @return Owned padded string, retained source, empty singleton, or `NULL`
///         after validation/allocation failure.
rt_string rt_str_pad_left(rt_string str, int64_t width, rt_string pad_str) {
    if (!rt_string_arg_valid_(str, "String.PadLeft: invalid source") ||
        !rt_string_arg_valid_(pad_str, "String.PadLeft: invalid padding")) {
        return NULL;
    }
    if (!str)
        return rt_empty_string();

    size_t str_len = rt_string_len_bytes(str);

    if (width <= 0 || !pad_str || rt_string_len_bytes(pad_str) == 0)
        return rt_string_ref(str);
    // Width is a BYTE width, so the padding must be exactly one byte:
    // repeating the first byte of a multibyte character would emit
    // malformed UTF-8 (VDOC-167).
    if (rt_string_len_bytes(pad_str) != 1) {
        rt_trap("String.PadLeft: padding must be a single byte");
        return NULL;
    }
    uint64_t requested_width = (uint64_t)width;
    if (requested_width <= (uint64_t)str_len)
        return rt_string_ref(str);
    if (requested_width > (uint64_t)(SIZE_MAX - 1)) {
        rt_trap("String.PadLeft: width too large");
        return NULL;
    }

    char pad_char = pad_str->data[0];
    size_t target = (size_t)requested_width;
    size_t pad_count = target - str_len;

    rt_string result = rt_string_alloc(target, target + 1);
    if (!result)
        return NULL;

    memset(result->data, pad_char, pad_count);
    memcpy(result->data + pad_count, str->data, str_len);
    result->data[target] = '\0';

    return result;
}

/// @brief Pad @p str on the right to the requested byte width.
/// @details Applies the same byte-width, single-byte-padding, null, overflow,
///          and ownership rules as @ref rt_str_pad_left, but appends padding
///          after the original bytes.
/// @param str Borrowed source string; may be NULL.
/// @param width Target stored-byte length.
/// @param pad_str Borrowed string supplying the single padding byte.
/// @return Owned padded string, retained source, empty singleton, or `NULL`
///         after validation/allocation failure.
rt_string rt_str_pad_right(rt_string str, int64_t width, rt_string pad_str) {
    if (!rt_string_arg_valid_(str, "String.PadRight: invalid source") ||
        !rt_string_arg_valid_(pad_str, "String.PadRight: invalid padding")) {
        return NULL;
    }
    if (!str)
        return rt_empty_string();

    size_t str_len = rt_string_len_bytes(str);

    if (width <= 0 || !pad_str || rt_string_len_bytes(pad_str) == 0)
        return rt_string_ref(str);
    if (rt_string_len_bytes(pad_str) != 1) {
        rt_trap("String.PadRight: padding must be a single byte");
        return NULL;
    }
    uint64_t requested_width = (uint64_t)width;
    if (requested_width <= (uint64_t)str_len)
        return rt_string_ref(str);
    if (requested_width > (uint64_t)(SIZE_MAX - 1)) {
        rt_trap("String.PadRight: width too large");
        return NULL;
    }

    char pad_char = pad_str->data[0];
    size_t target = (size_t)requested_width;
    size_t pad_count = target - str_len;

    rt_string result = rt_string_alloc(target, target + 1);
    if (!result)
        return NULL;

    memcpy(result->data, str->data, str_len);
    memset(result->data + str_len, pad_char, pad_count);
    result->data[target] = '\0';

    return result;
}

/// @brief Split @p str at non-overlapping occurrences of @p delim.
/// @details Uses stored-byte matching and preserves leading, adjacent, and
///          trailing empty segments. A null source yields one empty element.
///          A null/empty delimiter or a delimiter longer than the source yields
///          one element containing the complete source. The result Seq owns
///          its element references.
/// @param str Borrowed source string; may be NULL.
/// @param delim Borrowed delimiter byte string; may be NULL.
/// @return Newly allocated owned-element Seq containing the segments, or
///         `NULL` after a result-size trap; allocation behavior otherwise
///         follows the Seq and string constructors.
void *rt_str_split(rt_string str, rt_string delim) {
    if (!rt_string_arg_valid_(str, "String.Split: invalid source") ||
        !rt_string_arg_valid_(delim, "String.Split: invalid delimiter")) {
        return NULL;
    }
    if (!str) {
        // Push empty string for null input
        void *result = rt_seq_with_capacity_owned(1);
        rt_seq_push(result, (void *)rt_empty_string());
        return result;
    }

    size_t str_len = rt_string_len_bytes(str);
    size_t delim_len = delim ? rt_string_len_bytes(delim) : 0;

    // Empty delimiter: return single element with original string
    if (delim_len == 0 || delim_len > str_len) {
        void *result = rt_seq_with_capacity_owned(1);
        rt_seq_push(result, (void *)str);
        return result;
    }

    // Pass 1: Count delimiters to pre-allocate result sequence
    // Uses memchr for SIMD-optimized first-character scanning
    const char *p = str->data;
    const char *end = str->data + str_len;
    const char first = delim->data[0];
    size_t count = 1; // At least one segment

    while (p <= end - delim_len) {
        const char *match = memchr(p, first, (size_t)(end - delim_len - p + 1));
        if (!match)
            break;

        p = match;
        if (memcmp(p, delim->data, delim_len) == 0) {
            if (count == (size_t)INT64_MAX) {
                rt_trap("String.Split: result too large");
                return NULL;
            }
            count++;
            p += delim_len;
        } else {
            p++;
        }
    }

    // Pre-allocate sequence with exact capacity
    void *result = rt_seq_with_capacity_owned((int64_t)count);

    // Pass 2: Build segments
    const char *start = str->data;
    p = str->data;

    while (p <= end - delim_len) {
        const char *match = memchr(p, first, (size_t)(end - delim_len - p + 1));
        if (!match)
            break;

        p = match;
        if (memcmp(p, delim->data, delim_len) == 0) {
            size_t chunk_len = (size_t)(p - start);
            rt_string chunk = rt_string_from_bytes(start, chunk_len);
            rt_seq_push(result, (void *)chunk);
            rt_string_unref(chunk);
            p += delim_len;
            start = p;
        } else {
            p++;
        }
    }

    // Add final segment
    size_t final_len = (size_t)(end - start);
    rt_string final_str = rt_string_from_bytes(start, final_len);
    rt_seq_push(result, (void *)final_str);
    rt_string_unref(final_str);

    return result;
}

/// @brief Split @p str into logical lines and normalize CRLF boundaries.
/// @details Splits at each LF byte, preserves empty segments including a final
///          one after trailing LF, and removes at most one CR immediately
///          before each LF or end of input. A null source yields one empty
///          line. The returned Seq owns its line-string references.
/// @param str Borrowed source string; may be NULL.
/// @return Newly allocated owned-element Seq of normalized line strings, or
///         `NULL` after a result-size trap; allocation behavior otherwise
///         follows the Seq and string constructors.
void *rt_str_lines(rt_string str) {
    if (!rt_string_arg_valid_(str, "String.Lines: invalid source"))
        return NULL;
    if (!str) {
        // Mirror rt_str_split: a null source yields a single empty segment.
        void *result = rt_seq_with_capacity_owned(1);
        rt_seq_push(result, (void *)rt_empty_string());
        return result;
    }

    size_t str_len = rt_string_len_bytes(str);
    const char *data = str->data;

    // Pass 1: count newlines so the result has exactly newlines + 1 segments.
    size_t count = 1;
    for (size_t i = 0; i < str_len; i++) {
        if (data[i] == '\n') {
            if (count == (size_t)INT64_MAX) {
                rt_trap("String.Lines: result too large");
                return NULL;
            }
            count++;
        }
    }

    void *result = rt_seq_with_capacity_owned((int64_t)count);

    // Pass 2: emit each segment, dropping one trailing '\r' (CRLF -> LF).
    size_t start = 0;
    for (size_t i = 0; i <= str_len; i++) {
        if (i == str_len || data[i] == '\n') {
            size_t seg_len = i - start;
            if (seg_len > 0 && data[start + seg_len - 1] == '\r')
                seg_len--;
            rt_string seg = rt_string_from_bytes(data + start, seg_len);
            rt_seq_push(result, (void *)seg);
            rt_string_unref(seg);
            start = i + 1;
        }
    }

    return result;
}

//=============================================================================
// Zanna.Text.Char — ASCII character classification (identifier rules)
//=============================================================================

/// @brief Test whether a byte is an ASCII alphabetic character.
/// @param c Unsigned byte value to classify.
/// @return Nonzero for `A-Z` or `a-z`; otherwise zero.
static int rt_char_is_ascii_alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/// @brief Test whether a byte is an ASCII decimal digit.
/// @param c Unsigned byte value to classify.
/// @return Nonzero for `0-9`; otherwise zero.
static int rt_char_is_ascii_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

/// @brief Read the first stored byte for ASCII character classification.
/// @details The caller intentionally classifies bytes rather than Unicode
///          codepoints, so a UTF-8 leading byte does not qualify as an ASCII
///          identifier character.
/// @param s Borrowed source string; may be NULL or empty.
/// @return First byte promoted as an unsigned value, or -1 for no byte.
static int rt_char_first_byte(rt_string s) {
    if (!rt_string_arg_valid_(s, "Text.Char: invalid string"))
        return -1;
    if (!s)
        return -1;
    const char *d = rt_string_cstr(s);
    int64_t n = rt_str_len(s);
    if (!d || n <= 0)
        return -1;
    return (unsigned char)d[0];
}

/// @brief Test whether the first byte may start an ASCII identifier.
/// @param s Borrowed source string; may be NULL or empty.
/// @return One for an ASCII letter or underscore; otherwise zero.
int8_t rt_text_char_is_identifier_start(rt_string s) {
    int c = rt_char_first_byte(s);
    if (c < 0)
        return 0;
    return (rt_char_is_ascii_alpha((unsigned char)c) || c == '_') ? 1 : 0;
}

/// @brief Test whether the first byte may continue an ASCII identifier.
/// @param s Borrowed source string; may be NULL or empty.
/// @return One for an ASCII letter, digit, or underscore; otherwise zero.
int8_t rt_text_char_is_identifier_part(rt_string s) {
    int c = rt_char_first_byte(s);
    if (c < 0)
        return 0;
    unsigned char ch = (unsigned char)c;
    return (rt_char_is_ascii_alpha(ch) || rt_char_is_ascii_digit(ch) || c == '_') ? 1 : 0;
}

/// @brief Test whether the first byte is ASCII alphanumeric.
/// @param s Borrowed source string; may be NULL or empty.
/// @return One for an ASCII letter or digit; otherwise zero.
int8_t rt_text_char_is_alnum(rt_string s) {
    int c = rt_char_first_byte(s);
    if (c < 0)
        return 0;
    unsigned char ch = (unsigned char)c;
    return (rt_char_is_ascii_alpha(ch) || rt_char_is_ascii_digit(ch)) ? 1 : 0;
}

/// @copydoc rt_text_char_is_word
int8_t rt_text_char_is_word(rt_string s) {
    if (!rt_string_arg_valid_(s, "Text.Char.IsWord: invalid string") || !s)
        return 0;
    const char *bytes = rt_string_cstr(s);
    size_t length = rt_string_len_bytes(s);
    size_t width = rt_utf8_strict_step(bytes, length);
    if (!bytes || width == 0)
        return 0;

    const unsigned char *input = (const unsigned char *)bytes;
    uint32_t codepoint = 0;
    if (width == 1) {
        codepoint = input[0];
    } else if (width == 2) {
        codepoint = ((uint32_t)(input[0] & 0x1Fu) << 6) | (uint32_t)(input[1] & 0x3Fu);
    } else if (width == 3) {
        codepoint = ((uint32_t)(input[0] & 0x0Fu) << 12) | ((uint32_t)(input[1] & 0x3Fu) << 6) |
                    (uint32_t)(input[2] & 0x3Fu);
    } else {
        codepoint = ((uint32_t)(input[0] & 0x07u) << 18) | ((uint32_t)(input[1] & 0x3Fu) << 12) |
                    ((uint32_t)(input[2] & 0x3Fu) << 6) | (uint32_t)(input[3] & 0x3Fu);
    }
    return re_is_word_codepoint(codepoint) ? 1 : 0;
}

/// @brief Join the string elements of @p seq with @p sep between them.
/// @details Borrows the sequence and every element. Null elements and a null
///          separator contribute zero bytes, although separator positions
///          still correspond to element boundaries. A null or empty sequence
///          returns the empty singleton. Total-length overflow traps.
/// @param sep Borrowed separator string; NULL means empty.
/// @param seq Borrowed Seq of runtime string handles; may be NULL.
/// @return Owned joined string or empty singleton, or `NULL` on overflow or
///         allocation failure.
rt_string rt_str_join(rt_string sep, void *seq) {
    if (!rt_string_arg_valid_(sep, "String.Join: invalid separator"))
        return NULL;
    if (!seq)
        return rt_empty_string();

    int64_t len = rt_seq_len(seq);
    if (len == 0)
        return rt_empty_string();

    size_t sep_len = sep ? rt_string_len_bytes(sep) : 0;

    // Calculate total length
    size_t total = 0;
    for (int64_t i = 0; i < len; i++) {
        rt_string item = (rt_string)rt_seq_get(seq, i);
        if (!rt_string_arg_valid_(item, "String.Join: invalid sequence item"))
            return NULL;
        size_t item_len = item ? rt_string_len_bytes(item) : 0;
        if (total > SIZE_MAX - item_len) {
            rt_trap("rt_str_join: length overflow");
            return NULL;
        }
        total += item_len;
        if (i < len - 1 && sep_len > 0) {
            if (total > SIZE_MAX - sep_len) {
                rt_trap("rt_str_join: length overflow");
                return NULL;
            }
            total += sep_len;
        }
    }

    rt_string result = rt_string_alloc(total, total + 1);
    if (!result)
        return NULL;

    char *dst = result->data;
    for (int64_t i = 0; i < len; i++) {
        rt_string item = (rt_string)rt_seq_get(seq, i);
        if (!rt_string_arg_valid_(item, "String.Join: invalid sequence item")) {
            rt_string_unref(result);
            return NULL;
        }
        size_t item_len = item ? rt_string_len_bytes(item) : 0;
        if (item_len > 0) {
            memcpy(dst, item->data, item_len);
            dst += item_len;
        }

        if (i < len - 1 && sep_len > 0) {
            memcpy(dst, sep->data, sep_len);
            dst += sep_len;
        }
    }

    *dst = '\0';
    return result;
}

/// @brief Repeat the stored bytes of @p str @p count times.
/// @details Null/empty input and non-positive counts return the empty
///          singleton. The multiplication is checked against `SIZE_MAX`
///          before allocating the result.
/// @param str Borrowed source string; may be NULL.
/// @param count Number of repetitions.
/// @return Owned repeated string or empty singleton, or `NULL` after an
///         overflow trap or allocation failure.
rt_string rt_str_repeat(rt_string str, int64_t count) {
    if (!rt_string_arg_valid_(str, "String.Repeat: invalid source"))
        return NULL;
    if (!str || count <= 0)
        return rt_empty_string();

    size_t str_len = rt_string_len_bytes(str);
    if (str_len == 0)
        return rt_empty_string();

    // Check for overflow
    if ((size_t)count > SIZE_MAX / str_len) {
        rt_trap("rt_str_repeat: length overflow");
        return NULL;
    }

    size_t total = str_len * (size_t)count;
    rt_string result = rt_string_alloc(total, total + 1);
    if (!result)
        return NULL;

    char *dst = result->data;
    for (int64_t i = 0; i < count; i++) {
        memcpy(dst, str->data, str_len);
        dst += str_len;
    }

    *dst = '\0';
    return result;
}

/// @brief Validate and measure one strict UTF-8 codepoint.
/// @details Accepts ASCII and canonical two-, three-, or four-byte sequences.
///          Rejects invalid lead/continuation bytes, truncation, overlong
///          encodings, UTF-16 surrogates, and values above U+10FFFF. This
///          helper does not trap so its caller can provide operation-specific
///          diagnostics.
/// @param data Pointer to the candidate leading byte; may be NULL.
/// @param remaining Number of readable bytes beginning at @p data.
/// @return Valid sequence length from one through four, or zero if invalid.
size_t rt_utf8_strict_step(const char *data, size_t remaining) {
    if (!data || remaining == 0)
        return 0;
    unsigned char lead = (unsigned char)data[0];
    if (lead < 0x80)
        return 1;
    size_t extra;
    uint32_t cp;
    if (lead >= 0xC2 && lead <= 0xDF) {
        extra = 1;
        cp = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        extra = 2;
        cp = lead & 0x0Fu;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        extra = 3;
        cp = lead & 0x07u;
    } else {
        return 0; // 0x80..0xC1 (continuation/overlong lead) or 0xF5..0xFF
    }
    if (remaining - 1 < extra)
        return 0;
    for (size_t k = 1; k <= extra; k++) {
        unsigned char ch = (unsigned char)data[k];
        if ((ch & 0xC0u) != 0x80u)
            return 0;
        cp = (cp << 6) | (uint32_t)(ch & 0x3Fu);
    }
    if ((extra == 2 && cp < 0x800u) || (extra == 3 && cp < 0x10000u) ||
        (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu)
        return 0;
    return extra + 1;
}

/// @brief Convert a one-based UTF-8 codepoint position to a byte offset.
/// @details Strictly validates every traversed codepoint. Null data or
///          positions at/below one map to byte zero. A position beyond the
///          available codepoints maps to @p byte_len. Invalid UTF-8 traps and
///          returns @p byte_len if the trap handler returns.
/// @param data Borrowed byte span containing UTF-8 data; may be NULL.
/// @param byte_len Number of available bytes.
/// @param char_pos One-based codepoint position.
/// @return Zero-based byte offset, clamped to @p byte_len.
size_t utf8_char_to_byte_offset(const char *data, size_t byte_len, int64_t char_pos) {
    if (!data || char_pos <= 1)
        return 0;
    size_t byte_off = 0;
    int64_t cp = 1;
    while (byte_off < byte_len && cp < char_pos) {
        // Strict decoding (VDOC-166): overlong encodings, surrogates, and
        // out-of-range scalars are malformed, not one-byte "characters".
        size_t clen = rt_utf8_strict_step(data + byte_off, byte_len - byte_off);
        if (clen == 0) {
            rt_trap("String: invalid UTF-8 sequence");
            return byte_len;
        }
        byte_off += clen;
        cp++;
    }
    return byte_off;
}

/// @brief Infer an expected UTF-8 sequence width from one leading byte.
/// @details This compatibility helper examines only the lead-bit pattern; it
///          does not validate continuation bytes, overlong forms, surrogates,
///          or the Unicode maximum. An invalid leading pattern traps.
/// @param c Candidate leading byte.
/// @return Expected width from one through four, or zero after a returning
///         invalid-lead trap.
size_t utf8_char_len(unsigned char c) {
    if ((c & 0x80) == 0)
        return 1; // ASCII: 0xxxxxxx
    if ((c & 0xE0) == 0xC0)
        return 2; // 110xxxxx
    if ((c & 0xF0) == 0xE0)
        return 3; // 1110xxxx
    if ((c & 0xF8) == 0xF0)
        return 4; // 11110xxx
    rt_trap("String: invalid UTF-8 lead byte");
    return 0;
}

/// @brief Reverse @p str by strict UTF-8 codepoint.
/// @details Validates the complete input, records every codepoint boundary,
///          then copies the original byte sequences in reverse order without
///          changing bytes within a sequence. Null/empty input returns the
///          empty singleton. Malformed UTF-8, offset-array overflow, or
///          allocation failure traps.
/// @param str Borrowed UTF-8 source string; may be NULL.
/// @return Owned reversed string or empty singleton, or `NULL` after failure.
rt_string rt_str_flip(rt_string str) {
    if (!rt_string_arg_valid_(str, "String.Flip: invalid source"))
        return NULL;
    if (!str)
        return rt_empty_string();

    size_t len = rt_string_len_bytes(str);
    if (len == 0)
        return rt_empty_string();
    if (len == SIZE_MAX) {
        rt_trap("String.Flip: string too large");
        return NULL;
    }

    const char *data = str->data;

    // First pass: count characters and find their start positions
    size_t char_count = 0;
    for (size_t i = 0; i < len;) {
        size_t clen = rt_utf8_strict_step(data + i, len - i);
        if (clen == 0) {
            rt_trap("String.Flip: invalid UTF-8 sequence");
            return NULL;
        }
        i += clen;
        char_count++;
    }

    // Allocate positions array (offsets of each character start)
    if (char_count > (SIZE_MAX / sizeof(size_t)) - 1) {
        rt_trap("String.Flip: string too large");
        return NULL;
    }
    size_t *positions = (size_t *)malloc((char_count + 1) * sizeof(size_t));
    if (!positions) {
        rt_trap("String.Flip: allocation failed");
        return NULL;
    }

    // Second pass: record character positions
    size_t idx = 0;
    for (size_t i = 0; i < len;) {
        positions[idx++] = i;
        size_t clen = rt_utf8_strict_step(data + i, len - i);
        if (clen == 0) {
            free(positions);
            rt_trap("String.Flip: invalid UTF-8 sequence");
            return NULL;
        }
        i += clen;
    }
    positions[char_count] = len; // End sentinel

    // Allocate result buffer
    rt_string result = rt_string_alloc(len, len + 1);
    if (!result) {
        free(positions);
        return NULL;
    }

    // Build reversed string by copying characters in reverse order
    size_t dest = 0;
    for (size_t i = char_count; i > 0; i--) {
        size_t start = positions[i - 1];
        size_t end = positions[i];
        size_t clen = end - start;
        memcpy(result->data + dest, data + start, clen);
        dest += clen;
    }
    result->data[len] = '\0';

    free(positions);
    return result;
}

/// @brief Compare two strings using unsigned-byte lexicographic order.
/// @details Null sorts before every non-null string, and two null handles are
///          equal. Equal prefixes are ordered by stored byte length.
/// @param a Borrowed first string; may be NULL.
/// @param b Borrowed second string; may be NULL.
/// @return Exactly -1, 0, or 1 according to the total ordering.
int64_t rt_str_cmp(rt_string a, rt_string b) {
    if (!rt_string_arg_valid_(a, "String.Compare: invalid left operand") ||
        !rt_string_arg_valid_(b, "String.Compare: invalid right operand")) {
        return 0;
    }
    if (!a && !b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;

    size_t alen = rt_string_len_bytes(a);
    size_t blen = rt_string_len_bytes(b);
    size_t minlen = alen < blen ? alen : blen;

    int result = memcmp(a->data, b->data, minlen);
    if (result != 0)
        return (result > 0) - (result < 0);

    if (alen < blen)
        return -1;
    if (alen > blen)
        return 1;
    return 0;
}

/// @brief Compare two strings after ASCII-only case folding.
/// @details Lowercases `A-Z` byte-by-byte, leaves all other bytes unchanged,
///          and otherwise uses the null and length ordering of
///          @ref rt_str_cmp.
/// @param a Borrowed first string; may be NULL.
/// @param b Borrowed second string; may be NULL.
/// @return Exactly -1, 0, or 1 according to ASCII-folded byte ordering.
int64_t rt_str_cmp_nocase(rt_string a, rt_string b) {
    if (!rt_string_arg_valid_(a, "String.CompareNoCase: invalid left operand") ||
        !rt_string_arg_valid_(b, "String.CompareNoCase: invalid right operand")) {
        return 0;
    }
    if (!a && !b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;

    size_t alen = rt_string_len_bytes(a);
    size_t blen = rt_string_len_bytes(b);
    size_t minlen = alen < blen ? alen : blen;

    for (size_t i = 0; i < minlen; i++) {
        unsigned char ca = (unsigned char)rt_ascii_tolower((unsigned char)a->data[i]);
        unsigned char cb = (unsigned char)rt_ascii_tolower((unsigned char)b->data[i]);
        if (ca < cb)
            return -1;
        if (ca > cb)
            return 1;
    }

    if (alen < blen)
        return -1;
    if (alen > blen)
        return 1;
    return 0;
}

/// @brief Validate an entire byte span as strict UTF-8.
/// @details Applies the same scalar constraints as @ref rt_utf8_strict_step to
///          every codepoint. A null pointer is valid only for an empty span.
///          This predicate never traps.
/// @param data Borrowed byte span; may be NULL only when @p len is zero.
/// @param len Number of bytes to validate.
/// @return One when the whole span is valid UTF-8; otherwise zero.
int rt_utf8_span_valid(const char *data, size_t len) {
    if (!data)
        return len == 0;
    size_t i = 0;
    while (i < len) {
        unsigned char lead = (unsigned char)data[i];
        if (lead < 0x80) {
            i++;
            continue;
        }
        size_t extra;
        uint32_t cp;
        if (lead >= 0xC2 && lead <= 0xDF) {
            extra = 1;
            cp = lead & 0x1Fu;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            extra = 2;
            cp = lead & 0x0Fu;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            extra = 3;
            cp = lead & 0x07u;
        } else {
            return 0; // 0x80-0xC1 (bare continuation / overlong lead) or 0xF5+.
        }
        if (len - i - 1 < extra)
            return 0;
        for (size_t k = 1; k <= extra; k++) {
            unsigned char ch = (unsigned char)data[i + k];
            if ((ch & 0xC0u) != 0x80u)
                return 0;
            cp = (cp << 6) | (uint32_t)(ch & 0x3Fu);
        }
        if ((extra == 2 && cp < 0x800u) || (extra == 3 && cp < 0x10000u))
            return 0;
        if (cp >= 0xD800u && cp <= 0xDFFFu)
            return 0;
        if (cp > 0x10FFFFu)
            return 0;
        i += extra + 1;
    }
    return 1;
}
