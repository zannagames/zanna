//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTObjectClassDtorHookTests.cpp
// Purpose: Verify the program-wide class destructor hook: rt_obj_free invokes
//          it for payloads with a positive class id (before any per-object
//          finalizer), skips runtime-internal ids (zero/negative), and stops
//          once the hook is cleared.
// Key invariants:
//   - The hook observes the payload with its class id still readable.
//   - The hook runs before the per-object finalizer, each exactly once.
//   - Objects released by the runtime itself (array elements) reach the hook.
// Ownership/Lifetime:
//   - Every payload is released through the public release helpers.
// Links: src/runtime/oop/rt_object.c, docs/adr/0313-class-destructor-hook.md
//
//===----------------------------------------------------------------------===//

#include "core/rt_heap.h"
#include "rt.hpp"
#include "rt_array_obj.h"
#include "rt_object.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static int g_hook_calls = 0;
static int64_t g_hook_last_class = 0;
static int g_finalizer_calls = 0;
static int g_finalizer_after_hook = 0;

static void class_dtor_hook(void *obj) {
    ++g_hook_calls;
    g_hook_last_class = rt_obj_class_id(obj);
}

static void object_finalizer(void *obj) {
    (void)obj;
    ++g_finalizer_calls;
    if (g_hook_calls == 1)
        g_finalizer_after_hook = 1;
}

int main() {
    assert(rt_obj_get_class_dtor_hook() == NULL);
    rt_obj_set_class_dtor_hook((void *)&class_dtor_hook);
    assert(rt_obj_get_class_dtor_hook() == (void *)&class_dtor_hook);

    // A compiled class instance (positive id) reaches the hook on rt_obj_free.
    void *obj = rt_obj_new_i64(42, 32);
    assert(obj != NULL);
    assert(rt_obj_release_check0(obj) == 1);
    rt_obj_free(obj);
    assert(g_hook_calls == 1);
    assert(g_hook_last_class == 42);

    // The hook runs before a per-object finalizer, each once.
    void *fin = rt_obj_new_i64(7, 16);
    rt_obj_set_finalizer(fin, &object_finalizer);
    g_hook_calls = 0;
    assert(rt_obj_release_check0(fin) == 1);
    rt_obj_free(fin);
    assert(g_hook_calls == 1);
    assert(g_finalizer_calls == 1);
    assert(g_finalizer_after_hook == 1);

    // Runtime-internal ids (zero and negative) never reach the hook.
    g_hook_calls = 0;
    void *env = rt_obj_new_i64(0, 16);
    assert(rt_obj_release_check0(env) == 1);
    rt_obj_free(env);
    void *internal = rt_obj_new_i64(-5, 16);
    assert(rt_obj_release_check0(internal) == 1);
    rt_obj_free(internal);
    assert(g_hook_calls == 0);

    // An element released by the runtime (array teardown) reaches the hook.
    void **arr = rt_arr_obj_new(1);
    assert(arr != NULL);
    void *elem = rt_obj_new_i64(9, 16);
    rt_arr_obj_put(arr, 0, elem); // the array retains its own reference
    assert(rt_obj_release_check0(elem) == 0);
    g_hook_calls = 0;
    rt_arr_obj_release(arr);
    assert(g_hook_calls == 1);
    assert(g_hook_last_class == 9);

    // Clearing the hook stops dispatch.
    rt_obj_set_class_dtor_hook(NULL);
    g_hook_calls = 0;
    void *late = rt_obj_new_i64(42, 16);
    assert(rt_obj_release_check0(late) == 1);
    rt_obj_free(late);
    assert(g_hook_calls == 0);
    return 0;
}
