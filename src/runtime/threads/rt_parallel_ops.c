//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_parallel_ops.c
// Purpose: Data-parallel combinators — ForEach, Map, Invoke, Reduce, and For
//          (plus their *Pool variants). Split out of rt_parallel.c; uses the
//          shared task-context types and pool/sync helpers from
//          rt_parallel_internal.h.
//
// Key invariants:
//   - Each combinator splits work into parallel_choose_task_count tasks and
//     blocks until every task drains; processing order is non-deterministic.
//   - Map/Reduce preserve positional order in their result Seq / accumulator.
//   - The first worker to trap wins; its message is rethrown on the caller
//     thread after the batch completes.
//
// Ownership/Lifetime:
//   - Callback pointers and arguments are borrowed; Map retains result objects
//     before placing them in the returned Seq.
//
// Links: src/runtime/threads/rt_parallel.c (pool + sync + callbacks),
//        src/runtime/threads/rt_parallel_internal.h (shared types/helpers)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements ForEach, Map, Invoke, Reduce, and integer For operations.
/// @details Each public operation partitions work, submits callback records to
///          a Threadpool, waits for every submitted record to finish, and then
///          reports the first worker failure on the calling thread. Calls made
///          from a worker of the selected pool execute serially to avoid
///          saturated-pool self-deadlock.

#include "rt_parallel.h"
#include "rt_parallel_internal.h"

#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_threadpool.h"

#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Execute one ForEach task slice and publish its completion.
/// @param arg Pointer to a borrowed `foreach_task` record.
static void foreach_callback(void *arg);

/// @brief Execute one Map task slice and publish its completion.
/// @param arg Pointer to a borrowed `map_task` record.
static void map_callback(void *arg);

/// @brief Execute one Invoke callback and publish its completion.
/// @param arg Pointer to a borrowed `invoke_task` record.
static void invoke_callback(void *arg);

/// @brief Fold one Reduce slice and publish its partial result and completion.
/// @param arg Pointer to a borrowed `reduce_task` record.
static void reduce_callback(void *arg);

/// @brief Execute one integer For subrange and publish its completion.
/// @param arg Pointer to a borrowed `for_task` record.
static void for_callback(void *arg);

//=============================================================================
// Parallel ForEach
//=============================================================================

/// @brief Apply a callback to each sequence element using an optional Threadpool.
/// @details The input is copied into a borrowed pointer array, balanced task
///          slices are submitted, and the call waits for the entire batch.
///          Processing order is unspecified. Nested calls from a worker of the
///          same pool execute serially. The first callback trap is re-raised
///          after completion; submission or allocation failures also trap.
/// @param seq Sequence whose elements are passed to the callback.
/// @param func Borrowed callback with signature `void (*)(void *)`.
/// @param pool Borrowed Threadpool, or NULL to use the shared default.
void rt_parallel_foreach_pool(void *seq, void *func, void *pool) {
    if (!seq || !func)
        return;

    int64_t count = rt_seq_len(seq);
    if (count < 0) {
        rt_trap("Parallel.ForEach: negative sequence length");
        return;
    }
    if (count == 0)
        return;

    void *actual_pool = pool ? pool : rt_parallel_default_pool();
    int64_t task_count = parallel_choose_task_count(actual_pool, count);
    if (parallel_is_current_pool(actual_pool)) {
        char task_error[512];
        task_error[0] = '\0';
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (RT_SETJMP(recovery) == 0) {
            void (*worker)(void *) = RT_FN_PTR_CAST((void (*)(void *))func);
            for (int64_t i = 0; i < count; i++)
                worker(rt_seq_get(seq, i));
        } else {
            parallel_copy_error(task_error, sizeof(task_error), "Parallel.ForEach: task trapped");
        }
        rt_trap_clear_recovery();
        parallel_release_default_pool(pool, actual_pool);
        if (task_error[0])
            parallel_trap_error("Parallel.ForEach: task trapped", task_error);
        return;
    }

    if (!parallel_count_fits_array(count, sizeof(void *))) {
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.ForEach: allocation size overflow");
        return;
    }
    void **items = (void **)malloc((size_t)count * sizeof(void *));
    if (!items) {
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.ForEach: memory allocation failed");
        return;
    }
    for (int64_t i = 0; i < count; i++)
        items[i] = rt_seq_get(seq, i);

    if (!parallel_count_fits_wait_counter(task_count)) {
        free(items);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.ForEach: allocation size overflow");
        return;
    }

#if RT_PLATFORM_WINDOWS
    LONG *remaining = parallel_win_remaining_new(task_count);
    if (!remaining) {
        free(items);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.ForEach: memory allocation failed");
        return;
    }
    LONG task_failed = 0;
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!event) {
        free(remaining);
        free(items);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.ForEach: event creation failed");
        return;
    }
    CRITICAL_SECTION error_lock;
    if (!InitializeCriticalSectionEx(&error_lock, 0, 0)) {
        parallel_win_close_event(event);
        free(remaining);
        free(items);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.ForEach: error mutex initialization failed");
        return;
    }
#else
    int task_failed = 0;
    parallel_sync *sync = parallel_sync_new((int)task_count);
    if (!sync) {
        free(items);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.ForEach: memory allocation failed");
        return;
    }
