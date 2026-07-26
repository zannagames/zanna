//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/ZipWriter.cpp
// Purpose: ZIP archive writer with Unix permissions. Produces valid archives
//          that preserve file mode bits when extracted on macOS/Linux.
//
// Key invariants:
//   - version_made_by byte layout: high byte = host OS (3 = Unix), low byte =
//     spec version (20 = 2.0), giving (3 << 8) | 20 = 0x0314.
//   - external_file_attributes: upper 16 bits = Unix mode, lower 16 = MS-DOS.
//   - Compression: DEFLATE (method 8) for entries > 64 bytes when it saves space.
//   - Symlinks: stored with typeflag in external attributes (0120000 prefix).
//
// Ownership/Lifetime:
//   - Internal buffer freed on destruction or after finish().
//
// Links: ZipWriter.hpp, PkgDeflate.hpp, rt_crc32.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements deterministic ZIP32 writing with Unix file metadata.
/// @details Entries are sanitized and copied into local records immediately;
///          finalization emits the central directory and EOCD, with optional
///          DEFLATE compression and reproducible DOS timestamps.

#include "ZipWriter.hpp"
#include "PkgDeflate.hpp"
#include "PkgUtils.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <stdexcept>

extern "C" {
uint32_t rt_crc32_compute(const uint8_t *data, size_t len);
}

