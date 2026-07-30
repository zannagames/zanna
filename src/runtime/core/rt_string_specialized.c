//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_string_specialized.c
// Purpose: Implements specialized byte-string transforms, similarity/distance
// metrics, identifier naming conversions, and SQL LIKE-style matching.
//
// Key invariants:
//   - Identifier-style case conversions use a shared split_words() helper that
//     handles explicit separators, lower-to-upper boundaries, and acronym
//     boundaries. Casing is ASCII-only and locale-independent.
//   - Levenshtein and Hamming return byte distances; Jaro and Jaro-Winkler
//     return double similarity scores in the range 0.0-1.0.
//   - LIKE literal comparison is byte-wise, while `_` and `%` backtracking move
//     over strictly validated UTF-8 codepoints.
//   - Null is treated as empty by these specialized APIs unless a declaration
//     documents another sentinel/failure result.
//
// Ownership/Lifetime:
//   - String-returning operations allocate a fresh owned result, including for
//     null/empty inputs; they do not retain or mutate their sources.
//   - Temporary buffers (word arrays, match maps, and DP rows) are released on
//     every ordinary success/failure path.
//   - Distance and LIKE operations borrow inputs and allocate no returned state.
//
// Links: src/runtime/core/rt_string_internal.h (shared helpers),
//        src/runtime/core/rt_string_ops.c (core operations),
//        src/runtime/core/rt_string.h (public API)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Specialized runtime string transforms, metrics, and LIKE matching.

#include "rt_ascii.h"
#include "rt_internal.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_string_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Extended String Utilities
//=============================================================================

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

/// @brief Uppercase the first byte of a newly copied string.
/// @details Applies ASCII-only `toupper` to byte zero and leaves every remaining
///          byte unchanged. Null/empty input still creates an ordinary owned
///          empty string rather than returning a shared singleton.
/// @param str Borrowed source string; may be NULL.
/// @return Newly allocated owned copy, or `NULL` on allocation failure.
rt_string rt_str_capitalize(rt_string str) {
    if (!rt_string_arg_valid_(str, "String.Capitalize: invalid source"))
        return NULL;
    if (!str)
        return rt_string_from_bytes("", 0);
    size_t len = rt_string_len_bytes(str);
    if (len == 0)
        return rt_string_from_bytes("", 0);

    rt_string result = rt_string_alloc(len, len + 1);
    if (!result)
        return NULL;
    memcpy(result->data, str->data, len);
    result->data[len] = '\0';
    result->data[0] = (char)rt_ascii_toupper((unsigned char)result->data[0]);
    return result;
}

/// @brief Uppercase the first byte of each ASCII-whitespace-delimited word.
/// @details Copies the complete byte string, uppercases only a byte at the
///          beginning or immediately after ASCII whitespace, and does not
///          lowercase any other byte. Null/empty input produces a fresh empty
///          string.
/// @param str Borrowed source string; may be NULL.
/// @return Newly allocated owned copy, or `NULL` on allocation failure.
rt_string rt_str_title(rt_string str) {
    if (!rt_string_arg_valid_(str, "String.Title: invalid source"))
        return NULL;
    if (!str)
        return rt_string_from_bytes("", 0);
    size_t len = rt_string_len_bytes(str);
    if (len == 0)
        return rt_string_from_bytes("", 0);

    rt_string result = rt_string_alloc(len, len + 1);
    if (!result)
        return NULL;
    memcpy(result->data, str->data, len);
    result->data[len] = '\0';

    int capitalize_next = 1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)result->data[i];
        if (rt_ascii_isspace(c)) {
            capitalize_next = 1;
        } else if (capitalize_next) {
            result->data[i] = (char)rt_ascii_toupper(c);
            capitalize_next = 0;
        }
    }
    return result;
}

/// @brief Remove one matching byte prefix from @p str.
/// @details Always allocates a copy. Null source becomes a fresh empty string;
///          null, empty, overlong, or nonmatching prefix copies the complete
///          source.
/// @param str Borrowed source string; may be NULL.
/// @param prefix Borrowed prefix byte string; may be NULL.
/// @return Newly allocated owned result, or `NULL` on allocation failure.
rt_string rt_str_remove_prefix(rt_string str, rt_string prefix) {
    if (!rt_string_arg_valid_(str, "String.RemovePrefix: invalid source") ||
        !rt_string_arg_valid_(prefix, "String.RemovePrefix: invalid prefix")) {
        return NULL;
    }
    if (!str)
        return rt_string_from_bytes("", 0);
    size_t slen = rt_string_len_bytes(str);
    if (!prefix)
        return rt_string_from_bytes(str->data, slen);

    size_t plen = rt_string_len_bytes(prefix);
    if (plen == 0 || plen > slen)
        return rt_string_from_bytes(str->data, slen);

    if (memcmp(str->data, prefix->data, plen) == 0)
        return rt_string_from_bytes(str->data + plen, slen - plen);

    return rt_string_from_bytes(str->data, slen);
}

