//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_concqueue.c
// Purpose: Implements a thread-safe concurrent FIFO queue for the
//          Zanna.Threads.ConcQueue class. Uses a linked-list of nodes protected
//          by a mutex and condition variable. Supports Enqueue, Dequeue
//          (blocking), TryDequeue (non-blocking), Count, and Clear.
//
// Key invariants:
//   - The queue has no logical capacity bound. Strict Enqueue traps if its node
//     allocation fails; the internal TryEnqueue reports failure instead.
//   - Dequeue blocks until an item is available or a timeout expires.
//   - TryDequeue returns false immediately if the queue is empty.
//   - Count reflects the number of items currently in the queue.
//   - All operations acquire the mutex before touching the head/tail/count.
//   - Win32 uses CRITICAL_SECTION + CONDITION_VARIABLE; POSIX uses pthreads.
//
// Ownership/Lifetime:
//   - Enqueued void* values are retained on enqueue. Dequeue transfers that
//     retained reference to the caller; clear and finalize release it.
//   - Node allocations are freed on Dequeue or during Clear/finalize.
//   - The queue object is heap-allocated and managed by the runtime GC.
//
// Links: src/runtime/threads/rt_concqueue.h (public API),
//        src/runtime/threads/rt_channel.h (bounded channel, related concept)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Unbounded mutex-and-condition-variable concurrent FIFO queue.
/// @details Enqueue retains runtime values, dequeue transfers those references,
///          and clear/finalization releases them after detaching nodes under a
///          GC graph barrier. Platform-specific waits share absolute-deadline
///          logic where supported.

#include "rt_concqueue.h"

#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_platform.h"
#include "rt_threads.h"
#include "rt_trap.h"

#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Install a trap-recovery jump target for the current thread.
/// @param buf Recovery buffer to install.
void rt_trap_set_recovery(jmp_buf *buf);
/// @brief Clear the current thread's trap-recovery target.
void rt_trap_clear_recovery(void);
/// @brief Read the current thread's most recent trap message.
/// @return Borrowed diagnostic text, or null.
const char *rt_trap_get_error(void);

#if RT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "rt_win32_wait.h"
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
#if RT_PLATFORM_MACOS
/// @brief Wait on a condition variable using a relative macOS timeout.
/// @param cond Condition variable to wait on.
/// @param mutex Locked mutex released and reacquired by the wait.
/// @param rel_time Relative timeout.
/// @return Zero when signalled, or an errno-style failure code.
extern int pthread_cond_timedwait_relative_np(pthread_cond_t *cond,
                                              pthread_mutex_t *mutex,
                                              const struct timespec *rel_time);
#endif
#endif

// --- Node for linked list queue ---

/// @brief One heap-allocated FIFO node carrying a retained value.
typedef struct cq_node {
    void *value;
    struct cq_node *next;
} cq_node;

/// @brief Queue state protected by the platform mutex.
typedef struct {
    void *vptr;
    cq_node *head;
    cq_node *tail;
    int64_t count;
    int8_t closed;
#if RT_PLATFORM_WINDOWS
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE cond;
#else
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int8_t cond_uses_monotonic;
#endif
} rt_concqueue_impl;

// --- Platform abstraction macros ---

#if RT_PLATFORM_WINDOWS
#define CQ_LOCK(cq) EnterCriticalSection(&(cq)->mutex)
#define CQ_UNLOCK(cq) LeaveCriticalSection(&(cq)->mutex)
#define CQ_SIGNAL(cq) WakeConditionVariable(&(cq)->cond)
#define CQ_WAIT(cq) SleepConditionVariableCS(&(cq)->cond, &(cq)->mutex, INFINITE)
#else
#define CQ_LOCK(cq) pthread_mutex_lock(&(cq)->mutex)
#define CQ_UNLOCK(cq) pthread_mutex_unlock(&(cq)->mutex)
#define CQ_SIGNAL(cq) pthread_cond_signal(&(cq)->cond)
#define CQ_WAIT(cq) pthread_cond_wait(&(cq)->cond, &(cq)->mutex)
#endif

