//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_retry.c
// Purpose: Implements fixed and exponential RetryPolicy state, bounded jitter,
//          and atomic attempt reservation for transient-operation retries.
// Key invariants:
//   - Successful NextDelay calls reserve distinct attempts up to max_retries.
//   - Every returned delay is non-negative and no greater than max_delay_ms.
// Ownership/Lifetime:
//   - RetryPolicy payloads are managed objects owned by their callers.
//   - Policy configuration and jitter seeds are immutable after construction.
// Links: src/runtime/network/rt_retry.h, src/runtime/core/rt_time.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_retry.c
 * @brief Implements fixed and exponential managed retry policies.
 * @details Retry attempts are reserved atomically, allowing concurrent callers
 *          to consume distinct slots. Delay calculation normalizes invalid
 *          configuration, avoids signed overflow, caps exponential growth,
 *          and derives bounded per-attempt jitter from immutable policy state.
 */

#include "rt_retry.h"

#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_time.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// --- Internal structure ---

typedef struct {
    int64_t max_retries;
    int64_t base_delay_ms;
    int64_t max_delay_ms;
    volatile int64_t current_attempt;
    uint64_t jitter_seed;
    int8_t exponential;
} rt_retry_data;

static volatile uint64_t g_retry_seed_sequence = UINT64_C(0x6A09E667F3BCC909);

/// @brief Finalize a RetryPolicy payload that owns no subordinate resources.
/// @param obj RetryPolicy allocation being finalized.
static void retry_finalizer(void *obj) {
    (void)obj;
}

/// @brief Mix a 64-bit value into a high-quality non-cryptographic bit pattern.
/// @details This is the SplitMix64 finalizer. Retry jitter needs independent,
///          well-distributed values, not cryptographic unpredictability.
/// @param value Input bits to avalanche.
/// @return Deterministically mixed 64-bit value.
static uint64_t retry_mix64(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27;
    value *= UINT64_C(0x94D049BB133111EB);
    value ^= value >> 31;
    return value;
}

/// @brief Build a non-zero process-unique seed for retry jitter.
///
/// @details Combines a wrapping atomic construction sequence, the runtime's
///          monotonic clock, and caller configuration. This avoids a blocking
///          OS entropy request for non-security jitter while still ensuring
///          policies constructed in the same tick use different sequences.
///
/// @param salt Additional caller state, usually delay/attempt data.
/// @return Non-zero immutable policy seed.
static uint64_t retry_jitter_seed(uint64_t salt) {
    uint64_t sequence = rt_atomic_fetch_add_u64(
        &g_retry_seed_sequence, UINT64_C(0x9E3779B97F4A7C15), __ATOMIC_RELAXED);
    uint64_t ticks = (uint64_t)rt_clock_ticks_us();
    uint64_t seed = retry_mix64(sequence ^ ticks ^ salt ^ UINT64_C(0xD1B54A32D192ED03));
    if (seed == 0)
        seed = UINT64_C(0xA0761D6478BD642F);
    return seed;
}

/// @brief Validate and cast one non-null RetryPolicy handle.
/// @param policy Candidate opaque managed object.
/// @param context Diagnostic raised for wrong-class or undersized handles.
/// @return Retry payload, or NULL after trapping.
static rt_retry_data *retry_require(void *policy, const char *context) {
    if (!rt_obj_is_instance(policy, RT_RETRY_CLASS_ID, sizeof(rt_retry_data))) {
        rt_trap(context ? context : "RetryPolicy: invalid policy");
        return NULL;
    }
    return (rt_retry_data *)policy;
}

