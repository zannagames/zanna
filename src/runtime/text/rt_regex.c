//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_regex.c
// Purpose: Implements regular expression pattern matching for the static
//          Zanna.Text.Pattern class using a backtracking AST matcher.
//          Supports literals, '.', '^', '$', character classes '[...]',
//          shorthand classes (\d \w \s and their complements), quantifiers
//          (*, +, ?), non-greedy quantifiers (*?, +?, ??), capture groups
//          '()', and alternation '|'.
//
// Key invariants:
//   - Bounded quantifiers '{n,m}', backreferences, lookaround, and named
//     groups are NOT supported; braces are ordinary literal characters.
//   - Patterns containing an embedded NUL byte trap (VDOC-053).
//   - Pattern compilation is cached (lock-protected) to amortize repeat use.
//   - FindAll returns all non-overlapping matches left-to-right.
//   - Replace replaces all non-overlapping matches; zero-width matches
//     preserve the stepped-over source byte (VDOC-054).
//   - Anchors (^ $) are applied relative to the full input string.
//   - Character classes are byte-level; Unicode codepoints are not decomposed.
//
// Ownership/Lifetime:
//   - Compiled patterns are cached in a bounded LRU table (max 16 entries);
//     least-recently-used entries are freed on eviction.
//   - Returned match strings and sequences are fresh allocations owned by caller.
//
// Links: src/runtime/text/rt_regex.h (public API),
//        src/runtime/text/rt_regex_internal.h (compiled NFA node definitions),
//        src/runtime/text/rt_compiled_pattern.h (cached pre-compiled wrapper)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_regex.c
 * @brief Implements the public byte-oriented regex API and pattern cache.
 * @details Static Pattern operations validate managed inputs, acquire immutable
 *          compiled ASTs through a synchronized bounded LRU cache, and expose
 *          searching, captures, enumeration, literal replacement, splitting,
 *          and escaping with explicit zero-width progress behavior.
 */

#include "rt_regex.h"
#include "rt_regex_internal.h"

#include "rt_internal.h"
#include "rt_option.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* S-12: Pattern cache lock — protect concurrent access */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static CRITICAL_SECTION g_pattern_cache_cs;
static INIT_ONCE g_pattern_cache_cs_once = INIT_ONCE_STATIC_INIT;

/// @brief One-shot initializer for the Win32 critical section guarding
///        the compiled-pattern cache. Called via `InitOnceExecuteOnce`.
/// @param o Windows one-time initialization token; unused.
/// @param p Caller parameter supplied by `InitOnceExecuteOnce`; unused.
/// @param ctx Optional initialization context output; unused.
/// @return `TRUE` after initialization, or `FALSE` when native allocation fails.
static BOOL WINAPI init_pattern_cache_cs(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o;
    (void)p;
    (void)ctx;
    return InitializeCriticalSectionEx(&g_pattern_cache_cs, 0, 0);
}

/// @brief Acquire the pattern-cache mutex (Windows path).
///
/// Lazily initializes the critical section on first call. Required
/// per S-12: concurrent regex calls must not race on the LRU cache.
static void pattern_cache_lock(void) {
    if (!InitOnceExecuteOnce(&g_pattern_cache_cs_once, init_pattern_cache_cs, NULL, NULL))
        rt_abort("Regex: pattern-cache lock initialization failed");
    EnterCriticalSection(&g_pattern_cache_cs);
}

/// @brief Release the pattern-cache mutex (Windows path).
static void pattern_cache_unlock(void) {
    LeaveCriticalSection(&g_pattern_cache_cs);
}
#else
#include <pthread.h>
static pthread_mutex_t g_pattern_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/// @brief Acquire the pattern-cache mutex (POSIX path).
///
/// Uses a statically-initialized mutex — no init step needed.
static void pattern_cache_lock(void) {
    pthread_mutex_lock(&g_pattern_cache_mutex);
}

/// @brief Release the pattern-cache mutex (POSIX path).
static void pattern_cache_unlock(void) {
    pthread_mutex_unlock(&g_pattern_cache_mutex);
}
#endif

#include "rt_trap.h"

