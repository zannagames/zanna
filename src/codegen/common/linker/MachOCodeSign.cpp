//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/MachOCodeSign.cpp
// Purpose: Ad-hoc code signature generation for Mach-O arm64 executables.
//          Builds an Apple-compatible embedded signature SuperBlob with
//          SHA-256 page hashes.
// Key invariants:
//   - Matches Apple's ad-hoc layout: CodeDirectory + Requirements +
//     empty BlobWrapper
//   - SHA-256 hash computed per target page size with a self-contained,
//     dependency-free implementation (identical output on every build host, so
//     arm64 binaries can be cross-signed off non-Apple hosts)
//   - CodeDirectory version 0x20400 (execSegBase/Limit/Flags)
// Ownership/Lifetime:
//   - Stateless builder — no persistent state
// Links: codegen/common/linker/MachOCodeSign.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file MachOCodeSign.cpp
 * @brief Implements reproducible SHA-256 Mach-O ad-hoc signature blobs.
 */

#include "codegen/common/linker/MachOCodeSign.hpp"
#include "codegen/common/linker/ExeWriterUtil.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace zanna::codegen::linker {

using encoding::writeBE32;
using encoding::writeBE64;

namespace {

static constexpr uint32_t CSMAGIC_EMBEDDED_SIGNATURE = 0xFADE0CC0;
static constexpr uint32_t CSMAGIC_CODEDIRECTORY = 0xFADE0C02;
static constexpr uint32_t CSMAGIC_REQUIREMENTS = 0xFADE0C01;
static constexpr uint32_t CSMAGIC_BLOBWRAPPER = 0xFADE0B01;
static constexpr uint32_t CSSLOT_CODEDIRECTORY = 0;
static constexpr uint32_t CSSLOT_REQUIREMENTS = 2;
static constexpr uint32_t CSSLOT_SIGNATURESLOT = 0x10000;
static constexpr uint32_t CS_ADHOC = 0x2;
static constexpr uint32_t CS_EXECSEG_MAIN_BINARY = 0x1;
static constexpr uint32_t CD_VERSION = 0x20400;
static constexpr uint32_t kCodeDirectoryHeaderSize = 88;
static constexpr uint32_t kHashSize = 32;
static constexpr uint32_t kSpecialSlots = 2;
static constexpr uint32_t kRequirementsSize = 12;
static constexpr uint32_t kBlobWrapperSize = 8;
static constexpr uint32_t kSuperBlobHeaderSize = 12 + 3 * 8;

/// @brief Rotates a 32-bit SHA-256 word right.
/// @param value Word to rotate.
/// @param bits Rotation count in the range 1 through 31.
/// @return Rotated word.
inline uint32_t ror32(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

/// @brief Reads an unaligned big-endian 32-bit word.
/// @param p Pointer to at least four readable bytes.
/// @return Host-order word.
inline uint32_t readBE32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/// @brief Computes the SHA-256 digest of a byte range.
/// @details This self-contained FIPS 180-4 implementation makes signatures
///          byte-identical on every build host and avoids a host crypto
///          dependency during cross-signing.
/// @param data Pointer to @p len readable bytes.
/// @param len Input length in bytes.
/// @param out Caller-provided 32-byte digest buffer.
void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    static constexpr std::array<uint32_t, 64> k = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u};

    uint32_t h[8] = {0x6a09e667u,
                     0xbb67ae85u,
                     0x3c6ef372u,
                     0xa54ff53au,
                     0x510e527fu,
                     0x9b05688cu,
                     0x1f83d9abu,
                     0x5be0cd19u};

