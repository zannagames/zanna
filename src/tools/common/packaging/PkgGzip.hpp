//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PkgGzip.hpp
// Purpose: GZIP compression (RFC 1952) for the packaging library.
//          Wraps DEFLATE output with GZIP header and CRC-32/size trailer.
//
// Key invariants:
//   - No runtime (zanna_rt_*) dependencies — uses PkgDeflate + rt_crc32.
//   - Produces standard GZIP streams decompressible by gunzip/zlib.
//
// Ownership/Lifetime:
//   - Output returned as std::vector<uint8_t> (caller-owned).
//
// Links: PkgDeflate.hpp, src/runtime/core/rt_crc32.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares RFC 1952 GZIP compression and validated decompression.
/// @details Inputs are borrowed for each call and output is returned in an owned
///          byte vector; the implementation uses the self-contained raw DEFLATE codec.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace zanna::pkg {

/// @brief Compress data with GZIP wrapper (RFC 1952).
/// @param data Input bytes to compress.
/// @param len Length of input data.
/// @param level DEFLATE compression level 1-9 (default 6).
/// @return GZIP-compressed stream.
/// @throws std::runtime_error If input pointers or GZIP field sizes are invalid.
/// @throws DeflateError If compression fails.
std::vector<uint8_t> gzip(const uint8_t *data, size_t len, int level = 6);

/// @brief Decompress a GZIP stream and validate its CRC/ISIZE trailer.
/// @param data GZIP-compressed input bytes.
/// @param len Length of the compressed input.
/// @return The decompressed bytes.
/// @throws std::runtime_error on malformed GZIP data or CRC/size mismatch.
std::vector<uint8_t> gunzip(const uint8_t *data, size_t len);

} // namespace zanna::pkg