#if !RT_PLATFORM_WINDOWS
// ----- Timed-wait scaffolding (POSIX) ---------------------------------------
// Mirror of `rt_future.c`'s deadline helpers — same `CLOCK_MONOTONIC` preference
// (so deadlines aren't disturbed by NTP/DST), same macOS fallback to the
// relative-time API (`pthread_cond_timedwait_relative_np`) because Apple lacks
// per-cond clock selection. Functions: cq_cond_init, cq_now_clock,
// cq_deadline_ms_from_now, cq_remaining_ms (Apple), cq_cond_timedwait_deadline.
// Each performs the obvious operation; see rt_future.c for exhaustive notes on
// the rationale.
typedef struct {
    struct timespec deadline;
} cq_deadline_t;

/// @brief Initialize a pthread condvar, preferring CLOCK_MONOTONIC when supported.
/// @details Linux supports `pthread_condattr_setclock(CLOCK_MONOTONIC)` so
///          timed waits are immune to wall-clock adjustments. macOS doesn't
///          expose that API but provides `pthread_cond_timedwait_relative_np`
///          (which is intrinsically monotonic), so we still report
///          @p uses_monotonic = 1 there. On any other platform we fall back
///          to the realtime default. The output flag tells the wait helper
///          which clock the deadline was computed against.
/// @param cond Condition variable to initialize.
/// @param uses_monotonic Receives whether timed waits use monotonic time.
/// @return 0 on success, errno-style error code on failure.
static int cq_cond_init(pthread_cond_t *cond, int8_t *uses_monotonic) {
    if (uses_monotonic)
        *uses_monotonic = 0;
#if RT_PLATFORM_MACOS
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
    {
        if (uses_monotonic)
            *uses_monotonic = 1;
        const int rc = pthread_cond_init(cond, &attr);
        if (rc != 0 && uses_monotonic)
            *uses_monotonic = 0;
        pthread_condattr_destroy(&attr);
        return rc;
    }
#else
    return pthread_cond_init(cond, NULL);
#endif
}

