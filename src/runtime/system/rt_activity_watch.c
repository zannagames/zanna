//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/system/rt_activity_watch.c
// Purpose: Multiplexes Process and PTY readiness probes on one runtime worker.
//
// Key invariants:
//   - Probe callbacks are nonblocking and serialized with unregistration.
//   - Readiness coalesces until the owning consumer rearms the registration.
//   - Worker initialization is process-wide and race-free.
//
// Ownership/Lifetime:
//   - Registrations own their retained wake targets and list nodes.
//   - The detached process-wide worker and synchronization primitives live
//     until operating-system process teardown.
//
// Links: src/runtime/system/rt_activity_watch.h,
//        src/runtime/system/rt_process.c, src/runtime/system/rt_pty.c
//
//===----------------------------------------------------------------------===//

#include "rt_activity_watch.h"

#include "rt_platform.h"

#include <stdlib.h>

#if RT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

struct rt_activity_watch {
    struct rt_activity_watch *next;
    rt_activity_probe probe;
    void *context;
    rt_activity_wake_target *target;
    int armed;
};

static rt_activity_watch *activity_watches;
static int activity_watch_ready;

#if RT_PLATFORM_WINDOWS
static INIT_ONCE activity_watch_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION activity_watch_lock;

static DWORD WINAPI activity_watch_main(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(16);
        EnterCriticalSection(&activity_watch_lock);
        for (rt_activity_watch *watch = activity_watches; watch; watch = watch->next) {
            if (watch->armed && watch->probe(watch->context)) {
                watch->armed = 0;
                rt_activity_wake_signal(watch->target);
            }
        }
        LeaveCriticalSection(&activity_watch_lock);
    }
}

static BOOL CALLBACK activity_watch_initialize(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once;
    (void)param;
    (void)context;
    InitializeCriticalSection(&activity_watch_lock);
    HANDLE thread = CreateThread(NULL, 0, activity_watch_main, NULL, 0, NULL);
    if (!thread)
        return TRUE;
    CloseHandle(thread);
    activity_watch_ready = 1;
    return TRUE;
}

static int activity_watch_ensure_ready(void) {
    (void)InitOnceExecuteOnce(&activity_watch_once, activity_watch_initialize, NULL, NULL);
    return activity_watch_ready;
}

#define ACTIVITY_WATCH_LOCK() EnterCriticalSection(&activity_watch_lock)
#define ACTIVITY_WATCH_UNLOCK() LeaveCriticalSection(&activity_watch_lock)

#else

static pthread_once_t activity_watch_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t activity_watch_lock = PTHREAD_MUTEX_INITIALIZER;

static void *activity_watch_main(void *unused) {
    (void)unused;
    const struct timespec interval = {0, 16000000L};
    for (;;) {
        (void)nanosleep(&interval, NULL);
        (void)pthread_mutex_lock(&activity_watch_lock);
        for (rt_activity_watch *watch = activity_watches; watch; watch = watch->next) {
            if (watch->armed && watch->probe(watch->context)) {
                watch->armed = 0;
                rt_activity_wake_signal(watch->target);
            }
        }
        (void)pthread_mutex_unlock(&activity_watch_lock);
    }
}

static void activity_watch_initialize(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, activity_watch_main, NULL) != 0)
        return;
    (void)pthread_detach(thread);
    activity_watch_ready = 1;
}

static int activity_watch_ensure_ready(void) {
    (void)pthread_once(&activity_watch_once, activity_watch_initialize);
    return activity_watch_ready;
}

#define ACTIVITY_WATCH_LOCK() (void)pthread_mutex_lock(&activity_watch_lock)
#define ACTIVITY_WATCH_UNLOCK() (void)pthread_mutex_unlock(&activity_watch_lock)

#endif

rt_activity_watch *rt_activity_watch_register(rt_activity_probe probe,
                                              void *context,
                                              rt_activity_wake_target *target) {
    if (!probe || !context || !target || !activity_watch_ensure_ready())
        return NULL;
    rt_activity_watch *watch = (rt_activity_watch *)calloc(1, sizeof(*watch));
    if (!watch)
        return NULL;
    watch->probe = probe;
    watch->context = context;
    watch->target = rt_activity_wake_retain(target);
    watch->armed = 1;
    if (!watch->target) {
        free(watch);
        return NULL;
    }
    ACTIVITY_WATCH_LOCK();
    watch->next = activity_watches;
    activity_watches = watch;
    ACTIVITY_WATCH_UNLOCK();
    return watch;
}

void rt_activity_watch_rearm(rt_activity_watch *watch) {
    if (!watch || !activity_watch_ready)
        return;
    ACTIVITY_WATCH_LOCK();
    watch->armed = 1;
    ACTIVITY_WATCH_UNLOCK();
}

void rt_activity_watch_unregister(rt_activity_watch *watch) {
    if (!watch || !activity_watch_ready)
        return;
    ACTIVITY_WATCH_LOCK();
    rt_activity_watch **slot = &activity_watches;
    while (*slot && *slot != watch)
        slot = &(*slot)->next;
    if (*slot == watch)
        *slot = watch->next;
    ACTIVITY_WATCH_UNLOCK();
    rt_activity_wake_release(watch->target);
    free(watch);
}
