//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_threadpool.c
// Purpose: Implements a fixed-size thread pool for the Zanna.Threads.Pool class.
//          Worker threads dequeue tasks from a FIFO linked-list queue protected
//          by a monitor. Supports Submit, Wait (drain all pending tasks), and
//          Shutdown.
//
// Key invariants:
//   - Worker count is fixed at construction; it cannot change after creation.
//   - Submit enqueues a (callback, arg) pair; workers dequeue and execute in FIFO.
//   - Wait blocks until all submitted tasks have completed execution.
//   - Shutdown signals workers to exit after draining the queue, then joins them.
//   - ShutdownNow discards any pending tasks before joining workers.
//   - Submitting to a shut-down pool returns false immediately.
//   - Task traps are reported once: the first subsequent Wait, WaitFor, Shutdown, or
//     ShutdownNow call drains the captured worker error and rethrows it on the caller
//     thread instead of silently dropping it.
//   - Calls into Wait / WaitFor / Shutdown / ShutdownNow from a worker already
//     running in the same pool trap to prevent self-deadlock under exhaustion.
//   - Worker handles are stolen under the pool monitor and joined outside the lock,
//     so repeated Shutdown calls and concurrent finalization can never double-join.
//   - All queue and state access is serialized through the monitor.
//
// Ownership/Lifetime:
//   - Borrowed task arguments are not retained; callers must keep them alive
//     through execution. SubmitOwned retains managed arguments until execution
//     or discard, and queued owned arguments are exposed to cycle collection.
//   - Worker thread handles are owned by the pool and joined on Shutdown.
//   - The pool itself is heap-allocated and managed by the runtime GC.
//
// Links: src/runtime/threads/rt_threadpool.h (public API),
//        src/runtime/threads/rt_threads.h (OS thread creation and joining),
//        src/runtime/threads/rt_monitor.h (synchronization for task queue)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the fixed-size `Zanna.Threads.Pool` task executor.
/// @details A monitor-protected FIFO feeds persistent workers. Submission may
///          borrow or retain task arguments, waits observe queue-and-active
///          drain state, and graceful or immediate shutdown elects exactly one
///          caller to join stolen worker handles. Worker traps are captured and
///          consumed by the next wait or shutdown observer.

#include "rt_threadpool.h"

#include "rt_gc.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_string.h"
#include "rt_threads.h"

#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if RT_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#define RT_THREADPOOL_DEFAULT_MAX_PENDING 65536

//=============================================================================
// Internal Structures
//=============================================================================

/// @brief Task entry in the queue.
typedef struct pool_task {
    rt_threadpool_task_fn callback; ///< Task function pointer.
    void *arg;                      ///< Argument for the callback.
    int8_t owns_arg;                ///< 1 when the pool retains/releases the arg (VDOC-128).
    struct pool_task *next;         ///< Next task in queue.
} pool_task;

/// @brief Release a task's owned argument (no-op for borrowed args).
/// @param task Task whose retained argument should be released and cleared.
static void pool_task_release_arg(pool_task *task) {
    if (task && task->owns_arg && task->arg) {
        if (rt_obj_release_check0(task->arg))
            rt_obj_free(task->arg);
        task->arg = NULL;
        task->owns_arg = 0;
    }
}

/// @brief Worker thread state.
typedef struct pool_worker {
    void *thread;           ///< Thread handle.
    struct pool_impl *pool; ///< Back-reference to pool.
} pool_worker;

/// @brief Thread pool implementation.
typedef struct pool_impl {
    void *monitor;            ///< Monitor for synchronization.
    pool_task *queue_head;    ///< Head of task queue.
    pool_task *queue_tail;    ///< Tail of task queue.
    pool_worker *workers;     ///< Array of workers.
    int64_t worker_count;     ///< Number of workers.
    int64_t pending_count;    ///< Number of tasks in queue.
    int64_t active_count;     ///< Number of tasks running.
    int64_t error_count;      ///< Number of worker task traps not yet observed.
    char last_error[512];     ///< Last worker task trap message.
    int shutdown;             ///< Atomic shutdown flag.
    int shutdown_now;         ///< Atomic immediate-shutdown flag.
    int8_t shutdown_joining;  ///< One caller owns the worker-handle join phase.
    int8_t shutdown_complete; ///< Every worker handle has finished and been released.
    int cleanup_scheduled;    ///< Atomic deferred-cleanup state (0 none, 1 thread, 2 fallback).
    int64_t max_pending;      ///< Maximum queued tasks before Submit applies backpressure.
} pool_impl;

//=============================================================================
// Forward Declarations
//=============================================================================

/// @brief Finalize a Pool, stopping workers and releasing queued resources.
/// @param obj Pool object being finalized.
static void pool_finalizer(void *obj);

/// @brief Run one persistent worker's dequeue-and-execute loop.
/// @param arg Pointer to the worker record owned by its Pool.
static void worker_entry(void *arg);

/// @brief Shut down a resurrected Pool from a detached cleanup thread.
/// @param arg Retained Pool object transferred to the cleanup thread.
static void pool_deferred_cleanup_entry(void *arg);

#include "rt_trap.h"

/// @brief Install a non-local recovery destination for runtime traps.
/// @param buf Jump buffer that receives control when a trap is raised.
void rt_trap_set_recovery(jmp_buf *buf);

/// @brief Remove the active runtime trap recovery destination.
void rt_trap_clear_recovery(void);

/// @brief Read the diagnostic associated with the current recovered trap.
/// @return Borrowed NUL-terminated diagnostic text, or NULL when unavailable.
const char *rt_trap_get_error(void);

static RT_THREAD_LOCAL pool_impl *g_current_worker_pool = NULL;
static RT_THREAD_LOCAL pool_worker *g_exiting_worker = NULL;

/// @brief Read whether shutdown has been requested with acquire ordering.
/// @param pool Pool whose atomic shutdown flag is inspected.
/// @return One after either shutdown mode has been requested, otherwise zero.
static int8_t pool_shutdown_requested(const pool_impl *pool) {
    return pool && __atomic_load_n(&pool->shutdown, __ATOMIC_ACQUIRE) ? 1 : 0;
}

/// @brief Read whether immediate shutdown has been requested with acquire ordering.
/// @param pool Pool whose immediate-shutdown flag is inspected.
/// @return One after immediate shutdown has been requested, otherwise zero.
static int8_t pool_shutdown_now_requested(const pool_impl *pool) {
    return pool && __atomic_load_n(&pool->shutdown_now, __ATOMIC_ACQUIRE) ? 1 : 0;
}

/// @brief Publish a shutdown request visible even when a collector prevents monitor acquisition.
/// @details The ordinary lifecycle paths still take the pool monitor for queue
///          state and wakeups. Atomic flags provide the resource-exhaustion
///          fallback used by a finalizer that cannot create its cleanup thread.
/// @param pool Pool whose workers should stop.
/// @param immediate Non-zero to discard queued work rather than drain it.
static void pool_request_shutdown_atomic(pool_impl *pool, int8_t immediate) {
    if (!pool)
        return;
    if (immediate)
        __atomic_store_n(&pool->shutdown_now, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&pool->shutdown, 1, __ATOMIC_RELEASE);
}

