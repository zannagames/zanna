//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_async.c
// Purpose: Implements high-level async task combinators for the Zanna.Threads.Async
//          class by composing Future/Promise, Thread, and Cancellation primitives.
//          Provides Run, All, Any, Delay, Map, and RunCancellable.
//
// Key invariants:
//   - Run/Delay/RunCancellable spawn background threads and resolve a Future.
//   - All/Any/Map register completion listeners instead of spinning or blocking
//     helper threads on other futures.
//   - Traps inside async callbacks are converted into Future errors.
//   - Promise/Future lifetimes are held explicitly while async work is in flight.
//
// Ownership/Lifetime:
//   - Callback arguments are forwarded as raw pointers; callers must keep raw
//     non-runtime pointers alive until the async callback consumes them.
//   - Async contexts own the promise reference they were created with.
//   - Listener states self-retain while callbacks are registered or executing.
//
// Links: src/runtime/threads/rt_async.h (public API),
//        src/runtime/threads/rt_future.h (Promise/Future primitives),
//        src/runtime/threads/rt_cancellation.h (cancellation token)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Thread-backed async execution and listener-backed Future combinators.
/// @details Worker callbacks run under trap recovery, while Map, Any, and All
///          attach completion listeners without consuming blocking worker
///          threads. Runtime-object ownership is retained explicitly for every
///          context and in-flight listener.

#include "rt_async.h"

#include "rt_cancellation.h"
#include "rt_future.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_threads.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_trap.h"

// =============================================================================
// Internal helpers — small lifetime-management primitives shared across the
// async combinators below. Each `release_*` helper handles "drop a reference if
// it's still alive AND clear the slot" in one call so the caller can't
// double-release. The `promise_error_*` helpers wrap the most common error path:
// "format a message, set the promise as an Err, release the temporary string."
// =============================================================================

/// @brief Release a runtime reference and clear its storage slot.
/// @param slot Address of a runtime-object pointer; null and empty slots are ignored.
static void async_release_ref(void **slot) {
    if (!slot || !*slot)
        return;
    if (rt_obj_release_check0(*slot))
        rt_obj_free(*slot);
    *slot = NULL;
}

/// @brief True if a @p count-element array of @p elem_size bytes can be
///        allocated without size_t overflow (rejects negative counts).
/// @param count Requested element count.
/// @param elem_size Nonzero size of each element in bytes.
/// @return 1 when the multiplication fits in @c size_t, otherwise 0.
static int async_count_fits_array(int64_t count, size_t elem_size) {
    return count >= 0 && elem_size > 0 && (uint64_t)count <= (uint64_t)SIZE_MAX / elem_size;
}

/// @brief Release the GC reference held by each of @p count source slots
///        (used when tearing down a combined/awaited source array).
/// @param sources Array of retained runtime-object pointers, or null.
/// @param count Number of slots to release.
static void async_release_source_array(void **sources, int64_t count) {
    if (!sources)
        return;
    for (int64_t i = 0; i < count; i++)
        async_release_ref(&sources[i]);
}

/// @brief Release a slot ONLY if its `owned` flag is set; clears both. Used so non-owned args
/// (caller keeps lifetime) aren't accidentally released when the worker context finalizes.
/// @param slot Address of the callback-argument pointer.
/// @param owned Address of the ownership flag.
static void async_release_owned_arg(void **slot, int8_t *owned) {
    if (!slot || !owned || !*owned)
        return;
    async_release_ref(slot);
    *owned = 0;
}

/// @brief Resolve a promise as Err using a C string literal (uses `rt_const_cstr` — no allocation).
/// Falls back to "Unknown error" if `msg` is NULL.
/// @param promise Destination Promise.
/// @param msg Borrowed error text, or null for the fallback.
static void async_promise_error_cstr(void *promise, const char *msg) {
    rt_promise_set_error(promise, rt_const_cstr(msg ? msg : "Unknown error"));
}

/// @brief Resolve a promise as Err with a heap-allocated copy of `msg` (or `fallback` if NULL).
/// Use when the source string's lifetime might end before the future's consumers see the error.
/// @param promise Destination Promise.
/// @param msg Preferred borrowed error text, or null.
/// @param fallback Secondary borrowed text, or null for `"Unknown error"`.
static void async_promise_error_copy(void *promise, const char *msg, const char *fallback) {
    const char *text = msg ? msg : fallback;
    if (!text) {
        async_promise_error_cstr(promise, "Unknown error");
        return;
    }
    rt_string copy = rt_string_from_bytes(text, strlen(text));
    rt_promise_set_error(promise, copy);
    rt_string_unref(copy);
}

/// @brief Resolve a promise as Err using the most recent trap message captured by the runtime
/// (via `rt_trap_get_error`). Used inside `setjmp` recovery branches to forward Zia-side traps
/// as Future errors instead of crashing the worker.
/// @param promise Destination Promise.
/// @param fallback Text used when the trap has no message.
static void async_promise_error_from_trap(void *promise, const char *fallback) {
    async_promise_error_copy(promise, rt_trap_get_error(), fallback);
}

/// @brief Forward the rejection reason from a source `future` onto `promise`.
/// @details Standard error-propagation primitive used by combinators (Map/Then/All/Any)
///          when the upstream future has rejected. Reads the rejected future's error
///          string (which bumps the rt_string refcount), assigns it to the destination
///          promise via `rt_promise_set_error`, and then drops the local reference so
///          ownership of the live string sits solely with the promise's stored copy.
/// @param promise Destination Promise.
/// @param future Rejected source Future.
static void async_promise_error_from_future(void *promise, void *future) {
    rt_string error = rt_future_get_error(future);
    rt_promise_set_error(promise, error);
    rt_string_unref(error);
}

