//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/threads/rt_future.h
// Purpose: Future/Promise primitives for asynchronous result passing between threads, with blocking
// get, timed get, non-blocking try-get, and error propagation.
//
// Key invariants:
//   - A Promise can be completed exactly once (value or error); subsequent completions trap.
//   - Each Promise has exactly one associated Future.
//   - Future.get blocks until the Promise is resolved.
//   - Future.get traps if the Promise was completed with an error.
//   - Internal no-throw rejection still publishes an error state if diagnostic
//     string allocation fails, so worker Futures never remain pending on OOM.
//
// Ownership/Lifetime:
//   - Opaque Promise and Future objects are managed by the runtime object system.
//   - The Future is obtained from its Promise and shares its lifetime.
//
// Links: src/runtime/threads/rt_future.c (implementation)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares thread-safe Promise and Future asynchronous-result primitives.
/// @details A Promise publishes exactly one value or error, and its Future
///          exposes blocking, timed, polling, and completion-listener APIs.
///          Runtime-managed values are retained or transferred according to
///          the selected setter; returned owned results carry a reference that
///          callers must release or transfer.

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Zanna.Threads.Promise
//=============================================================================

/// @brief Create a new Promise.
/// @details The new Promise is pending and may be settled exactly once from
///          any thread. Allocation or synchronization initialization failure
///          raises a runtime trap.
/// @return New caller-owned opaque Promise object.
void *rt_promise_new(void);

/// @brief Get the Future associated with this Promise.
/// @details The Future can be passed to another thread to receive the result.
///          Calls share one live cached wrapper; after that wrapper is
///          finalized, a later call may create a replacement for the same
///          Promise state. Allocation and refcount failures trap.
/// @param promise Promise object pointer.
/// @return Associated Future object with a caller-owned reference.
void *rt_promise_get_future(void *promise);

/// @brief Complete the Promise with a value.
/// @details Retains a runtime-managed @p value for the Promise, wakes all
///          waiters, and invokes listeners in registration order. NULL is a
///          valid successful result. Subsequent completion attempts trap.
/// @param promise Promise object pointer.
/// @param value Runtime-managed result to retain, or NULL.
void rt_promise_set(void *promise, void *value);

/// @brief Complete the Promise with a retained runtime-managed value.
/// @details This explicit ownership spelling has the same retain semantics as
///          @ref rt_promise_set. The Promise releases its reference during
///          finalization. Duplicate completion balances the attempted retain
///          and traps.
/// @param promise Promise object pointer.
/// @param value Runtime-managed result object to retain, or NULL.
void rt_promise_set_owned(void *promise, void *value);

/// @brief Complete the Promise by transferring one existing value reference.
/// @details The promise owns @p value after this call and releases it from the
///          promise finalizer. Unlike rt_promise_set_owned, this does not
///          retain first; callers must only use it for callback results whose
///          reference is being handed to the Future. The input reference is
///          consumed even when duplicate completion traps.
/// @param promise Promise object pointer.
/// @param value Producer-owned runtime reference to transfer, or NULL.
void rt_promise_set_transferred(void *promise, void *value);

/// @brief Try to complete a producer-owned Promise by transferring one value reference.
/// @details This internal worker primitive never traps for duplicate completion:
///          when another producer has already settled the Promise, it releases
///          the transferred @p value and returns zero. On success the Promise
///          owns that reference until Future retrieval/finalization. The caller
///          must own a live Promise reference throughout the call; this function
///          does not consume it. Invalid handles are rejected without trapping.
/// @param promise Producer-owned Promise object pointer.
/// @param value Runtime-managed result reference being transferred, or NULL.
/// @return One when this call completed the Promise, otherwise zero; @p value is
///         consumed in either case and no invalid/duplicate-completion trap is
///         propagated.
int8_t rt_promise_try_set_transferred(void *promise, void *value);

/// @brief Complete the Promise with an error.
/// @details Copies the diagnostic, resolves the Future as an error, wakes
///          waiters, and invokes listeners. NULL becomes `"Unknown error"`.
///          Subsequent completion attempts trap.
/// @param promise Promise object pointer.
/// @param error Borrowed error-message String, or NULL.
void rt_promise_set_error(void *promise, rt_string error);