/// @brief Atomically transition the deferred-cleanup state.
/// @param pool Pool whose cleanup ownership is being changed.
/// @param expected Required current state.
/// @param desired New state on success.
/// @return Non-zero when the transition succeeded.
static int8_t pool_cleanup_transition(pool_impl *pool, int expected, int desired) {
    return pool && __atomic_compare_exchange_n(&pool->cleanup_scheduled,
                                               &expected,
                                               desired,
                                               0,
                                               __ATOMIC_ACQ_REL,
                                               __ATOMIC_ACQUIRE)
               ? 1
               : 0;
}

/// @brief Decode the legacy object-pointer representation of a Pool callback.
/// @details IL transports native callback addresses through its `obj` ABI.
///          Native runtime callers use the typed API directly; this single
///          compatibility boundary copies the representation after proving the
///          platform gives data and function pointers equal ABI widths.
/// @param opaque Opaque callback address supplied by generated code.
/// @return Typed callback with the same representation, or NULL.
static rt_threadpool_task_fn pool_task_from_opaque(void *opaque) {
    _Static_assert(sizeof(rt_threadpool_task_fn) == sizeof(void *),
                   "pool callback and object pointers must have equal ABI size");
    rt_threadpool_task_fn callback = NULL;
    memcpy(&callback, &opaque, sizeof(callback));
    return callback;
}

/// @brief Compute the default pending-task limit for new thread pools.
/// @details Uses ZANNA_THREADPOOL_MAX_PENDING when it contains a positive
///          integer, otherwise falls back to a conservative fixed cap. The cap
///          prevents unbounded queue growth under load while preserving existing
///          asynchronous behavior for normal workloads.
/// @return Maximum number of tasks allowed to wait in the queue.
static int64_t pool_default_max_pending(void) {
    const char *env = getenv("ZANNA_THREADPOOL_MAX_PENDING");
    if (env && env[0]) {
        char *end = NULL;
        errno = 0;
        long long value = strtoll(env, &end, 10);
        if (errno != ERANGE && end && *end == '\0' && value > 0)
            return (int64_t)value;
    }
    return RT_THREADPOOL_DEFAULT_MAX_PENDING;
}

/// @brief Release a worker-thread handle held in @p *slot and NULL the slot.
/// @details Standard ownership-discipline helper used during pool shutdown
///          and finalize. Releases the runtime ref; if the refcount drops
///          to zero the object is freed. NULLing the slot prevents any
///          subsequent re-release from double-freeing.
/// @param slot Address of an owned worker-handle slot.
static void pool_release_thread_handle(void **slot) {
    if (!slot || !*slot)
        return;
    if (rt_obj_release_check0(*slot))
        rt_obj_free(*slot);
    *slot = NULL;
}

/// @brief Release a retained pool reference; free the impl when the count reaches zero.
/// @details Used by deferred-cleanup paths that hold a pool ref across
///          background work. Safe to call with NULL — no-op in that case.
/// @param pool Retained Pool reference to release, or NULL.
static void pool_release_object(pool_impl *pool) {
    if (pool && rt_obj_release_check0(pool))
        rt_obj_free(pool);
}

/// @brief Validate-and-cast an opaque pool pointer to its impl.
/// @details Every public Pool entry point dispatches through this guard.
///          NULL @p pool_obj triggers a trap iff @p trap_on_null is set
///          (otherwise returns NULL); wrong-class id always traps. Returns
///          the downcast pointer on success.
/// @param pool_obj Candidate Pool runtime object.
/// @param what Diagnostic used when NULL must trap.
/// @param trap_on_null Whether a NULL handle raises a runtime trap.
/// @return Valid Pool payload, or NULL for tolerated or returning trap paths.
static pool_impl *pool_require(void *pool_obj, const char *what, int8_t trap_on_null) {
    if (!pool_obj) {
        if (trap_on_null)
            rt_trap(what ? what : "Pool: null object");
        return NULL;
    }
    if (!rt_obj_is_instance(pool_obj, RT_THREADPOOL_CLASS_ID, sizeof(pool_impl))) {
        rt_trap("Pool: invalid object");
        return NULL;
    }
    return (pool_impl *)pool_obj;
}

/// @brief If the pool has captured a worker-task trap message, copy it to @p out and report 1.
/// @details Caller must hold pool->monitor. Used by Wait / Shutdown to
///          surface the first task-trap message seen by any worker. The
///          buffer is null-terminated (truncating long messages) so the
///          caller can pass it directly to rt_trap. Returns 0 if no
///          worker has trapped since the last error-take.
/// @param pool Locked Pool whose error state is consumed.
/// @param out Optional destination for the captured diagnostic.
/// @param out_size Capacity of @p out in bytes.
/// @return One when accumulated errors were consumed, otherwise zero.
static int8_t pool_take_error_locked(pool_impl *pool, char *out, size_t out_size) {
    if (!pool || pool->error_count <= 0)
        return 0;
    const char *msg = pool->last_error[0] ? pool->last_error : "Pool.Wait: task trapped";
    if (out && out_size > 0) {
        strncpy(out, msg, out_size - 1);
        out[out_size - 1] = '\0';
    }
    pool->error_count = 0;
    pool->last_error[0] = '\0';
    return 1;
}

/// @brief Drain the accumulated worker-task trap state under the pool's monitor.
/// @details The pool counts every unobserved trap but stores only the most
///          recently captured message. Used by Wait / Shutdown callers after
///          releasing any temporary pool retain so trap unwinding cannot leak
///          the self-retain.
/// @param pool Pool whose monitor and error state are inspected.
/// @param error Optional destination for the captured diagnostic.
/// @param error_size Capacity of @p error in bytes.
/// @return One when accumulated errors were consumed, otherwise zero.
static int8_t pool_take_error(pool_impl *pool, char *error, size_t error_size) {
    if (error && error_size > 0)
        error[0] = '\0';
    rt_monitor_enter(pool->monitor);
    int8_t has_error = pool_take_error_locked(pool, error, error_size);
    rt_monitor_exit(pool->monitor);
    return has_error;
}

/// @brief Move every worker thread handle out of the pool's `workers` array into @p handles.
/// @details Caller must hold pool->monitor. Steals the thread handles
///          (NULLing the pool's slots) so the caller can join them
///          without holding the lock. Used by Shutdown's two-phase
///          sequence: take handles under the lock, drop the lock, then
///          join the handles to avoid blocking submit/wait callers.
/// @param pool Locked Pool whose handle slots are consumed.
/// @param handles Zeroed worker-count array receiving the owned handles.
static void pool_take_worker_handles_locked(pool_impl *pool, void **handles) {
    if (!pool || !handles || !pool->workers)
        return;
    for (int64_t i = 0; i < pool->worker_count; i++) {
        handles[i] = pool->workers[i].thread;
        pool->workers[i].thread = NULL;
    }
}

