//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTLruCacheTests.cpp
// Purpose: Tests for Zanna.Collections.LruCache runtime helpers.
//
//===----------------------------------------------------------------------===//

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_lrucache.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstring>

namespace {
static jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_trap_expected = false;
static int g_finalizer_calls = 0;
static int g_trapping_finalizer_calls = 0;

enum class ReentryAction {
    None,
    Clear,
    Put,
    Remove,
    Resurrect,
};

static void *g_reentry_cache = nullptr;
static rt_string g_reentry_key = nullptr;
static void *g_reentry_value = nullptr;
static ReentryAction g_reentry_action = ReentryAction::None;
static int g_reentry_calls = 0;
static int8_t g_reentry_remove_result = -1;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    rt_abort(msg);
}

#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = true;                                                                    \
        g_last_trap = nullptr;                                                                     \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            (void)(expr);                                                                          \
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

static void count_finalizer(void *) {
    ++g_finalizer_calls;
}

static void trapping_finalizer(void *) {
    ++g_trapping_finalizer_calls;
    rt_trap("LRUCache test finalizer trap");
}

static void reentrant_finalizer(void *) {
    ++g_reentry_calls;
    switch (g_reentry_action) {
        case ReentryAction::Clear:
            rt_lrucache_clear(g_reentry_cache);
            break;
        case ReentryAction::Put:
            rt_lrucache_put(g_reentry_cache, g_reentry_key, g_reentry_value);
            break;
        case ReentryAction::Remove:
            g_reentry_remove_result = rt_lrucache_remove(g_reentry_cache, g_reentry_key);
            break;
        case ReentryAction::Resurrect:
            rt_obj_resurrect(g_reentry_cache);
            break;
        case ReentryAction::None:
            break;
    }
}

static void reset_reentry_state() {
    g_reentry_cache = nullptr;
    g_reentry_key = nullptr;
    g_reentry_value = nullptr;
    g_reentry_action = ReentryAction::None;
    g_reentry_calls = 0;
    g_reentry_remove_result = -1;
}

static rt_string make_key(const char *text) {
    assert(text != nullptr);
    return rt_string_from_bytes(text, strlen(text));
}

static rt_string make_key_bytes(const char *text, size_t len) {
    return rt_string_from_bytes(text, len);
}