/// @brief Resolve an async promise from a callback result.
/// @details Callback results are borrowed by default: the Future retains runtime
///          objects before publishing them. The only transfer case is owned-arg
///          passthrough, where the context already holds exactly the reference
///          that should become the Future's value.
/// @param promise Destination Promise.
/// @param result Callback return value, possibly null.
/// @param owned_arg_slot Address of the context argument slot, or null.
/// @param owns_arg Address of the argument-ownership flag, or null.
static void async_promise_set_callback_result(void *promise,
                                              void *result,
                                              void **owned_arg_slot,
                                              int8_t *owns_arg) {
    if (owned_arg_slot && owns_arg && *owns_arg && result == *owned_arg_slot) {
        *owned_arg_slot = NULL;
        *owns_arg = 0;
        rt_promise_set_transferred(promise, result);
        return;
    }
    if (owned_arg_slot && owns_arg && !*owns_arg && result == *owned_arg_slot) {
        rt_promise_set(promise, result);
        return;
    }
    rt_promise_set(promise, result);
}

/// @brief Spawn a detached worker thread on `entry(arg)`, releasing the thread handle immediately.
/// Returns 0 on thread-creation failure (caller should resolve the promise as Err in that case).
/// "Detached" means the runtime won't await the thread; cleanup happens via the worker's own
/// finalize path on the context object.
/// @param entry Worker entry point.
/// @param arg Opaque worker argument.
/// @return 1 after starting and releasing the thread handle, otherwise 0.
static int8_t async_start_detached(rt_thread_entry_fn entry, void *arg) {
    void *thread = rt_thread_start_fn(entry, arg);
    if (!thread)
        return 0;
    if (rt_obj_release_check0(thread))
        rt_obj_free(thread);
    return 1;
}

//=============================================================================
// Async.Run / Async.Delay / Async.RunCancellable
//=============================================================================

/// @brief Retained execution state for one `Async.Run` worker.
typedef struct {
    /// @brief Execute asynchronous work with one borrowed argument.
    /// @param arg Worker argument retained according to @ref owns_arg.
    /// @return Runtime value used to resolve the promise.
    void *(*callback)(void *);
    void *arg;
    int8_t owns_arg;
    void *promise;
} async_run_ctx;

/// @brief Retained execution state for one `Async.Delay` worker.
typedef struct {
    int64_t ms;
    void *promise;
} async_delay_ctx;

/// @brief Retained execution state for one cancellable worker.
typedef struct {
    /// @brief Execute asynchronous work with an argument and cancellation token.
    /// @param arg Worker argument retained according to @ref owns_arg.
    /// @param token Borrowed cancellation token.
    /// @return Runtime value used to resolve the promise.
    void *(*callback)(void *, void *);
    void *arg;
    int8_t owns_arg;
    void *token;
    void *promise;
} async_cancel_ctx;

// Worker context finalizers — release any owned arg + the promise reference. Each
// context is dual-retained at construction (one for the worker, one for the
// caller); these finalizers fire when both holders have released.

/// @brief GC finalizer for `Async.Run` worker context: release owned arg and promise.
/// @param obj Context object to finalize; null is ignored.
static void async_run_ctx_finalize(void *obj) {
    async_run_ctx *ctx = (async_run_ctx *)obj;
    if (!ctx)
        return;
    async_release_owned_arg(&ctx->arg, &ctx->owns_arg);
    async_release_ref(&ctx->promise);
}

/// @brief GC finalizer for `Async.Delay` worker context: release the promise reference.
/// @param obj Context object to finalize; null is ignored.
static void async_delay_ctx_finalize(void *obj) {
    async_delay_ctx *ctx = (async_delay_ctx *)obj;
    if (!ctx)
        return;
    async_release_ref(&ctx->promise);
}

/// @brief GC finalizer for `Async.RunCancellable` worker context: release owned arg, token,
/// promise.
/// @param obj Context object to finalize; null is ignored.
static void async_cancel_ctx_finalize(void *obj) {
    async_cancel_ctx *ctx = (async_cancel_ctx *)obj;
    if (!ctx)
        return;
    async_release_owned_arg(&ctx->arg, &ctx->owns_arg);
    async_release_ref(&ctx->token);
    async_release_ref(&ctx->promise);
}

/// @brief Worker thread entry for `Async.Run`. Sets up trap recovery via `setjmp`, invokes the
/// user callback inside the protected scope, and forwards either the result (resolve) or the
/// caught trap (reject) to the promise. Always releases the worker's context reference on exit.
/// **Trap-safety:** any Zia trap inside `callback` longjmps back to the recovery point; the
/// promise is then resolved as Err with the trap message rather than the worker crashing.
/// @param ctx_ptr Retained @c async_run_ctx worker reference.
static void async_run_entry(void *ctx_ptr) {
    async_run_ctx *ctx = (async_run_ctx *)ctx_ptr;
    if (!ctx || !ctx->callback) {
        if (ctx && ctx->promise)
            async_promise_error_cstr(ctx->promise, "Async.Run: nil callback");
        goto done;
    }

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        void *result = ctx->callback(ctx->arg);
        async_promise_set_callback_result(ctx->promise, result, &ctx->arg, &ctx->owns_arg);
    } else {
        async_promise_error_from_trap(ctx->promise, "Async.Run: task trapped");
    }
    rt_trap_clear_recovery();

