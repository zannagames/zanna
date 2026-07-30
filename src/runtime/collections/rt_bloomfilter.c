//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_bloomfilter.c
// Purpose: Implements a probabilistic set membership filter (Bloom filter) that
//   uses multiple independent hash functions to map elements into a compact bit
//   array. Supports only Add and MightContain — there is no Remove. The bit
//   array size and hash function count are computed from the expected item count
//   and the caller-supplied false-positive rate at construction time.
//
// Key invariants:
//   - Optimal bit count: m = -n * ln(p) / (ln(2)^2), minimum 64 bits.
//   - Optimal hash count: k = (m/n) * ln(2), clamped to [1, 30].
//   - Hash functions are derived from a MurmurHash3-style mix seeded with
//     different seed values per function index; no external hash table is used.
//   - MightContain returns 1 if ALL k bits for a key are set; false positives
//     are possible but false negatives are not (once added, always found).
//   - False positive rate rises above the specified target if more items than
//     `expected_items` are added.
//   - Not thread-safe; external synchronization required for concurrent access.
//
// Ownership/Lifetime:
//   - BloomFilter objects are GC-managed (rt_obj_new_i64). The bits array is
//     heap-allocated and freed by the GC finalizer (bloomfilter_finalizer).
//
// Links: src/runtime/collections/rt_bloomfilter.h (public API)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements a configurable probabilistic string-membership filter.
///
/// Construction derives a packed bit-array size and number of seeded hash
/// probes from an expected insertion count and target false-positive rate.
/// Adding a non-null string sets every derived position; querying succeeds
/// only when all positions are set. Consequently a populated filter can
/// report false positives but not false negatives until it is cleared.
///
/// The stored item count records successful add calls, including duplicates,
/// while the false-positive estimate derives from actual bit occupancy.
/// Compatible filters can be unioned in place with a bytewise OR. Objects are
/// runtime-managed and mutation is unsynchronized.

#include "rt_bloomfilter.h"
#include "rt_collection_ids.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal structure
// ---------------------------------------------------------------------------

/// @brief Internal BloomFilter state.
/// @details The byte array owns the packed bits; `bit_count` may leave unused
///          high bits in its last byte.
typedef struct {
    void *vptr;
    uint8_t *bits;
    int64_t bit_count;  // Total number of bits
    int64_t hash_count; // Number of hash functions
    int64_t item_count; // Items added
} rt_bloomfilter_impl;

/// @brief Checked cast of an opaque handle to the BloomFilter implementation.
/// @details Traps with @p what if @p obj is NULL or not a BloomFilter.
/// @param obj Opaque runtime object handle to validate.
/// @param what Trap message used when validation fails.
/// @return The validated implementation pointer, or NULL if a returning trap
///         handler resumes after failed validation.
static rt_bloomfilter_impl *as_bloomfilter(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_BLOOMFILTER_CLASS_ID, sizeof(rt_bloomfilter_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_bloomfilter_impl *)obj;
}

/// @brief Validate a string item and borrow its complete byte span.
/// @param item Nonnull runtime string handle.
/// @param what Diagnostic raised for a stale or forged handle.
/// @param out_data Receives borrowed string bytes.
/// @param out_len Receives the byte count.
/// @return Nonzero for a live string handle; otherwise zero after trapping.
static int bloom_string_data(rt_string item,
                             const char *what,
                             const char **out_data,
                             size_t *out_len) {
    *out_data = "";
    *out_len = 0;
    if (!rt_string_is_handle(item)) {
        rt_trap(what);
        return 0;
    }
    int64_t len = rt_str_len(item);
    const char *data = rt_string_cstr(item);
    if (len > 0 && !data) {
        rt_trap(what);
        return 0;
    }
    *out_data = data ? data : "";
    *out_len = len > 0 ? (size_t)len : 0;
    return 1;
}

/// @brief Count set bits in a byte (SWAR popcount; used for cardinality est.).
/// @param x Byte whose set bits are counted.
/// @return Population count in the range `[0, 8]`.
static int popcount8(uint8_t x) {
    x = (uint8_t)(x - ((x >> 1) & 0x55u));
    x = (uint8_t)((x & 0x33u) + ((x >> 2) & 0x33u));
    return (int)((x + (x >> 4)) & 0x0Fu);
}

// ---------------------------------------------------------------------------
// Hash functions (MurmurHash3-style with seed variation)
// ---------------------------------------------------------------------------

/// @brief Seeded 64-bit hash (MurmurHash3-style mix + fmix finalizer).
/// @details Varying @p seed yields the k independent hash positions a Bloom
///          filter needs from a single pass over @p data.
/// @param data Byte sequence to hash.
/// @param len Number of bytes in @p data.
/// @param seed Per-probe seed value.
/// @return Mixed 64-bit hash value.
static uint64_t bloom_hash(const char *data, size_t len, uint64_t seed) {
    uint64_t h = seed ^ (len * 0x9E3779B97F4A7C15ULL);
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)(unsigned char)data[i];
        h *= 0x9E3779B97F4A7C15ULL;
        h ^= h >> 27;
    }
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 31;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 32;
    return h;
}

