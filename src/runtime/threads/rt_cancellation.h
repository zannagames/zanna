//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/threads/rt_cancellation.h
// Purpose: Cooperative cancellation token for async operations using atomic
//          operations for thread-safe state management, including linked parent
//          propagation and explicit reset for reuse.
//
// Key invariants:
//   - Thread-safe via atomic compare-exchange operations.
//   - IsCancelled reflects either the token's local state or any linked parent.
//   - Reset clears only the token's local cancelled bit.
//   - Multiple tokens can share a single cancellation source.
//   - rt_cancellation_is_cancelled can be polled from any thread.
//
// Ownership/Lifetime:
//   - Tokens are runtime-managed objects.
//   - Linked child tokens retain their parent token for the child's lifetime.
//
// Links: src/runtime/threads/rt_cancellation.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Cooperative cancellation-token C ABI with linked-parent propagation.
/// @details Cancellation is sticky until a token's local flag is reset. Linked
///          children retain their parent and report cancellation when either
///          their own flag or any ancestor's flag is set.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create an independent token in the non-cancelled state.
/// @return New runtime-managed token, or null after an allocation trap.
void *rt_cancellation_new(void);

/// @brief Poll a token and its linked ancestor chain.
/// @param token Cancellation token, or null.
/// @return 1 when local or inherited cancellation is requested, otherwise 0.
/// @note A nonnull object of the wrong runtime type raises a trap.
int8_t rt_cancellation_is_cancelled(void *token);

/// @brief Atomically request cancellation on a token.
/// @details Immediate linked children are marked under the parent's monitor;
///          deeper descendants observe the request through their parent chain.
/// @param token Cancellation token, or null for a no-op.
void rt_cancellation_cancel(void *token);

/// @brief Clear only a token's local cancellation flag for reuse.
/// @details A linked token remains observably cancelled while any ancestor is
///          still cancelled. Reset does not alter children or parents.
/// @param token Cancellation token, or null for a no-op.
void rt_cancellation_reset(void *token);

/// @brief Create a child that inherits cancellation from an optional parent.
/// @param parent Valid parent token, or null to create an independent token.
/// @return New runtime-managed token, already cancelled when necessary, or
///         null after invalid input or allocation failure.
void *rt_cancellation_linked(void *parent);

/// @brief Alias for @ref rt_cancellation_is_cancelled.
/// @param token Cancellation token, linked or independent, or null.
/// @return 1 if the token or an ancestor is cancelled, otherwise 0.
int8_t rt_cancellation_check(void *token);

/// @brief Raise an interrupt trap at a cooperative cancellation point.
/// @details Does nothing for null or non-cancelled tokens. Observable
///          cancellation raises `OperationCancelledException` using
///          `RT_TRAP_KIND_INTERRUPT`.
/// @param token Cancellation token, or null.
void rt_cancellation_throw_if_cancelled(void *token);

#ifdef __cplusplus
}
#endif