done:
    if (ctx && rt_obj_release_check0(ctx))
        rt_obj_free(ctx);
}

/// @brief Worker thread entry for `Async.Delay`. Sleeps `ms` milliseconds (or skips on `ms<=0`),
/// then resolves the promise with NULL. No callback to trap-protect.
/// @param ctx_ptr Retained @c async_delay_ctx worker reference.
static void async_delay_entry(void *ctx_ptr) {
    async_delay_ctx *ctx = (async_delay_ctx *)ctx_ptr;
    if (!ctx || !ctx->promise)
        goto done;

    if (ctx->ms > 0)
        rt_thread_sleep(ctx->ms);
    rt_promise_set(ctx->promise, NULL);

done:
    if (ctx && rt_obj_release_check0(ctx))
        rt_obj_free(ctx);
}

/// @brief Worker thread entry for `Async.RunCancellable`. Same trap-recovery dance as
/// `async_run_entry` but checks the cancellation token both BEFORE invoking the callback
/// (early-out) AND AFTER (so a cooperative callback that observes cancellation but returns
/// normally still resolves as Err "cancelled" rather than Ok).
/// @param ctx_ptr Retained @c async_cancel_ctx worker reference.
static void async_cancel_entry(void *ctx_ptr) {
    async_cancel_ctx *ctx = (async_cancel_ctx *)ctx_ptr;
    if (!ctx || !ctx->callback) {
        if (ctx && ctx->promise)
            async_promise_error_cstr(ctx->promise, "Async.RunCancellable: nil callback");
        goto done;
    }

    if (ctx->token && rt_cancellation_is_cancelled(ctx->token)) {
        async_promise_error_cstr(ctx->promise, "cancelled");
        goto done;
    }

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        void *result = ctx->callback(ctx->arg, ctx->token);
        if (ctx->token && rt_cancellation_is_cancelled(ctx->token))
            async_promise_error_cstr(ctx->promise, "cancelled");
        else
            async_promise_set_callback_result(ctx->promise, result, &ctx->arg, &ctx->owns_arg);
    } else {
        async_promise_error_from_trap(ctx->promise, "Async.RunCancellable: task trapped");
    }
    rt_trap_clear_recovery();

done:
    if (ctx && rt_obj_release_check0(ctx))
        rt_obj_free(ctx);
}

/// @brief Implement raw and owned variants of asynchronous unary callback execution.
/// @param callback Nonnull function compatible with `void *(*)(void *)`.
/// @param arg Opaque callback argument.
/// @param retain_arg Nonzero to retain @p arg as a runtime object until completion.
/// @return Owned Future that resolves with the callback result or rejects on setup
///         failure or a captured trap; null only for a nil callback trap path.
static void *rt_async_run_impl(void *callback, void *arg, int8_t retain_arg) {
    if (!callback) {
        rt_trap("Async.Run: nil callback");
        return NULL;
    }

    void *promise = rt_promise_new();
    void *future = rt_promise_get_future(promise);
    async_run_ctx *ctx = (async_run_ctx *)rt_obj_new_i64(0, (int64_t)sizeof(async_run_ctx));
    if (!ctx) {
        async_promise_error_cstr(promise, "alloc failed");
        if (rt_obj_release_check0(promise))
            rt_obj_free(promise);
        return future;
    }

    ctx->callback = (void *(*)(void *))callback;
    ctx->arg = arg;
    ctx->owns_arg = (retain_arg && arg) ? 1 : 0;
    if (ctx->owns_arg)
        rt_obj_retain_maybe(arg);
    ctx->promise = promise; // Takes ownership of the creator reference.
    rt_obj_set_finalizer(ctx, async_run_ctx_finalize);

    rt_obj_retain_maybe(ctx); // Worker self-reference.
    if (!async_start_detached(async_run_entry, ctx)) {
        async_promise_error_cstr(promise, "Async.Run: failed to start thread");
        if (rt_obj_release_check0(ctx))
            rt_obj_free(ctx);
        if (rt_obj_release_check0(ctx))
            rt_obj_free(ctx);
        return future;
    }

    if (rt_obj_release_check0(ctx))
        rt_obj_free(ctx);
    return future;
}

/// @brief Run `callback(arg)` on a detached background thread.
/// @param callback Nonnull function compatible with `void *(*)(void *)`.
/// @param arg Borrowed opaque argument whose lifetime remains caller-managed.
/// @return Owned Future for the callback result; a nil callback traps and returns null.
void *rt_async_run(void *callback, void *arg) {
    return rt_async_run_impl(callback, arg, 0);
}

/// @brief Public API: run `callback(arg)` on a background thread, retaining `arg` so it survives
/// until the future resolves. Use when `arg` is a heap object and the caller might drop it.
/// @param callback Nonnull function compatible with `void *(*)(void *)`.
/// @param arg Runtime-managed argument retained through callback completion.
/// @return Owned Future for the callback result; a nil callback traps and returns null.
void *rt_async_run_owned(void *callback, void *arg) {
    return rt_async_run_impl(callback, arg, 1);
}

