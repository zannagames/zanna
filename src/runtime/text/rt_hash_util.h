//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_hash_util.h
// Purpose: Header-only SipHash-2-4 computation backed by a process-wide
//          128-bit key initialized from the platform CSPRNG.
//
// Key invariants:
//   - Uses SipHash-2-4 with a 128-bit key seeded once per process.
//   - Output is deterministic within a single process run but varies between runs.
//   - The SipHash algorithm is inlined; seed state is shared via extern linkage
//     (defined in rt_hash_util.c) for consistent hashing across translation units.
//   - Used by rt_map, rt_bag, rt_countmap, rt_multimap, rt_bimap, rt_lrucache,
//     rt_intmap, rt_concmap, and rt_box.
//
// Ownership/Lifetime:
//   - No heap allocation; pure computation.
//   - No ownership transfer; input pointer is borrowed for the duration of the call.
//   - Seed state has process lifetime and is initialized on first use.
//
// Links: src/runtime/text/rt_hash_util.c (seed init),
//        src/runtime/collections/rt_map.h, src/runtime/collections/rt_bag.h (users)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_hash_util.h
 * @brief Provides inline SipHash-2-4 using a process-wide random key.
 * @details Collection and object hashing call the header-only round and byte
 *          processing helpers after thread-safe seed initialization. Hashing
 *          borrows its input, allocates no memory, remains deterministic within
 *          one process, and deliberately varies between process runs.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// SipHash-2-4 Per-Process Seed (defined in rt_hash_util.c)
//=============================================================================

extern uint64_t rt_siphash_k0_; ///< First 64-bit key half.
extern uint64_t rt_siphash_k1_; ///< Second 64-bit key half.
extern int rt_siphash_seeded_;  ///< Process-wide initialization publication flag.

/// @brief Ensure the SipHash key is seeded through the platform once primitive.
/// @details POSIX and compatible compilers use an acquire-load fast path before
///          `pthread_once`. Native MSVC calls `InitOnceExecuteOnce`
///          unconditionally, avoiding an unsynchronized read of the plain
///          integer publication flag. Entropy failure raises a runtime trap.
void rt_hash_ensure_seeded_(void);

//=============================================================================
// SipHash-2-4 Implementation
//=============================================================================

/// Execute one SipHash mixing round over local variables `v0` through `v3`.
#define RT_SIPROUND_                                                                               \
    do {                                                                                           \
        v0 += v1;                                                                                  \
        v1 = (v1 << 13) | (v1 >> 51);                                                              \
        v1 ^= v0;                                                                                  \
        v0 = (v0 << 32) | (v0 >> 32);                                                              \
        v2 += v3;                                                                                  \
        v3 = (v3 << 16) | (v3 >> 48);                                                              \
        v3 ^= v2;                                                                                  \
        v0 += v3;                                                                                  \
        v3 = (v3 << 21) | (v3 >> 43);                                                              \
        v3 ^= v0;                                                                                  \
        v2 += v1;                                                                                  \
        v1 = (v1 << 17) | (v1 >> 47);                                                              \
        v1 ^= v2;                                                                                  \
        v2 = (v2 << 32) | (v2 >> 32);                                                              \
    } while (0)

/// @brief Compute the SipHash-2-4 64-bit hash of a byte sequence.
/// @details Uses a per-process random 128-bit key for HashDoS resistance.
///          The algorithm processes 8-byte blocks with 2 compression rounds
///          and 4 finalization rounds. Input words are decoded in little-endian
///          order, making results independent of host byte order.
/// @param data Non-null pointer to the byte sequence to hash.
/// @param len Length of the byte sequence in bytes; may be zero.
/// @return 64-bit SipHash-2-4 hash value.
static inline uint64_t rt_siphash24(const void *data, size_t len) {
#if defined(_MSC_VER) && !defined(__clang__)
    // Real MSVC lacks __atomic_*; enter InitOnceExecuteOnce unconditionally
    // rather than racily reading the plain `int` flag on the fast path
    // (VDOC-176). InitOnceExecuteOnce is cheap after first completion and
    // provides the acquire/release synchronization for the key itself.
    rt_hash_ensure_seeded_();
#else
    if (!__atomic_load_n(&rt_siphash_seeded_, __ATOMIC_ACQUIRE))
        rt_hash_ensure_seeded_();
#endif

    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t k0 = rt_siphash_k0_;
    uint64_t k1 = rt_siphash_k1_;

    uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
    uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
    uint64_t v3 = k1 ^ 0x7465646279746573ULL;

    /* Process 8-byte blocks. */
    size_t blocks = len / 8;
    for (size_t i = 0; i < blocks; i++) {
        const uint8_t *block = bytes + (i * 8);
        uint64_t m = 0;
        for (int j = 0; j < 8; j++)
            m |= ((uint64_t)block[j]) << (j * 8);
        v3 ^= m;
        RT_SIPROUND_;
        RT_SIPROUND_;
        v0 ^= m;
    }

    /* Process remaining bytes + length tag. */
    uint64_t last = ((uint64_t)len) << 56;
    const uint8_t *tail = bytes + blocks * 8;
    size_t remain = len & 7;
    if (remain >= 7)
        last |= ((uint64_t)tail[6]) << 48;
    if (remain >= 6)
        last |= ((uint64_t)tail[5]) << 40;
    if (remain >= 5)
        last |= ((uint64_t)tail[4]) << 32;
    if (remain >= 4)
        last |= ((uint64_t)tail[3]) << 24;
    if (remain >= 3)
        last |= ((uint64_t)tail[2]) << 16;
    if (remain >= 2)
        last |= ((uint64_t)tail[1]) << 8;
    if (remain >= 1)
        last |= ((uint64_t)tail[0]);

    v3 ^= last;
    RT_SIPROUND_;
    RT_SIPROUND_;
    v0 ^= last;

    /* Finalization: 4 rounds. */
    v2 ^= 0xff;
    RT_SIPROUND_;
    RT_SIPROUND_;
    RT_SIPROUND_;
    RT_SIPROUND_;

    return v0 ^ v1 ^ v2 ^ v3;
}

/// @brief Runtime keyed hash for collection keys.
/// @details This is the preferred audit-friendly name for SipHash-2-4 over
///          arbitrary byte sequences. It avoids implying that callers receive a
///          deterministic FNV value.
/// @param data Non-null pointer to bytes to hash.
/// @param len Number of bytes to hash; may be zero.
/// @return 64-bit per-process-keyed SipHash value.
static inline uint64_t rt_keyed_hash_bytes(const void *data, size_t len) {
    return rt_siphash24(data, len);
}

/// @brief Backward-compatible internal name for the runtime's fast keyed hash.
/// @details Despite its historical name, this function delegates to
///          `rt_keyed_hash_bytes` and does not implement FNV-1a.
/// @param data Non-null pointer to bytes to hash.
/// @param len Number of bytes to hash; may be zero.
/// @return 64-bit per-process-keyed SipHash-2-4 value.
static inline uint64_t rt_fnv1a(const void *data, size_t len) {
    return rt_keyed_hash_bytes(data, len);
}

#undef RT_SIPROUND_

#ifdef __cplusplus
}
#endif
