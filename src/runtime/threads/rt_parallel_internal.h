//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_parallel_internal.h
// Purpose: Shared task-context types and pool/sync helpers for the data-parallel
//          combinators, split between rt_parallel.c (pool + sync + callbacks)
//          and rt_parallel_ops.c (ForEach/Map/Invoke/Reduce/For).
//
// Key invariants:
//   - Engine-internal; included only by the threads/ parallel translation units.
//   - Platform sync uses Win32 events/Interlocked on Windows, pthread mutex/cond
//     otherwise; the per-task contexts carry the matching field set.
//   - The first worker to trap captures its message into the per-batch error
//     slot; helpers here marshal that slot and the completion counters.
//
// Ownership/Lifetime:
//   - Task contexts are stack/heap owned by the submitting combinator; helpers
//     borrow them. parallel_sync blocks are heap-allocated (POSIX path).
//
// Links: src/runtime/threads/rt_parallel.c, src/runtime/threads/rt_parallel_ops.c
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares task records and helpers shared by parallel operations.
/// @details Each operation fills per-task records that borrow its input,
///          callback, error buffer, and platform completion state until the
///          synchronous batch drains. Platform-neutral declarations expose
///          pool selection, partitioning, error propagation, and cleanup
///          implemented by `rt_parallel.c`.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rt_platform.h"

#if RT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

//=============================================================================
// Per-task context types (shared by the combinators and their callbacks)
//=============================================================================

/// @brief Work slice and shared completion state for a ForEach task.
typedef struct {
    void **items;
    int64_t start;
    int64_t end;
    /// @brief Process one borrowed item in the assigned slice.
    /// @param item Borrowed sequence element.
    void (*func)(void *);
#if RT_PLATFORM_WINDOWS
    LONG *remaining;
    LONG *failed;
    HANDLE event;
    CRITICAL_SECTION *error_lock;
#else
    int *remaining;
    int *failed;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
#endif
    char *error;
    size_t error_size;
} foreach_task;

/// @brief Work slice, positional output array, and shared state for a Map task.
typedef struct {
    void **items;
    void **results;
    /// @brief Transform one borrowed input item.
    /// @param item Borrowed sequence element.
    /// @return Runtime value stored at the corresponding output position.
    void *(*func)(void *);
    int64_t start;
    int64_t end;
#if RT_PLATFORM_WINDOWS
    LONG *remaining;
    LONG *failed;
    HANDLE event;
    CRITICAL_SECTION *error_lock;
#else
    int *remaining;
    int *failed;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
#endif
    char *error;
    size_t error_size;
} map_task;

/// @brief One nullary callback and shared completion state for an Invoke task.
typedef struct {
    /// @brief Execute the task's nullary user callback.
    void (*func)(void);
#if RT_PLATFORM_WINDOWS
    LONG *remaining;
    LONG *failed;
    HANDLE event;
    CRITICAL_SECTION *error_lock;
#else
    int *remaining;
    int *failed;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
#endif
    char *error;
    size_t error_size;
} invoke_task;

/// @brief Input slice, partial accumulator, and shared state for a Reduce task.
typedef struct {
    void **items;
    int64_t start;
    int64_t end;
    /// @brief Combine the current accumulator with one borrowed item.
    /// @param accumulator Current partial accumulator.
    /// @param item Borrowed sequence element.
    /// @return Updated accumulator.
    void *(*func)(void *, void *);
    void *identity;
    void *result;
#if RT_PLATFORM_WINDOWS
    LONG *remaining;
    LONG *failed;
    HANDLE event;
    CRITICAL_SECTION *error_lock;
#else
    int *remaining;
    int *failed;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
#endif
    char *error;
    size_t error_size;
} reduce_task;

/// @brief Integer subrange and shared completion state for a parallel For task.
typedef struct {
    int64_t start;
    int64_t end;
    /// @brief Process one integer in the assigned half-open range.
    /// @param index Current integer value.
    void (*func)(int64_t);
#if RT_PLATFORM_WINDOWS
    LONG *remaining;
    LONG *failed;
    HANDLE event;
    CRITICAL_SECTION *error_lock;
#else
    int *remaining;
    int *failed;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
#endif
    char *error;
    size_t error_size;
} for_task;

#if !RT_PLATFORM_WINDOWS
/// @brief Heap synchronization state shared by every POSIX task in one batch.
typedef struct {
    int remaining;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} parallel_sync;
#endif

//=============================================================================
// Shared pool / sync / error helpers (defined in rt_parallel.c)
//=============================================================================

#if RT_PLATFORM_WINDOWS
/// @brief Allocate a Windows interlocked completion counter.
/// @param count Initial outstanding-task count, validated to fit `LONG`.
/// @return Heap-allocated counter, or NULL on allocation failure.
LONG *parallel_win_remaining_new(int64_t count);

