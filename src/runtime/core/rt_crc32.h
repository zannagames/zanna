//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_crc32.h
/// @file
/// @brief Declares thread-safe table-driven IEEE CRC-32 checksum helpers.
///
// Purpose: CRC32 checksum computation using the IEEE 802.3 polynomial (0xEDB88320), compatible with
// Ethernet, ZIP, and PNG checksums.
//
// Key invariants:
//   - Uses the bit-reversed IEEE 802.3 polynomial, producing the same output as zlib crc32.
//   - The lookup table is initialized lazily on first call to rt_crc32_compute.
//   - Atomic once-state makes initialization safe for concurrent first calls.
//   - Input NULL with len 0 returns 0xFFFFFFFF XOR 0xFFFFFFFF = 0.
//
// Ownership/Lifetime:
//   - All functions are stateless except for the global lookup table (initialized once).
//   - No heap allocation; caller provides input buffer.
//
// Links: src/runtime/core/rt_crc32.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Initialize the CRC32 lookup table.
/// @details Thread-safe initialization using acquire/release atomic state and CAS.
///          Called automatically by rt_crc32_compute if needed.
void rt_crc32_init(void);

/// @brief Compute CRC32 checksum of data.
/// @details Uses the IEEE 802.3 polynomial (0xEDB88320, bit-reversed).
///          This is the same polynomial used by Ethernet, ZIP, PNG, etc.
/// @param data Pointer to data buffer.
/// @param len Length of data in bytes.
/// @return 32-bit CRC checksum.
/// @pre @p data is nonnull when @p len is nonzero; null is valid for an empty span.
/// @note Returns zero for an empty span and does not retain @p data.
uint32_t rt_crc32_compute(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