/// @brief Return a Future that resolves with null after a delay.
/// @param ms Delay in milliseconds; negative values are treated as zero.
/// @return Owned Future, rejected only if worker setup fails.
void *rt_async_delay(int64_t ms) {
    if (ms < 0)
        ms = 0;

    void *promise = rt_promise_new();
    void *future = rt_promise_get_future(promise);
    async_delay_ctx *ctx = (async_delay_ctx *)rt_obj_new_i64(0, (int64_t)sizeof(async_delay_ctx));
    if (!ctx) {
        async_promise_error_cstr(promise, "alloc failed");
        if (rt_obj_release_check0(promise))
            rt_obj_free(promise);
        return future;
    }

    ctx->ms = ms;
    ctx->promise = promise; // Takes ownership of the creator reference.
    rt_obj_set_finalizer(ctx, async_delay_ctx_finalize);

    rt_obj_retain_maybe(ctx); // Worker self-reference.
    if (!async_start_detached(async_delay_entry, ctx)) {
        async_promise_error_cstr(promise, "Async.Delay: failed to start thread");
        if (rt_obj_release_check0(ctx))
            rt_obj_free(ctx);
        if (rt_obj_release_check0(ctx))
            rt_obj_free(ctx);
        return future;
    }

    if (rt_obj_release_check0(ctx))
        rt_obj_free(ctx);
    return future;
}

/// @brief Implement raw and owned cancellable async callback variants.
/// @details The token is retained regardless of argument mode and checked
///          immediately before and after callback execution.
/// @param callback Nonnull function compatible with `void *(*)(void *, void *)`.
/// @param arg Opaque callback argument.
/// @param token Cancellation token to retain, or null.
/// @param retain_arg Nonzero to retain @p arg as a runtime object.
/// @return Owned Future for the callback result or rejection; a nil callback
///         traps and returns null.
static void *rt_async_run_cancellable_impl(void *callback,
                                           void *arg,
                                           void *token,
                                           int8_t retain_arg) {
    if (!callback) {
        rt_trap("Async.RunCancellable: nil callback");
        return NULL;
    }

    void *promise = rt_promise_new();
    void *future = rt_promise_get_future(promise);
    async_cancel_ctx *ctx =
        (async_cancel_ctx *)rt_obj_new_i64(0, (int64_t)sizeof(async_cancel_ctx));
    if (!ctx) {
        async_promise_error_cstr(promise, "alloc failed");
        if (rt_obj_release_check0(promise))
            rt_obj_free(promise);
        return future;
    }

    ctx->callback = (void *(*)(void *, void *))callback;
    ctx->arg = arg;
    ctx->owns_arg = (retain_arg && arg) ? 1 : 0;
    if (ctx->owns_arg)
        rt_obj_retain_maybe(arg);
    ctx->token = token;
    ctx->promise = promise; // Takes ownership of the creator reference.
    if (token)
        rt_obj_retain_maybe(token);
    rt_obj_set_finalizer(ctx, async_cancel_ctx_finalize);

    rt_obj_retain_maybe(ctx); // Worker self-reference.
    if (!async_start_detached(async_cancel_entry, ctx)) {
        async_promise_error_cstr(promise, "Async.RunCancellable: failed to start thread");
        if (rt_obj_release_check0(ctx))
            rt_obj_free(ctx);
        if (rt_obj_release_check0(ctx))
            rt_obj_free(ctx);
        return future;
    }

    if (rt_obj_release_check0(ctx))
        rt_obj_free(ctx);
    return future;
}

/// @brief Public API: cancellable async run. Caller manages `arg` lifetime; `token` is checked
/// before and after callback invocation so cooperative tasks can opt into early termination.
/// @param callback Nonnull function compatible with `void *(*)(void *, void *)`.
/// @param arg Borrowed opaque callback argument.
/// @param token Cancellation token, or null.
/// @return Owned Future that rejects as `"cancelled"` if the token is observed cancelled.
void *rt_async_run_cancellable(void *callback, void *arg, void *token) {
    return rt_async_run_cancellable_impl(callback, arg, token, 0);
}

/// @brief Public API: cancellable async run that retains `arg` for the worker's lifetime.
/// @param callback Nonnull function compatible with `void *(*)(void *, void *)`.
/// @param arg Runtime-managed argument retained through callback completion.
/// @param token Cancellation token, or null.
/// @return Owned Future that resolves or rejects with the worker outcome.
void *rt_async_run_cancellable_owned(void *callback, void *arg, void *token) {
    return rt_async_run_cancellable_impl(callback, arg, token, 1);
}

//=============================================================================
// Async.Map
//=============================================================================

/// @brief Retained listener state for one `Async.Map` transformation.
typedef struct {
    /// @brief Transform a completed value with a retained mapping argument.
    /// @param value Borrowed completed Future value.
    /// @param arg Mapping argument retained according to @ref owns_arg.
    /// @return Runtime value used to resolve the mapped promise.
    void *(*mapper)(void *, void *);
    void *arg;
    int8_t owns_arg;
    void *promise;
} async_map_state;

/// @brief GC finalizer for `Async.Map` listener state: release owned arg and promise.
/// @param obj Map listener state to finalize; null is ignored.
static void async_map_state_finalize(void *obj) {
    async_map_state *state = (async_map_state *)obj;
    if (!state)
        return;
    async_release_owned_arg(&state->arg, &state->owns_arg);
    async_release_ref(&state->promise);
}