/// @brief Remove one matching byte suffix from @p str.
/// @details Always allocates a copy and otherwise follows the null, empty,
///          overlong, and nonmatching rules of @ref rt_str_remove_prefix.
/// @param str Borrowed source string; may be NULL.
/// @param suffix Borrowed suffix byte string; may be NULL.
/// @return Newly allocated owned result, or `NULL` on allocation failure.
rt_string rt_str_remove_suffix(rt_string str, rt_string suffix) {
    if (!rt_string_arg_valid_(str, "String.RemoveSuffix: invalid source") ||
        !rt_string_arg_valid_(suffix, "String.RemoveSuffix: invalid suffix")) {
        return NULL;
    }
    if (!str)
        return rt_string_from_bytes("", 0);
    size_t slen = rt_string_len_bytes(str);
    if (!suffix)
        return rt_string_from_bytes(str->data, slen);

    size_t xlen = rt_string_len_bytes(suffix);
    if (xlen == 0 || xlen > slen)
        return rt_string_from_bytes(str->data, slen);

    if (memcmp(str->data + slen - xlen, suffix->data, xlen) == 0)
        return rt_string_from_bytes(str->data, slen - xlen);

    return rt_string_from_bytes(str->data, slen);
}

/// @brief Find the last occurrence of @p needle in @p haystack.
/// @details Compares stored bytes while scanning candidate starts backward.
///          An empty needle matches at `Length + 1`; null operands and an
///          overlong/missing needle return zero.
/// @param haystack Borrowed source string; may be NULL.
/// @param needle Borrowed byte sequence; may be NULL.
/// @return One-based byte index of the final match, or zero when absent.
int64_t rt_str_last_index_of(rt_string haystack, rt_string needle) {
    if (!rt_string_arg_valid_(haystack, "String.LastIndexOf: invalid source") ||
        !rt_string_arg_valid_(needle, "String.LastIndexOf: invalid needle")) {
        return 0;
    }
    if (!haystack || !needle)
        return 0;
    size_t hlen = rt_string_len_bytes(haystack);
    size_t nlen = rt_string_len_bytes(needle);
    if (nlen == 0)
        return (int64_t)hlen + 1;
    if (nlen > hlen)
        return 0;

    for (size_t i = hlen - nlen + 1; i > 0; i--) {
        if (memcmp(haystack->data + i - 1, needle->data, nlen) == 0)
            return (int64_t)i; // 1-based
    }
    return 0;
}

/// @brief Remove repeated occurrences of one byte from both ends of @p str.
/// @details Uses only the first stored byte of @p ch, even if it begins a
///          multibyte UTF-8 sequence. Null source becomes a fresh empty string;
///          null/empty @p ch copies the complete source.
/// @param str Borrowed source string; may be NULL.
/// @param ch Borrowed string supplying the trim byte; may be NULL.
/// @return Newly allocated owned trimmed copy, or `NULL` on allocation failure.
rt_string rt_str_trim_char(rt_string str, rt_string ch) {
    if (!rt_string_arg_valid_(str, "String.TrimChar: invalid source") ||
        !rt_string_arg_valid_(ch, "String.TrimChar: invalid character")) {
        return NULL;
    }
    if (!str)
        return rt_string_from_bytes("", 0);
    size_t len = rt_string_len_bytes(str);
    if (len == 0 || !ch)
        return rt_string_from_bytes(str->data, len);

    size_t chlen = rt_string_len_bytes(ch);
    if (chlen == 0)
        return rt_string_from_bytes(str->data, len);

    char trim_ch = ch->data[0];

    size_t start = 0;
    while (start < len && str->data[start] == trim_ch)
        start++;

    size_t end = len;
    while (end > start && str->data[end - 1] == trim_ch)
        end--;

    return rt_string_from_bytes(str->data + start, end - start);
}

/// @brief Convert @p str to a lowercase ASCII slug.
/// @details Preserves ASCII alphanumerics while lowercasing letters and
///          collapses each run of every other byte, including non-ASCII bytes,
///          to one hyphen. Leading/trailing separators are omitted. Null/empty
///          input produces a fresh empty string.
/// @param str Borrowed source string; may be NULL.
/// @return Newly allocated owned slug, or `NULL` after allocation failure.
rt_string rt_str_slug(rt_string str) {
    if (!rt_string_arg_valid_(str, "String.Slug: invalid source"))
        return NULL;
    if (!str)
        return rt_string_from_bytes("", 0);
    size_t len = rt_string_len_bytes(str);
    if (len == 0)
        return rt_string_from_bytes("", 0);
    if (len == SIZE_MAX) {
        rt_trap("String.Slug: input too large");
        return rt_string_from_bytes("", 0);
    }

    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        rt_trap("String.Slug: allocation failed");
        return NULL;
    }

    size_t out = 0;
    int last_was_sep = 1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str->data[i];
        if (rt_ascii_isalnum(c)) {
            buf[out++] = (char)rt_ascii_tolower(c);
            last_was_sep = 0;
        } else if (!last_was_sep) {
            buf[out++] = '-';
            last_was_sep = 1;
        }
    }
    if (out > 0 && buf[out - 1] == '-')
        out--;

    rt_string result = rt_string_from_bytes(buf, out);
    free(buf);
    return result;
}

