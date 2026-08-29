//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_monitor_posix.c
// Purpose: POSIX implementation of the monitor (mutual-exclusion) runtime, built on
//   pthread mutex/condition variables (with Apple relative-timedwait). Compiled
//   on non-_WIN32 platforms; Windows uses rt_monitor_win.c. Shared helpers live
//   in rt_monitor_internal.h.
//
// Key invariants:
//   - Every pthread initialization and wait result is checked; timeout is kept
//     distinct from an unexpected native synchronization failure.
//   - Timed waits use a single monotonic or platform-relative clock domain.
// Ownership/Lifetime:
//   - A monitor entry owns its mutex, condition variables, and waiter nodes
//     until the shared monitor table removes and destroys the entry.
//
// Links: rt_monitor_internal.h (shared), rt_monitor_win.c (Windows impl), rt_threads.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements reentrant object monitors with POSIX synchronization.
/// @details Per-object monitor state is stored in a striped global table.
///          Contending threads and condition waiters use FIFO queues with
///          per-waiter condition variables, preserving recursion depth across
///          Wait/reacquire cycles. Timed waits use monotonic deadlines where
///          the platform supports them.

#include "rt_monitor_internal.h"

#include "rt_platform.h"
#if !defined(_WIN32)

#include <pthread.h>
#if defined(__APPLE__)
/// @brief Wait on a condition variable for a relative interval on macOS.
/// @param cond Condition variable to wait on.
/// @param mutex Locked mutex atomically released during the wait.
/// @param rel_time Relative timeout measured from the start of the wait.
/// @return Zero after a signal, `ETIMEDOUT` on expiry, or another pthread
///         error code.
extern int pthread_cond_timedwait_relative_np(pthread_cond_t *cond,
                                              pthread_mutex_t *mutex,
                                              const struct timespec *rel_time);
#endif

/// @brief Waiter state enumeration.
///
/// Tracks the state of a thread waiting on a monitor, used for the
/// FIFO-fair handoff mechanism.
enum {
    RT_MON_WAITER_WAITING_PAUSE = 0, ///< Thread called Wait(), waiting for Pause signal.
    RT_MON_WAITER_WAITING_LOCK = 1,  ///< Thread waiting to acquire the lock.
    RT_MON_WAITER_ACQUIRED = 2,      ///< Thread has been granted ownership.
    RT_MON_WAITER_CANCELLED = 3,     ///< Monitor was retired while the thread was waiting.
};

/// @brief Represents a thread waiting on a monitor.
///
/// Each waiting thread gets its own RtMonitorWaiter node with a personal
/// condition variable. This enables FIFO-fair wake-up: we can signal specific
/// threads in order rather than having all waiters race.
///
/// **State machine:**
/// ```
/// [Enter] ──┬──▶ WAITING_LOCK ──(granted)──▶ ACQUIRED
///           │
/// [Wait]  ──┴──▶ WAITING_PAUSE ──(Pause)──▶ WAITING_LOCK ──▶ ACQUIRED
/// ```
typedef struct RtMonitorWaiter {
    struct RtMonitorWaiter *next; ///< Next waiter in queue (singly linked).
    pthread_cond_t cv;            ///< Per-waiter condition variable.
    int8_t cond_uses_monotonic;   ///< Non-zero when cv uses CLOCK_MONOTONIC.
    pthread_t thread;             ///< The waiting thread's ID.
    int state;                    ///< Current state (from enum above).
    size_t desired_recursion;     ///< Recursion count to restore on acquisition.
} RtMonitorWaiter;

/// @brief The monitor state associated with an object.
///
/// Contains the mutex protecting the state, ownership info, recursion count,
/// and two queues: one for threads waiting to acquire the lock (acq_queue),
/// and one for threads that called Wait() (wait_queue).
///
/// **Memory layout:**
/// ```
/// RtMonitor:
/// ┌─────────────────────────────────────────────────────┐
/// │ mu            │ pthread mutex protecting this state │
/// ├───────────────┼─────────────────────────────────────┤
/// │ owner         │ pthread_t of current owner          │
/// │ owner_valid   │ 1 if owned, 0 if free               │
/// │ recursion     │ How many times owner called Enter() │
/// ├───────────────┼─────────────────────────────────────┤
/// │ acq_head/tail │ Queue of threads waiting to acquire │
/// │ wait_head/tail│ Queue of threads that called Wait() │
/// └───────────────┴─────────────────────────────────────┘
/// ```
typedef struct RtMonitor {
    pthread_mutex_t mu;         ///< Mutex protecting all monitor state.
    pthread_t owner;            ///< Current owner thread.
    int owner_valid;            ///< Non-zero if monitor is currently owned.
    size_t recursion;           ///< Re-entry count for owner.
    RtMonitorWaiter *acq_head;  ///< Head of acquisition queue.
    RtMonitorWaiter *acq_tail;  ///< Tail of acquisition queue.
    RtMonitorWaiter *wait_head; ///< Head of wait queue.
    RtMonitorWaiter *wait_tail; ///< Tail of wait queue.
    int retired;                ///< Monitor owner object is being finalized.
} RtMonitor;

/// @brief Hash table entry mapping object address to monitor.
///
/// Monitors are looked up by object address. The hash table uses separate
/// chaining for collision resolution.
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

static pthread_mutex_t g_monitor_table_locks[RT_MONITOR_LOCK_STRIPES];
static pthread_once_t g_monitor_table_locks_once = PTHREAD_ONCE_INIT;
static int g_monitor_table_locks_status;
static RtMonitorEntry *g_monitor_table[RT_MONITOR_BUCKETS];

