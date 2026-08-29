//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_monitor_win.c
// Purpose: Windows implementation of the monitor (mutual-exclusion) runtime, built on
//   SRWLOCK + CONDITION_VARIABLE. Compiled only on _WIN32; the POSIX build
//   uses rt_monitor_posix.c. Shared helpers live in rt_monitor_internal.h.
//
// Key invariants:
//   - Windows wait results distinguish timeout from native failure and preserve
//     re-entrant ownership depth across pause/resume operations.
//   - Waiter publication and removal occur while the monitor entry is locked.
// Ownership/Lifetime:
//   - A monitor entry owns its waiter nodes and synchronization primitives until
//     removal by the shared striped monitor table.
//
// Links: rt_monitor_internal.h (shared), rt_monitor_posix.c (POSIX impl), rt_threads.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements reentrant object monitors with Windows synchronization.
/// @details Per-object monitor state is stored in a striped global table.
///          Contending threads and condition waiters use FIFO queues with
///          per-waiter condition variables, preserving recursion depth across
///          Wait/reacquire cycles. Timed operations use monotonic Win32 tick
///          deadlines and distinguish timeout from native failure.

#include "rt_monitor_internal.h"

#include "rt_platform.h"
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include "rt_win32_wait.h"
#include <windows.h>

/// @brief Waiter state enumeration for Windows.
enum {
    RT_MON_WAITER_WAITING_PAUSE = 0, ///< Thread called Wait(), waiting for Pause signal.
    RT_MON_WAITER_WAITING_LOCK = 1,  ///< Thread waiting to acquire the lock.
    RT_MON_WAITER_ACQUIRED = 2,      ///< Thread has been granted ownership.
    RT_MON_WAITER_CANCELLED = 3,     ///< Monitor was retired while the thread was waiting.
};

/// @brief Represents a thread waiting on a monitor (Windows version).
typedef struct RtMonitorWaiter {
    struct RtMonitorWaiter *next; ///< Next waiter in queue (singly linked).
    CONDITION_VARIABLE cv;        ///< Per-waiter condition variable.
    DWORD threadId;               ///< The waiting thread's ID.
    int state;                    ///< Current state (from enum above).
    size_t desired_recursion;     ///< Recursion count to restore on acquisition.
} RtMonitorWaiter;

/// @brief The monitor state associated with an object (Windows version).
typedef struct RtMonitor {
    CRITICAL_SECTION cs;        ///< Critical section protecting all monitor state.
    DWORD owner;                ///< Current owner thread ID.
    int owner_valid;            ///< Non-zero if monitor is currently owned.
    size_t recursion;           ///< Re-entry count for owner.
    RtMonitorWaiter *acq_head;  ///< Head of acquisition queue.
    RtMonitorWaiter *acq_tail;  ///< Tail of acquisition queue.
    RtMonitorWaiter *wait_head; ///< Head of wait queue.
    RtMonitorWaiter *wait_tail; ///< Tail of wait queue.
    int retired;                ///< Monitor owner object is being finalized.
} RtMonitor;

/// @brief Hash table entry mapping object address to monitor.
typedef struct RtMonitorEntry {
    void *key;                   ///< Object address (hash key).
    int retired;                 ///< Object was finalized while monitor was busy.
    struct RtMonitorEntry *next; ///< Next entry in hash chain.
    RtMonitor monitor;           ///< The monitor state.
} RtMonitorEntry;

/// @brief Mark every waiter in a detached queue as canceled and wake it.
/// @param w Head of the detached waiter chain, or NULL.
static void monitor_cancel_queue(RtMonitorWaiter *w);

#define RT_MONITOR_BUCKETS 4096u
#define RT_MONITOR_LOCK_STRIPES 64u

static SRWLOCK g_monitor_table_locks[RT_MONITOR_LOCK_STRIPES] = {SRWLOCK_INIT};
static RtMonitorEntry *g_monitor_table[RT_MONITOR_BUCKETS];

/// @brief Return the statically initialized SRW stripe for one hash bucket.
/// @details SRW locks require no fallible lazy initialization and independent
///          buckets usually map to different stripes, avoiding the former
///          process-wide monitor-table critical section.
/// @param bucket Monitor hash bucket index.
/// @return Address of the SRW lock protecting that bucket's stripe.
static SRWLOCK *monitor_table_lock_for(size_t bucket) {
    return &g_monitor_table_locks[bucket & (RT_MONITOR_LOCK_STRIPES - 1u)];
}

