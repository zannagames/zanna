//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_orderedmap.h"
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

static rt_string make_str(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static rt_string make_bytes(const char *s, size_t len) {
    return rt_string_from_bytes(s, len);
}

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

static void count_finalizer(void *) {
    ++g_finalizer_calls;
}

static void trapping_finalizer(void *) {
    ++g_trapping_finalizer_calls;
    rt_trap("OrderedMap test finalizer trap");
}

static void reentrant_finalizer(void *) {
    ++g_reentry_calls;
    switch (g_reentry_action) {
        case ReentryAction::Clear:
            rt_orderedmap_clear(g_reentry_map);
            break;
        case ReentryAction::Set:
            rt_orderedmap_set(g_reentry_map, g_reentry_key, g_reentry_value);
            break;
        case ReentryAction::Remove:
            g_reentry_remove_result = rt_orderedmap_remove(g_reentry_map, g_reentry_key);
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

static void *new_obj() {
    void *p = rt_obj_new_i64(0, 8);
    assert(p != nullptr);
    return p;
}

static void release_obj(void *p) {
    if (p && rt_obj_release_check0(p))
        rt_obj_free(p);
}

static bool str_eq(rt_string s, const char *expected) {
    if (!s)
        return false;
    const char *cstr = rt_string_cstr(s);
    return cstr && strcmp(cstr, expected) == 0;
}

static void test_new_empty() {
    void *m = rt_orderedmap_new();
    assert(m != NULL);
    assert(rt_orderedmap_len(m) == 0);
    assert(rt_orderedmap_is_empty(m) == 1);
}

static void test_set_and_get() {
    void *m = rt_orderedmap_new();
    rt_string k = make_str("key1");
    rt_string v = make_str("value1");

    rt_orderedmap_set(m, k, v);
    assert(rt_orderedmap_len(m) == 1);
    assert(rt_orderedmap_is_empty(m) == 0);

    void *got = rt_orderedmap_get(m, k);
    assert(got == v);

    rt_string_unref(k);
}

static void test_overwrite() {
    void *m = rt_orderedmap_new();
    rt_string k = make_str("key");
    rt_string v1 = make_str("first");
    rt_string v2 = make_str("second");

    rt_orderedmap_set(m, k, v1);
    rt_orderedmap_set(m, k, v2);

    assert(rt_orderedmap_len(m) == 1);
    assert(rt_orderedmap_get(m, k) == v2);

    rt_string_unref(k);
}

static void test_has() {
    void *m = rt_orderedmap_new();
    rt_string k1 = make_str("exists");
    rt_string k2 = make_str("missing");
    rt_string v = make_str("val");

    rt_orderedmap_set(m, k1, v);
    assert(rt_orderedmap_has(m, k1) == 1);
    assert(rt_orderedmap_has(m, k2) == 0);

    rt_string_unref(k1);
    rt_string_unref(k2);
}

static void test_remove() {
    void *m = rt_orderedmap_new();
    rt_string k = make_str("key");
    rt_string v = make_str("val");

    rt_orderedmap_set(m, k, v);
    assert(rt_orderedmap_remove(m, k) == 1);
    assert(rt_orderedmap_len(m) == 0);
    assert(rt_orderedmap_has(m, k) == 0);

    assert(rt_orderedmap_remove(m, k) == 0); // Already removed

    rt_string_unref(k);
}

static void test_insertion_order() {
    void *m = rt_orderedmap_new();
    rt_string ka = make_str("alpha");
    rt_string kb = make_str("beta");
    rt_string kc = make_str("gamma");
    rt_string va = make_str("a");
    rt_string vb = make_str("b");
    rt_string vc = make_str("c");

    rt_orderedmap_set(m, ka, va);
    rt_orderedmap_set(m, kb, vb);
    rt_orderedmap_set(m, kc, vc);

    // Keys should be in insertion order
    void *keys = rt_orderedmap_keys(m);
    assert(rt_seq_len(keys) == 3);
    assert(rt_seq_cap(keys) == 3);
    assert(str_eq((rt_string)rt_seq_get(keys, 0), "alpha"));
    assert(str_eq((rt_string)rt_seq_get(keys, 1), "beta"));
    assert(str_eq((rt_string)rt_seq_get(keys, 2), "gamma"));

    void *values = rt_orderedmap_values(m);
    assert(rt_seq_len(values) == 3);
    assert(rt_seq_cap(values) == 3);

    rt_string_unref(ka);
    rt_string_unref(kb);
    rt_string_unref(kc);
}

static void test_key_at() {
    void *m = rt_orderedmap_new();
    rt_string k1 = make_str("first");
    rt_string k2 = make_str("second");
    rt_string k3 = make_str("third");
    rt_string v = make_str("v");

    rt_orderedmap_set(m, k1, v);
    rt_orderedmap_set(m, k2, v);
    rt_orderedmap_set(m, k3, v);

    rt_string at0 = rt_orderedmap_key_at(m, 0);
    rt_string at1 = rt_orderedmap_key_at(m, 1);
    rt_string at2 = rt_orderedmap_key_at(m, 2);

    assert(str_eq(at0, "first"));
    assert(str_eq(at1, "second"));
    assert(str_eq(at2, "third"));
    assert(rt_orderedmap_key_at(m, 3) == NULL);

    rt_string_unref(at0);
    rt_string_unref(at1);
    rt_string_unref(at2);
    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_string_unref(k3);
}

static void test_clear() {
    void *m = rt_orderedmap_new();
    rt_string k = make_str("key");
    rt_string v = make_str("val");

    rt_orderedmap_set(m, k, v);
    rt_orderedmap_clear(m);

    assert(rt_orderedmap_len(m) == 0);
    assert(rt_orderedmap_has(m, k) == 0);

    rt_string_unref(k);
}

static void test_many_entries() {
    void *m = rt_orderedmap_new();
    char buf[32];

    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof(buf), "key_%03d", i);
        rt_string k = make_str(buf);
        rt_string v = make_str(buf);
        rt_orderedmap_set(m, k, v);
        rt_string_unref(k);
    }

    assert(rt_orderedmap_len(m) == 100);

    // Verify order is preserved
    rt_string first = rt_orderedmap_key_at(m, 0);
    rt_string last = rt_orderedmap_key_at(m, 99);
    assert(str_eq(first, "key_000"));
    assert(str_eq(last, "key_099"));
    rt_string_unref(first);
    rt_string_unref(last);
}

static void test_value_release_on_clear() {
    void *m = rt_orderedmap_new();
    rt_string k = make_str("owned");
    void *value = new_obj();
    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_orderedmap_set(m, k, value);
    release_obj(value);
    assert(g_finalizer_calls == 0);

    rt_orderedmap_clear(m);
    assert(g_finalizer_calls == 1);

    rt_string_unref(k);
}

static void test_remove_commits_before_reentrant_clear() {
    void *m = rt_orderedmap_new();
    rt_string victim_key = make_str("victim");
    rt_string other_key = make_str("other");
    void *victim = new_obj();
    void *other = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_orderedmap_set(m, victim_key, victim);
    rt_orderedmap_set(m, other_key, other);
    release_obj(victim);
    release_obj(other);

    g_reentry_map = m;
    g_reentry_action = ReentryAction::Clear;
    assert(rt_orderedmap_remove(m, victim_key) == 1);
    assert(g_reentry_calls == 1);
    assert(rt_orderedmap_len(m) == 0);
    assert(rt_orderedmap_is_empty(m) == 1);

    reset_reentry_state();
    rt_string_unref(victim_key);
    rt_string_unref(other_key);
    release_obj(m);
}

static void test_clear_preserves_reentrant_insert() {
    void *m = rt_orderedmap_new();
    rt_string key = make_str("same-key");
    void *victim = new_obj();
    void *inserted = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_orderedmap_set(m, key, victim);
    release_obj(victim);

    g_reentry_map = m;
    g_reentry_key = key;
    g_reentry_value = inserted;
    g_reentry_action = ReentryAction::Set;
    rt_orderedmap_clear(m);

    assert(g_reentry_calls == 1);
    assert(rt_orderedmap_len(m) == 1);
    assert(rt_orderedmap_get(m, key) == inserted);

    reset_reentry_state();
    rt_orderedmap_clear(m);
    release_obj(inserted);
    rt_string_unref(key);
    release_obj(m);
}

static void test_owner_finalizer_blocks_reentrant_insert() {
    void *m = rt_orderedmap_new();
    rt_string key = make_str("owner-finalizer");
    void *victim = new_obj();
    void *inserted = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_orderedmap_set(m, key, victim);
    release_obj(victim);

    g_reentry_map = m;
    g_reentry_key = key;
    g_reentry_value = inserted;
    g_reentry_action = ReentryAction::Set;
    release_obj(m);

    rt_heap_info_t inserted_info;
    assert(g_reentry_calls == 1);
    assert(rt_heap_get_info(inserted, &inserted_info) == 1);
    assert(inserted_info.refcnt == 1);

    reset_reentry_state();
    release_obj(inserted);
    rt_string_unref(key);
}

static void test_resurrected_owner_is_empty_reusable_and_rearmed() {
    void *m = rt_orderedmap_new();
    rt_string key = make_str("resurrect");
    void *victim = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_orderedmap_set(m, key, victim);
    release_obj(victim);

    g_reentry_map = m;
    g_reentry_action = ReentryAction::Resurrect;
    release_obj(m);

    assert(g_reentry_calls == 1);
    assert(rt_orderedmap_len(m) == 0);
    assert(rt_orderedmap_is_empty(m) == 1);
    reset_reentry_state();

    g_finalizer_calls = 0;
    void *second = new_obj();
    rt_obj_set_finalizer(second, count_finalizer);
    rt_orderedmap_set(m, key, second);
    release_obj(second);
    assert(rt_orderedmap_get(m, key) == second);

    release_obj(m);
    assert(g_finalizer_calls == 1);
    rt_string_unref(key);
}

static void test_finalizer_drains_after_value_finalizer_trap() {
    void *m = rt_orderedmap_new();
    rt_string first_key = make_str("first");
    rt_string second_key = make_str("second");
    void *first = new_obj();
    void *second = new_obj();

    g_trapping_finalizer_calls = 0;
    g_finalizer_calls = 0;
    rt_obj_set_finalizer(first, trapping_finalizer);
    rt_obj_set_finalizer(second, count_finalizer);
    rt_orderedmap_set(m, first_key, first);
    rt_orderedmap_set(m, second_key, second);
    release_obj(first);
    release_obj(second);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        release_obj(m);
        assert(false && "trapping value finalizer should propagate");
    } else {
        const char *error = rt_trap_get_error();
        assert(error != nullptr);
        assert(strstr(error, "OrderedMap test finalizer trap") != nullptr);
        rt_trap_clear_recovery();
    }

    assert(g_trapping_finalizer_calls == 1);
    assert(g_finalizer_calls == 1);
    rt_string_unref(first_key);
    rt_string_unref(second_key);
}

