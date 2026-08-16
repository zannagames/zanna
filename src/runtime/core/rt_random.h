//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_random.h
// Purpose: Deterministic pseudo-random number generator implementing BASIC's RND and RANDOMIZE
// functions, unbiased integer ranges, probability distributions, shuffling,
// and independently seeded Random objects using a fixed 64-bit LCG.
//
// Key invariants:
//   - Uses the MMIX LCG multiplier 6364136223846793005 with increment 1.
//   - Floating-point output uses the high 53 bits of state for maximum double precision.
//   - Static generator state is stored and locked per effective runtime context,
//     with the legacy context used for unbound threads.
//   - Random object methods mutate independent inline state without locking.
//   - rt_rand_range normalizes inverted bounds and samples the inclusive range uniformly.
//
// Ownership/Lifetime:
//   - Static generator state is held by the active RtContext.
//   - Random instances store their own independent RNG state in a GC-managed object.
//   - Shuffle borrows its Seq handle and transfers no element ownership.
//
// Links: src/runtime/core/rt_random.c (implementation),
//        src/runtime/core/rt_context_internal.h (effective-context state locking),
//        src/runtime/collections/rt_seq.h (shuffle target API)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Deterministic context and object random-number API.
#pragma once

#include <stdint.h>

/// @brief Stable heap class ID stamped on Random instances.
#define RT_RANDOM_CLASS_ID INT64_C(-0x430601)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Seed the random number generator with an unsigned 64-bit value.
/// @details Replaces the effective context's state while holding its RNG lock.
///          A context-acquisition failure leaves state unchanged.
/// @param seed Exact new state; the generator is not advanced during seeding.
void rt_randomize_u64(uint64_t seed);

/// @brief Seed the random number generator with a signed 64-bit value.
/// @details Converts the value modulo 2^64 and delegates to
///          @ref rt_randomize_u64.
/// @param seed Signed value whose bit pattern initializes the state.
void rt_randomize_i64(long long seed);

/// @brief Generate the next deterministic pseudo-random number.
/// @details Advances a 64-bit linear congruential generator and maps the
///          high 53 bits to a multiple of 2^-53. Resolution and mutation of the
///          effective context are serialized by its RNG lock.
/// @return A double in `[0, 1)`, or `0.0` without advancing if context
///         acquisition fails.
double rt_rnd(void);

/// @brief Generate a random integer in the range [0, max).
/// @details Uses rejection sampling rather than biased modulo reduction.
///          Non-positive bounds return zero without advancing.
/// @param max Exclusive upper bound.
/// @return Uniform integer in `[0, max)` for a positive bound; otherwise zero.
long long rt_rand_int(long long max);

//=========================================================================
// Random Distributions
//=========================================================================

/// @brief Generate a random integer in the range [min, max] (inclusive).
/// @param min Lower bound (inclusive).
/// @param max Upper bound (inclusive).
/// @return A random integer in the normalized inclusive range.
/// @details If min > max, the bounds are swapped automatically. The full
///          signed 64-bit range is supported and advances the generator once;
///          other widths use rejection sampling.
long long rt_rand_range(long long min, long long max);

/// @brief Generate a random number from a Gaussian (normal) distribution.
/// @param mean The mean (center) of the distribution.
/// @details Uses the Box-Muller transform for generating normally
///          distributed values. A non-positive standard deviation returns the
///          mean without advancing; NaN consumes samples and propagates.
///          The two context samples are locked separately and may interleave
///          with other threads sharing that context.
/// @param mean Location parameter.
/// @param stddev Scale parameter.
/// @return A transformed normal sample, or @p mean for non-positive @p stddev.
double rt_rand_gaussian(double mean, double stddev);

/// @brief Generate a random number from an exponential distribution.
/// @details Uses inverse transform sampling: -ln(1-U)/lambda.
///          A non-positive rate returns zero without advancing; NaN consumes a
///          sample and propagates.
/// @param lambda Rate parameter.
/// @return Transformed exponential sample, or zero for a non-positive rate.
double rt_rand_exponential(double lambda);

/// @brief Simulate a dice roll (1 to sides inclusive).
/// @details Convenience function for game programming. Returns 1 if
///          sides is non-positive without advancing the generator.
/// @param sides Inclusive positive upper face.
/// @return Uniform integer in `[1, sides]`, or one for a non-positive count.
long long rt_rand_dice(long long sides);

/// @brief Generate a random boolean with given probability.
/// @details Values at or below zero return zero and values at or above one
///          return one without advancing. Interior values consume one sample.
///          NaN consumes a sample and returns zero.
/// @param probability Threshold compared with a uniform unit sample.
/// @return One when the sampled value is below @p probability; otherwise zero.
long long rt_rand_chance(double probability);