/// @brief Elect one shutdown caller to own worker joining.
/// @details Must be called with the pool monitor held. The first caller before
///          completion marks `shutdown_joining` and steals all worker handles;
///          later callers leave handles untouched and wait for the owner to
///          publish completion. This preserves idempotence without allowing a
///          repeated Shutdown call to return while workers are still active.
/// @param pool Pool whose monitor is currently owned by the caller.
/// @param handles Zeroed worker-count array receiving handles for the elected owner.
/// @return One when the caller owns joining; zero when it must wait or shutdown is complete.
static int8_t pool_claim_shutdown_join_locked(pool_impl *pool, void **handles) {
    if (!pool || pool->shutdown_complete || pool->shutdown_joining)
        return 0;
    pool->shutdown_joining = 1;
    pool_take_worker_handles_locked(pool, handles);
    return 1;
}

/// @brief Wait for shutdown completion or claim a join phase abandoned after a trap.
/// @details Must be called with the pool monitor held. If another caller owns
///          joining, the function waits until that caller either publishes
///          completion or restores its unjoined handles after a recoverable
///          trap. In the latter case this caller atomically becomes the next
///          join owner. This prevents both premature Shutdown returns and a
///          permanent waiter deadlock after an exceptional native wait.
/// @param pool Pool whose shutdown has already been requested.
/// @param handles Zeroed worker-count array receiving handles when ownership is claimed.
/// @return One when the caller must join @p handles; zero when shutdown is complete.
static int8_t pool_wait_or_claim_shutdown_locked(pool_impl *pool, void **handles) {
    while (pool && !pool->shutdown_complete) {
        if (!pool->shutdown_joining)
            return pool_claim_shutdown_join_locked(pool, handles);
        rt_monitor_wait(pool->monitor);
    }
    return 0;
}

/// @brief Restore handles that could not be joined and relinquish join ownership.
/// @details Must be called with the pool monitor held. Each non-NULL entry is
///          moved back to the corresponding worker slot, then the caller-owner
///          flag is cleared and all waiters are awakened. Already joined and
///          released entries are NULL and remain absent. Restoring ownership is
///          essential because workers use a borrowed pool back-reference; the
///          pool must not discard an unjoined handle and finalize underneath it.
/// @param pool Pool whose join phase trapped before completion.
/// @param handles Partially consumed worker-handle array to restore.
/// @param count Number of entries in @p handles.
static void pool_restore_worker_handles_locked(pool_impl *pool, void **handles, int64_t count) {
    if (!pool)
        return;
    if (handles && pool->workers) {
        for (int64_t i = 0; i < count; ++i) {
            if (!handles[i])
                continue;
            if (!pool->workers[i].thread) {
                pool->workers[i].thread = handles[i];
                handles[i] = NULL;
            }
        }
    }
    pool->shutdown_joining = 0;
    rt_monitor_pause_all(pool->monitor);
}

/// @brief Publish completion of the worker-join phase and wake shutdown waiters.
/// @details Acquires the monitor, clears the single-owner flag, sets the stable
///          completion flag with monitor synchronization, and broadcasts to
///          every concurrent graceful or immediate shutdown caller.
/// @param pool Pool whose stolen worker handles have all been joined and released.
static void pool_publish_shutdown_complete(pool_impl *pool) {
    if (!pool || !pool->monitor)
        return;
    rt_monitor_enter(pool->monitor);
    pool->shutdown_joining = 0;
    pool->shutdown_complete = 1;
    rt_monitor_pause_all(pool->monitor);
    rt_monitor_exit(pool->monitor);
}

/// @brief Join every non-NULL handle in @p handles and release each retained ref.
/// @details Companion to pool_take_worker_handles_locked. Walks the handle
///          array, calls rt_thread_join on each, and releases the runtime
///          ref. NULL slots are skipped (the take helper may have left
///          some empty if the pool was constructed but never started).
/// @param handles Owned worker-handle array.
/// @param count Number of entries in @p handles.
/// @param skip_index Worker index not joined because it is the current exiting thread.
static void pool_join_worker_handles(void **handles, int64_t count, int64_t skip_index) {
    if (!handles)
        return;
    for (int64_t i = 0; i < count; i++) {
        if (handles[i]) {
            if (i != skip_index)
                rt_thread_join(handles[i]);
            pool_release_thread_handle(&handles[i]);
        }
    }
}

/// @brief Complete an elected shutdown join phase with recoverable-trap rollback.
/// @details Joins and releases each stolen worker handle. A trap from a native
///          thread wait is caught locally so every unconsumed handle, including
///          the active one, can be restored to the pool before ownership is
///          relinquished. Successful completion publishes the stable completion
///          condition. On failure the original diagnostic is copied to @p error
///          and the caller may re-raise it only after releasing temporary memory
///          and its pool retain.
/// @param pool Pool that elected the current caller as join owner.
/// @param handles Stolen worker-handle array.
/// @param count Number of entries in @p handles.
/// @param error Destination for a bounded, NUL-terminated trap diagnostic.
/// @param error_size Capacity of @p error in bytes.
/// @return One after all handles are joined and completion is published; zero
///         after a trap and restoration of every unjoined handle.
static int8_t pool_finish_shutdown_join(
    pool_impl *pool, void **handles, int64_t count, char *error, size_t error_size) {
    volatile int64_t active_index = -1;
    void *volatile active_handle = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        const char *message = rt_trap_get_error();
        if (error && error_size > 0) {
            snprintf(error,
                     error_size,
                     "%s",
                     message && message[0] ? message : "Pool.Shutdown: worker join failed");
        }
        rt_trap_clear_recovery();

        if (active_index >= 0 && active_index < count && active_handle && !handles[active_index]) {
            handles[active_index] = (void *)active_handle;
        }
        rt_monitor_enter(pool->monitor);
        pool_restore_worker_handles_locked(pool, handles, count);
        rt_monitor_exit(pool->monitor);
        return 0;
    }

    for (int64_t i = 0; i < count; ++i) {
        if (!handles[i])
            continue;
        active_index = i;
        active_handle = handles[i];
        handles[i] = NULL;
        rt_thread_join((void *)active_handle);
        void *owned_handle = (void *)active_handle;
        pool_release_thread_handle(&owned_handle);
        active_handle = NULL;
        active_index = -1;
    }
    rt_trap_clear_recovery();
    pool_publish_shutdown_complete(pool);
    return 1;
}

/// @brief Allocate a snapshot array of the pool's worker handles, taking ownership under the lock.
/// @details Wrapper around `pool_take_worker_handles_locked` that handles the calloc and the
///          monitor enter/exit dance. Steals every live worker pointer into the returned array
///          and zeroes the pool's per-worker slot for each one — so the caller can safely join
///          the workers outside the lock without racing a concurrent shutdown that might also
///          try to claim them. Returns NULL on a NULL pool, an empty pool, or `calloc` failure;
///          callers must `free` the returned array after joining its handles.
/// @param pool Pool whose worker-handle ownership should be detached.
/// @return Heap worker-count array containing stolen handles, or NULL.
static void **pool_detach_worker_handles(pool_impl *pool) {
    if (!pool || pool->worker_count <= 0)
        return NULL;
    void **handles = (void **)calloc((size_t)pool->worker_count, sizeof(void *));
    if (!handles)
        return NULL;
    if (pool->monitor)
        rt_monitor_enter(pool->monitor);
    pool_take_worker_handles_locked(pool, handles);
    if (pool->monitor)
        rt_monitor_exit(pool->monitor);
    return handles;
}

