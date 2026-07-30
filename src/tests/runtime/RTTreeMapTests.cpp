//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTTreeMapTests.cpp
// Purpose: Tests for Zanna.Collections.SortedMap sorted key-value storage.
//
//===----------------------------------------------------------------------===//

#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_treemap.h"

#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" void rt_trap_set_recovery(jmp_buf *buf);
extern "C" void rt_trap_clear_recovery(void);
extern "C" const char *rt_trap_get_error(void);

namespace {
static int g_finalizer_calls = 0;
static int g_trapping_finalizer_calls = 0;
enum class ReentryAction {
    None,
    Clear,
    Set,
    Remove,
    Resurrect,
};
static void *g_reentry_map = nullptr;
static rt_string g_reentry_key = nullptr;
static void *g_reentry_value = nullptr;
static ReentryAction g_reentry_action = ReentryAction::None;
static int g_reentry_calls = 0;
static int8_t g_reentry_remove_result = -1;
} // namespace

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

// ============================================================================
// Helper
// ============================================================================

static rt_string make_str(const char *s) {
    return rt_const_cstr(s);
}

static rt_string make_bytes(const char *s, size_t len) {
    return rt_string_from_bytes(s, len);
}

static const char *str_cstr(rt_string s) {
    return rt_string_cstr(s);
}

/// Create a simple test object with 8 bytes payload
static void *new_test_obj() {
    void *p = rt_obj_new_i64(0, 8);
    assert(p != nullptr);
    return p;
}

static void release_obj(void *p) {
    if (p && rt_obj_release_check0(p))
        rt_obj_free(p);
}

static void count_finalizer(void *) {
    ++g_finalizer_calls;
}

static void trapping_finalizer(void *) {
    ++g_trapping_finalizer_calls;
    rt_trap("TreeMap test finalizer trap");
}