/// @brief Listener callback fired when the source future completes. Forwards Err transparently;
/// for Ok runs the mapper inside trap-recovery and resolves the downstream promise with the
/// mapped value. Releases the listener-side state reference unconditionally on exit.
/// @param future Completed source Future.
/// @param ctx Retained @c async_map_state listener context.
static void async_map_complete(void *future, void *ctx) {
    async_map_state *state = (async_map_state *)ctx;
    if (!state || !state->promise)
        goto done;

    if (rt_future_is_error(future)) {
        async_promise_error_from_future(state->promise, future);
        goto done;
    }

    volatile int8_t source_owned = 0;
    void *volatile source_value = NULL;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        source_owned = rt_future_value_is_owned(future);
        source_value = rt_future_peek_value(future);
        void *mapped = state->mapper((void *)source_value, state->arg);
        if (mapped == source_value) {
            if (source_owned)
                rt_promise_set_owned(state->promise, mapped);
            else
                rt_promise_set(state->promise, mapped);
        } else {
            async_promise_set_callback_result(
                state->promise, mapped, &state->arg, &state->owns_arg);
        }
    } else {
        async_promise_error_from_trap(state->promise, "Async.Map: mapper trapped");
    }
    if (source_owned) {
        void *owned_source = (void *)source_value;
        async_release_ref(&owned_source);
        source_value = NULL;
    }
    rt_trap_clear_recovery();

done:
    if (state && rt_obj_release_check0(state))
        rt_obj_free(state);
}

/// @brief Implement raw and owned Future-mapping variants.
/// @details Completion registration is nonblocking. Upstream errors propagate
///          unchanged, while mapper traps become downstream Future errors.
/// @param future Nonnull source Future.
/// @param mapper Nonnull function compatible with `void *(*)(void *, void *)`.
/// @param arg Extra mapper argument.
/// @param retain_arg Nonzero to retain @p arg as a runtime object.
/// @return Owned downstream Future, or null after trapping on invalid inputs.
static void *rt_async_map_impl(void *future, void *mapper, void *arg, int8_t retain_arg) {
    if (!future || !mapper) {
        rt_trap("Async.Map: nil future or mapper");
        return NULL;
    }

    void *promise = rt_promise_new();
    void *result_future = rt_promise_get_future(promise);
    async_map_state *state = (async_map_state *)rt_obj_new_i64(0, (int64_t)sizeof(async_map_state));
    if (!state) {
        async_promise_error_cstr(promise, "alloc failed");
        if (rt_obj_release_check0(promise))
            rt_obj_free(promise);
        return result_future;
    }

    state->mapper = (void *(*)(void *, void *))mapper;
    state->arg = arg;
    state->owns_arg = (retain_arg && arg) ? 1 : 0;
    if (state->owns_arg)
        rt_obj_retain_maybe(arg);
    state->promise = promise; // Takes ownership of the creator reference.
    rt_obj_set_finalizer(state, async_map_state_finalize);

    rt_obj_retain_maybe(state); // Completion callback reference.
    if (!rt_future_on_complete(future, async_map_complete, state)) {
        async_promise_error_cstr(promise, "Async.Map: failed to register completion");
        if (rt_obj_release_check0(state))
            rt_obj_free(state);
        if (rt_obj_release_check0(state))
            rt_obj_free(state);
        return result_future;
    }

    if (rt_obj_release_check0(state))
        rt_obj_free(state);
    return result_future;
}

/// @brief Chain a mapper onto an existing Future with a borrowed extra argument.
/// @param future Source Future.
/// @param mapper Function compatible with `void *(*)(void *, void *)`.
/// @param arg Borrowed extra mapper argument.
/// @return Owned downstream Future, or null after an invalid-input trap.
void *rt_async_map(void *future, void *mapper, void *arg) {
    return rt_async_map_impl(future, mapper, arg, 0);
}

/// @brief Public API: chain a mapper, retaining `arg` until the result future resolves.
/// @param future Source Future.
/// @param mapper Function compatible with `void *(*)(void *, void *)`.
/// @param arg Runtime-managed extra argument retained until mapper completion.
/// @return Owned downstream Future, or null after an invalid-input trap.
void *rt_async_map_owned(void *future, void *mapper, void *arg) {
    return rt_async_map_impl(future, mapper, arg, 1);
}

//=============================================================================
// Async.Any
//=============================================================================

/// @brief Shared race state for one `Async.Any` operation.
typedef struct {
    void *promise;
    void *monitor;
    void **sources;
    int64_t count;
    int8_t completed;
} async_any_state;

/// @brief Process one source completion for an `Async.Any` race.
/// @param future Source Future that completed.
/// @param ctx Retained @c async_any_state listener context.
static void async_any_complete(void *future, void *ctx);

// ----- Any state machine ----------------------------------------------------
// `Async.Any` registers a completion listener on every input future. The first
// future to resolve (Ok or Err) wins via the `completed` flag (monitor-guarded
// to keep concurrent completions race-free). Remaining listeners are then
// cancelled in-place to avoid spurious callbacks. Each listener takes an extra
// state retain at registration; the cancel hook drops it.

/// @brief GC finalizer for `Async.Any` state: free the sources array, release monitor + promise.
/// @param obj Any-combinator state to finalize; null is ignored.
static void async_any_state_finalize(void *obj) {
    async_any_state *state = (async_any_state *)obj;
    if (!state)
        return;
    async_release_source_array(state->sources, state->count);
    free(state->sources);
    state->sources = NULL;
    async_release_ref(&state->monitor);
    async_release_ref(&state->promise);
}

