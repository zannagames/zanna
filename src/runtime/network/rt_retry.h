//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/network/rt_retry.h
// Purpose: Retry policy with fixed or exponential backoff for transient-failure handling in
// network and I/O operations.
//
// Key invariants:
//   - Tracks attempt count and computes the appropriate delay for each strategy.
//   - Strategies: fixed (constant delay) and exponential (delay * 2^attempt, capped).
//   - Maximum attempt count caps retries; rt_retry_can_retry reports whether another remains.
//   - Exponential delays include 0-25% additive jitter.
//   - Attempt reservation is atomic, so concurrent callers consume distinct attempts.
//
// Ownership/Lifetime:
//   - Retry policy objects are runtime-managed heap objects.
//
// Links: src/runtime/network/rt_retry.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable managed-object class tag for RetryPolicy handles.
/// @details Public entry points validate this class and their minimum private
///          payload size before reading mutable retry state.
#define RT_RETRY_CLASS_ID INT64_C(-0x720203)

/// @brief Create a fixed-delay retry policy.
/// @details Negative retry counts and delays normalize to zero. Every
///          successfully reserved attempt returns the same normalized delay.
/// @param max_retries Maximum attempt reservations; zero disables retries.
/// @param base_delay_ms Constant delay in milliseconds.
/// @return Caller-owned managed RetryPolicy, or NULL on allocation failure.
void *rt_retry_new(int64_t max_retries, int64_t base_delay_ms);

/// @brief Create an exponential-backoff policy with bounded additive jitter.
/// @details Attempt @c n starts from `base_delay_ms * 2^n`, capped without
///          signed overflow, then receives zero-to-25-percent jitter without
///          exceeding the maximum. Policy-local immutable seed material makes
///          concurrent results deterministic by reserved attempt number.
/// @param max_retries Maximum attempt reservations; negatives normalize to
///        zero.
/// @param base_delay_ms Initial delay; negatives normalize to zero.
/// @param max_delay_ms Delay ceiling, normalized to at least the base.
/// @return Caller-owned managed RetryPolicy, or NULL on allocation failure.
void *rt_retry_exponential(int64_t max_retries, int64_t base_delay_ms, int64_t max_delay_ms);

/// @brief Check whether another retry attempt can currently be reserved.
/// @details This atomic snapshot does not consume an attempt and does not
///          guarantee a later reservation when callers race.
/// @param policy RetryPolicy receiver; NULL yields zero.
/// @return One when at least one retry remains; otherwise zero.
int8_t rt_retry_can_retry(void *policy);

/// @brief Atomically reserve an attempt and compute its delay.
/// @details Concurrent successful calls receive distinct zero-based attempts.
///          Exhaustion leaves the counter unchanged.
/// @param policy RetryPolicy receiver; NULL yields -1.
/// @return Nonnegative delay in milliseconds, or -1 when exhausted or invalid.
int64_t rt_retry_next_delay(void *policy);

/// @brief Read the number of attempts reserved in the current epoch.
/// @param policy RetryPolicy receiver; NULL yields zero.
/// @return Atomic count of successful reservations.
int64_t rt_retry_get_attempt(void *policy);

/// @brief Read the normalized immutable retry limit.
/// @param policy RetryPolicy receiver; NULL yields zero.
/// @return Maximum number of successful attempt reservations.
int64_t rt_retry_get_max_retries(void *policy);

/// @brief Atomically reset the attempt counter for policy reuse.
/// @details Callers needing a strict epoch boundary must externally coordinate
///          this operation with concurrent reservations.
/// @param policy RetryPolicy receiver; NULL is a no-op.
void rt_retry_reset(void *policy);

/// @brief Read the total attempts reserved since construction or reset.
/// @param policy RetryPolicy receiver; NULL yields zero.
/// @return Atomic attempt count, identical to @ref rt_retry_get_attempt.
int64_t rt_retry_get_total_attempts(void *policy);

/// @brief Check whether the policy has consumed its retry budget.
/// @param policy RetryPolicy receiver; NULL or invalid is treated as exhausted.
/// @return One when the attempt count reached the limit; otherwise zero.
int8_t rt_retry_is_exhausted(void *policy);

#ifdef __cplusplus
}
#endif