//=============================================================================
// Pool Management
//=============================================================================

/// @brief GC traverse callback for thread pools.
/// @details Visits each queued argument retained by SubmitOwned. Queue-link mutation is
///          protected by the managed-graph barrier, and traversal runs under its exclusive
///          side, so the queue is stable without taking the pool monitor. Borrowed arguments,
///          worker handles, and the native monitor are not strong managed-graph edges.
/// @param obj Pool object whose queued owned arguments are enumerated.
/// @param visitor GC callback invoked for every strong managed argument edge.
/// @param ctx Opaque traversal context forwarded to @p visitor.
static void pool_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    pool_impl *pool = (pool_impl *)obj;
    if (!pool || !visitor)
        return;
    for (pool_task *task = pool->queue_head; task; task = task->next)
        if (task->owns_arg && task->arg)
            visitor(task->arg, ctx);
}

/// @brief Detach every queued task while holding the pool monitor.
/// @details Queue edge removal is enclosed in the managed-graph mutator scope;
///          returned tasks must be released outside the monitor so owned-arg
///          finalizers may safely call back into the pool.
/// @param pool Locked Pool whose queued task chain is detached.
/// @return Previous queue head, or NULL when no tasks were pending.
static pool_task *pool_detach_tasks_locked(pool_impl *pool) {
    if (!pool)
        return NULL;
    rt_gc_mutator_enter();
    pool_task *tasks = pool->queue_head;
    pool->queue_head = NULL;
    pool->queue_tail = NULL;
    pool->pending_count = 0;
    rt_gc_mutator_exit();
    return tasks;
}

/// @brief Release a detached task list and every retained owned argument.
/// @param tasks First detached task, or NULL.
static void pool_release_task_list(pool_task *tasks) {
    while (tasks) {
        pool_task *next = tasks->next;
        tasks->next = NULL;
        pool_task_release_arg(tasks);
        free(tasks);
        tasks = next;
    }
}

/// @brief GC finalizer for a `ThreadPool` — drain, join, and tear down.
/// @details Two paths, gated on whether the current thread is one of the pool's
///          own workers:
///
///          **Self-finalize guard.** If a worker thread is the last owner (a user
///          callback returned the pool as its only live reference), joining and
///          freeing the worker array + monitor from that same worker would free
///          state the thread is still actively executing. The finalizer detects
///          this via `g_current_worker_pool` and instead:
///            1. Resurrects the pool object so GC doesn't reclaim it.
///            2. Re-installs itself as the finalizer.
///            3. Signals shutdown so workers exit their loop.
///          A later non-worker release will reclaim the pool safely. If native
///          cleanup-thread creation fails, the first worker to exit detaches
///          queued owned arguments and consumes the resurrection reference; its
///          finalizer joins all other workers and deliberately skips self-join.
///
///          **Cycle-reclaim guard.** A pool retained only by one of its queued owned
///          arguments cannot join workers while the collector holds exclusive graph
///          access: a worker may need that same barrier to finish. The finalizer therefore
///          resurrects the pool and schedules the same non-worker cleanup path, which runs
///          after collection resumes mutators and detaches the cyclic queue edge safely.
///
///          **Normal path.** Untracks from GC, signals shutdown, joins every
///          worker, destroys the monitor, and frees the pool struct. Handles
///          the at-exit case where a program terminates without an explicit
///          `ThreadPool.Shutdown` call.
/// @param obj Pool object being finalized.
static void pool_finalizer(void *obj) {
    pool_impl *pool = (pool_impl *)obj;
    if (!pool)
        return;

    int8_t cycle_reclaim = rt_gc_should_suppress_cycle_release(pool);
    if (g_current_worker_pool == pool || cycle_reclaim) {
        /*
         * A worker may be the last owner of a pool through a user callback.
         * Joining/freeing the worker array and monitor from that same worker
         * would free state while it is still executing. Keep the object alive
         * and ask workers to stop; a later non-worker release/finalizer can
         * reclaim the pool safely.
         */
        int8_t start_cleanup = 0;
        rt_obj_resurrect(pool);
        rt_obj_set_finalizer(pool, pool_finalizer);
        if (cycle_reclaim) {
            start_cleanup = pool_cleanup_transition(pool, 0, 1);
        } else if (pool->monitor) {
            rt_monitor_enter(pool->monitor);
            pool_request_shutdown_atomic(pool, 1);
            start_cleanup = pool_cleanup_transition(pool, 0, 1);
            rt_monitor_pause_all(pool->monitor);
            rt_monitor_exit(pool->monitor);
        }
        if (start_cleanup) {
            void *cleanup_thread = NULL;
            jmp_buf recovery;
            rt_trap_set_recovery(&recovery);
            if (setjmp(recovery) != 0) {
                rt_trap_clear_recovery();
                __atomic_store_n(&pool->cleanup_scheduled, 2, __ATOMIC_RELEASE);
                pool_request_shutdown_atomic(pool, 1);
                return;
            }
            cleanup_thread = rt_thread_start_owned_fn(pool_deferred_cleanup_entry, pool);
            rt_trap_clear_recovery();
            if (!cleanup_thread) {
                __atomic_store_n(&pool->cleanup_scheduled, 2, __ATOMIC_RELEASE);
                pool_request_shutdown_atomic(pool, 1);
                return;
            }
            pool_release_thread_handle(&cleanup_thread);
        }
        return;
    }

    rt_gc_untrack(pool);

    /* If not already shut down, signal workers and join them.
       This handles the case where a program exits without calling
       rt_threadpool_shutdown() — the atexit finalizer sweep invokes
       this finalizer to prevent abandoned worker threads. */
    if (pool->monitor) {
        rt_monitor_enter(pool->monitor);
        if (!pool_shutdown_requested(pool))
            pool_request_shutdown_atomic(pool, 1);
        rt_monitor_pause_all(pool->monitor);
        rt_monitor_exit(pool->monitor);
    }

    int64_t exiting_index = -1;
    if (g_exiting_worker && pool->workers) {
        for (int64_t i = 0; i < pool->worker_count; ++i) {
            if (&pool->workers[i] == g_exiting_worker) {
                exiting_index = i;
                break;
            }
        }
    }

    void **handles = pool_detach_worker_handles(pool);
    if (handles) {
        pool_join_worker_handles(handles, pool->worker_count, exiting_index);
        free(handles);
    } else {
        for (int64_t i = 0; i < pool->worker_count; i++) {
            void *thread = NULL;
            if (pool->monitor)
                rt_monitor_enter(pool->monitor);
            if (pool->workers && pool->workers[i].thread) {
                thread = pool->workers[i].thread;
                pool->workers[i].thread = NULL;
            }
            if (pool->monitor)
                rt_monitor_exit(pool->monitor);
            if (thread) {
                if (i != exiting_index)
                    rt_thread_join(thread);
                pool_release_thread_handle(&thread);
            }
        }
    }

    // Detach the remaining queue before releasing owned arguments. This keeps
    // traversal from observing edges whose refcounts are already being dropped.
    rt_gc_mutator_enter();
    pool_task *task = pool->queue_head;
    pool->queue_head = NULL;
    pool->queue_tail = NULL;
    pool->pending_count = 0;
    pool_worker *workers = pool->workers;
    pool->workers = NULL;
    void *monitor = pool->monitor;
    pool->monitor = NULL;
    rt_gc_mutator_exit();
    pool_release_task_list(task);

    // Free workers array
    free(workers);

    // Release monitor
    if (monitor) {
        if (rt_obj_release_check0(monitor))
            rt_obj_free(monitor);
    }
}

