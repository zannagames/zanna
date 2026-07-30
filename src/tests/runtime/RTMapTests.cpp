//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTMapTests.cpp
// Purpose: Tests for Zanna.Collections.Map runtime helpers.
// Key invariants: Resize/Trim publication is transactional and cached hashes do
//                 not change key equality or insertion-order behavior.
// Ownership/Lifetime: Tests release every map/key/value; injected OOM recovery
//                     preserves ownership of the original usable table.
// Links: src/runtime/collections/rt_map.c,
//        docs/adr/0133-runtime-concurrency-and-collection-hardening.md
//
//===----------------------------------------------------------------------===//

#include "rt_internal.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_string.h"

#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstring>

namespace {
static jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_trap_expected = false;
static int g_finalizer_calls = 0;
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
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
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

static void count_finalizer(void *) {
    ++g_finalizer_calls;
}

static void reentrant_finalizer(void *) {
    ++g_reentry_calls;
    switch (g_reentry_action) {
        case ReentryAction::Clear:
            rt_map_clear(g_reentry_map);
            break;
        case ReentryAction::Set:
            rt_map_set(g_reentry_map, g_reentry_key, g_reentry_value);
            break;
        case ReentryAction::Remove:
            g_reentry_remove_result = rt_map_remove(g_reentry_map, g_reentry_key);
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
    assert(text != nullptr);
    return rt_string_from_bytes(text, strlen(text));
}

static rt_string make_key_bytes(const char *text, size_t len) {
    return rt_string_from_bytes(text, len);
}

/// @brief White-box Map payload view used only to assert capacity policy.
struct MapLayout {
    void **vptr;
    void *buckets;
    size_t capacity;
    size_t count;
};

static void *reject_runtime_allocation(int64_t, void *(*)(int64_t)) {
    return nullptr;
}

static void set_numbered_key(void *map, int index, void *value) {
    char text[32];
    int written = snprintf(text, sizeof(text), "key-%d", index);
    assert(written > 0 && static_cast<size_t>(written) < sizeof(text));
    rt_string key = make_key(text);
    rt_map_set(map, key, value);
    rt_string_unref(key);
}

static rt_string numbered_key(int index) {
    char text[32];
    int written = snprintf(text, sizeof(text), "key-%d", index);
    assert(written > 0 && static_cast<size_t>(written) < sizeof(text));
    return make_key(text);
}

static void test_failed_growth_and_trim_preserve_map() {
    void *map = rt_map_new();
    void *value = new_obj();
    assert(map != nullptr && value != nullptr);

    for (int i = 0; i < 12; ++i)
        set_numbered_key(map, i, value);
    auto *layout = static_cast<MapLayout *>(map);
    assert(layout->capacity == 16);
    assert(layout->count == 12);

    rt_string thirteenth = numbered_key(12);
    g_trap_expected = true;
    g_last_trap = nullptr;
    rt_set_alloc_hook(reject_runtime_allocation);
    if (setjmp(g_trap_jmp) == 0) {
        rt_map_set(map, thirteenth, value);
        assert(false && "expected Map growth allocation trap");
    }
    rt_set_alloc_hook(nullptr);
    g_trap_expected = false;
    assert(g_last_trap && strstr(g_last_trap, "Map") != nullptr);
    assert(layout->capacity == 16);
    assert(layout->count == 12);
    assert(rt_map_has(map, thirteenth) == 0);
    rt_string_unref(thirteenth);

    for (int i = 12; i < 100; ++i)
        set_numbered_key(map, i, value);
    assert(layout->capacity == 256);

    for (int i = 1; i < 100; ++i) {
        rt_string key = numbered_key(i);
        assert(rt_map_remove(map, key) == 1);
        rt_string_unref(key);
    }
    rt_string first = numbered_key(0);
    assert(layout->count == 1);
    assert(rt_map_get(map, first) == value);

    g_trap_expected = true;
    g_last_trap = nullptr;
    rt_set_alloc_hook(reject_runtime_allocation);
    if (setjmp(g_trap_jmp) == 0) {
        (void)rt_map_trim(map);
        assert(false && "expected Map trim allocation trap");
    }
    rt_set_alloc_hook(nullptr);
    g_trap_expected = false;
    assert(layout->capacity == 256);
    assert(layout->count == 1);
    assert(rt_map_get(map, first) == value);

    assert(rt_map_trim(map) == 1);
    assert(layout->capacity == 16);
    assert(layout->count == 1);
    assert(rt_map_get(map, first) == value);

    rt_string_unref(first);
    rt_release_obj(map);
    rt_release_obj(value);
}

static void test_remove_frees_last_reference_without_invalid_free() {
    void *map = rt_map_new();
    assert(map != nullptr);

    rt_string key = make_key("k");
    void *value = new_obj();

    rt_map_set(map, key, value);
    // Drop the creator reference; map now owns the single remaining ref.
    rt_release_obj(value);

    assert(rt_map_len(map) == 1);
    assert(rt_map_remove(map, key) == 1);
    assert(rt_map_len(map) == 0);

    rt_string_unref(key);
    rt_release_obj(map);
}

static void test_overwrite_frees_old_last_reference_without_invalid_free() {
    void *map = rt_map_new();
    assert(map != nullptr);

    rt_string key = make_key("k");
    void *value1 = new_obj();
    void *value2 = new_obj();

    rt_map_set(map, key, value1);
    rt_release_obj(value1);

    // Overwrite should release+free the old value when it was the last reference.
    rt_map_set(map, key, value2);
    rt_release_obj(value2);

    assert(rt_map_len(map) == 1);
    assert(rt_map_remove(map, key) == 1);
    assert(rt_map_len(map) == 0);

    rt_string_unref(key);
    rt_release_obj(map);
}

static void test_free_runs_map_finalizer_and_releases_values() {
    void *map = rt_map_new();
    assert(map != nullptr);

    rt_string key = make_key("k");
    void *value = new_obj();

    g_finalizer_calls = 0;
    rt_obj_set_finalizer(value, count_finalizer);

    rt_map_set(map, key, value);
    rt_release_obj(value); // map now owns the only remaining ref
    assert(g_finalizer_calls == 0);

    rt_string_unref(key);
    rt_release_obj(map);
    assert(g_finalizer_calls == 1);
}

static void test_embedded_nul_keys_are_distinct() {
    void *map = rt_map_new();
    assert(map != nullptr);

    const char bytes[] = {'a', '\0', 'b'};
    rt_string k1 = make_key_bytes(bytes, sizeof(bytes));
    rt_string k2 = make_key("a");
    void *v1 = new_obj();
    void *v2 = new_obj();

    rt_map_set(map, k1, v1);
    rt_map_set(map, k2, v2);

    assert(rt_map_len(map) == 2);
    assert(rt_map_get(map, k1) == v1);
    assert(rt_map_get(map, k2) == v2);

    rt_string_unref(k1);
    rt_string_unref(k2);
    rt_release_obj(v1);
    rt_release_obj(v2);
    rt_release_obj(map);
}

static void test_remove_commits_before_reentrant_finalizer() {
    void *map = rt_map_new();
    void *victim = new_obj();
    void *other = new_obj();
    rt_string victim_key = make_key("victim");
    rt_string other_key = make_key("other");

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_map_set(map, victim_key, victim);
    rt_map_set(map, other_key, other);
    rt_release_obj(victim);

    g_reentry_map = map;
    g_reentry_action = ReentryAction::Clear;
    assert(rt_map_remove(map, victim_key) == 1);
    assert(g_reentry_calls == 1);
    assert(rt_map_len(map) == 0);

    reset_reentry_state();
    rt_string_unref(victim_key);
    rt_string_unref(other_key);
    rt_release_obj(other);
    rt_release_obj(map);
}

static void test_clear_detaches_before_reentrant_finalizer() {
    void *map = rt_map_new();
    void *victim = new_obj();
    void *inserted = new_obj();
    rt_string key = make_key("same-key");

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_map_set(map, key, victim);
    rt_release_obj(victim);

    g_reentry_map = map;
    g_reentry_key = key;
    g_reentry_value = inserted;
    g_reentry_action = ReentryAction::Set;
    rt_map_clear(map);

    assert(g_reentry_calls == 1);
    assert(rt_map_len(map) == 1);
    assert(rt_map_get(map, key) == inserted);

    reset_reentry_state();
    rt_map_clear(map);
    rt_string_unref(key);
    rt_release_obj(inserted);
    rt_release_obj(map);
}

static void test_owner_finalizer_invalidates_before_value_cleanup() {
    void *map = rt_map_new();
    void *victim = new_obj();
    rt_string key = make_key("owner-finalizer");

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_map_set(map, key, victim);
    rt_release_obj(victim);

    g_reentry_map = map;
    g_reentry_key = key;
    g_reentry_action = ReentryAction::Remove;
    rt_release_obj(map);

    assert(g_reentry_calls == 1);
    assert(g_reentry_remove_result == 0);
    reset_reentry_state();
    rt_string_unref(key);
}

int main() {
    test_failed_growth_and_trim_preserve_map();
    test_remove_frees_last_reference_without_invalid_free();
    test_overwrite_frees_old_last_reference_without_invalid_free();
    test_free_runs_map_finalizer_and_releases_values();
    test_embedded_nul_keys_are_distinct();
    test_remove_commits_before_reentrant_finalizer();
    test_clear_detaches_before_reentrant_finalizer();
    test_owner_finalizer_invalidates_before_value_cleanup();
    return 0;
}
