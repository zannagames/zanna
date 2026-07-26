//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/threads/rt_parallel.h
// Purpose: High-level parallel execution patterns (ForEach, Map, Invoke, Reduce) using the thread
// pool, preserving output order and supporting custom pool injection.
//
// Key invariants:
//   - Output order matches input order for Map; ForEach has no defined result order.
//   - NULL pool argument falls back to the default global thread pool.
//   - All patterns wait for all tasks to complete before returning.
//   - Reduce uses a binary combine function with an initial accumulator value.
//
// Ownership/Lifetime:
//   - Returned sequences are caller-owned; caller must release.
//   - Thread pools are shared; caller must not destroy a pool in use by parallel operations.
//
// Links: src/runtime/threads/rt_parallel.c (implementation), src/runtime/threads/rt_threadpool.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares synchronous data-parallel sequence and integer-range operations.
/// @details Each operation partitions work across a caller-selected or shared
///          Threadpool and waits for the full batch before returning. Nested
///          use from a worker in the same pool falls back to serial execution
///          to avoid starvation deadlock. Worker traps are captured and raised
///          on the submitting thread after outstanding tasks drain.

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Zanna.Threads.Parallel
//=============================================================================

/// @brief Execute a function for each item in a sequence, in parallel.
/// @details Uses the retained shared default pool and returns only after every
///          callback has finished. Processing order is unspecified; the first
///          worker trap is re-raised after the batch drains.
/// @param seq Sequence of items to process.
/// @param func Borrowed callback with signature `void (*)(void *)`.
void rt_parallel_foreach(void *seq, void *func);

/// @brief Execute a function for each item with a custom thread pool.
/// @details Behaves like @ref rt_parallel_foreach while using @p pool when
///          supplied. The call borrows the pool for the synchronous operation.
/// @param seq Sequence of items to process.
/// @param func Borrowed callback with signature `void (*)(void *)`.
/// @param pool Thread pool to use (or NULL for default).
void rt_parallel_foreach_pool(void *seq, void *func, void *pool);

/// @brief Transform a sequence in parallel using a map function.
/// @details Applies @p func through the default pool and preserves input
///          position in the output regardless of worker completion order.
///          Runtime-managed callback results are retained for the result
///          sequence. Worker traps are re-raised after cleanup.
/// @param seq Sequence of items to transform.
/// @param func Borrowed transform with signature `void *(*)(void *)`.
/// @return Caller-owned sequence containing transformed items in input order.
void *rt_parallel_map(void *seq, void *func);

/// @brief Transform a sequence in parallel with a custom thread pool.
/// @details Behaves like @ref rt_parallel_map while borrowing @p pool for the
///          synchronous operation when supplied.
/// @param seq Sequence of items to transform.
/// @param func Borrowed transform with signature `void *(*)(void *)`.
/// @param pool Thread pool to use (or NULL for default).
/// @return Caller-owned sequence containing transformed items in input order.
void *rt_parallel_map_pool(void *seq, void *func, void *pool);

/// @brief Execute multiple functions in parallel and wait for all to complete.
/// @details Uses the default pool. Invocation order is unspecified, and the
///          first callback trap is re-raised after every submitted task drains.
/// @param funcs Sequence of borrowed callbacks with signature `void (*)(void)`.
void rt_parallel_invoke(void *funcs);

/// @brief Execute multiple functions in parallel with custom pool.
/// @details Borrows @p pool for the synchronous operation when supplied.
/// @param funcs Sequence of borrowed callbacks with signature `void (*)(void)`.
/// @param pool Thread pool to use (or NULL for default).
void rt_parallel_invoke_pool(void *funcs, void *pool);

/// @brief Parallel for loop over a range of integers.
/// @details Calls @p func once for each integer in `[start, end)` through the
///          default pool and waits for completion. An empty or reversed range
///          performs no callbacks.
/// @param start Start of range (inclusive).
/// @param end End of range (exclusive).
/// @param func Function to call (signature: void(*)(int64_t)).
void rt_parallel_for(int64_t start, int64_t end, void *func);

/// @brief Parallel for loop with custom pool.
/// @details Behaves like @ref rt_parallel_for while borrowing @p pool for the
///          synchronous operation when supplied.
/// @param start Start of range (inclusive).
/// @param end End of range (exclusive).
/// @param func Function to call.
/// @param pool Thread pool to use (or NULL for default).
void rt_parallel_for_pool(int64_t start, int64_t end, void *func, void *pool);

/// @brief Reduce a sequence in parallel using a binary combine function.
/// @details Splits the sequence into chunks, reduces each chunk in parallel from
///          that chunk's first element, then applies @p identity exactly once on
///          the calling thread while combining partial results. The operation
///          must be suitable for this grouping; worker completion order does
///          not reorder positional partial results.
/// @param seq Sequence of items to reduce.
/// @param func Binary function combining two items (signature: void*(*)(void*, void*)).
/// @param identity Identity element for the reduce operation.
/// @return The final reduced value.
void *rt_parallel_reduce(void *seq, void *func, void *identity);

/// @brief Reduce a sequence in parallel with a custom thread pool.
/// @details Behaves like @ref rt_parallel_reduce while borrowing @p pool for
///          the synchronous operation when supplied.
/// @param seq Sequence of items to reduce.
/// @param func Binary combine function.
/// @param identity Identity element.
/// @param pool Thread pool to use (or NULL for default).
/// @return The final reduced value.
void *rt_parallel_reduce_pool(void *seq, void *func, void *identity, void *pool);

/// @brief Get the default number of parallel workers.
/// @return Positive online CPU count, or 4 if detection fails.
int64_t rt_parallel_default_workers(void);

/// @brief Get or lazily create the shared default thread pool.
/// @details A shut-down singleton is replaced on demand. The singleton keeps
///          its own reference and this function returns an additional reference
///          that the caller must release.
/// @return Retained default Threadpool object, or NULL on creation failure.
void *rt_parallel_default_pool(void);

/// @brief Shut down and drain the shared default thread pool, joining its workers.
/// @details Intended for per-run teardown (e.g. VM context destruction). The default pool is a
///          process-lifetime singleton whose worker threads inherit and *bind* the runtime
///          context active when the pool was first created (see rt_thread_trampoline). If that
///          per-run context is freed while workers are still alive, their exit-time unbind
///          underflows the context bind_count and touches freed memory. Draining here forces
///          each worker to unbind while the context is still valid. Safe to call repeatedly and
///          when no pool exists; the next rt_parallel_default_pool() lazily recreates one.
void rt_parallel_shutdown_default_pool(void);

#ifdef __cplusplus
}
#endif