namespace zanna::pkg {

//=============================================================================
// ZIP Constants
//=============================================================================

static constexpr uint32_t kLocalHeaderSig = 0x04034B50;
static constexpr uint32_t kCentralHeaderSig = 0x02014B50;
static constexpr uint32_t kEndRecordSig = 0x06054B50;

static constexpr uint16_t kMethodStored = 0;
static constexpr uint16_t kMethodDeflate = 8;
static constexpr uint16_t kGeneralPurposeUtf8Flag = 1u << 11;

static constexpr uint16_t kVersionNeeded = 20;
// Unix(3) in high byte, version 2.0 (20) in low byte
static constexpr uint16_t kVersionMadeBy = (3 << 8) | 20;

static constexpr int kLocalHeaderSize = 30;
static constexpr int kCentralHeaderSize = 46;
static constexpr int kEndRecordSize = 22;

//=============================================================================
// Helpers
//=============================================================================

/// @brief Write a 16-bit little-endian integer into an unaligned byte buffer at p.
/// @param p Pointer to at least two writable bytes.
/// @param v Host-order value to encode.
static inline void putU16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

/// @brief Write a 32-bit little-endian integer into an unaligned byte buffer at p.
/// @param p Pointer to at least four writable bytes.
/// @param v Host-order value to encode.
static inline void putU32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

//=============================================================================
// ZipWriter Implementation
//=============================================================================

/// @brief Pre-allocate 64 KiB in buffer_ to avoid frequent reallocations for typical
/// small package payloads. The buffer grows automatically beyond this.
ZipWriter::ZipWriter() {
    buffer_.reserve(65536);
}

ZipWriter::~ZipWriter() = default;

/// @brief Reject mutation or repeated finalization of a completed archive.
/// @throws std::runtime_error If this writer has already been finalized.
void ZipWriter::ensureOpen() const {
    if (finalized_)
        throw std::runtime_error("ZipWriter: archive has already been finalized");
}

/// @brief Enforce ZIP32 limits: entry count ≤ 65535, archive size ≤ 4 GiB, name length ≤ 65535.
/// All three limits arise from the 16- or 32-bit integer fields in the PKWARE spec.
/// @param value Candidate current or projected quantity.
/// @param maxValue Largest value representable by the corresponding ZIP32 field.
/// @param what Quantity description used in diagnostics.
/// @throws std::runtime_error If @p value exceeds @p maxValue.
void ZipWriter::validateArchiveLimit(size_t value, size_t maxValue, const char *what) const {
    if (value > maxValue) {
        throw std::runtime_error(std::string("ZipWriter: ZIP64 is not supported for ") + what);
    }
}

/// @brief Check a projected append against the 32-bit archive-offset limit.
/// @param additionalBytes Number of bytes about to be appended.
/// @param what Quantity description used in diagnostics.
/// @throws std::runtime_error If the projected buffer cannot use ZIP32 offsets.
void ZipWriter::validateProjectedArchiveSize(size_t additionalBytes, const char *what) const {
    if (additionalBytes > 0xFFFFFFFFu || buffer_.size() > 0xFFFFFFFFu - additionalBytes) {
        throw std::runtime_error(std::string("ZipWriter: ZIP64 is not supported for ") + what);
    }
}

/// @brief Normalize and validate a ZIP entry name: convert backslashes to forward
/// slashes, strip trailing slashes (then re-append for directories), and run
/// sanitizePackageRelativePath to reject absolute paths and ".." components.
/// @param name Caller-supplied file or directory entry name.
/// @return Sanitized ZIP32-length path using forward slashes.
/// @throws std::runtime_error If the path is empty/unsafe or exceeds ZIP32 limits.
std::string ZipWriter::normalizeEntryName(const std::string &name) const {
    validateArchiveLimit(name.size(), 0xFFFFu, "entry names longer than 65535 bytes");
    std::string normalized = name;
    const bool isDirectory = !normalized.empty() && normalized.back() == '/';
    for (char &c : normalized) {
        if (c == '\\')
            c = '/';
    }
    while (!normalized.empty() && normalized.back() == '/')
        normalized.pop_back();
    normalized = sanitizePackageRelativePath(normalized, "zip entry name");
    if (normalized.empty() && !isDirectory)
        throw std::runtime_error("ZipWriter: entry name must not be empty");
    if (isDirectory && !normalized.empty())
        normalized.push_back('/');
    validateArchiveLimit(normalized.size(), 0xFFFFu, "entry names longer than 65535 bytes");
    return normalized;
}

/// @brief Normalize a symlink target and require it to remain inside the archive root.
/// @param entryName Sanitized archive path of the symlink.
/// @param target Caller-supplied target text.
/// @return Slash-normalized relative target.
/// @throws std::runtime_error If empty, multi-line, absolute, escaping, or oversized.
std::string ZipWriter::normalizeSymlinkTarget(const std::string &entryName,
                                              const std::string &target) const {
    if (target.empty())
        throw std::runtime_error("ZipWriter: symlink target must not be empty");
    validateSingleLineField(target, "zip symlink target");
    validateArchiveLimit(target.size(), 0xFFFFFFFFu, "symlinks larger than 4 GiB");

    std::string normalizedTarget = target;
    for (char &c : normalizedTarget) {
        if (c == '\\')
            c = '/';
    }
    if (normalizedTarget.front() == '/' ||
        (normalizedTarget.size() >= 2 &&
         std::isalpha(static_cast<unsigned char>(normalizedTarget[0])) &&
         normalizedTarget[1] == ':')) {
        throw std::runtime_error("ZipWriter: symlink target must be relative: " + target);
    }

    const std::filesystem::path resolved =
        (std::filesystem::path(entryName).parent_path() / normalizedTarget).lexically_normal();
    const std::string resolvedText = resolved.generic_string();
    if (resolvedText.empty() || resolvedText == "." || resolvedText == ".." ||
        resolvedText.rfind("../", 0) == 0) {
        throw std::runtime_error("ZipWriter: symlink target escapes archive root: " + target);
    }
    return normalizedTarget;
}

/// @brief Append len raw bytes to the internal archive buffer.
/// @param data Source bytes; may be null only when @p len is zero.
/// @param len Number of bytes to append.
/// @throws std::runtime_error For null non-empty input.
void ZipWriter::writeBytes(const uint8_t *data, size_t len) {
    if (len == 0)
        return;
    if (data == nullptr)
        throw std::runtime_error("ZipWriter: null data pointer for non-empty write");
    buffer_.insert(buffer_.end(), data, data + len);
}

/// @brief Append a 16-bit little-endian integer to the archive buffer.
/// @param v Host-order value to encode.
void ZipWriter::writeU16(uint16_t v) {
    uint8_t buf[2];
    putU16(buf, v);
    writeBytes(buf, 2);
}

/// @brief Append a 32-bit little-endian integer to the archive buffer.
/// @param v Host-order value to encode.
void ZipWriter::writeU32(uint32_t v) {
    uint8_t buf[4];
    putU32(buf, v);
    writeBytes(buf, 4);
}

namespace {

/// @brief Compute CRC-32 over a buffer, tolerating a null pointer when empty.
/// @details Passes a dummy byte for zero-length input so the runtime CRC call
///          never dereferences null; used to checksum every ZIP entry.
/// @param data Input bytes; may be null only when @p len is zero.
/// @param len Number of bytes to checksum.
/// @return CRC-32 of the uncompressed entry data.
uint32_t crc32Bytes(const uint8_t *data, size_t len) {
    static constexpr uint8_t kEmpty = 0;
    return rt_crc32_compute(len == 0 ? &kEmpty : data, len);
}

/// @brief Thread-safe UTC conversion (gmtime_s on Windows, gmtime_r else).
/// @param timestamp Unix timestamp to convert.
/// @param out Receives broken-down UTC on success.
/// @return false if the platform call fails.
bool portableGmTime(std::time_t timestamp, std::tm &out) {
#if defined(_WIN32)
    return gmtime_s(&out, &timestamp) == 0;
#else
    return gmtime_r(&timestamp, &out) != nullptr;
#endif
}

/// @brief Read the SOURCE_DATE_EPOCH environment variable for reproducible builds.
/// @details When set to a valid Unix timestamp, stores it in @p timestamp and
///          returns true so callers stamp archive entries deterministically (in
///          UTC). Returns false when the variable is unset/empty; throws on a
///          malformed or out-of-range value.
/// @param timestamp Receives the parsed epoch on success.
/// @return true when a valid SOURCE_DATE_EPOCH was found.
bool sourceDateEpoch(std::time_t &timestamp) {
    const char *env = std::getenv("SOURCE_DATE_EPOCH");
    if (env == nullptr || *env == '\0')
        return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(env, &end, 10);
    if (errno != 0 || end == env || *end != '\0')
        throw std::runtime_error("ZipWriter: invalid SOURCE_DATE_EPOCH");
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<std::time_t>::max()))
        throw std::runtime_error("ZipWriter: SOURCE_DATE_EPOCH is too large for this host");
    timestamp = static_cast<std::time_t>(parsed);
    return true;
}

