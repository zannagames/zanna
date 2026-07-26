//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PkgVerify.cpp
// Purpose: Post-build verification of generated packages.
//
// Key invariants:
//   - Read-only: never modifies input data.
//   - Returns false on first structural error found.
//
// Ownership/Lifetime:
//   - Pure functions, no state.
//
// Links: PkgVerify.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements dependency-free structural verification for generated
///        package and installer containers.
/// @details The routines in this translation unit parse untrusted in-memory
///          archive bytes defensively, reject unsafe or duplicate paths, verify
///          embedded integrity metadata, and report the first failure through
///          a caller-provided diagnostic stream. Verification is read-only.

#include "PkgVerify.hpp"
#include "PkgGzip.hpp"
#include "PkgHash.hpp"
#include "PkgUtils.hpp"
#include "PkgZlib.hpp"
#include "WindowsInstallerMetadata.hpp"
#include "WindowsPEValidation.hpp"
#include "ZipReader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace zanna::pkg {

namespace {

/// @brief Determine whether an archive path contains an AppleDouble sidecar component.
/// @param path Slash-separated archive-relative path to inspect.
/// @return true if any component begins with `._`; otherwise false.
bool hasAppleDoubleComponent(const std::string &path);

/// @brief Read a little-endian uint16_t from an unaligned byte pointer.
/// @param p Pointer to at least two readable bytes.
/// @return Decoded host-order 16-bit value.
uint16_t rdLE16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

/// @brief Read a little-endian uint32_t from an unaligned byte pointer.
/// @param p Pointer to at least four readable bytes.
/// @return Decoded host-order 32-bit value.
uint32_t rdLE32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/// @brief Read a big-endian uint16_t from an unaligned byte pointer (XAR/PE fields).
/// @param p Pointer to at least two readable bytes.
/// @return Decoded host-order 16-bit value.
uint16_t rdBE16(const uint8_t *p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

/// @brief Read a big-endian uint32_t from an unaligned byte pointer (XAR/PE fields).
/// @param p Pointer to at least four readable bytes.
/// @return Decoded host-order 32-bit value.
uint32_t rdBE32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/// @brief Read a big-endian uint64_t from an unaligned byte pointer (XAR header).
/// @param p Pointer to at least eight readable bytes.
/// @return Decoded host-order 64-bit value.
uint64_t rdBE64(const uint8_t *p) {
    return (static_cast<uint64_t>(rdBE32(p)) << 32) | rdBE32(p + 4);
}

/// @brief Return true if the byte range [offset, offset+length) is entirely within [0, size).
/// Used to guard all bounds-checked reads from archive and PE structures.
/// @param offset Zero-based start of the candidate range.
/// @param length Number of bytes in the candidate range.
/// @param size Total size of the containing buffer.
/// @return true when the complete range is addressable without overflow.
bool hasRange(size_t offset, size_t length, size_t size) {
    return offset <= size && length <= size - offset;
}

/// @brief Decode the XML entity references that can appear in XAR text nodes.
/// @details XAR stores paths inside XML. Sanitizing the raw `<name>` text would
///          miss traversal names encoded as entities (for example `..&#x2f;`).
///          This decoder handles the five named XML entities and decimal/hex
///          numeric references for ASCII code points.
/// @param text Raw XML text-node content.
/// @param out Receives decoded text when the function succeeds.
/// @param err Receives a short parse error when the function fails.
/// @return true when every entity was recognized and decoded.
bool xmlUnescapeText(std::string_view text, std::string &out, std::string &err) {
    out.clear();
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c != '&') {
            out.push_back(c);
            continue;
        }
        const size_t semi = text.find(';', i + 1);
        if (semi == std::string_view::npos) {
            err = "unterminated XML entity";
            return false;
        }
        const std::string_view entity = text.substr(i + 1, semi - (i + 1));
        if (entity == "amp")
            out.push_back('&');
        else if (entity == "lt")
            out.push_back('<');
        else if (entity == "gt")
            out.push_back('>');
        else if (entity == "quot")
            out.push_back('"');
        else if (entity == "apos")
            out.push_back('\'');
        else if (!entity.empty() && entity[0] == '#') {
            const bool hex = entity.size() >= 2 && (entity[1] == 'x' || entity[1] == 'X');
            const size_t begin = hex ? 2 : 1;
            if (begin >= entity.size()) {
                err = "empty numeric XML entity";
                return false;
            }
            uint32_t value = 0;
            for (size_t j = begin; j < entity.size(); ++j) {
                const unsigned char ch = static_cast<unsigned char>(entity[j]);
                uint32_t digit = 0;
                if (hex && std::isxdigit(ch)) {
                    digit = std::isdigit(ch) ? static_cast<uint32_t>(ch - '0')
                                             : static_cast<uint32_t>(std::tolower(ch) - 'a' + 10);
                } else if (!hex && std::isdigit(ch)) {
                    digit = static_cast<uint32_t>(ch - '0');
                } else {
                    err = "invalid numeric XML entity";
                    return false;
                }
                const uint32_t base = hex ? 16u : 10u;
                if (value > (0x7fu - digit) / base) {
                    err = "non-ASCII XML entity in path";
                    return false;
                }
                value = value * base + digit;
            }
            if (value == 0) {
                err = "NUL XML entity in path";
                return false;
            }
            out.push_back(static_cast<char>(value));
        } else {
            err = "unknown XML entity";
            return false;
        }
        i = semi;
    }
    return true;
}

/// @brief Rotate a 32-bit value right by @p bits (SHA-256 round operation).
/// @param value Word to rotate.
/// @param bits Rotation distance; SHA-256 callers supply a value in `[1, 31]`.
/// @return The circular right rotation of @p value.
uint32_t rotr32(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

/// @brief Compute the SHA-256 of a buffer as a lowercase hex string.
/// @details A self-contained SHA-256 used to check the integrity manifests that
///          the package writers embed (e.g. SHA-256 entries inside a ZIP). Kept
///          local to the verifier so it has no dependency on the runtime.
/// @param data Buffer to hash; may be null only when @p len is zero.
/// @param len Number of bytes available at @p data.
/// @return The 32-byte digest encoded as 64 lowercase hexadecimal characters.
std::string sha256Hex(const uint8_t *data, size_t len) {
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
    std::vector<uint8_t> msg;
    if (len != 0)
        msg.assign(data, data + len);
    const uint64_t bitLen = static_cast<uint64_t>(len) * 8ull;
    msg.push_back(0x80);
    while ((msg.size() % 64u) != 56u)
        msg.push_back(0);
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xffu));
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64] = {};
        for (size_t i = 0; i < 16; ++i) {
            const size_t p = off + i * 4u;
            w[i] = (static_cast<uint32_t>(msg[p]) << 24) |
                   (static_cast<uint32_t>(msg[p + 1]) << 16) |
                   (static_cast<uint32_t>(msg[p + 2]) << 8) | static_cast<uint32_t>(msg[p + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (size_t i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
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
    }
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (uint32_t word : h)
        os << std::setw(8) << word;
    return os.str();
}

/// @brief Return true if @p text is exactly 64 hexadecimal digits (a SHA-256).
/// @param text Candidate textual digest.
/// @return true when @p text has the lexical form of a SHA-256 digest.
bool isSha256Hex(const std::string &text) {
    /// @brief Test whether one digest byte is a hexadecimal digit.
    /// @param ch Byte to inspect.
    /// @return `true` when the active C locale classifies `ch` as hexadecimal.
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
               return std::isxdigit(ch) != 0;
           });
}

/// @brief Return true if all 512 bytes of a USTAR tar block are zero.
/// Two consecutive zero blocks mark the end-of-archive in the POSIX tar format.
/// @param p Pointer to a complete 512-byte tar block.
/// @return true when every byte in the block is zero.
bool isAllZeroBlock(const uint8_t *p) {
    for (size_t i = 0; i < 512; ++i) {
        if (p[i] != 0)
            return false;
    }
    return true;
}

/// @brief Parse a fixed-width space/NUL-terminated decimal field (used in ar member headers).
/// @details Leading padding is not accepted. After the first space or NUL, all
///          remaining bytes must use the same permitted padding alphabet.
/// @param field Pointer to the fixed-width field.
/// @param width Number of bytes available at @p field.
/// @param out Receives the parsed value; reset to zero before parsing.
/// @return false for an empty field, invalid characters, non-padding suffix
///         bytes, or a value that overflows `size_t`.
bool parseDecimalField(const uint8_t *field, size_t width, size_t &out) {
    out = 0;
    bool sawDigit = false;
    for (size_t i = 0; i < width; ++i) {
        const unsigned char c = field[i];
        if (c == ' ' || c == '\0') {
            for (++i; i < width; ++i) {
                if (field[i] != ' ' && field[i] != '\0')
                    return false;
            }
            return sawDigit;
        }
        if (!std::isdigit(c))
            return false;
        sawDigit = true;
        if (out > (static_cast<size_t>(-1) - (c - '0')) / 10)
            return false;
        out = out * 10 + static_cast<size_t>(c - '0');
    }
    return sawDigit;
}

/// @brief Parse a fixed-width space/NUL-terminated octal field (used in USTAR tar headers
/// for file size, checksum, mode, etc.).
/// @param field Pointer to the fixed-width field.
/// @param width Number of bytes available at @p field.
/// @param out Receives the parsed value; reset to zero before parsing.
/// @return false for an empty field, invalid octal characters, non-padding
///         suffix bytes, or a value that overflows `uint64_t`.
bool parseOctalField(const uint8_t *field, size_t width, uint64_t &out) {
    out = 0;
    bool sawDigit = false;
    for (size_t i = 0; i < width; ++i) {
        const unsigned char c = field[i];
        if (c == ' ' || c == '\0') {
            for (++i; i < width; ++i) {
                if (field[i] != ' ' && field[i] != '\0')
                    return false;
            }
            return sawDigit;
        }
        if (c < '0' || c > '7')
            return false;
        sawDigit = true;
        if (out > (static_cast<uint64_t>(-1) - (c - '0')) / 8)
            return false;
        out = (out << 3) + static_cast<uint64_t>(c - '0');
    }
    return sawDigit;
}

/// @brief Extract a NUL-terminated string from a fixed-width tar header field.
/// @param field Pointer to the fixed-width field.
/// @param width Number of bytes available at @p field.
/// @return Bytes before the first NUL, or all @p width bytes when none is present.
std::string tarFieldString(const uint8_t *field, size_t width) {
    size_t len = 0;
    while (len < width && field[len] != '\0')
        ++len;
    return std::string(reinterpret_cast<const char *>(field), len);
}

/// @brief Parse POSIX PAX extended header records from a tar entry payload.
/// @details Supports the per-file keys needed by this packaging library:
///          `path` for long entry paths and `linkpath` for long symlink targets.
///          Unknown keys are ignored, but malformed record lengths fail the
///          archive verification.
/// @param payload Pointer to the first byte of the PAX payload.
/// @param size PAX payload length from the tar header.
/// @param err Stream for diagnostics.
/// @param out Map receiving supported PAX key/value records.
/// @return true when all PAX records are well formed.
bool parsePaxRecords(const uint8_t *payload,
                     uint64_t size,
                     std::ostream &err,
                     std::map<std::string, std::string> &out) {
    if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        err << "TAR: PAX payload too large\n";
        return false;
    }
    const size_t payloadSize = static_cast<size_t>(size);
    size_t pos = 0;
    while (pos < payloadSize) {
        size_t len = 0;
        bool sawDigit = false;
        const size_t recordStart = pos;
        while (pos < payloadSize && payload[pos] != ' ') {
            const unsigned char ch = payload[pos++];
            if (!std::isdigit(ch)) {
                err << "TAR: invalid PAX record length\n";
                return false;
            }
            sawDigit = true;
            if (len > (std::numeric_limits<size_t>::max() - static_cast<size_t>(ch - '0')) / 10u) {
                err << "TAR: PAX record length overflow\n";
                return false;
            }
            len = len * 10u + static_cast<size_t>(ch - '0');
        }
        if (!sawDigit || pos >= payloadSize || payload[pos] != ' ') {
            err << "TAR: malformed PAX record prefix\n";
            return false;
        }
        if (len == 0 || len > payloadSize - recordStart) {
            err << "TAR: PAX record extends past payload\n";
            return false;
        }
        const size_t recordEnd = recordStart + len;
        if (payload[recordEnd - 1] != '\n') {
            err << "TAR: PAX record is missing newline terminator\n";
            return false;
        }
        ++pos;
        const size_t bodyLen = recordEnd - pos - 1u;
        const std::string body(reinterpret_cast<const char *>(payload + pos), bodyLen);
        const size_t eq = body.find('=');
        if (eq == std::string::npos || eq == 0) {
            err << "TAR: malformed PAX key/value record\n";
            return false;
        }
        const std::string key = body.substr(0, eq);
        if (key == "path" || key == "linkpath")
            out[key] = body.substr(eq + 1);
        pos = recordEnd;
    }
    return true;
}

