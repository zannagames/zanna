//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTTrieTests.cpp
// Purpose: Tests for Zanna.Collections.Trie runtime helpers.
// Key invariants: Prefix mutation preserves exact key/value membership and
//                 allocation failure never publishes a partial path.
// Ownership/Lifetime: Tests release every Trie and managed value after clearing
//                     trap-recovery state.
// Links: src/runtime/collections/rt_trie.c
//
//===----------------------------------------------------------------------===//

#include "rt_box.h"
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trie.h"

#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstring>

namespace {
static jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_trap_expected = false;
static bool g_return_traps = false;
static int g_returned_trap_count = 0;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    if (g_return_traps) {
        ++g_returned_trap_count;
        return;
    }
    rt_abort(msg);
}

#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = true;                                                                    \
        g_last_trap = nullptr;                                                                     \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            expr;                                                                                  \
            assert(false && "Expected trap did not occur");                                        \
        }                                                                                          \
        g_trap_expected = false;                                                                   \
    } while (0)

static void rt_release_obj(void *p) {
    if (p && rt_obj_release_check0(p))
        rt_obj_free(p);
}

static void *new_obj() {
    void *p = rt_obj_new_i64(0, 8);
    assert(p != nullptr);
    return p;
}

static rt_string make_key(const char *text) {
    return rt_string_from_bytes(text, strlen(text));
}

static rt_string make_key_bytes(const char *bytes, size_t len) {
    return rt_string_from_bytes(bytes, len);
}

static bool str_eq(rt_string s, const char *expected) {
    const char *cstr = rt_string_cstr(s);
    return cstr && strcmp(cstr, expected) == 0;
}

static bool str_bytes_eq(rt_string s, const char *expected, size_t len) {
    const char *data = rt_string_cstr(s);
    return data && rt_str_len(s) == (int64_t)len && memcmp(data, expected, len) == 0;
}

static void test_new() {
    void *t = rt_trie_new();
    assert(t != nullptr);
    assert(rt_trie_len(t) == 0);
    assert(rt_trie_is_empty(t) == 1);
    rt_release_obj(t);
}

static void test_put_and_get() {
    void *t = rt_trie_new();
    rt_string k1 = make_key("hello");
    rt_string k2 = make_key("help");
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_trie_set(t, k1, v1);
    rt_trie_set(t, k2, v2);

    assert(rt_trie_len(t) == 2);
    assert(rt_trie_get(t, k1) == v1);
    assert(rt_trie_get(t, k2) == v2);

    rt_string missing = make_key("missing");
    assert(rt_trie_get(t, missing) == NULL);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(missing);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(t);
}

static void test_has() {
    void *t = rt_trie_new();
    rt_string k = make_key("apple");
    void *v = new_obj();

    rt_trie_set(t, k, v);
    assert(rt_trie_has(t, k) == 1);

    rt_string nope = make_key("app");
    assert(rt_trie_has(t, nope) == 0); // "app" is a prefix, not a complete key

    rt_string_unref(k);
    rt_string_unref(nope);
    rt_release_obj(v);
    rt_release_obj(t);
}

static void test_overwrite() {
    void *t = rt_trie_new();
    rt_string k = make_key("key");
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_trie_set(t, k, v1);
    assert(rt_trie_get(t, k) == v1);
    assert(rt_trie_len(t) == 1);

    rt_trie_set(t, k, v2);
    assert(rt_trie_get(t, k) == v2);
    assert(rt_trie_len(t) == 1); // Count unchanged

    rt_string_unref(k);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(t);
}

static void test_has_prefix() {
    void *t = rt_trie_new();
    rt_string k1 = make_key("apple");
    rt_string k2 = make_key("application");
    rt_string k3 = make_key("banana");
    void *v = new_obj();

    rt_trie_set(t, k1, v);
    rt_trie_set(t, k2, v);
    rt_trie_set(t, k3, v);

    rt_string prefix = make_key("app");
    assert(rt_trie_has_prefix(t, prefix) == 1);

    rt_string no_prefix = make_key("cherry");
    assert(rt_trie_has_prefix(t, no_prefix) == 0);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(k3);
    rt_string_unref(prefix);
    rt_string_unref(no_prefix);
    rt_release_obj(v);
    rt_release_obj(t);
}