// ---------------------------------------------------------------------------
// Finalizer
// ---------------------------------------------------------------------------

/// @brief GC finalizer: free the BloomFilter's bit array.
/// @param obj BloomFilter object being finalized, or NULL for a no-op.
static void bloomfilter_finalizer(void *obj) {
    rt_bloomfilter_impl *bf =
        obj ? as_bloomfilter(obj, "BloomFilter: invalid BloomFilter object") : NULL;
    if (!bf)
        return;
    if (bf->bits) {
        free(bf->bits);
        bf->bits = NULL;
    }
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/// @brief Creates an empty BloomFilter with parameters derived from a target workload.
/// @param expected_items Expected number of add operations. Zero is normalized
///        to one; negative values trap.
/// @param false_positive_rate Requested probability. Non-finite or non-positive
///        values use 0.01, while values at least 1.0 use 0.5.
/// @return A new runtime-managed BloomFilter, or NULL after a size or allocation
///         trap.
void *rt_bloomfilter_new(int64_t expected_items, double false_positive_rate) {
    if (expected_items < 0) {
        rt_trap("BloomFilter: negative expected item count");
        return NULL;
    }
    if (expected_items == 0)
        expected_items = 1;
    if (!isfinite(false_positive_rate) || false_positive_rate <= 0.0)
        false_positive_rate = 0.01;
    if (false_positive_rate >= 1.0)
        false_positive_rate = 0.5;

    // Optimal bit count: m = -n * ln(p) / (ln(2)^2)
    double n = (double)expected_items;
    double m = -n * log(false_positive_rate) / (log(2.0) * log(2.0));
    if (!isfinite(m) || m > (double)INT64_MAX) {
        rt_trap("BloomFilter: size overflow");
        return NULL;
    }
    int64_t bit_count = (int64_t)ceil(m);
    if (bit_count < 64)
        bit_count = 64;

    // Optimal hash count: k = (m/n) * ln(2)
    double k = ((double)bit_count / n) * log(2.0);
    int64_t hash_count = (int64_t)ceil(k);
    if (hash_count < 1)
        hash_count = 1;
    if (hash_count > 30)
        hash_count = 30;

    if (bit_count > INT64_MAX - 7) {
        rt_trap("BloomFilter: size overflow");
        return NULL;
    }
    int64_t byte_count = (bit_count + 7) / 8;
    if ((uint64_t)byte_count > SIZE_MAX) {
        rt_trap("BloomFilter: allocation size overflow");
        return NULL;
    }

    rt_bloomfilter_impl *bf =
        (rt_bloomfilter_impl *)rt_obj_new_i64(RT_BLOOMFILTER_CLASS_ID, sizeof(rt_bloomfilter_impl));
    if (!bf) {
        rt_trap("BloomFilter: memory allocation failed");
        return NULL;
    }
    bf->bits = (uint8_t *)calloc((size_t)byte_count, 1);
    if (!bf->bits) {
        if (rt_obj_release_check0(bf))
            rt_obj_free(bf);
        rt_trap("BloomFilter: memory allocation failed");
        return NULL;
    }
    bf->bit_count = bit_count;
    bf->hash_count = hash_count;
    bf->item_count = 0;

    rt_obj_set_finalizer(bf, bloomfilter_finalizer);
    return bf;
}

// ---------------------------------------------------------------------------
// Add / query
// ---------------------------------------------------------------------------

/// @brief Add an element's hash to the Bloom filter.
/// @details Sets the bits at positions determined by multiple hash functions.
///          After adding, the element will always test positive with
///          might_contain (no false negatives).
/// @param filter BloomFilter handle, or NULL for a no-op.
/// @param item Runtime string to hash; NULL is ignored.
/// @note The operation count increases even if @p item was previously added.
/// @note Invalid non-null handles and count overflow raise a runtime trap.
void rt_bloomfilter_add(void *filter, rt_string item) {
    if (!filter || !item)
        return;
    rt_bloomfilter_impl *bf = as_bloomfilter(filter, "BloomFilter.Add: invalid BloomFilter object");
    if (!bf)
        return;

    size_t len = 0;
    const char *data = NULL;
    if (!bloom_string_data(item, "BloomFilter.Add: invalid string", &data, &len))
        return;

    if (bf->item_count == INT64_MAX) {
        rt_trap("BloomFilter: item count overflow");
        return;
    }
    for (int64_t i = 0; i < bf->hash_count; i++) {
        uint64_t h = bloom_hash(data, len, (uint64_t)i);
        int64_t pos = (int64_t)(h % (uint64_t)bf->bit_count);
        bf->bits[pos / 8] |= (uint8_t)(1 << (pos % 8));
    }
    bf->item_count++;
}

/// @brief Test whether an element might be in the Bloom filter.
/// @details Returns true if all bits for the element's hashes are set.
///          May return false positives but never false negatives.
/// @param filter BloomFilter handle, or NULL to query an empty filter.
/// @param item Runtime string to test; NULL returns 0.
/// @return 1 if the item may have been added, or 0 if it is definitely absent
///         or either argument is NULL.
int8_t rt_bloomfilter_might_contain(void *filter, rt_string item) {
    if (!filter || !item)
        return 0;
    rt_bloomfilter_impl *bf =
        as_bloomfilter(filter, "BloomFilter.MightContain: invalid BloomFilter object");
    if (!bf)
        return 0;

    size_t len = 0;
    const char *data = NULL;
    if (!bloom_string_data(item, "BloomFilter.MightContain: invalid string", &data, &len))
        return 0;

    for (int64_t i = 0; i < bf->hash_count; i++) {
        uint64_t h = bloom_hash(data, len, (uint64_t)i);
        int64_t pos = (int64_t)(h % (uint64_t)bf->bit_count);
        if (!(bf->bits[pos / 8] & (1 << (pos % 8))))
            return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

/// @brief Return the number of elements that have been added to the filter.
/// @details This is an exact count of add operations, not a cardinality
///          estimate from the bit array.
/// @param filter BloomFilter handle, or NULL to query an empty filter.
/// @return Number of successful non-null add calls, including duplicates, or
///         zero when @p filter is NULL.
int64_t rt_bloomfilter_count(void *filter) {
    if (!filter)
        return 0;
    rt_bloomfilter_impl *bf =
        as_bloomfilter(filter, "BloomFilter.Count: invalid BloomFilter object");
    return bf ? bf->item_count : 0;
}

/// @brief Estimate the current false positive rate of the Bloom filter.
/// @details Computed from the number of set bits, total bits, and number
///          of hash functions using the standard Bloom filter FPR formula.
/// @param filter BloomFilter handle, or NULL to query an empty filter.
/// @return Estimated probability in `[0.0, 1.0]`, or 0.0 for NULL.
double rt_bloomfilter_fpr(void *filter) {
    if (!filter)
        return 0.0;
    rt_bloomfilter_impl *bf = as_bloomfilter(filter, "BloomFilter.Fpr: invalid BloomFilter object");
    if (!bf)
        return 0.0;

    int64_t set_bits = 0;
    int64_t byte_count = (bf->bit_count + 7) / 8;
    for (int64_t i = 0; i < byte_count; i++)
        set_bits += popcount8(bf->bits[i]);

    if (set_bits <= 0)
        return 0.0;
    double fill = (double)set_bits / (double)bf->bit_count;
    if (fill >= 1.0)
        return 1.0;
    return pow(fill, (double)bf->hash_count);
}

/// @brief Reset the Bloom filter by clearing all bits and the element count.
/// @details After clearing, might_contain returns false for all inputs.
/// @param filter BloomFilter handle, or NULL for a no-op.
/// @note The derived bit and hash counts remain unchanged for reuse.
void rt_bloomfilter_clear(void *filter) {
    if (!filter)
        return;
    rt_bloomfilter_impl *bf =
        as_bloomfilter(filter, "BloomFilter.Clear: invalid BloomFilter object");
    if (!bf)
        return;
    int64_t byte_count = (bf->bit_count + 7) / 8;
    memset(bf->bits, 0, (size_t)byte_count);
    bf->item_count = 0;
}

/// @brief Merge another Bloom filter into this one (bitwise OR).
/// @details Both filters must have the same size and hash count.
///          After merging, this filter contains the union of both sets.
/// @param filter Destination BloomFilter modified in place.
/// @param other Source BloomFilter whose bits and operation count are added.
/// @return 1 on success, including a self-merge; 0 for NULL or incompatible
///         filters, or after reporting a count-overflow trap.
/// @note @p other is not modified. The combined count is the sum of add-call
///       counts and may double-count values present in both filters.
int64_t rt_bloomfilter_merge(void *filter, void *other) {
    if (!filter || !other)
        return 0;
    rt_bloomfilter_impl *a =
        as_bloomfilter(filter, "BloomFilter.Merge: invalid BloomFilter object");
    rt_bloomfilter_impl *b = as_bloomfilter(other, "BloomFilter.Merge: invalid BloomFilter object");
    if (!a || !b)
        return 0;
    if (filter == other)
        return 1;

    if (a->bit_count != b->bit_count || a->hash_count != b->hash_count)
        return 0;
    if (b->item_count > INT64_MAX - a->item_count) {
        rt_trap("BloomFilter: item count overflow");
        return 0;
    }

    int64_t byte_count = (a->bit_count + 7) / 8;
    for (int64_t i = 0; i < byte_count; i++)
        a->bits[i] |= b->bits[i];

    a->item_count += b->item_count;
    return 1;
}
