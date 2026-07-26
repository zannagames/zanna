//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PkgZlib.hpp
// Purpose: Minimal RFC 1950 zlib framing around the packaging DEFLATE engine.
// Key invariants: Wraps PkgDeflate output with a 2-byte zlib header and a
//                 4-byte big-endian Adler-32 trailer; decompression verifies
//                 both the header and the checksum.
// Ownership/Lifetime: Output returned as std::vector<uint8_t> (caller-owned);
//                     input is read-only and not retained.
// Links: src/tools/common/packaging/PkgZlib.cpp, PkgDeflate.hpp, XarWriter.cpp
//
//===----------------------------------------------------------------------===//
#pragma once

/// @file
/// @brief Declares minimal RFC 1950 compression and decompression helpers.
/// @details These APIs wrap the repository-native DEFLATE engine and return
///          caller-owned byte vectors without retaining input pointers.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace zanna::pkg {

/// @brief Compress data into an RFC 1950 zlib stream.
/// @param data Input bytes to compress (may be null only when @p len is 0).
/// @param len Length of the input.
/// @param level DEFLATE compression level 1-9 (default 6).
/// @return The zlib-framed compressed stream (header + DEFLATE + Adler-32).
std::vector<uint8_t> zlibCompress(const uint8_t *data, size_t len, int level = 6);

/// @brief Decompress an RFC 1950 zlib stream with a known output size.
/// @param data zlib-framed compressed input.
/// @param len Length of the compressed input.
/// @param expectedSize Exact expected size of the decompressed output.
/// @return The decompressed bytes.
/// @throws std::runtime_error on an invalid header, a preset dictionary, a size
///         mismatch, or an Adler-32 mismatch.
std::vector<uint8_t> zlibDecompress(const uint8_t *data, size_t len, size_t expectedSize);

} // namespace zanna::pkg