    /// Expands and compresses one 64-byte SHA-256 message block.
    auto processBlock = [&](const uint8_t block[64]) {
        uint32_t w[64] = {};
        for (size_t i = 0; i < 16; ++i)
            w[i] = readBE32(block + i * 4);
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (size_t i = 0; i < 64; ++i) {
            const uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            const uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    };

    size_t offset = 0;
    while (offset + 64 <= len) {
        processBlock(data + offset);
        offset += 64;
    }

    // Final block(s): remaining bytes + 0x80 + zero pad + 64-bit big-endian bit length.
    std::vector<uint8_t> tail;
    if (offset < len)
        tail.assign(data + offset, data + len);
    tail.push_back(0x80);
    while ((tail.size() % 64) != 56)
        tail.push_back(0);
    const uint64_t bitLen = static_cast<uint64_t>(len) * 8ull;
    for (int shift = 56; shift >= 0; shift -= 8)
        tail.push_back(static_cast<uint8_t>((bitLen >> shift) & 0xffu));
    for (size_t pos = 0; pos < tail.size(); pos += 64)
        processBlock(tail.data() + pos);

    for (size_t i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>((h[i] >> 24) & 0xffu);
        out[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xffu);
        out[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xffu);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xffu);
    }
}

/// @brief Encodes a target page size for the CodeDirectory header.
/// @param pageSize Nonzero power-of-two page size.
/// @return Base-two logarithm; zero is treated as a one-byte page defensively.
uint8_t pageSizeLog2(size_t pageSize) {
    uint8_t log2 = 0;
    size_t value = (pageSize == 0) ? 1 : pageSize;
    while (value > 1) {
        value >>= 1;
        ++log2;
    }
    return log2;
}

/// @brief Computes the number of page hashes needed to cover the signed prefix.
/// @param codeLimit Signed file-prefix length.
/// @param pageSize Target page size; zero is treated as one for this calculation.
/// @return Ceiling of `codeLimit / pageSize`, narrowed to the format field.
uint32_t codeSlotCount(size_t codeLimit, size_t pageSize) {
    const size_t effectivePageSize = (pageSize == 0) ? 1 : pageSize;
    return static_cast<uint32_t>((codeLimit + effectivePageSize - 1) / effectivePageSize);
}

/// @brief Computes the serialized CodeDirectory size.
/// @param codeLimit Signed file-prefix length.
/// @param identifier CodeDirectory identifier.
/// @param pageSize Target code-slot page size.
/// @return Header, identifier, special-slot, and code-slot byte count.
uint32_t codeDirectorySize(size_t codeLimit, const std::string &identifier, size_t pageSize) {
    const uint32_t identLen = static_cast<uint32_t>(identifier.size() + 1);
    const uint32_t nCodeSlots = codeSlotCount(codeLimit, pageSize);
    const uint32_t hashOffset = kCodeDirectoryHeaderSize + identLen + kSpecialSlots * kHashSize;
    return hashOffset + nCodeSlots * kHashSize;
}

} // anonymous namespace

/// @copydoc estimateCodeSignatureSize(size_t, const std::string &, size_t)
size_t estimateCodeSignatureSize(size_t codeLimit, const std::string &identifier, size_t pageSize) {
    return kSuperBlobHeaderSize + codeDirectorySize(codeLimit, identifier, pageSize) +
           kRequirementsSize + kBlobWrapperSize;
}

