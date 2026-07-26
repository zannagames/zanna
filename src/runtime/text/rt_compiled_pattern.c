//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_compiled_pattern.c
// Purpose: Implements pre-compiled regex patterns for the Zanna.Text.Pattern
//          class. Compiles a regex string once into an internal representation
//          and supports IsMatch, Find, FindAll, Replace, and Split operations
//          with better performance for repeated use on the same pattern.
//
// Key invariants:
//   - Patterns are compiled exactly once at construction; compilation errors trap.
//   - The compiled form is immutable after creation; all match operations are
//     read-only and thread-safe on the same pattern object.
//   - Find returns the first match start and length; FindAll returns all matches.
//   - Replace substitutes all non-overlapping matches with the replacement string.
//   - Split divides the input at each match position.
//
// Ownership/Lifetime:
//   - Pattern objects are heap-allocated and managed by the runtime GC.
//   - The internal compiled state is freed in the finalizer.
//   - Returned match strings and sequences are fresh allocations owned by caller.
//
// Links: src/runtime/text/rt_compiled_pattern.h (public API),
//        src/runtime/text/rt_regex.h (underlying regex engine)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_compiled_pattern.c
 * @brief Implements immutable managed compiled regular-expression patterns.
 * @details Construction validates and compiles a pattern once into owned
 *          engine state. Reusable operations perform matching, capture
 *          extraction, find-all, literal replacement, and splitting while
 *          returning fresh managed Strings, Options, and sequences.
 */

#include "rt_compiled_pattern.h"
#include "rt_object.h"
#include "rt_regex_internal.h"

#include "rt_internal.h"
#include "rt_option.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rt_trap.h"

/// @brief Convert a nullable runtime-string byte length for the regex engine.
/// @param s Runtime string, or NULL to represent empty text.
/// @return Byte length as int, or 0 after trapping when it exceeds INT_MAX.
static int safe_rt_string_len_int(rt_string s) {
    size_t n = s ? (size_t)rt_str_len(s) : 0;
    if (n > (size_t)INT_MAX) {
        rt_trap("CompiledPattern: string too long for regex engine");
        return 0;
    }
    return (int)n;
}

/// @brief Obtain a borrowed text pointer with a NULL-as-empty policy.
/// @param text Runtime string, or NULL.
/// @return Borrowed runtime bytes, or a static empty C string for NULL.
static const char *compiled_text_or_empty(rt_string text) {
    return text ? rt_string_cstr(text) : "";
}

/// @brief Grow a replacement buffer to accommodate an append.
/// @details Detects length arithmetic overflow and grows geometrically while
///          preserving existing bytes. The current capacity must be nonzero.
/// @param result Address of the caller-owned allocation.
/// @param result_cap Address of its writable byte capacity.
/// @param result_len Number of bytes currently used.
/// @param add Additional bytes that must fit.
/// @return 1 when capacity exceeds the required used length, otherwise 0 after
///         an overflow or allocation trap.
static int compiled_ensure_result_capacity(char **result,
                                           size_t *result_cap,
                                           size_t result_len,
                                           size_t add) {
    if (add > SIZE_MAX - result_len) {
        rt_trap("CompiledPattern: replacement length overflow");
        return 0;
    }
    size_t needed = result_len + add;
    if (needed < *result_cap)
        return 1;
    if (needed == SIZE_MAX) {
        rt_trap("CompiledPattern: replacement length overflow");
        return 0;
    }
    size_t new_cap = *result_cap;
    while (new_cap <= needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed + 1;
            break;
        }
        new_cap *= 2;
    }
    char *tmp = (char *)realloc(*result, new_cap);
    if (!tmp) {
        rt_trap("CompiledPattern: memory allocation failed");
        return 0;
    }
    *result = tmp;
    *result_cap = new_cap;
    return 1;
}

//=============================================================================
// Internal Structure
//=============================================================================

/// @brief GC wrapper around the regex engine's separately allocated automaton.
typedef struct {
    re_compiled_pattern *pattern; ///< Owned immutable compiled regex state.
} compiled_pattern_obj;


