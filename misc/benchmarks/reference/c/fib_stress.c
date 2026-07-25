//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: misc/benchmarks/reference/c/fib_stress.c
// Purpose: C reference for the fib_stress benchmark kernel.
// Key invariants:
//   - Standalone translation unit; no cross-layer dependencies.
// Ownership/Lifetime:
//   - No long-lived state; all allocations are scoped to the run.
// Links: examples/il/benchmarks/fib_stress.il, docs/internals/testing.md
//
//===----------------------------------------------------------------------===//

/**
 * @file misc/benchmarks/reference/c/fib_stress.c
 * @brief Native reference implementation of the recursive Fibonacci benchmark.
 *
 * @details
 * The workload intentionally computes the fortieth Fibonacci number with the
 * direct exponential recursive definition. The resulting low byte is returned
 * as the process checksum for comparison with VM and native Zanna execution.
 */

/* fib_stress.c — Recursive fibonacci(40) benchmark.
   Equivalent to examples/il/benchmarks/fib_stress.il */
#include <stdint.h>
#include <stdlib.h>

/**
 * @brief Compute a Fibonacci value with direct recursion.
 * @param n Sequence index. Values less than or equal to one are base cases.
 * @return `n` for a base case; otherwise `fib(n - 1) + fib(n - 2)`.
 *
 * @pre Conventional callers supply a non-negative index whose result fits in
 *      `int64_t`.
 * @note The absence of memoization is intentional benchmark behavior.
 */
static int64_t fib(int64_t n)
{
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

/**
 * @brief Compute `fib(40)` and expose its compact checksum.
 * @return The Fibonacci result reduced to its least-significant eight bits.
 */
int main(void)
{
    int64_t result = fib(40);
    return (int)(result & 0xFF);
}