/// @brief Read a runtime string length for the engine's signed-int domain.
/// @details A null runtime string is treated as empty.
/// @param s Borrowed runtime string, or `NULL`.
/// @return Byte length as `int`; traps and returns zero for an invalid length
///         or a length greater than `INT_MAX`.
static int safe_rt_string_len_int(rt_string s) {
    if (!s)
        return 0;
    int64_t n = rt_str_len(s);
    if (n < 0 || (uint64_t)n > (uint64_t)INT_MAX) {
        rt_trap("Pattern: string too long for regex engine");
        return 0;
    }
    return (int)n;
}

/// @brief Obtain a borrowed text buffer while treating null input as empty.
/// @param text Borrowed runtime text string, or `NULL`.
/// @return Borrowed backing buffer, or a stable empty C string when absent or
///         unbacked.
static const char *pattern_text_or_empty(rt_string text) {
    const char *cstr = text ? rt_string_cstr(text) : "";
    return cstr ? cstr : "";
}

/// @brief Validate a required regex pattern for C-string compilation.
/// @details Null, unbacked, and embedded-null patterns trap. Pattern source is
///          length-checked because compilation and cache keys use C strings.
/// @param pattern Borrowed runtime pattern string.
/// @return Borrowed null-terminated pattern bytes, or an empty C string if
///         execution resumes after a validation trap.
static const char *pattern_required(rt_string pattern) {
    if (!pattern) {
        rt_trap("Pattern: null pattern");
        return "";
    }
    const char *cstr = rt_string_cstr(pattern);
    if (!cstr) {
        rt_trap("Pattern: invalid pattern string");
        return "";
    }
    // Runtime strings are length-prefixed and may contain NUL, but the regex
    // engine consumes C strings; a shorter C view would silently truncate the
    // pattern and alias distinct patterns in the compile cache.
    if (strlen(cstr) != (size_t)rt_str_len(pattern)) {
        rt_trap("Pattern: pattern contains NUL byte");
        return "";
    }
    return cstr;
}

/// @brief Grow a replacement buffer to accommodate additional bytes.
/// @details Capacity includes room beyond the requested data length. Growth is
///          geometric until overflow proximity requires an exact allocation.
/// @param result In/out pointer to the reallocatable buffer.
/// @param result_cap In/out allocated capacity in bytes.
/// @param result_len Number of data bytes already stored.
/// @param add Number of additional data bytes required.
/// @param trap_msg Trap message used for arithmetic overflow.
/// @return `1` when capacity is sufficient, otherwise `0` after trapping.
static int ensure_result_capacity(
    char **result, size_t *result_cap, size_t result_len, size_t add, const char *trap_msg) {
    if (add > SIZE_MAX - result_len) {
        rt_trap(trap_msg);
        return 0;
    }
    size_t needed = result_len + add;
    if (needed < *result_cap)
        return 1;
    if (needed == SIZE_MAX) {
        rt_trap(trap_msg);
        return 0;
    }
    size_t new_cap = *result_cap;
    if (new_cap == 0)
        new_cap = 64;
    while (new_cap <= needed) {
        if (new_cap > SIZE_MAX / 2) {
            if (needed == SIZE_MAX) {
                rt_trap(trap_msg);
                return 0;
            }
            new_cap = needed + 1;
            break;
        }
        new_cap *= 2;
    }
    char *tmp = (char *)realloc(*result, new_cap);
    if (!tmp) {
        rt_trap("Pattern: memory allocation failed");
        return 0;
    }
    *result = tmp;
    *result_cap = new_cap;
    return 1;
}

//=============================================================================
// Memory Management
//=============================================================================


//=============================================================================
// Pattern Cache (Simple LRU)
//=============================================================================

#define PATTERN_CACHE_SIZE 16

typedef struct cache_entry {
    compiled_pattern *pattern;
    unsigned long access_count;
} cache_entry;

static cache_entry pattern_cache[PATTERN_CACHE_SIZE];
static unsigned long access_counter = 0;