// ---------------------------------------------------------------------------
// String Similarity / Distance
// ---------------------------------------------------------------------------

/// @brief Compute byte-wise Levenshtein edit distance.
/// @details Treats null as empty and assigns unit cost to byte insertion,
///          deletion, and substitution. A rolling row gives O(m*n) time and
///          O(min(m,n)) temporary space.
/// @param a Borrowed first byte string; may be NULL.
/// @param b Borrowed second byte string; may be NULL.
/// @return Edit distance, or -1 for lengths above `INT64_MAX`, temporary-size
///         overflow, or allocation failure.
int64_t rt_str_levenshtein(rt_string a, rt_string b) {
    if (!rt_string_arg_valid_(a, "String.Levenshtein: invalid left operand") ||
        !rt_string_arg_valid_(b, "String.Levenshtein: invalid right operand")) {
        return -1;
    }
    if (!a && !b)
        return 0;
    size_t alen = a ? rt_string_len_bytes(a) : 0;
    size_t blen = b ? rt_string_len_bytes(b) : 0;
    if (alen > (size_t)INT64_MAX || blen > (size_t)INT64_MAX)
        return -1;
    if (alen == 0)
        return (int64_t)blen;
    if (blen == 0)
        return (int64_t)alen;

    const char *sa = rt_string_cstr(a);
    const char *sb = rt_string_cstr(b);

    // Use single-row DP to save memory: O(min(m,n)) space
    // Ensure blen is the smaller dimension
    if (alen < blen) {
        const char *tmp_s = sa;
        sa = sb;
        sb = tmp_s;
        size_t tmp_n = alen;
        alen = blen;
        blen = tmp_n;
    }

    if (blen > (SIZE_MAX / sizeof(size_t)) - 1)
        return -1;
    size_t *row = (size_t *)malloc((blen + 1) * sizeof(size_t));
    if (!row)
        return -1;

    for (size_t j = 0; j <= blen; ++j)
        row[j] = j;

    for (size_t i = 1; i <= alen; ++i) {
        size_t prev = row[0];
        row[0] = i;
        for (size_t j = 1; j <= blen; ++j) {
            size_t cost = (sa[i - 1] == sb[j - 1]) ? 0 : 1;
            size_t del = row[j] + 1;
            size_t ins = row[j - 1] + 1;
            size_t sub = prev + cost;

            size_t min = del < ins ? del : ins;
            if (sub < min)
                min = sub;

            prev = row[j];
            row[j] = min;
        }
    }

    int64_t result = (int64_t)row[blen];
    free(row);
    return result;
}

/// @brief Compute byte-wise Jaro similarity.
/// @details Uses a match window of `max(lengths)/2 - 1`, clamped to zero,
///          followed by half-transposition scoring. Null is empty and two empty
///          inputs score one.
/// @param a Borrowed first byte string; may be NULL.
/// @param b Borrowed second byte string; may be NULL.
/// @return Similarity in `[0.0, 1.0]`; zero also represents unsupported huge
///         lengths or match-map allocation failure.
double rt_str_jaro(rt_string a, rt_string b) {
    if (!rt_string_arg_valid_(a, "String.Jaro: invalid left operand") ||
        !rt_string_arg_valid_(b, "String.Jaro: invalid right operand")) {
        return 0.0;
    }
    if (!a && !b)
        return 1.0;
    size_t alen = a ? rt_string_len_bytes(a) : 0;
    size_t blen = b ? rt_string_len_bytes(b) : 0;
    if (alen > (size_t)INT64_MAX || blen > (size_t)INT64_MAX)
        return 0.0;
    if (alen == 0 && blen == 0)
        return 1.0;
    if (alen == 0 || blen == 0)
        return 0.0;

    const char *sa = rt_string_cstr(a);
    const char *sb = rt_string_cstr(b);

    size_t max_len = alen > blen ? alen : blen;
    size_t match_dist = (max_len / 2) > 0 ? (max_len / 2) - 1 : 0;

    int8_t *a_matched = (int8_t *)calloc(alen, sizeof(int8_t));
    int8_t *b_matched = (int8_t *)calloc(blen, sizeof(int8_t));
    if (!a_matched || !b_matched) {
        free(a_matched);
        free(b_matched);
        return 0.0;
    }

    double matches = 0;
    double transpositions = 0;

    for (size_t i = 0; i < alen; ++i) {
        size_t start = (i > match_dist) ? i - match_dist : 0;
        size_t end = i + match_dist + 1;
        if (end > blen)
            end = blen;

        for (size_t j = start; j < end; ++j) {
            if (b_matched[j] || sa[i] != sb[j])
                continue;
            a_matched[i] = 1;
            b_matched[j] = 1;
            matches++;
            break;
        }
    }

    if (matches == 0.0) {
        free(a_matched);
        free(b_matched);
        return 0.0;
    }

    // Count transpositions
    size_t k = 0;
    for (size_t i = 0; i < alen; ++i) {
        if (!a_matched[i])
            continue;
        while (!b_matched[k])
            ++k;
        if (sa[i] != sb[k])
            transpositions++;
        ++k;
    }

    free(a_matched);
    free(b_matched);

    return (matches / (double)alen + matches / (double)blen +
            (matches - transpositions / 2.0) / matches) /
           3.0;
}

