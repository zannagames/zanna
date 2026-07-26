//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/TarWriter.cpp
// Purpose: Write USTAR tar archives for .deb data/control tarballs.
//
// Key invariants:
//   - Header is exactly 512 bytes (struct posix_header layout).
//   - Octal fields are NUL-terminated ASCII strings.
//   - Checksum computed with checksum field treated as 8 spaces (0x20).
//   - Data blocks padded with NUL bytes to 512-byte boundary.
//   - Archive ends with 2 x 512 zero blocks (1024 bytes of NUL).
//
// Ownership/Lifetime:
//   - Single-use writer, accumulates entries then outputs.
//
// Links: TarWriter.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements deterministic USTAR serialization with POSIX PAX extensions.
/// @details Entry paths and symlink targets are normalized before storage;
///          serialization emits bounded octal fields, checksums, block padding,
///          optional long-path metadata, and the canonical end marker.

#include "TarWriter.hpp"
#include "PkgUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace zanna::pkg {

namespace {

/// @brief Add @p value to @p total while checking for size_t overflow.
/// @details Archive writers pre-reserve their final output buffers. A wrapped estimate would cause
///          under-reservation and potentially mask pathological inputs, so overflow is reported as a
///          package-construction error before serialization begins.
/// @param total Running estimate to increase.
/// @param value Number of bytes to add.
/// @param archiveKind Writer name included in the exception message.
/// @throws std::runtime_error If the sum cannot be represented by `size_t`.
void checkedAddEstimate(size_t &total, size_t value, const char *archiveKind) {
    if (value > std::numeric_limits<size_t>::max() - total)
        throw std::runtime_error(std::string(archiveKind) + ": archive size estimate overflow");
    total += value;
}

/// @brief Normalize a tar entry path: strips the leading "./" prefix (emitted by some
/// tools), removes trailing slashes, sanitizes via sanitizePackageRelativePath,
/// then re-appends "/" for directory entries. Returns "" for the root ".".
/// @param path Caller-supplied archive path.
/// @param directory Whether the entry is a directory requiring a trailing slash.
/// @param fieldName Context label used by path-validation diagnostics.
/// @return Sanitized archive-relative path, or empty text for the directory root.
/// @throws std::runtime_error If the path is absolute, traversing, or otherwise unsafe.
std::string normalizeTarEntryPath(const std::string &path, bool directory, const char *fieldName) {
    std::string clean = path;
    if (clean.rfind("./", 0) == 0)
        clean = clean.substr(2);
    while (!clean.empty() && clean.back() == '/')
        clean.pop_back();
    if (clean.empty())
        return directory ? std::string() : sanitizePackageRelativePath(clean, fieldName);
    clean = sanitizePackageRelativePath(clean, fieldName);
    if (directory && !clean.empty())
        clean.push_back('/');
    return clean;
}

/// @brief Validate that a tar symlink target is safe to include in the archive.
/// Rejects empty targets, absolute paths, Windows drive paths, and targets that
/// resolve outside the archive root when combined with the symlink's directory.
/// @param linkPath Sanitized archive path of the symlink entry.
/// @param target Caller-supplied target to normalize and validate.
/// @return Slash-normalized relative target suitable for the tar link-name field.
/// @throws std::runtime_error If the target is empty, multi-line, absolute, or escaping.
std::string normalizeTarSymlinkTarget(const std::string &linkPath, const std::string &target) {
    if (target.empty())
        throw std::runtime_error("tar symlink target must not be empty");
    validateSingleLineField(target, "tar symlink target");
    std::string normalizedTarget = target;
    for (char &c : normalizedTarget) {
        if (c == '\\')
            c = '/';
    }
    if (normalizedTarget.front() == '/' ||
        (normalizedTarget.size() >= 2 &&
         std::isalpha(static_cast<unsigned char>(normalizedTarget[0])) &&
         normalizedTarget[1] == ':')) {
        throw std::runtime_error("tar symlink target must be relative: " + target);
    }

    std::string cleanLink = normalizeTarEntryPath(linkPath, false, "tar symlink path");
    const std::filesystem::path resolved =
        (std::filesystem::path(cleanLink).parent_path() / normalizedTarget).lexically_normal();
    const std::string resolvedText = resolved.generic_string();
    if (resolvedText.empty() || resolvedText == "." || resolvedText.rfind("../", 0) == 0 ||
        resolvedText == "..") {
        throw std::runtime_error("tar symlink target escapes archive root: " + target);
    }
    return normalizedTarget;
}

} // namespace