/// @brief Initialize the striped monitor-table locks exactly once.
/// @details POSIX has no portable repeated static initializer for an array of
///          mutexes. The once callback initializes every stripe and rolls back
///          already-created mutexes if a later initialization fails. The
///          stored errno-style status lets all callers fail deterministically
///          instead of using a partially initialized lock table.
static void monitor_table_locks_init_once(void) {
    for (size_t i = 0; i < RT_MONITOR_LOCK_STRIPES; ++i) {
        const int rc = pthread_mutex_init(&g_monitor_table_locks[i], NULL);
        if (rc == 0)
            continue;
        for (size_t initialized = 0; initialized < i; ++initialized)
            (void)pthread_mutex_destroy(&g_monitor_table_locks[initialized]);
        g_monitor_table_locks_status = rc;
        return;
    }
    g_monitor_table_locks_status = 0;
}

/// @brief Return the lock stripe protecting one monitor-table bucket.
/// @details Initialization and lock failures are reported through the runtime
///          trap dispatcher. A returning trap hook receives NULL, allowing the
///          caller to stop before touching shared table state.
/// @param bucket Monitor hash bucket index.
/// @return Locked stripe mutex, or NULL after a reported failure.
static pthread_mutex_t *monitor_table_lock_bucket(size_t bucket) {
    const int once_rc = pthread_once(&g_monitor_table_locks_once, monitor_table_locks_init_once);
    const int init_rc = __atomic_load_n(&g_monitor_table_locks_status, __ATOMIC_ACQUIRE);
    if (once_rc != 0 || init_rc != 0) {
        rt_trap("rt_monitor: table lock initialization failed");
        return NULL;
    }
    pthread_mutex_t *lock = &g_monitor_table_locks[bucket & (RT_MONITOR_LOCK_STRIPES - 1u)];
    if (pthread_mutex_lock(lock) != 0) {
        rt_trap("rt_monitor: table lock failed");
        return NULL;
    }
    return lock;
}

// ---------------------------------------------------------------------------
// POSIX backend — pthread_mutex_t + pthread_cond_t. Mirrors the
// Win32 helpers above; see those for FIFO-fairness rationale.
// ---------------------------------------------------------------------------

/// @brief Hash a pointer to a monitor-table bucket index (Knuth golden-ratio mix).
/// @param p Object address used as the monitor key.
/// @return Bucket index in `[0, RT_MONITOR_BUCKETS)`.
static size_t hash_ptr(void *p) {
    uintptr_t x = (uintptr_t)p;
    x >>= 4;
    x ^= x >> 16;
    x *= 0x9E3779B97F4A7C15ull;
    return (size_t)(x & (RT_MONITOR_BUCKETS - 1u));
}

/// @brief Locate (or lazily allocate) the monitor for `obj` (POSIX path).
/// @see Win32 `get_monitor_for` for the design rationale.
/// @param obj Non-NULL object address used as the table key.
/// @return Existing or newly allocated monitor, or NULL after a reported
///         allocation, initialization, or table-lock failure.
static RtMonitor *get_monitor_for(void *obj) {
    size_t idx = hash_ptr(obj);

    pthread_mutex_t *table_lock = monitor_table_lock_bucket(idx);
    if (!table_lock)
        return NULL;
    RtMonitorEntry *it = g_monitor_table[idx];
    while (it) {
        if (it->key == obj && !it->retired) {
            (void)pthread_mutex_unlock(table_lock);
            return &it->monitor;
        }
        it = it->next;
    }

    RtMonitorEntry *node = (RtMonitorEntry *)calloc(1, sizeof(*node));
    if (!node) {
        (void)pthread_mutex_unlock(table_lock);
        rt_trap("rt_monitor: alloc failed");
        return NULL;
    }
    const int init_rc = pthread_mutex_init(&node->monitor.mu, NULL);
    if (init_rc != 0) {
        free(node);
        (void)pthread_mutex_unlock(table_lock);
        rt_trap("rt_monitor: monitor mutex initialization failed");
        return NULL;
    }
    node->key = obj;
    node->next = g_monitor_table[idx];
    g_monitor_table[idx] = node;

    (void)pthread_mutex_unlock(table_lock);
    return &node->monitor;
}

/// @brief Release the monitor associated with `obj` from the global table (POSIX).
///
/// Called from the GC finalizer of any object that has had a
/// monitor attached. Idempotent: silently no-ops if no entry exists.
/// @param obj Object address whose active monitor entry should be retired or removed.
void rt_monitor_forget(void *obj) {
    if (!obj)
        return;
    size_t idx = hash_ptr(obj);

    pthread_mutex_t *table_lock = monitor_table_lock_bucket(idx);
    if (!table_lock)
        return;
    RtMonitorEntry **link = &g_monitor_table[idx];
    RtMonitorEntry *node = *link;
    while (node && (node->key != obj || node->retired)) {
        link = &node->next;
        node = node->next;
    }
    if (!node) {
        (void)pthread_mutex_unlock(table_lock);
        return;
    }
    pthread_mutex_lock(&node->monitor.mu);
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
        pthread_mutex_unlock(&node->monitor.mu);
        (void)pthread_mutex_unlock(table_lock);
        return;
    }

    *link = node->next;
    pthread_mutex_unlock(&node->monitor.mu);
    (void)pthread_mutex_unlock(table_lock);

    (void)pthread_mutex_destroy(&node->monitor.mu);
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

    pthread_mutex_t *table_lock = monitor_table_lock_bucket(idx);
    if (!table_lock)
        return;
    RtMonitorEntry **link = &g_monitor_table[idx];
    RtMonitorEntry *node = *link;
    while (node && (node->key != obj || &node->monitor != monitor)) {
        link = &node->next;
        node = node->next;
    }
    if (!node) {
        (void)pthread_mutex_unlock(table_lock);
        return;
    }

    pthread_mutex_lock(&node->monitor.mu);
    if (!node->retired || node->monitor.owner_valid || node->monitor.acq_head ||
        node->monitor.wait_head) {
        pthread_mutex_unlock(&node->monitor.mu);
        (void)pthread_mutex_unlock(table_lock);
        return;
    }

    *link = node->next;
    pthread_mutex_unlock(&node->monitor.mu);
    (void)pthread_mutex_unlock(table_lock);

    (void)pthread_mutex_destroy(&node->monitor.mu);
    free(node);
}