#endif
    char task_error[512];
    task_error[0] = '\0';

    // Allocate task array
    if (!parallel_count_fits_array(task_count, sizeof(foreach_task))) {
#if RT_PLATFORM_WINDOWS
        parallel_win_close_event(event);
        DeleteCriticalSection(&error_lock);
        free(remaining);
#else
        parallel_sync_destroy(sync);
#endif
        free(items);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.ForEach: allocation size overflow");
        return;
    }
    foreach_task *tasks = (foreach_task *)malloc((size_t)task_count * sizeof(foreach_task));
    if (!tasks) {
#if RT_PLATFORM_WINDOWS
        parallel_win_close_event(event);
        DeleteCriticalSection(&error_lock);
        free(remaining);
#else
        parallel_sync_destroy(sync);
#endif
        free(items);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.ForEach: memory allocation failed");
        return;
    }

    int submit_failed = 0;

    // Submit all tasks
    for (int64_t i = 0; i < task_count; i++) {
        tasks[i].items = items;
        parallel_split_range(count, task_count, i, &tasks[i].start, &tasks[i].end);
        tasks[i].func = RT_FN_PTR_CAST((void (*)(void *))func);
#if RT_PLATFORM_WINDOWS
        tasks[i].remaining = remaining;
        tasks[i].failed = &task_failed;
        tasks[i].event = event;
        tasks[i].error_lock = &error_lock;
#else
        tasks[i].remaining = &sync->remaining;
        tasks[i].failed = &task_failed;
        tasks[i].mutex = &sync->mutex;
        tasks[i].cond = &sync->cond;
#endif
        tasks[i].error = task_error;
        tasks[i].error_size = sizeof(task_error);
        if (!rt_threadpool_submit(actual_pool, fnptr_to_voidptr(foreach_callback), &tasks[i])) {
            submit_failed = 1;
#if RT_PLATFORM_WINDOWS
            parallel_win_complete_one(remaining, event);
#else
            parallel_sync_complete(sync);
#endif
        }
    }

    // Wait for completion
#if RT_PLATFORM_WINDOWS
    parallel_win_wait_for_completion(event);
    parallel_win_close_event(event);
    DeleteCriticalSection(&error_lock);
    free(remaining);
#else
    parallel_sync_wait_and_free(sync);
#endif

    free(tasks);
    free(items);

    parallel_release_default_pool(pool, actual_pool);
    if (task_failed)
        parallel_trap_error("Parallel.ForEach: task trapped", task_error);
    if (submit_failed)
        rt_trap("Parallel.ForEach: failed to submit work");
}

/// @brief Apply a callback to every sequence element through the default pool.
/// @param seq Sequence whose elements are passed to the callback.
/// @param func Borrowed callback with signature `void (*)(void *)`.
void rt_parallel_foreach(void *seq, void *func) {
    rt_parallel_foreach_pool(seq, func, NULL);
}

//=============================================================================
// Parallel Map
//=============================================================================

/// @brief Transform each sequence element through an optional Threadpool.
/// @details Callback execution may be unordered, but results are stored at
///          their input indices and collected in positional order. Each
///          runtime-managed result is retained for the caller-owned output
///          sequence. Partial results are released if a task traps or
///          submission fails. Nested calls on the same pool run serially.
/// @param seq Sequence whose elements are transformed.
/// @param func Borrowed mapper with signature `void *(*)(void *)`.
/// @param pool Borrowed Threadpool, or NULL to use the shared default.
/// @return Caller-owned result sequence, an empty sequence for invalid or empty
///         input where construction succeeds, or NULL after a reported failure.
void *rt_parallel_map_pool(void *seq, void *func, void *pool) {
    if (!seq || !func)
        return rt_seq_new();

    int64_t count = rt_seq_len(seq);
    if (count < 0) {
        rt_trap("Parallel.Map: negative sequence length");
        return rt_seq_new();
    }
    if (count == 0)
        return rt_seq_new();

    void *actual_pool = pool ? pool : rt_parallel_default_pool();
    int64_t task_count = parallel_choose_task_count(actual_pool, count);
    if (parallel_is_current_pool(actual_pool)) {
        void *result = rt_seq_new();
        if (!result) {
            parallel_release_default_pool(pool, actual_pool);
            return NULL;
        }
        rt_seq_set_owns_elements(result, 1);
        char task_error[512];
        task_error[0] = '\0';
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (RT_SETJMP(recovery) == 0) {
            void *(*mapper)(void *) = RT_FN_PTR_CAST((void *(*)(void *))func);
            for (int64_t i = 0; i < count; i++) {
                void *mapped = mapper(rt_seq_get(seq, i));
                rt_seq_push(result, mapped);
            }
        } else {
            parallel_copy_error(task_error, sizeof(task_error), "Parallel.Map: task trapped");
        }
        rt_trap_clear_recovery();
        parallel_release_default_pool(pool, actual_pool);
        if (task_error[0]) {
            if (rt_obj_release_check0(result))
                rt_obj_free(result);
            parallel_trap_error("Parallel.Map: task trapped", task_error);
            return NULL;
        }
        return result;
    }

    if (!parallel_count_fits_array(count, sizeof(void *))) {
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Map: allocation size overflow");
        return NULL;
    }
    void **items = (void **)malloc((size_t)count * sizeof(void *));
    void **results = (void **)calloc((size_t)count, sizeof(void *));
    if (!items || !results) {
        free(items);
        free(results);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Map: memory allocation failed");
        return NULL;
    }
    for (int64_t i = 0; i < count; i++)
        items[i] = rt_seq_get(seq, i);

    if (!parallel_count_fits_wait_counter(task_count)) {
        free(items);
        free(results);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Map: allocation size overflow");
        return NULL;
    }

#if RT_PLATFORM_WINDOWS
    LONG *remaining = parallel_win_remaining_new(task_count);
    if (!remaining) {
        free(items);
        free(results);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Map: memory allocation failed");
        return NULL;
    }
    LONG task_failed = 0;
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!event) {
        free(remaining);
        free(items);
        free(results);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Map: event creation failed");
        return NULL;
    }
    CRITICAL_SECTION error_lock;
    if (!InitializeCriticalSectionEx(&error_lock, 0, 0)) {
        parallel_win_close_event(event);
        free(remaining);
        free(items);
        free(results);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Map: error mutex initialization failed");
        return NULL;
    }