/// @brief Compute byte-wise Jaro-Winkler similarity.
/// @details Adds `prefix * 0.1 * (1 - jaro)` for up to four equal leading
///          bytes. The bonus is applied for every base score rather than only
///          above a separate similarity threshold. Null is treated as empty.
/// @param a Borrowed first byte string; may be NULL.
/// @param b Borrowed second byte string; may be NULL.
/// @return Jaro score adjusted by the common-prefix bonus.
double rt_str_jaro_winkler(rt_string a, rt_string b) {
    if (!rt_string_arg_valid_(a, "String.JaroWinkler: invalid left operand") ||
        !rt_string_arg_valid_(b, "String.JaroWinkler: invalid right operand")) {
        return 0.0;
    }
    double jaro = rt_str_jaro(a, b);

    // Compute common prefix length (up to 4)
    size_t alen = a ? rt_string_len_bytes(a) : 0;
    size_t blen = b ? rt_string_len_bytes(b) : 0;
    size_t max_prefix = 4;
    if (alen < max_prefix)
        max_prefix = alen;
    if (blen < max_prefix)
        max_prefix = blen;

    const char *sa = a ? rt_string_cstr(a) : "";
    const char *sb = b ? rt_string_cstr(b) : "";

    size_t prefix = 0;
    for (size_t i = 0; i < max_prefix; ++i) {
        if (sa[i] == sb[i])
            prefix++;
        else
            break;
    }

    double p = 0.1; // Winkler scaling factor
    return jaro + (double)prefix * p * (1.0 - jaro);
}

/// @brief Compute Hamming distance between equal-length byte strings.
/// @details Treats null as empty and compares every stored byte, including
///          embedded NUL.
/// @param a Borrowed first byte string; may be NULL.
/// @param b Borrowed second byte string; may be NULL.
/// @return Number of differing positions, or -1 when lengths differ.
int64_t rt_str_hamming(rt_string a, rt_string b) {
    if (!rt_string_arg_valid_(a, "String.Hamming: invalid left operand") ||
        !rt_string_arg_valid_(b, "String.Hamming: invalid right operand")) {
        return -1;
    }
    size_t alen = a ? rt_string_len_bytes(a) : 0;
    size_t blen = b ? rt_string_len_bytes(b) : 0;
    if (alen != blen)
        return -1;
    if (alen == 0)
        return 0;

    const char *sa = rt_string_cstr(a);
    const char *sb = rt_string_cstr(b);
    int64_t dist = 0;
    for (size_t i = 0; i < alen; ++i) {
        if (sa[i] != sb[i])
            dist++;
    }
    return dist;
}

// ---------------------------------------------------------------------------
// Case conversion utilities
// ---------------------------------------------------------------------------

/// @brief Test whether @p c is one of the conventional word-separator characters.
/// @details Recognises space, underscore, hyphen, and tab. Used by camelCase
///          and snake_case splitter heuristics that need a uniform definition
///          of "boundary".
/// @param c Byte to classify.
/// @return Non-zero if @p c is a separator.
static int is_separator(char c) {
    return c == ' ' || c == '_' || c == '-' || c == '\t';
}