/// @brief Compute the POSIX USTAR header checksum: sum all 512 bytes treating the
/// checksum field at bytes 148-155 as if they were spaces (0x20).
/// The stored octal value must equal this sum for the header to be valid.
/// @param hdr Pointer to a complete 512-byte tar header.
/// @return Unsigned checksum calculated according to the USTAR convention.
uint32_t tarChecksum(const uint8_t *hdr) {
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; ++i)
        sum += (i >= 148 && i < 156) ? static_cast<uint8_t>(' ') : hdr[i];
    return sum;
}

/// @brief Verify a raw (already decompressed) USTAR tar stream.
/// Checks: 512-byte-aligned size, ustar magic, checksums, safe relative paths,
/// non-duplicate entries, valid symlink targets (no escapes), and two end-of-archive
/// zero blocks. Optionally collects normalized entry names into *outNames.
/// @param data Uncompressed tar bytes.
/// @param err Stream that receives the first structural diagnostic.
/// @param outNames Optional set populated with normalized non-PAX entry paths.
/// @return true when the complete stream is a safe, structurally valid USTAR archive.
bool verifyTarBytes(const std::vector<uint8_t> &data,
                    std::ostream &err,
                    std::set<std::string> *outNames = nullptr) {
    if (data.size() < 1024 || data.size() % 512 != 0) {
        err << "TAR: archive size is not a valid 512-byte-block tar stream\n";
        return false;
    }

    size_t pos = 0;
    bool sawEnd = false;
    std::set<std::string> seenNames;
    std::map<std::string, std::string> pendingPax;
    while (pos + 512 <= data.size()) {
        const uint8_t *hdr = data.data() + pos;
        if (isAllZeroBlock(hdr)) {
            if (pos + 1024 > data.size() || !isAllZeroBlock(data.data() + pos + 512)) {
                err << "TAR: missing second zero end-of-archive block\n";
                return false;
            }
            sawEnd = true;
            for (size_t tail = pos + 1024; tail < data.size(); ++tail) {
                if (data[tail] != 0) {
                    err << "TAR: non-zero bytes after end-of-archive marker\n";
                    return false;
                }
            }
            break;
        }

        if (std::memcmp(hdr + 257, "ustar", 5) != 0) {
            err << "TAR: missing ustar magic at offset " << pos << "\n";
            return false;
        }

        uint64_t storedChecksum = 0;
        if (!parseOctalField(hdr + 148, 8, storedChecksum)) {
            err << "TAR: invalid checksum field at offset " << pos << "\n";
            return false;
        }
        if (storedChecksum != tarChecksum(hdr)) {
            err << "TAR: checksum mismatch at offset " << pos << "\n";
            return false;
        }

        uint64_t fileSize = 0;
        if (!parseOctalField(hdr + 124, 12, fileSize)) {
            err << "TAR: invalid size field at offset " << pos << "\n";
            return false;
        }

        const char type = hdr[156] == '\0' ? '0' : static_cast<char>(hdr[156]);
        if (type != '0' && type != '5' && type != '2' && type != 'x') {
            err << "TAR: unsupported entry type '" << type << "' at offset " << pos << "\n";
            return false;
        }

        if (fileSize > std::numeric_limits<uint64_t>::max() - 511u) {
            err << "TAR: entry size overflows block padding at offset " << pos << "\n";
            return false;
        }
        const size_t paddedSize =
            static_cast<size_t>((fileSize + 511u) & ~static_cast<uint64_t>(511u));
        if (fileSize > static_cast<uint64_t>(data.size() - pos - 512) ||
            paddedSize > data.size() - pos - 512) {
            err << "TAR: entry data extends past end of archive\n";
            return false;
        }

        std::string name = tarFieldString(hdr, 100);
        const std::string prefix = tarFieldString(hdr + 345, 155);
        if (!prefix.empty())
            name = prefix + "/" + name;
        const auto paxPathIt = pendingPax.find("path");
        if (type != 'x' && paxPathIt != pendingPax.end())
            name = paxPathIt->second;
        if (name.rfind("./", 0) == 0)
            name = name.substr(2);
        if (!name.empty()) {
            try {
                const bool isDir = type == '5';
                const std::string clean =
                    sanitizePackageRelativePath(name, isDir ? "tar directory path" : "tar path");
                if (clean.empty() && !isDir && type != 'x') {
                    err << "TAR: empty file path at offset " << pos << "\n";
                    return false;
                }
                if (hasAppleDoubleComponent(clean)) {
                    err << "TAR: AppleDouble sidecar path is not allowed: '" << clean << "'\n";
                    return false;
                }
                if (type != 'x' && !clean.empty() && !seenNames.insert(clean).second) {
                    err << "TAR: duplicate entry '" << clean << "' at offset " << pos << "\n";
                    return false;
                }
                if (type != 'x' && outNames)
                    outNames->insert(clean);
            } catch (const std::exception &ex) {
                err << "TAR: unsafe path '" << name << "': " << ex.what() << "\n";
                return false;
            }
        }

        if (name.empty() && type != '5') {
            err << "TAR: empty entry path at offset " << pos << "\n";
            return false;
        }
        if (type == '2') {
            std::string target = tarFieldString(hdr + 157, 100);
            const auto paxLinkIt = pendingPax.find("linkpath");
            if (paxLinkIt != pendingPax.end())
                target = paxLinkIt->second;
            if (target.empty()) {
                err << "TAR: empty symlink target at offset " << pos << "\n";
                return false;
            }
            try {
                validateSingleLineField(target, "tar symlink target");
                for (char &c : target) {
                    if (c == '\\')
                        c = '/';
                }
                if (target.front() == '/' ||
                    (target.size() >= 2 && std::isalpha(static_cast<unsigned char>(target[0])) &&
                     target[1] == ':')) {
                    err << "TAR: absolute symlink target '" << target << "' at offset " << pos
                        << "\n";
                    return false;
                }
                const std::filesystem::path resolved =
                    (std::filesystem::path(name).parent_path() / target).lexically_normal();
                const std::string resolvedText = resolved.generic_string();
                if (resolvedText.empty() || resolvedText == "." ||
                    resolvedText.rfind("../", 0) == 0 || resolvedText == "..") {
                    err << "TAR: symlink target escapes archive root at offset " << pos << "\n";
                    return false;
                }
            } catch (const std::exception &ex) {
                err << "TAR: unsafe symlink target '" << target << "': " << ex.what() << "\n";
                return false;
            }
        }
        if (type != '0' && type != 'x' && fileSize != 0) {
            err << "TAR: non-file entry carries data at offset " << pos << "\n";
            return false;
        }
        if (type == 'x') {
            pendingPax.clear();
            if (!parsePaxRecords(data.data() + pos + 512, fileSize, err, pendingPax))
                return false;
        } else if (!pendingPax.empty()) {
            pendingPax.clear();
        }
        pos += 512 + paddedSize;
    }

    if (!sawEnd) {
        err << "TAR: missing end-of-archive marker\n";
        return false;
    }
    return true;
}

/// @brief Parse an 8-character ASCII-hex field (newc CPIO header fields).
/// @param field Pointer to exactly eight readable ASCII bytes.
/// @param out Receives the decoded 32-bit value; reset to zero before parsing.
/// @return false if any character is not a hexadecimal digit.
bool parseHex32Field(const uint8_t *field, uint32_t &out) {
    out = 0;
    for (size_t i = 0; i < 8; ++i) {
        const unsigned char ch = field[i];
        uint32_t digit = 0;
        if (ch >= '0' && ch <= '9')
            digit = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            digit = 10u + ch - 'a';
        else if (ch >= 'A' && ch <= 'F')
            digit = 10u + ch - 'A';
        else
            return false;
        out = (out << 4u) | digit;
    }
    return true;
}

/// @brief Parse a fixed-width all-octal-digit field (odc CPIO header fields).
/// @details Unlike parseOctalField, every byte must be an octal digit (no
///          space/NUL terminator).
/// @param field Pointer to the fixed-width ASCII field.
/// @param width Number of bytes available at @p field.
/// @param out Receives the decoded value; reset to zero before parsing.
/// @return false on a non-octal digit or `uint64_t` overflow.
bool parseFixedOctalField(const uint8_t *field, size_t width, uint64_t &out) {
    out = 0;
    for (size_t i = 0; i < width; ++i) {
        const unsigned char ch = field[i];
        if (ch < '0' || ch > '7')
            return false;
        if (out > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(ch - '0')) / 8u)
            return false;
        out = (out << 3u) + static_cast<uint64_t>(ch - '0');
    }
    return true;
}

/// @brief Round @p value up to the next multiple of 4 (newc CPIO field alignment).
/// @param value Unaligned byte offset or length.
/// @return Smallest multiple of four greater than or equal to @p value.
/// @pre @p value is at most `SIZE_MAX - 3`.
size_t align4(size_t value) {
    return (value + 3u) & ~static_cast<size_t>(3u);
}

/// @brief Return true if any path component begins with "._" (AppleDouble sidecar).
/// @details AppleDouble files leak macOS resource forks into archives; the
///          verifiers reject them so packages stay clean and reproducible.
/// @param path Slash-separated archive-relative path to inspect.
/// @return true if any path component has the AppleDouble `._` prefix.
bool hasAppleDoubleComponent(const std::string &path) {
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t next = path.find('/', pos);
        const std::string_view component = next == std::string::npos
                                               ? std::string_view(path).substr(pos)
                                               : std::string_view(path).substr(pos, next - pos);
        if (component.size() >= 2 && component[0] == '.' && component[1] == '_')
            return true;
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return false;
}

/// @brief Validate that an archive symlink target stays inside the archive root.
/// @details Rejects empty, multi-line, absolute, and drive-qualified targets,
///          then resolves the target relative to the entry's parent directory and
///          rejects it if the normalized result is empty, ".", "..", or escapes
///          upward ("../..."). Prevents symlink-based path traversal on extract.
/// @param entryName Name of the symlink entry (provides the base directory).
/// @param target Raw symlink target text (taken by value; normalized internally).
/// @param kind Archive-kind label used in diagnostics (e.g. "TAR", "CPIO").
/// @param err Stream for error messages.
/// @return true when the target is a safe in-archive relative path.
bool validateRelativeSymlinkTarget(const std::string &entryName,
                                   std::string target,
                                   const char *kind,
                                   std::ostream &err) {
    if (target.empty()) {
        err << kind << ": empty symlink target for '" << entryName << "'\n";
        return false;
    }
    try {
        validateSingleLineField(target, kind);
    } catch (const std::exception &ex) {
        err << kind << ": invalid symlink target for '" << entryName << "': " << ex.what() << "\n";
        return false;
    }
    for (char &ch : target) {
        if (ch == '\\')
            ch = '/';
    }
    if (target.front() == '/' ||
        (target.size() >= 2 && std::isalpha(static_cast<unsigned char>(target[0])) &&
         target[1] == ':')) {
        err << kind << ": absolute symlink target for '" << entryName << "'\n";
        return false;
    }
    const std::filesystem::path resolved =
        (std::filesystem::path(entryName).parent_path() / target).lexically_normal();
    const std::string resolvedText = resolved.generic_string();
    if (resolvedText.empty() || resolvedText == "." || resolvedText == ".." ||
        resolvedText.rfind("../", 0) == 0) {
        err << kind << ": symlink target escapes archive root for '" << entryName << "'\n";
        return false;
    }
    return true;
}