#else
    int task_failed = 0;
    parallel_sync *sync = parallel_sync_new((int)task_count);
    if (!sync) {
        free(items);
        free(results);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Map: memory allocation failed");
        return NULL;
    }
#endif
    char task_error[512];
    task_error[0] = '\0';

    // Allocate task array
    if (!parallel_count_fits_array(task_count, sizeof(map_task))) {
#if RT_PLATFORM_WINDOWS
        parallel_win_close_event(event);
        DeleteCriticalSection(&error_lock);
        free(remaining);
#else
        parallel_sync_destroy(sync);
#endif
        free(items);
        free(results);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Map: allocation size overflow");
        return NULL;
    }
    map_task *tasks = (map_task *)malloc((size_t)task_count * sizeof(map_task));
    if (!tasks) {
#if RT_PLATFORM_WINDOWS
        parallel_win_close_event(event);
        DeleteCriticalSection(&error_lock);
        free(remaining);
#else
        parallel_sync_destroy(sync);
#endif
        free(items);
        free(results);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Map: memory allocation failed");
        return NULL;
    }

    int submit_failed = 0;

    // Submit all tasks
    for (int64_t i = 0; i < task_count; i++) {
        tasks[i].items = items;
        tasks[i].results = results;
        tasks[i].func = RT_FN_PTR_CAST((void *(*)(void *))func);
        parallel_split_range(count, task_count, i, &tasks[i].start, &tasks[i].end);
#if RT_PLATFORM_WINDOWS
        tasks[i].remaining = remaining;
        tasks[i].failed = &task_failed;
        tasks[i].event = event;
        tasks[i].error_lock = &error_lock;
#else
        tasks[i].remaining = &sync->remaining;
        tasks[i].failed = &task_failed;
        tasks[i].mutex = &sync->mutex;
        tasks[i].cond = &sync->cond;
#endif
        tasks[i].error = task_error;
        tasks[i].error_size = sizeof(task_error);
        if (!rt_threadpool_submit(actual_pool, fnptr_to_voidptr(map_callback), &tasks[i])) {
            submit_failed = 1;
#if RT_PLATFORM_WINDOWS
            parallel_win_complete_one(remaining, event);
#else
            parallel_sync_complete(sync);
#endif
        }
    }

    // Wait for completion
#if RT_PLATFORM_WINDOWS
    parallel_win_wait_for_completion(event);
    parallel_win_close_event(event);
    DeleteCriticalSection(&error_lock);
    free(remaining);
#else
    parallel_sync_wait_and_free(sync);
#endif

    if (task_failed || submit_failed) {
        parallel_release_map_results(results, items, count);
        free(tasks);
        free(items);
        free(results);
        parallel_release_default_pool(pool, actual_pool);
        if (task_failed)
            parallel_trap_error("Parallel.Map: task trapped", task_error);
        rt_trap("Parallel.Map: failed to submit work");
        return NULL;
    }

    // Collect results in order
    void *result = rt_seq_new();
    if (!result) {
        parallel_release_map_results(results, items, count);
        free(tasks);
        free(items);
        free(results);
        parallel_release_default_pool(pool, actual_pool);
        return NULL;
    }
    rt_seq_set_owns_elements(result, 1);
    for (int64_t i = 0; i < count; i++) {
        rt_seq_push_raw(result, results[i]);
        results[i] = NULL;
    }

    free(tasks);
    free(items);
    free(results);
    parallel_release_default_pool(pool, actual_pool);
    return result;
}

/// @brief Transform each sequence element through the default pool.
/// @param seq Sequence whose elements are transformed.
/// @param func Borrowed mapper with signature `void *(*)(void *)`.
/// @return Caller-owned ordered result sequence, or NULL after a reported failure.
void *rt_parallel_map(void *seq, void *func) {
    return rt_parallel_map_pool(seq, func, NULL);
}

//=============================================================================
// Parallel Invoke
//=============================================================================

/// @brief Invoke a sequence of nullary callbacks through an optional Threadpool.
/// @details One task is submitted per callback and the function waits for all
///          tasks, including those preceding a submission failure. Nested calls
///          on the same pool run serially. The first callback trap is re-raised
///          after the batch drains.
/// @param funcs Sequence of borrowed callbacks with signature `void (*)(void)`.
/// @param pool Borrowed Threadpool, or NULL to use the shared default.
void rt_parallel_invoke_pool(void *funcs, void *pool) {
    if (!funcs)
        return;

    int64_t count = rt_seq_len(funcs);
    if (count < 0) {
        rt_trap("Parallel.Invoke: negative sequence length");
        return;
    }
    if (count == 0)
        return;

    void *actual_pool = pool ? pool : rt_parallel_default_pool();
    if (parallel_is_current_pool(actual_pool)) {
        char task_error[512];
        task_error[0] = '\0';
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (RT_SETJMP(recovery) == 0) {
            for (int64_t i = 0; i < count; i++) {
                void (*worker)(void) = RT_FN_PTR_CAST((void (*)(void))rt_seq_get(funcs, i));
                worker();
            }
        } else {
            parallel_copy_error(task_error, sizeof(task_error), "Parallel.Invoke: task trapped");
        }
        rt_trap_clear_recovery();
        parallel_release_default_pool(pool, actual_pool);
        if (task_error[0])
            parallel_trap_error("Parallel.Invoke: task trapped", task_error);
        return;
    }

    if (!parallel_count_fits_wait_counter(count)) {
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Invoke: allocation size overflow");
        return;
    }

#if RT_PLATFORM_WINDOWS
    LONG *remaining = parallel_win_remaining_new(count);
    if (!remaining) {
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Invoke: memory allocation failed");
        return;
    }
    LONG task_failed = 0;
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!event) {
        free(remaining);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Invoke: event creation failed");
        return;
    }
    CRITICAL_SECTION error_lock;
    if (!InitializeCriticalSectionEx(&error_lock, 0, 0)) {
        parallel_win_close_event(event);
        free(remaining);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Invoke: error mutex initialization failed");
        return;
    }