/// @brief Listener-cancel hook for `Async.Any`: drop the per-listener retain on the state.
/// @param ctx Retained @c async_any_state listener context.
static void async_any_listener_cancel(void *ctx) {
    async_any_state *state = (async_any_state *)ctx;
    if (state && rt_obj_release_check0(state))
        rt_obj_free(state);
}

/// @brief Walk the remaining sources and request listener cancellation on each. Sources whose
/// listeners cancel successfully are zeroed in the array (so finalize doesn't double-cancel).
/// @param state Any-combinator state whose outstanding listeners are cancelled.
static void async_any_cancel_remaining(async_any_state *state) {
    if (!state || !state->sources)
        return;
    for (int64_t i = 0; i < state->count; i++) {
        void *source = state->sources[i];
        if (!source)
            continue;
        if (rt_future_cancel_listener(source, async_any_complete, state))
            async_release_ref(&state->sources[i]);
    }
}

/// @brief Listener fired when any input future resolves. Acquires the monitor to flip `completed`
/// atomically (only the first arrival wins). On win, forwards Ok value (preserving owned-ness) or
/// Err to the result promise, then cancels remaining listeners. Always releases the listener-side
/// state retain on exit.
/// @param future Source Future that completed.
/// @param ctx Retained @c async_any_state listener context.
static void async_any_complete(void *future, void *ctx) {
    async_any_state *state = (async_any_state *)ctx;
    if (!state || !state->promise || !state->monitor)
        goto done;

    int8_t should_resolve = 0;
    rt_monitor_enter(state->monitor);
    if (!state->completed) {
        state->completed = 1;
        should_resolve = 1;
    }
    rt_monitor_exit(state->monitor);

    if (!should_resolve)
        goto done;

    int8_t forward_failed = 0;
    volatile int8_t value_owned = 0;
    void *volatile value = NULL;
    char forward_error[256];
    forward_error[0] = '\0';

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        if (rt_future_is_error(future)) {
            async_promise_error_from_future(state->promise, future);
        } else {
            value_owned = rt_future_value_is_owned(future);
            value = rt_future_peek_value(future);
            if (value_owned)
                rt_promise_set_owned(state->promise, (void *)value);
            else
                rt_promise_set(state->promise, (void *)value);
        }
    } else {
        const char *err = rt_trap_get_error();
        snprintf(forward_error,
                 sizeof(forward_error),
                 "%s",
                 err && err[0] ? err : "Async.Any: failed to forward result");
        forward_failed = 1;
    }
    rt_trap_clear_recovery();

    if (value_owned) {
        void *owned_value = (void *)value;
        async_release_ref(&owned_value);
        value = NULL;
    }

    if (forward_failed)
        async_promise_error_copy(
            state->promise, forward_error, "Async.Any: failed to forward result");

    async_any_cancel_remaining(state);

done:
    if (state && rt_obj_release_check0(state))
        rt_obj_free(state);
}

/// @brief Resolve with the first input Future to complete, whether Ok or Err.
/// @details Registers one listener per source and cancels outstanding listeners
///          after the monitor-guarded winner is selected. Empty input returns an
///          already-rejected Future.
/// @param futures Sequence of Future handles.
/// @return Owned Future forwarding the first completion or a setup error.
void *rt_async_any(void *futures) {
    void *promise = rt_promise_new();
    void *future = rt_promise_get_future(promise);

    int64_t count = futures ? rt_seq_len(futures) : 0;
    if (count == 0) {
        async_promise_error_cstr(promise, "Async.Any: empty futures");
        if (rt_obj_release_check0(promise))
            rt_obj_free(promise);
        return future;
    }
    if (!async_count_fits_array(count, sizeof(void *))) {
        async_promise_error_cstr(promise, "Async.Any: futures count too large");
        if (rt_obj_release_check0(promise))
            rt_obj_free(promise);
        return future;
    }

    async_any_state *state = (async_any_state *)rt_obj_new_i64(0, (int64_t)sizeof(async_any_state));
    if (!state) {
        async_promise_error_cstr(promise, "alloc failed");
        if (rt_obj_release_check0(promise))
            rt_obj_free(promise);
        return future;
    }

    state->promise = promise; // Takes ownership of the creator reference.
    state->monitor = rt_obj_new_i64(0, 1);
    state->sources = (void **)calloc((size_t)count, sizeof(void *));
    state->count = count;
    state->completed = 0;
    rt_obj_set_finalizer(state, async_any_state_finalize);
    if (!state->monitor || !state->sources) {
        async_promise_error_cstr(promise, "Async.Any: failed to allocate state");
        if (rt_obj_release_check0(state))
            rt_obj_free(state);
        return future;
    }

    int8_t registration_failed = 0;
    for (int64_t i = 0; i < count; i++) {
        void *source = rt_seq_get(futures, i);
        rt_obj_retain_maybe(source);
        state->sources[i] = source;
        rt_obj_retain_maybe(state);
        if (!rt_future_on_complete_ex(
                source, async_any_complete, state, async_any_listener_cancel)) {
            if (rt_obj_release_check0(state))
                rt_obj_free(state);
            async_release_ref(&state->sources[i]);
            registration_failed = 1;
            break;
        }
        int8_t already_completed = 0;
        rt_monitor_enter(state->monitor);
        already_completed = state->completed;
        rt_monitor_exit(state->monitor);
        if (already_completed)
            break;
    }

    if (registration_failed) {
        int8_t should_resolve = 0;
        rt_monitor_enter(state->monitor);
        if (!state->completed) {
            state->completed = 1;
            should_resolve = 1;
        }
        rt_monitor_exit(state->monitor);
        if (should_resolve)
            async_promise_error_cstr(promise, "Async.Any: failed to register completion");
        async_any_cancel_remaining(state);
    }

    if (rt_obj_release_check0(state))
        rt_obj_free(state);
    return future;
}