/// @brief Acquire a compiled pattern from the bounded shared cache.
/// @details Cache hits increment an active-reference count. Cache misses compile
///          outside the lock, then deduplicate under the lock. If all 16 slots
///          are actively referenced, the new pattern remains unlinked and is
///          freed by `release_cached_pattern`.
/// @param pattern_str Required null-terminated pattern source.
/// @return Borrowed-for-use compiled pattern with one active cache reference,
///         or `NULL` after compilation failure.
static compiled_pattern *get_cached_pattern(const char *pattern_str) {
    re_set_failure_handler(rt_trap);
    pattern_cache_lock();

    for (int i = 0; i < PATTERN_CACHE_SIZE; i++) {
        if (pattern_cache[i].pattern &&
            strcmp(pattern_cache[i].pattern->pattern_str, pattern_str) == 0) {
            pattern_cache[i].access_count = ++access_counter;
            compiled_pattern *found = pattern_cache[i].pattern;
            found->cache_refs++;
            pattern_cache_unlock();
            return found;
        }
    }

    pattern_cache_unlock();
    compiled_pattern *cp = re_compile(pattern_str);
    if (!cp)
        return NULL;
    cp->cache_refs = 1;
    cp->cache_linked = false;

    pattern_cache_lock();

    for (int i = 0; i < PATTERN_CACHE_SIZE; i++) {
        if (pattern_cache[i].pattern &&
            strcmp(pattern_cache[i].pattern->pattern_str, pattern_str) == 0) {
            pattern_cache[i].access_count = ++access_counter;
            compiled_pattern *found = pattern_cache[i].pattern;
            found->cache_refs++;
            pattern_cache_unlock();
            re_free(cp);
            return found;
        }
    }

    int slot = -1;
    unsigned long min_access = ULONG_MAX;
    for (int i = 0; i < PATTERN_CACHE_SIZE; i++) {
        if (!pattern_cache[i].pattern) {
            slot = i;
            break;
        }
        if (pattern_cache[i].pattern->cache_refs == 0 &&
            pattern_cache[i].access_count < min_access) {
            min_access = pattern_cache[i].access_count;
            slot = i;
        }
    }

    if (slot < 0) {
        pattern_cache_unlock();
        return cp;
    }

    if (pattern_cache[slot].pattern) {
        pattern_cache[slot].pattern->cache_linked = false;
        re_free(pattern_cache[slot].pattern);
    }

    cp->cache_linked = true;
    pattern_cache[slot].pattern = cp;
    pattern_cache[slot].access_count = ++access_counter;

    pattern_cache_unlock();
    return cp;
}