#else
    int task_failed = 0;
    parallel_sync *sync = parallel_sync_new((int)count);
    if (!sync) {
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Invoke: memory allocation failed");
        return;
    }
#endif
    char task_error[512];
    task_error[0] = '\0';

    // Allocate task array
    if (!parallel_count_fits_array(count, sizeof(invoke_task))) {
#if RT_PLATFORM_WINDOWS
        parallel_win_close_event(event);
        DeleteCriticalSection(&error_lock);
        free(remaining);
#else
        parallel_sync_destroy(sync);
#endif
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Invoke: allocation size overflow");
        return;
    }
    invoke_task *tasks = (invoke_task *)malloc((size_t)count * sizeof(invoke_task));
    if (!tasks) {
#if RT_PLATFORM_WINDOWS
        parallel_win_close_event(event);
        DeleteCriticalSection(&error_lock);
        free(remaining);
#else
        parallel_sync_destroy(sync);
#endif
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Invoke: memory allocation failed");
        return;
    }

    int submit_failed = 0;

    // Submit all tasks
    for (int64_t i = 0; i < count; i++) {
        tasks[i].func = RT_FN_PTR_CAST((void (*)(void))rt_seq_get(funcs, i));
#if RT_PLATFORM_WINDOWS
        tasks[i].remaining = remaining;
        tasks[i].failed = &task_failed;
        tasks[i].event = event;
        tasks[i].error_lock = &error_lock;
#else
        tasks[i].remaining = &sync->remaining;
        tasks[i].failed = &task_failed;
        tasks[i].mutex = &sync->mutex;
        tasks[i].cond = &sync->cond;
#endif
        tasks[i].error = task_error;
        tasks[i].error_size = sizeof(task_error);
        if (!rt_threadpool_submit(actual_pool, fnptr_to_voidptr(invoke_callback), &tasks[i])) {
            submit_failed = 1;
#if RT_PLATFORM_WINDOWS
            parallel_win_complete_one(remaining, event);
#else
            parallel_sync_complete(sync);
#endif
        }
    }

    // Wait for completion
#if RT_PLATFORM_WINDOWS
    parallel_win_wait_for_completion(event);
    parallel_win_close_event(event);
    DeleteCriticalSection(&error_lock);
    free(remaining);
#else
    parallel_sync_wait_and_free(sync);
#endif

    free(tasks);

    parallel_release_default_pool(pool, actual_pool);
    if (task_failed)
        parallel_trap_error("Parallel.Invoke: task trapped", task_error);
    if (submit_failed)
        rt_trap("Parallel.Invoke: failed to submit work");
}

/// @brief Invoke a sequence of nullary callbacks through the default pool.
/// @param funcs Sequence of borrowed callbacks with signature `void (*)(void)`.
void rt_parallel_invoke(void *funcs) {
    rt_parallel_invoke_pool(funcs, NULL);
}

//=============================================================================
// Parallel Reduce
//=============================================================================