/// @brief Atomically reserve the next retry attempt number.
/// @details Concurrent calls use compare-and-swap so every successful caller
///          receives a distinct zero-based attempt and the count never exceeds
///          @c max_retries. The returned attempt drives deterministic jitter.
/// @param data Valid retry policy state.
/// @return Reserved attempt, or -1 when the policy is exhausted.
static int64_t retry_take_attempt(rt_retry_data *data) {
    int64_t attempt = rt_atomic_load_i64(&data->current_attempt, __ATOMIC_ACQUIRE);
    while (attempt < data->max_retries) {
        int64_t desired = attempt + 1;
        if (__atomic_compare_exchange_n(
                &data->current_attempt, &attempt, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return desired - 1;
    }
    return -1;
}

// --- Public API ---

/// @brief Construct a fixed-delay retry policy.
/// @details Negative retry counts and delays are normalized to zero. Each
///          successful @ref rt_retry_next_delay reservation returns the same
///          normalized delay until the retry budget is exhausted.
/// @param max_retries Maximum successful attempt reservations.
/// @param base_delay_ms Constant delay in milliseconds.
/// @return Caller-owned managed RetryPolicy, or NULL on allocation failure.
void *rt_retry_new(int64_t max_retries, int64_t base_delay_ms) {
    void *obj = rt_obj_new_i64(RT_RETRY_CLASS_ID, sizeof(rt_retry_data));
    if (!obj)
        return NULL;
    rt_retry_data *data = (rt_retry_data *)obj;
    int64_t base = base_delay_ms >= 0 ? base_delay_ms : 0;
    data->max_retries = max_retries >= 0 ? max_retries : 0;
    data->base_delay_ms = base;
    data->max_delay_ms = base; // Fixed delay
    data->current_attempt = 0;
    data->jitter_seed = 0;
    data->exponential = 0;
    rt_obj_set_finalizer(obj, retry_finalizer);
    return obj;
}

/// @brief Construct an exponential-backoff retry policy with additive jitter.
/// @details The base is normalized to zero and the maximum to at least the
///          normalized base. Reserved attempt @c n starts from
///          `base * 2^n`, is overflow-safely capped, and receives deterministic
///          per-policy jitter of up to 25% without exceeding the maximum.
/// @param max_retries Maximum successful attempt reservations; negatives
///        normalize to zero.
/// @param base_delay_ms Initial delay in milliseconds; negatives normalize to
///        zero.
/// @param max_delay_ms Delay ceiling, normalized to at least the base.
/// @return Caller-owned managed RetryPolicy, or NULL on allocation failure.
void *rt_retry_exponential(int64_t max_retries, int64_t base_delay_ms, int64_t max_delay_ms) {
    int64_t base = base_delay_ms >= 0 ? base_delay_ms : 0;
    int64_t max_delay = max_delay_ms >= base ? max_delay_ms : base;
    uint64_t jitter_seed = retry_jitter_seed((uint64_t)base ^ ((uint64_t)max_delay << 1));
    void *obj = rt_obj_new_i64(RT_RETRY_CLASS_ID, sizeof(rt_retry_data));
    if (!obj)
        return NULL;
    rt_retry_data *data = (rt_retry_data *)obj;
    data->max_retries = max_retries >= 0 ? max_retries : 0;
    data->base_delay_ms = base;
    data->max_delay_ms = max_delay;
    data->current_attempt = 0;
    data->jitter_seed = jitter_seed;
    data->exponential = 1;
    rt_obj_set_finalizer(obj, retry_finalizer);
    return obj;
}

/// @brief Check whether another retry attempt can be reserved.
/// @details This atomic snapshot does not itself consume an attempt; another
///          thread may reserve the final attempt before a subsequent call.
/// @param policy RetryPolicy receiver; NULL yields zero.
/// @return One when the consumed count is below the configured maximum;
///         otherwise zero.
int8_t rt_retry_can_retry(void *policy) {
    if (!policy)
        return 0;
    rt_retry_data *data = retry_require(policy, "RetryPolicy.CanRetry: invalid policy");
    if (!data)
        return 0;
    return rt_atomic_load_i64(&data->current_attempt, __ATOMIC_ACQUIRE) < data->max_retries ? 1 : 0;
}

/// @brief Compute the delay (ms) before the next retry attempt and advance the counter.
/// @details In exponential mode the delay is `base_delay_ms * 2^attempt`,
///          capped at `max_delay_ms`. The doubling loop checks
///          `delay > INT64_MAX / 2` *before* multiplying — checking after the
///          fact would already have wrapped past INT64_MAX into negative
///          territory, which would then compare incorrectly against
///          `max_delay_ms`. Once the cap is reached, the loop short-circuits
///          so further attempts don't keep iterating uselessly.
///
///          A 0–25% additive jitter is applied to the computed delay to
///          prevent the **thundering herd** problem — when N clients all
///          retry on the same backoff schedule (e.g., a server burped at
///          T=0 and they all retry at T=base, then T=2*base, then T=4*base
///          ...), they synchronize and re-overload the server at every
///          retry boundary. Adding a per-call random offset spreads them out.
///
///          The jitter PRNG is per policy, seeded at construction. That keeps
///          independent retry policies from sharing global `rand()` state or
///          coordinating their jitter sequence.
///
///          Returns -1 when retries are exhausted (`current_attempt >=
///          max_retries`); the counter is *not* advanced in that case.
/// @param policy RetryPolicy whose next attempt is atomically reserved; NULL
///        yields -1.
/// @return Nonnegative delay in milliseconds for the reserved attempt, or -1
///         when the policy is exhausted or invalid.
int64_t rt_retry_next_delay(void *policy) {
    if (!policy)
        return -1;
    rt_retry_data *data = retry_require(policy, "RetryPolicy.NextDelay: invalid policy");
    if (!data)
        return -1;
    int64_t attempt = retry_take_attempt(data);
    if (attempt < 0)
        return -1;

    int64_t delay;
    if (data->exponential) {
        // Exponential backoff: base * 2^attempt, capped at max_delay_ms.
        // Overflow guard: check before multiplying to avoid wrapping past INT64_MAX.
        delay = data->base_delay_ms;
        for (int64_t i = 0; i < attempt; i++) {
            if (delay >= data->max_delay_ms) {
                delay = data->max_delay_ms;
                break;
            }
            // Avoid overflow: if doubling would exceed INT64_MAX, clamp to max_delay_ms
            if (delay > (INT64_MAX / 2)) {
                delay = data->max_delay_ms;
                break;
            }
            delay *= 2;
            if (delay > data->max_delay_ms)
                delay = data->max_delay_ms;
        }

        // Add 0-25% additive jitter without ever overflowing the signed
        // delay. Each attempt derives its own value from immutable policy
        // state, so concurrent callers do not race on PRNG state.
        if (delay > 0) {
            int64_t jitter_max = delay / 4;
            int64_t headroom = data->max_delay_ms - delay;
            if (jitter_max > headroom)
                jitter_max = headroom;
            uint64_t random_bits = retry_mix64(
                data->jitter_seed + (uint64_t)(attempt + 1) * UINT64_C(0x9E3779B97F4A7C15));
            uint64_t jitter_range = (uint64_t)jitter_max + 1;
            delay += (int64_t)(random_bits % jitter_range);
        }
    } else {
        // Fixed delay
        delay = data->base_delay_ms;
    }

    return delay;
}

/// @brief Return the number of retry attempts consumed so far.
/// @param policy RetryPolicy receiver; NULL yields zero.
/// @return Atomic count of successful attempt reservations.
int64_t rt_retry_get_attempt(void *policy) {
    if (!policy)
        return 0;
    rt_retry_data *data = retry_require(policy, "RetryPolicy.Attempt: invalid policy");
    return data ? rt_atomic_load_i64(&data->current_attempt, __ATOMIC_ACQUIRE) : 0;
}

/// @brief Return the maximum number of retries configured for this policy.
/// @param policy RetryPolicy receiver; NULL yields zero.
/// @return Normalized immutable retry limit.
int64_t rt_retry_get_max_retries(void *policy) {
    if (!policy)
        return 0;
    rt_retry_data *data = retry_require(policy, "RetryPolicy.MaxRetries: invalid policy");
    return data ? data->max_retries : 0;
}

/// @brief Atomically reset the attempt counter for policy reuse.
/// @details Concurrent reservation calls may run immediately before or after
///          the release-store; callers must externally coordinate a reset that
///          defines a strict retry epoch.
/// @param policy RetryPolicy receiver; NULL is a no-op.
void rt_retry_reset(void *policy) {
    if (!policy)
        return;
    rt_retry_data *data = retry_require(policy, "RetryPolicy.Reset: invalid policy");
    if (!data)
        return;
    __atomic_store_n(&data->current_attempt, 0, __ATOMIC_RELEASE);
}

/// @brief Return the total number of attempts reserved in the current epoch.
/// @param policy RetryPolicy receiver; NULL yields zero.
/// @return Atomic attempt count, identical to @ref rt_retry_get_attempt.
int64_t rt_retry_get_total_attempts(void *policy) {
    if (!policy)
        return 0;
    rt_retry_data *data = retry_require(policy, "RetryPolicy.TotalAttempts: invalid policy");
    return data ? rt_atomic_load_i64(&data->current_attempt, __ATOMIC_ACQUIRE) : 0;
}

/// @brief Check whether all retry attempts have been used up.
/// @param policy RetryPolicy receiver; NULL or an invalid receiver is treated
///        as exhausted.
/// @return One when the attempt count has reached the limit; otherwise zero.
int8_t rt_retry_is_exhausted(void *policy) {
    if (!policy)
        return 1;
    rt_retry_data *data = retry_require(policy, "RetryPolicy.IsExhausted: invalid policy");
    if (!data)
        return 1;
    return rt_atomic_load_i64(&data->current_attempt, __ATOMIC_ACQUIRE) >= data->max_retries ? 1
                                                                                             : 0;
}
