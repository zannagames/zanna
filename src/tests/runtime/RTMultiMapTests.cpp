//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTMultiMapTests.cpp
// Purpose: Tests for Zanna.Collections.MultiMap runtime helpers.
// Key invariants:
//   - Reentrant finalizers cannot corrupt multimap ownership or value groups.
//   - Test-owned runtime objects and strings are released after each case.
// Ownership/Lifetime:
//   - Runtime heap objects use the production reference-counting contract.
//   - Global trap and reentry state is reset before it can escape a test case.
// Links: src/runtime/collections/rt_multimap.c, src/runtime/collections/rt_multimap.h
//
//===----------------------------------------------------------------------===//

#include "rt_hash_util.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_multimap.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstring>

extern "C" void rt_trap_set_recovery(jmp_buf *buf);
extern "C" void rt_trap_clear_recovery(void);
extern "C" const char *rt_trap_get_error(void);

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

static void rt_release_obj(void *p) {
    if (p && rt_obj_release_check0(p))
        rt_obj_free(p);
}

static void *new_obj() {
    void *p = rt_obj_new_i64(0, 8);
    assert(p != nullptr);
    return p;
}

static int g_finalizer_calls = 0;
static int g_trapping_finalizer_calls = 0;

enum class ReentryAction {
    None,
    Clear,
    Put,
    RemoveAll,
    Resurrect,
};

static void *g_reentry_map = nullptr;
static rt_string g_reentry_key = nullptr;
static void *g_reentry_value = nullptr;
static ReentryAction g_reentry_action = ReentryAction::None;
static int g_reentry_calls = 0;
static int8_t g_reentry_remove_result = -1;

static void count_finalizer(void *) {
    ++g_finalizer_calls;
}

static void trapping_finalizer(void *) {
    ++g_trapping_finalizer_calls;
    rt_trap("MultiMap test finalizer trap");
}

static void reentrant_finalizer(void *) {
    ++g_reentry_calls;
    switch (g_reentry_action) {
        case ReentryAction::Clear:
            rt_multimap_clear(g_reentry_map);
            break;
        case ReentryAction::Put:
            rt_multimap_put(g_reentry_map, g_reentry_key, g_reentry_value);
            break;
        case ReentryAction::RemoveAll:
            g_reentry_remove_result = rt_multimap_remove_all(g_reentry_map, g_reentry_key);
            break;
        case ReentryAction::Resurrect:
            rt_obj_resurrect(g_reentry_map);
            break;
        case ReentryAction::None:
            break;
    }
}

static void reset_reentry_state() {
    g_reentry_map = nullptr;
    g_reentry_key = nullptr;
    g_reentry_value = nullptr;
    g_reentry_action = ReentryAction::None;
    g_reentry_calls = 0;
    g_reentry_remove_result = -1;
}

static rt_string make_key(const char *text) {
    return rt_string_from_bytes(text, strlen(text));
}

static rt_string make_key_bytes(const char *text, size_t len) {
    return rt_string_from_bytes(text, len);
}

static void test_new() {
    void *mm = rt_multimap_new();
    assert(mm != nullptr);
    assert(rt_multimap_len(mm) == 0);
    assert(rt_multimap_key_count(mm) == 0);
    assert(rt_multimap_is_empty(mm) == 1);
    rt_release_obj(mm);
}

static void test_put_and_get() {
    void *mm = rt_multimap_new();
    rt_string k = make_key("color");
    void *v1 = new_obj();
    void *v2 = new_obj();
    void *v3 = new_obj();

    rt_multimap_put(mm, k, v1);
    rt_multimap_put(mm, k, v2);
    rt_multimap_put(mm, k, v3);

    assert(rt_multimap_len(mm) == 3);
    assert(rt_multimap_key_count(mm) == 1);
    assert(rt_multimap_has(mm, k) == 1);
    assert(rt_multimap_count_for(mm, k) == 3);

    void *vals = rt_multimap_get(mm, k);
    assert(rt_seq_len(vals) == 3);
    assert(rt_seq_cap(vals) == 3);
    assert(rt_seq_get(vals, 0) == v1);
    assert(rt_seq_get(vals, 1) == v2);
    assert(rt_seq_get(vals, 2) == v3);

    assert(rt_multimap_get_first(mm, k) == v1);

    rt_release_obj(vals);
    rt_string_unref(k);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(v3);
    rt_release_obj(mm);
}

static void test_multiple_keys() {
    void *mm = rt_multimap_new();
    rt_string k1 = make_key("fruit");
    rt_string k2 = make_key("veggie");
    void *v1 = new_obj();
    void *v2 = new_obj();
    void *v3 = new_obj();

    rt_multimap_put(mm, k1, v1);
    rt_multimap_put(mm, k1, v2);
    rt_multimap_put(mm, k2, v3);

    assert(rt_multimap_len(mm) == 3);
    assert(rt_multimap_key_count(mm) == 2);
    assert(rt_multimap_count_for(mm, k1) == 2);
    assert(rt_multimap_count_for(mm, k2) == 1);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(v3);
    rt_release_obj(mm);
}