/// @brief Read the current clock for ConcQueue deadline computation, preferring
///        `CLOCK_MONOTONIC` when available and requested.
/// @details Same rationale as `rt_monitor.c::monitor_now_clock` — deadline math
///          for timed `Pop` / `Offer` calls wants a clock that's immune to
///          wall-clock adjustments. Falls back to `CLOCK_REALTIME` on platforms
///          without `CLOCK_MONOTONIC` or when the caller explicitly opts into
///          realtime (pthread-condvar default). Zero-init on error so
///          `clock_gettime` failure returns epoch rather than stack garbage.
/// @param use_monotonic Nonzero to prefer @c CLOCK_MONOTONIC.
/// @return Current time from the selected or fallback clock.
static struct timespec cq_now_clock(int8_t use_monotonic) {
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
#ifdef CLOCK_MONOTONIC
    if (use_monotonic && clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return ts;
#endif
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    return ts;
}

/// @brief Compute an absolute deadline @p timeout_ms in the future on the requested clock.
/// @details Used by Pop / Offer with timeout. Saturates at LONG_MAX seconds
///          if @p timeout_ms is large enough to overflow `time_t`. A non-
///          positive @p timeout_ms returns the current time so a caller
///          passing 0 immediately observes "deadline elapsed".
/// @param timeout_ms Relative timeout in milliseconds.
/// @param use_monotonic Nonzero to use the monotonic clock when available.
/// @return Saturating absolute deadline.
static cq_deadline_t cq_deadline_ms_from_now(int64_t timeout_ms, int8_t use_monotonic) {
    cq_deadline_t d;
    d.deadline = cq_now_clock(use_monotonic);
    if (timeout_ms <= 0)
        return d;
    int64_t add_sec = timeout_ms / 1000;
    long add_nsec = (long)((timeout_ms % 1000) * 1000000L);
    int64_t sec_room = (int64_t)LONG_MAX - (int64_t)d.deadline.tv_sec;
    if (add_sec > sec_room || (add_sec == sec_room && d.deadline.tv_nsec > 999999999L - add_nsec)) {
        d.deadline.tv_sec = (time_t)LONG_MAX;
        d.deadline.tv_nsec = 999999999L;
        return d;
    }
    d.deadline.tv_sec += (time_t)add_sec;
    d.deadline.tv_nsec += add_nsec;
    if (d.deadline.tv_nsec >= 1000000000L) {
        d.deadline.tv_sec++;
        d.deadline.tv_nsec -= 1000000000L;
    }
    return d;
}

#if RT_PLATFORM_MACOS
/// @brief Return milliseconds remaining until @p deadline (macOS only — uses relative wait).
/// @details macOS lacks pthread_condattr_setclock, so we manage timeouts as
///          deltas computed against the monotonic clock and pass them to
///          `pthread_cond_timedwait_relative_np`. Returns 0 when expired
///          and saturates at INT64_MAX for very large remaining intervals.
/// @param deadline Absolute deadline to compare.
/// @param use_monotonic Nonzero to read monotonic time.
/// @return Whole milliseconds remaining, clamped to 0..INT64_MAX.
static int64_t cq_remaining_ms(cq_deadline_t deadline, int8_t use_monotonic) {
    struct timespec now = cq_now_clock(use_monotonic);
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

/// @brief Cross-platform pthread_cond_timedwait against an absolute @p deadline.
/// @details On macOS, computes a relative timeout via cq_remaining_ms and
///          calls `pthread_cond_timedwait_relative_np`; returns ETIMEDOUT
///          immediately if the deadline has lapsed. On Linux/POSIX, calls
///          the standard `pthread_cond_timedwait` with the absolute
///          timespec. The mutex must already be held by the caller.
/// @param cond Condition variable to wait on.
/// @param mutex Locked mutex released and reacquired by the wait.
/// @param deadline Absolute deadline.
/// @param use_monotonic Nonzero when @p deadline uses monotonic time.
/// @return Zero when signalled, @c ETIMEDOUT on expiry, or another errno code.
static int cq_cond_timedwait_deadline(pthread_cond_t *cond,
                                      pthread_mutex_t *mutex,
                                      cq_deadline_t deadline,
                                      int8_t use_monotonic) {
#if RT_PLATFORM_MACOS
    int64_t remaining = cq_remaining_ms(deadline, use_monotonic);
    if (remaining <= 0)
        return ETIMEDOUT;
    struct timespec rel;
    rel.tv_sec = (time_t)(remaining / 1000);
    rel.tv_nsec = (long)((remaining % 1000) * 1000000L);
    return pthread_cond_timedwait_relative_np(cond, mutex, &rel);
#else
    return pthread_cond_timedwait(cond, mutex, &deadline.deadline);
#endif
}
#endif

// --- Internal helpers ---

/// @brief Validate-and-cast an opaque ConcurrentQueue handle to its impl.
/// @details Every public Pop / Offer / Count entry point dispatches through
///          this guard. NULL @p obj traps when @p trap_on_null is set
///          (otherwise returns NULL); wrong-class id always traps. The
///          NULL-trap toggle exists for non-trap convenience helpers
///          (Count returning 0 for a NULL handle).
/// @param obj Candidate ConcurrentQueue handle.
/// @param trap_on_null Nonzero to trap on a null handle.
/// @return Validated queue payload, or null for an accepted null/trap path.
static rt_concqueue_impl *concqueue_require(void *obj, int8_t trap_on_null) {
    if (!obj) {
        if (trap_on_null)
            rt_trap("ConcurrentQueue: null object");
        return NULL;
    }
    if (!rt_obj_is_instance(obj, RT_CONCQUEUE_CLASS_ID, sizeof(rt_concqueue_impl))) {
        rt_trap("ConcurrentQueue: invalid object");
        return NULL;
    }
    return (rt_concqueue_impl *)obj;
}

/// @brief Drop one GC reference to @p obj and free it if the count hit zero.
/// @param obj Runtime-managed object reference, or null.
static void concqueue_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Snapshot the current trap error message into @p buffer (or
///        @p fallback if none) so it survives lock cleanup before re-raise.
/// @param buffer Destination character array.
/// @param buffer_size Destination capacity including its terminator.
/// @param fallback Required message when no trap diagnostic is available.
static void concqueue_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Walk a linked list of queue nodes, releasing each retained value and freeing each node.
/// @details Used by Clear and the GC finalizer to drain any remaining
///          items. Decrements each value's refcount and frees the value
///          if it hits zero, then frees the linked-list node itself.
///          Robust against NULL @p n (no-op).
/// @param n Head of the detached node list, or null.
static void cq_release_nodes(cq_node *n) {
    while (n) {
        cq_node *next = n->next;
        if (rt_obj_release_check0(n->value))
            rt_obj_free(n->value);
        free(n);
        n = next;
    }
}

/// @brief Enumerate every managed value currently owned by a ConcurrentQueue.
/// @details Queue-link publication and removal occur inside the GC mutator
///          barrier, so collector-exclusive traversal can walk the raw node
///          chain without acquiring the queue mutex. Avoiding that mutex is
///          essential: a producer may own it while waiting for an active
///          collection to finish.
/// @param obj ConcurrentQueue payload being traversed.
/// @param visitor Collector visitor invoked for each non-null queued value.
/// @param ctx Opaque collector context forwarded to @p visitor.
static void cq_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    rt_concqueue_impl *cq = (rt_concqueue_impl *)obj;
    if (!cq || !visitor)
        return;
    for (cq_node *node = cq->head; node; node = node->next) {
        if (node->value)
            visitor(node->value, ctx);
    }
}

/// @brief Publish a prepared node at the queue tail while holding its mutex.
/// @details The graph-mutator scope makes the link and logical count one
///          atomic publication from the cycle collector's perspective. The
///          caller has already retained @p node's value.
/// @param cq Locked queue receiving the node.
/// @param node Prepared node whose ownership transfers to @p cq.
static void cq_publish_locked(rt_concqueue_impl *cq, cq_node *node) {
    rt_gc_mutator_enter();
    if (cq->tail)
        cq->tail->next = node;
    else
        cq->head = node;
    cq->tail = node;
    cq->count++;
    rt_gc_mutator_exit();
}

/// @brief Detach and return the queue head while holding its mutex.
/// @details No value reference is released: the queue's retained ownership is
///          transferred to the caller. Structural removal is protected from
///          concurrent collector traversal by the graph-mutator barrier.
/// @param cq Locked queue to pop.
/// @return Detached former head node, or null when empty.
static cq_node *cq_take_locked(rt_concqueue_impl *cq) {
    rt_gc_mutator_enter();
    cq_node *node = cq->head;
    if (node) {
        cq->head = node->next;
        if (!cq->head)
            cq->tail = NULL;
        node->next = NULL;
        cq->count--;
    }
    rt_gc_mutator_exit();
    return node;
}

/// @brief Detach the entire node chain while holding the queue mutex.
/// @details The returned nodes are no longer collector-visible and may be
///          released after the mutex is dropped, allowing value finalizers to
///          re-enter the queue safely.
/// @param cq Locked queue to empty.
/// @return Detached node-chain head, or null.
static cq_node *cq_detach_all_locked(rt_concqueue_impl *cq) {
    rt_gc_mutator_enter();
    cq_node *nodes = cq->head;
    cq->head = NULL;
    cq->tail = NULL;
    cq->count = 0;
    rt_gc_mutator_exit();
    return nodes;
}

/// @brief GC finalizer: drain the queue (releasing each retained value), then destroy the
/// platform mutex + condvar. Holds the lock during drain to interlock with any in-flight
/// enqueue (which would otherwise see freed memory).
/// @param obj ConcurrentQueue payload being finalized.
static void cq_finalizer(void *obj) {
    rt_concqueue_impl *cq = concqueue_require(obj, 0);
    if (!cq)
        return;

    CQ_LOCK(cq);
    cq->closed = 1;
#if RT_PLATFORM_WINDOWS
    WakeAllConditionVariable(&cq->cond);
#else
    pthread_cond_broadcast(&cq->cond);
#endif
    cq_node *n = cq_detach_all_locked(cq);
    CQ_UNLOCK(cq);

    cq_release_nodes(n);

#if RT_PLATFORM_WINDOWS
    DeleteCriticalSection(&cq->mutex);
    // CONDITION_VARIABLE does not need explicit destruction on Windows
#else
    pthread_cond_destroy(&cq->cond);
    pthread_mutex_destroy(&cq->mutex);
#endif
}

// --- Public API ---

/// @brief Create an empty unbounded queue with platform synchronization.
/// @return New runtime-managed ConcurrentQueue, or null after a construction trap.
void *rt_concqueue_new(void) {
    rt_concqueue_impl *cq =
        (rt_concqueue_impl *)rt_obj_new_i64(RT_CONCQUEUE_CLASS_ID, sizeof(rt_concqueue_impl));
    if (!cq) {
        rt_trap("ConcurrentQueue: memory allocation failed");
        return NULL;
    }
    cq->head = NULL;
    cq->tail = NULL;
    cq->count = 0;
    cq->closed = 0;
#if RT_PLATFORM_WINDOWS
    if (!InitializeCriticalSectionEx(&cq->mutex, 0, 0)) {
        concqueue_release_object(cq);
        rt_trap("ConcurrentQueue: mutex initialization failed");
        return NULL;
    }
    InitializeConditionVariable(&cq->cond);
#else
    if (pthread_mutex_init(&cq->mutex, NULL) != 0) {
        if (rt_obj_release_check0(cq))
            rt_obj_free(cq);
        rt_trap("ConcurrentQueue: mutex initialization failed");
        return NULL;
    }
    if (cq_cond_init(&cq->cond, &cq->cond_uses_monotonic) != 0) {
        pthread_mutex_destroy(&cq->mutex);
        if (rt_obj_release_check0(cq))
            rt_obj_free(cq);
        rt_trap("ConcurrentQueue: condition initialization failed");
        return NULL;
    }
#endif
    rt_obj_set_finalizer(cq, cq_finalizer);

    jmp_buf recovery;
    rt_concqueue_impl *volatile cq_for_cleanup = cq;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        concqueue_save_trap_error(
            saved_error, sizeof(saved_error), "ConcurrentQueue: GC tracking failed");
        rt_trap_clear_recovery();
        concqueue_release_object((void *)cq_for_cleanup);
        rt_trap(saved_error);
        return NULL;
    }
    rt_gc_track(cq, cq_traverse);
    if (!rt_gc_is_tracked(cq)) {
        rt_trap("ConcurrentQueue: GC tracking failed");
        return NULL;
    }
    rt_trap_clear_recovery();
    return (void *)cq;
}

/// @brief Read the exact queued-node count under the mutex.
/// @param obj ConcurrentQueue handle; null reports zero.
/// @return Element count at the instant of the locked read.
int64_t rt_concqueue_len(void *obj) {
    if (!obj)
        return 0;
    rt_concqueue_impl *cq = concqueue_require(obj, 0);
    if (!cq)
        return 0;
    rt_obj_retain_maybe(obj);
    CQ_LOCK(cq);
    int64_t len = cq->count;
    CQ_UNLOCK(cq);
    concqueue_release_object(obj);
    return len;
}

/// @brief Test whether the locked queue count is zero.
/// @param obj ConcurrentQueue handle; null is treated as empty.
/// @return 1 when empty, otherwise 0.
int8_t rt_concqueue_is_empty(void *obj) {
    return rt_concqueue_len(obj) == 0 ? 1 : 0;
}

/// @brief Returns 1 if `_close` has been called on the queue (no more enqueues allowed; pending
/// dequeue waiters wake immediately and return NULL once drained).
/// @param obj ConcurrentQueue handle.
/// @return Closed flag; null is treated as closed.
int8_t rt_concqueue_get_is_closed(void *obj) {
    if (!obj)
        return 1;
    rt_concqueue_impl *cq = concqueue_require(obj, 0);
    if (!cq)
        return 1;
    rt_obj_retain_maybe(obj);
    CQ_LOCK(cq);
    int8_t closed = cq->closed;
    CQ_UNLOCK(cq);
    concqueue_release_object(obj);
    return closed;
}

/// @brief Append a retained item and signal one waiting dequeuer.
/// @details Null items are permitted. Node allocation, retain, count-overflow,
///          and closed-queue failures trap without consuming the caller's
///          reference.
/// @param obj ConcurrentQueue handle; null is ignored.
/// @param item Runtime-managed item to retain, or null.
void rt_concqueue_enqueue(void *obj, void *item) {
    if (!obj)
        return;
    rt_concqueue_impl *cq = concqueue_require(obj, 0);
    if (!cq)
        return;
    rt_obj_retain_maybe(obj);

    cq_node *node = (cq_node *)malloc(sizeof(cq_node));
    if (!node) {
        concqueue_release_object(obj);
        rt_trap("ConcurrentQueue: memory allocation failed");
        return;
    }
    node->value = item;
    cq_node *volatile node_for_cleanup = node;
    void *volatile obj_for_cleanup = obj;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        concqueue_save_trap_error(
            saved_error, sizeof(saved_error), "ConcurrentQueue.Enqueue: item retain failed");
        rt_trap_clear_recovery();
        free((void *)node_for_cleanup);
        concqueue_release_object((void *)obj_for_cleanup);
        rt_trap(saved_error);
        return;
    }
    rt_obj_retain_maybe(item);
    rt_trap_clear_recovery();
    node->next = NULL;