/// @brief Add a regular file entry (typeflag '0') to the archive.
/// Normalizes path, checks for duplicates, and stores a copy of the data.
/// @param path Archive-relative file path.
/// @param data File bytes; may be null only when @p size is zero.
/// @param size Number of bytes available at @p data.
/// @param mode Unix permission bits encoded in the header.
/// @param mtime Unix modification timestamp encoded in the header.
/// @throws std::runtime_error If the path is invalid/duplicate or data is null.
void TarWriter::addFile(
    const std::string &path, const uint8_t *data, size_t size, uint32_t mode, uint32_t mtime) {
    const std::string cleanPath = normalizeTarEntryPath(path, false, "tar file path");
    if (cleanPath.empty())
        throw std::runtime_error("tar file path must not be empty");
    if (!seenPaths_.insert(cleanPath).second)
        throw std::runtime_error("duplicate tar entry path: " + cleanPath);
    Entry e;
    e.path = cleanPath;
    if (size > 0) {
        if (data == nullptr)
            throw std::runtime_error("tar file data pointer is null for non-empty file: " +
                                     cleanPath);
        e.data.assign(data, data + size);
    }
    e.mode = mode;
    e.mtime = mtime;
    e.typeflag = '0';
    entries_.push_back(std::move(e));
}

/// @brief Convenience overload: add a file whose content is given as a std::string.
/// @param path Archive-relative file path.
/// @param content Bytes to copy into the pending entry.
/// @param mode Unix permission bits encoded in the header.
/// @param mtime Unix modification timestamp encoded in the header.
/// @throws std::runtime_error If the path is invalid or duplicates another entry.
void TarWriter::addFileString(const std::string &path,
                              const std::string &content,
                              uint32_t mode,
                              uint32_t mtime) {
    addFile(path, reinterpret_cast<const uint8_t *>(content.data()), content.size(), mode, mtime);
}

/// @brief Convenience overload: add a file whose content is given as a byte vector.
/// @param path Archive-relative file path.
/// @param data Bytes to copy into the pending entry.
/// @param mode Unix permission bits encoded in the header.
/// @param mtime Unix modification timestamp encoded in the header.
/// @throws std::runtime_error If the path is invalid or duplicates another entry.
void TarWriter::addFileVec(const std::string &path,
                           const std::vector<uint8_t> &data,
                           uint32_t mode,
                           uint32_t mtime) {
    addFile(path, data.data(), data.size(), mode, mtime);
}

/// @brief Add a directory entry (typeflag '5') to the archive.
/// The path is normalized and a trailing "/" is always appended. The root
/// directory "." maps to "./" and is allowed without duplicate-path rejection.
/// @param path Archive-relative directory path.
/// @param mode Unix permission bits encoded in the header.
/// @param mtime Unix modification timestamp encoded in the header.
/// @throws std::runtime_error If a non-root path is invalid or duplicates another entry.
void TarWriter::addDirectory(const std::string &path, uint32_t mode, uint32_t mtime) {
    const std::string cleanPath = normalizeTarEntryPath(path, true, "tar directory path");
    if (!cleanPath.empty()) {
        std::string key = cleanPath;
        key.pop_back();
        if (!seenPaths_.insert(key).second)
            throw std::runtime_error("duplicate tar entry path: " + key);
    }
    Entry e;
    e.path = cleanPath.empty() ? std::string("./") : cleanPath;
    e.mode = mode;
    e.mtime = mtime;
    e.typeflag = '5';
    entries_.push_back(std::move(e));
}

