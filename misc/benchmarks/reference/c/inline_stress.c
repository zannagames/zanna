//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: misc/benchmarks/reference/c/inline_stress.c
// Purpose: C reference for the inline_stress benchmark kernel.
// Key invariants:
//   - Standalone translation unit; no cross-layer dependencies.
// Ownership/Lifetime:
//   - No long-lived state; all allocations are scoped to the run.
// Links: examples/il/benchmarks/inline_stress.il, docs/internals/testing.md
//
//===----------------------------------------------------------------------===//

/**
 * @file misc/benchmarks/reference/c/inline_stress.c
 * @brief Native reference implementation of the inlining-stress benchmark.
 *
 * @details
 * Five deliberately small helpers build a quadratic expression inside a
 * roughly fifty-million-iteration loop. The accumulator is masked to 28 bits
 * after each iteration, providing a deterministic bounded checksum while
 * preserving abundant call sites for compiler inlining comparisons.
 */

/* inline_stress.c — Inlining stress benchmark (50M iterations).
   Equivalent to examples/il/benchmarks/inline_stress.il */
#include <stdint.h>
#include <stdlib.h>

/**
 * @brief Double a signed 64-bit value by addition.
 * @param x Value to double.
 * @return `x + x`.
 * @pre The result fits in `int64_t`.
 */
static int64_t double_val(int64_t x)
{
    return x + x;
}

/**
 * @brief Square a signed 64-bit value.
 * @param x Value used as both multiplicands.
 * @return `x * x`.
 * @pre The result fits in `int64_t`.
 */
static int64_t square(int64_t x)
{
    return x * x;
}

/**
 * @brief Add three signed 64-bit values.
 * @param a First addend.
 * @param b Second addend.
 * @param c Third addend.
 * @return `a + b + c`.
 * @pre The mathematical sum fits in `int64_t`.
 */
static int64_t add3(int64_t a, int64_t b, int64_t c)
{
    return a + b + c;
}

/**
 * @brief Increment a signed 64-bit value.
 * @param x Value to increment.
 * @return `x + 1`.
 * @pre `x < INT64_MAX`.
 */
static int64_t inc(int64_t x)
{
    return x + 1;
}

/**
 * @brief Compose all leaf helpers into the benchmark expression.
 * @param x Current zero-based loop index.
 * @return `double_val(x) + square(x) + inc(x)`.
 * @pre Every helper result and their sum fit in `int64_t`.
 */
static int64_t combine(int64_t x)
{
    int64_t d = double_val(x);
    int64_t s = square(x);
    int64_t i = inc(x);
    return add3(d, s, i);
}

/**
 * @brief Execute the helper-heavy loop and return its checksum.
 * @param argc Hosted argument count. Each user argument adds one iteration.
 * @param argv Hosted argument vector; its contents are intentionally unused.
 * @return The masked accumulator's least-significant eight bits.
 *
 * @pre `argc >= 1`, and the derived loop indices keep all helper arithmetic
 *      representable by `int64_t`.
 */
int main(int argc, char **argv)
{
    (void)argv;
    int64_t n = 50000000 + (argc - 1);
    int64_t sum = 0;
    for (int64_t i = 0; i < n; ++i) {
        int64_t r = combine(i);
        int64_t raw_sum = sum + r;
        sum = raw_sum & 268435455;
    }
    return (int)(sum & 0xFF);
}