/// @brief Hash a pointer to a monitor-table bucket index.
///
/// Drops the low 4 bits (object pointers are typically aligned),
/// folds the upper half into the lower half, and multiplies by the
/// fractional golden-ratio prime — a Knuth-style mixing function
/// that gives a uniform distribution across the table.
/// @param p Object address used as the monitor key.
/// @return Bucket index in `[0, RT_MONITOR_BUCKETS)`.
static size_t hash_ptr(void *p) {
    uintptr_t x = (uintptr_t)p;
    x >>= 4;
    x ^= x >> 16;
    x *= 0x9E3779B97F4A7C15ull;
    return (size_t)(x & (RT_MONITOR_BUCKETS - 1u));
}

/// @brief Locate (or lazily create) the monitor associated with `obj`.
///
/// Walks the hash chain at `hash_ptr(obj)`. If no entry exists, a
/// new RtMonitor is allocated, initialised, and prepended to the
/// chain — all under the table critical section so concurrent
/// callers see at most one node per object.
/// @param obj Non-NULL object address used as the table key.
/// @return Pointer to the monitor (never NULL — traps on alloc failure).
static RtMonitor *get_monitor_for(void *obj) {
    size_t idx = hash_ptr(obj);
    SRWLOCK *table_lock = monitor_table_lock_for(idx);

    AcquireSRWLockExclusive(table_lock);
    RtMonitorEntry *it = g_monitor_table[idx];
    while (it) {
        if (it->key == obj && !it->retired) {
            ReleaseSRWLockExclusive(table_lock);
            return &it->monitor;
        }
        it = it->next;
    }

    RtMonitorEntry *node = (RtMonitorEntry *)calloc(1, sizeof(*node));
    if (!node) {
        ReleaseSRWLockExclusive(table_lock);
        rt_trap("rt_monitor: alloc failed");
        return NULL;
    }
    if (!InitializeCriticalSectionEx(&node->monitor.cs, 0, 0)) {
        free(node);
        ReleaseSRWLockExclusive(table_lock);
        rt_trap("rt_monitor: monitor critical-section initialization failed");
        return NULL;
    }
    node->key = obj;
    node->next = g_monitor_table[idx];
    g_monitor_table[idx] = node;

    ReleaseSRWLockExclusive(table_lock);
    return &node->monitor;
}

/// @brief Retire or remove the monitor associated with a finalized object.
/// @details An idle entry is unlinked and destroyed immediately. A busy entry
///          is marked retired, both waiter queues are canceled, and the last
///          exiting owner later performs destruction.
/// @param obj Object address whose active monitor entry should be retired or removed.
void rt_monitor_forget(void *obj) {
    if (!obj)
        return;
    size_t idx = hash_ptr(obj);
    SRWLOCK *table_lock = monitor_table_lock_for(idx);

    AcquireSRWLockExclusive(table_lock);
    RtMonitorEntry **link = &g_monitor_table[idx];
    RtMonitorEntry *node = *link;
    while (node && (node->key != obj || node->retired)) {
        link = &node->next;
        node = node->next;
    }
    if (!node) {
        ReleaseSRWLockExclusive(table_lock);
        return;
    }
    EnterCriticalSection(&node->monitor.cs);
    if (node->monitor.owner_valid || node->monitor.acq_head || node->monitor.wait_head) {
        node->retired = 1;
        node->monitor.retired = 1;
        RtMonitorWaiter *acq = node->monitor.acq_head;
        RtMonitorWaiter *wait = node->monitor.wait_head;
        node->monitor.acq_head = NULL;
        node->monitor.acq_tail = NULL;
        node->monitor.wait_head = NULL;
        node->monitor.wait_tail = NULL;
        monitor_cancel_queue(acq);
        monitor_cancel_queue(wait);
        LeaveCriticalSection(&node->monitor.cs);
        ReleaseSRWLockExclusive(table_lock);
        return;
    }

    *link = node->next;
    LeaveCriticalSection(&node->monitor.cs);
    ReleaseSRWLockExclusive(table_lock);

    DeleteCriticalSection(&node->monitor.cs);
    free(node);
}

