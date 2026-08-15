//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_parallel.c
// Purpose: Implements the data-parallel combinators on `Zanna.Threads.Parallel`:
//            - ForEach / ForEachPool        — apply a callback to each element.
//            - Map / MapPool                — transform each element into a result Seq.
//            - Reduce / ReducePool          — fold elements into a single accumulator.
//            - For / ForPool                — call a callback for each integer in a range.
//            - Invoke / InvokePool          — fan out a fixed list of nullary callbacks.
//          The non-`Pool` variants run on the shared default pool; the `Pool` variants
//          accept an explicit `Threadpool` handle so callers can isolate workloads.
//
// Key invariants:
//   - All combinators distribute work across N tasks chosen by `parallel_choose_task_count`
//     and wait for every task to finish before returning to the caller.
//   - Element order of processing is non-deterministic; Map / Reduce however preserve
//     positional order in their result Seq / final accumulator.
//   - The first worker callback to trap wins: its message is captured into a per-batch
//     error slot and rethrown on the submitting thread after the batch drains, instead
//     of letting the caller hang or crash mid-batch.
//   - Calls into a `*Pool` variant from a worker already running in the target pool
//     execute serially on the calling worker (`parallel_is_current_pool`) to avoid
//     self-deadlock under exhausted worker pools.
//   - Worker count defaults to the hardware thread count or the supplied pool's size.
//
// Ownership/Lifetime:
//   - Callback function pointers and primitive arguments are borrowed; callers must
//     keep them alive across the call.
//   - `Map` callback results are retained before entering the returned Seq so
//     borrowed input or shared runtime objects remain valid after the caller
//     releases its original references.
//   - `Reduce` callbacks own intermediate accumulator allocation and freeing; the
//     runtime forwards their pointer through the final combine step unchanged.
//   - Submitted callbacks run on a thread pool; no detached per-element threads
//     are created here.
//
// Links: src/runtime/threads/rt_parallel.h (public API),
//        src/runtime/threads/rt_threads.h (OS thread creation and joining),
//        src/runtime/threads/rt_threadpool.h (persistent pool alternative)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements shared infrastructure for Zanna's parallel combinators.
/// @details This translation unit owns default-pool lifecycle, platform batch
///          completion primitives, work partitioning, captured-trap reporting,
///          and result cleanup. Operation-specific ForEach, Map, Reduce, For,
///          and Invoke workers are implemented in `rt_parallel_ops.c`.

#include "rt_parallel.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_parallel_internal.h"
#include "rt_seq.h"
#include "rt_threadpool.h"

#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#include <windows.h>

/// @brief Allocate a Windows interlocked completion counter.
/// @param count Initial number of outstanding tasks, already validated to fit `LONG`.
/// @return Heap-allocated counter, or NULL on allocation failure.
LONG *parallel_win_remaining_new(int64_t count) {
    LONG *remaining = (LONG *)malloc(sizeof(LONG));
    if (!remaining)
        return NULL;
    *remaining = (LONG)count;
    return remaining;
}

/// @brief Record one completed Windows task and signal the final-completion event.
/// @details Failure to signal the last task is fatal because the submitting
///          thread cannot safely release worker-borrowed batch storage.
/// @param remaining Shared interlocked outstanding-task counter.
/// @param event Manual or auto-reset event awaited by the submitting thread.
void parallel_win_complete_one(LONG *remaining, HANDLE event) {
    if (remaining && event && InterlockedDecrement(remaining) == 0 && !SetEvent(event))
        rt_abort("Parallel: completion event signal failed");
}

/// @brief Wait until a Windows parallel batch signals completion.
/// @details A failed wait cannot safely return to the caller: worker tasks still
///          borrow the caller's task array and synchronization state. Abort
///          instead of freeing those objects underneath live workers.
/// @param event Completion event signaled by the last worker.
void parallel_win_wait_for_completion(HANDLE event) {
    if (!event || WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0)
        rt_abort("Parallel: completion event wait failed");
}