/// @brief Clamp a broken-down time into the DOS date range (1980-01-01..2107).
/// @details The DOS date format cannot represent years before 1980 or after
///          2107, so out-of-range times are pinned to the respective boundary to
///          keep the emitted ZIP date field valid.
/// @param t Broken-down UTC value modified in place if outside the DOS range.
void clampDosTime(std::tm &t) {
    const int year = t.tm_year + 1900;
    if (year < 1980) {
        t = {};
        t.tm_year = 80;
        t.tm_mon = 0;
        t.tm_mday = 1;
    } else if (year > 2107) {
        t = {};
        t.tm_year = 207;
        t.tm_mon = 11;
        t.tm_mday = 31;
        t.tm_hour = 23;
        t.tm_min = 59;
        t.tm_sec = 58;
    }
}

} // namespace

/// @brief Populate time and date with a DOS timestamp. Reproducible by default: stamps a fixed
/// epoch interpreted in UTC, so identical inputs yield byte-identical archives regardless of build
/// time or host timezone. Set SOURCE_DATE_EPOCH to stamp a specific Unix timestamp instead.
/// @param time Receives packed DOS time.
/// @param date Receives packed DOS date.
/// @throws std::runtime_error If `SOURCE_DATE_EPOCH` is malformed or out of range.
void ZipWriter::getDosTime(uint16_t &time, uint16_t &date) {
    std::time_t stamp = 0;        // Deterministic default; clamped up to the 1980 DOS floor below.
    (void)sourceDateEpoch(stamp); // Overrides `stamp` only when SOURCE_DATE_EPOCH is set.
    std::tm t{};
    if (!portableGmTime(stamp, t)) {
        time = 0;
        date = 0x0021; // 1980-01-01
        return;
    }
    clampDosTime(t);
    time = static_cast<uint16_t>((t.tm_sec / 2) | (t.tm_min << 5) | (t.tm_hour << 11));
    date = static_cast<uint16_t>(t.tm_mday | ((t.tm_mon + 1) << 5) | ((t.tm_year - 80) << 9));
}