/// @brief Create a thread pool with the given number of worker threads (1–1024).
/// @details Workers block on a shared work queue until tasks are submitted via
///          submit(). Tasks are dequeued in FIFO order, though concurrent
///          completion order is unspecified. The Pool installs GC traversal
///          and finalization so explicit shutdown is recommended but not
///          required for eventual worker cleanup.
/// @param size Requested worker count, clamped to the inclusive range 1–1024.
/// @return New caller-owned Pool object, or NULL after a reported construction failure.
void *rt_threadpool_new(int64_t size) {
    // Clamp size to valid range
    if (size < 1)
        size = 1;
    if (size > 1024)
        size = 1024;

    pool_impl *pool = (pool_impl *)rt_obj_new_i64(RT_THREADPOOL_CLASS_ID, sizeof(pool_impl));
    if (!pool)
        return NULL;

    memset(pool, 0, sizeof(*pool));
    rt_obj_set_finalizer(pool, pool_finalizer);

    char constructor_error[512];
    jmp_buf constructor_recovery;
    rt_trap_set_recovery(&constructor_recovery);
    if (setjmp(constructor_recovery) != 0) {
        const char *error = rt_trap_get_error();
        snprintf(constructor_error,
                 sizeof(constructor_error),
                 "%s",
                 error && error[0] ? error : "Pool: construction failed");
        rt_trap_clear_recovery();
        pool_release_object(pool);
        rt_trap(constructor_error);
        return NULL;
    }

    pool->monitor = rt_obj_new_i64(0, 1); // Create a monitor object
    if (!pool->monitor) {
        rt_trap_clear_recovery();
        pool_release_object(pool);
        rt_trap("Pool: monitor allocation failed");
        return NULL;
    }

    pool->queue_head = NULL;
    pool->queue_tail = NULL;
    pool->pending_count = 0;
    pool->active_count = 0;
    pool->max_pending = pool_default_max_pending();
    pool->error_count = 0;
    pool->last_error[0] = '\0';
    __atomic_store_n(&pool->shutdown, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->shutdown_now, 0, __ATOMIC_RELAXED);
    pool->shutdown_joining = 0;
    pool->shutdown_complete = 0;
    __atomic_store_n(&pool->cleanup_scheduled, 0, __ATOMIC_RELAXED);
    pool->worker_count = size;

    // Allocate workers array
    pool->workers = (pool_worker *)calloc((size_t)size, sizeof(pool_worker));
    if (!pool->workers) {
        rt_trap_clear_recovery();
        pool_release_object(pool);
        rt_trap("Pool: worker array allocation failed");
        return NULL;
    }

    // Start worker threads
    for (int64_t i = 0; i < size; i++) {
        pool->workers[i].pool = pool;
        pool->workers[i].thread = rt_thread_start_fn(worker_entry, &pool->workers[i]);
        if (!pool->workers[i].thread) {
            rt_trap_clear_recovery();
            pool_release_object(pool);
            rt_trap("Pool: worker thread creation failed");
            return NULL;
        }
    }

    rt_gc_track(pool, pool_traverse);
    if (!rt_gc_is_tracked(pool)) {
        rt_trap("Pool: GC tracking failed");
        rt_trap_clear_recovery();
        pool_release_object(pool);
        return NULL;
    }
    rt_trap_clear_recovery();

    return pool;
}

//=============================================================================
// Worker Thread
//=============================================================================

/// @brief Main worker-thread loop: dequeue tasks, execute under trap recovery, signal
///        waiters when the pool drains.
/// @details Each iteration acquires the pool monitor, waits on the condition until
///          the queue has work OR shutdown was requested, dequeues one task (if any),
///          releases the monitor, and executes the task with a `setjmp` recovery
///          frame so a trap in user code doesn't unwind through the worker thread.
///          After the task returns (or the trap is caught), the monitor is re-acquired
///          to decrement `active_count`, and if both `pending_count` and
///          `active_count` have reached zero the waiter-set (e.g. `Pool.Wait()`) is
///          woken via `rt_monitor_pause_all`.
///
///          Two shutdown modes:
///            - `shutdown_now`: break out immediately regardless of remaining work.
///            - `shutdown` (graceful): drain the queue first, break when empty.
///
///          `g_current_worker_pool` is set so that user task code can detect
///          whether it's running inside a worker (used to reject re-entrant
///          `Pool.Wait()` calls that would deadlock).
///
///          Trap recovery invariants:
///            - `rt_trap_set_recovery` must pair with `rt_trap_clear_recovery` on
///              every path (happy + trap). Missing the clear would leave a stale
///              `longjmp` target in TLS that a later unrelated trap could jump to.
///            - Task memory is `free`'d only after the recovery clear so a trap
///              inside the task callback doesn't double-free when the next iteration
///              re-enters.
/// @param arg Pointer to the worker record owned by its Pool.
static void worker_entry(void *arg) {
    pool_worker *worker = (pool_worker *)arg;
    pool_impl *pool = worker->pool;
    g_current_worker_pool = pool;

    for (;;) {
        rt_monitor_enter(pool->monitor);

        // Wait for a task or shutdown
        while (pool->queue_head == NULL && !pool_shutdown_requested(pool)) {
            rt_monitor_wait(pool->monitor);
        }

        // Check for immediate shutdown
        if (pool_shutdown_now_requested(pool)) {
            rt_monitor_exit(pool->monitor);
            break;
        }

        // Check for graceful shutdown with empty queue
        if (pool_shutdown_requested(pool) && pool->queue_head == NULL) {
            rt_monitor_exit(pool->monitor);
            break;
        }

        // Dequeue a task. The monitor serializes workers; the graph scope makes
        // the strong owned-argument edge atomic with respect to GC traversal.
        pool_task *task = pool->queue_head;
        if (task) {
            rt_gc_mutator_enter();
#ifdef _MSC_VER
#pragma warning(suppress : 6001)
#endif
            pool->queue_head = task->next;
            if (pool->queue_head == NULL)
                pool->queue_tail = NULL;
            pool->pending_count--;
            pool->active_count++;
        }

        rt_monitor_exit(pool->monitor);
        if (task)
            rt_gc_mutator_exit();

        // Execute the task
        int8_t trapped = 0;
        char task_error[512];
        task_error[0] = '\0';
        if (task && task->callback) {
            jmp_buf recovery;
            rt_trap_set_recovery(&recovery);
            if (setjmp(recovery) == 0) {
                task->callback(task->arg);
            } else {
                const char *msg = rt_trap_get_error();
                if (!msg || !msg[0])
                    msg = "Pool.Wait: task trapped";
                strncpy(task_error, msg, sizeof(task_error) - 1);
                task_error[sizeof(task_error) - 1] = '\0';
                trapped = 1;
            }
            rt_trap_clear_recovery();
            pool_task_release_arg(task);
            free(task);
        }

        // Mark task complete
        rt_monitor_enter(pool->monitor);
        if (trapped) {
            if (pool->error_count < INT64_MAX)
                pool->error_count++;
            strncpy(pool->last_error,
                    task_error[0] ? task_error : "Pool.Wait: task trapped",
                    sizeof(pool->last_error) - 1);
            pool->last_error[sizeof(pool->last_error) - 1] = '\0';
        }
        pool->active_count--;
        // Signal waiters (for Wait() calls)
        if (pool->pending_count == 0 && pool->active_count == 0) {
            rt_monitor_pause_all(pool->monitor);
        }
        rt_monitor_exit(pool->monitor);
    }

    /* If native cleanup-thread creation failed, exactly one exiting worker
       assumes ownership of the resurrection reference. It first removes every
       queued owned edge, then releases the pool from a TLS-marked exit boundary
       so finalization skips only this still-running worker's join. */
    int8_t fallback_owner = pool_cleanup_transition(pool, 2, 3);
    pool_task *fallback_tasks = NULL;
    if (fallback_owner) {
        rt_monitor_enter(pool->monitor);
        pool_request_shutdown_atomic(pool, 1);
        fallback_tasks = pool_detach_tasks_locked(pool);
        rt_monitor_pause_all(pool->monitor);
        rt_monitor_exit(pool->monitor);
        pool_release_task_list(fallback_tasks);
    }

    g_current_worker_pool = NULL;
    if (fallback_owner) {
        g_exiting_worker = worker;
        pool_release_object(pool);
        g_exiting_worker = NULL;
    }
}