/// @brief Retire a Windows batch event without hiding handle corruption or a kernel leak.
/// @details Every caller has either failed before submission or waited for all workers, so no
///   task can legally retain the event. A close failure therefore violates the ownership model
///   and cannot be recovered while continuing safely.
/// @param event Owned completion-event handle.
void parallel_win_close_event(HANDLE event) {
    if (!event || event == INVALID_HANDLE_VALUE || !CloseHandle(event))
        rt_abort("Parallel: completion event close failed");
}
#else
#include <pthread.h>
#if RT_PLATFORM_MACOS
#include <sys/types.h>
/// @brief Query a named macOS kernel setting.
/// @param name NUL-terminated sysctl name.
/// @param oldp Optional destination for the current value.
/// @param oldlenp In/out byte size for @p oldp.
/// @param newp Optional replacement value.
/// @param newlen Byte size of @p newp.
/// @return Zero on success or minus one with `errno` set.
int sysctlbyname(const char *, void *, size_t *, void *, size_t);
#else
#include <unistd.h>
#endif
#endif


//=============================================================================
// Internal Types
//=============================================================================


#if !RT_PLATFORM_WINDOWS

/// @brief Allocate a heap-resident batch synchronization block (POSIX path).
/// @details Used to coordinate completion across N parallel tasks. The block lives on the heap
///          (not on the calling thread's stack) so that an early-return error path on the
///          submitter cannot pull the rug out from workers still racing to decrement
///          `remaining`. Returns NULL on allocation failure; the caller falls back to running
///          the work serially.
/// @param initial Number of task completions the block must observe.
/// @return Initialized heap synchronization block, or NULL on failure.
parallel_sync *parallel_sync_new(int initial) {
    parallel_sync *s = (parallel_sync *)malloc(sizeof(parallel_sync));
    if (!s)
        return NULL;
    s->remaining = initial;
    if (pthread_mutex_init(&s->mutex, NULL) != 0) {
        free(s);
        return NULL;
    }
    if (pthread_cond_init(&s->cond, NULL) != 0) {
        pthread_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }
    return s;
}

/// @brief Block until every task in the batch has called `parallel_sync_complete`, then free.
/// @details Spurious wake-ups are tolerated by the `while (remaining > 0)` loop. After the last
///          task signals, the mutex/cond are destroyed and the heap block freed in one step —
///          callers must not touch the pointer after this returns.
/// @param s Synchronization block to wait on and consume.
void parallel_sync_wait_and_free(parallel_sync *s) {
    pthread_mutex_lock(&s->mutex);
    while (s->remaining > 0)
        pthread_cond_wait(&s->cond, &s->mutex);
    pthread_mutex_unlock(&s->mutex);
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->cond);
    free(s);
}

/// @brief Tear down a sync block on a submission-failure path where no tasks were ever queued.
/// @details Distinct from `_wait_and_free` because there's nothing to wait for — the workers
///          that would have decremented `remaining` never started. Safe on NULL.
/// @param s Unpublished synchronization block to destroy, or NULL.
void parallel_sync_destroy(parallel_sync *s) {
    if (!s)
        return;
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->cond);
    free(s);
}

/// @brief Mark one task complete and wake the waiter once `remaining` hits zero.
/// @details Called once per submitted task after its work runs (success or trap). The
///          decrement and the conditional signal are both performed under the mutex, so the
///          waiter can never miss the final wake-up.
/// @param s Shared synchronization block for the running batch.
void parallel_sync_complete(parallel_sync *s) {
    pthread_mutex_lock(&s->mutex);
    s->remaining--;
    if (s->remaining == 0)
        pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
}
#endif

//=============================================================================
// Default Pool (singleton)
//=============================================================================

static void *g_default_pool = NULL;
#if RT_PLATFORM_WINDOWS
static INIT_ONCE g_pool_lock_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_pool_lock;

/// @brief One-shot initializer for the Windows default-pool critical section.
/// @details Invoked at most once per process by `InitOnceExecuteOnce`. Required because Win32
///          `CRITICAL_SECTION` has no static initializer (unlike `pthread_mutex_t`'s
///          `PTHREAD_MUTEX_INITIALIZER`), so we can't construct `g_pool_lock` at load time.
/// @param InitOnce Win32 once-control passed by `InitOnceExecuteOnce`.
/// @param Param Optional caller parameter; unused.
/// @param Ctx Optional context output; unused.
/// @return `TRUE` after initialization, or `FALSE` when native allocation fails.
static BOOL CALLBACK pool_lock_init_callback(PINIT_ONCE InitOnce, PVOID Param, PVOID *Ctx) {
    (void)InitOnce;
    (void)Param;
    (void)Ctx;
    return InitializeCriticalSectionEx(&g_pool_lock, 0, 0);
}
#else
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