static bool str_eq(rt_string s, const char *expected) {
    const char *cstr = rt_string_cstr(s);
    return cstr && strcmp(cstr, expected) == 0;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_new_cache() {
    void *cache = rt_lrucache_new(10);
    assert(cache != nullptr);
    assert(rt_lrucache_len(cache) == 0);
    assert(rt_lrucache_cap(cache) == 10);
    assert(rt_lrucache_is_empty(cache) == 1);
    rt_release_obj(cache);
}

static void test_put_and_get() {
    void *cache = rt_lrucache_new(5);
    rt_string k1 = make_key("alpha");
    rt_string k2 = make_key("beta");
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_lrucache_put(cache, k1, v1);
    rt_lrucache_put(cache, k2, v2);

    assert(rt_lrucache_len(cache) == 2);
    assert(rt_lrucache_is_empty(cache) == 0);
    assert(rt_lrucache_get(cache, k1) == v1);
    assert(rt_lrucache_get(cache, k2) == v2);

    rt_string missing = make_key("missing");
    assert(rt_lrucache_get(cache, missing) == NULL);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(missing);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(cache);
}

static void test_put_overwrites() {
    void *cache = rt_lrucache_new(5);
    rt_string k1 = make_key("key");
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_lrucache_put(cache, k1, v1);
    assert(rt_lrucache_get(cache, k1) == v1);
    assert(rt_lrucache_len(cache) == 1);

    // Overwrite with new value
    rt_lrucache_put(cache, k1, v2);
    assert(rt_lrucache_get(cache, k1) == v2);
    assert(rt_lrucache_len(cache) == 1); // Count unchanged

    rt_string_unref(k1);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(cache);
}

static void test_eviction() {
    void *cache = rt_lrucache_new(3);
    rt_string k1 = make_key("a");
    rt_string k2 = make_key("b");
    rt_string k3 = make_key("c");
    rt_string k4 = make_key("d");
    void *v1 = new_obj();
    void *v2 = new_obj();
    void *v3 = new_obj();
    void *v4 = new_obj();

    // Fill to capacity
    rt_lrucache_put(cache, k1, v1); // LRU order: a
    rt_lrucache_put(cache, k2, v2); // LRU order: b, a
    rt_lrucache_put(cache, k3, v3); // LRU order: c, b, a
    assert(rt_lrucache_len(cache) == 3);

    // Adding k4 should evict k1 (least recently used)
    rt_lrucache_put(cache, k4, v4); // LRU order: d, c, b (a evicted)
    assert(rt_lrucache_len(cache) == 3);
    assert(rt_lrucache_has(cache, k1) == 0); // Evicted
    assert(rt_lrucache_has(cache, k2) == 1);
    assert(rt_lrucache_has(cache, k3) == 1);
    assert(rt_lrucache_has(cache, k4) == 1);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(k3);
    rt_string_unref(k4);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(v3);
    rt_release_obj(v4);
    rt_release_obj(cache);
}

static void test_get_promotes() {
    void *cache = rt_lrucache_new(3);
    rt_string k1 = make_key("a");
    rt_string k2 = make_key("b");
    rt_string k3 = make_key("c");
    rt_string k4 = make_key("d");
    void *v1 = new_obj();
    void *v2 = new_obj();
    void *v3 = new_obj();
    void *v4 = new_obj();

    rt_lrucache_put(cache, k1, v1); // LRU order: a
    rt_lrucache_put(cache, k2, v2); // LRU order: b, a
    rt_lrucache_put(cache, k3, v3); // LRU order: c, b, a

    // Access k1, promoting it to MRU
    rt_lrucache_get(cache, k1); // LRU order: a, c, b

    // Now k2 is the LRU, so adding k4 should evict k2
    rt_lrucache_put(cache, k4, v4);          // LRU order: d, a, c (b evicted)
    assert(rt_lrucache_has(cache, k1) == 1); // Promoted, not evicted
    assert(rt_lrucache_has(cache, k2) == 0); // Evicted
    assert(rt_lrucache_has(cache, k3) == 1);
    assert(rt_lrucache_has(cache, k4) == 1);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(k3);
    rt_string_unref(k4);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(v3);
    rt_release_obj(v4);
    rt_release_obj(cache);
}

static void test_peek_does_not_promote() {
    void *cache = rt_lrucache_new(3);
    rt_string k1 = make_key("a");
    rt_string k2 = make_key("b");
    rt_string k3 = make_key("c");
    rt_string k4 = make_key("d");
    void *v1 = new_obj();
    void *v2 = new_obj();
    void *v3 = new_obj();
    void *v4 = new_obj();

    rt_lrucache_put(cache, k1, v1); // LRU order: a
    rt_lrucache_put(cache, k2, v2); // LRU order: b, a
    rt_lrucache_put(cache, k3, v3); // LRU order: c, b, a

    // Peek at k1 - should NOT promote it
    assert(rt_lrucache_peek(cache, k1) == v1);
    // k1 is still LRU, so adding k4 should evict k1
    rt_lrucache_put(cache, k4, v4);          // LRU order: d, c, b (a evicted)
    assert(rt_lrucache_has(cache, k1) == 0); // Evicted (peek didn't promote)
    assert(rt_lrucache_has(cache, k2) == 1);
    assert(rt_lrucache_has(cache, k3) == 1);
    assert(rt_lrucache_has(cache, k4) == 1);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(k3);
    rt_string_unref(k4);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(v3);
    rt_release_obj(v4);
    rt_release_obj(cache);
}

static void test_remove() {
    void *cache = rt_lrucache_new(5);
    rt_string k1 = make_key("x");
    rt_string k2 = make_key("y");
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_lrucache_put(cache, k1, v1);
    rt_lrucache_put(cache, k2, v2);
    assert(rt_lrucache_len(cache) == 2);

    assert(rt_lrucache_remove(cache, k1) == 1);
    assert(rt_lrucache_len(cache) == 1);
    assert(rt_lrucache_has(cache, k1) == 0);
    assert(rt_lrucache_has(cache, k2) == 1);

    // Remove non-existent
    assert(rt_lrucache_remove(cache, k1) == 0);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(cache);
}

static void test_remove_oldest() {
    void *cache = rt_lrucache_new(5);
    rt_string k1 = make_key("first");
    rt_string k2 = make_key("second");
    rt_string k3 = make_key("third");
    void *v1 = new_obj();
    void *v2 = new_obj();
    void *v3 = new_obj();

    rt_lrucache_put(cache, k1, v1);
    rt_lrucache_put(cache, k2, v2);
    rt_lrucache_put(cache, k3, v3);

    // k1 is the oldest (LRU)
    assert(rt_lrucache_remove_oldest(cache) == 1);
    assert(rt_lrucache_len(cache) == 2);
    assert(rt_lrucache_has(cache, k1) == 0);
    assert(rt_lrucache_has(cache, k2) == 1);
    assert(rt_lrucache_has(cache, k3) == 1);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(k3);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(v3);
    rt_release_obj(cache);
}

static void test_clear() {
    void *cache = rt_lrucache_new(5);
    rt_string k1 = make_key("a");
    rt_string k2 = make_key("b");
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_lrucache_put(cache, k1, v1);
    rt_lrucache_put(cache, k2, v2);
    assert(rt_lrucache_len(cache) == 2);

    rt_lrucache_clear(cache);
    assert(rt_lrucache_len(cache) == 0);
    assert(rt_lrucache_is_empty(cache) == 1);
    assert(rt_lrucache_has(cache, k1) == 0);
    assert(rt_lrucache_has(cache, k2) == 0);

    // Can still add after clear
    rt_lrucache_put(cache, k1, v1);
    assert(rt_lrucache_len(cache) == 1);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(cache);
}

static void test_keys_and_values_order() {
    void *cache = rt_lrucache_new(5);
    rt_string k1 = make_key("a");
    rt_string k2 = make_key("b");
    rt_string k3 = make_key("c");
    void *v1 = new_obj();
    void *v2 = new_obj();
    void *v3 = new_obj();

    rt_lrucache_put(cache, k1, v1); // LRU order: a
    rt_lrucache_put(cache, k2, v2); // LRU order: b, a
    rt_lrucache_put(cache, k3, v3); // LRU order: c, b, a

    // Keys should be in MRU order: c, b, a
    void *keys = rt_lrucache_keys(cache);
    assert(rt_seq_len(keys) == 3);
    assert(rt_seq_cap(keys) == 3);
    assert(str_eq((rt_string)rt_seq_get(keys, 0), "c"));
    assert(str_eq((rt_string)rt_seq_get(keys, 1), "b"));
    assert(str_eq((rt_string)rt_seq_get(keys, 2), "a"));

    // Values should also be in MRU order: v3, v2, v1
    void *vals = rt_lrucache_values(cache);
    assert(rt_seq_len(vals) == 3);
    assert(rt_seq_cap(vals) == 3);
    assert(rt_seq_get(vals, 0) == v3);
    assert(rt_seq_get(vals, 1) == v2);
    assert(rt_seq_get(vals, 2) == v1);

    rt_release_obj(keys);
    rt_release_obj(vals);
    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(k3);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(v3);
    rt_release_obj(cache);
}

static void test_embedded_nul_keys_are_distinct() {
    void *cache = rt_lrucache_new(5);
    const char bytes[] = {'a', '\0', 'b'};
    rt_string k1 = make_key_bytes(bytes, sizeof(bytes));
    rt_string k2 = make_key("a");
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_lrucache_put(cache, k1, v1);
    rt_lrucache_put(cache, k2, v2);

    assert(rt_lrucache_len(cache) == 2);
    assert(rt_lrucache_get(cache, k1) == v1);
    assert(rt_lrucache_get(cache, k2) == v2);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(cache);
}

static void test_finalizer_on_eviction() {
    void *cache = rt_lrucache_new(2);
    rt_string k1 = make_key("a");
    rt_string k2 = make_key("b");
    rt_string k3 = make_key("c");
    void *v1 = new_obj();
    void *v2 = new_obj();
    void *v3 = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(v1, count_finalizer);

    rt_lrucache_put(cache, k1, v1);
    rt_release_obj(v1); // Cache now owns the only reference

    rt_lrucache_put(cache, k2, v2);
    assert(g_finalizer_calls == 0); // v1 not evicted yet

    // Adding k3 should evict k1 -> v1 finalizer called
    rt_lrucache_put(cache, k3, v3);
    assert(g_finalizer_calls == 1);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(k3);
    rt_release_obj(v2);
    rt_release_obj(v3);
    rt_release_obj(cache);
}

static void test_finalizer_on_cache_free() {
    void *cache = rt_lrucache_new(5);
    rt_string k1 = make_key("a");
    void *v1 = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(v1, count_finalizer);

    rt_lrucache_put(cache, k1, v1);
    rt_release_obj(v1); // Cache now owns the only reference
    assert(g_finalizer_calls == 0);

    rt_string_unref(k1);
    rt_release_obj(cache);
    assert(g_finalizer_calls == 1);
}

static void test_replacement_commits_before_reentrant_clear() {
    void *cache = rt_lrucache_new(2);
    rt_string key = make_key("replace");
    rt_string other_key = make_key("other");
    void *old_value = new_obj();
    void *replacement = new_obj();
    void *other = new_obj();

    rt_obj_set_finalizer(old_value, reentrant_finalizer);
    rt_lrucache_put(cache, key, old_value);
    rt_lrucache_put(cache, other_key, other);
    rt_release_obj(old_value);

    g_reentry_cache = cache;
    g_reentry_action = ReentryAction::Clear;
    rt_lrucache_put(cache, key, replacement);

    assert(g_reentry_calls == 1);
    assert(rt_lrucache_len(cache) == 0);
    assert(rt_lrucache_is_empty(cache) == 1);

    reset_reentry_state();
    rt_string_unref(key);
    rt_string_unref(other_key);
    rt_release_obj(replacement);
    rt_release_obj(other);
    rt_release_obj(cache);
}

static void test_eviction_commits_new_entry_before_reentrant_put() {
    void *cache = rt_lrucache_new(1);
    rt_string victim_key = make_key("victim");
    rt_string outer_key = make_key("outer");
    rt_string reentry_key = make_key("reentry");
    void *victim = new_obj();
    void *outer = new_obj();
    void *reentry = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_lrucache_put(cache, victim_key, victim);
    rt_release_obj(victim);

    g_reentry_cache = cache;
    g_reentry_key = reentry_key;
    g_reentry_value = reentry;
    g_reentry_action = ReentryAction::Put;
    rt_lrucache_put(cache, outer_key, outer);

    assert(g_reentry_calls == 1);
    assert(rt_lrucache_len(cache) == 1);
    assert(rt_lrucache_has(cache, victim_key) == 0);
    assert(rt_lrucache_has(cache, outer_key) == 0);
    assert(rt_lrucache_get(cache, reentry_key) == reentry);

    reset_reentry_state();
    rt_string_unref(victim_key);
    rt_string_unref(outer_key);
    rt_string_unref(reentry_key);
    rt_release_obj(outer);
    rt_release_obj(reentry);
    rt_release_obj(cache);
}

static void test_clear_preserves_reentrant_put() {
    void *cache = rt_lrucache_new(2);
    rt_string key = make_key("same-key");
    void *victim = new_obj();
    void *inserted = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_lrucache_put(cache, key, victim);
    rt_release_obj(victim);

    g_reentry_cache = cache;
    g_reentry_key = key;
    g_reentry_value = inserted;
    g_reentry_action = ReentryAction::Put;
    rt_lrucache_clear(cache);

    assert(g_reentry_calls == 1);
    assert(rt_lrucache_len(cache) == 1);
    assert(rt_lrucache_get(cache, key) == inserted);

    reset_reentry_state();
    rt_lrucache_clear(cache);
    rt_release_obj(inserted);
    rt_string_unref(key);
    rt_release_obj(cache);
}

static void test_owner_finalizer_blocks_reentrant_put() {
    void *cache = rt_lrucache_new(2);
    rt_string key = make_key("owner-finalizer");
    void *victim = new_obj();
    void *inserted = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_lrucache_put(cache, key, victim);
    rt_release_obj(victim);

    g_reentry_cache = cache;
    g_reentry_key = key;
    g_reentry_value = inserted;
    g_reentry_action = ReentryAction::Put;
    rt_release_obj(cache);

    rt_heap_info_t inserted_info;
    assert(g_reentry_calls == 1);
    assert(rt_heap_get_info(inserted, &inserted_info) == 1);
    assert(inserted_info.refcnt == 1);

    reset_reentry_state();
    rt_release_obj(inserted);
    rt_string_unref(key);
}

static void test_resurrected_owner_is_empty_reusable_and_rearmed() {
    void *cache = rt_lrucache_new(2);
    rt_string key = make_key("resurrect");
    void *victim = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_lrucache_put(cache, key, victim);
    rt_release_obj(victim);

    g_reentry_cache = cache;
    g_reentry_action = ReentryAction::Resurrect;
    rt_release_obj(cache);

    assert(g_reentry_calls == 1);
    assert(rt_lrucache_len(cache) == 0);
    assert(rt_lrucache_cap(cache) == 2);
    reset_reentry_state();

    g_finalizer_calls = 0;
    void *second = new_obj();
    rt_obj_set_finalizer(second, count_finalizer);
    rt_lrucache_put(cache, key, second);
    rt_release_obj(second);
    assert(rt_lrucache_get(cache, key) == second);

    rt_release_obj(cache);
    assert(g_finalizer_calls == 1);
    rt_string_unref(key);
}

static void test_finalizer_drains_after_value_finalizer_trap() {
    void *cache = rt_lrucache_new(2);
    rt_string first_key = make_key("first");
    rt_string second_key = make_key("second");
    void *first = new_obj();
    void *second = new_obj();

    g_trapping_finalizer_calls = 0;
    g_finalizer_calls = 0;
    rt_obj_set_finalizer(first, trapping_finalizer);
    rt_obj_set_finalizer(second, count_finalizer);
    rt_lrucache_put(cache, second_key, second);
    rt_lrucache_put(cache, first_key, first);
    rt_release_obj(first);
    rt_release_obj(second);

    EXPECT_TRAP(rt_release_obj(cache));
    assert(g_last_trap != nullptr);
    assert(g_trapping_finalizer_calls == 1);
    assert(g_finalizer_calls == 1);

    rt_string_unref(first_key);
    rt_string_unref(second_key);
}

static void test_large_eviction_limit_does_not_preallocate_limit() {
#if SIZE_MAX >= INT64_MAX
    void *cache = rt_lrucache_new(INT64_MAX);
    assert(cache != nullptr);
    assert(rt_lrucache_cap(cache) == INT64_MAX);
    assert(rt_lrucache_len(cache) == 0);
    rt_release_obj(cache);
#endif
}

static void test_null_safety() {
    rt_string k = make_key("test");

    // All functions should handle NULL gracefully
    assert(rt_lrucache_len(NULL) == 0);
    assert(rt_lrucache_cap(NULL) == 0);
    assert(rt_lrucache_is_empty(NULL) == 1);
    assert(rt_lrucache_get(NULL, k) == NULL);
    assert(rt_lrucache_peek(NULL, k) == NULL);
    assert(rt_lrucache_has(NULL, k) == 0);
    assert(rt_lrucache_remove(NULL, k) == 0);
    assert(rt_lrucache_remove_oldest(NULL) == 0);
    rt_lrucache_put(NULL, k, NULL); // No-op, should not crash
    rt_lrucache_clear(NULL);        // No-op, should not crash

    rt_string_unref(k);
}

static void test_capacity_one() {
    void *cache = rt_lrucache_new(1);
    rt_string k1 = make_key("a");
    rt_string k2 = make_key("b");
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_lrucache_put(cache, k1, v1);
    assert(rt_lrucache_len(cache) == 1);
    assert(rt_lrucache_get(cache, k1) == v1);

    // Adding k2 should evict k1
    rt_lrucache_put(cache, k2, v2);
    assert(rt_lrucache_len(cache) == 1);
    assert(rt_lrucache_has(cache, k1) == 0);
    assert(rt_lrucache_get(cache, k2) == v2);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(cache);
}

static void test_capacity_zero_is_unbounded() {
    void *cache = rt_lrucache_new(0);
    rt_string k1 = make_key("a");
    rt_string k2 = make_key("b");
    rt_string k3 = make_key("c");
    void *v1 = new_obj();
    void *v2 = new_obj();
    void *v3 = new_obj();

    assert(rt_lrucache_cap(cache) == 0);
    rt_lrucache_put(cache, k1, v1);
    rt_lrucache_put(cache, k2, v2);
    rt_lrucache_put(cache, k3, v3);

    assert(rt_lrucache_len(cache) == 3);
    assert(rt_lrucache_has(cache, k1) == 1);
    assert(rt_lrucache_has(cache, k2) == 1);
    assert(rt_lrucache_has(cache, k3) == 1);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(k3);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(v3);
    rt_release_obj(cache);
}

static void test_negative_capacity_traps() {
    EXPECT_TRAP(rt_lrucache_new(-1));
    assert(g_last_trap && strstr(g_last_trap, "LRUCache") != nullptr);
}

static void test_remove_oldest_on_empty() {
    void *cache = rt_lrucache_new(5);
    assert(rt_lrucache_remove_oldest(cache) == 0);
    rt_release_obj(cache);
}

int main() {
    test_new_cache();
    test_put_and_get();
    test_put_overwrites();
    test_eviction();
    test_get_promotes();
    test_peek_does_not_promote();
    test_remove();
    test_remove_oldest();
    test_clear();
    test_keys_and_values_order();
    test_embedded_nul_keys_are_distinct();
    test_finalizer_on_eviction();
    test_finalizer_on_cache_free();
    test_replacement_commits_before_reentrant_clear();
    test_eviction_commits_new_entry_before_reentrant_put();
    test_clear_preserves_reentrant_put();
    test_owner_finalizer_blocks_reentrant_put();
    test_resurrected_owner_is_empty_reusable_and_rearmed();
    test_finalizer_drains_after_value_finalizer_trap();
    test_large_eviction_limit_does_not_preallocate_limit();
    test_null_safety();
    test_capacity_one();
    test_capacity_zero_is_unbounded();
    test_negative_capacity_traps();
    test_remove_oldest_on_empty();
    return 0;
}