    CQ_LOCK(cq);
    if (cq->closed) {
        CQ_UNLOCK(cq);
        if (item && rt_obj_release_check0(item))
            rt_obj_free(item);
        free(node);
        concqueue_release_object(obj);
        rt_trap("ConcurrentQueue.Enqueue: queue is closed");
        return;
    }
    if (cq->count == INT64_MAX) {
        CQ_UNLOCK(cq);
        if (item && rt_obj_release_check0(item))
            rt_obj_free(item);
        free(node);
        concqueue_release_object(obj);
        rt_trap("ConcurrentQueue.Enqueue: count overflow");
        return;
    }
    cq_publish_locked(cq, node);
    CQ_SIGNAL(cq);
    CQ_UNLOCK(cq);
    concqueue_release_object(obj);
}

/// @brief Status-returning enqueue used by internal queues that must preserve payload ownership
///        across memory pressure and shutdown races.
/// @details Unlike the public strict enqueue, node-allocation failure and a queue that closes while
///          the node is prepared return zero. The value retain is acquired before publication and
///          rolled back if the close check loses its race. Null items are
///          rejected by this status-returning variant.
/// @param obj ConcurrentQueue handle; null returns failure.
/// @param item Nonnull runtime-managed item to retain on success.
/// @return 1 when enqueued, otherwise 0 for null, allocation, close, or count overflow.
int8_t rt_concqueue_try_enqueue(void *obj, void *item) {
    rt_concqueue_impl *cq;
    cq_node *node;
    if (!obj || !item)
        return 0;
    cq = concqueue_require(obj, 0);
    if (!cq)
        return 0;
    rt_obj_retain_maybe(obj);
    node = (cq_node *)malloc(sizeof(*node));
    if (!node) {
        concqueue_release_object(obj);
        return 0;
    }
    node->value = item;
    node->next = NULL;
    cq_node *volatile node_for_cleanup = node;
    void *volatile obj_for_cleanup = obj;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        concqueue_save_trap_error(
            saved_error, sizeof(saved_error), "ConcurrentQueue.TryEnqueue: item retain failed");
        rt_trap_clear_recovery();
        free((void *)node_for_cleanup);
        concqueue_release_object((void *)obj_for_cleanup);
        rt_trap(saved_error);
        return 0;
    }
    rt_obj_retain_maybe(item);
    rt_trap_clear_recovery();

    CQ_LOCK(cq);
    if (cq->closed || cq->count == INT64_MAX) {
        CQ_UNLOCK(cq);
        if (rt_obj_release_check0(item))
            rt_obj_free(item);
        free(node);
        concqueue_release_object(obj);
        return 0;
    }
    cq_publish_locked(cq, node);
    CQ_SIGNAL(cq);
    CQ_UNLOCK(cq);
    concqueue_release_object(obj);
    return 1;
}