/// @brief Get the default number of worker threads (= number of CPU cores).
/// @details Uses the platform's online-processor query and falls back to four
///          workers when detection returns a non-positive value.
/// @return Positive default worker count.
int64_t rt_parallel_default_workers(void) {
#if RT_PLATFORM_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int64_t count = si.dwNumberOfProcessors;
#elif RT_PLATFORM_MACOS
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0);
    int64_t count = ncpu;
#else
    int64_t count = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return count > 0 ? count : 4;
}

/// @brief Get or lazily create the shared default thread pool.
/// @details Pool creation and replacement of a shut-down singleton are
///          serialized. The singleton keeps its own reference and each caller
///          receives an additional reference that must be released.
/// @return Retained default Threadpool object, or NULL if pool creation fails.
void *rt_parallel_default_pool(void) {
#if RT_PLATFORM_WINDOWS
    if (!InitOnceExecuteOnce(&g_pool_lock_once, pool_lock_init_callback, NULL, NULL))
        rt_abort("Parallel: default-pool lock initialization failed");
    EnterCriticalSection(&g_pool_lock);
#else
    pthread_mutex_lock(&g_pool_lock);
#endif

    if (!g_default_pool) {
        g_default_pool = rt_threadpool_new(rt_parallel_default_workers());
        if (g_default_pool)
            rt_obj_retain_maybe(g_default_pool); // Shared singleton reference.
    } else if (rt_threadpool_get_is_shutdown(g_default_pool)) {
        void *old_pool = g_default_pool;
        g_default_pool = rt_threadpool_new(rt_parallel_default_workers());
        if (g_default_pool)
            rt_obj_retain_maybe(g_default_pool);
        if (rt_obj_release_check0(old_pool))
            rt_obj_free(old_pool);
    }

    void *result = g_default_pool;
    if (result)
        rt_obj_retain_maybe(result);

#if RT_PLATFORM_WINDOWS
    LeaveCriticalSection(&g_pool_lock);
#else
    pthread_mutex_unlock(&g_pool_lock);
#endif

    return result;
}

/// @brief Drop the per-call retain on the default pool when the caller did not supply one.
/// @details `rt_parallel_default_pool()` returns a retained handle so the singleton can survive
///          the call. If the caller passed their own pool (`requested_pool != NULL`), we never
///          touched the singleton and there's nothing to release. If the caller relied on the
///          default (we did the lookup on their behalf), release the temporary retain — and
///          free the singleton if this happened to be the last reference (e.g. shutdown path).
/// @param requested_pool Pool originally supplied by the combinator caller, or NULL.
/// @param actual_pool Effective pool returned by default lookup or supplied directly.
void parallel_release_default_pool(void *requested_pool, void *actual_pool) {
    if (requested_pool || !actual_pool)
        return;
    if (rt_obj_release_check0(actual_pool))
        rt_obj_free(actual_pool);
}

/// @brief Shut down and drain the shared default thread pool, joining its worker threads.
/// @details See the header for the rationale (per-run context teardown must join the pool's
///          workers before the context they bound is freed). The slot is read under the pool
///          lock and retained, then shut down *outside* the lock — `rt_threadpool_shutdown`
///          blocks on thread joins and a worker's exit path may take other runtime locks, so
///          holding `g_pool_lock` across it risks a deadlock. Worker handles are taken (and
///          nulled) exactly once inside shutdown, so the later atexit finalizer sweep will not
///          double-join. `g_default_pool` is intentionally left pointing at the now-shutdown
///          pool; the next `rt_parallel_default_pool()` recreates it via its is_shutdown branch.
void rt_parallel_shutdown_default_pool(void) {
#if RT_PLATFORM_WINDOWS
    if (!InitOnceExecuteOnce(&g_pool_lock_once, pool_lock_init_callback, NULL, NULL))
        rt_abort("Parallel: default-pool lock initialization failed");
    EnterCriticalSection(&g_pool_lock);
#else
    pthread_mutex_lock(&g_pool_lock);
#endif

    void *pool = g_default_pool;
    if (pool)
        rt_obj_retain_maybe(pool); // Keep alive across the unlocked shutdown/join.

#if RT_PLATFORM_WINDOWS
    LeaveCriticalSection(&g_pool_lock);
#else
    pthread_mutex_unlock(&g_pool_lock);
#endif

    if (!pool)
        return;

    rt_threadpool_shutdown(pool); // Joins workers; each unbinds its inherited context here.
    if (rt_obj_release_check0(pool))
        rt_obj_free(pool);
}