//=============================================================================
// Creation and Lifecycle
//=============================================================================

/// @brief GC finalizer — free the underlying compiled regex automaton.
/// @details `re_compile` allocates a private NFA/DFA structure that
///          isn't part of Zanna's GC heap. This finalizer hands that
///          allocation back to the regex engine when the wrapping
///          GC object is collected. Nulled afterwards so a double
///          finalize (rare but possible during shutdown) is safe.
/// @param obj CompiledPattern wrapper being finalized; NULL is ignored.
static void compiled_pattern_finalizer(void *obj) {
    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    if (cpo && cpo->pattern) {
        re_free(cpo->pattern);
        cpo->pattern = NULL;
    }
}

/// @brief Compile a regex pattern for reuse (avoids recompilation on each call).
/// @param pattern Required regex source string without embedded NUL bytes.
/// @return New GC-managed CompiledPattern object, or NULL after a validation,
///         allocation, or regex-compilation trap.
void *rt_compiled_pattern_new(rt_string pattern) {
    if (!pattern) {
        rt_trap("CompiledPattern: null pattern");
        return NULL;
    }
    const char *pat_str = pattern ? rt_string_cstr(pattern) : "";
    if (!pat_str)
        pat_str = "";
    if (strlen(pat_str) != (size_t)rt_str_len(pattern)) {
        rt_trap("CompiledPattern: pattern contains NUL byte");
        return NULL;
    }

    compiled_pattern_obj *obj =
        (compiled_pattern_obj *)rt_obj_new_i64(0, (int64_t)sizeof(compiled_pattern_obj));
    if (!obj) {
        rt_trap("CompiledPattern: memory allocation failed");
        return NULL;
    }

    obj->pattern = re_compile(pat_str);
    if (!obj->pattern) {
        rt_trap("CompiledPattern: regex compilation failed");
        return NULL;
    }
    rt_obj_set_finalizer(obj, compiled_pattern_finalizer);
    return obj;
}

/// @brief Get the original regex source string that was compiled.
/// @param obj CompiledPattern object, or NULL.
/// @return Newly allocated pattern string for a valid object, or an empty
///         constant string for NULL.
rt_string rt_compiled_pattern_get_pattern(void *obj) {
    if (!obj)
        return rt_const_cstr("");

    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    const char *pat = re_get_pattern(cpo->pattern);
    return rt_string_from_bytes(pat, strlen(pat));
}

//=============================================================================
// Matching Operations
//=============================================================================

/// @brief Test whether the compiled pattern matches anywhere in the text.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @return 1 when any match exists, otherwise 0; a NULL object also traps.
int8_t rt_compiled_pattern_is_match(void *obj, rt_string text) {
    if (!obj) {
        rt_trap("CompiledPattern: null pattern object");
        return 0;
    }

    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    const char *txt_str = compiled_text_or_empty(text);

    int match_start, match_end;
    return re_find_match(
        cpo->pattern, txt_str, safe_rt_string_len_int(text), 0, &match_start, &match_end);
}

/// @brief Find the first match of the compiled pattern in the text.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @return Newly allocated first matching substring, or an empty string when no
///         match exists. Use rt_compiled_pattern_find_option() to distinguish no
///         match from a valid zero-width match.
rt_string rt_compiled_pattern_find(void *obj, rt_string text) {
    if (!obj) {
        rt_trap("CompiledPattern: null pattern object");
        return rt_const_cstr("");
    }

    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    const char *txt_str = compiled_text_or_empty(text);

    int text_len = safe_rt_string_len_int(text);
    int match_start, match_end;

    if (re_find_match(cpo->pattern, txt_str, text_len, 0, &match_start, &match_end)) {
        return rt_string_from_bytes(txt_str + match_start, match_end - match_start);
    }
    return rt_const_cstr("");
}

