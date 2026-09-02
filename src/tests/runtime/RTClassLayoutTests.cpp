//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTClassLayoutTests.cpp
// Purpose: ADR 0315 — per-class strong-slot layouts and cycle collection of
//          compiled class instances: layout register/query/redeclare, an
//          instance is auto-tracked only when its class has strong slots, a
//          two-object cycle is reclaimed by rt_gc_collect with the class
//          destructor hook running exactly once per member under release
//          suppression, an external child owned by a cycle member is released
//          exactly once, and value-only instances never enter the collector.
// Key invariants:
//   - The hook emulates the compiled destructor: release every registered
//     slot and free at zero (rt_obj_release_check0 + rt_obj_free).
// Links: src/runtime/oop/rt_class_layout.c, src/runtime/core/rt_gc.c
//
//===----------------------------------------------------------------------===//

#include "core/rt_heap.h"
#include "rt.hpp"
#include "rt_class_layout.h"
#include "rt_gc.h"
#include "rt_object.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// A "compiled" class: two strong slots and one value slot.
struct link_obj {
    void *next;  // strong, offset 0
    int64_t tag; // value, offset 8
    void *extra; // strong, offset 16
};

static const int64_t kLinkClass = 7;
static const int64_t kValueOnlyClass = 8;
static const int64_t kExternalClass = 9; // value-only payload owned by a link

static int g_dtor_calls = 0;
static int g_dtor_calls_link = 0;
static int g_dtor_calls_external = 0;

// The class destructor hook, as the Zia lowerer would emit it: release each
// registered strong slot, freeing at zero.
static void class_dtor_hook(void *obj) {
    ++g_dtor_calls;
    const int64_t cid = rt_obj_class_id(obj);
    if (cid == kLinkClass)
        ++g_dtor_calls_link;
    if (cid == kExternalClass)
        ++g_dtor_calls_external;
    const rt_class_layout_t *layout = rt_obj_class_layout_get(cid);
    if (!layout)
        return;
    for (int64_t i = 0; i < layout->count; ++i) {
        void **slot = (void **)((char *)obj + layout->offsets[i]);
        void *child = *slot;
        *slot = NULL;
        if (child && rt_obj_release_check0(child))
            rt_obj_free(child);
    }
}

static void *new_link(void) {
    void *obj = rt_obj_new_i64(kLinkClass, sizeof(struct link_obj));
    assert(obj != NULL);
    memset(obj, 0, sizeof(struct link_obj));
    return obj;
}

int main() {
    // --- Layout registration and queries -----------------------------------
    assert(rt_obj_class_layout_get(kLinkClass) == NULL);
    assert(rt_obj_class_layout_has_ref_slots(kLinkClass) == 0);
    rt_obj_class_layout_begin(kLinkClass, 2);
    rt_obj_class_layout_add_slot(kLinkClass, offsetof(struct link_obj, next), RT_CLASS_SLOT_OBJ);
    rt_obj_class_layout_add_slot(kLinkClass, offsetof(struct link_obj, extra), RT_CLASS_SLOT_OBJ);
    const rt_class_layout_t *layout = rt_obj_class_layout_get(kLinkClass);
    assert(layout != NULL && layout->count == 2);
    assert(layout->offsets[0] == 0 && layout->offsets[1] == 16);
    assert(rt_obj_class_layout_has_ref_slots(kLinkClass) == 1);
    // A value-only class registers an empty layout (or none): never tracked.
    rt_obj_class_layout_begin(kValueOnlyClass, 0);
    assert(rt_obj_class_layout_has_ref_slots(kValueOnlyClass) == 0);
    assert(rt_obj_class_layout_get(kValueOnlyClass) != NULL);
    assert(rt_obj_class_layout_has_ref_slots(kExternalClass) == 0);
    // Redeclaration resets the slot list (the entry prologue may run twice on
    // some executors).
    rt_obj_class_layout_begin(kLinkClass, 2);
    assert(rt_obj_class_layout_get(kLinkClass)->count == 0);
    rt_obj_class_layout_add_slot(kLinkClass, offsetof(struct link_obj, next), RT_CLASS_SLOT_OBJ);
    rt_obj_class_layout_add_slot(kLinkClass, offsetof(struct link_obj, extra), RT_CLASS_SLOT_OBJ);
    assert(rt_obj_class_layout_get(kLinkClass)->count == 2);
    assert(rt_obj_class_layout_count() == 2);

    rt_obj_set_class_dtor_hook((void *)&class_dtor_hook);

    // --- Instances are tracked only when the class has strong slots ---------
    const int64_t base = rt_gc_tracked_count();
    void *value_only = rt_obj_new_i64(kValueOnlyClass, 32);
    assert(value_only != NULL);
    assert(rt_gc_tracked_count() == base);
    void *a = new_link();
    assert(rt_gc_tracked_count() == base + 1);
    // A refcount death untracks the instance.
    assert(rt_obj_release_check0(a) == 1);
    rt_obj_free(a);
    assert(rt_gc_tracked_count() == base);
    assert(g_dtor_calls_link == 1);
    assert(rt_obj_release_check0(value_only) == 1);
    rt_obj_free(value_only);
    g_dtor_calls = 0;
    g_dtor_calls_link = 0;

    // --- A two-member cycle with an external child -------------------------
    void *p = new_link();
    void *q = new_link();
    void *external = rt_obj_new_i64(kExternalClass, 16);
    assert(external != NULL);
    ((struct link_obj *)p)->next = q; // p owns q (the only reference)
    ((struct link_obj *)q)->next = p; // q owns p: retain for the edge
    rt_obj_retain_maybe(p);
    ((struct link_obj *)p)->extra = external; // p owns the external object
    assert(rt_gc_tracked_count() == base + 2);
    // Drop the program's own reference to p: the cycle is now unreachable.
    assert(rt_obj_release_check0(p) == 0);
    assert(g_dtor_calls == 0);

    const int64_t freed = rt_gc_collect();
    assert(freed == 2);
    assert(rt_gc_tracked_count() == base);
    // Each member's destructor ran exactly once ...
    assert(g_dtor_calls_link == 2);
    // ... and the external child (value-only, class id 9) died exactly once
    // through p's destructor, not through a second traverse-release.
    assert(g_dtor_calls_external == 1);
    assert(g_dtor_calls == 3);

    // --- A cycle through a self reference ---------------------------------
    void *s = new_link();
    ((struct link_obj *)s)->next = s;
    rt_obj_retain_maybe(s);
    assert(rt_obj_release_check0(s) == 0);
    g_dtor_calls_link = 0;
    assert(rt_gc_collect() == 1);
    assert(g_dtor_calls_link == 1);
    assert(rt_gc_tracked_count() == base);

    // --- A live instance survives a collection -----------------------------
    void *live = new_link();
    void *child = new_link();
    ((struct link_obj *)live)->next = child; // live owns child's only ref
    assert(rt_gc_collect() == 0);
    assert(rt_gc_tracked_count() == base + 2);
    g_dtor_calls_link = 0;
    assert(rt_obj_release_check0(live) == 1);
    rt_obj_free(live); // releases child through the hook
    assert(g_dtor_calls_link == 2);
    assert(rt_gc_tracked_count() == base);

    rt_obj_set_class_dtor_hook(NULL);
    rt_obj_class_layout_shutdown();
    assert(rt_obj_class_layout_get(kLinkClass) == NULL);
    return 0;
}