//=============================================================================
// Async.All
//=============================================================================

/// @brief Shared aggregation state for one `Async.All` operation.
typedef struct {
    void *promise;
    void *monitor;
    void *results;
    void **sources;
    struct async_all_listener_ctx **listeners;
    int64_t slot_count;
    int64_t remaining;
    int8_t completed;
} async_all_state;

/// @brief Per-source index and shared-state link used by `Async.All` listeners.
typedef struct async_all_listener_ctx {
    async_all_state *state;
    int64_t index;
} async_all_listener_ctx;

/// @brief Process one indexed source completion for an `Async.All` aggregation.
/// @param future Source Future that completed.
/// @param ctx Consumed per-source @c async_all_listener_ctx.
static void async_all_complete(void *future, void *ctx);

// ----- All state machine ----------------------------------------------------
// `Async.All` waits for every input future. Each slot tracks: the source pointer
// (for cancellation), the per-listener context (so cancel hooks can drop the
// state retain), and a result sequence that owns deposited runtime objects. The
// `remaining` counter tracks unfilled slots; reaching zero resolves the result Seq.

/// @brief GC finalizer for `Async.All` state: free helper arrays, release monitor + result Seq +
/// promise. The result Seq owns and releases any runtime-managed values deposited into it.
/// @param obj All-combinator state to finalize; null is ignored.
static void async_all_state_finalize(void *obj) {
    async_all_state *state = (async_all_state *)obj;
    if (!state)
        return;
    async_release_source_array(state->sources, state->slot_count);
    free(state->listeners);
    state->listeners = NULL;
    free(state->sources);
    state->sources = NULL;
    async_release_ref(&state->results);
    async_release_ref(&state->monitor);
    async_release_ref(&state->promise);
}

/// @brief Listener-cancel hook for `Async.All`: drop the state retain and free the per-listener
/// context node (the listener is one heap object per source future).
/// @param ctx Heap-allocated @c async_all_listener_ctx to consume.
static void async_all_listener_cancel(void *ctx) {
    async_all_listener_ctx *listener = (async_all_listener_ctx *)ctx;
    async_all_state *state = listener ? listener->state : NULL;
    if (state && rt_obj_release_check0(state))
        rt_obj_free(state);
    free(listener);
}

/// @brief Cancel listeners on every source except the one at `skip_index` (typically the source
/// that just resolved). Zeros the listener slot on successful cancel so finalize doesn't
/// double-free.
/// @param state All-combinator state whose outstanding listeners are cancelled.
/// @param skip_index Source slot to leave untouched, or -1 to cancel every slot.
static void async_all_cancel_remaining(async_all_state *state, int64_t skip_index) {
    if (!state || !state->sources || !state->listeners)
        return;
    for (int64_t i = 0; i < state->slot_count; i++) {
        if (i == skip_index || !state->listeners[i] || !state->sources[i])
            continue;
        if (rt_future_cancel_listener(state->sources[i], async_all_complete, state->listeners[i])) {
            state->listeners[i] = NULL;
            async_release_ref(&state->sources[i]);
        }
    }
}

/// @brief Listener fired when one of the input futures resolves. **Err short-circuits:** the
/// first Err propagates to the result and cancels remaining listeners. **Ok accumulates:** value
/// is deposited at the per-listener index in the result Seq, which retains runtime objects, and
/// `remaining--`. When `remaining == 0` the result Seq is sent to the promise as Owned. Frees the
/// listener context unconditionally.
/// @param future Source Future that completed.
/// @param ctx Consumed per-source @c async_all_listener_ctx.
static void async_all_complete(void *future, void *ctx) {
    async_all_listener_ctx *listener = (async_all_listener_ctx *)ctx;
    async_all_state *state = listener ? listener->state : NULL;
    if (!state || !state->promise || !state->monitor || !state->results)
        goto done;

    int8_t should_process = 0;
    rt_monitor_enter(state->monitor);
    state->listeners[listener->index] = NULL;
    if (!state->completed)
        should_process = 1;
    rt_monitor_exit(state->monitor);
    if (!should_process)
        goto done;

    if (rt_future_is_error(future)) {
        int8_t should_resolve = 0;
        rt_monitor_enter(state->monitor);
        if (!state->completed) {
            state->completed = 1;
            should_resolve = 1;
        }
        rt_monitor_exit(state->monitor);
        if (should_resolve) {
            async_promise_error_from_future(state->promise, future);
            async_all_cancel_remaining(state, listener->index);
        }
        goto done;
    }

    int8_t resolve_success = 0;
    int8_t resolve_copy_error = 0;
    int8_t value_owned = 0;
    void *value = NULL;
    char copy_error[256];
    copy_error[0] = '\0';

    jmp_buf peek_recovery;
    rt_trap_set_recovery(&peek_recovery);
    if (setjmp(peek_recovery) == 0) {
        value_owned = rt_future_value_is_owned(future);
        value = rt_future_peek_value(future);
    } else {
        const char *err = rt_trap_get_error();
        snprintf(copy_error,
                 sizeof(copy_error),
                 "%s",
                 err && err[0] ? err : "Async.All: failed to copy result");
        resolve_copy_error = 1;
    }
    rt_trap_clear_recovery();

    if (!resolve_copy_error) {
        rt_monitor_enter(state->monitor);
        if (!state->completed) {
            jmp_buf set_recovery;
            rt_trap_set_recovery(&set_recovery);
            if (setjmp(set_recovery) == 0) {
                rt_seq_set(state->results, listener->index, value);
                state->remaining--;
                if (state->remaining == 0) {
                    state->completed = 1;
                    resolve_success = 1;
                }
            } else {
                const char *err = rt_trap_get_error();
                snprintf(copy_error,
                         sizeof(copy_error),
                         "%s",
                         err && err[0] ? err : "Async.All: failed to store result");
                state->completed = 1;
                resolve_copy_error = 1;
            }
            rt_trap_clear_recovery();
        }
        rt_monitor_exit(state->monitor);
    }

    if (value_owned)
        async_release_ref(&value);

    if (resolve_copy_error) {
        async_promise_error_copy(state->promise, copy_error, "Async.All: failed to copy result");
        async_all_cancel_remaining(state, listener->index);
        goto done;
    }

    if (resolve_success)
        rt_promise_set_owned(state->promise, state->results);

done:
    if (state && rt_obj_release_check0(state))
        rt_obj_free(state);
    free(listener);
}