/// @brief Release one active reference obtained from the pattern cache.
/// @details Linked entries remain resident at zero references; an unlinked
///          pattern is destroyed when its final active reference is released.
/// @param cp Compiled pattern previously returned by `get_cached_pattern`.
static void release_cached_pattern(compiled_pattern *cp) {
    if (!cp)
        return;
    bool should_free = false;
    pattern_cache_lock();
    if (cp->cache_refs > 0)
        cp->cache_refs--;
    should_free = (cp->cache_refs == 0 && !cp->cache_linked);
    pattern_cache_unlock();
    if (should_free)
        re_free(cp);
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Test whether a regex pattern matches anywhere in the text.
/// @details A null text is treated as empty. The required pattern is compiled
///          through the shared cache; invalid syntax or embedded null bytes
///          trap.
/// @param text Borrowed byte string to search, or `NULL` for empty text.
/// @param pattern Borrowed required regex pattern.
/// @return `1` when any match exists, otherwise `0`.
int8_t rt_pattern_is_match(rt_string text, rt_string pattern) {
    const char *pat_str = pattern_required(pattern);
    const char *txt_str = pattern_text_or_empty(text);
    int text_len = safe_rt_string_len_int(text);

    compiled_pattern *cp = get_cached_pattern(pat_str);
    int match_start, match_end;
    int8_t matched = re_find_match(cp, txt_str, text_len, 0, &match_start, &match_end) ? 1 : 0;
    release_cached_pattern(cp);
    return matched;
}

/// @brief Find the first match of a regex pattern in the text (empty string if no match).
/// @details Searches left-to-right from byte offset zero. Because the empty
///          string is also a valid zero-width match, use
///          `rt_pattern_find_option` when absence must be distinguishable.
/// @param text Borrowed byte string to search, or `NULL` for empty text.
/// @param pattern Borrowed required regex pattern.
/// @return Newly allocated matching substring, or an empty-string sentinel
///         when no match exists.
rt_string rt_pattern_find(rt_string text, rt_string pattern) {
    const char *pat_str = pattern_required(pattern);
    const char *txt_str = pattern_text_or_empty(text);

    compiled_pattern *cp = get_cached_pattern(pat_str);
    int text_len = safe_rt_string_len_int(text);
    int match_start, match_end;
    rt_string result;

    if (re_find_match(cp, txt_str, text_len, 0, &match_start, &match_end)) {
        result = rt_string_from_bytes(txt_str + match_start, match_end - match_start);
    } else {
        result = rt_const_cstr("");
    }
    release_cached_pattern(cp);
    return result;
}

/// @brief Find the first regex match and return it as an Option string.
/// @details This sentinel-free variant returns `SomeStr(match)` for any match,
///          including a valid empty-string match, and `None` when no match
///          exists. Invalid pattern syntax still traps like @ref rt_pattern_find.
/// @param text Text to search.
/// @param pattern Regex pattern string.
/// @return Opaque Zanna.Option containing the first match, or None.
void *rt_pattern_find_option(rt_string text, rt_string pattern) {
    const char *pat_str = pattern_required(pattern);
    const char *txt_str = pattern_text_or_empty(text);

    compiled_pattern *cp = get_cached_pattern(pat_str);
    int text_len = safe_rt_string_len_int(text);
    int match_start, match_end;
    void *option = NULL;

    if (re_find_match(cp, txt_str, text_len, 0, &match_start, &match_end)) {
        rt_string match = rt_string_from_bytes(txt_str + match_start, match_end - match_start);
        option = rt_option_some_str(match);
        rt_str_release_maybe(match);
    } else {
        option = rt_option_none();
    }
    release_cached_pattern(cp);
    return option;
}

/// @brief Find the first match starting at or after the given byte offset.
/// @details Negative offsets clamp to zero. An offset beyond the text length
///          returns the empty sentinel without compiling the pattern further;
///          use the Option variant to distinguish absence from an empty match.
/// @param text Borrowed byte string to search, or `NULL` for empty text.
/// @param pattern Borrowed required regex pattern.
/// @param start Starting byte offset.
/// @return Newly allocated matching substring, or an empty-string sentinel
///         when no match exists at or after the clamped offset.
rt_string rt_pattern_find_from(rt_string text, rt_string pattern, int64_t start) {
    const char *pat_str = pattern_required(pattern);
    const char *txt_str = pattern_text_or_empty(text);

    int text_len = safe_rt_string_len_int(text);
    if (start < 0)
        start = 0;
    if (start > text_len)
        return rt_const_cstr("");

    compiled_pattern *cp = get_cached_pattern(pat_str);
    int match_start, match_end;
    rt_string result;

    if (re_find_match(cp, txt_str, text_len, (int)start, &match_start, &match_end)) {
        result = rt_string_from_bytes(txt_str + match_start, match_end - match_start);
    } else {
        result = rt_const_cstr("");
    }
    release_cached_pattern(cp);
    return result;
}

/// @brief Find the first regex match at or after a byte offset as an Option string.
/// @details Negative starts are clamped to zero. A start beyond the text length
///          returns None. Empty matches are preserved as `SomeStr("")`.
/// @param text Text to search.
/// @param pattern Regex pattern string.
/// @param start Starting byte offset.
/// @return Opaque Zanna.Option containing the first match, or None.
void *rt_pattern_find_from_option(rt_string text, rt_string pattern, int64_t start) {
    const char *pat_str = pattern_required(pattern);
    const char *txt_str = pattern_text_or_empty(text);

    int text_len = safe_rt_string_len_int(text);
    if (start < 0)
        start = 0;
    if (start > text_len)
        return rt_option_none();

    compiled_pattern *cp = get_cached_pattern(pat_str);
    int match_start, match_end;
    void *option = NULL;

    if (re_find_match(cp, txt_str, text_len, (int)start, &match_start, &match_end)) {
        rt_string match = rt_string_from_bytes(txt_str + match_start, match_end - match_start);
        option = rt_option_some_str(match);
        rt_str_release_maybe(match);
    } else {
        option = rt_option_none();
    }
    release_cached_pattern(cp);
    return option;
}

/// @brief Find the byte position of the first match (-1 if no match).
/// @param text Borrowed byte string to search, or `NULL` for empty text.
/// @param pattern Borrowed required regex pattern.
/// @return Zero-based starting byte offset, or `-1` when no match exists.
int64_t rt_pattern_find_pos(rt_string text, rt_string pattern) {
    const char *pat_str = pattern_required(pattern);
    const char *txt_str = pattern_text_or_empty(text);

    compiled_pattern *cp = get_cached_pattern(pat_str);
    int match_start, match_end;
    int64_t result = -1;

    if (re_find_match(cp, txt_str, safe_rt_string_len_int(text), 0, &match_start, &match_end))
        result = (int64_t)match_start;
    release_cached_pattern(cp);
    return result;
}

/// @brief Find the byte position of the first regex match as an Option index.
/// @param text Text to search.
/// @param pattern Regex pattern string.
/// @return Opaque Zanna.Option containing the byte index, or None.
void *rt_pattern_find_pos_option(rt_string text, rt_string pattern) {
    const char *pat_str = pattern_required(pattern);
    const char *txt_str = pattern_text_or_empty(text);

    compiled_pattern *cp = get_cached_pattern(pat_str);
    int match_start, match_end;
    void *option = NULL;

    if (re_find_match(cp, txt_str, safe_rt_string_len_int(text), 0, &match_start, &match_end))
        option = rt_option_some_i64((int64_t)match_start);
    else
        option = rt_option_none();
    release_cached_pattern(cp);
    return option;
}

/// @brief Find all non-overlapping matches and return them as a sequence of strings.
/// @details Matches are collected left-to-right. After a zero-width match the
///          search advances by one byte to guarantee progress, while still
///          allowing a final empty match at end-of-text.
/// @param text Borrowed byte string to search, or `NULL` for empty text.
/// @param pattern Borrowed required regex pattern.
/// @return Caller-owned Seq that owns newly allocated match strings.
void *rt_pattern_find_all(rt_string text, rt_string pattern) {
    const char *pat_str = pattern_required(pattern);
    const char *txt_str = pattern_text_or_empty(text);

    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    compiled_pattern *cp = get_cached_pattern(pat_str);
    int text_len = safe_rt_string_len_int(text);
    int pos = 0;

    while (pos <= text_len) {
        int match_start, match_end;
        if (!re_find_match(cp, txt_str, text_len, pos, &match_start, &match_end))
            break;

        rt_string match = rt_string_from_bytes(txt_str + match_start, match_end - match_start);
        rt_seq_push(seq, (void *)match);
        rt_string_unref(match);

        // Move past this match (at least 1 char to avoid infinite loop on empty match)
        pos = match_end > match_start ? match_end : match_start + 1;
    }

    release_cached_pattern(cp);
    return seq;
}

/// @brief Replace all matches of a regex pattern with the replacement string.
/// @details Replacement bytes are inserted literally; capture substitutions
///          are not interpreted. Matches do not overlap. For a zero-width
///          match before a source byte, the replacement is emitted and that
///          byte is copied before search advances, so no text is swallowed.
/// @param text Borrowed byte string to transform, or `NULL` for empty text.
/// @param pattern Borrowed required regex pattern.
/// @param replacement Borrowed literal replacement bytes, or `NULL` for empty.
/// @return Newly allocated transformed string, or an empty string after a
///         recoverable allocation or size trap.
rt_string rt_pattern_replace(rt_string text, rt_string pattern, rt_string replacement) {
    const char *pat_str = pattern_required(pattern);
    const char *txt_str = pattern_text_or_empty(text);
    const char *rep_str = pattern_text_or_empty(replacement);

    compiled_pattern *cp = get_cached_pattern(pat_str);
    int text_len = safe_rt_string_len_int(text);
    int rep_len = safe_rt_string_len_int(replacement);

    // Build result
    size_t result_cap = (size_t)text_len + 64;
    if (result_cap < (size_t)text_len) {
        rt_trap("Pattern: replacement length overflow");
        release_cached_pattern(cp);
        return rt_string_from_bytes("", 0);
    }
    char *result = (char *)malloc(result_cap);
    if (!result) {
        rt_trap("Pattern: memory allocation failed");
        release_cached_pattern(cp);
        return rt_string_from_bytes("", 0);
    }
    size_t result_len = 0;

    int pos = 0;
    while (pos <= text_len) {
        int match_start, match_end;
        if (!re_find_match(cp, txt_str, text_len, pos, &match_start, &match_end)) {
            // Copy rest of text
            size_t remaining = text_len - pos;
            if (!ensure_result_capacity(&result,
                                        &result_cap,
                                        result_len,
                                        remaining,
                                        "Pattern: replacement length overflow")) {
                free(result);
                release_cached_pattern(cp);
                return rt_string_from_bytes("", 0);
            }
            memcpy(result + result_len, txt_str + pos, remaining);
            result_len += remaining;
            break;
        }

        // Copy text before match
        size_t before_len = match_start - pos;
        if ((size_t)rep_len > SIZE_MAX - before_len) {
            free(result);
            release_cached_pattern(cp);
            rt_trap("Pattern: replacement length overflow");
            return rt_string_from_bytes("", 0);
        }
        if (!ensure_result_capacity(&result,
                                    &result_cap,
                                    result_len,
                                    before_len + (size_t)rep_len,
                                    "Pattern: replacement length overflow")) {
            free(result);
            release_cached_pattern(cp);
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
                if (!ensure_result_capacity(&result,
                                            &result_cap,
                                            result_len,
                                            1,
                                            "Pattern: replacement length overflow")) {
                    free(result);
                    release_cached_pattern(cp);
                    return rt_string_from_bytes("", 0);
                }
                result[result_len++] = txt_str[match_start];
            }
            pos = match_start + 1;
        }
    }

    rt_string out = rt_string_from_bytes(result, result_len);
    free(result);
    release_cached_pattern(cp);
    return out;
}