/// @brief Free a retired monitor-table entry once it is idle.
/// @details Monitor entries enter the "retired" state when the owning
///          object is finalized but a thread is still inside Wait/Enter
///          (potentially blocked on the underlying primitive). The
///          retired entry is kept until ownership and both waiter queues are
///          empty, then unlinked under its table stripe and destroyed.
/// @param obj Object address used to locate the retired table entry.
/// @param monitor Expected monitor payload, preventing removal of a replacement entry.
static void monitor_cleanup_retired_if_idle(void *obj, RtMonitor *monitor) {
    if (!obj || !monitor)
        return;
    size_t idx = hash_ptr(obj);
    SRWLOCK *table_lock = monitor_table_lock_for(idx);

    AcquireSRWLockExclusive(table_lock);
    RtMonitorEntry **link = &g_monitor_table[idx];
    RtMonitorEntry *node = *link;
    while (node && (node->key != obj || &node->monitor != monitor)) {
        link = &node->next;
        node = node->next;
    }
    if (!node) {
        ReleaseSRWLockExclusive(table_lock);
        return;
    }

    EnterCriticalSection(&node->monitor.cs);
    if (!node->retired || node->monitor.owner_valid || node->monitor.acq_head ||
        node->monitor.wait_head) {
        LeaveCriticalSection(&node->monitor.cs);
        ReleaseSRWLockExclusive(table_lock);
        return;
    }

    *link = node->next;
    LeaveCriticalSection(&node->monitor.cs);
    ReleaseSRWLockExclusive(table_lock);

    DeleteCriticalSection(&node->monitor.cs);
    free(node);
}

/// @brief True if the calling thread (`self`) currently owns monitor `m`.
/// @param m Monitor whose owner state is inspected.
/// @param self Calling thread identifier.
/// @return Non-zero when @p self is the recorded owner.
static int monitor_is_owner(const RtMonitor *m, DWORD self) {
    return m->owner_valid && m->owner == self;
}

/// @brief Hand the lock to the head of the FIFO acquisition queue and wake it.
///
/// Called by `rt_monitor_exit` once the recursion count drops to
/// zero. Restores the new owner's prior recursion depth (used by
/// `Wait`, which releases an arbitrarily deep nesting and reclaims
/// it on wakeup) and signals their per-waiter condvar.
/// @param m Locked monitor whose next acquisition waiter receives ownership.
static void monitor_grant_next_waiter(RtMonitor *m) {
    RtMonitorWaiter *w = m->acq_head;
    if (!w)
        return;
    m->acq_head = w->next;
    if (!m->acq_head)
        m->acq_tail = NULL;

    m->owner = w->threadId;
    m->owner_valid = 1;
    m->recursion = w->desired_recursion;

    w->state = RT_MON_WAITER_ACQUIRED;
    WakeConditionVariable(&w->cv);
}

// ---------------------------------------------------------------------------
// FIFO queue helpers — append/remove on the singly-linked acquisition
// (lock-contention) and wait (condition-variable) queues. All callers
// already hold the monitor's critical section, so no extra locking is
// needed inside these helpers.
// ---------------------------------------------------------------------------

/// @brief Append `w` to the FIFO acquisition queue (lock contention).
/// @param m Locked monitor receiving the waiter.
/// @param w Waiter node to append.
static void monitor_enqueue_acq(RtMonitor *m, RtMonitorWaiter *w) {
    w->next = NULL;
    if (m->acq_tail) {
        m->acq_tail->next = w;
        m->acq_tail = w;
    } else {
        m->acq_head = w;
        m->acq_tail = w;
    }
}

