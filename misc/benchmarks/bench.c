//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: misc/benchmarks/bench.c
// Purpose: Native reference benchmark driver for VM/native comparisons.
// Key invariants:
//   - Standalone translation unit; no cross-layer dependencies.
// Ownership/Lifetime:
//   - No long-lived state; all allocations are scoped to the run.
// Links: misc/benchmarks/reference/, docs/internals/testing.md
//
//===----------------------------------------------------------------------===//

/**
 * @file misc/benchmarks/bench.c
 * @brief Provides a native recursive-Fibonacci reference workload.
 *
 * @details
 * This standalone C program computes `fib(45)` using the intentionally naive
 * recursive algorithm and prints the result. It serves as a simple native
 * timing and result baseline for equivalent Zanna VM and generated-code
 * workloads.
 */

#include <stdio.h>

/**
 * @brief Compute a Fibonacci number with direct exponential recursion.
 * @param n Sequence index. Values less than or equal to one are returned
 *          unchanged.
 * @return `n` for the base case; otherwise `fib(n - 1) + fib(n - 2)`.
 *
 * @pre For the conventional non-negative Fibonacci sequence, `n >= 0`.
 * @note The function intentionally performs no memoization and is suitable only
 *       for benchmark-sized indices whose result fits in `long`.
 */
long fib(long n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

/**
 * @brief Run and print the fixed Fibonacci benchmark.
 * @return Zero after writing the decimal `fib(45)` result to standard output.
 */
int main() {
    long result = fib(45);
    printf("fib(45) = %ld\n", result);
    return 0;
}