/// @brief Resolve the effective worker count for a parallel-for invocation.
/// @details Three-step fallback chain so we always return a usable positive
///          integer regardless of caller inputs:
///            1. If a `pool` is provided, ask it for its configured size.
///            2. If that returned zero or negative (no pool, or a mis-configured
///               pool), fall back to `rt_parallel_default_workers()` which
///               derives a platform-appropriate default (typically CPU count).
///            3. If even that fails (returning zero on a platform where we
///               can't query CPU count), return `1` so work still runs
///               serially instead of being silently skipped.
///          Used by `parallel_choose_task_count` to size the per-iteration
///          batch split.
/// @param pool Optional Threadpool whose configured size should be preferred.
/// @return Positive effective worker count.
int64_t parallel_pool_size(void *pool) {
    int64_t size = pool ? rt_threadpool_get_size(pool) : 0;
    if (size <= 0)
        size = rt_parallel_default_workers();
    return size > 0 ? size : 1;
}

/// @brief Multiply two non-negative int64s, saturating at INT64_MAX on
///        overflow; returns 0 if either operand is <= 0. Used to size work
///        partitions without UB on extreme range/chunk counts.
/// @param lhs Left multiplicand.
/// @param rhs Right multiplicand.
/// @return Zero for non-positive input, the exact product when representable,
///         or `INT64_MAX` on overflow.
static int64_t parallel_saturating_mul_i64(int64_t lhs, int64_t rhs) {
    if (lhs <= 0 || rhs <= 0)
        return 0;
    if (lhs > INT64_MAX / rhs)
        return INT64_MAX;
    return lhs * rhs;
}

/// @brief Decide how many tasks to split a `count`-element workload into.
/// @details Heuristic with three regimes:
///            - **Tiny / small (`count <= 8 * workers`):** one task per element so we don't
///              under-utilise the pool when there's little to do.
///            - **Medium / large:** target `4 * workers` tasks for good load balancing without
///              excessive scheduling overhead, then floor the chunk size at `min_chunk = 16`
///              elements so we don't pay queue cost per single iteration on huge ranges.
///            - **Degenerate (`count <= 1`):** return `count` directly so callers can skip the
///              parallel machinery entirely.
///          The `workers * 4` target gives ~4× over-subscription, which empirically smooths
///          tail latency when individual tasks have variable cost.
/// @param pool Optional effective Threadpool used to determine worker capacity.
/// @param count Number of work items to partition.
/// @return Chosen task count, equal to @p count for degenerate counts at most one.
int64_t parallel_choose_task_count(void *pool, int64_t count) {
    if (count <= 1)
        return count;

    int64_t workers = parallel_pool_size(pool);
    if (count <= parallel_saturating_mul_i64(workers, 8))
        return count;

    const int64_t min_chunk = 16;
    int64_t task_count = parallel_saturating_mul_i64(workers, 4);
    if (task_count < 1)
        task_count = 1;
    if (task_count > count)
        task_count = count;

    int64_t capped_by_chunk = count / min_chunk + (count % min_chunk != 0 ? 1 : 0);
    if (capped_by_chunk < 1)
        capped_by_chunk = 1;
    if (task_count > capped_by_chunk)
        task_count = capped_by_chunk;
    return task_count > 0 ? task_count : 1;
}