/// @brief Reduce a sequence with an associative combiner through an optional pool.
/// @details Empty or invalid input returns @p identity. Small inputs and nested
///          same-pool calls fold serially. Larger inputs are divided into
///          contiguous chunks that fold from their first item; the caller then
///          combines ordered partials starting from @p identity, so the
///          identity is applied exactly once. Accumulator pointers are passed
///          through without runtime retains or releases. Worker and final
///          combiner traps are propagated after batch cleanup.
/// @param seq Sequence of values to reduce.
/// @param func Borrowed combiner with signature `void *(*)(void *, void *)`.
/// @param identity Initial accumulator and empty-sequence result.
/// @param pool Borrowed Threadpool, or NULL to use the shared default.
/// @return Final accumulator pointer, or @p identity after a reported failure.
/// @brief Reduce body shared by the pool and default-pool entry points.
/// @details Returns the reducer's final pointer exactly as produced, or the
///          borrowed @p identity when the sequence is empty or the work failed.
static void *parallel_reduce_pool_impl(void *seq, void *func, void *identity, void *pool) {
    if (!seq || !func)
        return identity;

    int64_t count = rt_seq_len(seq);
    if (count < 0) {
        rt_trap("Parallel.Reduce: negative sequence length");
        return identity;
    }
    if (count == 0)
        return identity;

    /* For small sequences, reduce serially. */
    void *(*combine)(void *, void *) = RT_FN_PTR_CAST((void *(*)(void *, void *))func);
    if (count <= 4) {
        void *accum = identity;
        for (int64_t i = 0; i < count; i++) {
            accum = combine(accum, rt_seq_get(seq, i));
        }
        return accum;
    }

    void *actual_pool = pool ? pool : rt_parallel_default_pool();
    if (parallel_is_current_pool(actual_pool)) {
        void *result = identity;
        char task_error[512];
        task_error[0] = '\0';
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (RT_SETJMP(recovery) == 0) {
            for (int64_t i = 0; i < count; i++)
                result = combine(result, rt_seq_get(seq, i));
        } else {
            parallel_copy_error(task_error, sizeof(task_error), "Parallel.Reduce: reducer trapped");
        }
        rt_trap_clear_recovery();
        parallel_release_default_pool(pool, actual_pool);
        if (task_error[0]) {
            parallel_trap_error("Parallel.Reduce: reducer trapped", task_error);
            return identity;
        }
        return result;
    }

    int64_t nworkers = parallel_pool_size(actual_pool);
    if (nworkers > count)
        nworkers = count;
    if (!parallel_count_fits_wait_counter(nworkers)) {
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Reduce: allocation size overflow");
        return identity;
    }

    /* Extract items array for chunk access. */
    if (!parallel_count_fits_array(count, sizeof(void *))) {
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Reduce: allocation size overflow");
        return identity;
    }
    void **items = (void **)malloc((size_t)count * sizeof(void *));
    if (!items) {
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Reduce: memory allocation failed");
        return identity;
    }
    for (int64_t i = 0; i < count; i++) {
        items[i] = rt_seq_get(seq, i);
    }

#if RT_PLATFORM_WINDOWS
    LONG *remaining = parallel_win_remaining_new(nworkers);
    if (!remaining) {
        free(items);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Reduce: memory allocation failed");
        return identity;
    }
    LONG task_failed = 0;
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!event) {
        free(remaining);
        free(items);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Reduce: event creation failed");
        return identity;
    }
    CRITICAL_SECTION error_lock;
    if (!InitializeCriticalSectionEx(&error_lock, 0, 0)) {
        parallel_win_close_event(event);
        free(remaining);
        free(items);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Reduce: error mutex initialization failed");
        return identity;
    }
#else
    int task_failed = 0;
    parallel_sync *sync = parallel_sync_new((int)nworkers);
    if (!sync) {
        free(items);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Reduce: memory allocation failed");
        return identity;
    }
#endif
    char task_error[512];
    task_error[0] = '\0';

    if (!parallel_count_fits_array(nworkers, sizeof(reduce_task))) {
#if RT_PLATFORM_WINDOWS
        parallel_win_close_event(event);
        DeleteCriticalSection(&error_lock);
        free(remaining);
#else
        parallel_sync_destroy(sync);
#endif
        free(items);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Reduce: allocation size overflow");
        return identity;
    }
    reduce_task *tasks = (reduce_task *)malloc((size_t)nworkers * sizeof(reduce_task));
    if (!tasks) {
#if RT_PLATFORM_WINDOWS
        parallel_win_close_event(event);
        DeleteCriticalSection(&error_lock);
        free(remaining);
#else
        parallel_sync_destroy(sync);
#endif
        free(items);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.Reduce: memory allocation failed");
        return identity;
    }

    int64_t chunk = count / nworkers;
    int64_t remainder = count % nworkers;
    int64_t offset = 0;
    int submit_failed = 0;

    for (int64_t i = 0; i < nworkers; i++) {
        int64_t chunk_size = chunk + (i < remainder ? 1 : 0);
        tasks[i].items = items;
        tasks[i].start = offset;
        tasks[i].end = offset + chunk_size;
        tasks[i].func = combine;
        tasks[i].identity = identity;
        tasks[i].result = identity;
#if RT_PLATFORM_WINDOWS
        tasks[i].remaining = remaining;
        tasks[i].failed = &task_failed;
        tasks[i].event = event;
        tasks[i].error_lock = &error_lock;
#else
        tasks[i].remaining = &sync->remaining;
        tasks[i].failed = &task_failed;
        tasks[i].mutex = &sync->mutex;
        tasks[i].cond = &sync->cond;
#endif
        tasks[i].error = task_error;
        tasks[i].error_size = sizeof(task_error);
        if (!rt_threadpool_submit(actual_pool, fnptr_to_voidptr(reduce_callback), &tasks[i])) {
            submit_failed = 1;
#if RT_PLATFORM_WINDOWS
            parallel_win_complete_one(remaining, event);
#else
            parallel_sync_complete(sync);
#endif
        }
        offset += chunk_size;
    }

    /* Wait for completion. */
#if RT_PLATFORM_WINDOWS
    parallel_win_wait_for_completion(event);
    parallel_win_close_event(event);
    DeleteCriticalSection(&error_lock);
    free(remaining);
#else
    parallel_sync_wait_and_free(sync);
#endif

    /* Apply the identity exactly once on the caller thread. */
    void *result = identity;
    int combine_failed = 0;
    char combine_error[512];
    combine_error[0] = '\0';
    if (!task_failed && !submit_failed) {
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (RT_SETJMP(recovery) == 0) {
            for (int64_t i = 0; i < nworkers; i++) {
                result = combine(result, tasks[i].result);
            }
        } else {
            parallel_copy_error(
                combine_error, sizeof(combine_error), "Parallel.Reduce: reducer trapped");
            combine_failed = 1;
        }
        rt_trap_clear_recovery();
    }

    free(tasks);
    free(items);
    parallel_release_default_pool(pool, actual_pool);
    if (combine_failed) {
        parallel_trap_error("Parallel.Reduce: reducer trapped", combine_error);
        return identity;
    }
    if (task_failed) {
        parallel_trap_error("Parallel.Reduce: task trapped", task_error);
        return identity;
    }
    if (submit_failed) {
        rt_trap("Parallel.Reduce: failed to submit work");
        return identity;
    }
    return result;
}