/// @brief Non-blocking dequeue: pop the head if available, otherwise return NULL immediately.
/// **Ownership transfer:** the returned value carries the retain that `_enqueue` added — caller
/// must release. The wrapping node struct is freed before returning.
/// @param obj ConcurrentQueue handle; null behaves as empty.
/// @return Transferred oldest value, or null for empty or a queued null.
void *rt_concqueue_try_dequeue(void *obj) {
    if (!obj)
        return NULL;
    rt_concqueue_impl *cq = concqueue_require(obj, 0);
    if (!cq)
        return NULL;
    rt_obj_retain_maybe(obj);

    CQ_LOCK(cq);
    if (!cq->head) {
        CQ_UNLOCK(cq);
        concqueue_release_object(obj);
        return NULL;
    }

    cq_node *node = cq_take_locked(cq);
    CQ_UNLOCK(cq);

    void *value = node->value;
    free(node);
    concqueue_release_object(obj);
    return value;
}

/// @brief Non-blocking dequeue as an Option.
/// @details Returns `None` only when the queue is empty. This wrapper removes
///          the node while holding the queue lock so a queued NULL value can be
///          returned as `Some(NULL)`. The node's retained value transfer is
///          released after the Option has retained it.
/// @param obj ConcurrentQueue pointer.
/// @return Opaque Zanna.Option object containing the dequeued value, or None.
void *rt_concqueue_try_dequeue_option(void *obj) {
    if (!obj)
        return rt_option_none();
    rt_concqueue_impl *cq = concqueue_require(obj, 0);
    if (!cq)
        return rt_option_none();
    rt_obj_retain_maybe(obj);

    CQ_LOCK(cq);
    if (!cq->head) {
        CQ_UNLOCK(cq);
        concqueue_release_object(obj);
        return rt_option_none();
    }

    cq_node *node = cq_take_locked(cq);
    CQ_UNLOCK(cq);

    void *value = node->value;
    free(node);
    concqueue_release_object(obj);

    jmp_buf recovery;
    void *volatile value_for_cleanup = value;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        concqueue_save_trap_error(saved_error,
                                  sizeof(saved_error),
                                  "ConcurrentQueue.TryDequeueOption: allocation failed");
        rt_trap_clear_recovery();
        concqueue_release_object((void *)value_for_cleanup);
        rt_trap(saved_error);
        return NULL;
    }
    void *option = rt_option_some(value);
    rt_trap_clear_recovery();
    concqueue_release_object(value);
    return option;
}