/// @brief Try to reject a producer-owned Promise from a native C string without propagating OOM.
/// @details This internal worker primitive copies @p error when allocation is
///          available. If the copy traps or returns NULL, it still atomically
///          completes the Promise as an error with no stored message; Future.Get
///          then uses its generic error diagnostic and Future.IsError remains
///          true. Unlike @ref rt_promise_set_error, an already-completed Promise
///          returns zero instead of trapping. The caller must own a live Promise
///          reference for the entire call, and the function does not consume it.
/// @param promise Producer-owned Promise object pointer.
/// @param error NUL-terminated diagnostic text, or NULL for a message-less error.
/// @return One when this call completed the Promise, zero when the handle was
///         invalid or another producer had already completed it; neither case
///         invokes the trap dispatcher.
int8_t rt_promise_try_set_error_cstr(void *promise, const char *error);

/// @brief Check if the Promise is already completed.
/// @details Returns a synchronized point-in-time snapshot and does not block.
///          NULL or invalid tolerant handles return zero.
/// @param promise Promise object pointer.
/// @return 1 if completed, 0 otherwise.
int8_t rt_promise_is_done(void *promise);

//=============================================================================
// Zanna.Threads.Future
//=============================================================================

/// @brief Get the value from the Future, blocking until resolved.
/// @details Blocks until the associated Promise is completed.
///          Error completion and native wait failures trap. Promise-owned
///          results are retained for the caller; raw borrowed results remain
///          borrowed, and a successful NULL value returns NULL.
/// @param future Future object pointer.
/// @return Successful result value, possibly NULL.
void *rt_future_get(void *future);

/// @brief Get the value with a timeout.
/// @details Blocks up to @p ms milliseconds. Successful Promise-owned results
///          are retained before being stored. Passing NULL for @p out checks
///          only successful completion. Non-positive timeouts poll immediately;
///          native wait failures trap.
/// @param future Future object pointer.
/// @param ms Timeout in milliseconds.
/// @param out Optional pointer to receive the successful result.
/// @return 1 if resolved with value, 0 if timed out or error.
int8_t rt_future_get_for(void *future, int64_t ms, void **out);

/// @brief Check if the Future is resolved.
/// @details Returns a synchronized point-in-time snapshot without blocking.
///          Either successful or error completion counts as resolved.
/// @param future Future object pointer.
/// @return 1 if resolved (value or error), 0 if still pending.
int8_t rt_future_is_done(void *future);

/// @brief Check if the Future resolved with an error.
/// @details Pending, successful, NULL, and invalid tolerant handles return zero.
/// @param future Future object pointer.
/// @return 1 if resolved with error, 0 otherwise.
int8_t rt_future_is_error(void *future);

/// @brief Get the error message if the Future resolved with an error.
/// @details An actual error is returned with a caller-owned String reference.
///          Pending, successful, invalid, and message-less errors yield the
///          constant empty String.
/// @param future Future object pointer.
/// @return Referenced error message, or a constant empty String.
rt_string rt_future_get_error(void *future);

/// @brief Try to get the value without blocking (IL-friendly).
/// @details Promise-owned successful values are retained for the caller.
///          NULL is ambiguous between pending, error, and a successful NULL
///          payload; use @ref rt_future_try_get_option when that matters.
/// @param future Future object pointer.
/// @return Successful value, or NULL when unavailable or itself NULL.
void *rt_future_try_get_val(void *future);

/// @brief Try to get the value without blocking as an Option.
/// @details Returns `Some(value)` when the future has resolved successfully and
///          `None` when it is still pending or resolved with an error. A
///          successful NULL value is represented as `Some(NULL)`. The returned
///          Option is caller-owned.
/// @param future Future object pointer.
/// @return Caller-owned opaque Zanna.Option object.
void *rt_future_try_get_option(void *future);

