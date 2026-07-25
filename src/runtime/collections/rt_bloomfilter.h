//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/collections/rt_bloomfilter.h
// Purpose: Probabilistic set membership structure using multiple hash functions, guaranteeing no
// false negatives while allowing configurable false positive rates.
//
// Key invariants:
//   - False positives are possible; false negatives are not.
//   - The filter cannot remove elements once added.
//   - Optimal bit array size and hash count are computed from expected_items and
//   false_positive_rate.
//   - Current false-positive-rate estimates are based on the fraction of bits
//     currently set, so duplicate additions do not inflate the estimate.
//   - rt_bloomfilter_might_contain returns 1 for possible membership, 0 for definite absence.
//
// Ownership/Lifetime:
//   - Filter objects are GC-managed opaque pointers.
//   - Callers must not free filter objects directly.
//
// Links: src/runtime/collections/rt_bloomfilter.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares probabilistic string-membership filtering operations.
///
/// A BloomFilter compactly records string additions by setting multiple bits.
/// A negative membership result is definitive, while a positive result can be
/// a false positive. Values cannot be removed individually; clear resets the
/// full filter, and merge unions filters with identical derived shapes.
///
/// The count accessor reports add calls, including duplicates, rather than
/// distinct cardinality. BloomFilter objects are runtime-managed opaque
/// handles and are not safe for unsynchronized concurrent mutation.

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a new Bloom filter.
/// @param expected_items Expected number of items. Zero uses one expected item;
///                       negative values trap.
/// @param false_positive_rate Desired false-positive probability. Non-finite
///        or non-positive values use 0.01; values at least 1.0 use 0.5.
/// @return A new runtime-managed BloomFilter, or NULL after a size or allocation
///         trap.
void *rt_bloomfilter_new(int64_t expected_items, double false_positive_rate);

/// @brief Add a string to the filter.
/// @param filter BloomFilter handle, or NULL for a no-op.
/// @param item Runtime string to add; NULL is ignored.
/// @note The operation count increases for duplicate additions.
void rt_bloomfilter_add(void *filter, rt_string item);

/// @brief Check if a string might be in the filter.
/// @param filter BloomFilter handle, or NULL to query an empty filter.
/// @param item Runtime string to check; NULL returns 0.
/// @return 1 if possibly present, or 0 if definitely absent.
int8_t rt_bloomfilter_might_contain(void *filter, rt_string item);

/// @brief Get the number of successful add calls.
/// @param filter BloomFilter handle, or NULL to query an empty filter.
/// @return Add-call count including duplicates, or zero for NULL.
int64_t rt_bloomfilter_count(void *filter);

/// @brief Get the estimated false positive rate based on current set-bit fill.
/// @param filter BloomFilter handle, or NULL to query an empty filter.
/// @return Estimated false positive rate (0.0-1.0).
double rt_bloomfilter_fpr(void *filter);

/// @brief Clear the filter (remove all items).
/// @param filter BloomFilter handle, or NULL for a no-op.
/// @note The bit-array shape is retained for reuse.
void rt_bloomfilter_clear(void *filter);

/// @brief Merge two filters (union).
/// @param filter Destination filter modified in place.
/// @param other Second filter (must have the same derived bit and hash
///              counts; constructor arguments that derive to the same shape
///              are compatible).
/// @return 1 on success, including self-merge; 0 if either handle is NULL,
///         filters are incompatible, or the combined count overflows.
/// @note The source is unchanged. Counts are added and may include duplicates
///       across the two filters.
int64_t rt_bloomfilter_merge(void *filter, void *other);

#ifdef __cplusplus
}
#endif
