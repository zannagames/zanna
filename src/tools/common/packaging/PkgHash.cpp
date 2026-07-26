//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PkgHash.cpp
// Purpose: SHA-1 and SHA-256 helpers for package metadata and XAR checksums.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements self-contained SHA-1 and SHA-256 digest helpers.
/// @details Processes borrowed inputs in 64-byte blocks, applies standard
///          Merkle–Damgård padding, and exposes binary or lowercase hexadecimal output.

#include "PkgHash.hpp"

#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace zanna::pkg {
namespace {

/// @brief Rotate a 32-bit value left by @p bits (SHA-1 round operation).
/// @param value Word to rotate.
/// @param bits Rotation count.
/// @return Rotated word.
uint32_t rol32(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32u - bits));
}

/// @brief Rotate a 32-bit value right by @p bits (SHA-256 round operation).
/// @param value Word to rotate.
/// @param bits Rotation count.
/// @return Rotated word.
uint32_t ror32(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

/// @brief Read four bytes at @p p as a big-endian 32-bit word.
/// @param p Address of at least four readable bytes.
/// @return Decoded word.
uint32_t readBE32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/// @brief Store @p value at @p p as a big-endian 32-bit word.
/// @param p Address of at least four writable bytes.
/// @param value Word to encode.
void writeBE32(uint8_t *p, uint32_t value) {
    p[0] = static_cast<uint8_t>((value >> 24) & 0xffu);
    p[1] = static_cast<uint8_t>((value >> 16) & 0xffu);
    p[2] = static_cast<uint8_t>((value >> 8) & 0xffu);
    p[3] = static_cast<uint8_t>(value & 0xffu);
}

/// @brief Validate the common digest input buffer contract.
/// @details The hashing APIs allow a null data pointer only for empty inputs. This guard turns
///          accidental non-empty null buffers into deterministic exceptions instead of undefined
///          pointer arithmetic or reads in the block-processing loops.
/// @param data Borrowed input pointer.
/// @param len Input length.
/// @param algorithm Algorithm name used in the diagnostic.
/// @throws std::runtime_error If `data` is null for a non-empty input.
void validateDigestInput(const uint8_t *data, size_t len, const char *algorithm) {
    if (len != 0 && data == nullptr)
        throw std::runtime_error(std::string(algorithm) + ": null input buffer for non-empty hash");
}

/// @brief Render a fixed-size digest array as a lowercase hex string.
/// @tparam N Digest size in bytes.
/// @param digest Binary digest.
/// @return Exactly `2 * N` lowercase hexadecimal characters.
template <size_t N> std::string hexDigest(const std::array<uint8_t, N> &digest) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (uint8_t byte : digest)
        os << std::setw(2) << static_cast<unsigned>(byte);
    return os.str();
}

/// @brief Append the Merkle–Damgård padding shared by SHA-1 and SHA-256.
/// @details Appends the 0x80 marker byte, zero-pads to 56 bytes mod 64, and then
///          writes the original message length in bits as a big-endian 64-bit
///          value, so the resulting tail is a whole number of 64-byte blocks.
/// @param tail Trailing partial block to pad in place.
/// @param bitLen Total message length in bits.
void appendShaPadding(std::vector<uint8_t> &tail, uint64_t bitLen) {
    tail.push_back(0x80);
    while ((tail.size() % 64u) != 56u)
        tail.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8)
        tail.push_back(static_cast<uint8_t>((bitLen >> shift) & 0xffu));
}

} // namespace

/// @brief SHA-1 implementation: hash full 64-byte blocks, then the padded tail.
/// @param data Borrowed input bytes, optionally null when `len` is zero.
/// @param len Input length.
/// @return 20-byte SHA-1 digest.
/// @throws std::runtime_error If a non-empty input pointer is null.
std::array<uint8_t, 20> sha1Bytes(const uint8_t *data, size_t len) {
    validateDigestInput(data, len, "SHA-1");
    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xefcdab89u;
    uint32_t h2 = 0x98badcfeu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xc3d2e1f0u;

    /// @brief Expand and compress one 64-byte SHA-1 message block.
    /// @param block Complete message block in big-endian byte order.
    auto processBlock = [&](const uint8_t block[64]) {
        uint32_t w[80] = {};
        for (size_t i = 0; i < 16; ++i)
            w[i] = readBE32(block + i * 4);
        for (size_t i = 16; i < 80; ++i)
            w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        for (size_t i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcu;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6u;
            }
            const uint32_t temp = rol32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol32(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    };

    size_t offset = 0;
    while (offset + 64 <= len) {
        processBlock(data + offset);
        offset += 64;
    }

    std::vector<uint8_t> tail;
    if (offset < len)
        tail.assign(data + offset, data + len);
    appendShaPadding(tail, static_cast<uint64_t>(len) * 8ull);
    for (size_t pos = 0; pos < tail.size(); pos += 64)
        processBlock(tail.data() + pos);

    std::array<uint8_t, 20> out{};
    writeBE32(out.data() + 0, h0);
    writeBE32(out.data() + 4, h1);
    writeBE32(out.data() + 8, h2);
    writeBE32(out.data() + 12, h3);
    writeBE32(out.data() + 16, h4);
    return out;
}

/// @brief Compute SHA-1 and encode it as lowercase hexadecimal.
/// @param data Borrowed input bytes, optionally null when `len` is zero.
/// @param len Input length.
/// @return 40-character digest string.
/// @throws std::runtime_error If a non-empty input pointer is null.
std::string sha1Hex(const uint8_t *data, size_t len) {
    return hexDigest(sha1Bytes(data, len));
}

/// @brief SHA-256 implementation: hash full 64-byte blocks, then the padded tail.
/// @param data Borrowed input bytes, optionally null when `len` is zero.
/// @param len Input length.
/// @return 32-byte SHA-256 digest.
/// @throws std::runtime_error If a non-empty input pointer is null.
std::array<uint8_t, 32> sha256Bytes(const uint8_t *data, size_t len) {
    validateDigestInput(data, len, "SHA-256");
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

    /// @brief Expand and compress one 64-byte SHA-256 message block.
    /// @param block Complete message block in big-endian byte order.
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
    std::vector<uint8_t> tail;
    if (offset < len)
        tail.assign(data + offset, data + len);
    appendShaPadding(tail, static_cast<uint64_t>(len) * 8ull);
    for (size_t pos = 0; pos < tail.size(); pos += 64)
        processBlock(tail.data() + pos);

    std::array<uint8_t, 32> out{};
    for (size_t i = 0; i < 8; ++i)
        writeBE32(out.data() + i * 4, h[i]);
    return out;
}

/// @brief Compute SHA-256 and encode it as lowercase hexadecimal.
/// @param data Borrowed input bytes, optionally null when `len` is zero.
/// @param len Input length.
/// @return 64-character digest string.
/// @throws std::runtime_error If a non-empty input pointer is null.
std::string sha256Hex(const uint8_t *data, size_t len) {
    return hexDigest(sha256Bytes(data, len));
}

} // namespace zanna::pkg