/// @brief Generate a random boolean value with the given probability.
/// @details This is the boolean-returning public wrapper for
///          `Zanna.Math.Random.Chance`. It shares the exact sampling behavior of
///          `rt_rand_chance` and returns an `i1`-compatible value.
/// @param probability Threshold forwarded to @ref rt_rand_chance.
/// @return 1 when the chance succeeds, otherwise 0.
int8_t rt_rand_chance_bool(double probability);

/// @brief Shuffle elements in a Seq randomly.
/// @details Performs Fisher-Yates shuffle using the current RNG state.
///          Null and fewer-than-two-element sequences are unchanged without
///          advancing. Swaps preserve Seq element ownership by retaining the
///          displaced element across the first replacement.
/// @param seq Borrowed mutable Zanna.Collections.Seq handle, or `NULL`.
void rt_rand_shuffle(void *seq);

/// @brief Create a Random object with independent state.
/// @details Allocation failure traps and returns `NULL` if the trap hook returns.
/// @param seed Initial state converted modulo 2^64.
/// @return New GC-managed Random object, or `NULL` on allocation failure.
void *rt_random_new(long long seed);

// Instance methods operate on the receiver's independent RNG state.
/// @brief Instance variant of rt_rnd(): next double in [0, 1) for @p self.
/// @details Validates the receiver and mutates its state without internal
///          locking; callers must serialize shared-instance access.
/// @param self Runtime-managed Random handle.
/// @return Unit sample, or `0.0` after a returning invalid-receiver trap.
double rt_rnd_method(void *self);
/// @brief Instance variant of rt_rand_int(): random integer in [0, @p max).
/// @details A non-positive bound returns zero before receiver validation.
///          Positive bounds validate and mutate @p self without locking.
/// @param self Runtime-managed Random handle.
/// @param max Exclusive upper bound.
/// @return Uniform bounded sample, or zero for a non-positive bound or after a
///         returning invalid-receiver trap.
long long rt_rand_int_method(void *self, long long max);
/// @brief Instance variant of rt_rand_range(): random integer in
///        [@p min, @p max] (bounds swapped if inverted).
/// @details Validates and mutates @p self without internal locking.
/// @param self Runtime-managed Random handle.
/// @param min First inclusive bound.
/// @param max Second inclusive bound.
/// @return Uniform sample in the normalized range, or zero after a returning
///         invalid-receiver trap.
long long rt_rand_range_method(void *self, long long min, long long max);
/// @brief Re-seed @p self's independent RNG state with @p seed.
/// @details Validates and mutates @p self without internal locking.
/// @param self Runtime-managed Random handle.
/// @param seed New state converted modulo 2^64.
void rt_randomize_i64_method(void *self, long long seed);


// Stream state: snapshot, restore, fork, derive.
/// @brief Read @p self's raw stream cursor for checkpointing.
/// @param self Runtime-managed Random handle.
/// @return Current cursor, or zero after a returning invalid-receiver trap.
long long rt_random_get_state(void *self);
/// @brief Restore @p self's stream cursor from a checkpoint.
/// @param self Runtime-managed Random handle.
/// @param state Cursor to resume from, converted modulo 2^64.
void rt_random_set_state(void *self, long long state);
/// @brief Fork @p self into an independent stream at its current position.
/// @param self Runtime-managed Random handle.
/// @return New Random handle, or `NULL` on invalid receiver / allocation failure.
void *rt_random_clone(void *self);
/// @brief Derive a decorrelated child stream without advancing @p self.
/// @param self Runtime-managed Random handle supplying the parent cursor.
/// @param ns Caller-chosen namespace discriminator.
/// @return New independent Random handle, or `NULL` on failure.
void *rt_random_derive(void *self, long long ns);
/// @brief Read the effective context's shared (static-API) cursor.
/// @return Current shared cursor, or zero if context acquisition fails.
long long rt_random_get_global_state(void);
/// @brief Restore the effective context's shared (static-API) cursor.
/// @param state Cursor to resume from, converted modulo 2^64.
void rt_random_set_global_state(long long state);
/// @brief Stateless uniform draw in [@p lo, @p hi] from (@p seed, @p seq, @p salt).
/// @details Consumes no stream; a pure function of its arguments.
/// @param seed Run-level seed.
/// @param seq Monotonic sequence counter.
/// @param salt Per-call-site discriminator.
/// @param lo First inclusive bound.
/// @param hi Second inclusive bound.
/// @return Uniform value in the normalized inclusive range.
long long rt_random_hash_range(
    long long seed, long long seq, long long salt, long long lo, long long hi);
/// @brief Instance chance test on an integer percentage.
/// @param self Runtime-managed Random handle.
/// @param percent Probability in percent; clamped ends do not advance.
/// @return Non-zero when the draw succeeds.
int8_t rt_rand_chance_percent_method(void *self, long long percent);
#ifdef __cplusplus
}
#endif