/// @brief Add a symbolic link entry (typeflag '2') to the archive.
/// Normalizes path, validates the target is relative and does not escape the
/// archive root, and stores mode 0777 (conventional for symlinks).
/// @param path Archive-relative path of the symlink.
/// @param target Relative target resolved from the symlink's parent directory.
/// @param mtime Unix modification timestamp encoded in the header.
/// @throws std::runtime_error If the path/target is unsafe or the path is duplicate.
void TarWriter::addSymlink(const std::string &path, const std::string &target, uint32_t mtime) {
    const std::string cleanPath = normalizeTarEntryPath(path, false, "tar symlink path");
    if (cleanPath.empty())
        throw std::runtime_error("tar symlink path must not be empty");
    if (!seenPaths_.insert(cleanPath).second)
        throw std::runtime_error("duplicate tar entry path: " + cleanPath);
    const std::string normalizedTarget = normalizeTarSymlinkTarget(cleanPath, target);
    Entry e;
    e.path = cleanPath;
    e.linkTarget = normalizedTarget;
    e.mode = 0777;
    e.mtime = mtime;
    e.typeflag = '2';
    entries_.push_back(std::move(e));
}

namespace {

/// @brief Write an octal value as NUL-terminated ASCII into a fixed-width field.
/// Format: (width-1) octal digits + NUL byte.
/// @param field Destination buffer of at least @p width bytes.
/// @param width Fixed field width, including the final NUL.
/// @param value Unsigned value to encode with leading zeroes.
/// @throws std::runtime_error If @p value exceeds the field's octal capacity.
void writeOctal(uint8_t *field, size_t width, uint64_t value) {
    uint64_t maxValue = 0;
    for (size_t i = 0; i + 1 < width; ++i)
        maxValue = (maxValue << 3) | 7u;
    if (value > maxValue)
        throw std::runtime_error("tar numeric field overflow");

    // Fill with zeros first
    std::memset(field, '0', width - 1);
    field[width - 1] = '\0';

    // Write digits from right to left
    size_t pos = width - 2;
    if (value == 0) {
        field[pos] = '0';
        return;
    }
    while (value > 0 && pos < width) {
        field[pos] = static_cast<uint8_t>('0' + (value & 7));
        value >>= 3;
        if (pos == 0)
            break;
        pos--;
    }
}

/// @brief Copy a string into a zero-initialized fixed-width header field.
/// @param field Destination field, expected to have been zero-filled by the caller.
/// @param width Maximum number of bytes that may be copied.
/// @param s Bytes to write without adding an explicit terminator.
/// @throws std::runtime_error If @p s is wider than the destination field.
void writeString(uint8_t *field, size_t width, const std::string &s) {
    if (s.size() > width)
        throw std::runtime_error("tar string field too long: " + s);
    size_t len = s.size();
    std::memcpy(field, s.data(), len);
}

/// @brief Build one POSIX PAX key/value record with a stable length prefix.
/// @details PAX records encode their own decimal byte length, including the
///          length digits, the following space, the key/value payload, and the
///          trailing newline. The length can grow when more digits are needed,
///          so this helper iterates until the prefix length is stable.
/// @param key PAX key, such as `linkpath`.
/// @param value PAX value to emit verbatim.
/// @return One complete `LEN key=value\n` record.
std::string buildPaxRecord(const std::string &key, const std::string &value) {
    std::string payload = key + "=" + value + "\n";
    size_t len = payload.size() + 2u;
    while (true) {
        const std::string prefix = std::to_string(len);
        const size_t actualLen = prefix.size() + 1u + payload.size();
        if (actualLen == len)
            return prefix + " " + payload;
        len = actualLen;
    }
}

/// @brief Return the short synthetic USTAR path used for an extended PAX header.
/// @details PAX headers are metadata records and should not collide with payload
///          entries. The generated name is kept below the 100-byte USTAR name
///          limit so it never requires another extension header.
/// @param index Monotonic index of the generated PAX header in the archive.
/// @return Archive path for the PAX header.
std::string paxHeaderPath(size_t index) {
    return "PaxHeaders.X/zanna-" + std::to_string(index);
}

/// @brief Return the short fallback USTAR path for an entry covered by PAX metadata.
/// @details The real archive path is supplied by a preceding PAX `path` record.
///          This fallback is only for legacy readers that ignore PAX and must fit
///          in the basic USTAR name field.
/// @param index Monotonic index shared with the associated PAX header.
/// @return Archive path for the placeholder payload entry.
std::string paxPayloadFallbackPath(size_t index) {
    return "PaxPayloads.X/zanna-" + std::to_string(index);
}

/// @brief Split a normalized archive path between USTAR prefix and name fields.
/// @param path Normalized path to represent.
/// @param prefix Receives the optional directory prefix.
/// @param name Receives the final name component or short complete path.
/// @throws std::runtime_error If no valid 155-byte/100-byte split exists.
void splitUstarPath(const std::string &path, std::string &prefix, std::string &name);

/// @brief Test whether @p path can be represented in USTAR name/prefix fields.
/// @param path Normalized archive path to test.
/// @return true when the path requires a POSIX PAX `path` record.
bool pathNeedsPaxRecord(const std::string &path) {
    std::string prefix;
    std::string name;
    try {
        splitUstarPath(path, prefix, name);
        return false;
    } catch (const std::runtime_error &) {
        return true;
    }
}

/// @brief Compute the USTAR checksum for a 512-byte header.
/// The checksum field (offset 148, 8 bytes) is treated as spaces.
/// @param header Complete USTAR header bytes.
/// @return Sum of all header bytes under the POSIX checksum convention.
uint32_t computeChecksum(const uint8_t header[512]) {
    uint32_t sum = 0;
    for (int i = 0; i < 512; i++) {
        // Checksum field is at offset 148..155 — treat as spaces
        if (i >= 148 && i < 156)
            sum += ' ';
        else
            sum += header[i];
    }
    return sum;
}

/// @brief Split a normalized path into USTAR prefix/name fields.
/// The name field must be non-empty even for directory entries that end in '/'.
/// @param path Normalized path to represent.
/// @param prefix Receives at most 155 bytes preceding a slash.
/// @param name Receives at most 100 bytes following the split.
/// @throws std::runtime_error If @p path cannot be represented by USTAR fields.
void splitUstarPath(const std::string &path, std::string &prefix, std::string &name) {
    name = path;
    prefix.clear();
    if (name.size() <= 100)
        return;

    size_t searchFrom = name.size() - 1;
    if (name.back() == '/')
        --searchFrom;
    searchFrom = std::min<size_t>(searchFrom, 155);

    size_t splitAt = name.rfind('/', searchFrom);
    while (splitAt != std::string::npos) {
        if (splitAt > 0 && splitAt + 1 < name.size()) {
            const std::string candidatePrefix = name.substr(0, splitAt);
            const std::string candidateName = name.substr(splitAt + 1);
            if (!candidateName.empty() && candidateName.size() <= 100 &&
                candidatePrefix.size() <= 155) {
                prefix = candidatePrefix;
                name = candidateName;
                return;
            }
        }
        if (splitAt == 0)
            break;
        splitAt = name.rfind('/', splitAt - 1);
    }

    throw std::runtime_error("tar path too long: " + path);
}

} // namespace

