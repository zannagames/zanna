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
/// @return `TRUE` after initializing the critical section.
static BOOL WINAPI init_pattern_cache_cs(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o;
    (void)p;
    (void)ctx;
    InitializeCriticalSection(&g_pattern_cache_cs);
    return TRUE;
}

/// @brief Acquire the pattern-cache mutex (Windows path).
///
/// Lazily initializes the critical section on first call. Required
/// per S-12: concurrent regex calls must not race on the LRU cache.
static void pattern_cache_lock(void) {
    InitOnceExecuteOnce(&g_pattern_cache_cs_once, init_pattern_cache_cs, NULL, NULL);
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

/// @brief Measure a C string for the engine's signed-int length domain.
/// @param s Null-terminated string to measure.
/// @return String length as `int`; traps and returns zero for null input or a
///         length greater than `INT_MAX`.
static int safe_strlen_int(const char *s) {
    if (!s) {
        rt_trap("Pattern: null string");
        return 0;
    }
    size_t n = strlen(s);
    if (n > (size_t)INT_MAX) {
        rt_trap("Pattern: string too long for regex engine");
        return 0;
    }
    return (int)n;
}

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

/// @brief Allocate a zero-initialized AST node of the given type.
///
/// Uses `calloc` so the union starts cleared (important for the
/// `children` variant where `count`/`capacity` must be 0). Traps on
/// OOM — there's no recovery path during pattern compile.
/// @param type AST node discriminator to initialize.
/// @return Newly allocated node, or `NULL` after an allocation trap.
re_node *node_new(re_node_type type) {
    re_node *n = (re_node *)calloc(1, sizeof(re_node));
    if (!n) {
        rt_trap("Pattern: memory allocation failed");
        return NULL;
    }
    n->type = type;
    n->group_index = -1;
    return n;
}

/// @brief Recursively free an AST node and all its descendants.
///
/// Walks the tree depth-first: container types (concat/alt/group) free
/// each child then their `children` array; quantifier nodes free their
/// single child; leaf types just free themselves. Safe on NULL.
/// @param n Root of the owned AST subtree to destroy.
void node_free(re_node *n) {
    if (!n)
        return;

    switch (n->type) {
        case RE_CONCAT:
        case RE_ALT:
        case RE_GROUP:
            for (int i = 0; i < n->data.children.count; i++) {
                node_free(n->data.children.children[i]);
            }
            free(n->data.children.children);
            break;
        case RE_QUANT:
            node_free(n->data.quant.child);
            break;
        default:
            break;
    }
    free(n);
}

/// @brief Append a child node to a container (concat/alt/group).
///
/// Geometric resize (cap doubles, starting at 4) so amortized cost is
/// O(1) per add. Traps on OOM. Caller transfers ownership of `child`
/// to `n` — the parent's `node_free` will reclaim it.
/// @param n Destination container node.
/// @param child Child node whose ownership transfers on successful append.
void children_add(re_node *n, re_node *child) {
    if (!n || !child) {
        rt_trap("Pattern: invalid child node");
        return;
    }
    if (n->data.children.count >= n->data.children.capacity) {
        if (n->data.children.capacity > INT_MAX / 2) {
            rt_trap("Pattern: too many child nodes");
            return;
        }
        int new_cap = n->data.children.capacity == 0 ? 4 : n->data.children.capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(re_node *)) {
            rt_trap("Pattern: child node allocation overflow");
            return;
        }
        re_node **new_children =
            (re_node **)realloc(n->data.children.children, new_cap * sizeof(re_node *));
        if (!new_children) {
            rt_trap("Pattern: memory allocation failed");
            return;
        }
        n->data.children.children = new_children;
        n->data.children.capacity = new_cap;
    }
    n->data.children.children[n->data.children.count++] = child;
}