/// @brief Compute the [start, end) sub-range owned by a given task in an even partition.
/// @details Spreads a `remainder` of `count % task_count` extra elements across the first
///          `remainder` tasks (one extra each), so total task sizes differ by at most one.
///          Used by ForEach/Map/Reduce/For to slice the work without overlap or gaps.
///          `start_out`/`end_out` are written only when non-NULL.
/// @param count Total number of items in the logical range.
/// @param task_count Positive number of partitions.
/// @param task_index Zero-based partition index.
/// @param start_out Optional destination for the inclusive start index.
/// @param end_out Optional destination for the exclusive end index.
void parallel_split_range(
    int64_t count, int64_t task_count, int64_t task_index, int64_t *start_out, int64_t *end_out) {
    int64_t base = count / task_count;
    int64_t remainder = count % task_count;
    int64_t start = task_index * base + (task_index < remainder ? task_index : remainder);
    int64_t size = base + (task_index < remainder ? 1 : 0);
    if (start_out)
        *start_out = start;
    if (end_out)
        *end_out = start + size;
}

/// @brief Capture the current trap message into a per-batch error buffer if it's still empty.
/// @details Implements the "first trap wins" semantics shared by every parallel callback. The
///          `dst[0] != '\0'` guard means later traps are silently dropped — only the earliest
///          captured failure surfaces to the caller. Falls back to `fallback` (or a generic
///          "Parallel: task trapped") when `rt_trap_get_error` returned nothing useful.
/// @param dst Shared or task-local diagnostic buffer.
/// @param dst_size Capacity of @p dst in bytes.
/// @param fallback Optional diagnostic used when the recovered trap has no message.
void parallel_copy_error(char *dst, size_t dst_size, const char *fallback) {
    if (!dst || dst_size == 0 || dst[0] != '\0')
        return;
    const char *msg = rt_trap_get_error();
    if (!msg || !msg[0])
        msg = fallback ? fallback : "Parallel: task trapped";
    strncpy(dst, msg, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/// @brief Re-raise the captured failure on the submitting thread after the batch drains.
/// @details Workers cannot directly trap into the caller's setjmp recovery, so they store the
///          message into a shared buffer and the submitter calls this once all tasks have
///          completed. Picks the captured message if present, otherwise the generic fallback.
/// @param fallback Diagnostic used when no captured message is available.
/// @param captured Captured worker diagnostic buffer, or NULL.
void parallel_trap_error(const char *fallback, const char *captured) {
    rt_trap((captured && captured[0]) ? captured : fallback);
}

/// @brief True if a @p count-element array of @p elem_size bytes can be
///        allocated without size_t overflow (rejects negative counts).
/// @param count Requested element count.
/// @param elem_size Size of each element in bytes.
/// @return Non-zero when the allocation size is valid and representable.
int parallel_count_fits_array(int64_t count, size_t elem_size) {
    return count >= 0 && elem_size > 0 && (uint64_t)count <= (uint64_t)SIZE_MAX / elem_size;
}

/// @brief True if @p count fits a non-negative int wait-counter (the
///        cross-thread completion counter is an int; reject overflow).
/// @param count Requested number of task completions.
/// @return Non-zero when @p count is in `[0, INT_MAX]`.
int parallel_count_fits_wait_counter(int64_t count) {
    return count >= 0 && count <= INT_MAX;
}

/// @brief Detect whether the calling thread is already a worker of `pool`.
/// @details Used to short-circuit nested parallel calls — recursing into the same pool from
///          inside one of its worker tasks would deadlock if the pool is saturated, so the
///          caller falls back to running the work serially on the current thread.
/// @param pool Candidate Threadpool object.
/// @return Non-zero when the current worker is executing for @p pool.
int parallel_is_current_pool(void *pool) {
    return pool && rt_threadpool_current_worker_pool() == pool;
}

/// @brief Drop retained values stored in a Map result array on the failure path.
/// @details Map treats callback returns as borrowed and retains every runtime
///          object before storing it in the result array, so cleanup can release
///          every populated slot uniformly.
/// @param results Result-pointer array whose populated entries are released and cleared.
/// @param items Legacy input-array parameter; unused by uniform retained-result cleanup.
/// @param count Number of result slots to inspect.
void parallel_release_map_results(void **results, void **items, int64_t count) {
    (void)items;
    if (!results)
        return;
    for (int64_t i = 0; i < count; i++) {
        void *value = results[i];
        results[i] = NULL;
        if (value) {
            if (rt_obj_release_check0(value))
                rt_obj_free(value);
        }
    }
}
