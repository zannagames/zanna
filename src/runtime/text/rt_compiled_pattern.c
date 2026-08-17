//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_compiled_pattern.c
// Purpose: Implements reusable regex patterns, recoverable interactive
//          compilation, exact match ranges, capture expansion, and the legacy
//          Pattern matching/replacement surface.
//
// Key invariants:
//   - Patterns are compiled exactly once; New traps while TryNew returns syntax errors.
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
#include "rt_box.h"
#include "rt_object.h"
#include "rt_regex_internal.h"

#include "rt_internal.h"
#include "rt_option.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
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

/// @brief Allocate a managed wrapper that takes ownership of an engine pattern.
/// @param pattern Owned successfully compiled engine object.
/// @return New managed wrapper, or null after freeing @p pattern on failure.
static compiled_pattern_obj *compiled_pattern_wrap(re_compiled_pattern *pattern) {
    if (!pattern)
        return NULL;
    compiled_pattern_obj *obj =
        (compiled_pattern_obj *)rt_obj_new_i64(0, (int64_t)sizeof(compiled_pattern_obj));
    if (!obj) {
        re_free(pattern);
        rt_trap("CompiledPattern: memory allocation failed");
        return NULL;
    }
    obj->pattern = pattern;
    rt_obj_set_finalizer(obj, compiled_pattern_finalizer);
    return obj;
}

/// @brief Create a string-valued error Result from a bounded C diagnostic.
static void *compiled_pattern_error_result(const char *message) {
    const char *safe = message && message[0] ? message : "CompiledPattern: invalid pattern";
    rt_string text = rt_string_from_bytes(safe, strlen(safe));
    if (!text)
        return NULL;
    void *result = rt_result_err_str(text);
    rt_str_release_maybe(text);
    return result;
}

/// @brief Validate and compile one runtime string with recoverable syntax errors.
static re_compiled_pattern *compiled_pattern_compile_diagnostic(rt_string pattern,
                                                                unsigned int flags,
                                                                char *error,
                                                                size_t error_capacity) {
    if (error && error_capacity > 0)
        error[0] = '\0';
    if (!pattern) {
        if (error && error_capacity > 0)
            snprintf(error, error_capacity, "CompiledPattern: null pattern");
        return NULL;
    }
    const char *source = rt_string_cstr(pattern);
    size_t length = (size_t)rt_str_len(pattern);
    if (!source || strlen(source) != length) {
        if (error && error_capacity > 0)
            snprintf(error, error_capacity, "CompiledPattern: pattern contains NUL byte");
        return NULL;
    }
    re_set_failure_handler(rt_trap);
    return re_compile_diagnostic(source, flags, error, error_capacity);
}

/// @brief Compile a regex pattern for reuse (avoids recompilation on each call).
/// @param pattern Required regex source string without embedded NUL bytes.
/// @return New GC-managed CompiledPattern object, or NULL after a validation,
///         allocation, or regex-compilation trap.
void *rt_compiled_pattern_new(rt_string pattern) {
    char error[256];
    re_compiled_pattern *compiled =
        compiled_pattern_compile_diagnostic(pattern, RE_COMPILE_DEFAULT, error, sizeof(error));
    if (!compiled) {
        rt_trap(error[0] ? error : "CompiledPattern: regex compilation failed");
        return NULL;
    }
    return compiled_pattern_wrap(compiled);
}