/// @brief Split @p src into words, recognising both explicit separators and camelCase.
/// @details Copies each detected word into @p buf with NUL terminators packed
///          end-to-end, and writes a pointer to each word into @p words.
///          Boundaries are space/tab/underscore/hyphen, ASCII lower-to-upper
///          transitions, or the final uppercase byte before an acronym-to-word
///          transition such as `HTTPServer`. Embedded NUL is copied and exact
///          lengths avoid later `strlen` truncation. The routine stops at
///          @p max_words and copies only while @p buf_cap permits.
/// @param src Borrowed input byte span.
/// @param len Number of input bytes.
/// @param buf Writable packed-word storage.
/// @param buf_cap Total capacity of @p buf.
/// @param words Output array receiving pointers into @p buf.
/// @param word_lens Optional output array receiving exact byte lengths.
/// @param max_words Capacity of @p words and @p word_lens.
/// @return Number of word entries emitted, not exceeding @p max_words.
static int split_words(const char *src,
                       size_t len,
                       char *buf,
                       size_t buf_cap,
                       const char **words,
                       size_t *word_lens,
                       int max_words) {
    int wcount = 0;
    size_t bpos = 0;

    size_t i = 0;
    while (i < len && wcount < max_words) {
        // Skip separators
        while (i < len && is_separator(src[i]))
            ++i;
        if (i >= len)
            break;

        // Start of a word
        words[wcount] = buf + bpos;
        size_t word_start = bpos;

        // Collect word characters
        while (i < len && !is_separator(src[i])) {
            // Detect camelCase boundary: lowercase followed by uppercase
            if (i + 1 < len && rt_ascii_islower((unsigned char)src[i]) &&
                rt_ascii_isupper((unsigned char)src[i + 1])) {
                if (bpos < buf_cap)
                    buf[bpos++] = src[i];
                ++i;
                break; // End this word, next word starts with uppercase
            }
            // Detect ACRONYM boundary: multiple uppercase followed by lowercase
            if (i + 2 < len && rt_ascii_isupper((unsigned char)src[i]) &&
                rt_ascii_isupper((unsigned char)src[i + 1]) &&
                rt_ascii_islower((unsigned char)src[i + 2])) {
                if (bpos < buf_cap)
                    buf[bpos++] = src[i];
                ++i;
                break;
            }
            if (bpos < buf_cap)
                buf[bpos++] = src[i];
            ++i;
        }
        // Record the exact byte length: the copied bytes may themselves
        // contain NUL, which strlen would silently truncate (VDOC-165).
        if (word_lens)
            word_lens[wcount] = bpos - word_start;
        if (bpos < buf_cap)
            buf[bpos++] = '\0';
        ++wcount;
    }

    return wcount;
}

/// @brief Append bytes to a casing builder and trap on failure.
/// @details The case-conversion helpers build their result through
///          @ref rt_string_builder. Any non-success status raises @p context
///          (or a generic fallback), leaving builder/scratch cleanup to the
///          caller if the trap hook returns.
/// @param sb Initialized builder receiving bytes.
/// @param bytes Borrowed source span; may be NULL only when @p len is zero.
/// @param len Number of bytes to append.
/// @param context Borrowed diagnostic message; may be NULL.
/// @return One on success, or zero after a returning append-failure trap.
static int append_case_bytes(rt_string_builder *sb,
                             const char *bytes,
                             size_t len,
                             const char *context) {
    rt_sb_status_t status = rt_sb_append_bytes(sb, bytes, len);
    if (status == RT_SB_OK)
        return 1;
    rt_trap(context ? context : "string case conversion: append failed");
    return 0;
}

/// @brief Append a single byte to a casing builder.
/// @details Thin checked wrapper around @ref append_case_bytes.
/// @param sb Initialized builder receiving the byte.
/// @param ch Byte value to append.
/// @param context Borrowed diagnostic message; may be NULL.
/// @return One on success, or zero after a returning append-failure trap.
static int append_case_char(rt_string_builder *sb, char ch, const char *context) {
    return append_case_bytes(sb, &ch, 1, context);
}

/// @brief Return the length of the next UTF-8 codepoint at @p data.
/// @details Used by SQL LIKE `_` wildcard handling so `_` consumes one
///          strict UTF-8 codepoint rather than one raw byte. Invalid,
///          noncanonical, surrogate, out-of-range, or truncated sequences trap.
/// @param data Borrowed span beginning at the current byte.
/// @param remaining Number of bytes available from @p data.
/// @return Width from one through four, or zero after a returning invalid-input
///         trap.
static size_t like_utf8_step(const char *data, size_t remaining) {
    size_t step = rt_utf8_strict_step(data, remaining);
    if (step == 0) {
        rt_trap("String.Like: invalid UTF-8 sequence");
        return 0;
    }
    return step;
}

/// @brief Allocate scratch storage and split an arbitrary byte span into words.
/// @details Allocates a packed buffer large enough for every input byte and a
///          terminator per possible word, plus pointer and exact-length arrays.
///          Output pointers are cleared before validation. Input length is
///          limited to `INT_MAX` because @ref split_words uses an integer word
///          limit. Allocation/size failure traps and leaves outputs null.
/// @param src Borrowed input byte span.
/// @param len Number of input bytes.
/// @param buf_out Required output receiving owned packed byte storage.
/// @param words_out Required output receiving an owned pointer array whose
///        entries refer into `*buf_out`.
/// @param word_lens_out Optional output receiving an owned exact-length array.
/// @return Number of words, or zero for no words or after a returning trap.
static int split_words_dynamic(
    const char *src, size_t len, char **buf_out, const char ***words_out, size_t **word_lens_out) {
    if (buf_out)
        *buf_out = NULL;
    if (words_out)
        *words_out = NULL;
    if (word_lens_out)
        *word_lens_out = NULL;
    if (len > (size_t)INT_MAX) {
        rt_trap("string_ops: input too large");
        return 0;
    }

    size_t word_cap = len > 0 ? len : 1;
    if (len > SIZE_MAX - word_cap - 1) {
        rt_trap("string_ops: input too large");
        return 0;
    }
    size_t buf_cap = len + word_cap + 1;

    char *wbuf = (char *)malloc(buf_cap);
    if (!wbuf) {
        rt_trap("string_ops: memory allocation failed");
        return 0;
    }

    const char **words = (const char **)malloc(word_cap * sizeof(*words));
    if (!words) {
        free(wbuf);
        rt_trap("string_ops: memory allocation failed");
        return 0;
    }

    size_t *word_lens = (size_t *)malloc(word_cap * sizeof(*word_lens));
    if (!word_lens) {
        free(words);
        free(wbuf);
        rt_trap("string_ops: memory allocation failed");
        return 0;
    }

    int wc = split_words(src, len, wbuf, buf_cap, words, word_lens, (int)word_cap);
    *buf_out = wbuf;
    *words_out = words;
    if (word_lens_out)
        *word_lens_out = word_lens;
    else
        free(word_lens);
    return wc;
}

