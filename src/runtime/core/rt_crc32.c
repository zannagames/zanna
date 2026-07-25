//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_crc32.c
/// @file
/// @brief Implements thread-safe table-driven IEEE CRC-32 checksums.
///
// Purpose: Implements the CRC32 checksum (IEEE 802.3 / Ethernet polynomial
//          0xEDB88320) shared by the Zanna runtime's hash, compress, and
//          archive modules. Compatible with ZIP, PNG, GZIP, and other standard
//          formats that use the same polynomial.
//
// Key invariants:
//   - The 256-entry lookup table is computed once on first use via a double-
//     checked lock (acquire/release atomics), matching the pattern in
//     rt_context.c; concurrent callers spin until initialisation completes.
//   - Once initialised, the table is read-only; rt_crc32_compute is safe to
//     call from multiple threads concurrently.
//   - The CRC is computed with XOR pre/post-conditioning (initial value
//     0xFFFFFFFF, final XOR 0xFFFFFFFF) matching the IEEE standard.
//   - Input may be NULL only if len == 0; passing NULL with a non-zero length
//     produces undefined behaviour.
//
// Ownership/Lifetime:
//   - The lookup table is a process-global static array; no heap allocation
//     is performed at any point.
//   - rt_crc32_compute operates on caller-supplied buffers; it does not retain
//     the data pointer after returning.
//
// Links: src/runtime/core/rt_crc32.h (public API)
//
//===----------------------------------------------------------------------===//

#include "rt_crc32.h"

#include "rt_atomic_compat.h"

/// @brief CRC32 lookup table (256 entries for byte-at-a-time processing).
static uint32_t crc32_table[256];

/// @brief Atomic init state: 0=uninit, 1=initializing, 2=done.
/// @details Uses double-checked locking with acquire/release ordering so that
///          concurrent callers either wait for completion or see the fully
///          populated table. Matches the pattern in rt_context.c.
static int crc32_init_state = 0;

/// @brief Build the CRC32 lookup table (thread-safe, exactly-once).
/// @details Uses double-checked locking with atomic CAS: the first thread to
///          observe state=0 transitions to state=1, populates the 256-entry
///          table by computing the CRC of each possible byte value through 8
///          rounds of the IEEE polynomial, then transitions to state=2.
///          Concurrent callers spin until the table is ready. After init, the
///          table is read-only and subsequent calls return immediately.
/// @note The losing first-use callers busy-wait on the atomic state; callers
///       never need to invoke this function explicitly before computing a CRC.
void rt_crc32_init(void) {
    if (__atomic_load_n(&crc32_init_state, __ATOMIC_ACQUIRE) == 2)
        return;

    int expected = 0;
    if (__atomic_compare_exchange_n(
            &crc32_init_state, &expected, 1, /*weak=*/0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++) {
                if (crc & 1)
                    crc = 0xEDB88320 ^ (crc >> 1);
                else
                    crc >>= 1;
            }
            crc32_table[i] = crc;
        }
        __atomic_store_n(&crc32_init_state, 2, __ATOMIC_RELEASE);
        return;
    }

    // Another thread is initializing; spin until done.
    while (__atomic_load_n(&crc32_init_state, __ATOMIC_ACQUIRE) != 2) {
        // spin
    }
}

/// @brief Compute the CRC32 checksum of a byte buffer.
/// @details Lazily initializes the lookup table on first call, then processes
///          each byte through the table-driven algorithm. The initial CRC is
///          pre-conditioned to 0xFFFFFFFF and post-conditioned by XOR with
///          0xFFFFFFFF (the IEEE 802.3 standard "complement" convention). This
///          ensures that leading/trailing zeros affect the checksum — without
///          the conditioning, prepending zero bytes would not change the output.
/// @param data Pointer to the input buffer (may be NULL when len is 0).
/// @param len Number of bytes to checksum.
/// @return 32-bit CRC matching zlib crc32() output for the same input.
/// @pre @p data is nonnull whenever @p len is nonzero.
/// @note The checksum of an empty span is zero.
uint32_t rt_crc32_compute(const uint8_t *data, size_t len) {
    rt_crc32_init();

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}