/// @brief Add a regular file entry. Computes CRC-32, optionally DEFLATE-compresses the
/// data (skipped if compressed output ≥ original), writes the local file header,
/// and records an Entry + LayoutEntry for central-directory and stub-offset use.
/// @param name Archive-relative entry name.
/// @param data Uncompressed file bytes; may be null only when @p len is zero.
/// @param len Number of uncompressed bytes.
/// @param unixMode Unix type and permission bits for external attributes.
/// @throws std::runtime_error On duplicate/unsafe input, finalization, compression,
///         null data, or a ZIP32 limit.
void ZipWriter::addFile(const std::string &name,
                        const uint8_t *data,
                        size_t len,
                        uint32_t unixMode) {
    ensureOpen();
    const std::string entryName = normalizeEntryName(name);
    if (!entryName.empty() && entryName.back() == '/')
        throw std::runtime_error("ZipWriter: regular file entry must not end with '/': " + name);
    if (!seenNames_.insert(entryName).second)
        throw std::runtime_error("ZipWriter: duplicate entry name: " + entryName);
    validateArchiveLimit(len, 0xFFFFFFFFu, "files larger than 4 GiB");
    validateArchiveLimit(entries_.size() + 1, 0xFFFFu, "more than 65535 entries");
    validateArchiveLimit(buffer_.size(), 0xFFFFFFFFu, "archives larger than 4 GiB");

    if (len > 0 && data == nullptr)
        throw std::runtime_error("ZipWriter: null data pointer for non-empty file: " + entryName);
    uint32_t crc = crc32Bytes(data, len);

    // Decide compression
    uint16_t method = kMethodStored;
    const uint8_t *writeData = data;
    size_t writeLen = len;
    std::vector<uint8_t> compressed;

    if (compressionEnabled_ && len > 64) {
        compressed = deflate(data, len);
        if (compressed.size() < len) {
            method = kMethodDeflate;
            writeData = compressed.data();
            writeLen = compressed.size();
        }
    }
    validateArchiveLimit(writeLen, 0xFFFFFFFFu, "compressed files larger than 4 GiB");
    validateProjectedArchiveSize(kLocalHeaderSize + entryName.size() + writeLen,
                                 "archives larger than 4 GiB");

    // Record entry
    Entry e;
    e.name = entryName;
    e.crc32 = crc;
    e.compressedSize = static_cast<uint32_t>(writeLen);
    e.uncompressedSize = static_cast<uint32_t>(len);
    e.method = method;
    getDosTime(e.modTime, e.modDate);
    e.localOffset = static_cast<uint32_t>(buffer_.size());
    // Unix mode in upper 16 bits, MS-DOS archive attribute in lower
    e.externalAttrs = (unixMode << 16);

    // Write local file header
    uint8_t lh[kLocalHeaderSize];
    putU32(lh + 0, kLocalHeaderSig);
    putU16(lh + 4, kVersionNeeded);
    putU16(lh + 6, kGeneralPurposeUtf8Flag); // General purpose flags
    putU16(lh + 8, method);
    putU16(lh + 10, e.modTime);
    putU16(lh + 12, e.modDate);
    putU32(lh + 14, crc);
    putU32(lh + 18, static_cast<uint32_t>(writeLen));
    putU32(lh + 22, static_cast<uint32_t>(len));
    putU16(lh + 26, static_cast<uint16_t>(entryName.size()));
    putU16(lh + 28, 0); // Extra field length

    writeBytes(lh, kLocalHeaderSize);
    writeBytes(reinterpret_cast<const uint8_t *>(entryName.data()), entryName.size());
    writeBytes(writeData, writeLen);

    entries_.push_back(std::move(e));
    layoutEntries_.push_back(LayoutEntry{
        entryName,
        entries_.back().localOffset,
        static_cast<uint32_t>(entries_.back().localOffset + kLocalHeaderSize + entryName.size()),
        entries_.back().compressedSize,
        entries_.back().uncompressedSize,
        entries_.back().crc32,
        entries_.back().method,
        false});
}