/// @brief Validate, normalize, and de-duplicate one CPIO entry path.
/// @details Strips a leading "./", requires a directory/file/symlink mode,
///          sanitizes the path (rejecting traversal and AppleDouble sidecars),
///          and records it in @p seen (rejecting duplicates) and optionally in
///          @p outNames.
/// @param name Raw entry name from the CPIO header.
/// @param mode Entry mode bits (used to classify the entry type).
/// @param err Stream for error messages.
/// @param seen Set of already-seen clean paths, updated to detect duplicates.
/// @param outNames Optional collector for the normalized path.
/// @param cleanOut Receives the sanitized relative path.
/// @return true when the entry path is valid and not a duplicate.
bool recordCpioPath(const std::string &name,
                    uint32_t mode,
                    std::ostream &err,
                    std::set<std::string> &seen,
                    std::set<std::string> *outNames,
                    std::string &cleanOut) {
    std::string normalized = name;
    if (normalized.rfind("./", 0) == 0)
        normalized.erase(0, 2);
    const uint32_t type = mode & 0170000u;
    const bool isDir = type == 0040000u;
    const bool isFile = type == 0100000u;
    const bool isSymlink = type == 0120000u;
    if (!isDir && !isFile && !isSymlink) {
        err << "CPIO: unsupported entry type for '" << normalized << "'\n";
        return false;
    }
    try {
        cleanOut =
            sanitizePackageRelativePath(normalized, isDir ? "cpio directory path" : "cpio path");
    } catch (const std::exception &ex) {
        err << "CPIO: unsafe path '" << normalized << "': " << ex.what() << "\n";
        return false;
    }
    if (!cleanOut.empty()) {
        if (hasAppleDoubleComponent(cleanOut)) {
            err << "CPIO: AppleDouble sidecar path is not allowed: '" << cleanOut << "'\n";
            return false;
        }
        if (!seen.insert(cleanOut).second) {
            err << "CPIO: duplicate entry '" << cleanOut << "'\n";
            return false;
        }
        if (outNames)
            outNames->insert(cleanOut);
    }
    return true;
}

/// @brief Verify a POSIX portable ASCII ("odc", magic 070707) CPIO stream.
/// @details Walks each fixed 76-byte header, parses the octal mode/name-size/
///          file-size fields, validates and records each entry path, requires a
///          NUL-terminated name, and enforces the TRAILER!!! sentinel with no
///          trailing non-zero bytes. Optionally collects entry names into
///          @p outNames.
/// @param data CPIO archive bytes.
/// @param err Stream for error messages.
/// @param outNames Optional collector for normalized entry names.
/// @return true when the odc stream is structurally valid.
bool verifyCpioOdcBytes(const std::vector<uint8_t> &data,
                        std::ostream &err,
                        std::set<std::string> *outNames = nullptr) {
    size_t pos = 0;
    bool sawTrailer = false;
    std::set<std::string> seen;
    while (pos < data.size()) {
        if (pos + 76 > data.size()) {
            err << "CPIO: truncated odc entry header at offset " << pos << "\n";
            return false;
        }
        if (std::memcmp(data.data() + pos, "070707", 6) != 0) {
            for (size_t i = pos; i < data.size(); ++i) {
                if (data[i] != 0) {
                    err << "CPIO: missing odc magic at offset " << pos << "\n";
                    return false;
                }
            }
            break;
        }
        uint64_t mode64 = 0;
        uint64_t nameSize64 = 0;
        uint64_t fileSize64 = 0;
        if (!parseFixedOctalField(data.data() + pos + 18, 6, mode64) ||
            !parseFixedOctalField(data.data() + pos + 59, 6, nameSize64) ||
            !parseFixedOctalField(data.data() + pos + 65, 11, fileSize64)) {
            err << "CPIO: invalid odc octal field at offset " << pos << "\n";
            return false;
        }
        if (nameSize64 == 0 || nameSize64 > data.size() - pos - 76) {
            err << "CPIO: invalid odc name size at offset " << pos << "\n";
            return false;
        }
        if (fileSize64 > data.size() - pos - 76 - static_cast<size_t>(nameSize64)) {
            err << "CPIO: odc entry data extends past end of archive\n";
            return false;
        }
        const size_t nameSize = static_cast<size_t>(nameSize64);
        const size_t fileSize = static_cast<size_t>(fileSize64);
        const size_t nameOff = pos + 76;
        if (data[nameOff + nameSize - 1] != 0) {
            err << "CPIO: odc entry name is not NUL-terminated at offset " << pos << "\n";
            return false;
        }
        const std::string name(reinterpret_cast<const char *>(data.data() + nameOff), nameSize - 1);
        const size_t dataOff = nameOff + nameSize;
        if (name == "TRAILER!!!") {
            if (fileSize != 0) {
                err << "CPIO: trailer entry carries data\n";
                return false;
            }
            sawTrailer = true;
            for (size_t i = dataOff; i < data.size(); ++i) {
                if (data[i] != 0) {
                    err << "CPIO: non-zero bytes after trailer\n";
                    return false;
                }
            }
            break;
        }

        const uint32_t mode = static_cast<uint32_t>(mode64);
        const uint32_t type = mode & 0170000u;
        const bool isDir = type == 0040000u;
        const bool isSymlink = type == 0120000u;
        if (isDir && fileSize != 0) {
            err << "CPIO: directory entry carries data for '" << name << "'\n";
            return false;
        }
        std::string clean;
        if (!recordCpioPath(name, mode, err, seen, outNames, clean))
            return false;
        if (isSymlink) {
            const std::string target(reinterpret_cast<const char *>(data.data() + dataOff),
                                     fileSize);
            if (!validateRelativeSymlinkTarget(clean, target, "CPIO", err))
                return false;
        }
        pos = dataOff + fileSize;
    }
    if (!sawTrailer) {
        err << "CPIO: missing TRAILER!!! entry\n";
        return false;
    }
    return true;
}

/// @brief Verify a "newc"/"crc" (magic 070701/070702) CPIO stream.
/// @details Delegates to verifyCpioOdcBytes when the older odc magic is detected.
///          Otherwise walks each 110-byte header, parses the 13 ASCII-hex fields,
///          validates 4-byte-aligned name/data regions, records each entry path,
///          checks symlink targets for traversal, and requires the TRAILER!!!
///          sentinel. Optionally collects entry names into @p outNames.
/// @param data CPIO archive bytes.
/// @param err Stream for error messages.
/// @param outNames Optional collector for normalized entry names.
/// @return true when the CPIO stream is structurally valid.
bool verifyCpioNewcBytes(const std::vector<uint8_t> &data,
                         std::ostream &err,
                         std::set<std::string> *outNames = nullptr) {
    if (data.size() >= 6 && std::memcmp(data.data(), "070707", 6) == 0)
        return verifyCpioOdcBytes(data, err, outNames);

    size_t pos = 0;
    bool sawTrailer = false;
    std::set<std::string> seen;
    while (pos < data.size()) {
        if (!hasRange(pos, 110, data.size())) {
            err << "CPIO: truncated entry header at offset " << pos << "\n";
            return false;
        }
        if (std::memcmp(data.data() + pos, "070701", 6) != 0 &&
            std::memcmp(data.data() + pos, "070702", 6) != 0) {
            err << "CPIO: missing newc magic at offset " << pos << "\n";
            return false;
        }

        uint32_t fields[13] = {};
        for (size_t i = 0; i < 13; ++i) {
            if (!parseHex32Field(data.data() + pos + 6 + i * 8, fields[i])) {
                err << "CPIO: invalid hex field at offset " << pos << "\n";
                return false;
            }
        }
        const uint32_t mode = fields[1];
        const uint32_t fileSize = fields[6];
        const uint32_t nameSize = fields[11];
        if (nameSize == 0) {
            err << "CPIO: entry has zero name size at offset " << pos << "\n";
            return false;
        }
        const size_t nameOff = pos + 110;
        if (!hasRange(nameOff, nameSize, data.size())) {
            err << "CPIO: truncated entry name at offset " << pos << "\n";
            return false;
        }
        if (data[nameOff + nameSize - 1] != 0) {
            err << "CPIO: entry name is not NUL-terminated at offset " << pos << "\n";
            return false;
        }
        std::string name(reinterpret_cast<const char *>(data.data() + nameOff), nameSize - 1);
        const size_t dataOff = align4(nameOff + nameSize);
        if (!hasRange(dataOff, fileSize, data.size())) {
            err << "CPIO: entry data extends past end of archive for '" << name << "'\n";
            return false;
        }

        if (name == "TRAILER!!!") {
            if (fileSize != 0) {
                err << "CPIO: trailer entry carries data\n";
                return false;
            }
            sawTrailer = true;
            const size_t end = align4(dataOff);
            for (size_t i = end; i < data.size(); ++i) {
                if (data[i] != 0) {
                    err << "CPIO: non-zero bytes after trailer\n";
                    return false;
                }
            }
            break;
        }

        const uint32_t type = mode & 0170000u;
        const bool isDir = type == 0040000u;
        const bool isFile = type == 0100000u;
        const bool isSymlink = type == 0120000u;
        if (!isDir && !isFile && !isSymlink) {
            err << "CPIO: unsupported entry type for '" << name << "'\n";
            return false;
        }
        if ((isDir || isSymlink) && fileSize != 0 && !isSymlink) {
            err << "CPIO: directory entry carries data for '" << name << "'\n";
            return false;
        }

        std::string clean;
        if (!recordCpioPath(name, mode, err, seen, outNames, clean))
            return false;

        if (isSymlink) {
            const std::string target(reinterpret_cast<const char *>(data.data() + dataOff),
                                     fileSize);
            if (!validateRelativeSymlinkTarget(clean, target, "CPIO", err))
                return false;
        }
        pos = align4(dataOff + fileSize);
    }

    if (!sawTrailer) {
        err << "CPIO: missing TRAILER!!! entry\n";
        return false;
    }
    return true;
}

/// @brief Extract the text between the first @p openTag and following @p closeTag.
/// @details A deliberately minimal XML scrape (no full parser) used to read fields
///          out of a XAR table-of-contents. Returns "" when either tag is absent.
/// @param text XML-like text to search.
/// @param openTag Complete opening token that precedes the desired content.
/// @param closeTag Complete closing token that terminates the desired content.
/// @return The intervening text, or an empty string if either token is absent.
std::string extractXmlTagText(const std::string &text,
                              const std::string &openTag,
                              const std::string &closeTag) {
    const size_t begin = text.find(openTag);
    if (begin == std::string::npos)
        return {};
    const size_t content = begin + openTag.size();
    const size_t end = text.find(closeTag, content);
    if (end == std::string::npos)
        return {};
    return text.substr(content, end - content);
}

/// @brief Parse a non-empty all-digit decimal string into a uint64_t.
/// @param text Candidate unsigned decimal representation.
/// @param out Receives the parsed value; reset to zero after non-empty input is accepted.
/// @return false on empty input, any non-digit character, or `uint64_t` overflow.
bool parseUnsignedDecimalText(const std::string &text, uint64_t &out) {
    if (text.empty())
        return false;
    out = 0;
    for (unsigned char ch : text) {
        if (!std::isdigit(ch))
            return false;
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (out > (std::numeric_limits<uint64_t>::max() - digit) / 10u)
            return false;
        out = out * 10u + digit;
    }
    return true;
}