static void test_with_prefix() {
    void *t = rt_trie_new();
    rt_string k1 = make_key("apple");
    rt_string k2 = make_key("application");
    rt_string k3 = make_key("apply");
    rt_string k4 = make_key("banana");
    void *v = new_obj();

    rt_trie_set(t, k1, v);
    rt_trie_set(t, k2, v);
    rt_trie_set(t, k3, v);
    rt_trie_set(t, k4, v);

    rt_string prefix = make_key("app");
    void *results = rt_trie_with_prefix(t, prefix);
    assert(rt_seq_len(results) == 3);
    // Results should be in sorted order (lexicographic)

    rt_release_obj(results);
    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(k3);
    rt_string_unref(k4);
    rt_string_unref(prefix);
    rt_release_obj(v);
    rt_release_obj(t);
}

static void test_longest_prefix() {
    void *t = rt_trie_new();
    void *v = new_obj();

    rt_string k1 = make_key("a");
    rt_string k2 = make_key("ab");
    rt_string k3 = make_key("abc");
    rt_string k4 = make_key("abcdef");

    rt_trie_set(t, k1, v);
    rt_trie_set(t, k2, v);
    rt_trie_set(t, k3, v);
    rt_trie_set(t, k4, v);

    rt_string query = make_key("abcde");
    rt_string result = rt_trie_longest_prefix(t, query);
    assert(str_eq(result, "abc")); // "abcdef" doesn't match, but "abc" does
    rt_string_unref(result);

    rt_string query2 = make_key("xyz");
    rt_string result2 = rt_trie_longest_prefix(t, query2);
    assert(str_eq(result2, ""));
    rt_string_unref(result2);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(k3);
    rt_string_unref(k4);
    rt_string_unref(query);
    rt_string_unref(query2);
    rt_release_obj(v);
    rt_release_obj(t);
}

static void test_remove() {
    void *t = rt_trie_new();
    rt_string k1 = make_key("hello");
    rt_string k2 = make_key("help");
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_trie_set(t, k1, v1);
    rt_trie_set(t, k2, v2);
    assert(rt_trie_len(t) == 2);

    assert(rt_trie_remove(t, k1) == 1);
    assert(rt_trie_len(t) == 1);
    assert(rt_trie_has(t, k1) == 0);
    assert(rt_trie_has(t, k2) == 1);

    assert(rt_trie_remove(t, k1) == 0); // Already removed

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(t);
}

/// @brief Verify removal pruning preserves terminal ancestors and shared paths.
/// @details Removes keys from longest to shortest along one branch, checks a
///          sibling branch throughout, then reinserts the fully pruned path to
///          prove structural reclamation leaves the trie reusable.
static void test_remove_prunes_only_unshared_suffix() {
    void *trie = rt_trie_new();
    void *value = new_obj();
    rt_string a = make_key("a");
    rt_string ab = make_key("ab");
    rt_string abc = make_key("abc");
    rt_string ax = make_key("ax");

    rt_trie_set(trie, a, value);
    rt_trie_set(trie, ab, value);
    rt_trie_set(trie, abc, value);
    rt_trie_set(trie, ax, value);

    assert(rt_trie_remove(trie, abc) == 1);
    assert(rt_trie_has(trie, ab) == 1);
    assert(rt_trie_has(trie, ax) == 1);
    assert(rt_trie_remove(trie, ab) == 1);
    assert(rt_trie_has(trie, a) == 1);
    assert(rt_trie_has(trie, ax) == 1);
    assert(rt_trie_remove(trie, a) == 1);
    assert(rt_trie_has(trie, ax) == 1);

    rt_trie_set(trie, abc, value);
    assert(rt_trie_has(trie, abc) == 1);

    rt_string_unref(a);
    rt_string_unref(ab);
    rt_string_unref(abc);
    rt_string_unref(ax);
    rt_release_obj(value);
    rt_release_obj(trie);
}

static void test_clear() {
    void *t = rt_trie_new();
    rt_string k = make_key("test");
    void *v = new_obj();

    rt_trie_set(t, k, v);
    rt_trie_clear(t);

    assert(rt_trie_len(t) == 0);
    assert(rt_trie_is_empty(t) == 1);
    assert(rt_trie_has(t, k) == 0);

    rt_string_unref(k);
    rt_release_obj(v);
    rt_release_obj(t);
}