/// @brief True if pthread `self` currently owns the monitor (POSIX equality).
/// @param m Monitor whose owner state is inspected.
/// @param self Calling thread identifier.
/// @return Non-zero when @p self is the recorded owner.
static int monitor_is_owner(const RtMonitor *m, pthread_t self) {
    return m->owner_valid && pthread_equal(m->owner, self);
}

/// @brief Pop the FIFO acquisition queue head, hand it the lock, signal its condvar (POSIX).
/// @details The waiter's saved recursion depth becomes the monitor recursion count.
///          The caller must hold @p m's state mutex.
/// @param m Monitor whose next acquisition waiter is granted ownership.
static void monitor_grant_next_waiter(RtMonitor *m) {
    RtMonitorWaiter *w = m->acq_head;
    if (!w)
        return;
    m->acq_head = w->next;
    if (!m->acq_head)
        m->acq_tail = NULL;

    m->owner = w->thread;
    m->owner_valid = 1;
    m->recursion = w->desired_recursion;

    w->state = RT_MON_WAITER_ACQUIRED;
    pthread_cond_signal(&w->cv);
}

/// @brief POSIX FIFO append for the acquisition queue.
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

/// @brief POSIX queue removal — splice `w` out of the acquisition queue.
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

/// @brief POSIX FIFO append for the condition-wait queue.
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

/// @brief POSIX queue removal — splice `w` out of the wait queue.
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
///          here. The caller holds the monitor mutex while changing their
///          states and signaling their private condition variables.
/// @note Defined once per platform backend branch; both copies are identical.
/// @param w Head of the detached waiter chain, or NULL.
static void monitor_cancel_queue(RtMonitorWaiter *w) {
    while (w) {
        RtMonitorWaiter *next = w->next;
        w->next = NULL;
        w->state = RT_MON_WAITER_CANCELLED;
        pthread_cond_signal(&w->cv);
        w = next;
    }
}

typedef struct {
    struct timespec deadline;
} monitor_deadline_t;

/// @brief Initialize a pthread condvar for monitor Wait, preferring CLOCK_MONOTONIC.
/// @details Same rationale as the ConcurrentQueue cq_cond_init: timed Wait
///          deadlines should be immune to wall-clock adjustments. macOS
///          uses pthread_cond_timedwait_relative_np (intrinsically
///          monotonic, so we report uses_monotonic=1 even though
///          condattr_setclock isn't available). Other POSIX platforms
///          fall back to CLOCK_REALTIME if condattr_setclock fails.
/// @param cond Uninitialized waiter condition variable.
/// @param uses_monotonic Optional output set when its deadlines use a monotonic clock.
/// @return 0 on success, errno-style error code otherwise.
static int monitor_cond_init(pthread_cond_t *cond, int8_t *uses_monotonic) {
    if (uses_monotonic)
        *uses_monotonic = 0;
#if defined(__APPLE__)
    if (uses_monotonic)
        *uses_monotonic = 1;
    return pthread_cond_init(cond, NULL);
#elif defined(CLOCK_MONOTONIC)
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0)
        return pthread_cond_init(cond, NULL);
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        pthread_condattr_destroy(&attr);
        return pthread_cond_init(cond, NULL);
    }
    if (uses_monotonic)
        *uses_monotonic = 1;
    int rc = pthread_cond_init(cond, &attr);
    if (rc != 0 && uses_monotonic)
        *uses_monotonic = 0;
    pthread_condattr_destroy(&attr);
    return rc;
#else
    return pthread_cond_init(cond, NULL);
#endif
}