/// @brief Convenience overload: add a file entry whose content is a std::string.
/// @param name Archive-relative entry name.
/// @param content File bytes to copy without transcoding.
/// @param unixMode Unix type and permission bits.
/// @throws std::runtime_error Under the same conditions as addFile().
void ZipWriter::addFileString(const std::string &name,
                              const std::string &content,
                              uint32_t unixMode) {
    addFile(name, reinterpret_cast<const uint8_t *>(content.data()), content.size(), unixMode);
}

/// @brief Add a directory entry (zero-length data, trailing "/" in name). The MS-DOS
/// directory attribute bit (0x10) is set in the lower half of externalAttrs so
/// Windows extractors create the directory even without reading Unix mode bits.
/// @param name Archive-relative directory name.
/// @param unixMode Unix directory type and permission bits.
/// @throws std::runtime_error On duplicate/unsafe input, finalization, or ZIP32 limits.
void ZipWriter::addDirectory(const std::string &name, uint32_t unixMode) {
    ensureOpen();
    std::string dirName = name;
    if (dirName.empty() || dirName.back() != '/')
        dirName += '/';
    dirName = normalizeEntryName(dirName);
    if (dirName.empty())
        return;
    const std::string key =
        (dirName.back() == '/') ? dirName.substr(0, dirName.size() - 1) : dirName;
    if (!key.empty() && !seenNames_.insert(key).second)
        throw std::runtime_error("ZipWriter: duplicate entry name: " + key);
    validateArchiveLimit(entries_.size() + 1, 0xFFFFu, "more than 65535 entries");
    validateArchiveLimit(buffer_.size(), 0xFFFFFFFFu, "archives larger than 4 GiB");
    validateProjectedArchiveSize(kLocalHeaderSize + dirName.size(), "archives larger than 4 GiB");

    Entry e;
    e.name = dirName;
    e.crc32 = 0;
    e.compressedSize = 0;
    e.uncompressedSize = 0;
    e.method = kMethodStored;
    getDosTime(e.modTime, e.modDate);
    e.localOffset = static_cast<uint32_t>(buffer_.size());
    // Unix dir mode in upper 16 bits, MS-DOS directory attribute (0x10) in lower
    e.externalAttrs = (unixMode << 16) | 0x10;

    uint8_t lh[kLocalHeaderSize];
    putU32(lh + 0, kLocalHeaderSig);
    putU16(lh + 4, kVersionNeeded);
    putU16(lh + 6, kGeneralPurposeUtf8Flag);
    putU16(lh + 8, kMethodStored);
    putU16(lh + 10, e.modTime);
    putU16(lh + 12, e.modDate);
    putU32(lh + 14, 0);
    putU32(lh + 18, 0);
    putU32(lh + 22, 0);
    putU16(lh + 26, static_cast<uint16_t>(dirName.size()));
    putU16(lh + 28, 0);

    writeBytes(lh, kLocalHeaderSize);
    writeBytes(reinterpret_cast<const uint8_t *>(dirName.data()), dirName.size());

    entries_.push_back(std::move(e));
    layoutEntries_.push_back(LayoutEntry{
        dirName,
        entries_.back().localOffset,
        static_cast<uint32_t>(entries_.back().localOffset + kLocalHeaderSize + dirName.size()),
        0,
        0,
        0,
        entries_.back().method,
        true});
}