/// @brief Convert @p str to ASCII-folded camelCase.
/// @details Uses the shared explicit/case-boundary splitter, lowercases every
///          ASCII byte, then uppercases the first byte of each word after the
///          first. Separators are dropped; high-bit and embedded-NUL bytes are
///          preserved. Null/empty input produces a fresh empty string.
/// @param str Borrowed source string; may be NULL.
/// @return Newly allocated owned result, or `NULL` after scratch, builder, or
///         result allocation failure.
rt_string rt_str_camel_case(rt_string str) {
    if (!rt_string_arg_valid_(str, "String.CamelCase: invalid source"))
        return NULL;
    if (!str)
        return rt_string_from_bytes("", 0);
    size_t len = rt_string_len_bytes(str);
    if (len == 0)
        return rt_string_from_bytes("", 0);
    const char *src = str->data;

    char *wbuf = NULL;
    const char **words = NULL;
    size_t *word_lens = NULL;
    int wc = split_words_dynamic(src, len, &wbuf, &words, &word_lens);
    if (!wbuf || !words)
        return NULL;

    rt_string_builder sb;
    rt_sb_init(&sb);

    for (int w = 0; w < wc; ++w) {
        const char *word = words[w];
        size_t wlen = word_lens[w];
        if (wlen == 0)
            continue;

        char first = (w == 0) ? (char)rt_ascii_tolower((unsigned char)word[0])
                              : (char)rt_ascii_toupper((unsigned char)word[0]);
        if (!append_case_char(&sb, first, "String.CamelCase: append failed"))
            goto camel_fail;
        for (size_t j = 1; j < wlen; ++j) {
            char c = (char)rt_ascii_tolower((unsigned char)word[j]);
            if (!append_case_char(&sb, c, "String.CamelCase: append failed"))
                goto camel_fail;
        }
    }

    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    free(word_lens);
    free(words);
    free(wbuf);
    return result;

camel_fail:
    rt_sb_free(&sb);
    free(word_lens);
    free(words);
    free(wbuf);
    return NULL;
}

/// @brief Convert @p str to ASCII-folded PascalCase.
/// @details Uses the shared word splitter, lowercases ASCII bytes, uppercases
///          the first byte of every word, and drops separators. Null/empty
///          input produces a fresh empty string.
/// @param str Borrowed source string; may be NULL.
/// @return Newly allocated owned result, or `NULL` on failure.
rt_string rt_str_pascal_case(rt_string str) {
    if (!rt_string_arg_valid_(str, "String.PascalCase: invalid source"))
        return NULL;
    if (!str)
        return rt_string_from_bytes("", 0);
    size_t len = rt_string_len_bytes(str);
    if (len == 0)
        return rt_string_from_bytes("", 0);
    const char *src = str->data;

    char *wbuf = NULL;
    const char **words = NULL;
    size_t *word_lens = NULL;
    int wc = split_words_dynamic(src, len, &wbuf, &words, &word_lens);
    if (!wbuf || !words)
        return NULL;

    rt_string_builder sb;
    rt_sb_init(&sb);

    for (int w = 0; w < wc; ++w) {
        const char *word = words[w];
        size_t wlen = word_lens[w];
        if (wlen == 0)
            continue;

        char first = (char)rt_ascii_toupper((unsigned char)word[0]);
        if (!append_case_char(&sb, first, "String.PascalCase: append failed"))
            goto pascal_fail;
        for (size_t j = 1; j < wlen; ++j) {
            char c = (char)rt_ascii_tolower((unsigned char)word[j]);
            if (!append_case_char(&sb, c, "String.PascalCase: append failed"))
                goto pascal_fail;
        }
    }

    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    free(word_lens);
    free(words);
    free(wbuf);
    return result;

pascal_fail:
    rt_sb_free(&sb);
    free(word_lens);
    free(words);
    free(wbuf);
    return NULL;
}

