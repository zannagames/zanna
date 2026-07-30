//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_bitset.c
// Purpose: Implements an arbitrary-size bit array backed by a uint64_t word
//   array. Provides individual bit get/set/clear/toggle operations, bitwise
//   set operations (AND, OR, XOR, NOT), popcount, and automatic growth when
//   accessing a bit index beyond the current capacity.
//
// Key invariants:
//   - Each word stores BITS_PER_WORD (64) bits; word_count = ceil(bit_count/64).
//   - Growth doubles the word array (or allocates the minimum required); newly
//     added words are zero-filled.
//   - Bit index `i` lives in words[i/64] at bit position i%64.
//   - AND/OR/XOR accept unequal lengths (missing bits read as 0) and, like
//     NOT, return a new bitset without modifying either operand.
//   - Popcount uses __builtin_popcountll on GCC/Clang; falls back to a portable
//     Hamming-weight algorithm on other compilers.
//   - Not thread-safe; external synchronization required for concurrent access.
//
// Ownership/Lifetime:
//   - BitSet objects are GC-managed (rt_obj_new_i64). The words array is
//     realloc-managed and freed by the GC finalizer (bitset_finalizer).
//
// Links: src/runtime/collections/rt_bitset.h (public API)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime's dynamically growing BitSet collection.
///
/// Logical bits are packed into 64-bit words with bit index zero stored in the
/// least-significant bit of the first word. The logical bit count is distinct
/// from allocated word capacity: growth may reserve extra zeroed words, but
/// whole-set operations expose only logical bits. Setting or toggling beyond
/// the logical end extends it; reads and clears outside it do not.
///
/// Binary operations allocate independent results sized to the longer
/// operand, treating absent high bits as zero. Handles are runtime-managed and
/// validated against `RT_BITSET_CLASS_ID`. Mutation is unsynchronized.

#include "rt_bitset.h"

#include "rt_collection_ids.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <stdlib.h>
#include <string.h>

/// Bits per word.
#define BITS_PER_WORD 64

/// Convert bit count to word count (ceiling division).
#define WORDS_FOR_BITS(n) (((n) + BITS_PER_WORD - 1) / BITS_PER_WORD)

/// @brief BitSet implementation structure.
typedef struct rt_bitset_impl {
    void **vptr;       ///< Vtable pointer placeholder.
    uint64_t *words;   ///< Array of 64-bit words storing the bits.
    size_t word_count; ///< Number of words allocated.
    size_t bit_count;  ///< Logical number of bits.
} rt_bitset_impl;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief Checked cast of an opaque handle to the BitSet implementation.
/// @details Traps with the @p what message if @p obj is NULL or not a BitSet.
/// @param obj Opaque runtime object handle to validate.
/// @param what Trap message used on validation failure.
/// @return The validated implementation pointer, or NULL if a returning trap
///         handler resumes after failed validation.
static rt_bitset_impl *as_bitset(void *obj, const char *what) {
    if (!rt_obj_is_instance(obj, RT_BITSET_CLASS_ID, sizeof(rt_bitset_impl))) {
        rt_trap(what);
        return NULL;
    }
    return (rt_bitset_impl *)obj;
}

/// @brief Counts set bits in one 64-bit word.
/// @param x Word whose population is counted.
/// @return Number of one bits in @p x, in the range `[0, 64]`.
static int popcount64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#else
    // Hamming weight
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
#endif
}

