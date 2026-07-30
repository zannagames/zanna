//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "rt_defaultmap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <cstring>

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

static rt_string make_str(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static rt_string make_bytes(const char *s, size_t len) {
    return rt_string_from_bytes(s, len);
}

static void release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

enum class ReentryAction {
    None,
    Clear,
    Set,
    Remove,
};

static void *g_reentry_map = nullptr;
static rt_string g_reentry_key = nullptr;
static void *g_reentry_value = nullptr;
static ReentryAction g_reentry_action = ReentryAction::None;
static int g_reentry_calls = 0;
static int8_t g_reentry_remove_result = -1;

static void reentrant_value_finalizer(void *) {
    g_reentry_calls++;
    switch (g_reentry_action) {
        case ReentryAction::Clear:
            rt_defaultmap_clear(g_reentry_map);
            break;
        case ReentryAction::Set:
            rt_defaultmap_set(g_reentry_map, g_reentry_key, g_reentry_value);
            break;
        case ReentryAction::Remove:
            g_reentry_remove_result = rt_defaultmap_remove(g_reentry_map, g_reentry_key);
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

static void test_new() {
    rt_string def = make_str("default");
    void *m = rt_defaultmap_new(def);
    assert(m != NULL);
    assert(rt_defaultmap_len(m) == 0);
}

static void test_get_default() {
    rt_string def = make_str("DEFAULT");
    void *m = rt_defaultmap_new(def);

    rt_string key = make_str("missing");
    void *got = rt_defaultmap_get(m, key);
    assert(got == def);

    rt_string_unref(key);
}

static void test_set_and_get() {
    rt_string def = make_str("DEFAULT");
    void *m = rt_defaultmap_new(def);

    rt_string k = make_str("key1");
    rt_string v = make_str("value1");
    rt_defaultmap_set(m, k, v);

    assert(rt_defaultmap_len(m) == 1);
    assert(rt_defaultmap_get(m, k) == v);

    rt_string_unref(k);
}

static void test_has() {
    rt_string def = make_str("DEFAULT");
    void *m = rt_defaultmap_new(def);

    rt_string k = make_str("key");
    rt_string missing = make_str("nope");
    rt_string v = make_str("val");

    rt_defaultmap_set(m, k, v);
    assert(rt_defaultmap_has(m, k) == 1);
    assert(rt_defaultmap_has(m, missing) == 0);

    rt_string_unref(k);
    rt_string_unref(missing);
}

static void test_remove() {
    rt_string def = make_str("DEFAULT");
    void *m = rt_defaultmap_new(def);

    rt_string k = make_str("key");
    rt_string v = make_str("val");

    rt_defaultmap_set(m, k, v);
    assert(rt_defaultmap_remove(m, k) == 1);
    assert(rt_defaultmap_len(m) == 0);

    // Now get should return default
    assert(rt_defaultmap_get(m, k) == def);

    rt_string_unref(k);
}

static void test_keys() {
    rt_string def = make_str("D");
    void *m = rt_defaultmap_new(def);

    rt_string k1 = make_str("alpha");
    rt_string k2 = make_str("beta");
    rt_string v = make_str("v");

    rt_defaultmap_set(m, k1, v);
    rt_defaultmap_set(m, k2, v);

    void *keys = rt_defaultmap_keys(m);
    assert(rt_seq_len(keys) == 2);
    assert(rt_seq_cap(keys) == 2);

    rt_string_unref(k1);
    rt_string_unref(k2);
}

static void test_get_default_value() {
    rt_string def = make_str("MY_DEFAULT");
    void *m = rt_defaultmap_new(def);
    assert(rt_defaultmap_get_default(m) == def);
}

static void test_clear() {
    rt_string def = make_str("D");
    void *m = rt_defaultmap_new(def);

    rt_string k = make_str("key");
    rt_string v = make_str("val");
    rt_defaultmap_set(m, k, v);

    rt_defaultmap_clear(m);
    assert(rt_defaultmap_len(m) == 0);

    rt_string_unref(k);
}

static void test_null_default() {
    void *m = rt_defaultmap_new(NULL);
    rt_string k = make_str("key");
    assert(rt_defaultmap_get(m, k) == NULL);
    rt_string_unref(k);
}

static void test_null_key_uses_empty_key() {
    rt_string def = make_str("DEFAULT");
    void *m = rt_defaultmap_new(def);
    rt_string v = make_str("empty-key");

    assert(rt_defaultmap_get(m, NULL) == def);
    rt_defaultmap_set(m, NULL, v);
    assert(rt_defaultmap_has(m, NULL) == 1);
    assert(rt_defaultmap_get(m, NULL) == v);
    assert(rt_defaultmap_remove(m, NULL) == 1);
    assert(rt_defaultmap_get(m, NULL) == def);
}

static void test_embedded_nul_keys_are_distinct() {
    rt_string def = make_str("DEFAULT");
    void *m = rt_defaultmap_new(def);
    const char bytes[] = {'a', '\0', 'b'};
    rt_string k1 = make_bytes(bytes, sizeof(bytes));
    rt_string k2 = make_str("a");
    rt_string v1 = make_str("v1");
    rt_string v2 = make_str("v2");

    rt_defaultmap_set(m, k1, v1);
    rt_defaultmap_set(m, k2, v2);

    assert(rt_defaultmap_len(m) == 2);
    assert(rt_defaultmap_get(m, k1) == v1);
    assert(rt_defaultmap_get(m, k2) == v2);

    rt_string_unref(k1);
    rt_string_unref(k2);
}

static void test_null_safety() {
    assert(rt_defaultmap_len(NULL) == 0);
    assert(rt_defaultmap_get(NULL, NULL) == NULL);
    assert(rt_defaultmap_has(NULL, NULL) == 0);
    assert(rt_defaultmap_remove(NULL, NULL) == 0);
    assert(rt_defaultmap_get_default(NULL) == NULL);
}

static void test_remove_commits_before_value_finalizer_reentry() {
    void *map = rt_defaultmap_new(nullptr);
    void *victim = rt_obj_new_i64(0, 8);
    void *other = rt_obj_new_i64(0, 8);
    rt_string victim_key = make_str("victim");
    rt_string other_key = make_str("other");
    assert(map != nullptr);
    assert(victim != nullptr);
    assert(other != nullptr);

    rt_obj_set_finalizer(victim, reentrant_value_finalizer);
    rt_defaultmap_set(map, victim_key, victim);
    rt_defaultmap_set(map, other_key, other);
    release_obj(victim);

    g_reentry_map = map;
    g_reentry_action = ReentryAction::Clear;
    assert(rt_defaultmap_remove(map, victim_key) == 1);
    assert(g_reentry_calls == 1);
    assert(rt_defaultmap_len(map) == 0);

    reset_reentry_state();
    rt_string_unref(victim_key);
    rt_string_unref(other_key);
    release_obj(other);
    release_obj(map);
}

static void test_clear_detaches_before_value_finalizer_reentry() {
    void *map = rt_defaultmap_new(nullptr);
    void *victim = rt_obj_new_i64(0, 8);
    void *inserted = rt_obj_new_i64(0, 8);
    rt_string key = make_str("same-bucket");
    assert(map != nullptr);
    assert(victim != nullptr);
    assert(inserted != nullptr);

    rt_obj_set_finalizer(victim, reentrant_value_finalizer);
    rt_defaultmap_set(map, key, victim);
    release_obj(victim);

    g_reentry_map = map;
    g_reentry_key = key;
    g_reentry_value = inserted;
    g_reentry_action = ReentryAction::Set;
    rt_defaultmap_clear(map);

    assert(g_reentry_calls == 1);
    assert(rt_defaultmap_len(map) == 1);
    assert(rt_defaultmap_get(map, key) == inserted);

    reset_reentry_state();
    rt_defaultmap_clear(map);
    rt_string_unref(key);
    release_obj(inserted);
    release_obj(map);
}

static void test_map_finalizer_invalidates_before_value_cleanup() {
    void *map = rt_defaultmap_new(nullptr);
    void *victim = rt_obj_new_i64(0, 8);
    rt_string key = make_str("finalizer");
    assert(map != nullptr);
    assert(victim != nullptr);

    rt_obj_set_finalizer(victim, reentrant_value_finalizer);
    rt_defaultmap_set(map, key, victim);
    release_obj(victim);

    g_reentry_map = map;
    g_reentry_key = key;
    g_reentry_action = ReentryAction::Remove;
    release_obj(map);

    assert(g_reentry_calls == 1);
    assert(g_reentry_remove_result == 0);
    reset_reentry_state();
    rt_string_unref(key);
}

/// @brief Main.
int main() {
    test_new();
    test_get_default();
    test_set_and_get();
    test_has();
    test_remove();
    test_keys();
    test_get_default_value();
    test_clear();
    test_null_default();
    test_null_key_uses_empty_key();
    test_embedded_nul_keys_are_distinct();
    test_null_safety();
    test_remove_commits_before_value_finalizer_reentry();
    test_clear_detaches_before_value_finalizer_reentry();
    test_map_finalizer_invalidates_before_value_cleanup();

    return 0;
}