/// @brief Blocking dequeue: wait indefinitely until an item is enqueued or the queue is closed.
/// On close-with-empty, returns NULL (graceful end-of-stream signal). Wrapped in a `while` loop
/// to handle spurious wakeups robustly.
/// @param obj ConcurrentQueue handle; null behaves as closed and empty.
/// @return Transferred oldest value, or null for closed-and-empty or a queued null.
void *rt_concqueue_dequeue(void *obj) {
    if (!obj)
        return NULL;
    rt_concqueue_impl *cq = concqueue_require(obj, 0);
    if (!cq)
        return NULL;
    rt_obj_retain_maybe(obj);

    CQ_LOCK(cq);
    while (!cq->head && !cq->closed) {
#if RT_PLATFORM_WINDOWS
        if (!CQ_WAIT(cq)) {
            CQ_UNLOCK(cq);
            concqueue_release_object(obj);
            rt_trap("ConcurrentQueue.Dequeue: condition wait failed");
            return NULL;
        }
#else
        if (CQ_WAIT(cq) != 0) {
            CQ_UNLOCK(cq);
            concqueue_release_object(obj);
            rt_trap("ConcurrentQueue.Dequeue: condition wait failed");
            return NULL;
        }
#endif
    }
    if (!cq->head) {
        CQ_UNLOCK(cq);
        concqueue_release_object(obj);
        return NULL;
    }

    cq_node *node = cq_take_locked(cq);
    CQ_UNLOCK(cq);

    void *value = node->value;
    free(node);
    concqueue_release_object(obj);
    return value;
}