/// @brief Reduce a sequence through the default pool.
/// @param seq Sequence of values to reduce.
/// @param func Borrowed combiner with signature `void *(*)(void *, void *)`.
/// @param identity Initial accumulator and empty-sequence result.
/// @return Final accumulator pointer, or @p identity after a reported failure.
void *rt_parallel_reduce_pool(void *seq, void *func, void *identity, void *pool) {
    void *result = parallel_reduce_pool_impl(seq, func, identity, pool);
    /* The caller always receives one reference it owns (ADR 0314): the reducer
       hands its results back retained, and an untouched identity is retained here. */
    if (result == identity)
        rt_obj_retain_maybe(result);
    return result;
}

void *rt_parallel_reduce(void *seq, void *func, void *identity) {
    return rt_parallel_reduce_pool(seq, func, identity, NULL);
}

//=============================================================================
// Parallel For
//=============================================================================

/// @brief Invoke a callback for each integer in a half-open range.
/// @details The representable range is balanced across tasks in an optional
///          Threadpool and the call waits for all tasks. Empty or reversed
///          ranges and NULL callbacks are no-ops. Nested same-pool calls run
///          serially. An interval larger than `INT64_MAX` or any worker,
///          allocation, or submission failure raises a runtime trap.
/// @param start Inclusive first integer.
/// @param end Exclusive range bound.
/// @param func Borrowed callback with signature `void (*)(int64_t)`.
/// @param pool Borrowed Threadpool, or NULL to use the shared default.
void rt_parallel_for_pool(int64_t start, int64_t end, void *func, void *pool) {
    if (!func || start >= end)
        return;

    uint64_t count_u = (uint64_t)end - (uint64_t)start;
    if (count_u > (uint64_t)INT64_MAX) {
        rt_trap("Parallel.For: range too large");
        return;
    }
    int64_t count = (int64_t)count_u;
    void *actual_pool = pool ? pool : rt_parallel_default_pool();
    int64_t task_count = parallel_choose_task_count(actual_pool, count);
    if (parallel_is_current_pool(actual_pool)) {
        char task_error[512];
        task_error[0] = '\0';
        jmp_buf recovery;
        rt_trap_set_recovery(&recovery);
        if (RT_SETJMP(recovery) == 0) {
            void (*worker)(int64_t) = RT_FN_PTR_CAST((void (*)(int64_t))func);
            for (int64_t i = start; i < end; i++)
                worker(i);
        } else {
            parallel_copy_error(task_error, sizeof(task_error), "Parallel.For: task trapped");
        }
        rt_trap_clear_recovery();
        parallel_release_default_pool(pool, actual_pool);
        if (task_error[0])
            parallel_trap_error("Parallel.For: task trapped", task_error);
        return;
    }

    if (!parallel_count_fits_wait_counter(task_count)) {
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.For: allocation size overflow");
        return;
    }

#if RT_PLATFORM_WINDOWS
    LONG *remaining = parallel_win_remaining_new(task_count);
    if (!remaining) {
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.For: memory allocation failed");
        return;
    }
    LONG task_failed = 0;
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!event) {
        free(remaining);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.For: event creation failed");
        return;
    }
    CRITICAL_SECTION error_lock;
    if (!InitializeCriticalSectionEx(&error_lock, 0, 0)) {
        parallel_win_close_event(event);
        free(remaining);
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.For: error mutex initialization failed");
        return;
    }
#else
    int task_failed = 0;
    parallel_sync *sync = parallel_sync_new((int)task_count);
    if (!sync) {
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.For: memory allocation failed");
        return;
    }
#endif
    char task_error[512];
    task_error[0] = '\0';

    // Allocate task array
    if (!parallel_count_fits_array(task_count, sizeof(for_task))) {
#if RT_PLATFORM_WINDOWS
        parallel_win_close_event(event);
        DeleteCriticalSection(&error_lock);
        free(remaining);
#else
        parallel_sync_destroy(sync);
#endif
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.For: allocation size overflow");
        return;
    }
    for_task *tasks = (for_task *)malloc((size_t)task_count * sizeof(for_task));
    if (!tasks) {
#if RT_PLATFORM_WINDOWS
        parallel_win_close_event(event);
        DeleteCriticalSection(&error_lock);
        free(remaining);
#else
        parallel_sync_destroy(sync);
#endif
        parallel_release_default_pool(pool, actual_pool);
        rt_trap("Parallel.For: memory allocation failed");
        return;
    }

    int submit_failed = 0;

    // Submit all tasks
    for (int64_t i = 0; i < task_count; i++) {
        int64_t local_start = 0;
        int64_t local_end = 0;
        parallel_split_range(count, task_count, i, &local_start, &local_end);
        tasks[i].start = start + local_start;
        tasks[i].end = start + local_end;
        tasks[i].func = RT_FN_PTR_CAST((void (*)(int64_t))func);
#if RT_PLATFORM_WINDOWS
        tasks[i].remaining = remaining;
        tasks[i].failed = &task_failed;
        tasks[i].event = event;
        tasks[i].error_lock = &error_lock;
#else
        tasks[i].remaining = &sync->remaining;
        tasks[i].failed = &task_failed;
        tasks[i].mutex = &sync->mutex;
        tasks[i].cond = &sync->cond;
#endif
        tasks[i].error = task_error;
        tasks[i].error_size = sizeof(task_error);
        if (!rt_threadpool_submit(actual_pool, fnptr_to_voidptr(for_callback), &tasks[i])) {
            submit_failed = 1;
#if RT_PLATFORM_WINDOWS
            parallel_win_complete_one(remaining, event);
#else
            parallel_sync_complete(sync);
#endif
        }
    }

    // Wait for completion