/// @copydoc rt_compiled_pattern_try_new
void *rt_compiled_pattern_try_new(rt_string pattern, int8_t case_insensitive) {
    char error[256];
    unsigned int flags = case_insensitive ? RE_COMPILE_CASE_INSENSITIVE : RE_COMPILE_DEFAULT;
    re_compiled_pattern *compiled =
        compiled_pattern_compile_diagnostic(pattern, flags, error, sizeof(error));
    if (!compiled)
        return compiled_pattern_error_result(error);

    compiled_pattern_obj *obj = compiled_pattern_wrap(compiled);
    if (!obj)
        return NULL;
    void *result = rt_result_ok(obj);
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
    return result;
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

/// @brief Decode one strict UTF-8 scalar and its exclusive byte end.
static bool compiled_utf8_decode_at(
    const char *text, int text_len, int offset, uint32_t *codepoint, int *next_offset) {
    if (!text || !codepoint || !next_offset || offset < 0 || offset >= text_len)
        return false;
    const unsigned char *bytes = (const unsigned char *)text;
    unsigned char lead = bytes[offset];
    uint32_t value = 0;
    int width = 0;
    if (lead < 0x80u) {
        value = lead;
        width = 1;
    } else if (lead >= 0xC2u && lead <= 0xDFu) {
        value = lead & 0x1Fu;
        width = 2;
    } else if (lead >= 0xE0u && lead <= 0xEFu) {
        value = lead & 0x0Fu;
        width = 3;
    } else if (lead >= 0xF0u && lead <= 0xF4u) {
        value = lead & 0x07u;
        width = 4;
    } else {
        return false;
    }
    if (width > text_len - offset)
        return false;
    for (int i = 1; i < width; i++) {
        unsigned char byte = bytes[offset + i];
        if ((byte & 0xC0u) != 0x80u)
            return false;
        value = (value << 6) | (uint32_t)(byte & 0x3Fu);
    }
    if ((width == 3 && value < 0x800u) || (width == 4 && value < 0x10000u) ||
        (value >= 0xD800u && value <= 0xDFFFu) || value > 0x10FFFFu)
        return false;
    *codepoint = value;
    *next_offset = offset + width;
    return true;
}

/// @brief Test whether the scalar immediately before an offset continues a word.
static bool compiled_word_before(const char *text, int text_len, int offset) {
    if (!text || offset <= 0 || offset > text_len)
        return false;
    int start = offset - 1;
    while (start > 0 && ((unsigned char)text[start] & 0xC0u) == 0x80u)
        start--;
    uint32_t codepoint = 0;
    int next = 0;
    return compiled_utf8_decode_at(text, text_len, start, &codepoint, &next) && next == offset &&
           re_is_word_codepoint(codepoint);
}

/// @brief Test whether the scalar beginning at an offset continues a word.
static bool compiled_word_at(const char *text, int text_len, int offset) {
    if (!text || offset < 0 || offset >= text_len)
        return false;
    uint32_t codepoint = 0;
    int next = 0;
    return compiled_utf8_decode_at(text, text_len, offset, &codepoint, &next) &&
           re_is_word_codepoint(codepoint);
}

/// @brief Find a range while optionally rejecting Unicode word-adjacent matches.
static bool compiled_find_range(compiled_pattern_obj *obj,
                                const char *text,
                                int text_len,
                                int start,
                                bool whole_word,
                                int *match_start,
                                int *match_end) {
    int cursor = start;
    while (cursor <= text_len) {
        int found_start = 0;
        int found_end = 0;
        if (!re_find_match(obj->pattern, text, text_len, cursor, &found_start, &found_end))
            return false;
        if (!whole_word || (!compiled_word_before(text, text_len, found_start) &&
                            !compiled_word_at(text, text_len, found_end))) {
            *match_start = found_start;
            *match_end = found_end;
            return true;
        }
        if (found_start >= text_len)
            return false;
        uint32_t ignored = 0;
        int next = found_start + 1;
        (void)compiled_utf8_decode_at(text, text_len, found_start, &ignored, &next);
        if (next <= cursor)
            next = cursor + 1;
        cursor = next;
    }
    return false;
}

/// @brief Release one freshly allocated box after an aborted range build.
static void compiled_release_box(void *box) {
    if (box && rt_obj_release_check0(box))
        rt_obj_free(box);
}

/// @copydoc rt_compiled_pattern_find_range_from
void *rt_compiled_pattern_find_range_from(void *obj,
                                          rt_string text,
                                          int64_t start,
                                          int8_t whole_word) {
    void *range = rt_seq_new_owned();
    if (!range)
        return NULL;
    if (!obj) {
        rt_trap("CompiledPattern: null pattern object");
        return range;
    }

    int text_len = safe_rt_string_len_int(text);
    if (start < 0)
        start = 0;
    if (start > text_len)
        return range;
    const char *bytes = compiled_text_or_empty(text);
    int match_start = 0;
    int match_end = 0;
    if (!compiled_find_range((compiled_pattern_obj *)obj,
                             bytes,
                             text_len,
                             (int)start,
                             whole_word != 0,
                             &match_start,
                             &match_end))
        return range;

    int64_t resume = match_end;
    if (match_end == match_start) {
        if (match_start >= text_len) {
            resume = (int64_t)text_len + 1;
        } else {
            uint32_t ignored = 0;
            int next = match_start + 1;
            (void)compiled_utf8_decode_at(bytes, text_len, match_start, &ignored, &next);
            resume = next;
        }
    }

    void *start_box = rt_box_i64(match_start);
    void *end_box = rt_box_i64(match_end);
    void *resume_box = rt_box_i64(resume);
    if (!start_box || !end_box || !resume_box) {
        compiled_release_box(start_box);
        compiled_release_box(end_box);
        compiled_release_box(resume_box);
        rt_trap("CompiledPattern: match range allocation failed");
        return range;
    }
    rt_seq_push_raw(range, start_box);
    rt_seq_push_raw(range, end_box);
    rt_seq_push_raw(range, resume_box);
    return range;
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

/// @copydoc rt_compiled_pattern_expand_replacement_at
void *rt_compiled_pattern_expand_replacement_at(void *obj,
                                                rt_string text,
                                                int64_t start,
                                                rt_string replacement) {
    if (!obj)
        return compiled_pattern_error_result("CompiledPattern: null pattern object");
    int text_len = safe_rt_string_len_int(text);
    if (start < 0 || start > text_len)
        return compiled_pattern_error_result("CompiledPattern: replacement start is out of range");

    const char *source = compiled_text_or_empty(text);
    const char *replacement_bytes = compiled_text_or_empty(replacement);
    size_t replacement_len = replacement ? (size_t)rt_str_len(replacement) : 0;
    char *expanded = NULL;
    size_t expanded_len = 0;
    compiled_pattern_obj *pattern = (compiled_pattern_obj *)obj;
    if (!re_expand_replacement(pattern->pattern,
                               source,
                               text_len,
                               (int)start,
                               replacement_bytes,
                               replacement_len,
                               &expanded,
                               &expanded_len))
        return compiled_pattern_error_result(
            "CompiledPattern: no exact match exists at the replacement start");

    rt_string value = rt_string_from_bytes(expanded, expanded_len);
    free(expanded);
    if (!value)
        return NULL;
    void *result = rt_result_ok_str(value);
    rt_str_release_maybe(value);
    return result;
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
                rt_string part = rt_string_from_bytes(txt_str + seg_start, match_start - seg_start);
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