/// @brief Get the value with a timeout (IL-friendly).
/// @details Blocks up to @p ms milliseconds. Returns the value if resolved,
///          or NULL if timed out or resolved with error. Promise-owned results
///          are retained for the caller. Non-positive timeouts poll
///          immediately, and native wait failures trap.
/// @param future Future object pointer.
/// @param ms Timeout in milliseconds.
/// @return Successful value, or NULL on timeout, error, or a null result.
void *rt_future_get_for_val(void *future, int64_t ms);

/// @brief Try to get the value without blocking.
/// @details Promise-owned successful values are retained before storage. A
///          NULL @p out performs only a successful-completion check and can
///          therefore distinguish a successful NULL value.
/// @param future Future object pointer.
/// @param out Pointer to store the result value (can be NULL to just check).
/// @return 1 if resolved with value, 0 if pending or error.
int8_t rt_future_try_get(void *future, void **out);

/// @brief Wait for the Future to be resolved.
/// @details Blocks until either value or error completion without propagating
///          the stored error. Native wait failures trap.
/// @param future Future object pointer.
void rt_future_wait(void *future);

/// @brief Wait for the Future with a timeout.
/// @details Blocks up to @p ms milliseconds. Both successful and error
///          completion count as resolved; non-positive timeouts poll
///          immediately, and native wait failures trap.
/// @param future Future object pointer.
/// @param ms Timeout in milliseconds.
/// @return 1 if resolved, 0 if timed out.
int8_t rt_future_wait_for(void *future, int64_t ms);

/// @brief Internal extended listener hook with cancellation cleanup.
/// @details Like rt_future_on_complete, but also records an optional cleanup
///          callback that runs if the listener is removed before completion,
///          its Promise is abandoned, or the completion callback traps. An
///          already-completed Future invokes @p callback synchronously.
///          Callback and cleanup traps are swallowed.
/// @param future Future object pointer.
/// @param callback Listener receiving the completed @p future and @p ctx.
/// @param ctx Opaque listener context.
/// @param cancel Optional cleanup callback receiving @p ctx.
/// @return 1 on registration or synchronous invocation, 0 for invalid input
///         or listener allocation failure.
int8_t rt_future_on_complete_ex(void *future,
                                void (*callback)(void *future, void *ctx),
                                void *ctx,
                                void (*cancel)(void *ctx));

/// @brief Internal completion-listener hook used by async combinators.
/// @details Registers @p callback to run exactly once when @p future completes.
///          If the future is already complete, the callback runs synchronously
///          before this function returns.
/// @param future Future object pointer.
/// @param callback Listener callback receiving the completed future and @p ctx.
/// @param ctx Opaque listener context.
/// @return 1 on success, 0 on allocation failure or invalid input.
int8_t rt_future_on_complete(void *future, void (*callback)(void *future, void *ctx), void *ctx);

/// @brief Internal listener removal hook used by async combinators.
/// @details Removes the matching completion listener if it is still pending.
///          When removed, any cancellation cleanup registered with
///          rt_future_on_complete_ex is invoked exactly once behind a trap
///          boundary. The first callback/context match is removed.
/// @param future Future whose pending listener chain is searched.
/// @param callback Completion callback used as part of the listener key.
/// @param ctx Context pointer used as the other part of the listener key.
/// @return 1 if a listener was removed, 0 otherwise.
int8_t rt_future_cancel_listener(void *future,
                                 void (*callback)(void *future, void *ctx),
                                 void *ctx);

/// @brief Internal raw-value accessor for completed non-error futures.
/// @details Returns the stored value. If the promise owns that value, the
///          returned pointer is retained for the caller and must be released
///          after use. Borrowed values are returned without a retain. Intended
///          for runtime combinators that forward or inspect a value while
///          another future/promise may still own its lifetime.
/// @param future Future object pointer.
/// @return Stored value when the future is done and not errored; otherwise NULL.
void *rt_future_peek_value(void *future);

/// @brief Internal query for whether the stored value is promise-owned.
/// @details Retaining setters and ownership-transfer settlement create owned
///          values. Pending, errored, null-valued, and invalid Futures report
///          false.
/// @param future Future object pointer.
/// @return 1 when the stored value is promise-owned, otherwise 0.
int8_t rt_future_value_is_owned(void *future);

#ifdef __cplusplus
}
#endif