#if RT_PLATFORM_WINDOWS
    parallel_win_wait_for_completion(event);
    parallel_win_close_event(event);
    DeleteCriticalSection(&error_lock);
    free(remaining);
#else
    parallel_sync_wait_and_free(sync);
#endif

    free(tasks);

    parallel_release_default_pool(pool, actual_pool);
    if (task_failed)
        parallel_trap_error("Parallel.For: task trapped", task_error);
    if (submit_failed)
        rt_trap("Parallel.For: failed to submit work");
}

/// @brief Invoke a callback for each integer through the default pool.
/// @param start Inclusive first integer.
/// @param end Exclusive range bound.
/// @param func Borrowed callback with signature `void (*)(int64_t)`.
void rt_parallel_for(int64_t start, int64_t end, void *func) {
    rt_parallel_for_pool(start, end, func, NULL);
}

//=============================================================================
// Task Callbacks
//=============================================================================

/// @brief Per-task entry point for `Parallel.ForEach`.
/// @details Runs `task->func(task->items[i])` over its assigned [start, end) slice with a
///          local setjmp recovery frame so a Zia-side trap doesn't crash the worker. On trap,
///          captures the message into `task->error` (first-trap-wins under the batch lock)
///          and sets `*task->failed`. Always decrements `*task->remaining` exactly once and
///          signals the wait event/condvar when it hits zero so the submitter can return.
/// @param arg Pointer to a borrowed `foreach_task` record valid until the batch drains.
static void foreach_callback(void *arg) {
    foreach_task *task = (foreach_task *)arg;
    int task_failed = 0;
    char local_error[512];
    local_error[0] = '\0';

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) == 0) {
        for (int64_t i = task->start; i < task->end; i++)
            task->func(task->items[i]);
    } else {
        parallel_copy_error(local_error, sizeof(local_error), "Parallel.ForEach: task trapped");
        task_failed = 1;
    }
    rt_trap_clear_recovery();

#if RT_PLATFORM_WINDOWS
    if (task_failed) {
        EnterCriticalSection(task->error_lock);
        if (task->error && task->error_size > 0 && task->error[0] == '\0') {
            strncpy(task->error, local_error, task->error_size - 1);
            task->error[task->error_size - 1] = '\0';
        }
        LeaveCriticalSection(task->error_lock);
        InterlockedExchange(task->failed, 1);
    }
    parallel_win_complete_one(task->remaining, task->event);
#else
    pthread_mutex_lock(task->mutex);
    if (task_failed) {
        if (task->error && task->error_size > 0 && task->error[0] == '\0') {
            strncpy(task->error, local_error, task->error_size - 1);
            task->error[task->error_size - 1] = '\0';
        }
        *task->failed = 1;
    }
    (*task->remaining)--;
    if (*task->remaining == 0) {
        pthread_cond_signal(task->cond);
    }
    pthread_mutex_unlock(task->mutex);
#endif
}

/// @brief Per-task entry point for `Parallel.Map`.
/// @details Same skeleton as `foreach_callback`, but each iteration writes
///          `task->func(items[i])` into `results[i]`. Callback returns are
///          retained before storage so borrowed results remain valid until the
///          returned sequence owns them.
///          Cleanup of partially populated retained results on a trap path is the caller's
///          responsibility (see `parallel_release_map_results`). Trap capture and
///          batch-completion signaling are identical to ForEach.
/// @param arg Pointer to a borrowed `map_task` record valid until the batch drains.
static void map_callback(void *arg) {
    map_task *task = (map_task *)arg;
    int task_failed = 0;
    char local_error[512];
    local_error[0] = '\0';

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) == 0) {
        for (int64_t i = task->start; i < task->end; i++) {
            void *mapped = task->func(task->items[i]);
            rt_obj_retain_maybe(mapped);
            task->results[i] = mapped;
        }
    } else {
        parallel_copy_error(local_error, sizeof(local_error), "Parallel.Map: task trapped");
        task_failed = 1;
    }
    rt_trap_clear_recovery();

#if RT_PLATFORM_WINDOWS
    if (task_failed) {
        EnterCriticalSection(task->error_lock);
        if (task->error && task->error_size > 0 && task->error[0] == '\0') {
            strncpy(task->error, local_error, task->error_size - 1);
            task->error[task->error_size - 1] = '\0';
        }
        LeaveCriticalSection(task->error_lock);
        InterlockedExchange(task->failed, 1);
    }
    parallel_win_complete_one(task->remaining, task->event);
#else
    pthread_mutex_lock(task->mutex);
    if (task_failed) {
        if (task->error && task->error_size > 0 && task->error[0] == '\0') {
            strncpy(task->error, local_error, task->error_size - 1);
            task->error[task->error_size - 1] = '\0';
        }
        *task->failed = 1;
    }
    (*task->remaining)--;
    if (*task->remaining == 0) {
        pthread_cond_signal(task->cond);
    }
    pthread_mutex_unlock(task->mutex);
#endif
}