/// @brief Record one Windows task completion and signal the last-task event.
/// @param remaining Shared outstanding-task counter.
/// @param event Event awaited by the submitting thread.
void parallel_win_complete_one(LONG *remaining, HANDLE event);

/// @brief Wait indefinitely for a Windows parallel batch to drain.
/// @param event Completion event signaled by the last worker.
void parallel_win_wait_for_completion(HANDLE event);

/// @brief Close a batch-completion event or terminate on an ownership invariant failure.
/// @param event Owned event handle that must be valid and closable.
void parallel_win_close_event(HANDLE event);
#endif

/// @brief Convert a function pointer to void* without pedantic warnings.
/// @details Copies the pointer representation after statically requiring equal
///          function- and data-pointer sizes. Threadpool submission later
///          reverses the same representation bridge.
/// @param fn Unary parallel-task callback.
/// @return Data-pointer representation of @p fn.
static inline void *fnptr_to_voidptr(void (*fn)(void *)) {
    void *p;
    _Static_assert(sizeof(p) == sizeof(fn),
                   "Parallel task callback bridge requires equal function/data pointer sizes");
    memcpy(&p, &fn, sizeof(p));
    return p;
}

#if !RT_PLATFORM_WINDOWS
/// @brief Allocate and initialize a POSIX batch synchronization block.
/// @param initial Number of task completions to observe.
/// @return Heap synchronization block, or NULL on allocation or initialization failure.
parallel_sync *parallel_sync_new(int initial);

/// @brief Wait until a POSIX batch drains, then destroy and consume its sync block.
/// @param s Synchronization block to wait on and free.
void parallel_sync_wait_and_free(parallel_sync *s);

/// @brief Destroy an unpublished POSIX synchronization block without waiting.
/// @param s Synchronization block to consume, or NULL.
void parallel_sync_destroy(parallel_sync *s);

/// @brief Decrement a POSIX batch counter and signal when the final task completes.
/// @param s Shared synchronization block for the running batch.
void parallel_sync_complete(parallel_sync *s);
#endif

/// @brief Release a per-call default-pool retain when no explicit pool was supplied.
/// @param requested_pool Pool requested by the caller, or NULL for default selection.
/// @param actual_pool Effective pool used by the operation, or NULL.
void parallel_release_default_pool(void *requested_pool, void *actual_pool);

/// @brief Resolve a positive worker count for an optional Threadpool.
/// @param pool Optional Threadpool whose configured size is preferred.
/// @return Positive effective worker count.
int64_t parallel_pool_size(void *pool);

/// @brief Choose a bounded task count for a logical workload.
/// @param pool Optional effective Threadpool used for capacity.
/// @param count Number of work items.
/// @return Chosen partition count, including zero for an empty workload.
int64_t parallel_choose_task_count(void *pool, int64_t count);

/// @brief Compute one balanced half-open partition of a logical range.
/// @param count Total number of work items.
/// @param task_count Positive number of partitions.
/// @param task_index Zero-based partition index.
/// @param start_out Optional destination for the inclusive start.
/// @param end_out Optional destination for the exclusive end.
void parallel_split_range(
    int64_t count, int64_t task_count, int64_t task_index, int64_t *start_out, int64_t *end_out);

/// @brief Preserve the first recovered worker diagnostic in a bounded buffer.
/// @param dst Destination error buffer.
/// @param dst_size Capacity of @p dst in bytes.
/// @param fallback Optional text used when the trap has no diagnostic.
void parallel_copy_error(char *dst, size_t dst_size, const char *fallback);

/// @brief Raise a captured worker diagnostic on the submitting thread.
/// @param fallback Diagnostic used when @p captured is empty or NULL.
/// @param captured Captured worker diagnostic, or NULL.
void parallel_trap_error(const char *fallback, const char *captured);

/// @brief Test whether an array allocation size is non-negative and representable.
/// @param count Requested element count.
/// @param elem_size Size of each element in bytes.
/// @return Non-zero when `count * elem_size` fits `size_t`.
int parallel_count_fits_array(int64_t count, size_t elem_size);

/// @brief Test whether a task count fits the shared completion-counter type.
/// @param count Requested task count.
/// @return Non-zero when @p count is in `[0, INT_MAX]`.
int parallel_count_fits_wait_counter(int64_t count);

/// @brief Test whether the caller is already a worker of a Threadpool.
/// @param pool Candidate Threadpool object.
/// @return Non-zero when the current worker belongs to @p pool.
int parallel_is_current_pool(void *pool);

/// @brief Release and clear retained values in a failed Map result array.
/// @param results Result array whose populated entries are consumed.
/// @param items Legacy input-array argument, currently unused.
/// @param count Number of result slots to inspect.
void parallel_release_map_results(void **results, void **items, int64_t count);
