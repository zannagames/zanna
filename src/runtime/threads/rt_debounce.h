//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/threads/rt_debounce.h
// Purpose: Debouncer and Throttler utilities for rate-limiting operations: Debouncer delays
// execution until a quiet period elapses; Throttler limits to at most once per interval.
//
// Key invariants:
//   - Debouncer resets its timer on each call; fires only after quiet_ms have passed.
//   - Throttler fires at most once per interval_ms regardless of call frequency.
//   - Both utilities are time-based using the monotonic clock.
//   - Mutable state is synchronized internally so instances can be shared across threads.
//
// Ownership/Lifetime:
//   - Debouncer and Throttler objects are heap-allocated runtime objects.
//
// Links: src/runtime/threads/rt_debounce.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Passive, thread-safe debouncer and throttler state APIs.
/// @details These objects never store callbacks or schedule work. Debouncers
///          record signals for readiness polling; throttlers atomically decide
///          whether a caller may proceed at the current monotonic time.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Debouncer ---

/// @brief Create a new debouncer with the given delay in milliseconds.
/// @param delay_ms Quiet period in milliseconds; negative values become zero.
/// @return New runtime-managed Debouncer, or null after an allocation trap.
void *rt_debounce_new(int64_t delay_ms);

/// @brief Signal the debouncer (resets the timer).
/// @details Records the current time, marks the instance signalled, and
///          increments a count that saturates at @c INT64_MAX.
/// @param debouncer Debouncer handle; null is ignored.
void rt_debounce_signal(void *debouncer);

/// @brief Check if the debouncer has settled (delay elapsed since last signal).
/// @details A newly created or reset Debouncer is not ready until signalled,
///          even when its delay is zero.
/// @param debouncer Debouncer handle; null is not ready.
/// @return 1 after the quiet period, otherwise 0.
int8_t rt_debounce_is_ready(void *debouncer);

/// @brief Reset the debouncer to initial state.
/// @details Clears its timestamp, signal marker, and signal count.
/// @param debouncer Debouncer handle; null is ignored.
void rt_debounce_reset(void *debouncer);

/// @brief Get the configured delay in milliseconds.
/// @param debouncer Debouncer handle; null reports zero.
/// @return Nonnegative delay in milliseconds.
int64_t rt_debounce_get_delay(void *debouncer);

/// @brief Get the number of signals since construction or the last reset.
/// @details Readiness checks do not reset this saturating diagnostic counter.
/// @param debouncer Debouncer handle; null reports zero.
/// @return Signal count.
int64_t rt_debounce_get_signal_count(void *debouncer);

// --- Throttler ---

/// @brief Create a new throttler with the given interval in milliseconds.
/// @param interval_ms Minimum interval in milliseconds; negative values become zero.
/// @return New runtime-managed Throttler, or null after an allocation trap.
void *rt_throttle_new(int64_t interval_ms);

/// @brief Check if an operation is allowed (and mark as executed if so).
/// @details The first call always succeeds. Later successful calls update the
///          timestamp and increment a count that saturates at @c INT64_MAX.
/// @param throttler Throttler handle; null is rejected.
/// @return 1 if the slot was atomically claimed, otherwise 0.
int8_t rt_throttle_try(void *throttler);

/// @brief Check if an operation would be allowed (without marking).
/// @param throttler Throttler handle; null is rejected.
/// @return 1 before the first claim or after the interval, otherwise 0.
int8_t rt_throttle_can_proceed(void *throttler);

/// @brief Reset the throttler to allow immediate operation.
/// @details Clears the last-allowed timestamp and successful-operation count.
/// @param throttler Throttler handle; null is ignored.
void rt_throttle_reset(void *throttler);

/// @brief Get the configured interval in milliseconds.
/// @param throttler Throttler handle; null reports zero.
/// @return Nonnegative interval in milliseconds.
int64_t rt_throttle_get_interval(void *throttler);

/// @brief Get the number of operations allowed so far.
/// @param throttler Throttler handle; null reports zero.
/// @return Saturating count of successful @ref rt_throttle_try calls since reset.
int64_t rt_throttle_get_count(void *throttler);

/// @brief Get time remaining until next operation is allowed, in ms.
/// @param throttler Throttler handle; null is treated as ready.
/// @return Nonnegative milliseconds remaining, or zero if ready now.
int64_t rt_throttle_remaining_ms(void *throttler);

#ifdef __cplusplus
}
#endif
