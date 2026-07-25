//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: misc/benchmarks/reference/c/branch_stress.c
// Purpose: C reference for the branch_stress benchmark kernel.
// Key invariants:
//   - Standalone translation unit; no cross-layer dependencies.
// Ownership/Lifetime:
//   - No long-lived state; all allocations are scoped to the run.
// Links: examples/il/benchmarks/branch_stress.il, docs/internals/testing.md
//
//===----------------------------------------------------------------------===//

/**
 * @file misc/benchmarks/reference/c/branch_stress.c
 * @brief Native reference implementation of the branch-stress benchmark.
 *
 * @details
 * Each iteration evaluates four independent divisibility branches for the
 * primes 2, 3, 5, and 7 and adds a distinct weight for every match. The low
 * byte of the final count provides a compact checksum compatible with the
 * equivalent IL workload.
 */

/* branch_stress.c — Branch-heavy loop benchmark (20M iterations).
   Equivalent to examples/il/benchmarks/branch_stress.il */
#include <stdint.h>
#include <stdlib.h>

/**
 * @brief Execute the divisibility-branch kernel and return its checksum.
 * @param argc Hosted argument count. Each user argument adds one iteration.
 * @param argv Hosted argument vector; its contents are intentionally unused.
 * @return The weighted match count reduced to its least-significant eight bits.
 *
 * @pre `argc >= 1`, as required for a hosted C implementation.
 * @pre The derived iteration count and accumulated weights fit in `int64_t`.
 */
int main(int argc, char **argv)
{
    (void)argv;
    int64_t n = 20000000 + (argc - 1);
    int64_t count = 0;
    for (int64_t i = 0; i < n; ++i) {
        if (i % 2 == 0) count += 1;
        if (i % 3 == 0) count += 2;
        if (i % 5 == 0) count += 3;
        if (i % 7 == 0) count += 5;
    }
    return (int)(count & 0xFF);
}