/// @brief Extract the text of a `<tag ...>…</tag>` checksum element from a TOC block.
/// @details Tolerates attributes on the opening tag (e.g.
///          `<archived-checksum style="sha1">`) by scanning to the first '>'.
/// @param block XAR TOC `<file>` block text.
/// @param tag Element name without angle brackets (e.g. "archived-checksum").
/// @param out Receives the element's inner text on success.
/// @return true when the element is found and bounded by a closing tag.
bool extractXarFileChecksum(const std::string &block, const std::string &tag, std::string &out) {
    const std::string open = "<" + tag;
    const size_t openPos = block.find(open);
    if (openPos == std::string::npos)
        return false;
    const size_t content = block.find('>', openPos);
    if (content == std::string::npos)
        return false;
    const std::string close = "</" + tag + ">";
    const size_t closePos = block.find(close, content + 1);
    if (closePos == std::string::npos)
        return false;
    out = block.substr(content + 1, closePos - content - 1);
    return true;
}

/// @brief Parsed XAR contents: the decompressed TOC text and extracted payloads.
struct XarFileData {
    std::string tocText;                               ///< Decompressed TOC XML.
    std::map<std::string, std::vector<uint8_t>> files; ///< Path → extracted bytes.
};

/// @brief Find the `</file>` that closes the `<file>` opened at @p openPos.
/// @details Tracks nesting depth so nested `<file>` elements (directories
///          containing files) are matched correctly. Returns the position just
///          past the matching `</file>`, or npos if unbalanced within @p limit.
/// @param text TOC text to scan.
/// @param openPos Index of the opening `<file` token.
/// @param limit Exclusive upper bound for the search.
/// @return Position after the matching `</file>`, or std::string::npos.
size_t findMatchingXarFileEnd(const std::string &text, size_t openPos, size_t limit) {
    size_t pos = openPos;
    int depth = 0;
    while (pos < limit) {
        const size_t nextOpen = text.find("<file", pos);
        const size_t nextClose = text.find("</file>", pos);
        if (nextClose == std::string::npos || nextClose >= limit)
            return std::string::npos;
        if (nextOpen != std::string::npos && nextOpen < nextClose && nextOpen < limit) {
            ++depth;
            pos = nextOpen + 5;
        } else {
            --depth;
            pos = nextClose + 7;
            if (depth == 0)
                return pos;
            if (depth < 0)
                return std::string::npos;
        }
    }
    return std::string::npos;
}

/// @brief Extract and integrity-check one XAR file's heap payload.
/// @details Reads the offset/length/size fields from the TOC @p block, bounds-checks
///          them against the heap, zlib-decompresses gzip-encoded payloads (or
///          copies stored ones), verifies any archived/extracted SHA-1 checksums,
///          and inserts the result into @p out keyed by @p name (rejecting dupes).
/// @param block TOC `<file>` block text describing this entry.
/// @param name Archive-relative path of the entry.
/// @param data Full XAR archive bytes.
/// @param heapBase Byte offset where the heap (file data region) begins.
/// @param err Stream for error messages.
/// @param out Accumulator receiving the extracted file bytes.
/// @return true when the payload is in-bounds, decodes, and checksums match.
bool extractXarFilePayload(const std::string &block,
                           const std::string &name,
                           const std::vector<uint8_t> &data,
                           size_t heapBase,
                           std::ostream &err,
                           XarFileData &out) {
    uint64_t offset = 0;
    uint64_t length = 0;
    uint64_t size = 0;
    if (!parseUnsignedDecimalText(extractXmlTagText(block, "<offset>", "</offset>"), offset) ||
        !parseUnsignedDecimalText(extractXmlTagText(block, "<length>", "</length>"), length) ||
        !parseUnsignedDecimalText(extractXmlTagText(block, "<size>", "</size>"), size)) {
        err << "XAR: file element has invalid data bounds for '" << name << "'\n";
        return false;
    }
    if (offset > static_cast<uint64_t>(data.size() - heapBase) ||
        length > static_cast<uint64_t>(data.size() - heapBase) - offset) {
        err << "XAR: heap data for '" << name << "' extends past end of archive\n";
        return false;
    }
    if (size > static_cast<uint64_t>(std::vector<uint8_t>().max_size())) {
        err << "XAR: extracted data for '" << name << "' is too large\n";
        return false;
    }
    const size_t dataOff = heapBase + static_cast<size_t>(offset);
    const size_t dataLen = static_cast<size_t>(length);
    std::vector<uint8_t> archived(data.begin() + dataOff, data.begin() + dataOff + dataLen);
    std::vector<uint8_t> extracted;
    const bool compressed =
        block.find("encoding style=\"application/x-gzip\"") != std::string::npos;
    try {
        if (compressed) {
            extracted = zlibDecompress(archived.data(), archived.size(), static_cast<size_t>(size));
        } else {
            if (length != size) {
                err << "XAR: uncompressed length/size mismatch for '" << name << "'\n";
                return false;
            }
            extracted = archived;
        }
    } catch (const std::exception &ex) {
        err << "XAR: cannot extract '" << name << "': " << ex.what() << "\n";
        return false;
    }

    std::string archivedSha1;
    if (extractXarFileChecksum(block, "archived-checksum", archivedSha1) &&
        archivedSha1 != sha1Hex(archived.data(), archived.size())) {
        err << "XAR: archived SHA-1 mismatch for '" << name << "'\n";
        return false;
    }
    std::string extractedSha1;
    if (extractXarFileChecksum(block, "extracted-checksum", extractedSha1) &&
        extractedSha1 != sha1Hex(extracted.data(), extracted.size())) {
        err << "XAR: extracted SHA-1 mismatch for '" << name << "'\n";
        return false;
    }
    if (!out.files.emplace(name, std::move(extracted)).second) {
        err << "XAR: duplicate file entry '" << name << "'\n";
        return false;
    }
    return true;
}

/// @brief Recursively walk `<file>` elements in a TOC range, extracting payloads.
/// @details Each element's `<name>` is joined to @p prefix and sanitized; a
///          "directory" type recurses into its children while a "file" type is
///          extracted via extractXarFilePayload. AppleDouble paths are rejected.
/// @param tocText Decompressed TOC XML.
/// @param begin Inclusive start index in @p tocText to scan from.
/// @param end Exclusive end index bounding the scan.
/// @param prefix Accumulated parent path for nested entries ("" at the root).
/// @param data Full XAR archive bytes.
/// @param heapBase Byte offset where the heap (file data region) begins.
/// @param err Stream for error messages.
/// @param out Accumulator receiving the extracted files.
/// @return true when every element in the range is valid and extracted.
bool extractXarFileElements(const std::string &tocText,
                            size_t begin,
                            size_t end,
                            const std::string &prefix,
                            const std::vector<uint8_t> &data,
                            size_t heapBase,
                            std::ostream &err,
                            XarFileData &out) {
    size_t filePos = begin;
    while ((filePos = tocText.find("<file", filePos)) != std::string::npos && filePos < end) {
        const size_t fileEnd = findMatchingXarFileEnd(tocText, filePos, end);
        if (fileEnd == std::string::npos) {
            err << "XAR: unterminated file element\n";
            return false;
        }
        const std::string block = tocText.substr(filePos, fileEnd - filePos);
        const std::string encodedLeaf = extractXmlTagText(block, "<name>", "</name>");
        if (encodedLeaf.empty()) {
            err << "XAR: file element is missing a name\n";
            return false;
        }
        std::string leaf;
        std::string xmlErr;
        if (!xmlUnescapeText(encodedLeaf, leaf, xmlErr)) {
            err << "XAR: invalid XML path name: " << xmlErr << "\n";
            return false;
        }
        if (leaf.empty()) {
            err << "XAR: file element has an empty decoded name\n";
            return false;
        }
        std::string name = prefix.empty() ? leaf : prefix + "/" + leaf;
        try {
            name = sanitizePackageRelativePath(name, "xar file path");
        } catch (const std::exception &ex) {
            err << "XAR: unsafe file path '" << name << "': " << ex.what() << "\n";
            return false;
        }
        if (hasAppleDoubleComponent(name)) {
            err << "XAR: AppleDouble sidecar path is not allowed: '" << name << "'\n";
            return false;
        }

        const std::string type = extractXmlTagText(block, "<type>", "</type>");
        if (type == "directory") {
            const size_t contentBegin = tocText.find('>', filePos);
            if (contentBegin == std::string::npos || contentBegin + 1 > fileEnd) {
                err << "XAR: malformed directory element for '" << name << "'\n";
                return false;
            }
            if (!extractXarFileElements(
                    tocText, contentBegin + 1, fileEnd - 7, name, data, heapBase, err, out))
                return false;
        } else if (type == "file") {
            if (!extractXarFilePayload(block, name, data, heapBase, err, out))
                return false;
        } else {
            err << "XAR: unsupported file element type for '" << name << "'\n";
            return false;
        }
        filePos = fileEnd;
    }
    return true;
}

/// @brief Parse a XAR archive header, decompress its TOC, and extract all files.
/// @details Validates the "xar!" magic and header fields, inflates the zlib TOC
///          (capped at 64 MB), verifies the TOC SHA-1 stored at the heap base,
///          and then extracts every file element into @p out.
/// @param data Full XAR archive bytes.
/// @param err Stream for error messages.
/// @param out Receives the decompressed TOC text and extracted file payloads.
/// @return true when the archive parses, checksums, and extracts cleanly.
bool extractXarFiles(const std::vector<uint8_t> &data, std::ostream &err, XarFileData &out) {
    if (data.size() < 28 || std::memcmp(data.data(), "xar!", 4) != 0) {
        err << "XAR: missing xar header\n";
        return false;
    }
    const uint16_t headerSize = rdBE16(data.data() + 4);
    const uint16_t version = rdBE16(data.data() + 6);
    const uint64_t tocCompressed = rdBE64(data.data() + 8);
    const uint64_t tocUncompressed = rdBE64(data.data() + 16);
    const uint32_t checksumAlg = rdBE32(data.data() + 24);
    if (headerSize < 28 || headerSize > data.size()) {
        err << "XAR: invalid header size\n";
        return false;
    }
    if (version != 1) {
        err << "XAR: unsupported version " << version << "\n";
        return false;
    }
    if (checksumAlg != 1) {
        err << "XAR: unsupported TOC checksum algorithm " << checksumAlg << "\n";
        return false;
    }
    if (tocCompressed == 0 || tocUncompressed == 0 ||
        tocCompressed > static_cast<uint64_t>(data.size() - headerSize)) {
        err << "XAR: invalid TOC length\n";
        return false;
    }
    if (tocUncompressed > 64ull * 1024ull * 1024ull) {
        err << "XAR: TOC is unreasonably large\n";
        return false;
    }

    std::vector<uint8_t> toc;
    try {
        toc = zlibDecompress(data.data() + headerSize,
                             static_cast<size_t>(tocCompressed),
                             static_cast<size_t>(tocUncompressed));
    } catch (const std::exception &ex) {
        err << "XAR: cannot inflate TOC: " << ex.what() << "\n";
        return false;
    }
    out.tocText.assign(toc.begin(), toc.end());
    if (out.tocText.find("<xar") == std::string::npos ||
        out.tocText.find("</xar>") == std::string::npos) {
        err << "XAR: TOC is not a xar document\n";
        return false;
    }

    const size_t heapBase = headerSize + static_cast<size_t>(tocCompressed);
    if (!hasRange(heapBase, 20, data.size())) {
        err << "XAR: missing TOC checksum heap bytes\n";
        return false;
    }
    const auto tocSha1 = sha1Bytes(data.data() + headerSize, static_cast<size_t>(tocCompressed));
    if (!std::equal(tocSha1.begin(), tocSha1.end(), data.begin() + heapBase)) {
        err << "XAR: TOC SHA-1 checksum mismatch\n";
        return false;
    }

    if (!extractXarFileElements(out.tocText, 0, out.tocText.size(), "", data, heapBase, err, out))
        return false;

    if (out.files.empty()) {
        err << "XAR: archive contains no file entries\n";
        return false;
    }
    return true;
}

