//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: misc/benchmarks/reference/c/string_stress.c
// Purpose: C reference for the string_stress benchmark kernel.
// Key invariants:
//   - Standalone translation unit; no cross-layer dependencies.
// Ownership/Lifetime:
//   - No long-lived state; all allocations are scoped to the run.
// Links: examples/il/benchmarks/string_stress.il, docs/internals/testing.md
//
//===----------------------------------------------------------------------===//

/**
 * @file misc/benchmarks/reference/c/string_stress.c
 * @brief Native reference implementation of the string-manipulation benchmark.
 *
 * @details
 * The kernel reconstructs `Hello World!` in a fixed stack buffer with one copy
 * and three concatenations on each of roughly five hundred thousand
 * iterations. Summing the resulting lengths keeps every operation observable
 * and yields the checksum returned to the host.
 */

/* string_stress.c — String manipulation benchmark (500K iterations).
   Equivalent to examples/il/benchmarks/string_stress.il */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Execute the repeated string-concatenation workload.
 * @param argc Hosted argument count. Each user argument adds one iteration.
 * @param argv Hosted argument vector; its contents are intentionally unused.
 * @return The accumulated byte lengths reduced to the least-significant eight
 *         bits.
 *
 * @pre `argc >= 1`, and the derived iteration count keeps `sum` representable
 *      by `int64_t`.
 * @note The final 13-byte C string, including its terminator, fits comfortably
 *       in the 64-byte local buffer.
 */
int main(int argc, char **argv)
{
    (void)argv;
    int64_t n = 500000 + (argc - 1);
    int64_t sum = 0;
    for (int64_t i = 0; i < n; ++i) {
        char buf[64];
        /* Build "Hello World!" by concatenation */
        strcpy(buf, "Hello");
        strcat(buf, " ");
        strcat(buf, "World");
        strcat(buf, "!");
        sum += (int64_t)strlen(buf);
    }
    return (int)(sum & 0xFF);
}