/// @brief Serialize all accumulated entries into a USTAR tar byte stream.
/// For each entry: builds a 512-byte header (splitting long paths into
/// prefix+name fields), computes the POSIX checksum, and pads file data to
/// 512-byte blocks. Appends two zero-filled 1024-byte end-of-archive blocks.
/// @return Complete caller-owned archive bytes; pending entries remain unchanged.
/// @throws std::runtime_error If the output-size estimate or a header field
///         overflows its representation.
std::vector<uint8_t> TarWriter::finish() const {
    std::vector<uint8_t> out;

    // Estimate: per entry (512 header + data padded to 512) + 1024 end
    size_t est = 1024;
    for (const auto &e : entries_) {
        checkedAddEstimate(est, 512, "TarWriter");
        if (!e.data.empty()) {
            if (e.data.size() > std::numeric_limits<size_t>::max() - 511)
                throw std::runtime_error("TarWriter: archive size estimate overflow");
            checkedAddEstimate(est, ((e.data.size() + 511) / 512) * 512, "TarWriter");
        }
    }
    out.reserve(est);

    size_t paxIndex = 0;
    /// @brief Serialize one prepared archive entry in USTAR form.
    /// @param e Entry whose header and padded payload are appended to `out`.
    /// @throws std::runtime_error If the entry cannot be represented safely.
    auto appendEntry = [&](const Entry &e) {
        // Build 512-byte USTAR header
        uint8_t hdr[512];
        std::memset(hdr, 0, 512);

        std::string name = e.path;
        std::string prefix;
        splitUstarPath(e.path, prefix, name);

        // name[100]
        writeString(hdr + 0, 100, name);

        // mode[8]
        writeOctal(hdr + 100, 8, e.mode);

        // uid[8], gid[8] — both 0
        writeOctal(hdr + 108, 8, 0);
        writeOctal(hdr + 116, 8, 0);

        // size[12]
        writeOctal(hdr + 124, 12, (e.typeflag == '0' || e.typeflag == 'x') ? e.data.size() : 0);

        // mtime[12]
        writeOctal(hdr + 136, 12, e.mtime);

        // chksum[8] — filled after all other fields
        std::memset(hdr + 148, ' ', 8);

        // typeflag
        hdr[156] = static_cast<uint8_t>(e.typeflag);

        // linkname[100]
        if (e.typeflag == '2') {
            if (e.linkTarget.size() <= 100)
                writeString(hdr + 157, 100, e.linkTarget);
        }

        // magic[6] = "ustar\0"
        std::memcpy(hdr + 257, "ustar", 6);

        // version[2] = "00"
        hdr[263] = '0';
        hdr[264] = '0';

        // uname[32] = "root"
        writeString(hdr + 265, 32, "root");

        // gname[32] = "root"
        writeString(hdr + 297, 32, "root");

        // devmajor[8], devminor[8] — zero
        writeOctal(hdr + 329, 8, 0);
        writeOctal(hdr + 337, 8, 0);

        // prefix[155]
        if (!prefix.empty())
            writeString(hdr + 345, 155, prefix);

        // Compute and write checksum
        uint32_t cksum = computeChecksum(hdr);
        // Format: 6 octal digits + NUL + space
        char cksumStr[8];
        std::snprintf(cksumStr, sizeof(cksumStr), "%06o", cksum);
        std::memcpy(hdr + 148, cksumStr, 6);
        hdr[154] = '\0';
        hdr[155] = ' ';

        out.insert(out.end(), hdr, hdr + 512);

        // Write data blocks (padded to 512-byte boundary)
        if ((e.typeflag == '0' || e.typeflag == 'x') && !e.data.empty()) {
            out.insert(out.end(), e.data.begin(), e.data.end());
            // Pad to 512-byte boundary
            size_t remainder = e.data.size() % 512;
            if (remainder != 0) {
                size_t padBytes = 512 - remainder;
                out.resize(out.size() + padBytes, 0);
            }
        }
    };

    for (const auto &e : entries_) {
        std::string records;
        Entry emitted = e;
        if (pathNeedsPaxRecord(e.path)) {
            records += buildPaxRecord("path", e.path);
            emitted.path = paxPayloadFallbackPath(paxIndex);
        }
        if (e.typeflag == '2' && e.linkTarget.size() > 100)
            records += buildPaxRecord("linkpath", e.linkTarget);
        if (!records.empty()) {
            Entry pax;
            pax.path = paxHeaderPath(paxIndex++);
            pax.data.assign(records.begin(), records.end());
            pax.mode = 0644;
            pax.mtime = e.mtime;
            pax.typeflag = 'x';
            appendEntry(pax);
        }
        appendEntry(emitted);
    }

    // Two zero-filled 512-byte end-of-archive blocks
    out.resize(out.size() + 1024, 0);

    return out;
}

} // namespace zanna::pkg