/// @brief Verify a macOS flat package (component or product) and its payloads.
/// @details Extracts the XAR, then handles either a component package (top-level
///          Payload) or a product package (Distribution referencing a nested
///          ZannaToolchain.pkg, into which it recurses). Requires the Bom,
///          PackageInfo, and Scripts root files, gunzips and CPIO-verifies the
///          Payload and Scripts, and requires preinstall/postinstall scripts.
/// @param data Flat `.pkg` (XAR) bytes.
/// @param err Stream for error messages.
/// @param outPayloadNames Optional collector for the payload's CPIO entry names.
/// @return true when the package and all nested payloads verify.
bool verifyMacOSPkgInternal(const std::vector<uint8_t> &data,
                            std::ostream &err,
                            std::set<std::string> *outPayloadNames) {
    XarFileData xar;
    if (!extractXarFiles(data, err, xar))
        return false;

    std::string componentPrefix;
    auto payloadIt = xar.files.find("Payload");
    if (payloadIt == xar.files.end()) {
        const auto distributionIt = xar.files.find("Distribution");
        if (distributionIt == xar.files.end()) {
            err << "macOS pkg: expected either component Payload or product Distribution with "
                   "ZannaToolchain.pkg\n";
            return false;
        }
        const std::string distribution(distributionIt->second.begin(),
                                       distributionIt->second.end());
        if (distribution.find("#ZannaToolchain.pkg") == std::string::npos ||
            distribution.find("installKBytes=\"") == std::string::npos ||
            distribution.find("updateKBytes=\"") == std::string::npos ||
            distribution.find("<product ") == std::string::npos) {
            err << "macOS pkg: Distribution is missing installable component metadata\n";
            return false;
        }
        for (const char *requiredMetadata : {"<welcome file=\"welcome.html\"",
                                             "<readme file=\"readme.html\"",
                                             "<license file=\"license.txt\"",
                                             "<conclusion file=\"conclusion.html\"",
                                             "<background file=\"background.png\"",
                                             "<background-darkAqua file=\"background-dark.png\"",
                                             "enable_localSystem=\"true\"",
                                             "rootVolumeOnly=\"true\"",
                                             "<allowed-os-versions>",
                                             "hostArchitectures=\""}) {
            if (distribution.find(requiredMetadata) == std::string::npos) {
                err << "macOS pkg: Distribution is missing required installer metadata: "
                    << requiredMetadata << "\n";
                return false;
            }
        }
        for (const char *requiredResource : {"welcome.html",
                                             "readme.html",
                                             "license.txt",
                                             "conclusion.html",
                                             "background.png",
                                             "background-dark.png"}) {
            const auto resourceIt = xar.files.find(requiredResource);
            if (resourceIt == xar.files.end() || resourceIt->second.empty()) {
                err << "macOS pkg: missing or empty Installer.app resource " << requiredResource
                    << "\n";
                return false;
            }
        }
        componentPrefix = "ZannaToolchain.pkg/";
        payloadIt = xar.files.find(componentPrefix + "Payload");
        if (payloadIt == xar.files.end()) {
            const auto nestedComponentIt = xar.files.find("ZannaToolchain.pkg");
            if (nestedComponentIt != xar.files.end())
                return verifyMacOSPkgInternal(nestedComponentIt->second, err, outPayloadNames);
            err << "macOS pkg: product is missing ZannaToolchain.pkg/Payload\n";
            return false;
        }
    }

    for (const char *requiredRoot : {"Bom", "PackageInfo", "Scripts"}) {
        if (xar.files.find(componentPrefix + requiredRoot) == xar.files.end()) {
            err << "macOS pkg: missing root file " << requiredRoot << "\n";
            return false;
        }
    }

    std::vector<uint8_t> payloadCpio;
    try {
        payloadCpio = gunzip(payloadIt->second.data(), payloadIt->second.size());
    } catch (const std::exception &ex) {
        err << "macOS pkg: cannot inflate Payload: " << ex.what() << "\n";
        return false;
    }
    if (!verifyCpioNewcBytes(payloadCpio, err, outPayloadNames))
        return false;

    std::vector<uint8_t> scriptsCpio;
    try {
        const auto scriptsIt = xar.files.find(componentPrefix + "Scripts");
        scriptsCpio = gunzip(scriptsIt->second.data(), scriptsIt->second.size());
    } catch (const std::exception &ex) {
        err << "macOS pkg: cannot inflate Scripts: " << ex.what() << "\n";
        return false;
    }
    std::set<std::string> scriptNames;
    if (!verifyCpioNewcBytes(scriptsCpio, err, &scriptNames))
        return false;
    for (const char *script : {"preinstall", "postinstall"}) {
        if (scriptNames.find(script) == scriptNames.end()) {
            err << "macOS pkg scripts: missing required payload path '" << script << "'\n";
            return false;
        }
    }
    return true;
}

/// @brief Compute the byte offset where overlay data begins in a PE file.
/// @details Parses the DOS, COFF, optional, section, and security-directory
///          headers. The overlay begins after the final section and ends before
///          an Authenticode certificate table, when one is present.
/// @param data Complete PE file bytes.
/// @param overlayOff Receives the first byte after all section raw data.
/// @param overlayEnd Receives the exclusive overlay end, excluding a certificate table.
/// @param err Stream that receives the first structural diagnostic.
/// @return true when all relevant headers and ranges are valid.
bool parsePeOverlayRange(const std::vector<uint8_t> &data,
                         size_t &overlayOff,
                         size_t &overlayEnd,
                         std::ostream &err) {
    if (data.size() < 64) {
        err << "PE: file too small (" << data.size() << " bytes)\n";
        return false;
    }
    if (data[0] != 'M' || data[1] != 'Z') {
        err << "PE: missing DOS 'MZ' magic\n";
        return false;
    }

    const size_t peOff = static_cast<size_t>(rdLE32(data.data() + 60));
    if (!hasRange(peOff, 4, data.size())) {
        err << "PE: e_lfanew (" << peOff << ") points past end of file\n";
        return false;
    }
    if (data[peOff] != 'P' || data[peOff + 1] != 'E' || data[peOff + 2] != 0 ||
        data[peOff + 3] != 0) {
        err << "PE: invalid PE signature at offset " << peOff << "\n";
        return false;
    }

    const size_t coffOff = peOff + 4;
    if (!hasRange(coffOff, 20, data.size())) {
        err << "PE: COFF header truncated\n";
        return false;
    }

    uint16_t numSections = rdLE16(data.data() + coffOff + 2);
    uint16_t optHdrSize = rdLE16(data.data() + coffOff + 16);
    const size_t optOff = coffOff + 20;
    if (!hasRange(optOff, optHdrSize, data.size())) {
        err << "PE: optional header truncated\n";
        return false;
    }
    const size_t secTableOff = coffOff + 20 + static_cast<size_t>(optHdrSize);
    if (!hasRange(secTableOff, static_cast<size_t>(numSections) * 40u, data.size())) {
        err << "PE: section table truncated\n";
        return false;
    }

    size_t maxRawEnd = 0;
    for (uint16_t i = 0; i < numSections; ++i) {
        size_t secOff = secTableOff + static_cast<size_t>(i) * 40;
        uint32_t rawSize = rdLE32(data.data() + secOff + 16);
        uint32_t rawOff = rdLE32(data.data() + secOff + 20);
        uint64_t secEnd64 = static_cast<uint64_t>(rawOff) + static_cast<uint64_t>(rawSize);
        if (secEnd64 > static_cast<uint64_t>(data.size())) {
            err << "PE: section raw data extends past end of file\n";
            return false;
        }
        size_t secEnd = static_cast<size_t>(secEnd64);
        if (secEnd > maxRawEnd)
            maxRawEnd = secEnd;
    }

    overlayOff = maxRawEnd;
    overlayEnd = data.size();

    constexpr size_t kPe32PlusDataDirectoryOffset = 112;
    constexpr size_t kPeSecurityDirectoryIndex = 4;
    const uint16_t optMagic = rdLE16(data.data() + optOff);
    if (optMagic != 0x020B) {
        err << "PE: expected PE32+ magic before overlay, got 0x" << std::hex << optMagic << std::dec
            << "\n";
        return false;
    }
    if (optHdrSize < kPe32PlusDataDirectoryOffset) {
        err << "PE: PE32+ optional header is too small for data directories\n";
        return false;
    }
    if (optHdrSize >= kPe32PlusDataDirectoryOffset + 4) {
        const uint32_t numberOfRvaAndSizes = rdLE32(data.data() + optOff + 108);
        const uint64_t declaredDataDirBytes = static_cast<uint64_t>(numberOfRvaAndSizes) * 8u;
        if (numberOfRvaAndSizes > 16 ||
            declaredDataDirBytes >
                static_cast<uint64_t>(optHdrSize - kPe32PlusDataDirectoryOffset)) {
            err << "PE: invalid data directory count in optional header\n";
            return false;
        }
        if (numberOfRvaAndSizes > kPeSecurityDirectoryIndex) {
            const size_t certDirOff =
                optOff + kPe32PlusDataDirectoryOffset + kPeSecurityDirectoryIndex * 8u;
            const uint32_t certFileOff = rdLE32(data.data() + certDirOff);
            const uint32_t certSize = rdLE32(data.data() + certDirOff + 4);
            if (certFileOff != 0 || certSize != 0) {
                const uint64_t certEnd64 =
                    static_cast<uint64_t>(certFileOff) + static_cast<uint64_t>(certSize);
                if (certFileOff < overlayOff || certEnd64 > static_cast<uint64_t>(data.size())) {
                    err << "PE: security directory points outside the certificate table\n";
                    return false;
                }
                overlayEnd = static_cast<size_t>(certFileOff);
            }
        }
    }
    return true;
}

/// @brief Check that every path in requiredPaths (after sanitization) is present in names.
/// Used by the verifyXxxPayload family to assert the presence of critical files
/// without re-parsing the archive structure.
/// @param names Set of sanitized archive paths observed during verification.
/// @param requiredPaths Caller-supplied paths that must be present.
/// @param kind Archive-kind label used for path validation and diagnostics.
/// @param err Stream that receives the first missing- or invalid-path diagnostic.
/// @return true when every required path sanitizes to a non-empty member of @p names.
bool requireArchivePaths(const std::set<std::string> &names,
                         const std::vector<std::string> &requiredPaths,
                         const char *kind,
                         std::ostream &err) {
    for (const auto &required : requiredPaths) {
        std::string clean;
        try {
            clean = sanitizePackageRelativePath(required, kind);
        } catch (const std::exception &ex) {
            err << kind << ": invalid required path '" << required << "': " << ex.what() << "\n";
            return false;
        }
        if (clean.empty()) {
            err << kind << ": required path must not be empty\n";
            return false;
        }
        if (names.find(clean) == names.end()) {
            err << kind << ": missing required payload path '" << clean << "'\n";
            return false;
        }
    }
    return true;
}

/// @brief Verify a ZIP parses cleanly and has safe, unique, extractable entries.
/// @details Opens the archive with ZipReader, sanitizes every entry path
///          (rejecting traversal and duplicates after normalization), and forces
///          each entry to extract so corrupt payloads are caught.
/// @param data ZIP file bytes.
/// @param err Stream for error messages.
/// @return true when every entry is safe and extractable.
bool verifyZipStructure(const std::vector<uint8_t> &data, std::ostream &err) {
    try {
        ZipReader reader(data.data(), data.size());
        std::set<std::string> seen;
        for (const auto &entry : reader.entries()) {
            std::string clean;
            try {
                clean = sanitizePackageRelativePath(entry.name, "zip entry path");
            } catch (const std::exception &ex) {
                err << "ZIP: unsafe entry path '" << entry.name << "': " << ex.what() << "\n";
                return false;
            }
            if (clean.empty()) {
                err << "ZIP: empty entry path\n";
                return false;
            }
            if (hasAppleDoubleComponent(clean)) {
                err << "ZIP: AppleDouble sidecar path is not allowed: '" << clean << "'\n";
                return false;
            }
            if (!seen.insert(clean).second) {
                err << "ZIP: duplicate normalized entry '" << clean << "'\n";
                return false;
            }
            (void)reader.extract(entry);
        }
    } catch (const std::exception &ex) {
        err << ex.what() << "\n";
        return false;
    }
    return true;
}