/// @brief Per-task entry point for `Parallel.Invoke`.
/// @details Runs a single nullary `task->func()` under a setjmp recovery frame. Used to spawn
///          a fixed list of independent callbacks concurrently. Trap capture and the
///          remaining/failed/event/condvar accounting match ForEach exactly — only the inner
///          work is different.
/// @param arg Pointer to a borrowed `invoke_task` record valid until the batch drains.
static void invoke_callback(void *arg) {
    invoke_task *task = (invoke_task *)arg;
    int task_failed = 0;
    char local_error[512];
    local_error[0] = '\0';

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) == 0)
        task->func();
    else {
        parallel_copy_error(local_error, sizeof(local_error), "Parallel.Invoke: task trapped");
        task_failed = 1;
    }
    rt_trap_clear_recovery();

#if RT_PLATFORM_WINDOWS
    if (task_failed) {
        EnterCriticalSection(task->error_lock);
        if (task->error && task->error_size > 0 && task->error[0] == '\0') {
            strncpy(task->error, local_error, task->error_size - 1);
            task->error[task->error_size - 1] = '\0';
        }
        LeaveCriticalSection(task->error_lock);
        InterlockedExchange(task->failed, 1);
    }
    parallel_win_complete_one(task->remaining, task->event);
#else
    pthread_mutex_lock(task->mutex);
    if (task_failed) {
        if (task->error && task->error_size > 0 && task->error[0] == '\0') {
            strncpy(task->error, local_error, task->error_size - 1);
            task->error[task->error_size - 1] = '\0';
        }
        *task->failed = 1;
    }
    (*task->remaining)--;
    if (*task->remaining == 0) {
        pthread_cond_signal(task->cond);
    }
    pthread_mutex_unlock(task->mutex);
#endif
}

/// @brief Per-task entry point for `Parallel.Reduce` (per-task partial fold).
/// @details Folds the [start, end) slice with `task->func(accum, items[i])`. An empty slice
///          stores the user-supplied identity into `task->result` so the caller's final
///          combine step has a neutral value to absorb. Trap capture and batch signaling
///          mirror ForEach. The cross-slice combine that turns these partials into the
///          single overall result happens on the submitting thread, not here. Accumulator
///          ownership is the reducer's contract: the runtime forwards partial/final pointers
///          exactly as produced and does not retain or release intermediate accumulators.
/// @param arg Pointer to a borrowed `reduce_task` record valid until the batch drains.
static void reduce_callback(void *arg) {
    reduce_task *task = (reduce_task *)arg;
    int task_failed = 0;
    char local_error[512];
    local_error[0] = '\0';

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) == 0) {
        if (task->start >= task->end) {
            task->result = task->identity;
        } else {
            void *accum = task->items[task->start];
            for (int64_t i = task->start + 1; i < task->end; i++) {
                accum = task->func(accum, task->items[i]);
            }
            task->result = accum;
        }
    } else {
        parallel_copy_error(local_error, sizeof(local_error), "Parallel.Reduce: task trapped");
        task_failed = 1;
    }
    rt_trap_clear_recovery();

#if RT_PLATFORM_WINDOWS
    if (task_failed) {
        EnterCriticalSection(task->error_lock);
        if (task->error && task->error_size > 0 && task->error[0] == '\0') {
            strncpy(task->error, local_error, task->error_size - 1);
            task->error[task->error_size - 1] = '\0';
        }
        LeaveCriticalSection(task->error_lock);
        InterlockedExchange(task->failed, 1);
    }
    parallel_win_complete_one(task->remaining, task->event);
#else
    pthread_mutex_lock(task->mutex);
    if (task_failed) {
        if (task->error && task->error_size > 0 && task->error[0] == '\0') {
            strncpy(task->error, local_error, task->error_size - 1);
            task->error[task->error_size - 1] = '\0';
        }
        *task->failed = 1;
    }
    (*task->remaining)--;
    if (*task->remaining == 0) {
        pthread_cond_signal(task->cond);
    }
    pthread_mutex_unlock(task->mutex);
#endif
}

/// @brief Per-task entry point for `Parallel.For` (numeric range iteration).
/// @details Calls `task->func(i)` for each integer `i` in [start, end). Differs from
///          ForEach only in that the callback receives an `int64_t` index rather than an
///          element pointer — there is no input array to dereference. Trap capture and
///          batch-completion bookkeeping are identical to the other callbacks.
/// @param arg Pointer to a borrowed `for_task` record valid until the batch drains.
static void for_callback(void *arg) {
    for_task *task = (for_task *)arg;
    int task_failed = 0;
    char local_error[512];
    local_error[0] = '\0';

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (RT_SETJMP(recovery) == 0) {
        for (int64_t i = task->start; i < task->end; i++)
            task->func(i);
    } else {
        parallel_copy_error(local_error, sizeof(local_error), "Parallel.For: task trapped");
        task_failed = 1;
    }
    rt_trap_clear_recovery();

#if RT_PLATFORM_WINDOWS
    if (task_failed) {
        EnterCriticalSection(task->error_lock);
        if (task->error && task->error_size > 0 && task->error[0] == '\0') {
            strncpy(task->error, local_error, task->error_size - 1);
            task->error[task->error_size - 1] = '\0';
        }
        LeaveCriticalSection(task->error_lock);
        InterlockedExchange(task->failed, 1);
    }
    parallel_win_complete_one(task->remaining, task->event);
#else
    pthread_mutex_lock(task->mutex);
    if (task_failed) {
        if (task->error && task->error_size > 0 && task->error[0] == '\0') {
            strncpy(task->error, local_error, task->error_size - 1);
            task->error[task->error_size - 1] = '\0';
        }
        *task->failed = 1;
    }
    (*task->remaining)--;
    if (*task->remaining == 0) {
        pthread_cond_signal(task->cond);
    }
    pthread_mutex_unlock(task->mutex);
#endif
}
