//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/threads/rt_async.h
// Purpose: Async task combinators built on Future/Promise and threads, providing
//          All, Any, Delay, Run, Map, and RunCancellable wrappers for composing
//          asynchronous operations.
//
// Key invariants:
//   - All functions return Future objects.
//   - rt_async_all resolves when every input future completes or the first input
//     error arrives.
//   - rt_async_any resolves with the first completed input future's value/error.
//   - rt_async_run and rt_async_run_cancellable spawn background threads.
//   - All operations are thread-safe.
//
// Ownership/Lifetime:
//   - Returned Future objects are GC-managed opaque pointers.
//   - Callers should not free Future objects directly.
//
// Links: src/runtime/threads/rt_async.c (implementation), src/runtime/threads/rt_future.h,
// src/runtime/threads/rt_cancellation.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief High-level asynchronous execution and Future-composition C ABI.
/// @details Run, Delay, and RunCancellable use detached worker threads. Map,
///          Any, and All attach completion listeners and do not block helper
///          threads. Traps raised by user callbacks are converted into rejected
///          Futures.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Zanna.Threads.Async — High-level async combinators
//=========================================================================

/// @brief Execute a unary callback on a detached background thread.
/// @details A callback trap or thread-setup failure rejects the returned
///          Future. The callback result is published when execution completes.
/// @param callback Nonnull function compatible with `void *(*)(void *)`.
/// @param arg Borrowed opaque callback argument.
/// @return Runtime-managed Future resolving with the callback result; a nil
///         callback traps and returns null.
/// @warning The caller must keep @p arg alive until the callback consumes it.
void *rt_async_run(void *callback, void *arg);

/// @brief Run a callback asynchronously, retaining a runtime-managed argument.
/// @details Like rt_async_run, but @p arg must be a runtime-managed object or
///          string handle and is retained until the callback has finished. If
///          the callback returns that same argument, its retained reference is
///          transferred to the Future.
/// @param callback Nonnull function compatible with `void *(*)(void *)`.
/// @param arg Runtime-managed argument to retain, or null.
/// @return Runtime-managed Future resolving with the callback result; a nil
///         callback traps and returns null.
void *rt_async_run_owned(void *callback, void *arg);

/// @brief Wait for all Futures to complete and collect results.
/// @details Returns a Future that resolves with a Seq of results once all
///          input futures have resolved. If any future has an error, the
///          combined Future rejects and outstanding listeners are cancelled.
///          An empty input resolves immediately with an empty sequence.
/// @param futures Borrowed sequence of Future objects.
/// @return Runtime-managed Future resolving to an owning result sequence in
///         input order, or rejecting on a source or setup error.
void *rt_async_all(void *futures);

/// @brief Wait for the first Future to complete.
/// @details The first source completion wins whether it is successful or
///          rejected; listeners on remaining sources are cancelled. An empty
///          input produces a rejected Future.
/// @param futures Borrowed sequence of Future objects.
/// @return Runtime-managed Future forwarding the first completion or a setup error.
void *rt_async_any(void *futures);

/// @brief Return a Future that resolves after a delay.
/// @details The returned Future resolves with NULL after the delay.
/// @param ms Delay in milliseconds; negative values are treated as zero.
/// @return Runtime-managed Future, rejected only when worker setup fails.
void *rt_async_delay(int64_t ms);

/// @brief Chain a transformation onto a Future.
/// @details When the source future resolves, applies mapper(result, arg)
///          and resolves the returned Future with the mapper's result. Source
///          errors propagate without invoking the mapper; mapper traps become
///          downstream errors.
/// @param future Nonnull source Future.
/// @param mapper Nonnull function compatible with `void *(*)(void *, void *)`.
/// @param arg Borrowed extra mapper argument.
/// @return Runtime-managed downstream Future, or null after an invalid-input trap.
/// @warning The caller must keep @p arg alive until the mapper runs.
void *rt_async_map(void *future, void *mapper, void *arg);

/// @brief Chain a transformation onto a Future with a retained runtime argument.
/// @details Like rt_async_map, but retains @p arg until the mapper has run.
/// @param future Nonnull source Future.
/// @param mapper Nonnull function compatible with `void *(*)(void *, void *)`.
/// @param arg Runtime-managed extra argument to retain, or null.
/// @return Runtime-managed downstream Future, or null after an invalid-input trap.
void *rt_async_map_owned(void *future, void *mapper, void *arg);

/// @brief Run a callback asynchronously with cancellation support.
/// @details The callback receives the token as its second argument. The token
///          is retained and checked before and after invocation; observing
///          cancellation rejects the Future as `"cancelled"`.
/// @param callback Nonnull function compatible with `void *(*)(void *, void *)`.
/// @param arg Borrowed opaque callback argument.
/// @param token Cancellation token, or null.
/// @return Runtime-managed Future resolving with the callback result or rejecting.
/// @warning The caller must keep @p arg alive until the callback consumes it.
void *rt_async_run_cancellable(void *callback, void *arg, void *token);

/// @brief Run a cancellable callback asynchronously with a retained runtime argument.
/// @details Like rt_async_run_cancellable, but retains @p arg until the callback
///          has finished.
/// @param callback Nonnull function compatible with `void *(*)(void *, void *)`.
/// @param arg Runtime-managed callback argument to retain, or null.
/// @param token Cancellation token, or null.
/// @return Runtime-managed Future resolving with the callback result or rejecting.
void *rt_async_run_cancellable_owned(void *callback, void *arg, void *token);

#ifdef __cplusplus
}
#endif
