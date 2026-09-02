//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/oop/rt_class_layout.c
// Purpose: Dense per-class strong-slot table (ADR 0315). See the header.
// Key invariants:
//   - The table pointer is published with release ordering after growth, so a
//     reader that observes a class id in range observes its slots.
//   - Registration happens on the program's entry prologue before any object
//     of the class can be allocated; the allocator only reads.
// Ownership/Lifetime:
//   - Process-global; freed by rt_obj_class_layout_shutdown.
// Links: src/runtime/oop/rt_class_layout.h
//
//===----------------------------------------------------------------------===//
#include "rt_class_layout.h"

#include "rt_internal.h"
#include "rt_platform.h"

#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <sched.h>
#endif

static rt_class_layout_t *g_layouts = NULL; ///< Indexed by class id (slot 0 unused).
static int64_t g_layout_capacity = 0;       ///< Number of table entries.
static int64_t g_layout_registered = 0;     ///< Classes with a begun layout.
static int g_layout_lock = 0;

/// @brief Acquire the table spin lock (growth and registration only).
static void layout_lock(void) {
    if (__atomic_test_and_set(&g_layout_lock, __ATOMIC_ACQUIRE)) {
        do {
#if RT_PLATFORM_WINDOWS
            SwitchToThread();
#else
            sched_yield();
#endif
        } while (__atomic_test_and_set(&g_layout_lock, __ATOMIC_ACQUIRE));
    }
}

/// @brief Release the table spin lock.
static void layout_unlock(void) {
    __atomic_clear(&g_layout_lock, __ATOMIC_RELEASE);
}

/// @brief Grow the dense table so that @p class_id is in range (lock held).
/// @return 1 on success, 0 when the allocation failed.
static int layout_reserve_locked(int64_t class_id) {
    if (class_id < g_layout_capacity)
        return 1;
    int64_t new_cap = g_layout_capacity == 0 ? 64 : g_layout_capacity;
    while (new_cap <= class_id) {
        if (new_cap > INT64_MAX / 2)
            return 0;
        new_cap *= 2;
    }
    rt_class_layout_t *grown =
        (rt_class_layout_t *)calloc((size_t)new_cap, sizeof(rt_class_layout_t));
    if (!grown)
        return 0;
    if (g_layouts) {
        memcpy(grown, g_layouts, (size_t)g_layout_capacity * sizeof(rt_class_layout_t));
        free(g_layouts);
    }
    __atomic_store_n(&g_layouts, grown, __ATOMIC_RELEASE);
    __atomic_store_n(&g_layout_capacity, new_cap, __ATOMIC_RELEASE);
    return 1;
}

void rt_obj_class_layout_begin(int64_t class_id, int64_t slot_count) {
    if (class_id <= 0) {
        rt_trap("rt_obj_class_layout_begin: class id must be positive");
        return;
    }
    if (slot_count < 0) {
        rt_trap("rt_obj_class_layout_begin: negative slot count");
        return;
    }
    layout_lock();
    if (!layout_reserve_locked(class_id)) {
        layout_unlock();
        rt_trap("rt_obj_class_layout_begin: out of memory");
        return;
    }
    rt_class_layout_t *layout = &g_layouts[class_id];
    if (layout->offsets) {
        free(layout->offsets);
        layout->offsets = NULL;
    }
    if (!layout->declared) {
        layout->declared = 1;
        g_layout_registered++;
    }
    layout->count = 0;
    layout->capacity = slot_count;
    if (slot_count > 0) {
        layout->offsets = (int64_t *)calloc((size_t)slot_count, sizeof(int64_t));
        if (!layout->offsets) {
            layout->capacity = 0;
            layout_unlock();
            rt_trap("rt_obj_class_layout_begin: out of memory");
            return;
        }
    }
    layout_unlock();
}

void rt_obj_class_layout_add_slot(int64_t class_id, int64_t offset, int64_t kind) {
    if (kind != RT_CLASS_SLOT_OBJ) {
        rt_trap("rt_obj_class_layout_add_slot: unsupported slot kind");
        return;
    }
    if (offset < 0) {
        rt_trap("rt_obj_class_layout_add_slot: negative offset");
        return;
    }
    layout_lock();
    if (class_id <= 0 || class_id >= g_layout_capacity || !g_layouts ||
        !g_layouts[class_id].declared) {
        layout_unlock();
        rt_trap("rt_obj_class_layout_add_slot: class layout was not begun");
        return;
    }
    rt_class_layout_t *layout = &g_layouts[class_id];
    if (layout->count >= layout->capacity) {
        layout_unlock();
        rt_trap("rt_obj_class_layout_add_slot: more slots than declared");
        return;
    }
    layout->offsets[layout->count++] = offset;
    layout_unlock();
}

const rt_class_layout_t *rt_obj_class_layout_get(int64_t class_id) {
    if (class_id <= 0)
        return NULL;
    int64_t cap = __atomic_load_n(&g_layout_capacity, __ATOMIC_ACQUIRE);
    if (class_id >= cap)
        return NULL;
    rt_class_layout_t *table = __atomic_load_n(&g_layouts, __ATOMIC_ACQUIRE);
    if (!table)
        return NULL;
    const rt_class_layout_t *layout = &table[class_id];
    if (!layout->declared)
        return NULL;
    return layout;
}

int8_t rt_obj_class_layout_has_ref_slots(int64_t class_id) {
    const rt_class_layout_t *layout = rt_obj_class_layout_get(class_id);
    return layout && layout->count > 0 ? 1 : 0;
}

int64_t rt_obj_class_layout_count(void) {
    layout_lock();
    int64_t n = g_layout_registered;
    layout_unlock();
    return n;
}

void rt_obj_class_layout_shutdown(void) {
    layout_lock();
    if (g_layouts) {
        for (int64_t i = 0; i < g_layout_capacity; ++i)
            free(g_layouts[i].offsets);
        free(g_layouts);
    }
    g_layouts = NULL;
    g_layout_capacity = 0;
    g_layout_registered = 0;
    layout_unlock();
}
