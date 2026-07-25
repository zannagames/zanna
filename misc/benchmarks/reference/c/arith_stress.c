//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: misc/benchmarks/reference/c/arith_stress.c
// Purpose: C reference for the arith_stress benchmark kernel.
// Key invariants:
//   - Standalone translation unit; no cross-layer dependencies.
// Ownership/Lifetime:
//   - No long-lived state; all allocations are scoped to the run.
// Links: examples/il/benchmarks/arith_stress.il, docs/internals/testing.md
//
//===----------------------------------------------------------------------===//

/**
 * @file misc/benchmarks/reference/c/arith_stress.c
 * @brief Native reference implementation of the arithmetic-stress benchmark.
 *
 * @details
 * The kernel performs a fixed chain of signed 64-bit additions,
 * multiplications, and subtractions for roughly fifty million loop iterations.
 * Its process result is the low byte of the accumulated sum, matching the
 * corresponding IL benchmark while keeping terminal output out of timings.
 */

/* arith_stress.c — Arithmetic-heavy loop benchmark (50M iterations).
   Equivalent to examples/il/benchmarks/arith_stress.il */
#include <stdint.h>
#include <stdlib.h>

/**
 * @brief Execute the arithmetic kernel and return its checksum.
 * @param argc Hosted argument count. Each user-supplied argument adds one loop
 *             iteration so command-line inputs remain observably consumed.
 * @param argv Hosted argument vector; its contents are intentionally unused.
 * @return The accumulated signed 64-bit sum reduced to its least-significant
 *         eight bits.
 *
 * @pre `argc >= 1`, as required for a hosted C implementation.
 * @pre The derived iteration count and intermediate sum fit in `int64_t`.
 */
int main(int argc, char **argv)
{
    (void)argv;
    int64_t n = 50000000 + (argc - 1);
    int64_t sum = 0;
    for (int64_t i = 0; i < n; ++i) {
        int64_t t1 = i + 1;
        int64_t t2 = t1 * 2;
        int64_t t3 = i + 3;
        int64_t t4 = t2 + t3;
        int64_t t5 = t4 * 5;
        int64_t t6 = t5 - i;
        int64_t t7 = t6 + 7;
        int64_t t8 = t7 * 3;
        int64_t t9 = t8 - 11;
        sum += t9;
    }
    return (int)(sum & 0xFF);
}
