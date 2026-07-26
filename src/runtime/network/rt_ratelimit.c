//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_ratelimit.c
// Purpose: Implements a continuously refilled token bucket for network and API
//          operation rate limiting.
// Key invariants:
//   - Whole-token capacity and balance remain exact signed 64-bit integers.
//   - Refill uses the shared monotonic runtime clock and saturates at capacity.
// Ownership/Lifetime:
//   - RateLimiter payloads are managed objects owned by their callers.
//   - Individual limiter mutation requires external synchronization.
// Links: src/runtime/network/rt_ratelimit.h, src/runtime/core/rt_time.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_ratelimit.c
 * @brief Implements the managed continuously refilled token-bucket limiter.
 * @details Whole-token capacity and balance use exact signed integers while a
 *          fractional carry preserves sub-token refill progress. Public
 *          operations validate managed identity, advance state from the
 *          monotonic clock, and perform all-or-nothing token acquisition.
 */

#include "rt_ratelimit.h"

#include "rt_internal.h"
#include "rt_object.h"
#include "rt_time.h"

#include <math.h>
#include <stdlib.h>

#include "rt_trap.h"

//=============================================================================
// Internal Structures
//=============================================================================

/// @brief Internal rate limiter data.
/// @details Capacity and whole-token balance are exact `int64_t` values so the
///          constructor argument round-trips through `get_Max` for the entire
///          Integer range (doubles lose integer precision above 2^53). Only
///          the sub-token refill remainder is fractional.
typedef struct {
    int64_t tokens_whole;   ///< Current available whole tokens (exact).
    int64_t max_tokens;     ///< Maximum token capacity (exact).
    double tokens_frac;     ///< Fractional refill carry in [0, 1).
    double refill_per_sec;  ///< Tokens refilled per second.
    int64_t last_refill_us; ///< Last monotonic refill timestamp in microseconds.
} rt_ratelimit_data;

/// @brief Validate and cast one non-null RateLimiter handle.
/// @param limiter Candidate opaque managed object.
/// @param context Diagnostic raised for wrong-class or undersized handles.
/// @return Rate-limiter payload, or NULL after trapping.
static rt_ratelimit_data *ratelimit_require(void *limiter, const char *context) {
    if (!rt_obj_is_instance(limiter, RT_RATELIMIT_CLASS_ID, sizeof(rt_ratelimit_data))) {
        rt_trap(context ? context : "RateLimiter: invalid limiter");
        return NULL;
    }
    return (rt_ratelimit_data *)limiter;
}