/// @brief Read the current clock, preferring the monotonic source when requested
///        and available.
/// @details Monitor timeouts and pthread condvar waits need a consistent clock
///          source. `CLOCK_MONOTONIC` is the right choice for "wait 500ms" because
///          it's immune to wall-clock adjustments (NTP sync, DST), but some
///          pthread implementations require `CLOCK_REALTIME` for `pthread_cond_timedwait`
///          with the default attributes — the caller picks which per-condvar.
///          On platforms without `CLOCK_MONOTONIC` (few, at this point) we fall
///          back to `CLOCK_REALTIME` silently. A stack-allocated `timespec` is
///          zero-initialized so a `clock_gettime` failure returns a sensible
///          "epoch" value rather than uninitialized stack garbage.
/// @param use_monotonic Whether to prefer `CLOCK_MONOTONIC`.
/// @return Current timestamp from the selected available clock.
static struct timespec monitor_now_clock(int8_t use_monotonic) {
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
#ifdef CLOCK_MONOTONIC
    if (use_monotonic && clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return ts;
#endif
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    return ts;
}

/// @brief Compute an absolute deadline `ms` milliseconds in the future from now,
///        using the caller-selected clock source.
/// @details Takes the current clock reading and adds the millisecond budget as
///          `(sec, nsec)`. Overflow-safe: before adding, checks that
///          `d.deadline.tv_sec + add_sec` doesn't exceed `LONG_MAX` — if it
///          would, clamps to `(LONG_MAX, 999999999ns)` so the deadline becomes
///          "effectively forever" rather than wrapping to a past value that
///          would trigger an immediate spurious timeout. Also normalizes
///          nanosecond overflow (`ns >= 1e9` → carry into seconds) so downstream
///          `pthread_cond_timedwait` doesn't see a malformed timespec. Negative
///          or zero `ms` returns the current time so a "no timeout" caller still
///          gets a valid struct.
/// @param ms Wait duration in milliseconds.
/// @param use_monotonic Whether to base the deadline on a monotonic clock.
/// @return Saturating absolute deadline on the selected clock.
static monitor_deadline_t monitor_deadline_ms_from_now(int64_t ms, int8_t use_monotonic) {
    monitor_deadline_t d;
    d.deadline = monitor_now_clock(use_monotonic);
    if (ms <= 0)
        return d;

    int64_t add_sec = ms / 1000;
    long add_nsec = (long)((ms % 1000) * 1000000L);
    int64_t sec_room = (int64_t)LONG_MAX - (int64_t)d.deadline.tv_sec;
    if (add_sec > sec_room || (add_sec == sec_room && d.deadline.tv_nsec > 999999999L - add_nsec)) {
        d.deadline.tv_sec = (time_t)LONG_MAX;
        d.deadline.tv_nsec = 999999999L;
        return d;
    }
    int64_t sec = (int64_t)d.deadline.tv_sec + add_sec;
    int64_t ns = (int64_t)d.deadline.tv_nsec + add_nsec;
    if (ns >= 1000000000) {
        sec += 1;
        ns -= 1000000000;
    }
    d.deadline.tv_sec = (time_t)sec;
    d.deadline.tv_nsec = (long)ns;
    return d;
}

#if defined(__APPLE__)
/// @brief Return milliseconds remaining until @p deadline (macOS-only relative-wait helper).
/// @details Counterpart to cq_remaining_ms in rt_concqueue.c — used to
///          convert an absolute monitor-Wait deadline into a relative
///          timeout for pthread_cond_timedwait_relative_np. Returns 0 if
///          the deadline has lapsed; saturates at INT64_MAX for very
///          large remaining intervals.
/// @param deadline Absolute deadline to compare with the selected clock.
/// @param use_monotonic Whether to read the current monotonic rather than realtime clock.
/// @return Remaining whole milliseconds, zero after expiry, or `INT64_MAX`
///         when the interval cannot be represented.
static int64_t monitor_remaining_ms(monitor_deadline_t deadline, int8_t use_monotonic) {
    struct timespec now = monitor_now_clock(use_monotonic);
    int64_t sec = (int64_t)deadline.deadline.tv_sec - (int64_t)now.tv_sec;
    int64_t ns = (int64_t)deadline.deadline.tv_nsec - (int64_t)now.tv_nsec;
    if (ns < 0) {
        sec--;
        ns += 1000000000L;
    }
    if (sec < 0)
        return 0;
    if (sec > INT64_MAX / 1000)
        return INT64_MAX;
    return sec * 1000 + ns / 1000000L;
}
#endif

/// @brief Cross-platform pthread_cond_timedwait against an absolute monitor-Wait deadline.
/// @details Per-platform: on macOS, computes a relative timeout via
///          monitor_remaining_ms and calls
///          pthread_cond_timedwait_relative_np; returns ETIMEDOUT if the
///          deadline has lapsed. On Linux/POSIX, calls the standard
///          pthread_cond_timedwait with the absolute timespec stored in
///          the deadline. Caller must hold the monitor mutex.
/// @param cond Per-waiter condition variable.
/// @param mutex Locked monitor mutex released atomically during the wait.
/// @param deadline Absolute timeout on the condition variable's clock.
/// @param use_monotonic Whether @p deadline uses a monotonic clock.
/// @return Zero after a signal, `ETIMEDOUT` on expiry, or another pthread error code.
static int monitor_cond_timedwait_deadline(pthread_cond_t *cond,
                                           pthread_mutex_t *mutex,
                                           monitor_deadline_t deadline,
                                           int8_t use_monotonic) {
#if defined(__APPLE__)
    int64_t remaining = monitor_remaining_ms(deadline, use_monotonic);
    if (remaining <= 0)
        return ETIMEDOUT;
    struct timespec rel;
    rel.tv_sec = (time_t)(remaining / 1000);
    rel.tv_nsec = (long)((remaining % 1000) * 1000000L);
    return pthread_cond_timedwait_relative_np(cond, mutex, &rel);
#else
    (void)use_monotonic;
    return pthread_cond_timedwait(cond, mutex, &deadline.deadline);
#endif
}

/// @brief POSIX equivalent of the Win32 `monitor_enter_blocking`.
///
/// Uses pthread mutex + per-waiter condvar. Identical fast-paths
/// (re-entry, uncontended) and identical FIFO-fairness contract;
/// see the Win32 version for the design rationale.
/// @param m Locked monitor to acquire recursively or enqueue against.
/// @param self Calling thread identifier.
/// @param timeout_ms Maximum wait in milliseconds when @p timed is non-zero.
/// @param timed Whether the acquisition uses the supplied deadline.
/// @return One after ownership is acquired, or zero after timeout or a reported failure.
static int monitor_enter_blocking(RtMonitor *m, pthread_t self, int64_t timeout_ms, int timed) {
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
    if (monitor_cond_init(&w.cv, &w.cond_uses_monotonic) != 0) {
        rt_trap("Monitor.Enter: condition init failed");
        return 0;
    }
    w.thread = self;
    w.state = RT_MON_WAITER_WAITING_LOCK;
    w.desired_recursion = 1;
    monitor_enqueue_acq(m, &w);

    int rc = 0;
    const monitor_deadline_t deadline =
        monitor_deadline_ms_from_now(timeout_ms, w.cond_uses_monotonic);
    while (w.state != RT_MON_WAITER_ACQUIRED) {
        if (!timed) {
            rc = pthread_cond_wait(&w.cv, &m->mu);
        } else {
            rc = monitor_cond_timedwait_deadline(&w.cv, &m->mu, deadline, w.cond_uses_monotonic);
        }

        if (timed && rc == ETIMEDOUT && w.state != RT_MON_WAITER_ACQUIRED) {
            monitor_remove_acq(m, &w);
            pthread_cond_destroy(&w.cv);
            return 0;
        }
        if (rc != 0 && rc != ETIMEDOUT) {
            monitor_remove_acq(m, &w);
            pthread_cond_destroy(&w.cv);
            rt_trap("Monitor.Enter: condition wait failed");
            return 0;
        }
        if (w.state == RT_MON_WAITER_CANCELLED) {
            pthread_cond_destroy(&w.cv);
            rt_trap("Monitor.Enter: object finalized while waiting");
            return 0;
        }
    }

    pthread_cond_destroy(&w.cv);
    return w.state == RT_MON_WAITER_ACQUIRED ? 1 : 0;
}

/// @brief Acquires exclusive access to an object's monitor.
///
/// Blocks until the calling thread can acquire exclusive ownership of the
/// monitor. If the monitor is free, acquires immediately. If another thread
/// owns it, waits in a FIFO queue until granted ownership.
///
/// Re-entrancy: If the calling thread already owns the monitor, the recursion
/// count is incremented and the call returns immediately.
///
/// **Example:**
/// ```
/// Monitor.Enter(sharedData)
/// Try
///     ' Critical section - exclusive access guaranteed
///     sharedData.Update()
/// Finally
///     Monitor.Exit(sharedData)
/// End Try
/// ```
///
/// @param obj The object whose monitor to acquire. Must not be NULL.
///
/// @note Traps if obj is NULL.
/// @note Each Enter() must be balanced by a corresponding Exit().
/// @note Blocks indefinitely - use TryEnter or TryEnterFor for timeouts.
/// @note FIFO-fair: threads acquire in the order they called Enter().
///
/// @see rt_monitor_exit For releasing the monitor
/// @see rt_monitor_try_enter For non-blocking acquisition
/// @see rt_monitor_try_enter_for For acquisition with timeout
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
    pthread_mutex_lock(&m->mu);
    char saved_error[256];
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) != 0) {
        monitor_save_trap_error(saved_error, sizeof(saved_error), "Monitor.Enter: failed");
        rt_trap_clear_recovery();
        pthread_mutex_unlock(&m->mu);
        monitor_release_enter_ref(obj);
        rt_trap(saved_error);
        return;
    }
    int acquired = monitor_enter_blocking(m, pthread_self(), /*timeout_ms=*/0, /*timed=*/0);
    rt_trap_clear_recovery();
    pthread_mutex_unlock(&m->mu);
    if (!acquired)
        monitor_release_enter_ref(obj);
}