/// @brief Bounded-wait dequeue: same as `_dequeue` but returns NULL after `timeout_ms` if no item
/// arrives. `timeout_ms <= 0` falls through to non-blocking `_try_dequeue`. Cross-platform via
/// `SleepConditionVariableCS` (Win32) or `pthread_cond_timedwait` (POSIX) with the same monotonic-
/// clock preference as the rest of the runtime.
/// @param obj ConcurrentQueue handle; null behaves as empty.
/// @param timeout_ms Condition-wait timeout in milliseconds.
/// @return Transferred oldest value, or null on timeout, close, empty, or a queued null.
void *rt_concqueue_dequeue_timeout(void *obj, int64_t timeout_ms) {
    if (!obj)
        return NULL;
    rt_concqueue_impl *cq = concqueue_require(obj, 0);
    if (!cq)
        return NULL;
    if (timeout_ms <= 0)
        return rt_concqueue_try_dequeue(obj);
    rt_obj_retain_maybe(obj);

#if RT_PLATFORM_WINDOWS
    CQ_LOCK(cq);
    ULONGLONG deadline = rt_win32_deadline_from_now_ms(timeout_ms);
    while (!cq->head && !cq->closed) {
        DWORD remaining = rt_win32_wait_slice_until(deadline);
        if (remaining == 0) {
            CQ_UNLOCK(cq);
            concqueue_release_object(obj);
            return NULL;
        }
        if (!SleepConditionVariableCS(&cq->cond, &cq->mutex, remaining)) {
            DWORD err = GetLastError();
            if (err == ERROR_TIMEOUT) {
                continue;
            }
            CQ_UNLOCK(cq);
            concqueue_release_object(obj);
            rt_trap("ConcurrentQueue.DequeueTimeout: condition wait failed");
            return NULL;
        }
    }
#else
    cq_deadline_t deadline = cq_deadline_ms_from_now(timeout_ms, cq->cond_uses_monotonic);

    CQ_LOCK(cq);
    while (!cq->head && !cq->closed) {
        int rc =
            cq_cond_timedwait_deadline(&cq->cond, &cq->mutex, deadline, cq->cond_uses_monotonic);
        if (rc == ETIMEDOUT) {
            if (!cq->head && !cq->closed) {
                CQ_UNLOCK(cq);
                concqueue_release_object(obj);
                return NULL;
            }
            continue;
        } else if (rc != 0) {
            CQ_UNLOCK(cq);
            concqueue_release_object(obj);
            rt_trap("ConcurrentQueue.DequeueTimeout: condition wait failed");
            return NULL;
        }
    }
#endif

    if (!cq->head) {
        CQ_UNLOCK(cq);
        concqueue_release_object(obj);
        return NULL;
    }

    cq_node *node = cq_take_locked(cq);
    CQ_UNLOCK(cq);

    void *value = node->value;
    free(node);
    concqueue_release_object(obj);
    return value;
}