/// @brief Free a compiled pattern and its AST.
///
/// Releases the duplicated pattern string, recursively frees the AST
/// root, then frees the wrapper. Safe on NULL — used both by the
/// cache eviction path and by error-recovery paths during compile.
/// @param p Owned compiled-pattern object to destroy.
static void pattern_free(compiled_pattern *p) {
    if (!p)
        return;
    free(p->pattern_str);
    node_free(p->root);
    free(p);
}

/// @brief Public free entry point exposed via `rt_regex_internal.h`.
///
/// Thin wrapper around `pattern_free` so external callers (e.g., the
/// cached-pattern wrapper) don't need to see the static helper.
/// @param cp Owned compiled pattern to destroy; may be `NULL`.
void re_free(re_compiled_pattern *cp) {
    pattern_free(cp);
}

//=============================================================================
// Character Class Helpers
//=============================================================================

/// @brief Set bit `ch` in a character-class bitset (no-op out of range).
///
/// The bitset is 256 bits (32 bytes), one per ASCII byte value. Bytes
/// outside [0, 255] are ignored — matching of multibyte/Unicode chars
/// happens via the negation flag in `class_test`.
/// @param c Character-class bitmap to modify.
/// @param ch Byte value to add.
void class_set(re_class *c, int ch) {
    if (ch >= 0 && ch < 256) {
        c->bits[ch / 8] |= (1 << (ch % 8));
    }
}

/// @brief Test whether `ch` is in the class (after applying negation).
///
/// Bytes outside [0, 255] match if and only if the class is negated —
/// preserves the "negated class accepts everything not explicitly listed"
/// semantics for arbitrary code units.
/// @param c Character class to inspect.
/// @param ch Candidate byte value.
/// @return Whether @p ch belongs to the effective class.
bool class_test(const re_class *c, int ch) {
    if (ch < 0 || ch >= 256)
        return c->negated;
    bool in_class = (c->bits[ch / 8] & (1 << (ch % 8))) != 0;
    return c->negated ? !in_class : in_class;
}

/// @brief Set every bit in the inclusive range `[from, to]`.
/// @details Values at or above 256 are ignored; a reversed range adds nothing.
/// @param c Character-class bitmap to modify.
/// @param from Inclusive first byte value.
/// @param to Inclusive final byte value.
void class_add_range(re_class *c, int from, int to) {
    for (int ch = from; ch <= to && ch < 256; ch++) {
        class_set(c, ch);
    }
}

/// @brief Return 1 when `ch` is in the base set of a lowercase shorthand.
/// @param shorthand Lowercase shorthand discriminator: `d`, `w`, or `s`.
/// @param ch Candidate byte value.
/// @return `1` for membership in the ASCII shorthand set, otherwise `0`.
static int shorthand_member(char shorthand, int ch) {
    switch (shorthand) {
        case 'd':
            return ch >= '0' && ch <= '9';
        case 'w':
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '_';
        case 's':
            return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' ||
                   ch == '\v';
        default:
            return 0;
    }
}

/// @brief Apply a `\\d`/`\\D`/`\\w`/`\\W`/`\\s`/`\\S` shorthand to a class.
///
/// Lowercase shorthands union the matching bytes into the class bitmap;
/// uppercase variants union the complement bytes directly. Unioning (rather
/// than toggling the class-wide `negated` flag) keeps mixed classes like
/// `[a\\D]` correct: complementing one member must not complement the whole
/// class (VDOC-055). Used both inside `[...]` brackets and as standalone
/// atoms.
/// @param c Character-class bitmap to extend.
/// @param shorthand One of `d`, `D`, `w`, `W`, `s`, or `S`.
void class_add_shorthand(re_class *c, char shorthand) {
    char base = shorthand;
    int complement = 0;
    if (shorthand >= 'A' && shorthand <= 'Z') {
        base = (char)(shorthand - 'A' + 'a');
        complement = 1;
    }
    for (int ch = 0; ch < 256; ch++) {
        if (shorthand_member(base, ch) != complement)
            class_set(c, ch);
    }
}

