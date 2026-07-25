//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: misc/benchmarks/reference/c/call_stress.c
// Purpose: C reference for the call_stress benchmark kernel.
// Key invariants:
//   - Standalone translation unit; no cross-layer dependencies.
// Ownership/Lifetime:
//   - No long-lived state; all allocations are scoped to the run.
// Links: examples/il/benchmarks/call_stress.il, docs/internals/testing.md
//
//===----------------------------------------------------------------------===//

/**
 * @file misc/benchmarks/reference/c/call_stress.c
 * @brief Native reference implementation of the function-call stress benchmark.
 *
 * @details
 * The loop deliberately routes simple signed 64-bit arithmetic through three
 * small helper functions, exercising call/return overhead and argument passing
 * rather than a complex arithmetic kernel. Its low-byte exit checksum matches
 * the corresponding IL program.
 */

/* call_stress.c — Function call overhead benchmark (10M iterations).
   Equivalent to examples/il/benchmarks/call_stress.il */
#include <stdint.h>
#include <stdlib.h>

/**
 * @brief Add three signed 64-bit operands.
 * @param a First addend.
 * @param b Second addend.
 * @param c Third addend.
 * @return `a + b + c`.
 * @pre The mathematical sum is representable by `int64_t`.
 */
static int64_t add_triple(int64_t a, int64_t b, int64_t c)
{
    return a + b + c;
}

/**
 * @brief Multiply two signed 64-bit operands.
 * @param x Multiplicand.
 * @param y Multiplier.
 * @return `x * y`.
 * @pre The mathematical product is representable by `int64_t`.
 */
static int64_t mul_pair(int64_t x, int64_t y)
{
    return x * y;
}

/**
 * @brief Compose the helper calls used by one benchmark iteration.
 * @param n Current zero-based loop index.
 * @return Three times the sum of `n`, `n + 1`, and `n + 2`.
 * @pre All additions and the final multiplication fit in `int64_t`.
 */
static int64_t compute(int64_t n)
{
    int64_t a = n;
    int64_t b = n + 1;
    int64_t c = n + 2;
    int64_t sum = add_triple(a, b, c);
    return mul_pair(sum, 3);
}

/**
 * @brief Execute the call-heavy kernel and return its checksum.
 * @param argc Hosted argument count. Each user argument adds one iteration.
 * @param argv Hosted argument vector; its contents are intentionally unused.
 * @return The accumulated helper results reduced to the least-significant eight
 *         bits.
 *
 * @pre `argc >= 1`, as required for a hosted C implementation.
 * @pre The derived iteration count and accumulated result fit in `int64_t`.
 */
int main(int argc, char **argv)
{
    (void)argv;
    int64_t n = 10000000 + (argc - 1);
    int64_t sum = 0;
    for (int64_t i = 0; i < n; ++i) {
        int64_t r1 = compute(i);
        int64_t r2 = add_triple(i, r1, 1);
        int64_t r3 = mul_pair(r2, 2);
        sum += r3;
    }
    return (int)(sum & 0xFF);
}
