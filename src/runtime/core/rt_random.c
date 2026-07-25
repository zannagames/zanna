//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_random.c
// Purpose: Implements the deterministic pseudo-random number generator (PRNG)
//          for the Zanna runtime. Mirrors BASIC RND semantics using a 64-bit
//          linear congruential generator (LCG) with fixed multiplier and
//          increment. Static-call state lives in the effective RtContext;
//          Random objects carry independent inline state.
//
// Key invariants:
//   - The LCG uses the MMIX multiplier 6364136223846793005 and increment 1;
//     the same seed always produces the same sequence.
//   - rt_rnd returns values in the half-open interval [0.0, 1.0) by extracting
//     the top 53 bits of state and scaling to IEEE 754 double range.
//   - Static operations resolve and lock the effective context RNG state, using
//     the legacy context for unbound threads.
//   - Instance methods do not acquire a lock; callers must serialize concurrent
//     mutation of one Random object.
//   - Bounded integer sampling rejects the modulo-biased prefix of the 64-bit
//     sample space and supports the complete signed 64-bit inclusive range.
//
// Ownership/Lifetime:
//   - Static RNG state is a uint64_t field owned by RtContext.
//   - Random class instances are GC-managed objects with their own uint64_t state.
//   - Sequence shuffling borrows the sequence and balances temporary element
//     retains while swapping through the public Seq mutation API.
//
// Links: src/runtime/core/rt_random.h (public API),
//        src/runtime/core/rt_context.h (RtContext definition, rng_state field)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Deterministic context and object PRNG operations and distributions.

#include "rt_random.h"
#include "rt_context.h"
#include "rt_context_internal.h"
#include "rt_internal.h"
#include "rt_object.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>

// Forward declare to avoid header dependency (must be at file scope for MSVC)
/// @brief Return the logical length of a runtime sequence.
/// @param seq Runtime Seq handle.
/// @return Number of elements, subject to the Seq API's receiver validation.
extern int64_t rt_seq_len(void *);
/// @brief Borrow one element from a runtime sequence.
/// @param seq Runtime Seq handle.
/// @param index Zero-based element index.
/// @return Borrowed stored value, subject to the Seq API's validation rules.
extern void *rt_seq_get(void *, int64_t);
/// @brief Replace one runtime-sequence element.
/// @param seq Runtime Seq handle.
/// @param index Zero-based element index.
/// @param value New element value, retained according to the Seq contract.
extern void rt_seq_set(void *, int64_t, void *);

/// @brief Private payload for an independently seeded Random object.
typedef struct rt_random_impl {
    /// Reserved object dispatch-table slot.
    void **vptr;
    /// Current 64-bit LCG state.
    uint64_t state;
} rt_random_impl;

/// @brief Sample an optional exclusive bound while mutating explicit state.
static uint64_t rt_random_bounded_u64_from_state(uint64_t *state, uint64_t bound);

/// @brief Validate a `void *` handle as a `Random` instance.
/// @details Checks the class ID and traps on mismatch so a wrong-type handle
///          surfaces as a real error. Returning trap hooks receive a safe null
///          result so callers do not read an unchecked payload.
/// @param self Caller-supplied handle expected to be a `Random` object.
/// @return Validated typed pointer, or `NULL` after trapping on mismatch.
static rt_random_impl *as_random(void *self) {
    if (!rt_obj_is_instance(self, RT_RANDOM_CLASS_ID, sizeof(rt_random_impl))) {
        rt_trap("Random: invalid Random object");
        // Return NULL rather than the unchecked pointer so a returning trap hook
        // (tests/embedders) cannot fall through into a null/wrong-class
        // dereference; every caller must stop when this is NULL (VDOC-200,
        // same trap-discipline class as VDOC-177/189).
        return NULL;
    }
    return (rt_random_impl *)self;
}

