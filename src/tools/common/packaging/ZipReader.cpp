//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/ZipReader.cpp
// Purpose: Minimal ZIP archive reader — parses central directory and extracts
//          entries using stored or DEFLATE decompression.
//
// Key invariants:
//   - Scans backwards for EOCD signature (handles ZIP comments).
//   - Validates CRC-32 after extraction.
//   - Thread-safe: no mutable state after construction.
//
// Ownership/Lifetime:
//   - References external buffer. Does not copy input data.
//
// Links: ZipReader.hpp, PkgDeflate.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements strict read-only parsing and extraction of classic ZIP archives.
/// @details Construction validates central/local record bounds, path safety, and
///          non-overlap; extraction supports stored and DEFLATE entries and
///          cross-checks metadata and CRC-32.

#include "ZipReader.hpp"
#include "PkgDeflate.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <utility>

extern "C" {
uint32_t rt_crc32_compute(const uint8_t *data, size_t len);
}

namespace zanna::pkg {

namespace {

/// @brief Read a 16-bit little-endian unsigned integer from an unaligned byte pointer.
/// @param p Pointer to at least two readable bytes.
/// @return Decoded host-order 16-bit value.
uint16_t rdLE16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

/// @brief Read a 32-bit little-endian unsigned integer from an unaligned byte pointer.
/// @param p Pointer to at least four readable bytes.
/// @return Decoded host-order 32-bit value.
uint32_t rdLE32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/// @brief Reject ZIP entry names that could escape the extraction directory.
/// @details Flags empty names, absolute paths, backslashes, drive colons, and any
///          empty/"."/".." path segment (after trimming a trailing slash on
///          directory entries). Used while parsing the central directory.
/// @param name Entry name from the central directory.
/// @return true when the name is unsafe to extract.
bool isUnsafeZipName(const std::string &name) {
    if (name.empty() || name.front() == '/' || name.find('\\') != std::string::npos ||
        name.find(':') != std::string::npos) {
        return true;
    }
    std::string checkName = name;
    while (!checkName.empty() && checkName.back() == '/')
        checkName.pop_back();
    if (checkName.empty())
        return true;
    size_t pos = 0;
    while (pos <= checkName.size()) {
        const size_t slash = checkName.find('/', pos);
        const std::string segment =
            slash == std::string::npos ? checkName.substr(pos) : checkName.substr(pos, slash - pos);
        if (segment.empty() || segment == "." || segment == "..")
            return true;
        if (slash == std::string::npos)
            break;
        pos = slash + 1;
    }
    return false;
}

/// @brief Return the duplicate-detection key for a ZIP entry name.
/// @details Directory markers such as `foo/` and file entries such as `foo`
///          address the same extraction path. The reader rejects both by trimming
///          trailing slashes before inserting the name in the seen set.
/// @param name Central-directory entry name, taken by value.
/// @return Name without trailing slash characters.
std::string normalizedZipDuplicateKey(std::string name) {
    while (!name.empty() && name.back() == '/')
        name.pop_back();
    return name;
}

} // namespace

/// @brief Construct a ZipReader over the given memory buffer and immediately parse
/// the central directory. data must remain valid for the lifetime of this object.
/// @param data Caller-owned complete archive bytes.
/// @param len Number of bytes available at @p data.
/// @throws ZipReadError If the buffer is null, truncated, unsafe, or unsupported.
ZipReader::ZipReader(const uint8_t *data, size_t len) : data_(data), len_(len) {
    if (!data || len < 22)
        throw ZipReadError("ZIP: buffer too small");
    parseCentralDirectory();
}

/// @brief Locate the End-of-Central-Directory (EOCD) record by scanning backwards from
/// the end of the buffer (to handle optional ZIP comments), read the central
/// directory, and populate entries_. Rejects ZIP64, split archives, encrypted
/// entries, data-descriptor entries, and unsupported compression methods.
void ZipReader::parseCentralDirectory() {
    // Scan backwards for EOCD signature 0x06054B50
    // EOCD can have up to 65535 bytes of comment, so search range is limited
    size_t searchStart = (len_ > 65557) ? len_ - 65557 : 0;
    size_t eocdOff = 0;
    bool found = false;

    for (size_t i = len_ - 22; i >= searchStart; --i) {
        if (rdLE32(data_ + i) == 0x06054B50) {
            eocdOff = i;
            found = true;
            break;
        }
        if (i == 0)
            break;
    }

    if (!found)
        throw ZipReadError("ZIP: end-of-central-directory signature not found");

    uint16_t totalEntries = rdLE16(data_ + eocdOff + 10);
    uint16_t diskEntries = rdLE16(data_ + eocdOff + 8);
    uint16_t commentLen = rdLE16(data_ + eocdOff + 20);
    uint32_t cdSize = rdLE32(data_ + eocdOff + 12);
    uint32_t cdOffset = rdLE32(data_ + eocdOff + 16);

    if (diskEntries == 0xFFFF || totalEntries == 0xFFFF || cdSize == 0xFFFFFFFFu ||
        cdOffset == 0xFFFFFFFFu)
        throw ZipReadError("ZIP: ZIP64 archives are not supported");
    if (diskEntries != totalEntries)
        throw ZipReadError("ZIP: split archives are not supported");
    if (eocdOff + 22 + commentLen != len_)
        throw ZipReadError("ZIP: EOCD comment length does not match file size");
    if (static_cast<uint64_t>(cdOffset) + cdSize > eocdOff)
        throw ZipReadError("ZIP: central directory extends past EOCD");
    if (static_cast<uint64_t>(cdOffset) + cdSize < eocdOff)
        throw ZipReadError("ZIP: unexpected data between central directory and EOCD");
    if (cdOffset >= len_ && totalEntries != 0)
        throw ZipReadError("ZIP: central directory offset out of bounds");

    // Parse central directory entries
    size_t pos = cdOffset;
    std::set<std::string> seenNames;
    std::vector<std::pair<size_t, size_t>> occupiedLocalRanges;
    for (uint16_t i = 0; i < totalEntries; ++i) {
        if (pos + 46 > len_)
            throw ZipReadError("ZIP: central directory entry truncated");

        if (rdLE32(data_ + pos) != 0x02014B50)
            throw ZipReadError("ZIP: invalid central directory header signature");

        ZipEntry entry;
        const uint16_t flags = rdLE16(data_ + pos + 8);
        entry.flags = flags;
        entry.method = rdLE16(data_ + pos + 10);
        entry.crc32 = rdLE32(data_ + pos + 16);
        entry.compressedSize = rdLE32(data_ + pos + 20);
        entry.uncompressedSize = rdLE32(data_ + pos + 24);

        uint16_t nameLen = rdLE16(data_ + pos + 28);
        uint16_t extraLen = rdLE16(data_ + pos + 30);
        uint16_t entryCommentLen = rdLE16(data_ + pos + 32);
        entry.localHeaderOffset = rdLE32(data_ + pos + 42);

        if ((flags & 0x0001) != 0)
            throw ZipReadError("ZIP: encrypted entries are not supported");
        if ((flags & 0x0008) != 0)
            throw ZipReadError("ZIP: data descriptors are not supported");
        if (entry.method != 0 && entry.method != 8)
            throw ZipReadError("ZIP: unsupported compression method " +
                               std::to_string(entry.method));
        if (entry.method == 0 && entry.compressedSize != entry.uncompressedSize)
            throw ZipReadError("ZIP: stored entry has mismatched compressed size");

        size_t centralEnd = pos + 46 + static_cast<size_t>(nameLen) +
                            static_cast<size_t>(extraLen) + static_cast<size_t>(entryCommentLen);
        if (centralEnd > len_ || centralEnd > static_cast<size_t>(cdOffset) + cdSize)
            throw ZipReadError("ZIP: central directory entry truncated");
        if (entry.localHeaderOffset == 0xFFFFFFFFu || entry.compressedSize == 0xFFFFFFFFu ||
            entry.uncompressedSize == 0xFFFFFFFFu)
            throw ZipReadError("ZIP: ZIP64 entry fields are not supported");
        const size_t lhOff = entry.localHeaderOffset;
        if (lhOff + 30 > cdOffset)
            throw ZipReadError("ZIP: local file header points into the central directory");
        if (rdLE32(data_ + lhOff) != 0x04034B50)
            throw ZipReadError("ZIP: invalid local file header signature");
        const uint16_t lhNameLen = rdLE16(data_ + lhOff + 26);
        const uint16_t lhExtraLen = rdLE16(data_ + lhOff + 28);
        if (static_cast<uint64_t>(lhOff) + 30u + lhNameLen + lhExtraLen >
            static_cast<uint64_t>(cdOffset)) {
            throw ZipReadError("ZIP: local file header extends into central directory");
        }
        const size_t dataOff = lhOff + 30u + lhNameLen + lhExtraLen;
        const uint64_t dataEnd64 = static_cast<uint64_t>(dataOff) + entry.compressedSize;
        if (dataEnd64 > cdOffset)
            throw ZipReadError("ZIP: entry payload overlaps central directory");
        const size_t localEnd = static_cast<size_t>(dataEnd64);
        occupiedLocalRanges.emplace_back(lhOff, localEnd);

        entry.name.assign(reinterpret_cast<const char *>(data_ + pos + 46), nameLen);
        if (isUnsafeZipName(entry.name))
            throw ZipReadError("ZIP: unsafe entry path: " + entry.name);
        const std::string duplicateKey = normalizedZipDuplicateKey(entry.name);
        if (!seenNames.insert(duplicateKey).second)
            throw ZipReadError("ZIP: duplicate normalized entry: " + entry.name);
        entries_.push_back(std::move(entry));

        pos = centralEnd;
    }
    std::sort(occupiedLocalRanges.begin(), occupiedLocalRanges.end());
    for (size_t i = 1; i < occupiedLocalRanges.size(); ++i) {
        if (occupiedLocalRanges[i].first < occupiedLocalRanges[i - 1].second)
            throw ZipReadError("ZIP: local file records overlap");
    }
    if (pos != static_cast<size_t>(cdOffset) + cdSize)
        throw ZipReadError("ZIP: central directory size does not match EOCD");
}

/// @brief Return a pointer to the entry whose name matches exactly, or nullptr if not found.
/// Linear search is acceptable because manifests typically contain < 1000 entries.
/// @param name Exact case-sensitive central-directory name.
/// @return Stable pointer into the reader's entry vector, or null when absent.
const ZipEntry *ZipReader::find(const std::string &name) const {
    for (const auto &e : entries_) {
        if (e.name == name)
            return &e;
    }
    return nullptr;
}

/// @brief Extract entry from the archive and return its uncompressed content.
/// Cross-validates flags, method, CRC, and sizes against the central directory.
/// After decompression, the CRC-32 is re-computed and checked against the stored value.
/// @param entry Entry descriptor obtained from this reader.
/// @return Caller-owned uncompressed bytes.
/// @throws ZipReadError On inconsistent headers, invalid bounds/method, size
///         mismatch, decompression failure, or CRC mismatch.
std::vector<uint8_t> ZipReader::extract(const ZipEntry &entry) const {
    // Navigate to local file header
    size_t lhOff = entry.localHeaderOffset;
    if (lhOff + 30 > len_)
        throw ZipReadError("ZIP: local header offset out of bounds");

    if (rdLE32(data_ + lhOff) != 0x04034B50)
        throw ZipReadError("ZIP: invalid local file header signature");

    const uint16_t localFlags = rdLE16(data_ + lhOff + 6);
    const uint16_t localMethod = rdLE16(data_ + lhOff + 8);
    const uint32_t localCrc = rdLE32(data_ + lhOff + 14);
    const uint32_t localCompressedSize = rdLE32(data_ + lhOff + 18);
    const uint32_t localUncompressedSize = rdLE32(data_ + lhOff + 22);
    if ((localFlags & 0x0001) != 0)
        throw ZipReadError("ZIP: encrypted entries are not supported");
    if ((localFlags & 0x0008) != 0)
        throw ZipReadError("ZIP: data descriptors are not supported");
    if (localFlags != entry.flags)
        throw ZipReadError("ZIP: local flags do not match central directory");
    if (localMethod != entry.method)
        throw ZipReadError("ZIP: local compression method does not match central directory");
    if (localCrc != entry.crc32)
        throw ZipReadError("ZIP: local CRC-32 does not match central directory");
    if (localCompressedSize != entry.compressedSize ||
        localUncompressedSize != entry.uncompressedSize)
        throw ZipReadError("ZIP: local sizes do not match central directory");

    uint16_t lhNameLen = rdLE16(data_ + lhOff + 26);
    uint16_t lhExtraLen = rdLE16(data_ + lhOff + 28);
    size_t dataOff = lhOff + 30 + lhNameLen + lhExtraLen;

    if (lhOff + 30 + lhNameLen + lhExtraLen < lhOff)
        throw ZipReadError("ZIP: local header offset overflow");
    if (dataOff > len_ || entry.compressedSize > len_ - dataOff)
        throw ZipReadError("ZIP: entry data extends past buffer");
    std::string localName(reinterpret_cast<const char *>(data_ + lhOff + 30), lhNameLen);
    if (localName != entry.name)
        throw ZipReadError("ZIP: local file name does not match central directory");

    std::vector<uint8_t> result;

    if (entry.method == 0) {
        // Stored — direct copy
        result.assign(data_ + dataOff, data_ + dataOff + entry.compressedSize);
    } else if (entry.method == 8) {
        // DEFLATE
        result = inflate(
            data_ + dataOff, entry.compressedSize, static_cast<size_t>(entry.uncompressedSize));
    } else {
        throw ZipReadError("ZIP: unsupported compression method " + std::to_string(entry.method));
    }

    if (result.size() != entry.uncompressedSize)
        throw ZipReadError("ZIP: uncompressed size mismatch for '" + entry.name + "'");

    // Verify CRC-32 for every entry, including zero-length files/directories.
    static constexpr uint8_t kEmpty = 0;
    uint32_t actualCrc = rt_crc32_compute(result.empty() ? &kEmpty : result.data(), result.size());
    if (actualCrc != entry.crc32)
        throw ZipReadError("ZIP: CRC-32 mismatch for '" + entry.name + "'");

    return result;
}

} // namespace zanna::pkg