/// @brief Attempts to acquire a monitor without blocking.
///
/// Tries to acquire the monitor immediately. If successful, returns true and
/// the calling thread now owns the monitor. If another thread owns it or
/// threads are waiting, returns false without blocking.
///
/// **Example:**
/// ```
/// If Monitor.TryEnter(resource) Then
///     Try
///         UseResource(resource)
///     Finally
///         Monitor.Exit(resource)
///     End Try
/// Else
///     ' Resource busy, do something else
///     DoAlternateWork()
/// End If
/// ```
///
/// @param obj The object whose monitor to try acquiring. Must not be NULL.
///
/// @return 1 (true) if the monitor was acquired, 0 (false) if it's busy.
///
/// @note Traps if obj is NULL.
/// @note If already owner, increments recursion and returns true.
/// @note Never blocks - returns immediately.
///
/// @see rt_monitor_enter For blocking acquisition
/// @see rt_monitor_try_enter_for For acquisition with timeout
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
    pthread_t self = pthread_self();

    pthread_mutex_lock(&m->mu);
    if (m->retired) {
        pthread_mutex_unlock(&m->mu);
        if (rt_obj_release_check0(obj))
            rt_obj_free(obj);
        rt_trap("Monitor.Enter: object finalized");
        return 0;
    }
    if (monitor_is_owner(m, self)) {
        if (m->recursion == SIZE_MAX) {
            pthread_mutex_unlock(&m->mu);
            if (rt_obj_release_check0(obj))
                rt_obj_free(obj);
            rt_trap("Monitor.Enter: recursion overflow");
            return 0;
        }
        m->recursion += 1;
        pthread_mutex_unlock(&m->mu);
        return 1;
    }
    if (!m->owner_valid && m->acq_head == NULL) {
        m->owner = self;
        m->owner_valid = 1;
        m->recursion = 1;
        pthread_mutex_unlock(&m->mu);
        return 1;
    }
    pthread_mutex_unlock(&m->mu);
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
    return 0;
}