/// @brief Validate an optional `meta/manifest.sha256` integrity manifest in a ZIP.
/// @details When the manifest is absent this succeeds (it is optional). When
///          present, every "<sha256>  <path>" line must reference an existing
///          entry whose recomputed SHA-256 matches, paths must be unique, and —
///          conversely — every non-directory entry (except the manifest itself)
///          must be covered by the manifest.
/// @param reader Open ZIP reader over the archive.
/// @param kind Archive-kind label used in diagnostics.
/// @param err Stream for error messages.
/// @return true when the manifest is absent or fully consistent.
bool verifyZipSha256Manifest(const ZipReader &reader, const char *kind, std::ostream &err) {
    const ZipEntry *manifest = reader.find("meta/manifest.sha256");
    if (manifest == nullptr)
        return true;

    const auto manifestBytes = reader.extract(*manifest);
    const std::string text(manifestBytes.begin(), manifestBytes.end());
    std::istringstream lines(text);
    std::string line;
    size_t lineNo = 0;
    std::set<std::string> manifestPaths;
    while (std::getline(lines, line)) {
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty())
            continue;
        if (line.size() < 67 || !isSha256Hex(line.substr(0, 64)) || line[64] != ' ' ||
            line[65] != ' ') {
            err << kind << ": invalid SHA-256 manifest line " << lineNo << "\n";
            return false;
        }
        const std::string path = sanitizePackageRelativePath(line.substr(66), kind);
        if (!manifestPaths.insert(path).second) {
            err << kind << ": SHA-256 manifest lists duplicate entry '" << path << "'\n";
            return false;
        }
        const ZipEntry *listed = reader.find(path);
        if (listed == nullptr) {
            err << kind << ": SHA-256 manifest references missing entry '" << path << "'\n";
            return false;
        }
        const auto bytes = reader.extract(*listed);
        const std::string actual = sha256Hex(bytes.data(), bytes.size());
        std::string expected = line.substr(0, 64);
        /// @brief Normalize one expected-digest character to lowercase.
        /// @param ch Character to fold.
        /// @return Lowercase representation of `ch` as an unsigned-byte-safe conversion.
        std::transform(expected.begin(), expected.end(), expected.begin(), [](char ch) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        });
        if (actual != expected) {
            err << kind << ": SHA-256 mismatch for '" << path << "'\n";
            return false;
        }
    }
    for (const auto &entry : reader.entries()) {
        if (!entry.name.empty() && entry.name.back() == '/')
            continue;
        const std::string path = sanitizePackageRelativePath(entry.name, kind);
        if (path.empty() || path == "meta/manifest.sha256")
            continue;
        if (manifestPaths.find(path) == manifestPaths.end()) {
            err << kind << ": SHA-256 manifest does not cover entry '" << path << "'\n";
            return false;
        }
    }
    return true;
}

/// @brief Verify a ZIP archive and assert the presence of all requiredEntries.
/// Combines verifyZip (structural check) with requireArchivePaths (payload check).
/// @param data Complete ZIP archive bytes.
/// @param requiredEntries Paths that must occur in the archive.
/// @param kind Archive-kind label used in diagnostics and path sanitization.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the ZIP structure, required inventory, and optional
///         SHA-256 manifest all verify.
bool verifyZipPayload(const std::vector<uint8_t> &data,
                      const std::vector<std::string> &requiredEntries,
                      const char *kind,
                      std::ostream &err) {
    if (!verifyZipStructure(data, err))
        return false;
    try {
        ZipReader reader(data.data(), data.size());
        std::set<std::string> names;
        for (const auto &entry : reader.entries())
            names.insert(sanitizePackageRelativePath(entry.name, kind));
        if (!requireArchivePaths(names, requiredEntries, kind, err))
            return false;
        return verifyZipSha256Manifest(reader, kind, err);
    } catch (const std::exception &ex) {
        err << kind << ": " << ex.what() << "\n";
        return false;
    }
}

} // namespace

// ============================================================================
// ZIP Verification
// ============================================================================

/// @brief Verify a ZIP's structure, safe entry paths, extraction, and optional manifest.
/// @param data Complete ZIP archive bytes.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when every entry and the optional SHA-256 manifest are valid.
bool verifyZip(const std::vector<uint8_t> &data, std::ostream &err) {
    if (!verifyZipStructure(data, err))
        return false;
    try {
        ZipReader reader(data.data(), data.size());
        return verifyZipSha256Manifest(reader, "ZIP", err);
    } catch (const std::exception &ex) {
        err << "ZIP: " << ex.what() << "\n";
        return false;
    }
}

/// @brief Verify the standard payload layout of a zipped macOS app bundle.
/// @param data Complete ZIP archive bytes.
/// @param appBundleName Expected top-level `.app` directory name.
/// @param executableName Expected executable under `Contents/MacOS`.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the ZIP is valid and contains the standard bundle files.
bool verifyMacOSAppZip(const std::vector<uint8_t> &data,
                       const std::string &appBundleName,
                       const std::string &executableName,
                       std::ostream &err) {
    return verifyMacOSAppZipPayload(data, appBundleName, executableName, {}, err);
}

/// @brief Verify a macOS app ZIP and assert additional resource payload paths.
/// @details Builds the standard required bundle path set, prefixes each caller
///          supplied resource path with `<bundle>/Contents/Resources`, and then
///          delegates to the shared ZIP payload verifier.
/// @param data Complete ZIP archive bytes.
/// @param appBundleName Expected top-level `.app` directory name.
/// @param executableName Expected executable under `Contents/MacOS`.
/// @param requiredResourcePaths Relative paths required below `Contents/Resources`.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the ZIP is valid and contains every standard and requested file.
bool verifyMacOSAppZipPayload(const std::vector<uint8_t> &data,
                              const std::string &appBundleName,
                              const std::string &executableName,
                              const std::vector<std::string> &requiredResourcePaths,
                              std::ostream &err) {
    std::vector<std::string> required = {appBundleName + "/Contents/Info.plist",
                                         appBundleName + "/Contents/PkgInfo",
                                         appBundleName + "/Contents/MacOS/" + executableName};
    required.reserve(required.size() + requiredResourcePaths.size());
    const std::string resourcesPrefix = appBundleName + "/Contents/Resources";
    for (const auto &path : requiredResourcePaths)
        required.push_back(joinPackageRelativePath(resourcesPrefix, path, "macOS app ZIP path"));
    return verifyZipPayload(data, required, "macOS app ZIP", err);
}

// ============================================================================
// .deb (ar) Verification
// ============================================================================

/// @brief Full .deb (ar) structural verification: checks ar magic, expects exactly
/// debian-binary / control.tar.gz / data.tar.gz members in that order,
/// validates control.tar.gz contains a "control" entry, and verifies the USTAR
/// structure of both tarballs. If outDataNames is non-null, fills it with
/// normalized paths from data.tar.gz for payload checking.
/// @param data Complete Debian package bytes.
/// @param err Stream that receives the first verification diagnostic.
/// @param outDataNames Optional collector for normalized `data.tar.gz` entry paths.
/// @return true when the ar container and both compressed tar members are valid.
bool verifyDebInternal(const std::vector<uint8_t> &data,
                       std::ostream &err,
                       std::set<std::string> *outDataNames) {
    // Check ar magic
    if (data.size() < 8) {
        err << "DEB: file too small (" << data.size() << " bytes)\n";
        return false;
    }

    if (std::memcmp(data.data(), "!<arch>\n", 8) != 0) {
        err << "DEB: missing ar magic '!<arch>\\n'\n";
        return false;
    }

    std::vector<uint8_t> controlTarGz;
    std::vector<uint8_t> dataTarGz;
    bool foundControl = false;
    bool foundData = false;
    size_t pos = 8; // after ar magic
    size_t memberIndex = 0;
    const char *expectedMembers[] = {"debian-binary", "control.tar.gz", "data.tar.gz"};
    while (pos + 60 <= data.size()) {
        // Check header terminator
        if (data[pos + 58] != '`' || data[pos + 59] != '\n') {
            err << "DEB: invalid member header terminator at offset " << pos << "\n";
            return false;
        }

        // Read name (16 bytes)
        std::string name(reinterpret_cast<const char *>(data.data() + pos), 16);
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        auto end = name.find('/');
        if (end != std::string::npos)
            name.resize(end);

        // Read size
        size_t sz = 0;
        if (!parseDecimalField(data.data() + pos + 48, 10, sz)) {
            err << "DEB: invalid member size field at offset " << pos << "\n";
            return false;
        }
        const size_t contentOff = pos + 60;
        if (sz > data.size() - contentOff) {
            err << "DEB: member '" << name << "' content truncated\n";
            return false;
        }
        if (memberIndex >= 3) {
            err << "DEB: unexpected extra ar member '" << name << "'\n";
            return false;
        }
        if (name != expectedMembers[memberIndex]) {
            err << "DEB: member " << memberIndex << " is '" << name << "', expected '"
                << expectedMembers[memberIndex] << "'\n";
            return false;
        }

        const uint8_t *content = data.data() + contentOff;
        if (memberIndex == 0) {
            if (sz != 4 || std::memcmp(content, "2.0\n", 4) != 0) {
                err << "DEB: debian-binary content is not exactly '2.0\\n'\n";
                return false;
            }
        } else if (memberIndex == 1) {
            if (foundControl) {
                err << "DEB: duplicate control.tar.gz member\n";
                return false;
            }
            foundControl = true;
            controlTarGz.assign(content, content + sz);
        } else {
            if (foundData) {
                err << "DEB: duplicate data.tar.gz member\n";
                return false;
            }
            foundData = true;
            dataTarGz.assign(content, content + sz);
        }

        // Advance past header + data + optional padding
        pos += 60 + sz;
        if (sz % 2 != 0) {
            if (pos >= data.size()) {
                err << "DEB: missing ar padding byte after member '" << name << "'\n";
                return false;
            }
            if (data[pos] != '\n') {
                err << "DEB: invalid ar padding byte after member '" << name << "'\n";
                return false;
            }
            pos++; // odd-size padding
        }
        ++memberIndex;
    }

    if (pos != data.size()) {
        err << "DEB: trailing or truncated ar member data\n";
        return false;
    }
    if (memberIndex != 3) {
        err << "DEB: expected exactly debian-binary, control.tar.gz, and data.tar.gz members\n";
        return false;
    }
    if (!foundControl) {
        err << "DEB: 'control.tar.gz' member not found\n";
        return false;
    }
    if (!foundData) {
        err << "DEB: 'data.tar.gz' member not found\n";
        return false;
    }

    try {
        std::set<std::string> controlNames;
        const auto controlTar = gunzip(controlTarGz.data(), controlTarGz.size());
        if (!verifyTarBytes(controlTar, err, &controlNames))
            return false;
        if (controlNames.find("control") == controlNames.end()) {
            err << "DEB: control.tar.gz does not contain ./control\n";
            return false;
        }

        const auto dataTar = gunzip(dataTarGz.data(), dataTarGz.size());
        if (!verifyTarBytes(dataTar, err, outDataNames))
            return false;
    } catch (const std::exception &ex) {
        err << "DEB: compressed tar verification failed: " << ex.what() << "\n";
        return false;
    }

    return true;
}

/// @brief Verify the complete container structure of a Debian binary package.
/// @param data Complete `.deb` bytes.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the ar member inventory and compressed tar members are valid.
bool verifyDeb(const std::vector<uint8_t> &data, std::ostream &err) {
    return verifyDebInternal(data, err, nullptr);
}

/// @brief Verify a Debian package and require selected `data.tar.gz` paths.
/// @param data Complete `.deb` bytes.
/// @param requiredPaths Paths that must occur in the data payload.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the package verifies and every requested path is present.
bool verifyDebPayload(const std::vector<uint8_t> &data,
                      const std::vector<std::string> &requiredPaths,
                      std::ostream &err) {
    std::set<std::string> dataNames;
    if (!verifyDebInternal(data, err, &dataNames))
        return false;
    return requireArchivePaths(dataNames, requiredPaths, "DEB", err);
}