/// @brief Grows the bitset to expose at least @p min_bits logical bits.
/// @param bs BitSet implementation to grow.
/// @param min_bits Required logical bit count.
/// @return 1 after satisfying the request, or 0 after an overflow or
///         allocation trap with the original storage and logical size intact.
static int bitset_grow(rt_bitset_impl *bs, size_t min_bits) {
    if (min_bits > SIZE_MAX - (BITS_PER_WORD - 1)) {
        rt_trap("BitSet: bit capacity overflow");
        return 0;
    }
    size_t new_word_count = WORDS_FOR_BITS(min_bits);
    if (new_word_count <= bs->word_count) {
        if (min_bits > bs->bit_count)
            bs->bit_count = min_bits;
        return 1;
    }

    // Double or use min, whichever is larger (with overflow guards)
    size_t grow;
    if (bs->word_count > SIZE_MAX / 2)
        grow = new_word_count; // Can't double safely, use exact fit
    else
        grow = bs->word_count * 2;
    if (grow < new_word_count)
        grow = new_word_count;
    if (grow > SIZE_MAX / sizeof(uint64_t)) {
        rt_trap("BitSet: allocation size overflow");
        return 0;
    }

    uint64_t *new_words = (uint64_t *)realloc(bs->words, grow * sizeof(uint64_t));
    if (!new_words) {
        rt_trap("BitSet: memory allocation failed");
        return 0;
    }

    // Zero new words
    memset(new_words + bs->word_count, 0, (grow - bs->word_count) * sizeof(uint64_t));

    bs->words = new_words;
    bs->word_count = grow;
    bs->bit_count = min_bits;
    return 1;
}

/// @brief Number of words the logical bit_count occupies (VDOC-104).
/// @details word_count tracks the ALLOCATED capacity (growth doubles), which
///          can exceed the logical size; every whole-set walk must use this
///          bound so spare capacity words stay invisible.
/// @param bs BitSet whose logical extent is measured.
/// @return Ceiling of the logical bit count divided by 64.
static size_t bitset_logical_words(const rt_bitset_impl *bs) {
    return WORDS_FOR_BITS(bs->bit_count);
}

// ---------------------------------------------------------------------------
// Finalizer
// ---------------------------------------------------------------------------