/// @brief Find the first compiled-pattern match as an Option string.
/// @details Returns `SomeStr(match)` for any match, including empty-string
///          matches, and `None` when no match exists.
/// @param obj CompiledPattern pointer.
/// @param text Text to search.
/// @return Opaque Zanna.Option containing the first match, or None.
void *rt_compiled_pattern_find_option(void *obj, rt_string text) {
    if (!obj) {
        rt_trap("CompiledPattern: null pattern object");
        return rt_option_none();
    }

    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    const char *txt_str = compiled_text_or_empty(text);

    int text_len = safe_rt_string_len_int(text);
    int match_start, match_end;
    if (re_find_match(cpo->pattern, txt_str, text_len, 0, &match_start, &match_end)) {
        rt_string match = rt_string_from_bytes(txt_str + match_start, match_end - match_start);
        void *option = rt_option_some_str(match);
        rt_str_release_maybe(match);
        return option;
    }
    return rt_option_none();
}

/// @brief Find the first match starting at or after the given byte offset.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @param start Starting byte offset; negative values clamp to zero.
/// @return Newly allocated matching substring, or an empty string when no match
///         begins at/after the clamped offset.
rt_string rt_compiled_pattern_find_from(void *obj, rt_string text, int64_t start) {
    if (!obj) {
        rt_trap("CompiledPattern: null pattern object");
        return rt_const_cstr("");
    }

    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    const char *txt_str = compiled_text_or_empty(text);

    int text_len = safe_rt_string_len_int(text);
    if (start < 0)
        start = 0;
    if (start > text_len)
        return rt_const_cstr("");

    int match_start, match_end;
    if (re_find_match(cpo->pattern, txt_str, text_len, (int)start, &match_start, &match_end)) {
        return rt_string_from_bytes(txt_str + match_start, match_end - match_start);
    }
    return rt_const_cstr("");
}

/// @brief Find the first compiled-pattern match at or after a byte offset as an Option string.
/// @param obj CompiledPattern pointer.
/// @param text Text to search.
/// @param start Starting byte offset.
/// @return Opaque Zanna.Option containing the first match, or None.
void *rt_compiled_pattern_find_from_option(void *obj, rt_string text, int64_t start) {
    if (!obj) {
        rt_trap("CompiledPattern: null pattern object");
        return rt_option_none();
    }

    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    const char *txt_str = compiled_text_or_empty(text);

    int text_len = safe_rt_string_len_int(text);
    if (start < 0)
        start = 0;
    if (start > text_len)
        return rt_option_none();

    int match_start, match_end;
    if (re_find_match(cpo->pattern, txt_str, text_len, (int)start, &match_start, &match_end)) {
        rt_string match = rt_string_from_bytes(txt_str + match_start, match_end - match_start);
        void *option = rt_option_some_str(match);
        rt_str_release_maybe(match);
        return option;
    }
    return rt_option_none();
}

/// @brief Find the byte position of the first match (-1 if no match).
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @return Zero-based byte offset of the first match, or -1 when absent or
///         after trapping for a NULL object.
int64_t rt_compiled_pattern_find_pos(void *obj, rt_string text) {
    if (!obj) {
        rt_trap("CompiledPattern: null pattern object");
        return -1;
    }

    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    const char *txt_str = compiled_text_or_empty(text);

    int match_start, match_end;
    if (re_find_match(
            cpo->pattern, txt_str, safe_rt_string_len_int(text), 0, &match_start, &match_end)) {
        return (int64_t)match_start;
    }
    return -1;
}

/// @brief Find the byte position of the first compiled-pattern match as an Option index.
/// @param obj CompiledPattern pointer.
/// @param text Text to search.
/// @return Opaque Zanna.Option containing the first position, or None.
void *rt_compiled_pattern_find_pos_option(void *obj, rt_string text) {
    if (!obj) {
        rt_trap("CompiledPattern: null pattern object");
        return rt_option_none();
    }

    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    const char *txt_str = compiled_text_or_empty(text);

    int match_start, match_end;
    if (re_find_match(
            cpo->pattern, txt_str, safe_rt_string_len_int(text), 0, &match_start, &match_end))
        return rt_option_some_i64((int64_t)match_start);
    return rt_option_none();
}