/// @brief Advance an LCG state and return the new value.
/// @details Single step of a 64-bit linear-congruential generator with the
///          MMIX multiplier (`6364136223846793005`) and increment `1`. The
///          increment is co-prime with `2^64` so the period is the full
///          state space. Unsigned overflow supplies the required modulo-2^64
///          arithmetic.
/// @param state Non-null pointer to the 64-bit state; updated in place.
/// @return The new state value (also the function's raw u64 output).
static uint64_t rt_random_step_u64(uint64_t *state) {
    *state = *state * 6364136223846793005ULL + 1ULL;
    return *state;
}

/// @brief Step the active context's RNG and return one raw 64-bit sample.
/// @details Thin wrapper over @ref rt_random_step_u64 against the per-context
///          RNG state. Resolution and mutation occur while holding the effective
///          context's RNG lock.
/// @return Raw 64-bit LCG output, or zero without advancing if effective-context
///         acquisition fails.
static uint64_t rt_random_next_u64(void) {
    RtContext *ctx = rt_context_acquire_state(RT_CONTEXT_STATE_RNG, NULL);
    if (!ctx)
        return 0;
    uint64_t value = rt_random_step_u64(&ctx->rng_state);
    rt_context_release_state(ctx, RT_CONTEXT_STATE_RNG);
    return value;
}

/// @brief Sample a uniform random integer in `[0, bound)` with rejection sampling.
/// @details Naïve `next_u64() % bound` introduces modulo bias when `bound` does not
///          evenly divide `2^64`. The threshold trick rejects the small biased band so
///          the surviving samples are exactly uniform. A `bound` of zero means
///          the complete raw 64-bit space. The effective context RNG lock is
///          retained across all rejection attempts.
/// @param bound Exclusive upper bound, or zero for an unbounded raw sample.
/// @return Uniform sample in `[0, bound)`, a raw sample for zero @p bound, or
///         zero without advancing when context acquisition fails.
static uint64_t rt_random_bounded_u64(uint64_t bound) {
    RtContext *ctx = rt_context_acquire_state(RT_CONTEXT_STATE_RNG, NULL);
    if (!ctx)
        return 0;
    uint64_t value = rt_random_bounded_u64_from_state(&ctx->rng_state, bound);
    rt_context_release_state(ctx, RT_CONTEXT_STATE_RNG);
    return value;
}

/// @brief Sample `[0, bound)` from an explicit RNG state with rejection sampling.
/// @details Implementation behind @ref rt_random_bounded_u64 that takes the
///          state by pointer so it can also serve the per-instance `Random`
///          object. Computes the rejection threshold `(2^64 mod bound)` and
///          rejects samples below it; the surviving samples are exactly
///          uniform after `x % bound`. `bound == 0` falls back to returning
///          the raw step output (treated as unbounded).
/// @param state Non-null pointer to the LCG state; updated once per attempt.
/// @param bound Upper bound (exclusive); 0 means "no bound".
/// @return Uniform random integer in `[0, bound)`, or a raw u64 when `bound == 0`.
static uint64_t rt_random_bounded_u64_from_state(uint64_t *state, uint64_t bound) {
    if (bound == 0)
        return rt_random_step_u64(state);

    const uint64_t threshold = (uint64_t)(0ULL - bound) % bound;
    for (;;) {
        uint64_t x = rt_random_step_u64(state);
        if (x >= threshold)
            return x % bound;
    }
}

/// @brief Sample a uniform `[0.0, 1.0)` double from an explicit RNG state.
/// @details Takes the top 53 bits of one LCG step (matching the precision of
///          a `double`'s significand) and divides by `2^53` to produce a
///          uniformly-distributed result in the half-open interval `[0, 1)`.
///          Skipping the low 11 bits avoids the LCG's weaker low-order bits.
/// @param state Non-null pointer to the LCG state; advanced by one step.
/// @return Uniform double in `[0.0, 1.0)`.
static double rt_random_unit_from_state(uint64_t *state) {
    uint64_t x = (rt_random_step_u64(state) >> 11) & ((1ULL << 53) - 1);
    return (double)x * (1.0 / 9007199254740992.0);
}