static void test_embedded_nul_keys_are_distinct() {
    void *m = rt_orderedmap_new();
    const char bytes[] = {'a', '\0', 'b'};
    rt_string k1 = make_bytes(bytes, sizeof(bytes));
    rt_string k2 = make_str("a");
    rt_string v1 = make_str("v1");
    rt_string v2 = make_str("v2");

    rt_orderedmap_set(m, k1, v1);
    rt_orderedmap_set(m, k2, v2);

    assert(rt_orderedmap_len(m) == 2);
    assert(rt_orderedmap_get(m, k1) == v1);
    assert(rt_orderedmap_get(m, k2) == v2);

    rt_string_unref(k1);
    rt_string_unref(k2);
}

static void test_null_safety() {
    assert(rt_orderedmap_len(NULL) == 0);
    assert(rt_orderedmap_is_empty(NULL) == 1);
    assert(rt_orderedmap_get(NULL, NULL) == NULL);
    assert(rt_orderedmap_has(NULL, NULL) == 0);
    assert(rt_orderedmap_remove(NULL, NULL) == 0);
    assert(rt_orderedmap_key_at(NULL, 0) == NULL);
}

/// @brief Main.
int main() {
    test_new_empty();
    test_set_and_get();
    test_overwrite();
    test_has();
    test_remove();
    test_insertion_order();
    test_key_at();
    test_clear();
    test_many_entries();
    test_value_release_on_clear();
    test_remove_commits_before_reentrant_clear();
    test_clear_preserves_reentrant_insert();
    test_owner_finalizer_blocks_reentrant_insert();
    test_resurrected_owner_is_empty_reusable_and_rearmed();
    test_finalizer_drains_after_value_finalizer_trap();
    test_embedded_nul_keys_are_distinct();
    test_null_safety();

    return 0;
}
