//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_hash_table_util.h
// Purpose: Provides overflow-safe integer capacity calculations shared by the
//          runtime's separate-chaining hash collections.
//
// Key invariants:
//   - Load decisions use exact integer arithmetic and never lose precision at
//     large capacities.
//   - Capacities grow by powers of two and are never allowed to wrap size_t.
//   - The normal maximum load is three quarters of the bucket count.
//
// Ownership/Lifetime:
//   - Header-only arithmetic helpers own no storage and keep no global state.
//
// Links: src/runtime/collections/rt_map.c,
//        src/runtime/collections/rt_intmap.c,
//        src/runtime/collections/rt_bag.c,
//        docs/adr/0133-runtime-concurrency-and-collection-hardening.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Provides shared overflow-safe hash-table capacity arithmetic.
///
/// These header-only helpers centralize the three-quarter load policy used by
/// separately chained runtime collections. They avoid overflow-prone
/// multiplication, reject unrepresentable doubling, and write output
/// capacities only after a complete calculation succeeds. No helper allocates
/// storage or mutates a table directly.

#pragma once

#include <stddef.h>
#include <stdint.h>

/// @brief Test whether an entry count exceeds a three-quarter load factor.
/// @details Computes `count > floor(capacity * 3 / 4)` without multiplying
///          `capacity`, so the comparison remains exact even near `SIZE_MAX`.
///          A zero-capacity table is considered overloaded whenever it would
///          contain at least one entry.
/// @param count Prospective number of entries.
/// @param capacity Number of available buckets.
/// @return Non-zero when the table must grow before publishing @p count.
static inline int rt_hash_table_exceeds_load(size_t count, size_t capacity) {
    size_t threshold = capacity - capacity / 4 - (capacity % 4 != 0 ? 1u : 0u);
    return count > threshold;
}

/// @brief Compute the next doubled hash-table capacity.
/// @details Rejects zero and values greater than `SIZE_MAX / 2`, preventing a
///          doubling operation from wrapping. The output is written only on
///          success so callers can preserve their current table transactionally.
/// @param capacity Current non-zero bucket count.
/// @param out_capacity Receives twice @p capacity on success.
/// @return Non-zero on success; zero when doubling is not representable.
static inline int rt_hash_table_double_capacity(size_t capacity, size_t *out_capacity) {
    if (!out_capacity || capacity == 0 || capacity > SIZE_MAX / 2)
        return 0;
    *out_capacity = capacity * 2;
    return 1;
}

/// @brief Find the smallest geometric capacity suitable for an entry count.
/// @details Starting at @p initial_capacity, repeatedly doubles until @p count
///          is at or below the normal three-quarter threshold. This is the
///          target used by explicit Trim operations; it never returns a value
///          below the collection's initial capacity.
/// @param count Current number of entries.
/// @param initial_capacity Minimum permitted bucket count; must be non-zero.
/// @param out_capacity Receives the selected capacity on success.
/// @return Non-zero on success; zero if no representable doubled capacity can
///         satisfy the requested count.
static inline int rt_hash_table_trim_capacity(size_t count,
                                              size_t initial_capacity,
                                              size_t *out_capacity) {
    if (!out_capacity || initial_capacity == 0)
        return 0;
    size_t capacity = initial_capacity;
    while (rt_hash_table_exceeds_load(count, capacity)) {
        if (!rt_hash_table_double_capacity(capacity, &capacity))
            return 0;
    }
    *out_capacity = capacity;
    return 1;
}