static void test_remove_all() {
    void *mm = rt_multimap_new();
    rt_string k = make_key("key");
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_multimap_put(mm, k, v1);
    rt_multimap_put(mm, k, v2);
    assert(rt_multimap_len(mm) == 2);

    assert(rt_multimap_remove_all(mm, k) == 1);
    assert(rt_multimap_len(mm) == 0);
    assert(rt_multimap_key_count(mm) == 0);
    assert(rt_multimap_has(mm, k) == 0);

    // Remove non-existent
    assert(rt_multimap_remove_all(mm, k) == 0);

    rt_string_unref(k);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(mm);
}

static void test_clear() {
    void *mm = rt_multimap_new();
    rt_string k1 = make_key("a");
    rt_string k2 = make_key("b");
    void *v = new_obj();

    rt_multimap_put(mm, k1, v);
    rt_multimap_put(mm, k2, v);
    rt_multimap_clear(mm);

    assert(rt_multimap_len(mm) == 0);
    assert(rt_multimap_key_count(mm) == 0);
    assert(rt_multimap_is_empty(mm) == 1);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_release_obj(v);
    rt_release_obj(mm);
}

static void test_keys() {
    void *mm = rt_multimap_new();
    rt_string k1 = make_key("x");
    rt_string k2 = make_key("y");
    void *v = new_obj();

    rt_multimap_put(mm, k1, v);
    rt_multimap_put(mm, k2, v);

    void *keys = rt_multimap_keys(mm);
    assert(rt_seq_len(keys) == 2);
    assert(rt_seq_cap(keys) == 2);

    rt_release_obj(keys);
    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_release_obj(v);
    rt_release_obj(mm);
}

static void test_get_missing_returns_empty_seq() {
    void *mm = rt_multimap_new();
    rt_string k = make_key("missing");
    void *vals = rt_multimap_get(mm, k);
    assert(rt_seq_len(vals) == 0);
    assert(rt_multimap_get_first(mm, k) == NULL);

    rt_release_obj(vals);
    rt_string_unref(k);
    rt_release_obj(mm);
}

static void test_values_are_retained() {
    void *mm = rt_multimap_new();
    rt_string k = make_key("owned");
    void *value = new_obj();
    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_multimap_put(mm, k, value);
    rt_release_obj(value);
    assert(g_finalizer_calls == 0);

    assert(rt_multimap_remove_all(mm, k) == 1);
    assert(g_finalizer_calls == 1);

    rt_string_unref(k);
    rt_release_obj(mm);
}

static void test_remove_all_commits_before_reentrant_clear() {
    void *mm = rt_multimap_new();
    rt_string victim_key = make_key("victim");
    rt_string other_key = make_key("other");
    void *victim = new_obj();
    void *other = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_multimap_put(mm, victim_key, victim);
    rt_multimap_put(mm, other_key, other);
    rt_release_obj(victim);
    rt_release_obj(other);

    g_reentry_map = mm;
    g_reentry_action = ReentryAction::Clear;
    assert(rt_multimap_remove_all(mm, victim_key) == 1);
    assert(g_reentry_calls == 1);
    assert(rt_multimap_len(mm) == 0);
    assert(rt_multimap_key_count(mm) == 0);

    reset_reentry_state();
    rt_string_unref(victim_key);
    rt_string_unref(other_key);
    rt_release_obj(mm);
}

static void test_clear_preserves_reentrant_put() {
    void *mm = rt_multimap_new();
    rt_string key = make_key("same-key");
    void *victim = new_obj();
    void *inserted = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_multimap_put(mm, key, victim);
    rt_release_obj(victim);

    g_reentry_map = mm;
    g_reentry_key = key;
    g_reentry_value = inserted;
    g_reentry_action = ReentryAction::Put;
    rt_multimap_clear(mm);

    assert(g_reentry_calls == 1);
    assert(rt_multimap_len(mm) == 1);
    assert(rt_multimap_key_count(mm) == 1);
    assert(rt_multimap_count_for(mm, key) == 1);

    reset_reentry_state();
    rt_multimap_clear(mm);
    rt_release_obj(inserted);
    rt_string_unref(key);
    rt_release_obj(mm);
}

static void test_owner_finalizer_blocks_reentrant_put() {
    void *mm = rt_multimap_new();
    rt_string key = make_key("owner-finalizer");
    void *victim = new_obj();
    void *inserted = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_multimap_put(mm, key, victim);
    rt_release_obj(victim);

    g_reentry_map = mm;
    g_reentry_key = key;
    g_reentry_value = inserted;
    g_reentry_action = ReentryAction::Put;
    rt_release_obj(mm);

    [[maybe_unused]] rt_heap_info_t inserted_info;
    assert(g_reentry_calls == 1);
    assert(rt_heap_get_info(inserted, &inserted_info) == 1);
    assert(inserted_info.refcnt == 1);

    reset_reentry_state();
    rt_release_obj(inserted);
    rt_string_unref(key);
}

