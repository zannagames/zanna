//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: misc/benchmarks/reference/c/mixed_stress.c
// Purpose: C reference for the mixed_stress benchmark kernel.
// Key invariants:
//   - Standalone translation unit; no cross-layer dependencies.
// Ownership/Lifetime:
//   - No long-lived state; all allocations are scoped to the run.
// Links: examples/il/benchmarks/mixed_stress.il, docs/internals/testing.md
//
//===----------------------------------------------------------------------===//

/**
 * @file misc/benchmarks/reference/c/mixed_stress.c
 * @brief Native reference implementation of the mixed-workload benchmark.
 *
 * @details
 * The kernel combines arithmetic dependencies, a helper call, and divisibility
 * branches over roughly ten million iterations. Its process exit value is the
 * low byte of the signed 64-bit accumulator, matching the corresponding IL
 * program without adding output overhead to the timed region.
 */

/* mixed_stress.c — Mixed workload benchmark (10M iterations).
   Equivalent to examples/il/benchmarks/mixed_stress.il */
#include <stdint.h>
#include <stdlib.h>

/**
 * @brief Apply the affine transform used by both conditional paths.
 * @param x Signed 64-bit input.
 * @return `3 * x + 7`.
 * @pre The multiplication and addition fit in `int64_t`.
 */
static int64_t helper(int64_t x)
{
    return x * 3 + 7;
}

/**
 * @brief Execute the mixed arithmetic, call, and branch workload.
 * @param argc Hosted argument count. Each user argument adds one iteration.
 * @param argv Hosted argument vector; its contents are intentionally unused.
 * @return The accumulated results reduced to the least-significant eight bits.
 *
 * @pre `argc >= 1`, and the derived iteration count keeps all intermediate and
 *      accumulated arithmetic representable by `int64_t`.
 */
int main(int argc, char **argv)
{
    (void)argv;
    int64_t n = 10000000 + (argc - 1);
    int64_t sum = 0;
    for (int64_t i = 0; i < n; ++i) {
        int64_t t1 = i + 1;
        int64_t t2 = t1 * 2;
        int64_t t3 = t2 - i;
        int64_t tmp;
        if (i % 4 == 0) {
            int64_t r1 = helper(t3);
            tmp = r1 * 2;
        } else {
            int64_t r3 = t3 + 100;
            tmp = r3 * 3;
        }
        if (i % 7 == 0) {
            int64_t bonus_val = helper(tmp);
            tmp = tmp + bonus_val;
        }
        sum += tmp;
    }
    return (int)(sum & 0xFF);
}
