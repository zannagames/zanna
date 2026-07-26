//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PkgGzip.cpp
// Purpose: GZIP compression — 10-byte header + DEFLATE + 8-byte CRC/size
//          trailer. Ported from rt_compress.c gzip_data().
//
// Key invariants:
//   - Header uses method=DEFLATE(08), flags=0, mtime=0, OS=0xFF(unknown).
//   - CRC-32 is computed on the uncompressed input data.
//   - Size in trailer is original size mod 2^32.
//
// Ownership/Lifetime:
//   - Returned vector owns all memory.
//
// Links: PkgDeflate.hpp (DEFLATE), src/runtime/core/rt_crc32.h (CRC-32)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements RFC 1952 GZIP framing around the package DEFLATE codec.
/// @details Generates deterministic headers, parses optional header fields, and
///          validates both CRC-32 and ISIZE trailers during decompression.

#include "PkgGzip.hpp"
#include "PkgDeflate.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

// rt_crc32 has no GC dependency — link directly
extern "C" {
uint32_t rt_crc32_compute(const uint8_t *data, size_t len);
}

namespace zanna::pkg {

namespace {

static constexpr size_t kGzipMaxOutput = 0xFFFFFFFFull;
static_assert(kGzipMaxOutput == std::numeric_limits<uint32_t>::max(),
              "gzip trailer ISIZE is a 32-bit field");

/// @brief Read a little-endian uint16_t from a possibly-unaligned byte pointer.
/// @param p Address of at least two readable bytes.
/// @return Decoded 16-bit value.
uint16_t rdLE16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

/// @brief Read a little-endian uint32_t from a possibly-unaligned byte pointer.
/// @param p Address of at least four readable bytes.
/// @return Decoded 32-bit value.
uint32_t rdLE32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/// @brief Compute CRC-32 over a buffer, tolerating a null pointer when empty.
/// @details Delegates to the runtime rt_crc32_compute; passes a dummy byte for
///          zero-length input so the call never dereferences a null pointer.
/// @param data Borrowed input bytes, optionally null when `len` is zero.
/// @param len Input length.
/// @return Standard CRC-32 checksum.
uint32_t crc32Bytes(const uint8_t *data, size_t len) {
    static constexpr uint8_t kEmpty = 0;
    return rt_crc32_compute(len == 0 ? &kEmpty : data, len);
}

} // namespace

/// @brief Wrap raw DEFLATE output in a GZIP container (RFC 1952).
/// Header: method=8, flags=0, mtime=0, OS=0xFF. Trailer: CRC-32 + original size (little-endian).
/// @param data Borrowed uncompressed bytes.
/// @param len Input length, limited by the 32-bit ISIZE field.
/// @param level DEFLATE compression level.
/// @return Complete GZIP member bytes.
/// @throws std::runtime_error If a non-empty input is null or a size exceeds
///         the GZIP representation.
/// @throws DeflateError If raw compression fails.
std::vector<uint8_t> gzip(const uint8_t *data, size_t len, int level) {
    if (len > 0 && data == nullptr)
        throw std::runtime_error("gzip: null data pointer for non-empty input");
    if (len > 0xFFFFFFFFull)
        throw std::runtime_error("gzip: input too large for gzip ISIZE field");
    // Compress with raw DEFLATE
    auto deflated = deflate(data, len, level);
    if (deflated.size() > kGzipMaxOutput - 18)
        throw std::runtime_error("gzip: compressed output is too large");

    // CRC-32 of original data
    uint32_t crc = crc32Bytes(data, len);

    // Assemble: 10-byte header + deflated + 8-byte trailer
    size_t totalLen = 10 + deflated.size() + 8;
    std::vector<uint8_t> result(totalLen);
    uint8_t *out = result.data();

    // GZIP header (RFC 1952)
    out[0] = 0x1F; // Magic
    out[1] = 0x8B; // Magic
    out[2] = 0x08; // Compression method (deflate)
    out[3] = 0x00; // Flags (none)
    out[4] = 0x00; // MTIME
    out[5] = 0x00;
    out[6] = 0x00;
    out[7] = 0x00;
    out[8] = 0x00; // XFL
    out[9] = 0xFF; // OS (unknown)

    // Compressed data
    std::memcpy(out + 10, deflated.data(), deflated.size());

    // Trailer: CRC-32 + original size (both little-endian)
    size_t tp = 10 + deflated.size();
    out[tp + 0] = crc & 0xFF;
    out[tp + 1] = (crc >> 8) & 0xFF;
    out[tp + 2] = (crc >> 16) & 0xFF;
    out[tp + 3] = (crc >> 24) & 0xFF;
    out[tp + 4] = len & 0xFF;
    out[tp + 5] = (len >> 8) & 0xFF;
    out[tp + 6] = (len >> 16) & 0xFF;
    out[tp + 7] = (len >> 24) & 0xFF;

    return result;
}

/// @brief Validate and decompress a GZIP stream (RFC 1952).
/// Parses the 10-byte header, skips optional FEXTRA/FNAME/FCOMMENT/FHCRC fields, inflates
/// the embedded DEFLATE payload, then verifies CRC-32 and size in the 8-byte trailer.
/// Throws `std::runtime_error` on any header, trailer, or integrity violation.
/// @param data Borrowed GZIP member bytes.
/// @param len Compressed input length.
/// @return Decompressed payload bytes.
/// @throws std::runtime_error If the header, optional fields, trailer, CRC, or size is invalid.
/// @throws DeflateError If the embedded raw stream cannot be decompressed.
std::vector<uint8_t> gunzip(const uint8_t *data, size_t len) {
    if (!data || len < 18)
        throw std::runtime_error("gzip: stream too small");
    if (data[0] != 0x1F || data[1] != 0x8B)
        throw std::runtime_error("gzip: missing magic header");
    if (data[2] != 0x08)
        throw std::runtime_error("gzip: unsupported compression method");
    const uint8_t flags = data[3];
    if ((flags & 0xE0) != 0)
        throw std::runtime_error("gzip: reserved flags are set");

    size_t pos = 10;
    if (flags & 0x04) {
        if (pos + 2 > len)
            throw std::runtime_error("gzip: truncated extra field");
        const uint16_t xlen = rdLE16(data + pos);
        pos += 2;
        if (pos + xlen > len)
            throw std::runtime_error("gzip: truncated extra field payload");
        pos += xlen;
    }
    /// @brief Skip one NUL-terminated optional gzip header field.
    /// @param field Field name used in truncation diagnostics.
    /// @throws std::runtime_error If no terminator exists within the input.
    auto skipZString = [&](const char *field) {
        while (pos < len && data[pos] != 0)
            ++pos;
        if (pos >= len)
            throw std::runtime_error(std::string("gzip: unterminated ") + field);
        ++pos;
    };
    if (flags & 0x08)
        skipZString("filename");
    if (flags & 0x10)
        skipZString("comment");
    if (flags & 0x02) {
        if (pos + 2 > len)
            throw std::runtime_error("gzip: truncated header CRC");
        pos += 2;
    }
    if (pos + 8 > len)
        throw std::runtime_error("gzip: missing trailer");

    const uint32_t expectedCrc = rdLE32(data + len - 8);
    const uint32_t expectedSize = rdLE32(data + len - 4);

    const size_t deflateLen = len - pos - 8;
    auto out = inflate(data + pos, deflateLen, expectedSize);
    const uint32_t actualCrc = crc32Bytes(out.data(), out.size());
    if (actualCrc != expectedCrc)
        throw std::runtime_error("gzip: CRC-32 mismatch");
    if (static_cast<uint32_t>(out.size()) != expectedSize)
        throw std::runtime_error("gzip: uncompressed size mismatch");
    return out;
}

} // namespace zanna::pkg