static void test_resurrected_owner_is_empty_reusable_and_rearmed() {
    void *mm = rt_multimap_new();
    rt_string key = make_key("resurrect");
    void *victim = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_multimap_put(mm, key, victim);
    rt_release_obj(victim);

    g_reentry_map = mm;
    g_reentry_action = ReentryAction::Resurrect;
    rt_release_obj(mm);

    assert(g_reentry_calls == 1);
    assert(rt_multimap_len(mm) == 0);
    assert(rt_multimap_key_count(mm) == 0);
    reset_reentry_state();

    g_finalizer_calls = 0;
    void *second = new_obj();
    rt_obj_set_finalizer(second, count_finalizer);
    rt_multimap_put(mm, key, second);
    rt_release_obj(second);
    assert(rt_multimap_count_for(mm, key) == 1);

    rt_release_obj(mm);
    assert(g_finalizer_calls == 1);
    rt_string_unref(key);
}

static void find_low_and_high_bucket_keys(char *low,
                                          size_t low_size,
                                          char *high,
                                          size_t high_size) {
    size_t low_bucket = 16;
    size_t high_bucket = 0;
    for (int i = 0; i < 4096; ++i) {
        char candidate[32];
        snprintf(candidate, sizeof(candidate), "cleanup-%d", i);
        size_t bucket = (size_t)(rt_keyed_hash_bytes(candidate, strlen(candidate)) % 16);
        if (bucket < low_bucket) {
            snprintf(low, low_size, "%s", candidate);
            low_bucket = bucket;
        }
        if (bucket > high_bucket) {
            snprintf(high, high_size, "%s", candidate);
            high_bucket = bucket;
        }
        if (low_bucket == 0 && high_bucket == 15)
            break;
    }
    assert(low_bucket < high_bucket);
}

static void test_finalizer_drains_after_value_finalizer_trap() {
    char low_text[32] = {0};
    char high_text[32] = {0};
    find_low_and_high_bucket_keys(low_text, sizeof(low_text), high_text, sizeof(high_text));

    void *mm = rt_multimap_new();
    rt_string count_key = make_key(low_text);
    rt_string trap_key = make_key(high_text);
    void *counted = new_obj();
    void *trapping = new_obj();

    g_finalizer_calls = 0;
    g_trapping_finalizer_calls = 0;
    rt_obj_set_finalizer(counted, count_finalizer);
    rt_obj_set_finalizer(trapping, trapping_finalizer);
    rt_multimap_put(mm, count_key, counted);
    rt_multimap_put(mm, trap_key, trapping);
    rt_release_obj(counted);
    rt_release_obj(trapping);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_release_obj(mm);
        assert(false && "trapping value finalizer should propagate");
    } else {
        const char *error = rt_trap_get_error();
        assert(error != nullptr);
        assert(strstr(error, "MultiMap test finalizer trap") != nullptr);
        rt_trap_clear_recovery();
    }

    assert(g_trapping_finalizer_calls == 1);
    assert(g_finalizer_calls == 1);
    rt_string_unref(count_key);
    rt_string_unref(trap_key);
}

static void test_embedded_nul_keys_are_distinct() {
    void *mm = rt_multimap_new();
    const char bytes[] = {'a', '\0', 'b'};
    rt_string k1 = make_key_bytes(bytes, sizeof(bytes));
    rt_string k2 = make_key("a");
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_multimap_put(mm, k1, v1);
    rt_multimap_put(mm, k2, v2);

    assert(rt_multimap_key_count(mm) == 2);
    assert(rt_multimap_get_first(mm, k1) == v1);
    assert(rt_multimap_get_first(mm, k2) == v2);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(mm);
}

static void test_null_safety() {
    rt_string k = make_key("test");
    assert(rt_multimap_len(NULL) == 0);
    assert(rt_multimap_key_count(NULL) == 0);
    assert(rt_multimap_is_empty(NULL) == 1);
    assert(rt_multimap_has(NULL, k) == 0);
    assert(rt_multimap_count_for(NULL, k) == 0);
    assert(rt_multimap_get_first(NULL, k) == NULL);
    assert(rt_multimap_remove_all(NULL, k) == 0);
    rt_multimap_put(NULL, k, NULL);
    rt_multimap_clear(NULL);
    rt_string_unref(k);
}

int main() {
    test_new();
    test_put_and_get();
    test_multiple_keys();
    test_remove_all();
    test_clear();
    test_keys();
    test_get_missing_returns_empty_seq();
    test_values_are_retained();
    test_remove_all_commits_before_reentrant_clear();
    test_clear_preserves_reentrant_put();
    test_owner_finalizer_blocks_reentrant_put();
    test_resurrected_owner_is_empty_reusable_and_rearmed();
    test_finalizer_drains_after_value_finalizer_trap();
    test_embedded_nul_keys_are_distinct();
    test_null_safety();
    return 0;
}