/// @brief Convert @p str to ASCII-folded snake_case.
/// @details Uses shared word boundaries, lowercases ASCII bytes, and joins
///          emitted words with one underscore. Null/empty input produces a
///          fresh empty string.
/// @param str Borrowed source string; may be NULL.
/// @return Newly allocated owned result, or `NULL` on failure.
rt_string rt_str_snake_case(rt_string str) {
    if (!rt_string_arg_valid_(str, "String.SnakeCase: invalid source"))
        return NULL;
    if (!str)
        return rt_string_from_bytes("", 0);
    size_t len = rt_string_len_bytes(str);
    if (len == 0)
        return rt_string_from_bytes("", 0);
    const char *src = str->data;

    char *wbuf = NULL;
    const char **words = NULL;
    size_t *word_lens = NULL;
    int wc = split_words_dynamic(src, len, &wbuf, &words, &word_lens);
    if (!wbuf || !words)
        return NULL;

    rt_string_builder sb;
    rt_sb_init(&sb);

    for (int w = 0; w < wc; ++w) {
        if (w > 0 && !append_case_bytes(&sb, "_", 1, "String.SnakeCase: append failed"))
            goto snake_fail;
        const char *word = words[w];
        size_t wlen = word_lens[w];
        for (size_t j = 0; j < wlen; ++j) {
            char c = (char)rt_ascii_tolower((unsigned char)word[j]);
            if (!append_case_char(&sb, c, "String.SnakeCase: append failed"))
                goto snake_fail;
        }
    }

    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    free(word_lens);
    free(words);
    free(wbuf);
    return result;

snake_fail:
    rt_sb_free(&sb);
    free(word_lens);
    free(words);
    free(wbuf);
    return NULL;
}

/// @brief Convert @p str to ASCII-folded kebab-case.
/// @details Uses shared word boundaries, lowercases ASCII bytes, and joins
///          emitted words with one hyphen. Null/empty input produces a fresh
///          empty string.
/// @param str Borrowed source string; may be NULL.
/// @return Newly allocated owned result, or `NULL` on failure.
rt_string rt_str_kebab_case(rt_string str) {
    if (!rt_string_arg_valid_(str, "String.KebabCase: invalid source"))
        return NULL;
    if (!str)
        return rt_string_from_bytes("", 0);
    size_t len = rt_string_len_bytes(str);
    if (len == 0)
        return rt_string_from_bytes("", 0);
    const char *src = str->data;

    char *wbuf = NULL;
    const char **words = NULL;
    size_t *word_lens = NULL;
    int wc = split_words_dynamic(src, len, &wbuf, &words, &word_lens);
    if (!wbuf || !words)
        return NULL;

    rt_string_builder sb;
    rt_sb_init(&sb);

    for (int w = 0; w < wc; ++w) {
        if (w > 0 && !append_case_bytes(&sb, "-", 1, "String.KebabCase: append failed"))
            goto kebab_fail;
        const char *word = words[w];
        size_t wlen = word_lens[w];
        for (size_t j = 0; j < wlen; ++j) {
            char c = (char)rt_ascii_tolower((unsigned char)word[j]);
            if (!append_case_char(&sb, c, "String.KebabCase: append failed"))
                goto kebab_fail;
        }
    }

    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    free(word_lens);
    free(words);
    free(wbuf);
    return result;

kebab_fail:
    rt_sb_free(&sb);
    free(word_lens);
    free(words);
    free(wbuf);
    return NULL;
}

/// @brief Convert @p str to SCREAMING_SNAKE_CASE.
/// @details Uses shared word boundaries, uppercases ASCII bytes, and joins
///          emitted words with one underscore. Null/empty input produces a
///          fresh empty string.
/// @param str Borrowed source string; may be NULL.
/// @return Newly allocated owned result, or `NULL` on failure.
rt_string rt_str_screaming_snake(rt_string str) {
    if (!rt_string_arg_valid_(str, "String.ScreamingSnake: invalid source"))
        return NULL;
    if (!str)
        return rt_string_from_bytes("", 0);
    size_t len = rt_string_len_bytes(str);
    if (len == 0)
        return rt_string_from_bytes("", 0);
    const char *src = str->data;

    char *wbuf = NULL;
    const char **words = NULL;
    size_t *word_lens = NULL;
    int wc = split_words_dynamic(src, len, &wbuf, &words, &word_lens);
    if (!wbuf || !words)
        return NULL;

    rt_string_builder sb;
    rt_sb_init(&sb);

    for (int w = 0; w < wc; ++w) {
        if (w > 0 && !append_case_bytes(&sb, "_", 1, "String.ScreamingSnake: append failed"))
            goto screaming_fail;
        const char *word = words[w];
        size_t wlen = word_lens[w];
        for (size_t j = 0; j < wlen; ++j) {
            char c = (char)rt_ascii_toupper((unsigned char)word[j]);
            if (!append_case_char(&sb, c, "String.ScreamingSnake: append failed"))
                goto screaming_fail;
        }
    }

    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    free(word_lens);
    free(words);
    free(wbuf);
    return result;

screaming_fail:
    rt_sb_free(&sb);
    free(word_lens);
    free(words);
    free(wbuf);
    return NULL;
}