/// @brief Detached-thread entry that shuts down and releases a deferred pool.
/// @details Used when a worker or cycle-collection finalizer cannot synchronously join the
///          pool. The helper runs outside both contexts, requests immediate shutdown to detach
///          queued owned arguments, absorbs any already-recorded task trap, and releases the
///          resurrection reference. The thread trampoline drops its separate owned-argument
///          retain after this function returns, allowing normal finalization at refcount zero.
/// @param arg Retained Pool object transferred to the cleanup thread.
static void pool_deferred_cleanup_entry(void *arg) {
    pool_impl *pool = (pool_impl *)arg;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0)
        rt_threadpool_shutdown_now(pool);
    rt_trap_clear_recovery();
    pool_release_object(pool);
}

//=============================================================================
// Public API - Task Submission
//=============================================================================

/// @brief Submit a typed task (callback + arg) for execution on the next available worker thread.
/// @details This internal C-facing entry point preserves the real function
///          pointer type instead of routing through the public `void *`
///          callback ABI. The pool is retained while the task is enqueued and
///          released after queueing succeeds or fails; worker execution owns the
///          queued task record.
/// @param pool_obj Runtime Pool object.
/// @param callback Function invoked by a worker with @p arg.
/// @param arg Opaque argument passed to @p callback.
/// @param owns_arg Whether to retain a managed @p arg until execution or discard.
/// @return 1 when the task is queued, 0 when inputs are invalid, the pool is
///         shutting down, or allocation fails.
static int8_t threadpool_submit_impl(void *pool_obj,
                                     rt_threadpool_task_fn callback,
                                     void *arg,
                                     int8_t owns_arg) {
    if (!pool_obj || !callback)
        return 0;

    pool_impl *pool = pool_require(pool_obj, "Pool.Submit: null object", 0);
    if (!pool)
        return 0;
    rt_obj_retain_maybe(pool_obj);

    /* Allocate and acquire argument ownership before the pool monitor. Heap
       registry contention and user-configured trap dispatch must never extend
       the queue's critical section or strand the monitor on a retain failure. */
    pool_task *task = (pool_task *)calloc(1, sizeof(pool_task));
    if (!task) {
        pool_release_object(pool);
        return 0;
    }
    task->callback = callback;
    task->arg = arg;
    task->owns_arg = 0;
    task->next = NULL;

    if (owns_arg && arg) {
        char retain_error[256];
        jmp_buf retain_recovery;
        rt_trap_set_recovery(&retain_recovery);
        if (setjmp(retain_recovery) != 0) {
            const char *error = rt_trap_get_error();
            snprintf(retain_error,
                     sizeof(retain_error),
                     "%s",
                     error && error[0] ? error : "Pool.SubmitOwned: argument retain failed");
            rt_trap_clear_recovery();
            free(task);
            pool_release_object(pool);
            rt_trap(retain_error);
            return 0;
        }
        rt_obj_retain_maybe(arg);
        task->owns_arg = rt_string_is_handle(arg) || rt_heap_is_payload(arg);
        rt_trap_clear_recovery();
    }

    rt_monitor_enter(pool->monitor);
    if (pool_shutdown_requested(pool)) {
        rt_monitor_exit(pool->monitor);
        pool_task_release_arg(task);
        free(task);
        pool_release_object(pool);
        return 0;
    }

    if (pool->pending_count == INT64_MAX) {
        rt_monitor_exit(pool->monitor);
        pool_task_release_arg(task);
        free(task);
        pool_release_object(pool);
        rt_trap("Pool.Submit: pending task count overflow");
        return 0;
    }
    if (pool->pending_count >= pool->max_pending) {
        rt_monitor_exit(pool->monitor);
        pool_task_release_arg(task);
        free(task);
        pool_release_object(pool);
        return 0;
    }

    // Enqueue task
    rt_gc_mutator_enter();
    if (pool->queue_tail) {
        pool->queue_tail->next = task;
        pool->queue_tail = task;
    } else {
        pool->queue_head = task;
        pool->queue_tail = task;
    }
    pool->pending_count++;

    // Wake one worker
    rt_monitor_pause(pool->monitor);
    rt_gc_mutator_exit();
    rt_monitor_exit(pool->monitor);

    pool_release_object(pool);
    return 1;
}

/// @brief Submit a typed native callback with a borrowed argument.
/// @details The callback and argument must remain valid until execution. The
///          task is rejected without consuming either input when the Pool is
///          shut down, backpressured, invalid, or unable to allocate a node.
/// @param pool_obj Pool that receives the task.
/// @param callback Native worker callback invoked once.
/// @param arg Borrowed callback context.
/// @return One when queued, otherwise zero.
int8_t rt_threadpool_submit_fn(void *pool_obj, rt_threadpool_task_fn callback, void *arg) {
    return threadpool_submit_impl(pool_obj, callback, arg, 0);
}

/// @brief Submit a task whose runtime-managed argument the pool owns
///        (VDOC-128): retained on acceptance, released after the callback
///        runs OR when the task is discarded by ShutdownNow/finalization.
/// @details Rejection leaves the caller's ownership unchanged. A retain trap is
///          propagated after task-node and temporary Pool cleanup.
/// @param pool_obj Pool that receives the task.
/// @param callback Native worker callback invoked once.
/// @param arg Runtime-managed callback context to retain, or NULL.
/// @return One when queued, otherwise zero.
int8_t rt_threadpool_submit_owned_fn(void *pool_obj, rt_threadpool_task_fn callback, void *arg) {
    return threadpool_submit_impl(pool_obj, callback, arg, 1);
}

