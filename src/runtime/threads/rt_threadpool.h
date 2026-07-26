//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/threads/rt_threadpool.h
// Purpose: Thread pool for async task execution (Zanna.Threads.Pool) with FIFO task dispatch,
// worker recycling, and configurable pool size.
//
// Key invariants:
//   - Tasks are dequeued FIFO; concurrent start/completion order is unspecified.
//   - The default pool size equals the number of CPU cores.
//   - rt_threadpool_wait blocks until all queued tasks are complete.
//   - Pool objects are reference-counted; the last release destroys the pool.
//
// Ownership/Lifetime:
//   - Pool objects are runtime-managed and reference-counted.
//   - Submitted task function pointers must remain valid until the task executes.
//
// Links: src/runtime/threads/rt_threadpool.c (implementation)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares a fixed-size asynchronous FIFO Threadpool.
/// @details Persistent workers dequeue submitted callbacks until graceful or
///          immediate shutdown. Borrowed and Pool-owned argument variants are
///          available. Wait and shutdown operations surface accumulated worker
///          traps on the observing caller and reject same-Pool worker waits to
///          prevent self-deadlock.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Type-safe native callback accepted by ThreadPool submission APIs.
/// @param arg Caller-provided task argument.
typedef void (*rt_threadpool_task_fn)(void *arg);

//=========================================================================
// Zanna.Threads.Pool
//=========================================================================

/// @brief Create a new thread pool with the specified number of workers.
/// @details Workers start immediately and wait for tasks. The pool size
///          is clamped into the supported range. Construction installs GC
///          traversal for owned queued arguments and a finalizer that stops and
///          joins workers if explicit shutdown is omitted.
/// @param size Number of worker threads (clamped to 1..1024).
/// @return Opaque Pool object pointer, or NULL on failure.
void *rt_threadpool_new(int64_t size);

/// @brief Submit a task to the pool for async execution.
/// @details The task is queued and executed by the next available worker.
///          The callback receives @p arg as its parameter. Both callback and
///          argument are borrowed and must remain valid until execution.
/// @param pool Pool object pointer.
/// @param callback Task function to execute (signature: void(*)(void*)).
/// @param arg Argument passed to the callback.
/// @return 1 if submitted; 0 for invalid inputs, shutdown, pending-queue
///         backpressure, or task-node allocation failure.
int8_t rt_threadpool_submit(void *pool, void *callback, void *arg);

/// @brief Submit a native task while preserving its C function-pointer type.
/// @details This internal runtime entry point has the same queueing and borrowed-
///          argument contract as @ref rt_threadpool_submit, but avoids converting
///          a function pointer through @c void*. Portable C adapters should use
///          this form; the legacy object-pointer ABI remains available to IL and
///          generated-code callers.
/// @param pool Live managed Pool object.
/// @param callback Native worker callback invoked once with @p arg.
/// @param arg Borrowed callback context that must remain live until execution.
/// @return One when accepted; zero for invalid input, shutdown, backpressure,
///         or task-node allocation failure.
int8_t rt_threadpool_submit_fn(void *pool, rt_threadpool_task_fn callback, void *arg);

/// @brief Owned-argument submit: the pool retains @p arg on acceptance and
///        releases it after the callback runs or when the task is discarded
///        by ShutdownNow/finalization (VDOC-128).
/// @details The callback uses the legacy object-pointer ABI. Rejection leaves
///          caller ownership unchanged.
/// @param pool Live managed Pool object.
/// @param callback Opaque representation of `void (*)(void *)`.
/// @param arg Runtime-managed callback context to retain, or NULL.
/// @return One when accepted; zero for invalid input, shutdown, backpressure,
///         argument-retain failure, or task-node allocation failure.
int8_t rt_threadpool_submit_owned(void *pool, void *callback, void *arg);

/// @brief Submit a typed native task whose managed argument is Pool-owned.
/// @details On acceptance the Pool acquires one managed reference to @p arg and
///          releases it after callback execution or queued-task cancellation.
///          Rejection leaves ownership unchanged. This typed companion avoids
///          the non-portable function-pointer conversion required by the legacy
///          @ref rt_threadpool_submit_owned ABI.
/// @param pool Live managed Pool object.
/// @param callback Native worker callback invoked once with @p arg.
/// @param arg Runtime-managed callback context, or NULL.
/// @return One when accepted; zero for invalid input, shutdown, backpressure,
///         argument-retain failure, or task-node allocation failure.
int8_t rt_threadpool_submit_owned_fn(void *pool, rt_threadpool_task_fn callback, void *arg);

/// @brief Wait for all pending tasks to complete.
/// @details Blocks until the task queue is empty and all workers are idle.
///          Does not prevent new tasks from being submitted. Accumulated worker
///          traps are consumed and raised after drain; calling from this Pool's
///          worker traps to prevent self-deadlock. NULL is a no-op.
/// @param pool Pool object pointer.
void rt_threadpool_wait(void *pool);

/// @brief Wait for all pending tasks with a timeout.
/// @details Blocks up to @p ms milliseconds for tasks to complete.
///          Non-positive values poll immediately. Timeout preserves worker
///          errors for a later observer; a completed wait consumes and raises
///          them. Same-Pool worker calls trap.
/// @param pool Pool object pointer.
/// @param ms Timeout in milliseconds.
/// @return 1 if all tasks completed, 0 if timed out.
int8_t rt_threadpool_wait_for(void *pool, int64_t ms);

/// @brief Shut down the pool gracefully.
/// @details Stops accepting new tasks and waits for pending tasks to
///          complete before terminating workers. Calling Submit after
///          shutdown returns 0. Repeated and concurrent calls coordinate one
///          worker-handle join phase. Accumulated worker errors are raised
///          after joining; same-Pool worker calls trap.
/// @param pool Pool object pointer.
void rt_threadpool_shutdown(void *pool);

/// @brief Shut down the pool immediately.
/// @details Stops accepting new tasks and discards queued tasks. Callbacks
///          already running are not interrupted; this call joins their workers
///          after those callbacks return. Pool-owned queued arguments are
///          released. Repeated calls are safe; same-Pool worker calls trap.
/// @param pool Pool object pointer.
void rt_threadpool_shutdown_now(void *pool);

/// @brief Get the number of worker threads.
/// @param pool Pool object pointer.
/// @return Number of workers in the pool.
int64_t rt_threadpool_get_size(void *pool);

/// @brief Get the number of pending tasks.
/// @details Returns the number of tasks waiting in the queue (not
///          including tasks currently being executed).
/// @param pool Pool object pointer.
/// @return Number of pending tasks.
int64_t rt_threadpool_get_pending(void *pool);

/// @brief Get the number of tasks currently running.
/// @param pool Pool object pointer.
/// @return Number of active tasks.
int64_t rt_threadpool_get_active(void *pool);

/// @brief Check if the pool is shut down.
/// @details Reports request state, which can precede completion of draining and
///          worker joining. NULL reports shut down.
/// @param pool Pool object pointer.
/// @return 1 if shut down, 0 otherwise.
int8_t rt_threadpool_get_is_shutdown(void *pool);

/// @brief Return the pool currently executing on this thread, or NULL.
/// @details Internal helper used to avoid nested parallel deadlocks. The
///          returned pointer is borrowed and only valid during worker execution.
/// @return Borrowed current Pool object, or NULL for non-worker threads.
void *rt_threadpool_current_worker_pool(void);

#ifdef __cplusplus
}
#endif
