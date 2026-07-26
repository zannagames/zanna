//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_zpak_format.h
// Purpose: Define the versioned, byte-level ZPAK asset-container constants
//          shared by the build-time writer and runtime reader.
//
// Key invariants:
//   - Every integer in a ZPAK file is encoded little-endian.
//   - Version 1 entries have 28 fixed bytes after their names.
//   - Version 2 appends one four-byte CRC-32 to each version 1 entry record.
//   - Unknown header and entry flag bits are rejected by current readers.
//
// Ownership/Lifetime:
//   - This header defines constants only and owns no process or heap state.
//
// Links: src/runtime/io/rt_zpak_reader.c,
//        src/tools/common/asset/ZpakWriter.cpp,
//        docs/adr/0134-zpak-v2-validation-and-entry-checksums.md
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Defines the versioned byte-level ZPAK archive format contract.
 * @details Centralizes header versions, supported flag masks, table-record
 * sizes, minimum encodings, and allocation ceilings shared by format writers
 * and runtime readers.
 */

#pragma once

#include <stdint.h>

/// @brief Encoded size of every supported ZPAK header.
/// @details The 32 bytes contain magic, version, flags, entry count, TOC
///          offset, TOC size, and a zero reserved word in that order.
enum { RT_ZPAK_HEADER_SIZE = 32 };

/// @brief Legacy ZPAK version without per-entry integrity checksums.
/// @details Version 1 permits only @ref RT_ZPAK_V1_HEADER_FLAGS and uses
///          @ref RT_ZPAK_V1_ENTRY_FIXED_SIZE fixed bytes per TOC entry.
enum { RT_ZPAK_VERSION_1 = 1 };

/// @brief Checksummed ZPAK version emitted by current writers.
/// @details Version 2 requires @ref RT_ZPAK_HEADER_FLAG_ENTRY_CRC32 and appends
///          the CRC-32 of each entry's original uncompressed bytes.
enum { RT_ZPAK_VERSION_2 = 2 };

/// @brief Format version that a new ZPAK writer must emit.
/// @details Readers still accept explicitly supported older versions.
enum { RT_ZPAK_VERSION_CURRENT = RT_ZPAK_VERSION_2 };

enum {
    /// Header flag indicating that at least one entry uses DEFLATE.
    /// Readers require this aggregate flag to agree with the per-entry flags.
    RT_ZPAK_HEADER_FLAG_COMPRESSED = UINT16_C(1) << 0,
    /// Version 2 header flag declaring one CRC-32 on every TOC entry.
    /// This flag is mandatory for every version 2 archive.
    RT_ZPAK_HEADER_FLAG_ENTRY_CRC32 = UINT16_C(1) << 1,
    /// Complete set of header flags understood for version 1.
    /// Any bit outside this mask makes a version 1 archive invalid.
    RT_ZPAK_V1_HEADER_FLAGS = RT_ZPAK_HEADER_FLAG_COMPRESSED,
    /// Complete set of header flags understood for version 2.
    /// Any bit outside this mask makes a version 2 archive invalid.
    RT_ZPAK_V2_HEADER_FLAGS = RT_ZPAK_HEADER_FLAG_COMPRESSED | RT_ZPAK_HEADER_FLAG_ENTRY_CRC32,
};

/// @brief Entry flag indicating that stored bytes contain a DEFLATE stream.
/// @details When clear, stored size must equal original size; when set, both
///          sizes must be nonzero and the reader inflates before verification.
enum { RT_ZPAK_ENTRY_FLAG_COMPRESSED = UINT16_C(1) << 0 };

/// @brief Complete set of per-entry flags understood by current readers.
/// @details An entry containing any bit outside this mask is rejected.
enum { RT_ZPAK_ENTRY_FLAGS_KNOWN = RT_ZPAK_ENTRY_FLAG_COMPRESSED };

/// @brief Fixed version 1 TOC bytes following an entry's variable-length name.
/// @details These bytes encode data offset, original size, stored size, entry
///          flags, and a zero reserved field as 8+8+8+2+2 bytes.
enum { RT_ZPAK_V1_ENTRY_FIXED_SIZE = 28 };

/// @brief Fixed version 2 TOC bytes following an entry's variable-length name.
/// @details The layout is the version 1 fixed record followed by a four-byte
///          CRC-32 of the original uncompressed payload.
enum { RT_ZPAK_V2_ENTRY_FIXED_SIZE = 32 };

/// @brief Smallest legal version 1 entry record, including length and one name byte.
/// @details Entry names are encoded as a two-byte length and non-empty UTF-8
///          bytes without a NUL terminator.
enum { RT_ZPAK_V1_ENTRY_MIN_SIZE = 2 + 1 + RT_ZPAK_V1_ENTRY_FIXED_SIZE };

/// @brief Smallest legal version 2 entry record, including length and one name byte.
/// @details Used to reject impossible entry counts before count-sized allocation.
enum { RT_ZPAK_V2_ENTRY_MIN_SIZE = 2 + 1 + RT_ZPAK_V2_ENTRY_FIXED_SIZE };

/// @brief Maximum accepted encoded table-of-contents size in bytes.
/// @details Readers reject TOCs larger than 64 MiB before allocating storage,
///          independently of the declared entry count.
#define RT_ZPAK_MAX_TOC_SIZE UINT64_C(67108864)
