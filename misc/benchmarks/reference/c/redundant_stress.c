//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: misc/benchmarks/reference/c/redundant_stress.c
// Purpose: C reference for the redundant_stress benchmark kernel.
// Key invariants:
//   - Standalone translation unit; no cross-layer dependencies.
// Ownership/Lifetime:
//   - No long-lived state; all allocations are scoped to the run.
// Links: examples/il/benchmarks/redundant_stress.il, docs/internals/testing.md
//
//===----------------------------------------------------------------------===//

/**
 * @file misc/benchmarks/reference/c/redundant_stress.c
 * @brief Native reference for redundant-expression and constant-folding stress.
 *
 * @details
 * Each iteration deliberately repeats one index-dependent expression and
 * rebuilds three constant arithmetic chains. The structure supplies common
 * subexpression elimination and sparse constant propagation opportunities.
 * Masking the accumulator to 28 bits keeps the checksum bounded throughout the
 * roughly fifty-million-iteration workload.
 */

/* redundant_stress.c — Redundant computation / constant propagation benchmark (50M iterations).
   Equivalent to examples/il/benchmarks/redundant_stress.il */
#include <stdint.h>
#include <stdlib.h>

/**
 * @brief Execute the redundant-computation kernel and return its checksum.
 * @param argc Hosted argument count. Each user argument adds one iteration.
 * @param argv Hosted argument vector; its contents are intentionally unused.
 * @return The bounded accumulator's least-significant eight bits.
 *
 * @pre `argc >= 1`, and the derived loop indices keep all signed arithmetic
 *      representable by `int64_t`.
 */
int main(int argc, char **argv)
{
    (void)argv;
    int64_t n = 50000000 + (argc - 1);
    int64_t sum = 0;
    for (int64_t i = 0; i < n; ++i) {
        /* Constant expressions: SCCP folds these to immediate constants. */
        int64_t k1 = 10 + 20;
        int64_t k2 = k1 * 3;
        int64_t k3 = k2 - 40;

        /* Redundant subexpressions: computed identically twice. */
        int64_t a1 = i + 7;
        int64_t a2 = a1 * 3;

        int64_t b1 = i + 7;
        int64_t b2 = b1 * 3;

        /* More constant folding chains. */
        int64_t c1 = 100 + 200;
        int64_t c2 = c1 * 2;
        int64_t c3 = c2 - 100;

        /* Third constant chain. */
        int64_t d1 = 5 + 10;
        int64_t d2 = d1 * 5;
        int64_t d3 = d2 - 5;

        /* Live computation that uses the redundant pair and constants. */
        int64_t live = a2 + b2 + k3 + c3 + d3;

        /* Keep sum within range. */
        int64_t raw_sum = sum + live;
        sum = raw_sum & 268435455;
    }
    return (int)(sum & 0xFF);
}