/// @brief Resolve with all input results in source order or reject on the first error.
/// @details Completion listeners populate an owning result sequence by index.
///          Empty input resolves immediately with an empty sequence; any
///          registration, allocation, copy, or source error rejects the result
///          and cancels remaining listeners.
/// @param futures Sequence of Future handles.
/// @return Owned Future resolving to an owned results sequence or rejecting.
void *rt_async_all(void *futures) {
    void *promise = rt_promise_new();
    void *future = rt_promise_get_future(promise);

    int64_t count = futures ? rt_seq_len(futures) : 0;
    if (count == 0) {
        void *results = rt_seq_new();
        rt_promise_set_owned(promise, results);
        if (rt_obj_release_check0(results))
            rt_obj_free(results);
        if (rt_obj_release_check0(promise))
            rt_obj_free(promise);
        return future;
    }
    if (!async_count_fits_array(count, sizeof(void *)) ||
        !async_count_fits_array(count, sizeof(async_all_listener_ctx *))) {
        async_promise_error_cstr(promise, "Async.All: futures count too large");
        if (rt_obj_release_check0(promise))
            rt_obj_free(promise);
        return future;
    }

    async_all_state *state = (async_all_state *)rt_obj_new_i64(0, (int64_t)sizeof(async_all_state));
    if (!state) {
        async_promise_error_cstr(promise, "alloc failed");
        if (rt_obj_release_check0(promise))
            rt_obj_free(promise);
        return future;
    }

    state->promise = promise; // Takes ownership of the creator reference.
    state->monitor = rt_obj_new_i64(0, 1);
    state->results = rt_seq_with_capacity(count);
    state->sources = (void **)calloc((size_t)count, sizeof(void *));
    state->listeners =
        (async_all_listener_ctx **)calloc((size_t)count, sizeof(async_all_listener_ctx *));
    state->slot_count = count;
    state->remaining = count;
    state->completed = 0;
    rt_obj_set_finalizer(state, async_all_state_finalize);
    if (state->results)
        rt_seq_set_owns_elements(state->results, 1);
    if (!state->monitor || !state->results || !state->sources || !state->listeners) {
        async_promise_error_cstr(promise, "Async.All: failed to allocate state");
        if (rt_obj_release_check0(state))
            rt_obj_free(state);
        return future;
    }

    for (int64_t i = 0; i < count; i++)
        rt_seq_push_raw(state->results, NULL);

    int8_t registration_failed = 0;
    for (int64_t i = 0; i < count; i++) {
        async_all_listener_ctx *listener =
            (async_all_listener_ctx *)calloc(1, sizeof(async_all_listener_ctx));
        if (!listener) {
            registration_failed = 1;
            break;
        }
        listener->state = state;
        listener->index = i;
        state->sources[i] = rt_seq_get(futures, i);
        rt_obj_retain_maybe(state->sources[i]);
        state->listeners[i] = listener;

        rt_obj_retain_maybe(state);
        if (!rt_future_on_complete_ex(
                state->sources[i], async_all_complete, listener, async_all_listener_cancel)) {
            if (rt_obj_release_check0(state))
                rt_obj_free(state);
            state->listeners[i] = NULL;
            async_release_ref(&state->sources[i]);
            free(listener);
            registration_failed = 1;
            break;
        }
        int8_t already_completed = 0;
        rt_monitor_enter(state->monitor);
        already_completed = state->completed;
        rt_monitor_exit(state->monitor);
        if (already_completed)
            break;
    }

    if (registration_failed) {
        int8_t should_resolve = 0;
        rt_monitor_enter(state->monitor);
        if (!state->completed) {
            state->completed = 1;
            should_resolve = 1;
        }
        rt_monitor_exit(state->monitor);
        if (should_resolve) {
            async_promise_error_cstr(promise, "Async.All: failed to register completion");
            async_all_cancel_remaining(state, -1);
        }
    }

    if (rt_obj_release_check0(state))
        rt_obj_free(state);
    return future;
}
