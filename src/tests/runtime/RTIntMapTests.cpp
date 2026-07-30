//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTIntMapTests.cpp
// Purpose: Tests for Zanna.Collections.IntMap runtime helpers.
// Key invariants: Resizing is transactional, cached hashes remain stable, and
//                 Trim selects the smallest legal geometric capacity.
// Ownership/Lifetime: Tests release every map and managed value; injected OOM
//                     recovery leaves the original map owned and usable.
// Links: src/runtime/collections/rt_intmap.c,
//        docs/adr/0133-runtime-concurrency-and-collection-hardening.md
//
//===----------------------------------------------------------------------===//

#include "rt_box.h"
#include "rt_internal.h"
#include "rt_intmap.h"
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
enum class ReentryAction {
    None,
    Clear,
    Set,
    Remove,
};
static void *g_reentry_map = nullptr;
static int64_t g_reentry_key = 0;
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

static void release_obj(void *p) {
    if (p && rt_obj_release_check0(p))
        rt_obj_free(p);
}

static rt_string make_str(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static void *new_obj() {
    void *obj = rt_obj_new_i64(0, 8);
    assert(obj != nullptr);
    return obj;
}

static void reentrant_finalizer(void *) {
    ++g_reentry_calls;
    switch (g_reentry_action) {
        case ReentryAction::Clear:
            rt_intmap_clear(g_reentry_map);
            break;
        case ReentryAction::Set:
            rt_intmap_set(g_reentry_map, g_reentry_key, g_reentry_value);
            break;
        case ReentryAction::Remove:
            g_reentry_remove_result = rt_intmap_remove(g_reentry_map, g_reentry_key);
            break;
        case ReentryAction::None:
            break;
    }
}

static void reset_reentry_state() {
    g_reentry_map = nullptr;
    g_reentry_key = 0;
    g_reentry_value = nullptr;
    g_reentry_action = ReentryAction::None;
    g_reentry_calls = 0;
    g_reentry_remove_result = -1;
}

/// @brief White-box IntMap payload view used to verify capacity invariants.
struct IntMapLayout {
    void **vptr;
    void *buckets;
    size_t capacity;
    size_t count;
};

static void *reject_runtime_allocation(int64_t, void *(*)(int64_t)) {
    return nullptr;
}

static void test_failed_growth_and_trim_preserve_intmap() {
    void *map = rt_intmap_new();
    rt_string value = make_str("value");
    assert(map != nullptr && value != nullptr);

    for (int64_t i = 0; i < 12; ++i)
        rt_intmap_set(map, i, value);
    auto *layout = static_cast<IntMapLayout *>(map);
    assert(layout->capacity == 16);
    assert(layout->count == 12);

    g_trap_expected = true;
    g_last_trap = nullptr;
    rt_set_alloc_hook(reject_runtime_allocation);
    if (setjmp(g_trap_jmp) == 0) {
        rt_intmap_set(map, 12, value);
        assert(false && "expected IntMap growth allocation trap");
    }
    rt_set_alloc_hook(nullptr);
    g_trap_expected = false;
    assert(g_last_trap && strstr(g_last_trap, "IntMap") != nullptr);
    assert(layout->capacity == 16);
    assert(layout->count == 12);
    assert(rt_intmap_has(map, 12) == 0);

    for (int64_t i = 12; i < 100; ++i)
        rt_intmap_set(map, i, value);
    assert(layout->capacity == 256);
    for (int64_t i = 1; i < 100; ++i)
        assert(rt_intmap_remove(map, i) == 1);
    assert(layout->count == 1);
    assert(rt_intmap_get(map, 0) == value);

    g_trap_expected = true;
    g_last_trap = nullptr;
    rt_set_alloc_hook(reject_runtime_allocation);
    if (setjmp(g_trap_jmp) == 0) {
        (void)rt_intmap_trim(map);
        assert(false && "expected IntMap trim allocation trap");
    }
    rt_set_alloc_hook(nullptr);
    g_trap_expected = false;
    assert(layout->capacity == 256);
    assert(layout->count == 1);
    assert(rt_intmap_get(map, 0) == value);

    assert(rt_intmap_trim(map) == 1);
    assert(layout->capacity == 16);
    assert(layout->count == 1);
    assert(rt_intmap_get(map, 0) == value);

    rt_string_unref(value);
    release_obj(map);
}

static void test_set_get_remove() {
    void *map = rt_intmap_new();
    assert(map != nullptr);
    assert(rt_intmap_is_empty(map) == 1);

    rt_string a = make_str("a");
    rt_string b = make_str("b");
    rt_intmap_set(map, -7, a);
    rt_intmap_set(map, 42, b);

    assert(rt_intmap_len(map) == 2);
    assert(rt_intmap_get(map, -7) == a);
    assert(rt_intmap_get(map, 42) == b);
    assert(rt_intmap_has(map, 9) == 0);
    assert(rt_intmap_remove(map, -7) == 1);
    assert(rt_intmap_len(map) == 1);
    assert(rt_intmap_get(map, -7) == nullptr);

    rt_string_unref(a);
    rt_string_unref(b);
    release_obj(map);
}

static void test_keys_are_boxed_i64_values() {
    void *map = rt_intmap_new();
    rt_string a = make_str("a");
    rt_string b = make_str("b");
    rt_intmap_set(map, INT64_MIN + 1, a);
    rt_intmap_set(map, INT64_MAX - 1, b);

    void *keys = rt_intmap_keys(map);
    assert(rt_seq_len(keys) == 2);

    bool found_min = false;
    bool found_max = false;
    for (int64_t i = 0; i < rt_seq_len(keys); ++i) {
        void *boxed = rt_seq_get(keys, i);
        assert(rt_box_type(boxed) == RT_BOX_I64);
        int64_t key = rt_unbox_i64(boxed);
        found_min = found_min || key == INT64_MIN + 1;
        found_max = found_max || key == INT64_MAX - 1;
    }
    assert(found_min);
    assert(found_max);

    void *values = rt_intmap_values(map);
    assert(rt_seq_len(values) == 2);

    rt_string_unref(a);
    rt_string_unref(b);
    release_obj(values);
    release_obj(keys);
    release_obj(map);
}

static void test_remove_commits_before_reentrant_finalizer() {
    void *map = rt_intmap_new();
    void *victim = new_obj();
    void *other = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_intmap_set(map, 1, victim);
    rt_intmap_set(map, 2, other);
    release_obj(victim);

    g_reentry_map = map;
    g_reentry_action = ReentryAction::Clear;
    assert(rt_intmap_remove(map, 1) == 1);
    assert(g_reentry_calls == 1);
    assert(rt_intmap_len(map) == 0);

    reset_reentry_state();
    release_obj(other);
    release_obj(map);
}

static void test_clear_detaches_before_reentrant_finalizer() {
    void *map = rt_intmap_new();
    void *victim = new_obj();
    void *inserted = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_intmap_set(map, 7, victim);
    release_obj(victim);

    g_reentry_map = map;
    g_reentry_key = 7;
    g_reentry_value = inserted;
    g_reentry_action = ReentryAction::Set;
    rt_intmap_clear(map);

    assert(g_reentry_calls == 1);
    assert(rt_intmap_len(map) == 1);
    assert(rt_intmap_get(map, 7) == inserted);

    reset_reentry_state();
    rt_intmap_clear(map);
    release_obj(inserted);
    release_obj(map);
}

static void test_owner_finalizer_invalidates_before_value_cleanup() {
    void *map = rt_intmap_new();
    void *victim = new_obj();

    rt_obj_set_finalizer(victim, reentrant_finalizer);
    rt_intmap_set(map, 99, victim);
    release_obj(victim);

    g_reentry_map = map;
    g_reentry_key = 99;
    g_reentry_action = ReentryAction::Remove;
    release_obj(map);

    assert(g_reentry_calls == 1);
    assert(g_reentry_remove_result == 0);
    reset_reentry_state();
}

int main() {
    test_failed_growth_and_trim_preserve_intmap();
    test_set_get_remove();
    test_keys_are_boxed_i64_values();
    test_remove_commits_before_reentrant_finalizer();
    test_clear_detaches_before_reentrant_finalizer();
    test_owner_finalizer_invalidates_before_value_cleanup();
    return 0;
}