/// @brief GC finalizer: free the BitSet's backing word array.
/// @param obj BitSet object being finalized, or NULL for a no-op.
static void rt_bitset_finalize(void *obj) {
    if (!obj)
        return;
    rt_bitset_impl *bs = as_bitset(obj, "BitSet: invalid BitSet object");
    free(bs->words);
    bs->words = NULL;
    bs->word_count = 0;
    bs->bit_count = 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// @brief Creates a zero-filled BitSet with a requested logical length.
/// @param nbits Initial logical bit count; zero selects 64 bits and a negative
///        value raises a runtime trap.
/// @return A new runtime-managed BitSet, or NULL after a size or allocation
///         failure.
void *rt_bitset_new(int64_t nbits) {
    if (nbits < 0) {
        rt_trap("BitSet: negative length");
        return NULL;
    }
    if (nbits == 0)
        nbits = 64; // Default to 64 bits

    if ((uint64_t)nbits > SIZE_MAX - (BITS_PER_WORD - 1)) {
        rt_trap("BitSet: bit capacity overflow");
        return NULL;
    }
    size_t wc = WORDS_FOR_BITS((size_t)nbits);
    if (wc > SIZE_MAX / sizeof(uint64_t)) {
        rt_trap("BitSet: allocation size overflow");
        return NULL;
    }

    rt_bitset_impl *bs =
        (rt_bitset_impl *)rt_obj_new_i64(RT_BITSET_CLASS_ID, (int64_t)sizeof(rt_bitset_impl));
    if (!bs)
        return NULL;

    bs->vptr = NULL;
    bs->words = (uint64_t *)calloc(wc, sizeof(uint64_t));
    if (!bs->words) {
        if (rt_obj_release_check0(bs))
            rt_obj_free(bs);
        rt_trap("BitSet: memory allocation failed");
        return NULL;
    }
    bs->word_count = wc;
    bs->bit_count = (size_t)nbits;
    rt_obj_set_finalizer(bs, rt_bitset_finalize);
    return bs;
}

/// @brief Total bit capacity (the highest valid index + 1). Grows automatically on `_set`.
/// @param obj BitSet handle, or NULL to query a zero-length set.
/// @return Logical bit count, or zero when @p obj is NULL.
/// @note Invalid non-null handles raise a runtime trap.
int64_t rt_bitset_len(void *obj) {
    if (!obj)
        return 0;
    rt_bitset_impl *bitset = as_bitset(obj, "BitSet.Len: invalid BitSet object");
    return bitset ? (int64_t)bitset->bit_count : 0;
}

/// @brief Population count: number of bits set to 1 across the entire bitset.
/// @param obj BitSet handle, or NULL to count an empty set.
/// @param what Trap message used for an invalid non-null handle.
/// @return Number of set logical bits, or zero when @p obj is NULL.
static int64_t bitset_popcount(void *obj, const char *what) {
    if (!obj)
        return 0;
    rt_bitset_impl *bs = as_bitset(obj, what);
    int64_t total = 0;
    size_t words = bitset_logical_words(bs);
    for (size_t i = 0; i < words; ++i)
        total += popcount64(bs->words[i]);
    return total;
}

/// @brief Returns the number of set logical bits.
/// @param obj BitSet handle, or NULL to count an empty set.
/// @return Population count, or zero when @p obj is NULL.
int64_t rt_bitset_count(void *obj) {
    return bitset_popcount(obj, "BitSet.Count: invalid BitSet object");
}

/// @brief Returns 1 if every bit is 0 (popcount == 0). O(n/64).
/// @param obj BitSet handle, or NULL to test an empty set.
/// @return 1 when no logical bit is set; otherwise 0.
int8_t rt_bitset_is_empty(void *obj) {
    return bitset_popcount(obj, "BitSet.IsEmpty: invalid BitSet object") == 0;
}

/// @brief Read the bit at `idx`. Returns 0 for out-of-range indices (no growth on read).
/// @param obj BitSet handle, or NULL to read an empty set.
/// @param idx Zero-based bit index.
/// @return 1 if the indexed bit is set; otherwise 0. Negative and
///         out-of-range indices return 0.
int8_t rt_bitset_get(void *obj, int64_t idx) {
    if (!obj || idx < 0)
        return 0;
    rt_bitset_impl *bs = as_bitset(obj, "BitSet.Get: invalid BitSet object");
    if ((size_t)idx >= bs->bit_count)
        return 0;
    size_t w = (size_t)idx / BITS_PER_WORD;
    size_t b = (size_t)idx % BITS_PER_WORD;
    return (bs->words[w] >> b) & 1;
}

/// @brief Set the bit at `idx` to 1. Auto-grows the underlying word array if `idx` is past
/// the current bit_count.
/// @param obj BitSet handle, or NULL for a no-op.
/// @param idx Non-negative zero-based bit index; negative indices are ignored.
/// @note Growth overflow and allocation failure raise a runtime trap without
///       changing the logical bit count.
void rt_bitset_set(void *obj, int64_t idx) {
    if (!obj || idx < 0)
        return;
    rt_bitset_impl *bs = as_bitset(obj, "BitSet.Set: invalid BitSet object");
    if ((size_t)idx >= bs->bit_count && !bitset_grow(bs, (size_t)idx + 1))
        return;
    size_t w = (size_t)idx / BITS_PER_WORD;
    size_t b = (size_t)idx % BITS_PER_WORD;
    if (w < bs->word_count)
        bs->words[w] |= (1ULL << b);
}

/// @brief Set the bit at `idx` to 0. Out-of-range indices are no-ops (no growth).
/// @param obj BitSet handle, or NULL for a no-op.
/// @param idx Zero-based bit index; negative and out-of-range values are
///        ignored.
void rt_bitset_clear(void *obj, int64_t idx) {
    if (!obj || idx < 0)
        return;
    rt_bitset_impl *bs = as_bitset(obj, "BitSet.Clear: invalid BitSet object");
    if ((size_t)idx >= bs->bit_count)
        return; // Nothing to clear
    size_t w = (size_t)idx / BITS_PER_WORD;
    size_t b = (size_t)idx % BITS_PER_WORD;
    bs->words[w] &= ~(1ULL << b);
}

/// @brief Flip the bit at `idx`. Auto-grows like `_set`.
/// @param obj BitSet handle, or NULL for a no-op.
/// @param idx Non-negative zero-based bit index; negative indices are ignored.
void rt_bitset_toggle(void *obj, int64_t idx) {
    if (!obj || idx < 0)
        return;
    rt_bitset_impl *bs = as_bitset(obj, "BitSet.Toggle: invalid BitSet object");
    if ((size_t)idx >= bs->bit_count && !bitset_grow(bs, (size_t)idx + 1))
        return;
    size_t w = (size_t)idx / BITS_PER_WORD;
    size_t b = (size_t)idx % BITS_PER_WORD;
    if (w < bs->word_count)
        bs->words[w] ^= (1ULL << b);
}

/// @brief Set every bit to 1 (within the current bit_count). Excess bits in the trailing word
/// are masked off so popcount remains exact.
/// @param obj BitSet handle, or NULL for a no-op.
void rt_bitset_set_all(void *obj) {
    if (!obj)
        return;
    rt_bitset_impl *bs = as_bitset(obj, "BitSet.SetAll: invalid BitSet object");
    size_t words = bitset_logical_words(bs);
    if (words == 0)
        return;
    memset(bs->words, 0xFF, words * sizeof(uint64_t));
    // Mask off excess bits in the last LOGICAL word; spare capacity words
    // beyond it stay zero (VDOC-104).
    size_t extra = bs->bit_count % BITS_PER_WORD;
    if (extra > 0)
        bs->words[words - 1] &= (1ULL << extra) - 1;
}

/// @brief Clear every bit to 0. O(n/64) memset; capacity is preserved.
/// @param obj BitSet handle, or NULL for a no-op.
void rt_bitset_clear_all(void *obj) {
    if (!obj)
        return;
    rt_bitset_impl *bs = as_bitset(obj, "BitSet.ClearAll: invalid BitSet object");
    if (bs->word_count == 0)
        return;
    memset(bs->words, 0, bs->word_count * sizeof(uint64_t));
}

/// @brief Bitwise AND of two bitsets — returns a fresh bitset where bit i is `a[i] & b[i]`.
/// Result size is `max(|a|, |b|)`; missing bits in either operand are treated as 0.
/// @param a First BitSet handle.
/// @param b Second BitSet handle.
/// @return A new independent result, or a new empty 64-bit BitSet if either
///         operand is NULL. Allocation failure returns NULL.
void *rt_bitset_and(void *a, void *b) {
    if (!a || !b)
        return rt_bitset_new(64);

    rt_bitset_impl *ba = as_bitset(a, "BitSet.And: invalid BitSet object");
    rt_bitset_impl *bb = as_bitset(b, "BitSet.And: invalid BitSet object");
    size_t max_bits = ba->bit_count > bb->bit_count ? ba->bit_count : bb->bit_count;

    void *result = rt_bitset_new((int64_t)max_bits);
    if (!result)
        return NULL;

    rt_bitset_impl *br = (rt_bitset_impl *)result;
    size_t wa = bitset_logical_words(ba);
    size_t wb = bitset_logical_words(bb);
    size_t min_words = wa < wb ? wa : wb;
    if (min_words > br->word_count)
        min_words = br->word_count;
    for (size_t i = 0; i < min_words; ++i)
        br->words[i] = ba->words[i] & bb->words[i];
    // Remaining words stay 0 (AND with 0 = 0)

    return result;
}

/// @brief Bitwise OR of two bitsets — bit i = `a[i] | b[i]`. Result extends to the longer set.
/// @param a First BitSet handle.
/// @param b Second BitSet handle.
/// @return A new independent result, or a new empty 64-bit BitSet if either
///         operand is NULL. Allocation failure returns NULL.
void *rt_bitset_or(void *a, void *b) {
    if (!a || !b)
        return rt_bitset_new(64);

    rt_bitset_impl *ba = as_bitset(a, "BitSet.Or: invalid BitSet object");
    rt_bitset_impl *bb = as_bitset(b, "BitSet.Or: invalid BitSet object");
    size_t max_bits = ba->bit_count > bb->bit_count ? ba->bit_count : bb->bit_count;

    void *result = rt_bitset_new((int64_t)max_bits);
    if (!result)
        return NULL;

    rt_bitset_impl *br = (rt_bitset_impl *)result;
    size_t wa = bitset_logical_words(ba);
    size_t wb = bitset_logical_words(bb);
    size_t min_words = wa < wb ? wa : wb;
    if (min_words > br->word_count)
        min_words = br->word_count;
    size_t i = 0;
    for (; i < min_words; ++i)
        br->words[i] = ba->words[i] | bb->words[i];
    // Copy remaining from the logically longer one
    rt_bitset_impl *longer = wa > wb ? ba : bb;
    size_t longer_words = wa > wb ? wa : wb;
    for (; i < longer_words && i < br->word_count; ++i)
        br->words[i] = longer->words[i];

    return result;
}

/// @brief Bitwise XOR of two bitsets — bit i = `a[i] ^ b[i]`. Useful for symmetric difference.
/// @param a First BitSet handle.
/// @param b Second BitSet handle.
/// @return A new independent result, or a new empty 64-bit BitSet if either
///         operand is NULL. Allocation failure returns NULL.
void *rt_bitset_xor(void *a, void *b) {
    if (!a || !b)
        return rt_bitset_new(64);

    rt_bitset_impl *ba = as_bitset(a, "BitSet.Xor: invalid BitSet object");
    rt_bitset_impl *bb = as_bitset(b, "BitSet.Xor: invalid BitSet object");
    size_t max_bits = ba->bit_count > bb->bit_count ? ba->bit_count : bb->bit_count;

    void *result = rt_bitset_new((int64_t)max_bits);
    if (!result)
        return NULL;

    rt_bitset_impl *br = (rt_bitset_impl *)result;
    size_t wa = bitset_logical_words(ba);
    size_t wb = bitset_logical_words(bb);
    size_t min_words = wa < wb ? wa : wb;
    if (min_words > br->word_count)
        min_words = br->word_count;
    size_t i = 0;
    for (; i < min_words; ++i)
        br->words[i] = ba->words[i] ^ bb->words[i];
    // XOR with 0 = copy
    rt_bitset_impl *longer = wa > wb ? ba : bb;
    size_t longer_words = wa > wb ? wa : wb;
    for (; i < longer_words && i < br->word_count; ++i)
        br->words[i] = longer->words[i];

    return result;
}

/// @brief Bitwise complement of `obj`. Result has the same bit_count; trailing word is masked
/// so excess bits past `bit_count` stay 0.
/// @param obj Source BitSet handle.
/// @return A new independent complement, or a new empty 64-bit BitSet when
///         @p obj is NULL. Allocation failure returns NULL.
void *rt_bitset_not(void *obj) {
    if (!obj)
        return rt_bitset_new(64);

    rt_bitset_impl *bs = as_bitset(obj, "BitSet.Not: invalid BitSet object");
    void *result = rt_bitset_new((int64_t)bs->bit_count);
    if (!result)
        return NULL;

    rt_bitset_impl *br = (rt_bitset_impl *)result;
    size_t src_words = bitset_logical_words(bs);
    for (size_t i = 0; i < src_words && i < br->word_count; ++i)
        br->words[i] = ~bs->words[i];

    // Mask off excess bits in the last logical word
    size_t words = bitset_logical_words(br);
    size_t extra = br->bit_count % BITS_PER_WORD;
    if (extra > 0 && words > 0)
        br->words[words - 1] &= (1ULL << extra) - 1;

    return result;
}

/// @brief Render the bitset as a binary string, MSB-first, leading zeros suppressed (always
/// at least one digit). Empty/null bitsets produce "0". Useful for debug printing or
/// hash-friendly serialization.
/// @param obj BitSet handle, or NULL to render an empty set.
/// @return A newly created binary string. Temporary-buffer allocation failure
///         falls back to a newly created `"0"` string.
rt_string rt_bitset_to_string(void *obj) {
    if (!obj)
        return rt_string_from_bytes("0", 1);

    rt_bitset_impl *bs = as_bitset(obj, "BitSet.ToString: invalid BitSet object");
    if (bs->bit_count == 0)
        return rt_string_from_bytes("0", 1);

    // Build string from MSB to LSB
    size_t len = bs->bit_count;
    char *buf = (char *)malloc(len + 1);
    if (!buf)
        return rt_string_from_bytes("0", 1);

    for (size_t i = 0; i < len; ++i) {
        size_t bit_idx = len - 1 - i;
        size_t w = bit_idx / BITS_PER_WORD;
        size_t b = bit_idx % BITS_PER_WORD;
        buf[i] = (w < bs->word_count && (bs->words[w] >> b) & 1) ? '1' : '0';
    }
    buf[len] = '\0';

    // Skip leading zeros (but keep at least one character)
    size_t start = 0;
    while (start < len - 1 && buf[start] == '0')
        ++start;

    rt_string result = rt_string_from_bytes(buf + start, len - start);
    free(buf);
    return result;
}