/// @brief Seed the random generator with an unsigned 64-bit value.
/// @details Replaces the linear congruential generator state in the current
///          effective context under its RNG lock. If context acquisition fails,
///          the request has no effect.
/// @param seed Exact new 64-bit state; no warm-up step is performed.
void rt_randomize_u64(uint64_t seed) {
    RtContext *ctx = rt_context_acquire_state(RT_CONTEXT_STATE_RNG, NULL);
    if (!ctx)
        return;
    ctx->rng_state = seed;
    rt_context_release_state(ctx, RT_CONTEXT_STATE_RNG);
}

/// @brief Seed the random generator with a signed 64-bit value.
/// @details Casts the argument to an unsigned representation before updating
///          the current VM's RNG state so that negative seeds map to the
///          expected bit patterns produced by the BASIC runtime.
/// @param seed New state converted modulo 2^64.
void rt_randomize_i64(long long seed) {
    rt_randomize_u64((uint64_t)seed);
}

/// @brief Produce a pseudo-random double in the half-open interval [0, 1).
/// @details Advances the linear congruential generator in the current VM's
///          context using the MMIX multiplier and increment,
///          extracts the top 53 bits, and scales them into IEEE 754 double
///          range.  The algorithm mirrors the VM implementation so identical
///          seeds yield identical sequences when calls occur in the same order.
///          Context resolution and the state step are serialized by the RNG
///          lock.
/// @return A multiple of 2^-53 in `[0.0, 1.0)`, or `0.0` without advancing if
///         effective-context acquisition fails.
double rt_rnd(void) {
    RtContext *ctx = rt_context_acquire_state(RT_CONTEXT_STATE_RNG, NULL);
    if (!ctx)
        return 0.0;
    double value = rt_random_unit_from_state(&ctx->rng_state);
    rt_context_release_state(ctx, RT_CONTEXT_STATE_RNG);
    return value;
}

/// @brief Generate a random integer in the half-open interval [0, max).
/// @details Advances the linear congruential generator and uses rejection
///          sampling to avoid modulo bias. A non-positive bound returns zero
///          without resolving a context or advancing its sequence.
/// @param max Exclusive upper bound.
/// @return Uniform integer in `[0, max)` when positive; otherwise zero.
long long rt_rand_int(long long max) {
    if (max <= 0)
        return 0;
    return (long long)rt_random_bounded_u64((uint64_t)max);
}

//=============================================================================
// Random Distributions
//=============================================================================

/// @brief Generate a random integer in the range [min, max] (inclusive).
/// @details Swaps inverted bounds and uses unsigned width arithmetic to avoid
///          signed overflow. Rejection sampling makes every value in the
///          normalized inclusive range equiprobable. The full signed 64-bit
///          range is represented by a wrapped width of zero and consumes one
///          raw LCG state.
/// @param min First inclusive bound.
/// @param max Second inclusive bound.
/// @return Uniform value between the normalized bounds, or the zero fallback
///         if effective-context acquisition fails.
long long rt_rand_range(long long min, long long max) {
    if (min > max) {
        long long tmp = min;
        min = max;
        max = tmp;
    }
    // Use unsigned arithmetic to avoid signed overflow when max - min == LLONG_MAX
    unsigned long long urange = (unsigned long long)max - (unsigned long long)min + 1ULL;
    if (urange == 0) {
        // Full signed range: every 64-bit LCG state maps to one signed value.
        return (long long)rt_random_next_u64();
    }
    return (long long)((uint64_t)min + rt_random_bounded_u64((uint64_t)urange));
}