/// @brief Splice `w` out of the acquisition queue (used on timeout/abort).
/// @param m Locked monitor whose acquisition queue is searched.
/// @param w Waiter node to remove if present.
static void monitor_remove_acq(RtMonitor *m, RtMonitorWaiter *w) {
    RtMonitorWaiter *prev = NULL;
    RtMonitorWaiter *cur = m->acq_head;
    while (cur) {
        if (cur == w) {
            if (prev)
                prev->next = cur->next;
            else
                m->acq_head = cur->next;
            if (m->acq_tail == cur)
                m->acq_tail = prev;
            cur->next = NULL;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

/// @brief Append `w` to the FIFO condition-wait queue (Wait / WaitFor).
/// @param m Locked monitor receiving the waiter.
/// @param w Waiter node to append.
static void monitor_enqueue_wait(RtMonitor *m, RtMonitorWaiter *w) {
    w->next = NULL;
    if (m->wait_tail) {
        m->wait_tail->next = w;
        m->wait_tail = w;
    } else {
        m->wait_head = w;
        m->wait_tail = w;
    }
}

/// @brief Splice `w` out of the wait queue (timeout, signal-then-cancel).
/// @param m Locked monitor whose condition-wait queue is searched.
/// @param w Waiter node to remove if present.
static void monitor_remove_wait(RtMonitor *m, RtMonitorWaiter *w) {
    RtMonitorWaiter *prev = NULL;
    RtMonitorWaiter *cur = m->wait_head;
    while (cur) {
        if (cur == w) {
            if (prev)
                prev->next = cur->next;
            else
                m->wait_head = cur->next;
            if (m->wait_tail == cur)
                m->wait_tail = prev;
            cur->next = NULL;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

/// @brief Wake and detach every waiter in a monitor's acquire/wait queue with
///        a cancellation result (used when the monitor is destroyed or the
///        queue is torn down) so no thread is left blocked.
/// @details Waiter nodes are owned by blocked thread stacks and are not freed
///          here. The caller holds the monitor critical section while changing
///          their states and signaling their private condition variables.
/// @note Defined once per platform backend branch; both copies are identical.
/// @param w Head of the detached waiter chain, or NULL.
static void monitor_cancel_queue(RtMonitorWaiter *w) {
    while (w) {
        RtMonitorWaiter *next = w->next;
        w->next = NULL;
        w->state = RT_MON_WAITER_CANCELLED;
        WakeConditionVariable(&w->cv);
        w = next;
    }
}

/// @brief Acquire monitor `m` for thread `self`, blocking up to `timeout_ms` if `timed`.
///
/// Three fast paths run before any blocking occurs:
///   1. Re-entry (`self` already owns) — bumps recursion and returns.
///   2. Uncontended (no owner, empty queue) — claims the lock immediately.
///   3. Contended — appends a `RtMonitorWaiter` to the FIFO acquisition
///      queue and sleeps on its per-waiter `CONDITION_VARIABLE` until
///      `monitor_grant_next_waiter` flips its state.
/// On timeout the waiter is spliced out and we return without owning
/// the lock. Traps with a "null monitor" message if `m` is NULL.
/// @param m Locked monitor to acquire recursively or enqueue against.
/// @param self Calling thread identifier.
/// @param timeout_ms Maximum wait in milliseconds when @p timed is non-zero.
/// @param timed Whether acquisition uses @p timeout_ms instead of waiting indefinitely.
/// @return One after ownership is acquired, or zero after timeout or a reported failure.
static int monitor_enter_blocking(RtMonitor *m, DWORD self, DWORD timeout_ms, int timed) {
    if (!m) {
        rt_trap("rt_monitor: null monitor");
        return 0;
    }
    if (m->retired) {
        rt_trap("Monitor.Enter: object finalized");
        return 0;
    }

    if (monitor_is_owner(m, self)) {
        if (m->recursion == SIZE_MAX) {
            rt_trap("Monitor.Enter: recursion overflow");
            return 0;
        }
        m->recursion += 1;
        return 1;
    }

    if (!m->owner_valid && m->acq_head == NULL) {
        m->owner = self;
        m->owner_valid = 1;
        m->recursion = 1;
        return 1;
    }

    RtMonitorWaiter w = {0};
    InitializeConditionVariable(&w.cv);
    w.threadId = self;
    w.state = RT_MON_WAITER_WAITING_LOCK;
    w.desired_recursion = 1;
    monitor_enqueue_acq(m, &w);

    ULONGLONG start = GetTickCount64();
    while (w.state != RT_MON_WAITER_ACQUIRED) {
        DWORD wait_time = INFINITE;
        if (timed) {
            ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= timeout_ms) {
                // Timeout
                if (w.state != RT_MON_WAITER_ACQUIRED) {
                    monitor_remove_acq(m, &w);
                    return 0;
                }
                break;
            }
            wait_time = (DWORD)(timeout_ms - elapsed);
        }

        BOOL ok = SleepConditionVariableCS(&w.cv, &m->cs, wait_time);
        if (!ok && w.state != RT_MON_WAITER_ACQUIRED) {
            DWORD error = GetLastError();
            if (error == ERROR_TIMEOUT) {
                monitor_remove_acq(m, &w);
                return 0;
            }
            monitor_remove_acq(m, &w);
            rt_trap("Monitor.Enter: condition wait failed");
            return 0;
        }
        if (w.state == RT_MON_WAITER_CANCELLED) {
            rt_trap("Monitor.Enter: object finalized while waiting");
            return 0;
        }
    }
    return w.state == RT_MON_WAITER_ACQUIRED ? 1 : 0;
}

/// @brief Acquire an object's reentrant monitor, blocking until available.
/// @details The object is retained before lookup and that reference remains
///          held for the acquired ownership level until @ref rt_monitor_exit.
///          Contended callers enter the FIFO acquisition queue. NULL, retired
///          monitors, recursion overflow, and native wait failures trap.
/// @param obj Object whose monitor should be acquired.
void rt_monitor_enter(void *obj) {
    if (!obj)
        rt_trap("Monitor.Enter: null object");
    if (!obj)
        return;
    rt_obj_retain_maybe(obj);
    RtMonitor *m = get_monitor_for(obj);
    if (!m) {
        if (rt_obj_release_check0(obj))
            rt_obj_free(obj);
        return;
    }
    EnterCriticalSection(&m->cs);
    char saved_error[256];
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        monitor_save_trap_error(saved_error, sizeof(saved_error), "Monitor.Enter: failed");
        rt_trap_clear_recovery();
        LeaveCriticalSection(&m->cs);
        monitor_release_enter_ref(obj);
        rt_trap(saved_error);
        return;
    }
    int acquired = monitor_enter_blocking(m, GetCurrentThreadId(), 0, 0);
    rt_trap_clear_recovery();
    LeaveCriticalSection(&m->cs);
    if (!acquired)
        monitor_release_enter_ref(obj);
}

/// @brief Try to acquire an object's reentrant monitor without blocking.
/// @details Recursive or uncontended acquisition succeeds and retains the
///          object until the matching exit. Existing ownership or queued
///          contenders cause an immediate zero result.
/// @param obj Object whose monitor should be acquired.
/// @return One after acquisition, otherwise zero when busy or setup cannot complete.
int8_t rt_monitor_try_enter(void *obj) {
    if (!obj)
        rt_trap("Monitor.Enter: null object");
    if (!obj)
        return 0;
    rt_obj_retain_maybe(obj);
    RtMonitor *m = get_monitor_for(obj);
    if (!m) {
        if (rt_obj_release_check0(obj))
            rt_obj_free(obj);
        return 0;
    }
    DWORD self = GetCurrentThreadId();

    EnterCriticalSection(&m->cs);
    if (m->retired) {
        LeaveCriticalSection(&m->cs);
        if (rt_obj_release_check0(obj))
            rt_obj_free(obj);
        rt_trap("Monitor.Enter: object finalized");
        return 0;
    }
    if (monitor_is_owner(m, self)) {
        if (m->recursion == SIZE_MAX) {
            LeaveCriticalSection(&m->cs);
            if (rt_obj_release_check0(obj))
                rt_obj_free(obj);
            rt_trap("Monitor.Enter: recursion overflow");
            return 0;
        }
        m->recursion += 1;
        LeaveCriticalSection(&m->cs);
        return 1;
    }
    if (!m->owner_valid && m->acq_head == NULL) {
        m->owner = self;
        m->owner_valid = 1;
        m->recursion = 1;
        LeaveCriticalSection(&m->cs);
        return 1;
    }
    LeaveCriticalSection(&m->cs);
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
    return 0;
}

/// @brief Try to acquire an object's reentrant monitor within a timeout.
/// @details An unowned monitor or recursive entry succeeds immediately.
///          Otherwise the caller joins the FIFO acquisition queue and waits
///          against a monotonic tick deadline. Negative durations become zero.
///          Successful ownership retains @p obj until the matching exit.
/// @param obj Object whose monitor should be acquired.
/// @param ms Maximum wait in milliseconds.
/// @return One after acquisition, or zero on timeout or recoverable setup failure.
int8_t rt_monitor_try_enter_for(void *obj, int64_t ms) {
    if (!obj)
        rt_trap("Monitor.Enter: null object");
    if (!obj)
        return 0;
    if (ms < 0)
        ms = 0;
    rt_obj_retain_maybe(obj);
    RtMonitor *m = get_monitor_for(obj);
    if (!m) {
        if (rt_obj_release_check0(obj))
            rt_obj_free(obj);
        return 0;
    }
    DWORD self = GetCurrentThreadId();

    EnterCriticalSection(&m->cs);
    if (m->retired) {
        LeaveCriticalSection(&m->cs);
        if (rt_obj_release_check0(obj))
            rt_obj_free(obj);
        rt_trap("Monitor.Enter: object finalized");
        return 0;
    }
    if (monitor_is_owner(m, self)) {
        if (m->recursion == SIZE_MAX) {
            LeaveCriticalSection(&m->cs);
            if (rt_obj_release_check0(obj))
                rt_obj_free(obj);
            rt_trap("Monitor.Enter: recursion overflow");
            return 0;
        }
        m->recursion += 1;
        LeaveCriticalSection(&m->cs);
        return 1;
    }
    if (!m->owner_valid && m->acq_head == NULL) {
        m->owner = self;
        m->owner_valid = 1;
        m->recursion = 1;
        LeaveCriticalSection(&m->cs);
        return 1;
    }

    RtMonitorWaiter w = {0};
    InitializeConditionVariable(&w.cv);
    w.threadId = self;
    w.state = RT_MON_WAITER_WAITING_LOCK;
    w.desired_recursion = 1;
    monitor_enqueue_acq(m, &w);

    ULONGLONG deadline = rt_win32_deadline_from_now_ms(ms);
    while (w.state != RT_MON_WAITER_ACQUIRED) {
        DWORD wait_time = rt_win32_wait_slice_until(deadline);
        if (wait_time == 0) {
            monitor_remove_acq(m, &w);
            LeaveCriticalSection(&m->cs);
            if (rt_obj_release_check0(obj))
                rt_obj_free(obj);
            return 0;
        }

        BOOL ok = SleepConditionVariableCS(&w.cv, &m->cs, wait_time);
        if (!ok && w.state != RT_MON_WAITER_ACQUIRED) {
            DWORD error = GetLastError();
            if (error == ERROR_TIMEOUT)
                continue;
            monitor_remove_acq(m, &w);
            LeaveCriticalSection(&m->cs);
            if (rt_obj_release_check0(obj))
                rt_obj_free(obj);
            rt_trap("Monitor.Enter: condition wait failed");
            return 0;
        }
        if (w.state == RT_MON_WAITER_CANCELLED) {
            LeaveCriticalSection(&m->cs);
            if (rt_obj_release_check0(obj))
                rt_obj_free(obj);
            rt_trap("Monitor.Enter: object finalized while waiting");
            return 0;
        }
    }

    LeaveCriticalSection(&m->cs);
    return 1;
}

/// @brief Release one recursion level of an owned object monitor.
/// @details The last level transfers ownership to the oldest acquisition
///          waiter, cleans up an idle retired entry, and releases the object
///          reference held by the corresponding successful enter. Non-owners
///          and NULL objects trap.
/// @param obj Object whose monitor should be released.
void rt_monitor_exit(void *obj) {
    if (!obj)
        rt_trap("Monitor.Exit: null object");
    if (!obj)
        return;
    RtMonitor *m = get_monitor_for(obj);
    if (!m)
        return;
    DWORD self = GetCurrentThreadId();

    EnterCriticalSection(&m->cs);
    if (!monitor_is_owner(m, self)) {
        LeaveCriticalSection(&m->cs);
        rt_trap("Monitor.Exit: not owner");
        return;
    }
    if (m->recursion > 1) {
        m->recursion -= 1;
        LeaveCriticalSection(&m->cs);
        if (rt_obj_release_check0(obj))
            rt_obj_free(obj);
        return;
    }

    m->owner_valid = 0;
    m->recursion = 0;
    monitor_grant_next_waiter(m);
    LeaveCriticalSection(&m->cs);
    monitor_cleanup_retired_if_idle(obj, m);
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Release an owned monitor and wait until another thread notifies it.
/// @details The current recursion depth is saved, ownership is fully released,
///          and the thread joins the condition-wait queue. Notification moves
///          it to the FIFO acquisition queue; the saved recursion depth is
///          restored before return. Ownership, retirement, and wait failures trap.
/// @param obj Object whose monitor must be owned by the calling thread.
void rt_monitor_wait(void *obj) {
    if (!obj)
        rt_trap("Monitor.Wait: not owner");
    if (!obj)
        return;
    RtMonitor *m = get_monitor_for(obj);
    if (!m)
        return;
    DWORD self = GetCurrentThreadId();
    EnterCriticalSection(&m->cs);

    if (!monitor_is_owner(m, self)) {
        LeaveCriticalSection(&m->cs);
        rt_trap("Monitor.Wait: not owner");
        return;
    }

    const size_t saved_recursion = m->recursion;

    RtMonitorWaiter w = {0};
    InitializeConditionVariable(&w.cv);
    w.threadId = self;
    w.state = RT_MON_WAITER_WAITING_PAUSE;
    w.desired_recursion = saved_recursion;
    monitor_enqueue_wait(m, &w);

    // Queue before releasing so Pause/PauseAll cannot miss this waiter.
    m->owner_valid = 0;
    m->recursion = 0;
    monitor_grant_next_waiter(m);

    int wait_failed = 0;
    while (w.state == RT_MON_WAITER_WAITING_PAUSE) {
        if (!SleepConditionVariableCS(&w.cv, &m->cs, INFINITE)) {
            wait_failed = 1;
            break;
        }
    }

    if (wait_failed && w.state == RT_MON_WAITER_WAITING_PAUSE) {
        monitor_remove_wait(m, &w);
        w.state = RT_MON_WAITER_WAITING_LOCK;
        monitor_enqueue_acq(m, &w);
        if (!m->owner_valid && m->acq_head)
            monitor_grant_next_waiter(m);
    }

    // Wait for re-acquisition.
    while (w.state != RT_MON_WAITER_ACQUIRED && w.state != RT_MON_WAITER_CANCELLED) {
        if (!SleepConditionVariableCS(&w.cv, &m->cs, INFINITE)) {
            wait_failed = 1;
            if (w.state == RT_MON_WAITER_WAITING_LOCK) {
                monitor_remove_acq(m, &w);
                if (!m->owner_valid && m->acq_head)
                    monitor_grant_next_waiter(m);
            }
            break;
        }
    }

    LeaveCriticalSection(&m->cs);
    if (w.state == RT_MON_WAITER_CANCELLED)
        rt_trap("Monitor.Wait: object finalized while waiting");
    else if (wait_failed)
        rt_trap("Monitor.Wait: condition wait failed");
}

/// @brief Release an owned monitor and wait for notification up to a deadline.
/// @details Timeout and notification both move the thread into the FIFO
///          acquisition queue, so the original recursion depth is reacquired
///          before return. Negative durations are treated as zero. Ownership,
///          retirement, and native wait failures trap.
/// @param obj Object whose monitor must be owned by the calling thread.
/// @param ms Maximum notification wait in milliseconds.
/// @return One when notified and reacquired, or zero after timeout or a native
///         wait failure handled by a returning trap hook.
int8_t rt_monitor_wait_for(void *obj, int64_t ms) {
    if (!obj)
        rt_trap("Monitor.Wait: not owner");
    if (!obj)
        return 0;
    RtMonitor *m = get_monitor_for(obj);
    if (!m)
        return 0;
    DWORD self = GetCurrentThreadId();
    EnterCriticalSection(&m->cs);

    if (!monitor_is_owner(m, self)) {
        LeaveCriticalSection(&m->cs);
        rt_trap("Monitor.Wait: not owner");
        return 0;
    }

    if (ms < 0)
        ms = 0;

    const size_t saved_recursion = m->recursion;

    RtMonitorWaiter w = {0};
    InitializeConditionVariable(&w.cv);
    w.threadId = self;
    w.state = RT_MON_WAITER_WAITING_PAUSE;
    w.desired_recursion = saved_recursion;
    monitor_enqueue_wait(m, &w);

    // Queue before releasing so Pause/PauseAll cannot miss this waiter.
    m->owner_valid = 0;
    m->recursion = 0;
    monitor_grant_next_waiter(m);

    int timed_out = 0;
    int wait_failed = 0;
    ULONGLONG deadline = rt_win32_deadline_from_now_ms(ms);
    while (w.state == RT_MON_WAITER_WAITING_PAUSE) {
        DWORD wait_time = rt_win32_wait_slice_until(deadline);
        if (wait_time == 0) {
            timed_out = 1;
            break;
        }
        BOOL ok = SleepConditionVariableCS(&w.cv, &m->cs, wait_time);
        if (!ok && w.state == RT_MON_WAITER_WAITING_PAUSE) {
            DWORD error = GetLastError();
            if (error == ERROR_TIMEOUT)
                continue;
            wait_failed = 1;
            break;
        }
    }

    if (timed_out || wait_failed) {
        // Timeout while still in the wait queue: remove and begin fair re-acquire.
        monitor_remove_wait(m, &w);
        w.state = RT_MON_WAITER_WAITING_LOCK;
        monitor_enqueue_acq(m, &w);
        if (!m->owner_valid && m->acq_head)
            monitor_grant_next_waiter(m);
    }

    while (w.state != RT_MON_WAITER_ACQUIRED && w.state != RT_MON_WAITER_CANCELLED) {
        if (!SleepConditionVariableCS(&w.cv, &m->cs, INFINITE)) {
            wait_failed = 1;
            if (w.state == RT_MON_WAITER_WAITING_LOCK) {
                monitor_remove_acq(m, &w);
                if (!m->owner_valid && m->acq_head)
                    monitor_grant_next_waiter(m);
            }
            break;
        }
    }

    LeaveCriticalSection(&m->cs);
    if (w.state == RT_MON_WAITER_CANCELLED)
        rt_trap("Monitor.Wait: object finalized while waiting");
    else if (wait_failed)
        rt_trap("Monitor.Wait: condition wait failed");
    return (timed_out || wait_failed) ? 0 : 1;
}

/// @brief Move the oldest condition waiter toward monitor reacquisition.
/// @details The waiter is appended to the FIFO acquisition queue and signaled,
///          but the calling owner retains the monitor until it exits. With no
///          waiter this is a no-op. NULL and non-owner calls trap.
/// @param obj Object whose owned monitor should notify one waiter.
void rt_monitor_pause(void *obj) {
    if (!obj)
        rt_trap("Monitor.Notify: not owner");
    if (!obj)
        return;
    RtMonitor *m = get_monitor_for(obj);
    if (!m)
        return;
    DWORD self = GetCurrentThreadId();
    EnterCriticalSection(&m->cs);

    if (!monitor_is_owner(m, self)) {
        LeaveCriticalSection(&m->cs);
        rt_trap("Monitor.Notify: not owner");
        return;
    }

    RtMonitorWaiter *w = m->wait_head;
    if (w) {
        m->wait_head = w->next;
        if (!m->wait_head)
            m->wait_tail = NULL;
        w->next = NULL;

        w->state = RT_MON_WAITER_WAITING_LOCK;
        monitor_enqueue_acq(m, w);
        WakeConditionVariable(&w->cv);
    }

    LeaveCriticalSection(&m->cs);
}

/// @brief Move every condition waiter toward monitor reacquisition.
/// @details Waiters retain their FIFO order while being appended to the
///          acquisition queue and individually signaled. The calling owner
///          keeps the monitor until exit. With no waiters this is a no-op;
///          NULL and non-owner calls trap.
/// @param obj Object whose owned monitor should notify all waiters.
void rt_monitor_pause_all(void *obj) {
    if (!obj)
        rt_trap("Monitor.NotifyAll: not owner");
    if (!obj)
        return;
    RtMonitor *m = get_monitor_for(obj);
    if (!m)
        return;
    DWORD self = GetCurrentThreadId();
    EnterCriticalSection(&m->cs);

    if (!monitor_is_owner(m, self)) {
        LeaveCriticalSection(&m->cs);
        rt_trap("Monitor.NotifyAll: not owner");
        return;
    }

    while (m->wait_head) {
        RtMonitorWaiter *w = m->wait_head;
        m->wait_head = w->next;
        if (!m->wait_head)
            m->wait_tail = NULL;
        w->next = NULL;

        w->state = RT_MON_WAITER_WAITING_LOCK;
        monitor_enqueue_acq(m, w);
        WakeConditionVariable(&w->cv);
    }

    LeaveCriticalSection(&m->cs);
}

#endif // defined(_WIN32)