/// @brief Submit a task through the legacy object-pointer callback API.
/// @details Preserves the historical runtime ABI while routing actual queueing
///          through the typed implementation. New C code should prefer
///          `rt_threadpool_submit_fn` to avoid converting function pointers
///          through `void *`.
/// @param pool_obj Pool that receives the task.
/// @param callback Opaque ABI representation of `void (*)(void *)`.
/// @param arg Borrowed callback context.
/// @return One when queued, otherwise zero.
int8_t rt_threadpool_submit(void *pool_obj, void *callback, void *arg) {
    return rt_threadpool_submit_fn(pool_obj, pool_task_from_opaque(callback), arg);
}

/// @brief Owned-argument variant of @ref rt_threadpool_submit (VDOC-128).
/// @details The opaque callback representation is decoded at the legacy ABI
///          boundary. Accepted managed arguments are retained until execution
///          or discard; rejection leaves ownership unchanged.
/// @param pool_obj Pool that receives the task.
/// @param callback Opaque ABI representation of `void (*)(void *)`.
/// @param arg Runtime-managed callback context to retain, or NULL.
/// @return One when queued, otherwise zero.
int8_t rt_threadpool_submit_owned(void *pool_obj, void *callback, void *arg) {
    return rt_threadpool_submit_owned_fn(pool_obj, pool_task_from_opaque(callback), arg);
}

//=============================================================================
// Public API - Waiting
//=============================================================================

/// @brief Block until the Pool queue is empty and every worker is idle.
/// @details New submissions remain allowed while waiting and can extend the
///          drain interval. A call from one of this Pool's workers traps to
///          avoid self-deadlock. After drain, accumulated worker-trap state is
///          consumed and raised on the waiting thread. NULL is a no-op.
/// @param pool_obj Pool whose current workload should drain.
void rt_threadpool_wait(void *pool_obj) {
    if (!pool_obj)
        return;

    pool_impl *pool = pool_require(pool_obj, "Pool.Wait: null object", 0);
    if (!pool)
        return;
    rt_obj_retain_maybe(pool_obj);
    if (g_current_worker_pool == pool) {
        pool_release_object(pool);
        rt_trap("Pool.Wait: cannot wait from pool worker");
        return;
    }

    rt_monitor_enter(pool->monitor);

    while (pool->pending_count > 0 || pool->active_count > 0) {
        rt_monitor_wait(pool->monitor);
    }

    rt_monitor_exit(pool->monitor);
    char error[512];
    int8_t has_error = pool_take_error(pool, error, sizeof(error));
    pool_release_object(pool);
    if (has_error)
        rt_trap(error[0] ? error : "Pool.Wait: task trapped");
}

/// @brief Wait up to a deadline for the Pool queue and active workers to drain.
/// @details Non-positive durations perform an immediate check. Timeout leaves
///          accumulated worker errors for a later drain observer. A completed
///          wait consumes and raises worker errors; same-Pool worker calls trap.
///          A NULL Pool is treated as already drained.
/// @param pool_obj Pool whose current workload should drain.
/// @param ms Maximum wait in milliseconds.
/// @return One after a clean drain, or zero on timeout, invalid input, or after
///         an error trap returns.
int8_t rt_threadpool_wait_for(void *pool_obj, int64_t ms) {
    if (!pool_obj)
        return 1;

    pool_impl *pool = pool_require(pool_obj, "Pool.WaitFor: null object", 0);
    if (!pool)
        return 0;
    rt_obj_retain_maybe(pool_obj);
    if (g_current_worker_pool == pool) {
        pool_release_object(pool);
        rt_trap("Pool.WaitFor: cannot wait from pool worker");
        return 0;
    }

    if (ms <= 0) {
        // Immediate check
        if (!rt_monitor_try_enter(pool->monitor)) {
            pool_release_object(pool);
            return 0;
        }
        int8_t done = (pool->pending_count == 0 && pool->active_count == 0) ? 1 : 0;
        char error[512];
        error[0] = '\0';
        int8_t has_error = done ? pool_take_error_locked(pool, error, sizeof(error)) : 0;
        rt_monitor_exit(pool->monitor);
        pool_release_object(pool);
        if (has_error) {
            rt_trap(error[0] ? error : "Pool.Wait: task trapped");
            return 0;
        }
        return done;
    }

#if RT_PLATFORM_WINDOWS
    ULONGLONG deadline = rt_win32_deadline_from_now_ms(ms);
#else
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        pool_release_object(pool);
        rt_trap("Pool.WaitFor: monotonic clock failed");
        return 0;
    }
#endif

    if (!rt_monitor_try_enter_for(pool->monitor, ms)) {
        pool_release_object(pool);
        return 0;
    }

    while (pool->pending_count > 0 || pool->active_count > 0) {
#if RT_PLATFORM_WINDOWS
        ULONGLONG now = GetTickCount64();
        ULONGLONG delta = now >= deadline ? 0 : deadline - now;
        int64_t remaining = delta > (ULONGLONG)INT64_MAX ? INT64_MAX : (int64_t)delta;
#else
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            rt_monitor_exit(pool->monitor);
            pool_release_object(pool);
            rt_trap("Pool.WaitFor: monotonic clock failed");
            return 0;
        }
        int64_t sec = (int64_t)now.tv_sec - (int64_t)start.tv_sec;
        int64_t ns = (int64_t)now.tv_nsec - (int64_t)start.tv_nsec;
        if (ns < 0) {
            sec--;
            ns += 1000000000L;
        }
        int64_t elapsed_ms = 0;
        if (sec < 0) {
            elapsed_ms = 0;
        } else if (sec > INT64_MAX / 1000) {
            elapsed_ms = INT64_MAX;
        } else {
            elapsed_ms = sec * 1000;
            int64_t frac_ms = ns / 1000000L;
            if (elapsed_ms <= INT64_MAX - frac_ms)
                elapsed_ms += frac_ms;
            else
                elapsed_ms = INT64_MAX;
        }
        int64_t remaining = elapsed_ms >= ms ? 0 : ms - elapsed_ms;
#endif
        if (remaining <= 0 || !rt_monitor_wait_for(pool->monitor, remaining)) {
            if (pool->pending_count == 0 && pool->active_count == 0)
                break;
            // Timed out
            rt_monitor_exit(pool->monitor);
            pool_release_object(pool);
            return 0;
        }
    }

    rt_monitor_exit(pool->monitor);
    char error[512];
    int8_t has_error = pool_take_error(pool, error, sizeof(error));
    pool_release_object(pool);
    if (has_error) {
        rt_trap(error[0] ? error : "Pool.Wait: task trapped");
        return 0;
    }
    return 1;
}

//=============================================================================
// Public API - Shutdown
//=============================================================================