/// @brief Generate a random number from a Gaussian (normal) distribution.
/// @details Uses the Box-Muller transform: given two uniform U1, U2 in (0,1),
///          Z = sqrt(-2*ln(U1)) * cos(2*pi*U2) is standard normal N(0,1).
///          Then scale/shift to N(mean, stddev^2). A non-positive standard
///          deviation returns @p mean without advancing. NaN passes through the
///          transform and consumes samples. Exact-zero U1 samples are retried;
///          each context RNG access is separately locked, so another thread
///          sharing the context may interleave between samples.
/// @param mean Location parameter added to the standard-normal sample.
/// @param stddev Scale parameter; non-positive values select the fallback.
/// @return The transformed sample, or @p mean for non-positive @p stddev.
double rt_rand_gaussian(double mean, double stddev) {
    if (stddev <= 0.0)
        return mean;

    // Generate two uniform random numbers in (0, 1)
    // Avoid exact 0 to prevent log(0)
    double u1 = rt_rnd();
    double u2 = rt_rnd();

    // Ensure u1 is not zero (extremely rare but possible)
    while (u1 == 0.0)
        u1 = rt_rnd();

    // Box-Muller transform
    static const double TWO_PI = 6.283185307179586476925286766559;
    double z = sqrt(-2.0 * log(u1)) * cos(TWO_PI * u2);

    return mean + stddev * z;
}

/// @brief Generate a random number from an exponential distribution.
/// @details Uses inverse transform sampling: X = -ln(1-U)/lambda where U is
///          uniform `[0,1)`. A non-positive rate returns zero without advancing.
///          NaN reaches the transform, consumes one sample, and yields NaN.
/// @param lambda Rate divisor.
/// @return The transformed sample for a positive or unordered rate, or `0.0`
///         when @p lambda is non-positive.
double rt_rand_exponential(double lambda) {
    if (lambda <= 0.0)
        return 0.0;

    double u = rt_rnd();
    // Avoid log(0) when u is exactly 1.0 (extremely rare)
    while (u >= 1.0)
        u = rt_rnd();

    return -log(1.0 - u) / lambda;
}

/// @brief Simulate a dice roll (1 to sides inclusive).
/// @details A positive side count delegates to @ref rt_rand_int and adds one.
///          A non-positive count returns one without advancing the generator.
/// @param sides Inclusive positive upper face value.
/// @return Uniform integer in `[1, sides]`, or one for a non-positive count.
long long rt_rand_dice(long long sides) {
    if (sides <= 0)
        return 1;
    return 1 + rt_rand_int(sides);
}

/// @brief Generate a random boolean with given probability.
/// @details Values at or below zero return false and values at or above one
///          return true without advancing. Values strictly between consume one
///          unit sample. NaN consumes a sample and compares false.
/// @param probability Threshold compared with a uniform `[0,1)` sample.
/// @return One when the sample is below @p probability; otherwise zero.
long long rt_rand_chance(double probability) {
    if (probability <= 0.0)
        return 0;
    if (probability >= 1.0)
        return 1;
    return rt_rnd() < probability ? 1 : 0;
}

/// @brief Generate an i1-compatible random boolean with the given probability.
/// @details Uses the same deterministic RNG state and probability clamping as
///          `rt_rand_chance`; this wrapper only narrows the ABI return type.
/// @param probability Threshold forwarded to @ref rt_rand_chance.
/// @return One for success; otherwise zero.
int8_t rt_rand_chance_bool(double probability) {
    return rt_rand_chance(probability) ? 1 : 0;
}

/// @brief Create a Random object with independent RNG state.
/// @details This enables `new Zanna.Math.Random(seed)` without mutating the
///          effective context's RNG used by static functions. State is stored
///          inline and no finalizer is required. Allocation failure traps and
///          returns `NULL` if the trap hook returns.
/// @param seed Initial state converted modulo 2^64; no warm-up step is performed.
/// @return New runtime-managed Random handle, or `NULL` after allocation failure.
void *rt_random_new(long long seed) {
    rt_random_impl *rng =
        (rt_random_impl *)rt_obj_new_i64(RT_RANDOM_CLASS_ID, (int64_t)sizeof(rt_random_impl));
    if (!rng) {
        rt_trap("Random.New: memory allocation failed");
        return NULL;
    }
    rng->state = (uint64_t)seed;
    return rng;
}