static void test_keys() {
    void *t = rt_trie_new();
    void *v = new_obj();
    rt_string k1 = make_key("banana");
    rt_string k2 = make_key("apple");
    rt_string k3 = make_key("cherry");

    rt_trie_set(t, k1, v);
    rt_trie_set(t, k2, v);
    rt_trie_set(t, k3, v);

    void *keys = rt_trie_keys(t);
    assert(rt_seq_len(keys) == 3);
    // Trie traversal produces lexicographic order
    assert(str_eq((rt_string)rt_seq_get(keys, 0), "apple"));
    assert(str_eq((rt_string)rt_seq_get(keys, 1), "banana"));
    assert(str_eq((rt_string)rt_seq_get(keys, 2), "cherry"));

    rt_release_obj(keys);
    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(k3);
    rt_release_obj(v);
    rt_release_obj(t);
}

static void test_empty_key() {
    void *t = rt_trie_new();
    rt_string k = make_key("");
    void *v = new_obj();

    rt_trie_set(t, k, v);
    assert(rt_trie_len(t) == 1);
    assert(rt_trie_has(t, k) == 1);
    assert(rt_trie_get(t, k) == v);

    rt_string_unref(k);
    rt_release_obj(v);
    rt_release_obj(t);
}

static void test_set_retain_overflow_leaves_key_absent() {
    void *t = rt_trie_new();
    rt_string k = make_key("boom");
    void *value = new_obj();

    rt_heap_hdr_t *hdr = rt_heap_hdr(value);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    EXPECT_TRAP(rt_trie_set(t, k, value));
    assert(rt_trie_len(t) == 0);
    assert(rt_trie_has(t, k) == 0);
    assert(rt_trie_get(t, k) == NULL);

    hdr->refcnt = 1;
    rt_string_unref(k);
    rt_release_obj(value);
    rt_release_obj(t);
}

static void test_clone_has_independent_structure_and_retained_values() {
    void *source = rt_trie_new();
    void *value = new_obj();
    rt_string alpha = make_key("alpha");
    rt_string alpine = make_key("alpine");
    rt_trie_set(source, alpha, value);
    rt_trie_set(source, alpine, value);

    void *clone = rt_trie_clone(source);
    assert(clone != nullptr);
    assert(rt_trie_len(clone) == 2);
    assert(rt_trie_get(clone, alpha) == value);
    assert(rt_trie_get(clone, alpine) == value);

    assert(rt_trie_remove(source, alpha) == 1);
    assert(rt_trie_has(source, alpha) == 0);
    assert(rt_trie_has(clone, alpha) == 1);
    rt_trie_clear(clone);
    assert(rt_trie_len(clone) == 0);
    assert(rt_trie_has(source, alpine) == 1);

    rt_string_unref(alpha);
    rt_string_unref(alpine);
    rt_release_obj(value);
    rt_release_obj(clone);
    rt_release_obj(source);
}

static void test_clone_retain_overflow_keeps_source_unchanged() {
    void *t = rt_trie_new();
    rt_string k = make_key("clone-boom");
    void *value = new_obj();

    rt_trie_set(t, k, value);
    rt_heap_hdr_t *hdr = rt_heap_hdr(value);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
    int64_t tracked_before = rt_gc_tracked_count();

    EXPECT_TRAP((void)rt_trie_clone(t));
    assert(rt_trie_len(t) == 1);
    assert(rt_trie_has(t, k) == 1);
    assert(rt_trie_get(t, k) == value);
    assert(hdr->refcnt == RT_HEAP_MAX_MORTAL_REFCNT);
    assert(rt_gc_tracked_count() == tracked_before);

    hdr->refcnt = 2;
    rt_string_unref(k);
    rt_release_obj(value);
    rt_release_obj(t);
}