/// @brief Find all non-overlapping matches and return as a sequence of strings.
/// @details Zero-width matches advance by one byte to guarantee progress.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @return Caller-owned element-owning Seq of newly allocated match strings;
///         no matches produce an empty Seq.
void *rt_compiled_pattern_find_all(void *obj, rt_string text) {
    if (!obj) {
        rt_trap("CompiledPattern: null pattern object");
        void *seq = rt_seq_new();
        if (seq)
            rt_seq_set_owns_elements(seq, 1);
        return seq;
    }

    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    const char *txt_str = compiled_text_or_empty(text);

    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    int text_len = safe_rt_string_len_int(text);
    int pos = 0;

    while (pos <= text_len) {
        int match_start, match_end;
        if (!re_find_match(cpo->pattern, txt_str, text_len, pos, &match_start, &match_end))
            break;

        rt_string match = rt_string_from_bytes(txt_str + match_start, match_end - match_start);
        rt_seq_push(seq, (void *)match);
        rt_string_unref(match);

        pos = match_end > match_start ? match_end : match_start + 1;
    }

    return seq;
}

//=============================================================================
// Capture Groups
//=============================================================================

/// @brief Extract capture groups from the first match (returns a sequence of group strings).
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @return Caller-owned Seq whose first element is the full match followed by
///         capture groups, or an empty Seq when no match exists.
void *rt_compiled_pattern_captures(void *obj, rt_string text) {
    return rt_compiled_pattern_captures_from(obj, text, 0);
}

/// @brief Extract capture groups starting at or after the given byte offset.
/// @details Nonparticipating capture groups are represented by empty strings.
/// @param obj Required CompiledPattern object.
/// @param text Text to search; NULL is treated as empty.
/// @param start Starting byte offset; negatives clamp to zero.
/// @return Caller-owned element-owning Seq containing full match then lexical
///         capture groups, or an empty Seq when out of range/no match.
void *rt_compiled_pattern_captures_from(void *obj, rt_string text, int64_t start) {
    if (!obj) {
        rt_trap("CompiledPattern: null pattern object");
        void *seq = rt_seq_new();
        if (seq)
            rt_seq_set_owns_elements(seq, 1);
        return seq;
    }

    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    const char *txt_str = compiled_text_or_empty(text);

    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    int text_len = safe_rt_string_len_int(text);

    if (start < 0)
        start = 0;
    if (start > text_len)
        return seq;

    // Size capture arrays from the compiled pattern so any group count is
    // supported without a fixed cap (VDOC-058).
    int total_groups = re_group_count(cpo->pattern);
    if (total_groups < 1)
        total_groups = 1;
    int *group_starts = (int *)malloc(sizeof(int) * (size_t)total_groups);
    int *group_ends = (int *)malloc(sizeof(int) * (size_t)total_groups);
    if (!group_starts || !group_ends) {
        free(group_starts);
        free(group_ends);
        rt_trap("CompiledPattern: memory allocation failed");
        return seq;
    }
    int match_start, match_end, num_groups;

    if (re_find_match_with_groups(cpo->pattern,
                                  txt_str,
                                  text_len,
                                  (int)start,
                                  &match_start,
                                  &match_end,
                                  group_starts,
                                  group_ends,
                                  total_groups,
                                  &num_groups)) {
        // Group 0 is the full match
        rt_string full_match = rt_string_from_bytes(txt_str + match_start, match_end - match_start);
        rt_seq_push(seq, (void *)full_match);
        rt_string_unref(full_match);

        // Add captured groups in lexical order; a group that did not
        // participate in the match (start == -1) reports an empty string.
        for (int i = 0; i < num_groups; i++) {
            rt_string group = group_starts[i] >= 0
                ? rt_string_from_bytes(txt_str + group_starts[i],
                                       group_ends[i] - group_starts[i])
                : rt_string_from_bytes("", 0);
            rt_seq_push(seq, (void *)group);
            rt_string_unref(group);
        }
    }

    free(group_starts);
    free(group_ends);
    return seq;
}