/// @brief Refill tokens based on elapsed time since last refill.
/// @details Uses the runtime's cross-platform microsecond clock rather than a
///          private platform clock cache. A zero/failed or non-advancing sample
///          leaves state unchanged. Credits too large for signed conversion
///          saturate the bucket before any cast.
/// @param data Valid externally serialized limiter state.
static void refill_tokens(rt_ratelimit_data *data) {
    int64_t now_us = rt_clock_ticks_us();
    if (now_us <= 0)
        return;
    if (data->last_refill_us <= 0) {
        data->last_refill_us = now_us;
        return;
    }
    int64_t elapsed_us = now_us - data->last_refill_us;
    if (elapsed_us <= 0)
        return;
    data->last_refill_us = now_us;

    double elapsed_sec = (double)elapsed_us / 1000000.0;
    double credit = elapsed_sec * data->refill_per_sec + data->tokens_frac;
    if (!(credit > 0.0)) // also rejects NaN
        return;

    // A non-finite credit or one at/above 2^63 cannot be cast to int64_t.
    // In either case it is sufficient to fill every valid bucket.
    if (!isfinite(credit) || credit >= (double)INT64_MAX) {
        data->tokens_whole = data->max_tokens;
        data->tokens_frac = 0.0;
        return;
    }

    int64_t whole = (int64_t)credit;
    // Both operands are non-negative with tokens_whole <= max_tokens, so the
    // subtraction cannot overflow.
    if (whole >= data->max_tokens - data->tokens_whole) {
        data->tokens_whole = data->max_tokens;
        data->tokens_frac = 0.0;
    } else {
        data->tokens_whole += whole;
        data->tokens_frac = credit - (double)whole;
    }
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Creates a new token bucket rate limiter.
///
/// The limiter starts at full capacity (all tokens available).
///
/// @param max_tokens Maximum token capacity. Values <= 0 default to 1.
/// @param refill_per_sec Tokens refilled per second. Values <= 0 default to 1.0.
/// @return A new rate limiter object. Traps on allocation failure.
void *rt_ratelimit_new(int64_t max_tokens, double refill_per_sec) {
    int64_t now_us = rt_clock_ticks_us();
    rt_ratelimit_data *data = (rt_ratelimit_data *)rt_obj_new_i64(
        RT_RATELIMIT_CLASS_ID, (int64_t)sizeof(rt_ratelimit_data));
    if (!data)
        return NULL;
    data->max_tokens = max_tokens > 0 ? max_tokens : 1;
    data->tokens_whole = data->max_tokens;
    data->tokens_frac = 0.0;
    data->refill_per_sec = isfinite(refill_per_sec) && refill_per_sec > 0.0 ? refill_per_sec : 1.0;
    data->last_refill_us = now_us;
    return data;
}

/// @brief Tries to consume 1 token.
///
/// Refills tokens based on elapsed time, then attempts to consume one token.
///
/// @param limiter Rate limiter pointer.
/// @return 1 if a token was consumed, 0 if no tokens available.
int8_t rt_ratelimit_try_acquire(void *limiter) {
    return rt_ratelimit_try_acquire_n(limiter, 1);
}

/// @brief Tries to consume N tokens.
///
/// Refills tokens based on elapsed time, then attempts to consume N tokens.
/// Either all N tokens are consumed or none are (atomic semantics).
///
/// @param limiter Rate limiter pointer.
/// @param n Number of tokens to consume. Values <= 0 return 0.
/// @return 1 if tokens were consumed, 0 if insufficient tokens.
int8_t rt_ratelimit_try_acquire_n(void *limiter, int64_t n) {
    if (!limiter || n <= 0)
        return 0;
    rt_ratelimit_data *data = ratelimit_require(limiter, "RateLimiter.TryAcquire: invalid limiter");
    if (!data)
        return 0;
    refill_tokens(data);
    if (data->tokens_whole >= n) {
        data->tokens_whole -= n;
        return 1;
    }
    return 0;
}

/// @brief Gets the number of currently available tokens.
///
/// Refills tokens based on elapsed time before returning.
///
/// @param limiter Rate limiter pointer.
/// @return Number of available whole tokens (exact).
int64_t rt_ratelimit_available(void *limiter) {
    if (!limiter)
        return 0;
    rt_ratelimit_data *data = ratelimit_require(limiter, "RateLimiter.Available: invalid limiter");
    if (!data)
        return 0;
    refill_tokens(data);
    return data->tokens_whole;
}

/// @brief Resets the limiter to full capacity.
///
/// Sets tokens to max and updates the refill timestamp.
///
/// @param limiter Rate limiter pointer.
void rt_ratelimit_reset(void *limiter) {
    if (!limiter)
        return;
    rt_ratelimit_data *data = ratelimit_require(limiter, "RateLimiter.Reset: invalid limiter");
    if (!data)
        return;
    int64_t now_us = rt_clock_ticks_us();
    data->tokens_whole = data->max_tokens;
    data->tokens_frac = 0.0;
    data->last_refill_us = now_us;
}

/// @brief Gets the maximum token capacity.
///
/// @param limiter Rate limiter pointer.
/// @return Maximum number of tokens.
int64_t rt_ratelimit_get_max(void *limiter) {
    if (!limiter)
        return 0;
    rt_ratelimit_data *data = ratelimit_require(limiter, "RateLimiter.Max: invalid limiter");
    return data ? data->max_tokens : 0;
}

/// @brief Gets the refill rate in tokens per second.
///
/// @param limiter Rate limiter pointer.
/// @return Refill rate (tokens/second).
double rt_ratelimit_get_rate(void *limiter) {
    if (!limiter)
        return 0.0;
    rt_ratelimit_data *data = ratelimit_require(limiter, "RateLimiter.Rate: invalid limiter");
    return data ? data->refill_per_sec : 0.0;
}