/// @brief Invalid nonnull string and object handles must not alias valid state.
/// @details A returning embedder trap hook exercises every key family without
///          non-local control transfer. The forged key must never address the
///          empty-key terminal, and checked casts must return safe defaults.
static void test_returning_traps_reject_invalid_handles() {
    void *trie = rt_trie_new();
    void *empty_value = new_obj();
    void *replacement = new_obj();
    rt_string empty = make_key("");
    rt_trie_set(trie, empty, empty_value);

    rt_string forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(0x13579U));
    void *wrong_object = new_obj();
    g_returned_trap_count = 0;
    g_return_traps = true;

    rt_trie_set(trie, forged, replacement);
    assert(rt_trie_len(trie) == 1);
    assert(rt_trie_get(trie, empty) == empty_value);
    assert(rt_trie_get(trie, forged) == nullptr);
    assert(rt_trie_has(trie, forged) == 0);
    assert(rt_trie_has_prefix(trie, forged) == 0);
    assert(rt_trie_remove(trie, forged) == 0);
    assert(rt_trie_with_prefix(trie, forged) == nullptr);
    assert(rt_trie_longest_prefix(trie, forged) == nullptr);
    assert(rt_trie_longest_prefix_option(trie, forged) == nullptr);

    assert(rt_trie_len(wrong_object) == 0);
    assert(rt_trie_clone(wrong_object) == nullptr);
    void *wrong_keys = rt_trie_keys(wrong_object);
    assert(wrong_keys != nullptr);
    assert(rt_seq_len(wrong_keys) == 0);

    g_return_traps = false;
    assert(g_returned_trap_count == 11);
    assert(g_last_trap != nullptr);

    rt_release_obj(wrong_keys);
    rt_release_obj(wrong_object);
    rt_string_unref(empty);
    rt_release_obj(replacement);
    rt_release_obj(empty_value);
    rt_release_obj(trie);
}

static void test_embedded_nul_keys() {
    void *t = rt_trie_new();
    void *v = new_obj();
    const char key_bytes[] = {'a', '\0', 'b'};
    const char prefix_bytes[] = {'a', '\0'};
    const char query_bytes[] = {'a', '\0', 'b', 'x'};
    rt_string key = make_key_bytes(key_bytes, sizeof(key_bytes));
    rt_string prefix = make_key_bytes(prefix_bytes, sizeof(prefix_bytes));
    rt_string query = make_key_bytes(query_bytes, sizeof(query_bytes));

    rt_trie_set(t, key, v);
    assert(rt_trie_len(t) == 1);
    assert(rt_trie_has(t, key) == 1);
    assert(rt_trie_get(t, key) == v);
    assert(rt_trie_has_prefix(t, prefix) == 1);

    void *matches = rt_trie_with_prefix(t, prefix);
    assert(rt_seq_len(matches) == 1);
    assert(str_bytes_eq((rt_string)rt_seq_get(matches, 0), key_bytes, sizeof(key_bytes)));

    rt_string longest = rt_trie_longest_prefix(t, query);
    assert(str_bytes_eq(longest, key_bytes, sizeof(key_bytes)));
    assert(rt_trie_remove(t, key) == 1);
    assert(rt_trie_len(t) == 0);

    rt_release_obj(matches);
    rt_string_unref(longest);
    rt_string_unref(key);
    rt_string_unref(prefix);
    rt_string_unref(query);
    rt_release_obj(v);
    rt_release_obj(t);
}

static void test_null_safety() {
    rt_string k = make_key("test");
    assert(rt_trie_len(NULL) == 0);
    assert(rt_trie_is_empty(NULL) == 1);
    assert(rt_trie_get(NULL, k) == NULL);
    assert(rt_trie_has(NULL, k) == 0);
    assert(rt_trie_has_prefix(NULL, k) == 0);
    assert(rt_trie_remove(NULL, k) == 0);
    rt_trie_set(NULL, k, NULL);
    rt_trie_clear(NULL);
    rt_string_unref(k);
}

static void test_longest_prefix_option() {
    // VDOC-103: the Option form distinguishes an empty-key match from no match.
    void *trie = rt_trie_new();
    rt_string empty = rt_string_from_bytes("", 0);
    rt_string probe = rt_string_from_bytes("zzz", 3);

    void *none = rt_trie_longest_prefix_option(trie, probe);
    assert(rt_option_is_none(none) == 1);

    rt_trie_set(trie, empty, rt_box_i64(1));
    void *some = rt_trie_longest_prefix_option(trie, probe);
    assert(rt_option_is_some(some) == 1);
    rt_string match = rt_option_unwrap_str(some);
    assert(rt_str_len(match) == 0);

    rt_string_unref(empty);
    rt_string_unref(probe);
}

int main() {
    test_longest_prefix_option();
    test_new();
    test_put_and_get();
    test_has();
    test_overwrite();
    test_has_prefix();
    test_with_prefix();
    test_longest_prefix();
    test_remove();
    test_remove_prunes_only_unshared_suffix();
    test_clear();
    test_keys();
    test_empty_key();
    test_set_retain_overflow_leaves_key_absent();
    test_clone_has_independent_structure_and_retained_values();
    test_clone_retain_overflow_keeps_source_unchanged();
    test_returning_traps_reject_invalid_handles();
    test_embedded_nul_keys();
    test_null_safety();
    return 0;
}
