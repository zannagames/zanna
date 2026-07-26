//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PkgZlib.cpp
// Purpose: zlib framing for native XAR TOCs and per-file compression.
// Key invariants: Emits the standard 0x78 0x9C zlib header, defers compression
//                 to PkgDeflate, and appends a big-endian Adler-32 of the
//                 uncompressed data. Decompression rejects preset dictionaries.
// Ownership/Lifetime: Returned vectors own their memory; inputs are read-only.
// Links: PkgZlib.hpp, PkgDeflate.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements RFC 1950 framing around Zanna's native DEFLATE codec.
/// @details Compression emits a fixed zlib header and Adler-32 trailer;
///          decompression validates both before returning owned output bytes.

#include "PkgZlib.hpp"

#include "PkgDeflate.hpp"

#include <stdexcept>

namespace zanna::pkg {
namespace {

/// @brief Compute the Adler-32 checksum of a buffer (RFC 1950 trailer).
/// @details Maintains the two rolling sums modulo 65521 and packs them as
///          (b << 16) | a, matching the value stored in a zlib trailer.
/// @param data Input bytes; may be null only when @p len is zero.
/// @param len Number of bytes to checksum.
/// @return Adler-32 value in host byte order.
uint32_t adler32(const uint8_t *data, size_t len) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/// @brief Read four bytes at @p p as a big-endian 32-bit word.
/// @param p Pointer to at least four readable bytes.
/// @return Decoded host-order 32-bit value.
uint32_t readBE32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

} // namespace

/// @brief Frame DEFLATE output as a zlib stream (header + payload + Adler-32).
/// @details Writes the fixed 0x78 0x9C header, the raw DEFLATE bytes, then the
///          big-endian Adler-32 of the original input.
/// @param data Input bytes; may be null only when @p len is zero.
/// @param len Number of input bytes.
/// @param level DEFLATE compression level forwarded to the native encoder.
/// @return Complete RFC 1950 stream as an owned byte vector.
/// @throws std::runtime_error if non-empty input has a null pointer or the
///         underlying DEFLATE encoder rejects the request.
std::vector<uint8_t> zlibCompress(const uint8_t *data, size_t len, int level) {
    if (len != 0 && data == nullptr)
        throw std::runtime_error("zlib: null data pointer for non-empty input");

    auto deflated = deflate(data, len, level);
    std::vector<uint8_t> out;
    out.reserve(2 + deflated.size() + 4);
    out.push_back(0x78);
    out.push_back(0x9c);
    out.insert(out.end(), deflated.begin(), deflated.end());
    const uint32_t sum = adler32(data, len);
    out.push_back(static_cast<uint8_t>((sum >> 24) & 0xffu));
    out.push_back(static_cast<uint8_t>((sum >> 16) & 0xffu));
    out.push_back(static_cast<uint8_t>((sum >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>(sum & 0xffu));
    return out;
}

/// @brief Validate and decompress a zlib stream.
/// @details Checks the CMF/FLG header (method 8, FCHECK multiple of 31), rejects
///          preset dictionaries, inflates the payload to @p expectedSize, and
///          verifies the trailing Adler-32.
/// @param data Pointer to the complete zlib stream.
/// @param len Number of compressed bytes, including header and trailer.
/// @param expectedSize Exact required size of the inflated output.
/// @return Decompressed bytes as an owned vector.
/// @throws std::runtime_error for a null/truncated stream, invalid header,
///         preset dictionary, DEFLATE error, size mismatch, or checksum mismatch.
std::vector<uint8_t> zlibDecompress(const uint8_t *data, size_t len, size_t expectedSize) {
    if (data == nullptr || len < 6)
        throw std::runtime_error("zlib: stream too small");
    const uint8_t cmf = data[0];
    const uint8_t flg = data[1];
    if ((cmf & 0x0fu) != 8u || ((static_cast<uint16_t>(cmf) << 8) + flg) % 31u != 0)
        throw std::runtime_error("zlib: invalid header");
    if ((flg & 0x20u) != 0)
        throw std::runtime_error("zlib: preset dictionaries are not supported");

    auto out = inflate(data + 2, len - 6, expectedSize);
    if (out.size() != expectedSize)
        throw std::runtime_error("zlib: uncompressed size mismatch");
    const uint32_t expectedAdler = readBE32(data + len - 4);
    const uint32_t actualAdler = adler32(out.data(), out.size());
    if (expectedAdler != actualAdler)
        throw std::runtime_error("zlib: Adler-32 mismatch");
    return out;
}

} // namespace zanna::pkg
