//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/system/rt_activity_wake.c
// Purpose: Implement lifetime-safe cross-thread event-loop wake callbacks.
// Key invariants:
//   - Callback/context publication and invalidation are serialized by one lock.
//   - The final release occurs only after every producer drops its reference.
// Ownership/Lifetime:
//   - Targets use malloc ownership and an atomic intrusive reference count.
// Links: src/runtime/system/rt_activity_wake.h,
//        src/runtime/graphics/gui/rt_gui_app.c,
//        docs/adr/0281-event-driven-process-pty-gui-wakes.md
//
//===----------------------------------------------------------------------===//

/** @file rt_activity_wake.c @brief Implements ref-counted asynchronous wake targets. */

#include "rt_activity_wake.h"

#include "rt_platform.h"

#include <stdlib.h>

#if RT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

struct rt_activity_wake_target {
    volatile int references;
    rt_activity_wake_callback callback;
    void *context;
#if RT_PLATFORM_WINDOWS
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
};

#if RT_PLATFORM_WINDOWS
#define ACTIVITY_WAKE_LOCK(target) EnterCriticalSection(&(target)->lock)
#define ACTIVITY_WAKE_UNLOCK(target) LeaveCriticalSection(&(target)->lock)
#else
#define ACTIVITY_WAKE_LOCK(target) (void)pthread_mutex_lock(&(target)->lock)
#define ACTIVITY_WAKE_UNLOCK(target) (void)pthread_mutex_unlock(&(target)->lock)
#endif

rt_activity_wake_target *rt_activity_wake_new(rt_activity_wake_callback callback, void *context) {
    if (!callback)
        return NULL;
    rt_activity_wake_target *target = calloc(1, sizeof(*target));
    if (!target)
        return NULL;
#if RT_PLATFORM_WINDOWS
    InitializeCriticalSection(&target->lock);
#else
    if (pthread_mutex_init(&target->lock, NULL) != 0) {
        free(target);
        return NULL;
    }
#endif
    rt_atomic_store_i32(&target->references, 1, __ATOMIC_RELEASE);
    target->callback = callback;
    target->context = context;
    return target;
}

rt_activity_wake_target *rt_activity_wake_retain(rt_activity_wake_target *target) {
    if (!target)
        return NULL;
    (void)rt_atomic_fetch_add_i32(&target->references, 1, __ATOMIC_RELAXED);
    return target;
}

void rt_activity_wake_signal(rt_activity_wake_target *target) {
    if (!target)
        return;
    ACTIVITY_WAKE_LOCK(target);
    if (target->callback)
        target->callback(target->context);
    ACTIVITY_WAKE_UNLOCK(target);
}

void rt_activity_wake_invalidate(rt_activity_wake_target *target) {
    if (!target)
        return;
    ACTIVITY_WAKE_LOCK(target);
    target->callback = NULL;
    target->context = NULL;
    ACTIVITY_WAKE_UNLOCK(target);
}

void rt_activity_wake_release(rt_activity_wake_target *target) {
    if (!target)
        return;
    if (rt_atomic_fetch_sub_i32(&target->references, 1, __ATOMIC_ACQ_REL) != 1)
        return;
#if RT_PLATFORM_WINDOWS
    DeleteCriticalSection(&target->lock);
#else
    (void)pthread_mutex_destroy(&target->lock);
#endif
    free(target);
}