/// @brief Decompress and structurally verify a gzip-wrapped USTAR archive.
/// @param data Complete `.tar.gz` bytes.
/// @param err Stream that receives the first decompression or TAR diagnostic.
/// @return true when gzip decoding and USTAR verification both succeed.
bool verifyTarGz(const std::vector<uint8_t> &data, std::ostream &err) {
    try {
        const auto tarBytes = gunzip(data.data(), data.size());
        return verifyTarBytes(tarBytes, err);
    } catch (const std::exception &ex) {
        err << "TAR.GZ: " << ex.what() << "\n";
        return false;
    }
}

/// @brief Verify a gzip-wrapped USTAR archive and require selected entry paths.
/// @param data Complete `.tar.gz` bytes.
/// @param requiredPaths Paths that must occur in the tar payload.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the tarball verifies and every requested path is present.
bool verifyTarGzPayload(const std::vector<uint8_t> &data,
                        const std::vector<std::string> &requiredPaths,
                        std::ostream &err) {
    try {
        std::set<std::string> names;
        const auto tarBytes = gunzip(data.data(), data.size());
        if (!verifyTarBytes(tarBytes, err, &names))
            return false;
        return requireArchivePaths(names, requiredPaths, "TAR.GZ", err);
    } catch (const std::exception &ex) {
        err << "TAR.GZ: " << ex.what() << "\n";
        return false;
    }
}

/// @brief Verify a portable-ASCII or `newc` CPIO byte stream.
/// @param data Complete CPIO archive bytes.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the stream has valid entries and a proper trailer.
bool verifyCpioNewc(const std::vector<uint8_t> &data, std::ostream &err) {
    return verifyCpioNewcBytes(data, err);
}

/// @brief Verify a CPIO stream and require selected entry paths.
/// @param data Complete CPIO archive bytes.
/// @param requiredPaths Paths that must occur in the archive.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the archive verifies and every requested path is present.
bool verifyCpioNewcPayload(const std::vector<uint8_t> &data,
                           const std::vector<std::string> &requiredPaths,
                           std::ostream &err) {
    std::set<std::string> names;
    if (!verifyCpioNewcBytes(data, err, &names))
        return false;
    return requireArchivePaths(names, requiredPaths, "CPIO", err);
}

/// @brief Verify and extract the file inventory of a XAR archive in memory.
/// @param data Complete XAR archive bytes.
/// @param err Stream that receives the first header, checksum, XML, or payload diagnostic.
/// @return true when the TOC and every embedded file verify.
bool verifyXar(const std::vector<uint8_t> &data, std::ostream &err) {
    XarFileData files;
    return extractXarFiles(data, err, files);
}

/// @brief Verify a macOS flat package and its component payloads and scripts.
/// @param data Complete flat `.pkg` bytes.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the XAR container and required package contents verify.
bool verifyMacOSPkg(const std::vector<uint8_t> &data, std::ostream &err) {
    return verifyMacOSPkgInternal(data, err, nullptr);
}

/// @brief Verify a macOS flat package and require selected payload paths.
/// @param data Complete flat `.pkg` bytes.
/// @param requiredPaths Paths that must occur in the component payload.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the package verifies and every requested path is present.
bool verifyMacOSPkgPayload(const std::vector<uint8_t> &data,
                           const std::vector<std::string> &requiredPaths,
                           std::ostream &err) {
    std::set<std::string> payloadNames;
    if (!verifyMacOSPkgInternal(data, err, &payloadNames))
        return false;
    return requireArchivePaths(payloadNames, requiredPaths, "macOS pkg", err);
}

// ============================================================================
// PE Verification
// ============================================================================

/// @brief Verify the headers and non-overlapping raw sections of a PE32+ image.
/// @param data Complete PE file bytes.
/// @param err Stream that receives the first structural diagnostic.
/// @return true for a bounded x86-64 or ARM64 PE32+ image with disjoint sections.
bool verifyPE(const std::vector<uint8_t> &data, std::ostream &err) {
    std::string validationError;
    if (validateWindowsPEImage(
            data.empty() ? nullptr : data.data(), data.size(), 0U, nullptr, validationError)) {
        return true;
    }
    err << "PE: " << validationError << '\n';
    return false;
}

/// @brief Verify a PE32+ image and the ZIP overlay following its sections.
/// @param data Complete PE file bytes, including the expected overlay.
/// @param err Stream that receives the first PE or ZIP diagnostic.
/// @return true when the PE and its non-empty ZIP overlay both verify.
bool verifyPEZipOverlay(const std::vector<uint8_t> &data, std::ostream &err) {
    if (!verifyPE(data, err))
        return false;

    size_t overlayOff = 0;
    size_t overlayEnd = 0;
    if (!parsePeOverlayRange(data, overlayOff, overlayEnd, err))
        return false;

    if (overlayOff >= overlayEnd) {
        err << "PE: expected ZIP overlay after sections, but no overlay bytes were found\n";
        return false;
    }

    std::vector<uint8_t> overlay(data.begin() + overlayOff, data.begin() + overlayEnd);
    if (!verifyZip(overlay, err)) {
        err << "PE: ZIP overlay verification failed\n";
        return false;
    }

    return true;
}

/// @brief Verify a PE ZIP overlay and require selected outer ZIP entries.
/// @param data Complete PE file bytes, including the expected overlay.
/// @param requiredEntries Paths that must occur in the overlay ZIP.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the PE, overlay ZIP, and required inventory verify.
bool verifyPEZipOverlayPayload(const std::vector<uint8_t> &data,
                               const std::vector<std::string> &requiredEntries,
                               std::ostream &err) {
    if (!verifyPE(data, err))
        return false;

    size_t overlayOff = 0;
    size_t overlayEnd = 0;
    if (!parsePeOverlayRange(data, overlayOff, overlayEnd, err))
        return false;

    if (overlayOff >= overlayEnd) {
        err << "PE: expected ZIP overlay after sections, but no overlay bytes were found\n";
        return false;
    }

    std::vector<uint8_t> overlay(data.begin() + overlayOff, data.begin() + overlayEnd);
    if (!verifyZipPayload(overlay, requiredEntries, "PE ZIP overlay", err)) {
        err << "PE: ZIP overlay verification failed\n";
        return false;
    }

    return true;
}

/// @brief Verify required entries in both a PE overlay ZIP and one nested ZIP.
/// @param data Complete PE file bytes, including the expected overlay.
/// @param requiredOuterEntries Paths required in the overlay ZIP.
/// @param innerZipEntry Overlay entry whose extracted bytes form the nested ZIP.
/// @param requiredInnerEntries Paths required in the nested ZIP.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the PE and both ZIP inventories verify.
bool verifyPEZipOverlayNestedPayload(const std::vector<uint8_t> &data,
                                     const std::vector<std::string> &requiredOuterEntries,
                                     const std::string &innerZipEntry,
                                     const std::vector<std::string> &requiredInnerEntries,
                                     std::ostream &err) {
    if (!verifyPE(data, err))
        return false;

    size_t overlayOff = 0;
    size_t overlayEnd = 0;
    if (!parsePeOverlayRange(data, overlayOff, overlayEnd, err))
        return false;

    if (overlayOff >= overlayEnd) {
        err << "PE: expected ZIP overlay after sections, but no overlay bytes were found\n";
        return false;
    }

    std::vector<uint8_t> overlay(data.begin() + overlayOff, data.begin() + overlayEnd);
    if (!verifyZipPayload(overlay, requiredOuterEntries, "PE ZIP overlay", err)) {
        err << "PE: ZIP overlay verification failed\n";
        return false;
    }

    try {
        ZipReader outer(overlay.data(), overlay.size());
        const ZipEntry *innerEntry = outer.find(innerZipEntry);
        if (innerEntry == nullptr) {
            err << "PE ZIP overlay: missing nested ZIP entry '" << innerZipEntry << "'\n";
            return false;
        }
        const auto inner = outer.extract(*innerEntry);
        if (!verifyZipPayload(inner, requiredInnerEntries, "PE ZIP inner payload", err)) {
            err << "PE: nested ZIP payload verification failed\n";
            return false;
        }
    } catch (const std::exception &ex) {
        err << "PE ZIP overlay: " << ex.what() << "\n";
        return false;
    }

    return true;
}