/// @brief Try to acquire an object's reentrant monitor within a timeout.
/// @details An unowned monitor or recursive entry succeeds immediately.
///          Otherwise the caller joins the FIFO acquisition queue and waits
///          against one absolute deadline. Negative durations are treated as
///          zero. The object is retained for the duration of a pending enter
///          and remains retained while the resulting ownership is held.
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
    pthread_t self = pthread_self();

    pthread_mutex_lock(&m->mu);
    if (m->retired) {
        pthread_mutex_unlock(&m->mu);
        if (rt_obj_release_check0(obj))
            rt_obj_free(obj);
        rt_trap("Monitor.Enter: object finalized");
        return 0;
    }
    if (monitor_is_owner(m, self)) {
        if (m->recursion == SIZE_MAX) {
            pthread_mutex_unlock(&m->mu);
            if (rt_obj_release_check0(obj))
                rt_obj_free(obj);
            rt_trap("Monitor.Enter: recursion overflow");
            return 0;
        }
        m->recursion += 1;
        pthread_mutex_unlock(&m->mu);
        return 1;
    }
    if (!m->owner_valid && m->acq_head == NULL) {
        m->owner = self;
        m->owner_valid = 1;
        m->recursion = 1;
        pthread_mutex_unlock(&m->mu);
        return 1;
    }

    RtMonitorWaiter w = {0};
    if (monitor_cond_init(&w.cv, &w.cond_uses_monotonic) != 0) {
        pthread_mutex_unlock(&m->mu);
        if (rt_obj_release_check0(obj))
            rt_obj_free(obj);
        rt_trap("Monitor.Enter: condition init failed");
        return 0;
    }
    w.thread = self;
    w.state = RT_MON_WAITER_WAITING_LOCK;
    w.desired_recursion = 1;
    monitor_enqueue_acq(m, &w);

    const monitor_deadline_t deadline = monitor_deadline_ms_from_now(ms, w.cond_uses_monotonic);
    int rc = 0;
    while (w.state != RT_MON_WAITER_ACQUIRED) {
        rc = monitor_cond_timedwait_deadline(&w.cv, &m->mu, deadline, w.cond_uses_monotonic);
        if (rc == ETIMEDOUT && w.state != RT_MON_WAITER_ACQUIRED) {
            monitor_remove_acq(m, &w);
            pthread_cond_destroy(&w.cv);
            pthread_mutex_unlock(&m->mu);
            if (rt_obj_release_check0(obj))
                rt_obj_free(obj);
            return 0;
        }
        if (rc != 0) {
            monitor_remove_acq(m, &w);
            pthread_cond_destroy(&w.cv);
            pthread_mutex_unlock(&m->mu);
            if (rt_obj_release_check0(obj))
                rt_obj_free(obj);
            rt_trap("Monitor.Enter: condition wait failed");
            return 0;
        }
        if (w.state == RT_MON_WAITER_CANCELLED) {
            pthread_cond_destroy(&w.cv);
            pthread_mutex_unlock(&m->mu);
            if (rt_obj_release_check0(obj))
                rt_obj_free(obj);
            rt_trap("Monitor.Enter: object finalized while waiting");
            return 0;
        }
    }

    pthread_cond_destroy(&w.cv);
    pthread_mutex_unlock(&m->mu);
    return 1;
}