/// @brief Add a Unix symlink entry. The symlink target is stored as the entry's file
/// data. Unix mode 0120777 in the upper 16 bits of externalAttrs tells Info-ZIP
/// compatible extractors (including macOS Archive Utility) that this is a symlink.
/// @param name Archive-relative symlink name.
/// @param target Relative target that must remain inside the archive root.
/// @throws std::runtime_error On duplicate/unsafe input, finalization, or ZIP32 limits.
void ZipWriter::addSymlink(const std::string &name, const std::string &target) {
    ensureOpen();
    const std::string entryName = normalizeEntryName(name);
    if (!seenNames_.insert(entryName).second)
        throw std::runtime_error("ZipWriter: duplicate entry name: " + entryName);
    const std::string normalizedTarget = normalizeSymlinkTarget(entryName, target);
    validateArchiveLimit(entries_.size() + 1, 0xFFFFu, "more than 65535 entries");
    validateArchiveLimit(buffer_.size(), 0xFFFFFFFFu, "archives larger than 4 GiB");
    validateProjectedArchiveSize(kLocalHeaderSize + entryName.size() + normalizedTarget.size(),
                                 "archives larger than 4 GiB");

    // Symlinks store the target path as file data
    auto *data = reinterpret_cast<const uint8_t *>(normalizedTarget.data());
    size_t len = normalizedTarget.size();
    uint32_t crc = crc32Bytes(data, len);

    Entry e;
    e.name = entryName;
    e.crc32 = crc;
    e.compressedSize = static_cast<uint32_t>(len);
    e.uncompressedSize = static_cast<uint32_t>(len);
    e.method = kMethodStored;
    getDosTime(e.modTime, e.modDate);
    e.localOffset = static_cast<uint32_t>(buffer_.size());
    // Symlink: Unix mode 0120777 in upper 16 bits
    e.externalAttrs = (0120777U << 16);

    uint8_t lh[kLocalHeaderSize];
    putU32(lh + 0, kLocalHeaderSig);
    putU16(lh + 4, kVersionNeeded);
    putU16(lh + 6, kGeneralPurposeUtf8Flag);
    putU16(lh + 8, kMethodStored);
    putU16(lh + 10, e.modTime);
    putU16(lh + 12, e.modDate);
    putU32(lh + 14, crc);
    putU32(lh + 18, static_cast<uint32_t>(len));
    putU32(lh + 22, static_cast<uint32_t>(len));
    putU16(lh + 26, static_cast<uint16_t>(entryName.size()));
    putU16(lh + 28, 0);

    writeBytes(lh, kLocalHeaderSize);
    writeBytes(reinterpret_cast<const uint8_t *>(entryName.data()), entryName.size());
    writeBytes(data, len);

    entries_.push_back(std::move(e));
    layoutEntries_.push_back(LayoutEntry{
        entryName,
        entries_.back().localOffset,
        static_cast<uint32_t>(entries_.back().localOffset + kLocalHeaderSize + entryName.size()),
        entries_.back().compressedSize,
        entries_.back().uncompressedSize,
        entries_.back().crc32,
        entries_.back().method,
        false});
}