static void reentrant_finalizer(void *) {
    ++g_reentry_calls;
    switch (g_reentry_action) {
        case ReentryAction::Clear:
            rt_treemap_clear(g_reentry_map);
            break;
        case ReentryAction::Set:
            rt_treemap_set(g_reentry_map, g_reentry_key, g_reentry_value);
            break;
        case ReentryAction::Remove:
            g_reentry_remove_result = rt_treemap_remove(g_reentry_map, g_reentry_key);
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

// ============================================================================
// Basic Creation Tests
// ============================================================================

static void test_new_treemap() {
    void *tm = rt_treemap_new();
    assert(tm != nullptr);
    assert(rt_treemap_len(tm) == 0);
    assert(rt_treemap_is_empty(tm) == 1);

    printf("test_new_treemap: PASSED\n");
}

// ============================================================================
// Set/Get/Has Tests
// ============================================================================

static void test_set_get() {
    void *tm = rt_treemap_new();

    // Create some test values
    void *val1 = new_test_obj();
    void *val2 = new_test_obj();
    void *val3 = new_test_obj();

    // Set values
    rt_treemap_set(tm, make_str("banana"), val1);
    rt_treemap_set(tm, make_str("apple"), val2);
    rt_treemap_set(tm, make_str("cherry"), val3);

    assert(rt_treemap_len(tm) == 3);
    assert(rt_treemap_is_empty(tm) == 0);

    // Get values
    void *got1 = rt_treemap_get(tm, make_str("banana"));
    void *got2 = rt_treemap_get(tm, make_str("apple"));
    void *got3 = rt_treemap_get(tm, make_str("cherry"));
    void *got4 = rt_treemap_get(tm, make_str("durian")); // not found

    assert(got1 == val1);
    assert(got2 == val2);
    assert(got3 == val3);
    assert(got4 == nullptr);

    printf("test_set_get: PASSED\n");
}

static void test_has() {
    void *tm = rt_treemap_new();

    rt_treemap_set(tm, make_str("key1"), new_test_obj());
    rt_treemap_set(tm, make_str("key2"), new_test_obj());

    assert(rt_treemap_has(tm, make_str("key1")) == 1);
    assert(rt_treemap_has(tm, make_str("key2")) == 1);
    assert(rt_treemap_has(tm, make_str("key3")) == 0);

    printf("test_has: PASSED\n");
}

static void test_update() {
    void *tm = rt_treemap_new();

    void *val1 = new_test_obj();
    void *val2 = new_test_obj();

    rt_treemap_set(tm, make_str("key"), val1);
    assert(rt_treemap_get(tm, make_str("key")) == val1);
    assert(rt_treemap_len(tm) == 1);

    // Update same key
    rt_treemap_set(tm, make_str("key"), val2);
    assert(rt_treemap_get(tm, make_str("key")) == val2);
    assert(rt_treemap_len(tm) == 1); // Still 1

    printf("test_update: PASSED\n");
}

static void test_update_releases_old_value() {
    void *tm = rt_treemap_new();
    void *old_val = new_test_obj();
    void *new_val = new_test_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(old_val, count_finalizer);

    rt_treemap_set(tm, make_str("key"), old_val);
    release_obj(old_val); // TreeMap now owns the only reference.
    assert(g_finalizer_calls == 0);

    rt_treemap_set(tm, make_str("key"), new_val);
    assert(rt_treemap_get(tm, make_str("key")) == new_val);
    assert(g_finalizer_calls == 1);

    release_obj(new_val);
    release_obj(tm);
}

static void test_embedded_nul_keys_are_distinct() {
    void *tm = rt_treemap_new();
    const char bytes[] = {'a', '\0', 'b'};
    rt_string k1 = make_bytes(bytes, sizeof(bytes));
    rt_string k2 = make_str("a");
    void *v1 = new_test_obj();
    void *v2 = new_test_obj();

    rt_treemap_set(tm, k1, v1);
    rt_treemap_set(tm, k2, v2);

    assert(rt_treemap_len(tm) == 2);
    assert(rt_treemap_get(tm, k1) == v1);
    assert(rt_treemap_get(tm, k2) == v2);

    rt_string_unref(k1);
    release_obj(v1);
    release_obj(v2);
    release_obj(tm);
}

// ============================================================================
// Drop/Clear Tests
// ============================================================================

static void test_drop() {
    void *tm = rt_treemap_new();

    rt_treemap_set(tm, make_str("a"), new_test_obj());
    rt_treemap_set(tm, make_str("b"), new_test_obj());
    rt_treemap_set(tm, make_str("c"), new_test_obj());

    assert(rt_treemap_len(tm) == 3);

    // Remove existing key
    assert(rt_treemap_remove(tm, make_str("b")) == 1);
    assert(rt_treemap_len(tm) == 2);
    assert(rt_treemap_has(tm, make_str("b")) == 0);

    // Remove non-existing key
    assert(rt_treemap_remove(tm, make_str("x")) == 0);
    assert(rt_treemap_len(tm) == 2);

    printf("test_remove: PASSED\n");
}

static void test_clear() {
    void *tm = rt_treemap_new();

    rt_treemap_set(tm, make_str("a"), new_test_obj());
    rt_treemap_set(tm, make_str("b"), new_test_obj());
    rt_treemap_set(tm, make_str("c"), new_test_obj());

    assert(rt_treemap_len(tm) == 3);

    rt_treemap_clear(tm);

    assert(rt_treemap_len(tm) == 0);
    assert(rt_treemap_is_empty(tm) == 1);

    printf("test_clear: PASSED\n");
}

static void test_remove_commits_before_reentrant_finalizer() {
    void *tm = rt_treemap_new();
    void *victim = new_test_obj();
    void *other = new_test_obj();
    rt_string victim_key = make_str("victim");
    rt_string other_key = make_str("other");

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_treemap_set(tm, victim_key, victim);
    rt_treemap_set(tm, other_key, other);
    release_obj(victim);

    g_reentry_map = tm;
    g_reentry_action = ReentryAction::Clear;
    assert(rt_treemap_remove(tm, victim_key) == 1);
    assert(g_reentry_calls == 1);
    assert(rt_treemap_len(tm) == 0);

    reset_reentry_state();
    release_obj(other);
    release_obj(tm);
}

static void test_clear_detaches_array_before_reentrant_finalizer() {
    void *tm = rt_treemap_new();
    void *victim = new_test_obj();
    void *inserted = new_test_obj();
    rt_string key = make_str("same-key");

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_treemap_set(tm, key, victim);
    release_obj(victim);

    g_reentry_map = tm;
    g_reentry_key = key;
    g_reentry_value = inserted;
    g_reentry_action = ReentryAction::Set;
    rt_treemap_clear(tm);

    assert(g_reentry_calls == 1);
    assert(rt_treemap_len(tm) == 1);
    assert(rt_treemap_get(tm, key) == inserted);

    reset_reentry_state();
    rt_treemap_clear(tm);
    release_obj(inserted);
    release_obj(tm);
}

static void test_owner_finalizer_blocks_reentrant_mutation() {
    void *tm = rt_treemap_new();
    void *victim = new_test_obj();
    rt_string key = make_str("owner-finalizer");

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_treemap_set(tm, key, victim);
    release_obj(victim);

    g_reentry_map = tm;
    g_reentry_key = key;
    g_reentry_action = ReentryAction::Remove;
    release_obj(tm);

    assert(g_reentry_calls == 1);
    assert(g_reentry_remove_result == 0);
    reset_reentry_state();
}

static void test_resurrected_owner_is_empty_reusable_and_rearmed() {
    void *tm = rt_treemap_new();
    void *victim = new_test_obj();
    rt_string key = make_str("resurrect");

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_treemap_set(tm, key, victim);
    release_obj(victim);

    g_reentry_map = tm;
    g_reentry_action = ReentryAction::Resurrect;
    release_obj(tm);

    assert(g_reentry_calls == 1);
    assert(rt_treemap_len(tm) == 0);
    assert(rt_treemap_is_empty(tm) == 1);
    reset_reentry_state();

    g_finalizer_calls = 0;
    void *second = new_test_obj();
    rt_obj_set_finalizer(second, count_finalizer);
    rt_treemap_set(tm, key, second);
    release_obj(second);
    assert(rt_treemap_get(tm, key) == second);

    release_obj(tm);
    assert(g_finalizer_calls == 1);
}

static void test_finalizer_drains_after_value_finalizer_trap() {
    void *tm = rt_treemap_new();
    void *first = new_test_obj();
    void *second = new_test_obj();

    g_trapping_finalizer_calls = 0;
    g_finalizer_calls = 0;
    rt_obj_set_finalizer(first, trapping_finalizer);
    rt_obj_set_finalizer(second, count_finalizer);
    rt_treemap_set(tm, make_str("first"), first);
    rt_treemap_set(tm, make_str("second"), second);
    release_obj(first);
    release_obj(second);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        release_obj(tm);
        assert(false && "trapping value finalizer should propagate");
    } else {
        const char *error = rt_trap_get_error();
        assert(error != nullptr);
        assert(strstr(error, "TreeMap test finalizer trap") != nullptr);
        rt_trap_clear_recovery();
    }

    assert(g_trapping_finalizer_calls == 1);
    assert(g_finalizer_calls == 1);
}

// ============================================================================
// Keys/Values Tests (sorted order)
// ============================================================================

static void test_keys_sorted() {
    void *tm = rt_treemap_new();

    // Insert in non-sorted order
    rt_treemap_set(tm, make_str("cherry"), new_test_obj());
    rt_treemap_set(tm, make_str("apple"), new_test_obj());
    rt_treemap_set(tm, make_str("banana"), new_test_obj());
    rt_treemap_set(tm, make_str("date"), new_test_obj());

    void *keys = rt_treemap_keys(tm);
    assert(rt_seq_len(keys) == 4);

    // Keys should be in sorted order
    // Keys() pushes rt_string directly to Seq
    rt_string k0 = (rt_string)rt_seq_get(keys, 0);
    rt_string k1 = (rt_string)rt_seq_get(keys, 1);
    rt_string k2 = (rt_string)rt_seq_get(keys, 2);
    rt_string k3 = (rt_string)rt_seq_get(keys, 3);

    assert(strcmp(str_cstr(k0), "apple") == 0);
    assert(strcmp(str_cstr(k1), "banana") == 0);
    assert(strcmp(str_cstr(k2), "cherry") == 0);
    assert(strcmp(str_cstr(k3), "date") == 0);

    printf("test_keys_sorted: PASSED\n");
}

static void test_values_sorted() {
    void *tm = rt_treemap_new();

    // Insert with known values to track order
    void *valA = new_test_obj();
    void *valB = new_test_obj();
    void *valC = new_test_obj();

    // Insert in non-sorted key order
    rt_treemap_set(tm, make_str("cherry"), valC);
    rt_treemap_set(tm, make_str("apple"), valA);
    rt_treemap_set(tm, make_str("banana"), valB);

    void *values = rt_treemap_values(tm);
    assert(rt_seq_len(values) == 3);

    // Values should be in key-sorted order: apple, banana, cherry
    void *v0 = rt_seq_get(values, 0);
    void *v1 = rt_seq_get(values, 1);
    void *v2 = rt_seq_get(values, 2);

    assert(v0 == valA);
    assert(v1 == valB);
    assert(v2 == valC);

    printf("test_values_sorted: PASSED\n");
}

// ============================================================================
// First/Last Tests
// ============================================================================

static void test_first_last() {
    void *tm = rt_treemap_new();

    // Empty map
    rt_string first_empty = rt_treemap_first(tm);
    rt_string last_empty = rt_treemap_last(tm);
    assert(strcmp(str_cstr(first_empty), "") == 0);
    assert(strcmp(str_cstr(last_empty), "") == 0);

    // Add entries
    rt_treemap_set(tm, make_str("cherry"), new_test_obj());
    rt_treemap_set(tm, make_str("apple"), new_test_obj());
    rt_treemap_set(tm, make_str("banana"), new_test_obj());

    rt_string first = rt_treemap_first(tm);
    rt_string last = rt_treemap_last(tm);

    assert(strcmp(str_cstr(first), "apple") == 0);
    assert(strcmp(str_cstr(last), "cherry") == 0);

    printf("test_first_last: PASSED\n");
}

// ============================================================================
// Floor/Ceil Tests
// ============================================================================

static void test_floor() {
    void *tm = rt_treemap_new();

    rt_treemap_set(tm, make_str("apple"), new_test_obj());
    rt_treemap_set(tm, make_str("cherry"), new_test_obj());
    rt_treemap_set(tm, make_str("elderberry"), new_test_obj());

    // Exact match
    rt_string f1 = rt_treemap_floor(tm, make_str("cherry"));
    assert(strcmp(str_cstr(f1), "cherry") == 0);

    // Between keys - should get lower key
    rt_string f2 = rt_treemap_floor(tm, make_str("banana"));
    assert(strcmp(str_cstr(f2), "apple") == 0);

    rt_string f3 = rt_treemap_floor(tm, make_str("date"));
    assert(strcmp(str_cstr(f3), "cherry") == 0);

    // Higher than all keys
    rt_string f4 = rt_treemap_floor(tm, make_str("zebra"));
    assert(strcmp(str_cstr(f4), "elderberry") == 0);

    // Lower than all keys - no floor
    rt_string f5 = rt_treemap_floor(tm, make_str("aardvark"));
    assert(strcmp(str_cstr(f5), "") == 0);

    printf("test_floor: PASSED\n");
}

static void test_ceil() {
    void *tm = rt_treemap_new();

    rt_treemap_set(tm, make_str("apple"), new_test_obj());
    rt_treemap_set(tm, make_str("cherry"), new_test_obj());
    rt_treemap_set(tm, make_str("elderberry"), new_test_obj());

    // Exact match
    rt_string c1 = rt_treemap_ceil(tm, make_str("cherry"));
    assert(strcmp(str_cstr(c1), "cherry") == 0);

    // Between keys - should get higher key
    rt_string c2 = rt_treemap_ceil(tm, make_str("banana"));
    assert(strcmp(str_cstr(c2), "cherry") == 0);

    rt_string c3 = rt_treemap_ceil(tm, make_str("date"));
    assert(strcmp(str_cstr(c3), "elderberry") == 0);

    // Lower than all keys - should get first key
    rt_string c4 = rt_treemap_ceil(tm, make_str("aardvark"));
    assert(strcmp(str_cstr(c4), "apple") == 0);

    // Higher than all keys - no ceiling
    rt_string c5 = rt_treemap_ceil(tm, make_str("zebra"));
    assert(strcmp(str_cstr(c5), "") == 0);

    printf("test_ceil: PASSED\n");
}

// ============================================================================
// Edge Cases
// ============================================================================

static void test_empty_key() {
    void *tm = rt_treemap_new();

    rt_treemap_set(tm, make_str(""), new_test_obj());
    assert(rt_treemap_has(tm, make_str("")) == 1);
    assert(rt_treemap_len(tm) == 1);

    printf("test_empty_key: PASSED\n");
}

static void test_null_value() {
    void *tm = rt_treemap_new();

    rt_treemap_set(tm, make_str("key"), nullptr);
    assert(rt_treemap_has(tm, make_str("key")) == 1);
    assert(rt_treemap_get(tm, make_str("key")) == nullptr);

    printf("test_null_value: PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("=== Zanna.Collections.SortedMap Tests ===\n\n");

    // Basic creation
    test_new_treemap();

    // Set/Get/Has
    test_set_get();
    test_has();
    test_update();
    test_update_releases_old_value();
    test_embedded_nul_keys_are_distinct();

    // Drop/Clear
    test_drop();
    test_clear();
    test_remove_commits_before_reentrant_finalizer();
    test_clear_detaches_array_before_reentrant_finalizer();
    test_owner_finalizer_blocks_reentrant_mutation();
    test_resurrected_owner_is_empty_reusable_and_rearmed();
    test_finalizer_drains_after_value_finalizer_trap();

    // Keys/Values (sorted)
    test_keys_sorted();
    test_values_sorted();

    // First/Last
    test_first_last();

    // Floor/Ceil
    test_floor();
    test_ceil();

    // Edge cases
    test_empty_key();
    test_null_value();

    printf("\nAll RTTreeMapTests passed!\n");
    return 0;
}