/// @brief Count `RE_GROUP` nodes in a subtree (excluding the implicit group 0).
///
/// Used after parse to populate `cp->group_count` so callers can size
/// match-result arrays correctly. Recurses through every container
/// kind so nested groups are tallied.
/// @param n Root of the AST subtree to inspect.
/// @return Number of explicit capture-group nodes below @p n.
static int count_groups(re_node *n) {
    if (!n)
        return 0;

    int count = 0;
    switch (n->type) {
        case RE_GROUP:
            count = 1; // This group
            for (int i = 0; i < n->data.children.count; i++) {
                count += count_groups(n->data.children.children[i]);
            }
            break;
        case RE_CONCAT:
        case RE_ALT:
            for (int i = 0; i < n->data.children.count; i++) {
                count += count_groups(n->data.children.children[i]);
            }
            break;
        case RE_QUANT:
            count = count_groups(n->data.quant.child);
            break;
        default:
            break;
    }
    return count;
}

/// @brief Top-level compile: parse `pattern` into a `compiled_pattern`.
///
/// Allocates the wrapper, duplicates the pattern source for diagnostics
/// and cache lookups, runs the parser, and counts capture groups. Traps
/// on syntax error (via `parse_error`) or OOM. Empty patterns are
/// represented as an empty concat (matches everywhere with zero width).
/// @param pattern Required null-terminated pattern source.
/// @return Newly allocated compiled pattern, or `NULL` after a syntax or
///         allocation trap.
static compiled_pattern *compile_pattern(const char *pattern) {
    if (!pattern) {
        rt_trap("Pattern: null pattern");
        return NULL;
    }

    compiled_pattern *cp = (compiled_pattern *)calloc(1, sizeof(compiled_pattern));
    if (!cp) {
        rt_trap("Pattern: memory allocation failed");
        return NULL;
    }

    cp->pattern_str = strdup(pattern);
    if (!cp->pattern_str) {
        free(cp);
        rt_trap("Pattern: memory allocation failed");
        return NULL;
    }

    parser_state p = {pattern, 0, safe_strlen_int(pattern), 0};

    cp->root = parse_alternation(&p);

    if (!at_end(&p)) {
        pattern_free(cp);
        parse_error(&p, "unexpected character");
        return NULL;
    }

    // Handle empty pattern
    if (!cp->root) {
        cp->root = node_new(RE_CONCAT);
    }

    // Count capture groups
    cp->group_count = count_groups(cp->root);

    return cp;
}

/// @brief Public compile entry point — exposed via `rt_regex_internal.h`.
///
/// Wraps the static `compile_pattern` so external callers (the cached
/// pattern wrapper) don't need access to the static helper.
/// @param pattern Required null-terminated pattern source.
/// @return Newly allocated compiled pattern, or `NULL` after a trap.
re_compiled_pattern *re_compile(const char *pattern) {
    return compile_pattern(pattern);
}

/// @brief Return the source pattern string a compiled pattern was built from.
///
/// Returns "" for NULL. Useful for cache lookup and diagnostics.
/// @param cp Borrowed compiled pattern, or `NULL`.
/// @return Borrowed original pattern text, or a stable empty string.
const char *re_get_pattern(re_compiled_pattern *cp) {
    return cp ? cp->pattern_str : "";
}

/// @brief Return the number of capture groups in the compiled pattern.
///
/// Counts only explicit `(...)` groups; group 0 (the whole match) is
/// not included. Returns 0 for NULL.
/// @param cp Borrowed compiled pattern, or `NULL`.
/// @return Number of explicit capture groups.
int re_group_count(re_compiled_pattern *cp) {
    return cp ? cp->group_count : 0;
}

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
    compiled_pattern *cp = compile_pattern(pattern_str);
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
            pattern_free(cp);
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
        pattern_free(pattern_cache[slot].pattern);
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
        pattern_free(cp);
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
                rt_string part =
                    rt_string_from_bytes(txt_str + seg_start, match_start - seg_start);
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