/// @brief Replace only the first match of a regex pattern with the replacement string.
/// @details Replacement bytes are literal. When no match exists, the function
///          returns a newly allocated copy of the source text.
/// @param text Borrowed byte string to transform, or `NULL` for empty text.
/// @param pattern Borrowed required regex pattern.
/// @param replacement Borrowed literal replacement bytes, or `NULL` for empty.
/// @return Newly allocated transformed string.
rt_string rt_pattern_replace_first(rt_string text, rt_string pattern, rt_string replacement) {
    const char *pat_str = pattern_required(pattern);
    const char *txt_str = pattern_text_or_empty(text);
    const char *rep_str = pattern_text_or_empty(replacement);

    compiled_pattern *cp = get_cached_pattern(pat_str);
    int text_len = safe_rt_string_len_int(text);
    int rep_len = safe_rt_string_len_int(replacement);

    int match_start, match_end;
    if (!re_find_match(cp, txt_str, text_len, 0, &match_start, &match_end)) {
        // No match, return original
        rt_string out = rt_string_from_bytes(txt_str, text_len);
        release_cached_pattern(cp);
        return out;
    }

    // Build result: before + replacement + after
    size_t result_len = (size_t)match_start;
    if ((size_t)rep_len > SIZE_MAX - result_len ||
        (size_t)(text_len - match_end) > SIZE_MAX - result_len - (size_t)rep_len) {
        release_cached_pattern(cp);
        rt_trap("Pattern: replacement length overflow");
        return rt_string_from_bytes("", 0);
    }
    result_len += (size_t)rep_len + (size_t)(text_len - match_end);
    if (result_len == SIZE_MAX) {
        release_cached_pattern(cp);
        rt_trap("Pattern: replacement length overflow");
        return rt_string_from_bytes("", 0);
    }
    char *result = (char *)malloc(result_len + 1);
    if (!result) {
        rt_trap("Pattern: memory allocation failed");
        release_cached_pattern(cp);
        return rt_string_from_bytes("", 0);
    }

    memcpy(result, txt_str, match_start);
    memcpy(result + match_start, rep_str, rep_len);
    memcpy(result + match_start + rep_len, txt_str + match_end, text_len - match_end);

    rt_string out = rt_string_from_bytes(result, result_len);
    free(result);
    release_cached_pattern(cp);
    return out;
}