//=============================================================================
// SQL LIKE Pattern Matching
//=============================================================================

/// @brief Match length-counted text against the runtime SQL LIKE grammar.
/// @details `%` matches any sequence, `_` consumes one strict UTF-8 codepoint,
///          and backslash escapes the following pattern byte; a final backslash
///          is treated literally. Literal matching is byte-wise. The matcher
///          stores the most recent `%` and backtracks through text one strict
///          codepoint at a time. Case-insensitive mode folds ASCII only.
/// @param text Borrowed text span.
/// @param tlen Number of text bytes.
/// @param pat Borrowed pattern span.
/// @param plen Number of pattern bytes.
/// @param case_insensitive Nonzero to fold ASCII letters for literal matches.
/// @return One for a complete match; zero for mismatch or after a returning
///         malformed-UTF-8 trap.
static int8_t like_match(
    const char *text, size_t tlen, const char *pat, size_t plen, int case_insensitive) {
    size_t ti = 0, pi = 0;
    size_t star_pi = (size_t)-1, star_ti = 0;

    while (ti < tlen) {
        if (pi < plen && pat[pi] == '%') {
            // Wildcard: remember this position for backtracking
            star_pi = pi;
            star_ti = ti;
            pi++;
            continue;
        }

        if (pi < plen && pat[pi] == '\\' && pi + 1 < plen) {
            // Escaped character — match literally
            pi++;
            char tc = text[ti];
            char pc = pat[pi];
            if (case_insensitive) {
                tc = (char)rt_ascii_tolower((unsigned char)tc);
                pc = (char)rt_ascii_tolower((unsigned char)pc);
            }
            if (tc == pc) {
                ti++;
                pi++;
                continue;
            }
        } else if (pi < plen && pat[pi] == '_') {
            // Single UTF-8 codepoint wildcard
            size_t step = like_utf8_step(text + ti, tlen - ti);
            if (step == 0)
                return 0;
            ti += step;
            pi++;
            continue;
        } else if (pi < plen) {
            char tc = text[ti];
            char pc = pat[pi];
            if (case_insensitive) {
                tc = (char)rt_ascii_tolower((unsigned char)tc);
                pc = (char)rt_ascii_tolower((unsigned char)pc);
            }
            if (tc == pc) {
                ti++;
                pi++;
                continue;
            }
        }

        // No match — backtrack to last %
        if (star_pi != (size_t)-1) {
            pi = star_pi + 1;
            size_t step = like_utf8_step(text + star_ti, tlen - star_ti);
            if (step == 0)
                return 0;
            star_ti += step;
            ti = star_ti;
            continue;
        }

        return 0;
    }

    // Consume trailing % in pattern
    while (pi < plen && pat[pi] == '%')
        pi++;

    return pi == plen ? 1 : 0;
}

/// @brief Match a runtime string with case-sensitive SQL LIKE semantics.
/// @details Null operands are treated as empty spans. Delegates wildcard,
///          escape, strict UTF-8 stepping, and whole-pattern matching to
///          @ref like_match.
/// @param text Borrowed text string; may be NULL.
/// @param pattern Borrowed LIKE pattern; may be NULL.
/// @return One for a complete match; otherwise zero.
int8_t rt_str_like(rt_string text, rt_string pattern) {
    if (!rt_string_arg_valid_(text, "String.Like: invalid text") ||
        !rt_string_arg_valid_(pattern, "String.Like: invalid pattern")) {
        return 0;
    }
    size_t tlen = rt_string_len_bytes(text);
    size_t plen = rt_string_len_bytes(pattern);
    const char *t = tlen ? text->data : "";
    const char *p = plen ? pattern->data : "";
    return like_match(t, tlen, p, plen, 0);
}

/// @brief Match a runtime string with ASCII-case-insensitive SQL LIKE semantics.
/// @details Uses the same null, wildcard, escape, and strict UTF-8 rules as
///          @ref rt_str_like, folding only ASCII literal bytes.
/// @param text Borrowed text string; may be NULL.
/// @param pattern Borrowed LIKE pattern; may be NULL.
/// @return One for a complete match; otherwise zero.
int8_t rt_str_like_ci(rt_string text, rt_string pattern) {
    if (!rt_string_arg_valid_(text, "String.LikeCI: invalid text") ||
        !rt_string_arg_valid_(pattern, "String.LikeCI: invalid pattern")) {
        return 0;
    }
    size_t tlen = rt_string_len_bytes(text);
    size_t plen = rt_string_len_bytes(pattern);
    const char *t = tlen ? text->data : "";
    const char *p = plen ? pattern->data : "";
    return like_match(t, tlen, p, plen, 1);
}