/// @brief Append central directory file headers followed by the EOCD record to @p archive.
/// @details This helper does not mutate finalized_ and is safe for staging a completed archive in
/// a temporary buffer before committing writer state.
/// @param archive Buffer already containing every local record; receives final records.
/// @throws std::runtime_error If entry counts, names, or projected offsets exceed ZIP32.
void ZipWriter::appendCentralDirectory(std::vector<uint8_t> &archive) const {
    validateArchiveLimit(entries_.size(), 0xFFFFu, "more than 65535 entries");
    validateArchiveLimit(archive.size(), 0xFFFFFFFFu, "archives larger than 4 GiB");

    /// @brief Append one byte range to the staged archive.
    /// @param data Borrowed source bytes.
    /// @param len Number of bytes to append.
    auto appendBytes = [&archive](const uint8_t *data, size_t len) {
        archive.insert(archive.end(), data, data + len);
    };

    uint32_t cdOffset = static_cast<uint32_t>(archive.size());

    for (const auto &e : entries_) {
        (void)normalizeEntryName(e.name);
        uint8_t ch[kCentralHeaderSize];
        putU32(ch + 0, kCentralHeaderSig);
        putU16(ch + 4, kVersionMadeBy);
        putU16(ch + 6, kVersionNeeded);
        putU16(ch + 8, kGeneralPurposeUtf8Flag); // Flags
        putU16(ch + 10, e.method);
        putU16(ch + 12, e.modTime);
        putU16(ch + 14, e.modDate);
        putU32(ch + 16, e.crc32);
        putU32(ch + 20, e.compressedSize);
        putU32(ch + 24, e.uncompressedSize);
        putU16(ch + 28, static_cast<uint16_t>(e.name.size()));
        putU16(ch + 30, 0); // Extra field length
        putU16(ch + 32, 0); // Comment length
        putU16(ch + 34, 0); // Disk number start
        putU16(ch + 36, 0); // Internal file attributes
        putU32(ch + 38, e.externalAttrs);
        putU32(ch + 42, e.localOffset);

        appendBytes(ch, kCentralHeaderSize);
        appendBytes(reinterpret_cast<const uint8_t *>(e.name.data()), e.name.size());
    }

    validateArchiveLimit(archive.size(), 0xFFFFFFFFu, "archives larger than 4 GiB");
    validateArchiveLimit(
        archive.size() + kEndRecordSize, 0xFFFFFFFFu, "archives larger than 4 GiB");
    uint32_t cdSize = static_cast<uint32_t>(archive.size()) - cdOffset;

    // End of central directory record
    uint8_t eocd[kEndRecordSize];
    putU32(eocd + 0, kEndRecordSig);
    putU16(eocd + 4, 0); // Disk number
    putU16(eocd + 6, 0); // Disk with central directory
    putU16(eocd + 8, static_cast<uint16_t>(entries_.size()));
    putU16(eocd + 10, static_cast<uint16_t>(entries_.size()));
    putU32(eocd + 12, cdSize);
    putU32(eocd + 16, cdOffset);
    putU16(eocd + 20, 0); // Comment length

    appendBytes(eocd, kEndRecordSize);
}

/// @brief Append all central directory file headers followed by the End-of-Central-
/// Directory (EOCD) record. Must be called exactly once, after all entries have
/// been added. The central directory echoes each local header's metadata plus
/// version_made_by (Unix) and externalAttrs (Unix mode) fields.
void ZipWriter::writeCentralDirectory() {
    ensureOpen();
    appendCentralDirectory(buffer_);
    finalized_ = true;
}

/// @brief Finalize the archive and write it to disk at path. Appends the central
/// directory and EOCD, then writes the entire buffer to the output file.
/// @param path Destination replaced atomically after successful staging.
/// @throws std::runtime_error On repeated finalization, ZIP32 overflow, or write failure.
void ZipWriter::finish(const std::string &path) {
    ensureOpen();

    std::vector<uint8_t> complete = buffer_;
    appendCentralDirectory(complete);
    writeFileAtomic(path, complete);
    buffer_ = std::move(complete);
    finalized_ = true;
}

/// @brief Finalize the archive and return the complete ZIP bytes. Used when the caller
/// needs the payload in memory (e.g. to append it as a PE overlay) rather than
/// writing to disk. Moves the internal buffer to avoid a copy.
std::vector<uint8_t> ZipWriter::finishToVector() {
    writeCentralDirectory();
    return std::move(buffer_);
}

} // namespace zanna::pkg