//=============================================================================
// Replacement Operations
//=============================================================================

/// @brief Replace all matches of the compiled pattern with the replacement string.
/// @details Replacement bytes are literal rather than capture-expansion syntax.
///          Zero-width matches preserve the stepped-over input byte.
/// @param obj Required CompiledPattern object.
/// @param text Input text; NULL is treated as empty.
/// @param replacement Literal replacement; NULL is treated as empty.
/// @return Newly allocated replaced string, or an empty fallback after a
///         validation, overflow, or allocation trap.
rt_string rt_compiled_pattern_replace(void *obj, rt_string text, rt_string replacement) {
    if (!obj) {
        rt_trap("CompiledPattern: null pattern object");
        return rt_string_from_bytes("", 0);
    }

    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    const char *txt_str = compiled_text_or_empty(text);
    const char *rep_str = compiled_text_or_empty(replacement);

    int text_len = safe_rt_string_len_int(text);
    int rep_len = safe_rt_string_len_int(replacement);

    // Build result
    size_t result_cap = (size_t)text_len + 64;
    if (result_cap < (size_t)text_len) {
        rt_trap("CompiledPattern: replacement length overflow");
        return rt_string_from_bytes("", 0);
    }
    char *result = (char *)malloc(result_cap);
    if (!result) {
        rt_trap("CompiledPattern: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }
    size_t result_len = 0;

    int pos = 0;
    while (pos <= text_len) {
        int match_start, match_end;
        if (!re_find_match(cpo->pattern, txt_str, text_len, pos, &match_start, &match_end)) {
            // Copy rest of text
            size_t remaining = text_len - pos;
            if (!compiled_ensure_result_capacity(&result, &result_cap, result_len, remaining)) {
                free(result);
                return rt_string_from_bytes("", 0);
            }
            memcpy(result + result_len, txt_str + pos, remaining);
            result_len += remaining;
            break;
        }

        // Copy text before match
        size_t before_len = match_start - pos;
        if (!compiled_ensure_result_capacity(
                &result, &result_cap, result_len, before_len + (size_t)rep_len)) {
            free(result);
            return rt_string_from_bytes("", 0);
        }
        memcpy(result + result_len, txt_str + pos, before_len);
        result_len += before_len;

        // Copy replacement
        memcpy(result + result_len, rep_str, rep_len);
        result_len += rep_len;

        // Move past match. A zero-width match must not swallow the byte we
        // step over to guarantee progress (VDOC-054).
        if (match_end > match_start) {
            pos = match_end;
        } else {
            if (match_start < text_len) {
                if (!compiled_ensure_result_capacity(&result, &result_cap, result_len, 1)) {
                    free(result);
                    return rt_string_from_bytes("", 0);
                }
                result[result_len++] = txt_str[match_start];
            }
            pos = match_start + 1;
        }
    }

    rt_string out = rt_string_from_bytes(result, result_len);
    free(result);
    return out;
}

/// @brief Replace only the first match of the compiled pattern.
/// @param obj Required CompiledPattern object.
/// @param text Input text; NULL is treated as empty.
/// @param replacement Literal replacement; NULL is treated as empty.
/// @return Newly allocated replaced string, or a fresh copy of @p text when no
///         match exists; failures trap and return an empty fallback.
rt_string rt_compiled_pattern_replace_first(void *obj, rt_string text, rt_string replacement) {
    if (!obj) {
        rt_trap("CompiledPattern: null pattern object");
        return rt_string_from_bytes("", 0);
    }

    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    const char *txt_str = compiled_text_or_empty(text);
    const char *rep_str = compiled_text_or_empty(replacement);

    int text_len = safe_rt_string_len_int(text);
    int rep_len = safe_rt_string_len_int(replacement);

    int match_start, match_end;
    if (!re_find_match(cpo->pattern, txt_str, text_len, 0, &match_start, &match_end)) {
        return rt_string_from_bytes(txt_str, text_len);
    }

    // Build result: before + replacement + after
    size_t result_len = (size_t)match_start;
    if ((size_t)rep_len > SIZE_MAX - result_len ||
        (size_t)(text_len - match_end) > SIZE_MAX - result_len - (size_t)rep_len) {
        rt_trap("CompiledPattern: replacement length overflow");
        return rt_string_from_bytes("", 0);
    }
    result_len += (size_t)rep_len + (size_t)(text_len - match_end);
    if (result_len == SIZE_MAX) {
        rt_trap("CompiledPattern: replacement length overflow");
        return rt_string_from_bytes("", 0);
    }
    char *result = (char *)malloc(result_len + 1);
    if (!result) {
        rt_trap("CompiledPattern: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    memcpy(result, txt_str, match_start);
    memcpy(result + match_start, rep_str, rep_len);
    memcpy(result + match_start + rep_len, txt_str + match_end, text_len - match_end);

    rt_string out = rt_string_from_bytes(result, result_len);
    free(result);
    return out;
}

//=============================================================================
// Split Operation
//=============================================================================

/// @brief Split a string by the compiled pattern (unlimited splits).
/// @param obj Required CompiledPattern object.
/// @param text Input text; NULL is treated as empty.
/// @return Caller-owned element-owning Seq of substrings, including a trailing
///         empty element when the text ends with a nonzero-width separator.
void *rt_compiled_pattern_split(void *obj, rt_string text) {
    return rt_compiled_pattern_split_n(obj, text, 0);
}

/// @brief Split a string by the compiled pattern, returning at most `limit` pieces (0 = unlimited).
/// @details Nonpositive limits are unlimited. Zero-width matches neither split
///          at the current segment start nor at end-of-text, preventing byte loss.
/// @param obj Required CompiledPattern object.
/// @param text Input text; NULL is treated as empty.
/// @param limit Maximum result pieces when positive; nonpositive is unlimited.
/// @return Caller-owned element-owning Seq of substring strings.
void *rt_compiled_pattern_split_n(void *obj, rt_string text, int64_t limit) {
    if (!obj) {
        rt_trap("CompiledPattern: null pattern object");
        void *seq = rt_seq_new();
        if (seq)
            rt_seq_set_owns_elements(seq, 1);
        return seq;
    }

    compiled_pattern_obj *cpo = (compiled_pattern_obj *)obj;
    const char *txt_str = compiled_text_or_empty(text);

    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    int text_len = safe_rt_string_len_int(text);
    int pos = 0;
    int64_t split_count = 0;

    int seg_start = 0;
    while (pos <= text_len) {
        // Check limit (0 means unlimited)
        if (limit > 0 && split_count >= limit - 1)
            break;

        int match_start, match_end;
        if (!re_find_match(cpo->pattern, txt_str, text_len, pos, &match_start, &match_end))
            break;

        if (match_end == match_start) {
            // Zero-width match: never split at the current segment start or
            // at end-of-text; the stepped-over byte stays in the next
            // segment so no source bytes are lost (VDOC-054).
            if (match_start >= text_len)
                break;
            if (match_start > seg_start) {
                rt_string part =
                    rt_string_from_bytes(txt_str + seg_start, match_start - seg_start);
                rt_seq_push(seq, (void *)part);
                rt_string_unref(part);
                seg_start = match_start;
                split_count++;
            }
            pos = match_start + 1;
            continue;
        }

        // Add text before match
        rt_string part = rt_string_from_bytes(txt_str + seg_start, match_start - seg_start);
        rt_seq_push(seq, (void *)part);
        rt_string_unref(part);
        split_count++;
        seg_start = match_end;
        pos = match_end;
    }

    // Remaining text (empty when the text ends with a separator).
    rt_string tail = rt_string_from_bytes(txt_str + seg_start, text_len - seg_start);
    rt_seq_push(seq, (void *)tail);
    rt_string_unref(tail);

    return seq;
}