/// @brief Split a string by a regex pattern, returning a sequence of substrings.
/// @details Nonzero-width delimiters produce the segments before, between, and
///          after matches, including an empty trailing segment. Zero-width
///          delimiters never split at the current segment start or at
///          end-of-text; otherwise they split without discarding a source byte.
/// @param text Borrowed byte string to split, or `NULL` for empty text.
/// @param pattern Borrowed required delimiter pattern.
/// @return Caller-owned Seq that owns newly allocated segment strings.
void *rt_pattern_split(rt_string text, rt_string pattern) {
    const char *pat_str = pattern_required(pattern);
    const char *txt_str = pattern_text_or_empty(text);

    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    compiled_pattern *cp = get_cached_pattern(pat_str);
    int text_len = safe_rt_string_len_int(text);
    int pos = 0;

    int seg_start = 0;
    while (pos <= text_len) {
        int match_start, match_end;
        if (!re_find_match(cp, txt_str, text_len, pos, &match_start, &match_end))
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
            }
            pos = match_start + 1;
            continue;
        }

        // Add text before match
        rt_string part = rt_string_from_bytes(txt_str + seg_start, match_start - seg_start);
        rt_seq_push(seq, (void *)part);
        rt_string_unref(part);
        seg_start = match_end;
        pos = match_end;
    }

    // Remaining text (empty when the text ends with a separator).
    rt_string tail = rt_string_from_bytes(txt_str + seg_start, text_len - seg_start);
    rt_seq_push(seq, (void *)tail);
    rt_string_unref(tail);

    release_cached_pattern(cp);
    return seq;
}