/// @brief Gracefully stop accepting tasks, drain queued work, and join all workers.
/// @details Concurrent or repeated shutdown calls coordinate through a
///          single-owner join phase and do not double-join handles. The call
///          blocks until stable shutdown completion, then consumes and raises
///          accumulated worker errors. Same-Pool worker calls trap; NULL is a no-op.
/// @param pool_obj Pool to shut down.
void rt_threadpool_shutdown(void *pool_obj) {
    if (!pool_obj)
        return;

    pool_impl *pool = pool_require(pool_obj, "Pool.Shutdown: null object", 0);
    if (!pool)
        return;
    rt_obj_retain_maybe(pool_obj);
    if (g_current_worker_pool == pool) {
        pool_release_object(pool);
        rt_trap("Pool.Shutdown: cannot shutdown from pool worker");
        return;
    }

    int64_t worker_count = pool->worker_count;
    void **handles = (void **)calloc((size_t)worker_count, sizeof(void *));
    if (!handles) {
        pool_release_object(pool);
        rt_trap("Pool.Shutdown: memory allocation failed");
        return;
    }

    rt_monitor_enter(pool->monitor);
    pool_request_shutdown_atomic(pool, 0);
    rt_monitor_pause_all(pool->monitor);
    int8_t join_owner = pool_wait_or_claim_shutdown_locked(pool, handles);
    rt_monitor_exit(pool->monitor);

    char join_error[512];
    join_error[0] = '\0';
    int8_t join_ok =
        !join_owner ||
        pool_finish_shutdown_join(pool, handles, worker_count, join_error, sizeof(join_error));
    free(handles);
    char error[512];
    int8_t has_error = pool_take_error(pool, error, sizeof(error));
    pool_release_object(pool);
    if (!join_ok) {
        rt_trap(join_error[0] ? join_error : "Pool.Shutdown: worker join failed");
        return;
    }
    if (has_error)
        rt_trap(error[0] ? error : "Pool.Wait: task trapped");
}

/// @brief Stop accepting tasks, discard queued work, and join all workers.
/// @details Running callbacks are not interrupted. Detached queued tasks are
///          freed and their Pool-owned arguments released outside the monitor.
///          Concurrent or repeated calls share one join phase. Accumulated
///          worker errors are consumed after joining. Same-Pool worker calls
///          trap; NULL is a no-op.
/// @param pool_obj Pool to shut down immediately.
void rt_threadpool_shutdown_now(void *pool_obj) {
    if (!pool_obj)
        return;

    pool_impl *pool = pool_require(pool_obj, "Pool.ShutdownNow: null object", 0);
    if (!pool)
        return;
    rt_obj_retain_maybe(pool_obj);
    if (g_current_worker_pool == pool) {
        pool_release_object(pool);
        rt_trap("Pool.ShutdownNow: cannot shutdown from pool worker");
        return;
    }

    int64_t worker_count = pool->worker_count;
    void **handles = (void **)calloc((size_t)worker_count, sizeof(void *));
    if (!handles) {
        pool_release_object(pool);
        rt_trap("Pool.ShutdownNow: memory allocation failed");
        return;
    }

    rt_monitor_enter(pool->monitor);
    pool_request_shutdown_atomic(pool, 1);
    // Detach the queue under the graph barrier, then release discarded
    // arguments outside the monitor so user finalizers cannot deadlock it.
    pool_task *task = pool_detach_tasks_locked(pool);

    rt_monitor_pause_all(pool->monitor);
    rt_monitor_exit(pool->monitor);
    pool_release_task_list(task);

    /* Wait only after leaving the shared graph scope. A concurrent collector
       must not be excluded for the potentially unbounded worker-join phase. */
    rt_monitor_enter(pool->monitor);
    int8_t join_owner = pool_wait_or_claim_shutdown_locked(pool, handles);
    rt_monitor_exit(pool->monitor);

    char join_error[512];
    join_error[0] = '\0';
    int8_t join_ok =
        !join_owner ||
        pool_finish_shutdown_join(pool, handles, worker_count, join_error, sizeof(join_error));
    free(handles);
    char error[512];
    int8_t has_error = pool_take_error(pool, error, sizeof(error));
    pool_release_object(pool);
    if (!join_ok) {
        rt_trap(join_error[0] ? join_error : "Pool.ShutdownNow: worker join failed");
        return;
    }
    if (has_error)
        rt_trap(error[0] ? error : "Pool.Wait: task trapped");
}

//=============================================================================
// Public API - Properties
//=============================================================================

/// @brief Get a Pool's fixed worker count.
/// @param pool_obj Pool object to inspect.
/// @return Configured worker count, or zero for NULL or invalid tolerant input.
int64_t rt_threadpool_get_size(void *pool_obj) {
    if (!pool_obj)
        return 0;

    pool_impl *pool = pool_require(pool_obj, "Pool.get_Size: null object", 0);
    if (!pool)
        return 0;
    rt_obj_retain_maybe(pool_obj);
    int64_t size = pool->worker_count;
    pool_release_object(pool);
    return size;
}

/// @brief Get the number of tasks waiting in a Pool's FIFO.
/// @details The result is a monitor-protected point-in-time snapshot and
///          excludes tasks already executing.
/// @param pool_obj Pool object to inspect.
/// @return Pending task count, or zero for NULL or invalid tolerant input.
int64_t rt_threadpool_get_pending(void *pool_obj) {
    if (!pool_obj)
        return 0;

    pool_impl *pool = pool_require(pool_obj, "Pool.get_Pending: null object", 0);
    if (!pool)
        return 0;
    rt_obj_retain_maybe(pool_obj);

    rt_monitor_enter(pool->monitor);
    int64_t count = pool->pending_count;
    rt_monitor_exit(pool->monitor);

    pool_release_object(pool);
    return count;
}

/// @brief Get the number of callbacks currently executing in a Pool.
/// @details The result is a monitor-protected point-in-time snapshot.
/// @param pool_obj Pool object to inspect.
/// @return Active callback count, or zero for NULL or invalid tolerant input.
int64_t rt_threadpool_get_active(void *pool_obj) {
    if (!pool_obj)
        return 0;

    pool_impl *pool = pool_require(pool_obj, "Pool.get_Active: null object", 0);
    if (!pool)
        return 0;
    rt_obj_retain_maybe(pool_obj);

    rt_monitor_enter(pool->monitor);
    int64_t count = pool->active_count;
    rt_monitor_exit(pool->monitor);

    pool_release_object(pool);
    return count;
}

/// @brief Check whether either Pool shutdown mode has been requested.
/// @details This reports request state, which may precede completion of worker
///          draining and joining. NULL or invalid tolerant input reports shut down.
/// @param pool_obj Pool object to inspect.
/// @return One after shutdown is requested or for invalid input, otherwise zero.
int8_t rt_threadpool_get_is_shutdown(void *pool_obj) {
    if (!pool_obj)
        return 1;

    pool_impl *pool = pool_require(pool_obj, "Pool.get_IsShutdown: null object", 0);
    if (!pool)
        return 1;
    rt_obj_retain_maybe(pool_obj);
    rt_monitor_enter(pool->monitor);
    int8_t shutdown = pool_shutdown_requested(pool);
    rt_monitor_exit(pool->monitor);
    pool_release_object(pool);
    return shutdown;
}

/// @brief Return the Pool whose callback is executing on the current thread.
/// @details The pointer is borrowed and valid only while the worker remains in
///          its task loop. Non-worker threads return NULL.
/// @return Borrowed current Pool pointer, or NULL.
void *rt_threadpool_current_worker_pool(void) {
    return g_current_worker_pool;
}