/// @brief Read the head value without removing it. Returns a freshly-retained reference (caller
/// releases). NULL on empty queue. Distinct from a `_try_dequeue` peek because the value stays in
/// the queue — useful for "is this the message I want?" inspection patterns.
/// @param obj ConcurrentQueue handle; null behaves as empty.
/// @return Newly retained front value, or null for empty or a null-valued head.
void *rt_concqueue_peek(void *obj) {
    if (!obj)
        return NULL;
    rt_concqueue_impl *cq = concqueue_require(obj, 0);
    if (!cq)
        return NULL;
    rt_obj_retain_maybe(obj);

    CQ_LOCK(cq);
    void *value = cq->head ? cq->head->value : NULL;
    if (value) {
        rt_concqueue_impl *volatile locked_cq = cq;
        void *volatile obj_for_cleanup = obj;
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (setjmp(recovery) != 0) {
            char saved_error[256];
            concqueue_save_trap_error(
                saved_error, sizeof(saved_error), "ConcurrentQueue.Peek: item retain failed");
            rt_trap_clear_recovery();
            CQ_UNLOCK((rt_concqueue_impl *)locked_cq);
            concqueue_release_object((void *)obj_for_cleanup);
            rt_trap(saved_error);
            return NULL;
        }
        rt_obj_retain_maybe(value);
        rt_trap_clear_recovery();
    }
    CQ_UNLOCK(cq);
    concqueue_release_object(obj);
    return value;
}

/// @brief Atomically detach all nodes and release their values after unlocking.
/// @param obj ConcurrentQueue handle; null is ignored.
void rt_concqueue_clear(void *obj) {
    if (!obj)
        return;
    rt_concqueue_impl *cq = concqueue_require(obj, 0);
    if (!cq)
        return;
    rt_obj_retain_maybe(obj);

    CQ_LOCK(cq);
    cq_node *n = cq_detach_all_locked(cq);
    CQ_UNLOCK(cq);
    cq_release_nodes(n);
    concqueue_release_object(obj);
}

/// @brief Mark the queue as closed and wake every blocked dequeue waiter. Subsequent enqueues
/// trap; subsequent dequeues drain remaining items, then return NULL. Idempotent — re-closing
/// is a no-op. The producer-side "we're done sending" signal for graceful pipeline shutdown.
/// @param obj ConcurrentQueue handle; null is ignored.
void rt_concqueue_close(void *obj) {
    if (!obj)
        return;
    rt_concqueue_impl *cq = concqueue_require(obj, 0);
    if (!cq)
        return;
    rt_obj_retain_maybe(obj);
    CQ_LOCK(cq);
    if (cq->closed) {
        CQ_UNLOCK(cq);
        concqueue_release_object(obj);
        return;
    }
    cq->closed = 1;
#if RT_PLATFORM_WINDOWS
    WakeAllConditionVariable(&cq->cond);
#else
    pthread_cond_broadcast(&cq->cond);
#endif
    CQ_UNLOCK(cq);
    concqueue_release_object(obj);
}