/// @brief Escape all regex metacharacters in a string so it matches literally.
/// @details Prefixes backslash, dot, quantifiers, anchors, brackets,
///          parentheses, alternation, and braces with a backslash. A null text
///          is treated as empty.
/// @param text Borrowed byte string to escape, or `NULL`.
/// @return Newly allocated escaped pattern text.
rt_string rt_pattern_escape(rt_string text) {
    const char *txt_str = pattern_text_or_empty(text);

    int text_len = safe_rt_string_len_int(text);

    // Count special characters
    size_t special_count = 0;
    for (int i = 0; i < text_len; i++) {
        char c = txt_str[i];
        if (c == '\\' || c == '.' || c == '*' || c == '+' || c == '?' || c == '^' || c == '$' ||
            c == '[' || c == ']' || c == '(' || c == ')' || c == '|' || c == '{' || c == '}') {
            if (special_count == SIZE_MAX) {
                rt_trap("Pattern: escape length overflow");
                return rt_string_from_bytes("", 0);
            }
            special_count++;
        }
    }

    // Allocate result
    if ((size_t)text_len > SIZE_MAX - special_count) {
        rt_trap("Pattern: escape length overflow");
        return rt_string_from_bytes("", 0);
    }
    size_t result_len = (size_t)text_len + special_count;
    if (result_len == SIZE_MAX) {
        rt_trap("Pattern: escape length overflow");
        return rt_string_from_bytes("", 0);
    }
    char *result = (char *)malloc(result_len + 1);
    if (!result) {
        rt_trap("Pattern: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    size_t j = 0;
    for (int i = 0; i < text_len; i++) {
        char c = txt_str[i];
        if (c == '\\' || c == '.' || c == '*' || c == '+' || c == '?' || c == '^' || c == '$' ||
            c == '[' || c == ']' || c == '(' || c == ')' || c == '|' || c == '{' || c == '}') {
            result[j++] = '\\';
        }
        result[j++] = c;
    }
    result[j] = '\0';

    rt_string out = rt_string_from_bytes(result, result_len);
    free(result);
    return out;
}
