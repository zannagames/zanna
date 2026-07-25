//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: misc/benchmarks/reference/c/udiv_stress.c
// Purpose: C reference for the udiv_stress benchmark kernel.
// Key invariants:
//   - Standalone translation unit; no cross-layer dependencies.
// Ownership/Lifetime:
//   - No long-lived state; all allocations are scoped to the run.
// Links: examples/il/benchmarks/udiv_stress.il, docs/internals/testing.md
//
//===----------------------------------------------------------------------===//

/**
 * @file misc/benchmarks/reference/c/udiv_stress.c
 * @brief Native reference implementation of the unsigned-division benchmark.
 *
 * @details
 * Positive loop indices are divided by eight ascending powers of two. Because
 * every dividend and divisor is positive, the C signed divisions have the same
 * quotient as the IL benchmark's unsigned operations. A 28-bit accumulator
 * mask keeps the running checksum bounded.
 */

/* udiv_stress.c — Unsigned division stress benchmark (50M iterations).
   Equivalent to examples/il/benchmarks/udiv_stress.il */
#include <stdint.h>
#include <stdlib.h>

/**
 * @brief Execute the power-of-two division kernel and return its checksum.
 * @param argc Hosted argument count. Each user argument adds one positive loop
 *             index to the workload.
 * @param argv Hosted argument vector; its contents are intentionally unused.
 * @return The bounded quotient sum reduced to its least-significant eight bits.
 *
 * @pre `argc >= 1`, and the derived iteration count keeps all loop arithmetic
 *      representable by `int64_t`.
 */
int main(int argc, char **argv)
{
    (void)argv;
    int64_t n = 50000001 + (argc - 1);
    int64_t sum = 0;
    for (int64_t i = 1; i < n; ++i) {
        /* Divide by ascending powers of 2. */
        int64_t d1 = i / 2;
        int64_t d2 = i / 4;
        int64_t d3 = i / 8;
        int64_t d4 = i / 16;
        int64_t d5 = i / 32;
        int64_t d6 = i / 64;
        int64_t d7 = i / 128;
        int64_t d8 = i / 256;

        /* Accumulate all quotients so nothing is dead. */
        int64_t s7 = d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8;

        /* Keep sum within range by masking upper bits. */
        int64_t raw_sum = sum + s7;
        sum = raw_sum & 268435455;
    }
    return (int)(sum & 0xFF);
}
