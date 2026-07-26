//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/network/rt_ratelimit.h
// Purpose: Token bucket rate limiter for network and API operations, continuously refilling tokens
// based on elapsed time up to a maximum capacity.
//
// Key invariants:
//   - Tokens refill continuously using a monotonic clock when the platform provides one.
//   - Available tokens never exceed max capacity.
//   - rt_ratelimit_try_acquire returns immediately; returns 1 if tokens available, 0 otherwise.
//
// Ownership/Lifetime:
//   - Rate limiter objects are runtime-managed heap objects.
//
// Links: src/runtime/network/rt_ratelimit.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable managed-object class tag for RateLimiter handles.
/// @details Public entry points validate this class and their minimum private
///          payload size before interpreting an opaque object as bucket state.
#define RT_RATELIMIT_CLASS_ID INT64_C(-0x720204)

/// @brief Create a token bucket rate limiter.
/// @details The bucket starts full. Nonpositive capacities become one, while a
///          nonfinite or nonpositive refill rate becomes one token per second.
///          Whole-token state remains exact across the full signed 64-bit range.
/// @param max_tokens Requested maximum whole-token capacity.
/// @param refill_per_sec Requested continuously accrued tokens per second.
/// @return Owned rate limiter object, or NULL after an allocation trap.
void *rt_ratelimit_new(int64_t max_tokens, double refill_per_sec);

/// @brief Try to consume one token without blocking.
/// @details Applies elapsed monotonic-clock refill before delegating to the
///          all-or-nothing multi-token acquisition.
/// @param limiter Managed RateLimiter receiver.
/// @return One when a token was consumed, otherwise zero.
int8_t rt_ratelimit_try_acquire(void *limiter);

/// @brief Try to consume an exact number of tokens without blocking.
/// @details Refills first and either subtracts the complete request or leaves
///          the balance unchanged. NULL and nonpositive counts return zero.
/// @param limiter Managed RateLimiter receiver.
/// @param n Positive whole-token count.
/// @return One when all @p n tokens were consumed, otherwise zero.
int8_t rt_ratelimit_try_acquire_n(void *limiter, int64_t n);

/// @brief Get the exact available whole-token balance after refill.
/// @param limiter Managed RateLimiter receiver.
/// @return Available whole tokens, or zero for NULL or after a returning
///         invalid-handle trap.
int64_t rt_ratelimit_available(void *limiter);

/// @brief Reset the limiter to full capacity.
/// @details Clears fractional carry and restarts elapsed refill accounting at
///          the current monotonic timestamp. NULL is a no-op.
/// @param limiter Managed RateLimiter receiver.
void rt_ratelimit_reset(void *limiter);

/// @brief Get the limiter's exact whole-token capacity.
/// @param limiter Managed RateLimiter receiver.
/// @return Configured capacity, or zero for NULL or after a returning
///         invalid-handle trap.
int64_t rt_ratelimit_get_max(void *limiter);

/// @brief Get the configured refill rate.
/// @param limiter Managed RateLimiter receiver.
/// @return Tokens per second, or zero for NULL or after a returning
///         invalid-handle trap.
double rt_ratelimit_get_rate(void *limiter);

#ifdef __cplusplus
}
#endif