/// @brief Releases the monitor, allowing other threads to acquire it.
///
/// Releases one level of ownership of the monitor. If the calling thread
/// entered the monitor multiple times (re-entrancy), only decrements the
/// recursion count. When recursion reaches zero, releases completely and
/// wakes the next waiting thread if any.
///
/// **Example:**
/// ```
/// Monitor.Enter(obj)
/// Try
///     DoWork()
/// Finally
///     Monitor.Exit(obj)  ' Always exit, even on exception
/// End Try
/// ```
///
/// @param obj The object whose monitor to release. Must not be NULL.
///
/// @note Traps if obj is NULL.
/// @note Traps if the calling thread doesn't own the monitor.
/// @note FIFO-fair: wakes the thread that has been waiting longest.
///
/// @see rt_monitor_enter For acquiring the monitor
/// @brief Release the monitor lock (must be called once for each enter, respects reentrancy).
void rt_monitor_exit(void *obj) {
    if (!obj)
        rt_trap("Monitor.Exit: null object");
    if (!obj)
        return;
    RtMonitor *m = get_monitor_for(obj);
    if (!m)
        return;
    pthread_t self = pthread_self();

    pthread_mutex_lock(&m->mu);
    if (!monitor_is_owner(m, self)) {
        pthread_mutex_unlock(&m->mu);
        rt_trap("Monitor.Exit: not owner");
        return;
    }
    if (m->recursion > 1) {
        m->recursion -= 1;
        pthread_mutex_unlock(&m->mu);
        if (rt_obj_release_check0(obj))
            rt_obj_free(obj);
        return;
    }

    m->owner_valid = 0;
    m->recursion = 0;
    monitor_grant_next_waiter(m);
    pthread_mutex_unlock(&m->mu);
    monitor_cleanup_retired_if_idle(obj, m);
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Releases the monitor and waits for a Pause signal.
///
/// Atomically releases the monitor and enters a wait state. The thread
/// remains blocked until another thread calls Monitor.Notify() or
/// Monitor.NotifyAll() on the same object. When signaled, the thread
/// re-acquires the monitor before returning.
///
/// **Producer/Consumer Example:**
/// ```
/// ' Consumer thread
/// Monitor.Enter(queue)
/// While queue.IsEmpty()
///     Monitor.Wait(queue)  ' Release lock and wait
/// Wend
/// Dim item = queue.Remove()
/// Monitor.Exit(queue)
/// ```
///
/// **Workflow:**
/// 1. Saves the current recursion count
/// 2. Fully releases the monitor (recursion → 0)
/// 3. Grants ownership to next thread waiting to acquire
/// 4. Joins the wait queue
/// 5. Blocks until Pause/PauseAll signals this thread
/// 6. Moves to acquisition queue
/// 7. Re-acquires the monitor (restoring recursion count)
/// 8. Returns to caller
///
/// @param obj The object whose monitor to wait on. Must be owned by caller.
///
/// @note Traps if obj is NULL.
/// @note Traps if the calling thread doesn't own the monitor.
/// @note The monitor is always re-acquired before this function returns.
/// @note Use WaitFor() for a timed wait.
///
/// @see rt_monitor_pause For waking one waiting thread
/// @see rt_monitor_pause_all For waking all waiting threads
/// @see rt_monitor_wait_for For waiting with timeout
/// @brief Release the lock and wait until another thread calls Pause/PauseAll on this monitor.
void rt_monitor_wait(void *obj) {
    if (!obj)
        rt_trap("Monitor.Wait: not owner");
    if (!obj)
        return;
    RtMonitor *m = get_monitor_for(obj);
    if (!m)
        return;
    pthread_t self = pthread_self();
    pthread_mutex_lock(&m->mu);

    if (!monitor_is_owner(m, self)) {
        pthread_mutex_unlock(&m->mu);
        rt_trap("Monitor.Wait: not owner");
        return;
    }

    const size_t saved_recursion = m->recursion;

    RtMonitorWaiter w = {0};
    if (monitor_cond_init(&w.cv, &w.cond_uses_monotonic) != 0) {
        pthread_mutex_unlock(&m->mu);
        rt_trap("Monitor.Wait: condition init failed");
        return;
    }
    w.thread = self;
    w.state = RT_MON_WAITER_WAITING_PAUSE;
    w.desired_recursion = saved_recursion;
    monitor_enqueue_wait(m, &w);

    // Queue before releasing so Pause/PauseAll cannot miss this waiter.
    m->owner_valid = 0;
    m->recursion = 0;
    monitor_grant_next_waiter(m);

    int wait_failed = 0;
    while (w.state == RT_MON_WAITER_WAITING_PAUSE) {
        const int rc = pthread_cond_wait(&w.cv, &m->mu);
        if (rc != 0) {
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
        const int rc = pthread_cond_wait(&w.cv, &m->mu);
        if (rc != 0) {
            wait_failed = 1;
            if (w.state == RT_MON_WAITER_WAITING_LOCK) {
                monitor_remove_acq(m, &w);
                if (!m->owner_valid && m->acq_head)
                    monitor_grant_next_waiter(m);
            }
            break;
        }
    }

    const int destroy_rc = pthread_cond_destroy(&w.cv);
    pthread_mutex_unlock(&m->mu);
    if (w.state == RT_MON_WAITER_CANCELLED)
        rt_trap("Monitor.Wait: object finalized while waiting");
    else if (wait_failed)
        rt_trap("Monitor.Wait: condition wait failed");
    else if (destroy_rc != 0)
        rt_trap("Monitor.Wait: condition destroy failed");
}

/// @brief Release an owned monitor and wait for notification up to a deadline.
/// @details The current recursion depth is saved, ownership is fully released,
///          and the thread joins the condition-wait queue. Notification moves
///          it to the FIFO acquisition queue. Timeout also moves it there, so
///          the original recursion depth is reacquired before return. Native
///          wait, destruction, ownership, and retirement failures trap.
/// @param obj Object whose monitor must be owned by the calling thread.
/// @param ms Maximum notification wait in milliseconds; negatives become zero.
/// @return One when notified and reacquired, or zero after timeout or a native
///         wait/destruction failure handled by a returning trap hook.
int8_t rt_monitor_wait_for(void *obj, int64_t ms) {
    if (!obj)
        rt_trap("Monitor.Wait: not owner");
    if (!obj)
        return 0;
    RtMonitor *m = get_monitor_for(obj);
    if (!m)
        return 0;
    pthread_t self = pthread_self();
    pthread_mutex_lock(&m->mu);

    if (!monitor_is_owner(m, self)) {
        pthread_mutex_unlock(&m->mu);
        rt_trap("Monitor.Wait: not owner");
        return 0;
    }

    if (ms < 0)
        ms = 0;

    const size_t saved_recursion = m->recursion;

    RtMonitorWaiter w = {0};
    if (monitor_cond_init(&w.cv, &w.cond_uses_monotonic) != 0) {
        pthread_mutex_unlock(&m->mu);
        rt_trap("Monitor.Wait: condition init failed");
        return 0;
    }
    w.thread = self;
    w.state = RT_MON_WAITER_WAITING_PAUSE;
    w.desired_recursion = saved_recursion;
    monitor_enqueue_wait(m, &w);

    // Queue before releasing so Pause/PauseAll cannot miss this waiter.
    m->owner_valid = 0;
    m->recursion = 0;
    monitor_grant_next_waiter(m);

    int timed_out = 0;
    int wait_failed = 0;
    const monitor_deadline_t deadline = monitor_deadline_ms_from_now(ms, w.cond_uses_monotonic);
    while (w.state == RT_MON_WAITER_WAITING_PAUSE) {
        const int rc =
            monitor_cond_timedwait_deadline(&w.cv, &m->mu, deadline, w.cond_uses_monotonic);
        if (rc == ETIMEDOUT && w.state == RT_MON_WAITER_WAITING_PAUSE) {
            timed_out = 1;
            break;
        }
        if (rc != 0 && rc != ETIMEDOUT) {
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
        const int rc = pthread_cond_wait(&w.cv, &m->mu);
        if (rc != 0) {
            wait_failed = 1;
            if (w.state == RT_MON_WAITER_WAITING_LOCK) {
                monitor_remove_acq(m, &w);
                if (!m->owner_valid && m->acq_head)
                    monitor_grant_next_waiter(m);
            }
            break;
        }
    }

    const int destroy_rc = pthread_cond_destroy(&w.cv);
    pthread_mutex_unlock(&m->mu);
    if (w.state == RT_MON_WAITER_CANCELLED)
        rt_trap("Monitor.Wait: object finalized while waiting");
    else if (wait_failed)
        rt_trap("Monitor.Wait: condition wait failed");
    else if (destroy_rc != 0)
        rt_trap("Monitor.Wait: condition destroy failed");
    return (timed_out || wait_failed || destroy_rc != 0) ? 0 : 1;
}

/// @brief Wakes one thread waiting on the monitor.
///
/// Moves the oldest thread from the wait queue (threads that called Wait())
/// to the acquisition queue. The woken thread will re-acquire the monitor
/// after the current owner releases it.
///
/// **Producer/Consumer Example:**
/// ```
/// ' Producer thread
/// Monitor.Enter(queue)
/// queue.Add(item)
/// Monitor.Notify(queue)  ' Wake one consumer
/// Monitor.Exit(queue)
/// ```
///
/// **Behavior:**
/// - If no threads are waiting, this is a no-op (no error)
/// - The woken thread doesn't run immediately - it waits to acquire the lock
/// - The current thread keeps the monitor until it calls Exit()
/// - FIFO-fair: wakes the thread that has been waiting longest
///
/// @param obj The object whose waiting threads to signal. Must be owned by caller.
///
/// @note Traps if obj is NULL.
/// @note Traps if the calling thread doesn't own the monitor.
/// @note Does nothing if no threads are waiting.
/// @note The caller still holds the monitor after this call.
///
/// @see rt_monitor_pause_all For waking all waiting threads
/// @see rt_monitor_wait For entering the wait state
/// @brief Wake one thread waiting on this monitor (signal/notify pattern).
void rt_monitor_pause(void *obj) {
    if (!obj)
        rt_trap("Monitor.Notify: not owner");
    if (!obj)
        return;
    RtMonitor *m = get_monitor_for(obj);
    if (!m)
        return;
    pthread_t self = pthread_self();
    pthread_mutex_lock(&m->mu);

    if (!monitor_is_owner(m, self)) {
        pthread_mutex_unlock(&m->mu);
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
        pthread_cond_signal(&w->cv);
    }

    pthread_mutex_unlock(&m->mu);
}

/// @brief Wakes all threads waiting on the monitor.
///
/// Moves all threads from the wait queue to the acquisition queue. All woken
/// threads will compete to re-acquire the monitor in FIFO order after the
/// current owner releases it.
///
/// **Broadcast Example:**
/// ```
/// ' Signal all consumers that data is ready
/// Monitor.Enter(queue)
/// dataReady = True
/// Monitor.NotifyAll(queue)  ' Wake all waiters
/// Monitor.Exit(queue)
/// ```
///
/// **When to use PauseAll vs Pause:**
/// - Use Pause() when any one waiter can handle the condition
/// - Use PauseAll() when the condition might affect multiple waiters
/// - Use PauseAll() for broadcast notifications (state changes)
/// - PauseAll() is safer but may cause more contention
///
/// @param obj The object whose waiting threads to signal. Must be owned by caller.
///
/// @note Traps if obj is NULL.
/// @note Traps if the calling thread doesn't own the monitor.
/// @note Does nothing if no threads are waiting.
/// @note The caller still holds the monitor after this call.
/// @note All woken threads will compete for the lock in FIFO order.
///
/// @see rt_monitor_pause For waking just one thread
/// @see rt_monitor_wait For entering the wait state
/// @brief Wake all threads waiting on this monitor (broadcast/notify-all pattern).
void rt_monitor_pause_all(void *obj) {
    if (!obj)
        rt_trap("Monitor.NotifyAll: not owner");
    if (!obj)
        return;
    RtMonitor *m = get_monitor_for(obj);
    if (!m)
        return;
    pthread_t self = pthread_self();
    pthread_mutex_lock(&m->mu);

    if (!monitor_is_owner(m, self)) {
        pthread_mutex_unlock(&m->mu);
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
        pthread_cond_signal(&w->cv);
    }

    pthread_mutex_unlock(&m->mu);
}
#endif // !defined(_WIN32)
