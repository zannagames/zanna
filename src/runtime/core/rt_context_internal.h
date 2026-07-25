//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_context_internal.h
/// @file
/// @brief Declares internal context-state locking and thread-binding handoff.
///
// Purpose: Internal context-binding handoff used by the platform thread
//   adapters to reserve an inherited context before an OS thread can run.
//
// Key invariants:
//   - Each successful reservation is consumed exactly once by a child thread
//     or canceled exactly once by the parent after native creation fails.
//   - A consumed reservation becomes the child's TLS binding without a second
//     bind-count increment; normal unbinding releases that same count.
//   - These hooks are runtime-internal and are not Zanna language ABI entries.
//
// Ownership/Lifetime:
//   - The reservation is a lifetime claim on caller-owned RtContext storage.
//     It does not allocate or own the RtContext itself.
//
// Links: rt_context.c, rt_context.h, ../threads/rt_threads_posix.c,
//        ../threads/rt_threads_win.c,
//        docs/adr/0136-runtime-context-binding-lifecycle-and-state-locks.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Mutable context subsystems with independently serialized state.
typedef enum rt_context_state_kind {
    RT_CONTEXT_STATE_RNG = 0,
    RT_CONTEXT_STATE_MODVAR = 1,
    RT_CONTEXT_STATE_FILE = 2,
    RT_CONTEXT_STATE_ARGS = 3,
    RT_CONTEXT_STATE_LIFECYCLE = 4
} rt_context_state_kind_t;

/// @brief Resolve and lock the calling thread's effective runtime context.
/// @details A bound context is protected directly. For an unbound thread, the
///          legacy initialization/handoff state is validated while its lifecycle
///          lock is acquired, preventing first/last-binding migration or shutdown
///          from invalidating the selected subsystem. Locks are recursive for
///          internal helper composition and are tracked for trap unwinding.
/// @param kind Mutable subsystem to serialize.
/// @param is_legacy Optional output set to non-zero for the process legacy context.
/// @return Locked effective context, or NULL after a recoverable initialization failure.
RtContext *rt_context_acquire_state(rt_context_state_kind_t kind, int *is_legacy);

/// @brief Lock one known-live context subsystem directly.
/// @details Used when a caller already owns a binding/lifetime reservation and
///          therefore does not need legacy-context resolution.
/// @param ctx Initialized context with live mutex storage.
/// @param kind Mutable subsystem to serialize.
/// @note Lock nesting is tracked per thread for trap unwinding. Invalid lock
///       storage or nesting beyond the fixed internal capacity aborts.
void rt_context_state_lock(RtContext *ctx, rt_context_state_kind_t kind);

/// @brief Release one context subsystem lock acquired by this thread.
/// @param ctx Context returned to or supplied by the matching acquire operation.
/// @param kind Subsystem passed to the matching lock operation.
/// @note Nonempty acquisitions must be released in strict LIFO order; an order
///       mismatch aborts. Calling with no tracked lock is a no-op.
void rt_context_release_state(RtContext *ctx, rt_context_state_kind_t kind);

/// @brief Abandon all context-state locks before a trap performs `longjmp`.
/// @details The central trap dispatcher invokes this immediately before its
///          non-local transfer because lexical unlock calls will be skipped.
///          Returning trap hooks retain lexical ownership and do not call it.
void rt_context_state_abort_for_trap(void);

/// @brief Acquire the type registry's write lock for a state migration.
/// @details Implemented by the OOP component; the base runtime supplies a weak
///          no-op when that component is not linked.
/// @param ctx Context whose registry arrays will be transferred.
void rt_type_registry_state_write_lock(RtContext *ctx);

/// @brief Release a type-registry migration write lock.
/// @param ctx Context passed to @ref rt_type_registry_state_write_lock.
void rt_type_registry_state_write_unlock(RtContext *ctx);

/// @brief Reserve an inherited-context binding before native thread creation.
/// @details Increments the context binding count transactionally and performs
///          first-binding legacy-state adoption when required. The reservation
///          prevents context cleanup between a successful native create call
///          and execution of the child trampoline.
/// @param ctx Initialized context that the child will inherit; must not be NULL.
/// @return Non-zero on success. Returns zero after trapping if initialization
///         fails, the argument is NULL, the context is not lifecycle-ready, or
///         the binding count would overflow.
/// @post A successful call must be paired with exactly one call to either
///       @ref rt_context_adopt_reserved_thread_binding or
///       @ref rt_context_cancel_reserved_thread_binding.
int rt_context_reserve_thread_binding(RtContext *ctx);

/// @brief Consume a reserved binding on the newly created child thread.
/// @details Publishes @p ctx as the calling thread's active TLS context without
///          incrementing its binding count again. This is valid only in a
///          native thread trampoline holding one reservation created by
///          @ref rt_context_reserve_thread_binding.
/// @param ctx Context named by the outstanding reservation; must not be NULL.
/// @return Non-zero when TLS was installed. Returns zero after trapping if the
///         caller already has a context or no live binding exists.
/// @post On success, the child releases the consumed reservation through the
///       ordinary `rt_set_current_context(NULL)` unbind path.
int rt_context_adopt_reserved_thread_binding(RtContext *ctx);

/// @brief Cancel a reservation after native thread creation fails.
/// @details Decrements the reserved binding and performs last-binding state
///          handoff to the legacy context when necessary. This function does
///          not modify the calling thread's TLS binding.
/// @param ctx Context passed to the successful reservation call.
/// @return Non-zero on success. Returns zero after trapping if @p ctx is NULL
///         or its binding count is already zero.
int rt_context_cancel_reserved_thread_binding(RtContext *ctx);

#ifdef __cplusplus
}
#endif