namespace {

/// @brief Verified identity data retained while checking a native Windows package.
struct VerifiedWindowsNativePackage {
    WindowsInstallerMetadata metadata; ///< Parsed versioned installer contract.
    std::string payloadSha256;          ///< Digest of the nested payload ZIP.
};

/// @brief Check that a PE's COFF machine matches installer architecture metadata.
/// @param data Complete PE image bytes.
/// @param architecture Metadata architecture (`arm64` selects ARM64; otherwise x64).
/// @param err Stream that receives an architecture or header diagnostic.
/// @param label Human-readable component name used in the diagnostic.
/// @return true when the PE header is bounded and its machine value matches.
bool windowsPeMatchesArchitecture(const std::vector<uint8_t> &data,
                                  std::string_view architecture,
                                  std::ostream &err,
                                  std::string_view label) {
    uint16_t expected = 0U;
    if (architecture == "arm64")
        expected = 0xAA64U;
    else if (architecture == "x64")
        expected = 0x8664U;
    else {
        err << "Windows native installer: " << label << " metadata architecture is unsupported\n";
        return false;
    }
    std::string validationError;
    if (!validateWindowsPEImage(data.empty() ? nullptr : data.data(),
                                data.size(),
                                expected,
                                nullptr,
                                validationError)) {
        err << "Windows native installer: invalid " << label << ": " << validationError << '\n';
        return false;
    }
    return true;
}

/// @brief Reject outer ZIP entries not owned by native-installer metadata.
/// @param outer Open reader for the installer's PE overlay ZIP.
/// @param metadata Parsed contract defining all permitted outer entries.
/// @param err Stream that receives the first unowned-entry diagnostic.
/// @return true when every non-directory entry belongs to the declared inventory.
bool verifyNativeOuterInventory(const ZipReader &outer,
                                const WindowsInstallerMetadata &metadata,
                                std::ostream &err) {
    std::set<std::string> allowed = {
        "meta/installer-v2.txt", metadata.payloadEntry, metadata.cleanupEntry};
    if (!metadata.licenseEntry.empty())
        allowed.insert(metadata.licenseEntry);
    if (!metadata.readmeEntry.empty())
        allowed.insert(metadata.readmeEntry);
    for (const auto &file : metadata.outerFiles)
        allowed.insert(file.overlayPath);
    for (const ZipEntry &entry : outer.entries()) {
        if (!entry.name.empty() && entry.name.back() == '/')
            continue;
        if (allowed.find(entry.name) == allowed.end()) {
            err << "Windows native installer: unowned outer entry '" << entry.name << "'\n";
            return false;
        }
    }
    return true;
}

/// @brief Compare setup metadata with the normalized maintenance contract.
/// @details Removes the one detached maintenance executable from the setup's
///          installed/core sizes and outer inventory, changes its mode, and
///          compares canonical serialized metadata.
/// @param setup Metadata parsed from the top-level setup executable.
/// @param maintenance Metadata parsed from its embedded maintenance executable.
/// @return true when the maintenance metadata is exactly the expected derivative.
bool maintenanceContractMatches(const WindowsInstallerMetadata &setup,
                                const WindowsInstallerMetadata &maintenance) {
    if (setup.outerFiles.size() != 1U)
        return false;
    WindowsInstallerMetadata normalized = setup;
    const uint64_t maintenanceSize = normalized.outerFiles.front().sizeBytes;
    if (maintenanceSize > normalized.installedSizeBytes)
        return false;
    normalized.installedSizeBytes -= maintenanceSize;
    /// @brief Identify the mandatory core installer component.
    /// @param component Component metadata to inspect.
    /// @return `true` when the component identifier is exactly `core`.
    auto core = std::find_if(
        normalized.components.begin(),
        normalized.components.end(),
        [](const WindowsInstallerComponentMetadata &component) { return component.id == "core"; });
    if (core == normalized.components.end() || maintenanceSize > core->sizeBytes)
        return false;
    core->sizeBytes -= maintenanceSize;
    normalized.outerFiles.clear();
    normalized.packageMode = "maintenance";
    return serializeWindowsInstallerMetadata(normalized) ==
           serializeWindowsInstallerMetadata(maintenance);
}

/// @brief Recursively verify one setup or maintenance native Windows package.
/// @details Verifies the PE and overlay, parses schema metadata, checks declared
///          payload and outer-file sizes and SHA-256 digests, validates the
///          cleanup helper, and permits exactly one setup-to-maintenance level.
/// @param data Complete native installer executable bytes.
/// @param expectedMode Required metadata mode (`setup` or `maintenance`).
/// @param depth Current recursion depth; values greater than one are rejected.
/// @param verified Receives parsed metadata and the verified payload digest.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the executable and all declared or nested artifacts verify.
bool verifyWindowsNativeInstallerImpl(const std::vector<uint8_t> &data,
                                      std::string_view expectedMode,
                                      unsigned depth,
                                      VerifiedWindowsNativePackage &verified,
                                      std::ostream &err) {
    if (depth > 1U) {
        err << "Windows native installer: recursive maintenance payload\n";
        return false;
    }
    if (!verifyPE(data, err))
        return false;
    size_t overlayOff = 0;
    size_t overlayEnd = 0;
    if (!parsePeOverlayRange(data, overlayOff, overlayEnd, err) || overlayOff >= overlayEnd) {
        err << "Windows native installer: missing ZIP overlay\n";
        return false;
    }
    try {
        ZipReader outer(data.data() + overlayOff, overlayEnd - overlayOff);
        const ZipEntry *metadataEntry = outer.find("meta/installer-v2.txt");
        if (!metadataEntry) {
            err << "Windows native installer: missing versioned metadata\n";
            return false;
        }
        const std::vector<uint8_t> metadataBytes = outer.extract(*metadataEntry);
        verified.metadata = parseWindowsInstallerMetadata(std::string_view(
            reinterpret_cast<const char *>(metadataBytes.data()), metadataBytes.size()));
        if (verified.metadata.packageMode != expectedMode) {
            err << "Windows native installer: expected " << expectedMode << " metadata, found "
                << verified.metadata.packageMode << "\n";
            return false;
        }
        if (!windowsPeMatchesArchitecture(
                data, verified.metadata.architecture, err, "installer host"))
            return false;
        if (!verifyNativeOuterInventory(outer, verified.metadata, err))
            return false;

        const ZipEntry *payloadEntry = outer.find(verified.metadata.payloadEntry);
        if (!payloadEntry) {
            err << "Windows native installer: missing nested payload\n";
            return false;
        }
        const std::vector<uint8_t> payloadBytes = outer.extract(*payloadEntry);
        verified.payloadSha256 = sha256Hex(payloadBytes.data(), payloadBytes.size());
        ZipReader payload(payloadBytes.data(), payloadBytes.size());
        if (payload.entries().size() != verified.metadata.payloadFiles.size()) {
            err << "Windows native installer: payload inventory count mismatch\n";
            return false;
        }
        for (const auto &file : verified.metadata.payloadFiles) {
            const ZipEntry *entry = payload.find(file.path);
            if (!entry || entry->uncompressedSize != file.sizeBytes) {
                err << "Windows native installer: payload inventory mismatch for " << file.path
                    << "\n";
                return false;
            }
            const std::vector<uint8_t> bytes = payload.extract(*entry);
            if (sha256Hex(bytes.data(), bytes.size()) != file.sha256) {
                err << "Windows native installer: payload hash mismatch for " << file.path << "\n";
                return false;
            }
        }

        const ZipEntry *cleanupEntry = outer.find(verified.metadata.cleanupEntry);
        if (!cleanupEntry) {
            err << "Windows native installer: detached cleanup helper is missing\n";
            return false;
        }
        const std::vector<uint8_t> cleanup = outer.extract(*cleanupEntry);
        if (sha256Hex(cleanup.data(), cleanup.size()) != verified.metadata.cleanupSha256) {
            err << "Windows native installer: detached cleanup helper hash mismatch\n";
            return false;
        }
        std::ostringstream cleanupError;
        if (!verifyPE(cleanup, cleanupError)) {
            err << "Windows native installer: invalid cleanup helper: " << cleanupError.str();
            return false;
        }
        if (!windowsPeMatchesArchitecture(
                cleanup, verified.metadata.architecture, err, "cleanup helper"))
            return false;

        for (const auto &file : verified.metadata.outerFiles) {
            const ZipEntry *entry = outer.find(file.overlayPath);
            if (!entry || entry->uncompressedSize != file.sizeBytes) {
                err << "Windows native installer: outer-file inventory mismatch\n";
                return false;
            }
            const std::vector<uint8_t> bytes = outer.extract(*entry);
            if (sha256Hex(bytes.data(), bytes.size()) != file.sha256) {
                err << "Windows native installer: outer-file hash mismatch\n";
                return false;
            }
        }

        if (expectedMode == "setup") {
            const ZipEntry *maintenanceEntry = outer.find("meta/uninstall.exe");
            if (!maintenanceEntry) {
                err << "Windows native installer: setup is missing maintenance executable\n";
                return false;
            }
            const std::vector<uint8_t> maintenance = outer.extract(*maintenanceEntry);
            VerifiedWindowsNativePackage nested;
            if (!verifyWindowsNativeInstallerImpl(
                    maintenance, "maintenance", depth + 1U, nested, err))
                return false;
            if (!maintenanceContractMatches(verified.metadata, nested.metadata) ||
                nested.payloadSha256 != verified.payloadSha256) {
                err << "Windows native installer: setup and maintenance identities differ\n";
                return false;
            }
        } else if (outer.find("meta/uninstall.exe")) {
            err << "Windows native installer: maintenance payload embeds itself recursively\n";
            return false;
        }
    } catch (const std::exception &error) {
        err << "Windows native installer: " << error.what() << "\n";
        return false;
    }
    return true;
}

} // namespace

/// @brief Verify a schema-versioned native Windows setup and maintenance pair.
/// @param data Complete setup executable bytes.
/// @param err Stream that receives the first verification diagnostic.
/// @return true when the setup, payload, helpers, metadata, and maintenance image verify.
bool verifyWindowsNativeInstaller(const std::vector<uint8_t> &data, std::ostream &err) {
    VerifiedWindowsNativePackage verified;
    return verifyWindowsNativeInstallerImpl(data, "setup", 0U, verified, err);
}

/// @brief Verify that @p data ends with a plausible UDIF trailer.
/// @details Apple disk images store a 512-byte trailer whose first four bytes
///          are the ASCII signature "koly". This does not mount the image, but
///          it rejects empty/truncated/non-UDIF files without depending on
///          macOS-only tooling.
/// @param data Complete disk-image bytes.
/// @param err Stream that receives a size or signature diagnostic.
/// @return true when a 512-byte UDIF trailer with `koly` magic is present.
bool verifyMacOSDmg(const std::vector<uint8_t> &data, std::ostream &err) {
    if (data.size() < 512u) {
        err << "dmg: file is too small to contain a UDIF trailer\n";
        return false;
    }
    const size_t trailer = data.size() - 512u;
    if (data[trailer] != static_cast<uint8_t>('k') ||
        data[trailer + 1u] != static_cast<uint8_t>('o') ||
        data[trailer + 2u] != static_cast<uint8_t>('l') ||
        data[trailer + 3u] != static_cast<uint8_t>('y')) {
        err << "dmg: missing UDIF trailer signature\n";
        return false;
    }
    return true;
}

/// @brief Verify an RPM header and return its aligned end plus observed tag ids.
/// @param data Complete RPM package bytes.
/// @param offset Byte offset of the candidate header.
/// @param name Header label used in diagnostics.
/// @param endOffset Receives the first byte after the header store.
/// @param tags Receives the set of tag identifiers present in the index.
/// @param err Stream that receives the first bounds or format diagnostic.
/// @return true when the header magic, index, store, and tag offsets are valid.
static bool verifyRpmHeader(const std::vector<uint8_t> &data,
                            size_t offset,
                            const char *name,
                            size_t &endOffset,
                            std::set<uint32_t> &tags,
                            std::ostream &err) {
    if (!hasRange(offset, 16u, data.size())) {
        err << "rpm: missing " << name << " header\n";
        return false;
    }
    if (data[offset] != 0x8E || data[offset + 1] != 0xAD || data[offset + 2] != 0xE8 ||
        data[offset + 3] != 0x01) {
        err << "rpm: missing " << name << " header magic\n";
        return false;
    }
    const uint32_t indexCount = rdBE32(data.data() + offset + 8u);
    const uint32_t storeSize = rdBE32(data.data() + offset + 12u);
    const size_t entriesOffset = offset + 16u;
    if (indexCount > (data.size() - entriesOffset) / 16u) {
        err << "rpm: " << name << " header index extends past end of file\n";
        return false;
    }
    const size_t storeOffset = entriesOffset + static_cast<size_t>(indexCount) * 16u;
    if (storeSize > data.size() - storeOffset) {
        err << "rpm: " << name << " header store extends past end of file\n";
        return false;
    }
    tags.clear();
    for (uint32_t index = 0; index < indexCount; ++index) {
        const size_t entry = entriesOffset + static_cast<size_t>(index) * 16u;
        const uint32_t tag = rdBE32(data.data() + entry);
        const uint32_t valueOffset = rdBE32(data.data() + entry + 8u);
        if (valueOffset > storeSize) {
            err << "rpm: " << name << " header tag " << tag << " points past its store\n";
            return false;
        }
        tags.insert(tag);
    }
    endOffset = storeOffset + storeSize;
    return true;
}

/// @brief Verify the lead, headers, required tags, file list, and payload of an RPM.
/// @param data Complete RPM package bytes.
/// @param err Stream that receives the first structural diagnostic.
/// @return true when the RPM container is structurally plausible and non-empty.
bool verifyRpm(const std::vector<uint8_t> &data, std::ostream &err) {
    if (data.size() < 112u || data[0] != 0xED || data[1] != 0xAB || data[2] != 0xEE ||
        data[3] != 0xDB) {
        err << "rpm: missing lead magic\n";
        return false;
    }
    if (data[4] != 3u || rdBE16(data.data() + 78u) != 5u) {
        err << "rpm: unsupported lead version or signature type\n";
        return false;
    }
    size_t signatureEnd = 0;
    std::set<uint32_t> signatureTags;
    if (!verifyRpmHeader(data, 96u, "signature", signatureEnd, signatureTags, err))
        return false;
    if (signatureEnd > std::numeric_limits<size_t>::max() - 7u) {
        err << "rpm: signature header alignment overflow\n";
        return false;
    }
    const size_t mainOffset = (signatureEnd + 7u) & ~size_t{7u};
    size_t mainEnd = 0;
    std::set<uint32_t> mainTags;
    if (!verifyRpmHeader(data, mainOffset, "main", mainEnd, mainTags, err))
        return false;
    for (const uint32_t requiredTag : {1000u, 1001u, 1002u, 1022u}) {
        if (mainTags.find(requiredTag) == mainTags.end()) {
            err << "rpm: main header is missing required tag " << requiredTag << "\n";
            return false;
        }
    }
    const bool hasFileList =
        mainTags.find(5000u) != mainTags.end() || mainTags.find(1027u) != mainTags.end() ||
        (mainTags.find(1116u) != mainTags.end() && mainTags.find(1117u) != mainTags.end() &&
         mainTags.find(1118u) != mainTags.end());
    if (!hasFileList) {
        err << "rpm: main header is missing file-list tags\n";
        return false;
    }
    /// @brief Test whether one byte in the prospective RPM payload is zero.
    /// @param byte Payload byte to inspect.
    /// @return `true` when `byte` equals zero.
    if (mainEnd >= data.size() || std::all_of(data.begin() + static_cast<std::ptrdiff_t>(mainEnd),
                                              data.end(),
                                              [](uint8_t byte) { return byte == 0; })) {
        err << "rpm: package payload is empty\n";
        return false;
    }
    return true;
}

} // namespace zanna::pkg