/// @brief Instance method for Random.Next/NextDouble.
/// @details Validates @p self and advances its inline state once without taking
///          a lock. Concurrent use of the same instance must be serialized by
///          the caller.
/// @param self Runtime-managed Random handle.
/// @return A multiple of 2^-53 in `[0.0, 1.0)`, or `0.0` after a returning
///         invalid-receiver trap.
double rt_rnd_method(void *self) {
    rt_random_impl *rng = as_random(self);
    if (!rng)
        return 0.0;
    return rt_random_unit_from_state(&rng->state);
}

/// @brief Instance method for Random.NextInt(max).
/// @details A non-positive @p max returns zero before receiver validation and
///          without advancing. Positive bounds validate @p self and use
///          unbiased rejection sampling without internal locking.
/// @param self Runtime-managed Random handle.
/// @param max Exclusive upper bound.
/// @return Uniform value in `[0, max)`, or zero for a non-positive bound or
///         after a returning invalid-receiver trap.
long long rt_rand_int_method(void *self, long long max) {
    if (max <= 0)
        return 0;
    rt_random_impl *rng = as_random(self);
    if (!rng)
        return 0;
    return (long long)rt_random_bounded_u64_from_state(&rng->state, (uint64_t)max);
}

/// @brief Instance method for Random.Range(min, max).
/// @details Validates @p self, swaps inverted bounds, and uses the same unsigned
///          inclusive-width and full-range handling as @ref rt_rand_range.
///          The instance state is mutated without internal locking.
/// @param self Runtime-managed Random handle.
/// @param min First inclusive bound.
/// @param max Second inclusive bound.
/// @return Uniform value in the normalized inclusive range, or zero after a
///         returning invalid-receiver trap.
long long rt_rand_range_method(void *self, long long min, long long max) {
    rt_random_impl *rng = as_random(self);
    if (!rng)
        return 0;
    if (min > max) {
        long long tmp = min;
        min = max;
        max = tmp;
    }
    unsigned long long urange = (unsigned long long)max - (unsigned long long)min + 1ULL;
    if (urange == 0)
        return (long long)rt_random_step_u64(&rng->state);
    return (long long)((uint64_t)min +
                       rt_random_bounded_u64_from_state(&rng->state, (uint64_t)urange));
}

/// @brief Instance method for Random.Seed(seed).
/// @details Validates @p self and replaces its inline state without taking a
///          lock or advancing the generator.
/// @param self Runtime-managed Random handle.
/// @param seed New state converted modulo 2^64.
void rt_randomize_i64_method(void *self, long long seed) {
    rt_random_impl *rng = as_random(self);
    if (!rng)
        return;
    rng->state = (uint64_t)seed;
}

/// @brief Shuffle elements in a Seq randomly (Fisher-Yates algorithm).
/// @details Null sequences and lengths below two are no-ops that consume no
///          randomness. Otherwise, walks backward and samples an unbiased
///          partner from `[0, i]` using the effective context RNG. Swaps use
///          Seq accessors; the element at @p i is temporarily retained because
///          replacing that slot may release its old value. The temporary retain
///          is balanced after insertion at @p j. Sequence validation, indexing,
///          and mutation traps are governed by the Seq API.
/// @param seq Borrowed mutable runtime Seq handle, or `NULL`.
void rt_rand_shuffle(void *seq) {
    if (!seq)
        return;

    int64_t n = rt_seq_len(seq);
    if (n <= 1)
        return;

    // Fisher-Yates shuffle
    for (int64_t i = n - 1; i > 0; i--) {
        int64_t j = rt_rand_int(i + 1);
        if (i != j) {
            // Retain tmp before swap to prevent use-after-free when Seq owns elements.
            // rt_seq_set releases the old value at i (which is tmp); retaining prevents
            // premature deallocation before tmp is stored at j.
            void *tmp = rt_seq_get(seq, i);
            rt_obj_retain_maybe(tmp);
            rt_seq_set(seq, i, rt_seq_get(seq, j));
            rt_seq_set(seq, j, tmp);
            if (rt_obj_release_check0(tmp))
                rt_obj_free(tmp);
        }
    }
}