/// @copydoc buildCodeSignature(const std::vector<uint8_t> &, size_t, const std::string &, uint64_t, uint64_t, size_t)
std::vector<uint8_t> buildCodeSignature(const std::vector<uint8_t> &file,
                                        size_t codeLimit,
                                        const std::string &identifier,
                                        uint64_t textSegFileOff,
                                        uint64_t textSegFileSize,
                                        size_t pageSize) {
    const uint32_t nCodeSlots = codeSlotCount(codeLimit, pageSize);
    const uint32_t identLen = static_cast<uint32_t>(identifier.size() + 1);
    const uint32_t hashOffset = kCodeDirectoryHeaderSize + identLen + kSpecialSlots * kHashSize;
    const uint32_t cdSize = codeDirectorySize(codeLimit, identifier, pageSize);
    const uint32_t codeLimit32 = static_cast<uint32_t>(
        std::min<size_t>(codeLimit, static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    const uint64_t codeLimit64 =
        (codeLimit > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
            ? static_cast<uint64_t>(codeLimit)
            : 0;

    // --- Build CodeDirectory ---
    std::vector<uint8_t> cd;
    cd.reserve(cdSize);
    writeBE32(cd, CSMAGIC_CODEDIRECTORY);
    writeBE32(cd, cdSize);
    writeBE32(cd, CD_VERSION);
    writeBE32(cd, CS_ADHOC);
    writeBE32(cd, hashOffset);               // hashOffset
    writeBE32(cd, kCodeDirectoryHeaderSize); // identOffset
    writeBE32(cd, kSpecialSlots);
    writeBE32(cd, nCodeSlots);
    writeBE32(cd, codeLimit32);
    cd.push_back(kHashSize);               // hashSize (SHA-256)
    cd.push_back(2);                       // hashType (SHA-256)
    cd.push_back(0);                       // platform
    cd.push_back(pageSizeLog2(pageSize));  // pageSize (log2 target page size)
    writeBE32(cd, 0);                      // spare2
    writeBE32(cd, 0);                      // scatterOffset (v >= 0x20100)
    writeBE32(cd, 0);                      // teamOffset (v >= 0x20200)
    writeBE32(cd, 0);                      // spare3 (v >= 0x20300)
    writeBE64(cd, codeLimit64);            // codeLimit64
    writeBE64(cd, textSegFileOff);         // execSegBase (v >= 0x20400)
    writeBE64(cd, textSegFileSize);        // execSegLimit
    writeBE64(cd, CS_EXECSEG_MAIN_BINARY); // execSegFlags

    // Identifier string.
    cd.insert(cd.end(), identifier.begin(), identifier.end());
    cd.push_back(0);

    // Special slot -2: requirements hash.
    std::vector<uint8_t> req;
    req.reserve(kRequirementsSize);
    writeBE32(req, CSMAGIC_REQUIREMENTS);
    writeBE32(req, kRequirementsSize);
    writeBE32(req, 0); // count = 0
    uint8_t reqHash[kHashSize];
    sha256(req.data(), req.size(), reqHash);
    cd.insert(cd.end(), reqHash, reqHash + kHashSize);

    // Special slot -1: no Info.plist hash for raw executables.
    cd.insert(cd.end(), kHashSize, 0);

    // Code slot hashes: SHA-256 of each page.
    for (uint32_t i = 0; i < nCodeSlots; ++i) {
        const size_t pageStart = static_cast<size_t>(i) * pageSize;
        const size_t pageEnd = std::min(pageStart + pageSize, codeLimit);
        uint8_t hash[32];
        sha256(file.data() + pageStart, pageEnd - pageStart, hash);
        cd.insert(cd.end(), hash, hash + 32);
    }

    // --- Build empty BlobWrapper slot ---
    std::vector<uint8_t> wrapper;
    wrapper.reserve(kBlobWrapperSize);
    writeBE32(wrapper, CSMAGIC_BLOBWRAPPER);
    writeBE32(wrapper, kBlobWrapperSize);

    // --- Build SuperBlob ---
    const uint32_t sbSize =
        kSuperBlobHeaderSize + static_cast<uint32_t>(cd.size() + req.size() + wrapper.size());

    std::vector<uint8_t> sb;
    sb.reserve(sbSize);
    writeBE32(sb, CSMAGIC_EMBEDDED_SIGNATURE);
    writeBE32(sb, sbSize);
    writeBE32(sb, 3); // count = 3 blobs

    // Index entry 0: CodeDirectory (type = 0)
    writeBE32(sb, CSSLOT_CODEDIRECTORY);
    writeBE32(sb, kSuperBlobHeaderSize);

    // Index entry 1: Requirements (type = 2)
    writeBE32(sb, CSSLOT_REQUIREMENTS);
    writeBE32(sb, kSuperBlobHeaderSize + static_cast<uint32_t>(cd.size()));

    // Index entry 2: empty BlobWrapper (type = 0x10000)
    writeBE32(sb, CSSLOT_SIGNATURESLOT);
    writeBE32(sb, kSuperBlobHeaderSize + static_cast<uint32_t>(cd.size() + req.size()));

    sb.insert(sb.end(), cd.begin(), cd.end());
    sb.insert(sb.end(), req.begin(), req.end());
    sb.insert(sb.end(), wrapper.begin(), wrapper.end());
    return sb;
}

} // namespace zanna::codegen::linker
