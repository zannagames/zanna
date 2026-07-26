//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/CpioWriter.cpp
// Purpose: Native portable ASCII CPIO writer for macOS flat package payloads.
//
// Key invariants:
//   - Emits the "070707" portable ASCII format with fixed-width octal fields.
//   - Entry paths are normalized and validated; symlink targets must not escape.
//   - finish() always emits a TRAILER!!! record and pads to 512 bytes.
//
// Ownership/Lifetime:
//   - Entries are copied in, owned by the writer, and remain after serialization.
//
// Links: CpioWriter.hpp, PkgUtils.hpp (path sanitization)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements safe path normalization and `070707` CPIO serialization.
/// @details Files, directories, and internal relative symlinks are converted to
///          fixed-width octal records in insertion order, followed by the
///          portable trailer record and block padding required by package tools.

#include "CpioWriter.hpp"

#include "PkgUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace zanna::pkg {
namespace {

/// @brief Normalize an entry path to a safe "./"-prefixed relative form.
/// @details Converts backslashes to slashes, strips trailing slashes and a
///          leading "./", and runs the path through sanitizePackageRelativePath
///          (which rejects traversal/absolute paths). The archive root maps to
///          ".".
/// @param path Raw entry path.
/// @param directory True if the path names a directory (affects the error label).
/// @return The normalized path, or "." for the archive root.
std::string normalizeCpioPath(std::string path, bool directory) {
    for (char &ch : path) {
        if (ch == '\\')
            ch = '/';
    }
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    if (path == "." || path == "./")
        return ".";
    if (path.rfind("./", 0) == 0)
        path.erase(0, 2);
    path = sanitizePackageRelativePath(path, directory ? "cpio directory path" : "cpio entry path");
    return path.empty() ? "." : "./" + path;
}

/// @brief Validate a symlink target and confirm it stays inside the archive.
/// @details Rejects empty, multi-line, absolute, and drive-qualified targets,
///          then resolves the target relative to the link's parent directory and
///          rejects it if it normalizes to "." / ".." or escapes upward.
/// @param linkPath Normalized path of the symlink entry (provides the base dir).
/// @param target Raw link target (taken by value; backslashes are normalized).
/// @return The validated (forward-slash) target string.
/// @throws std::runtime_error when the target is empty, absolute, or escapes.
std::string normalizeCpioSymlinkTarget(const std::string &linkPath, std::string target) {
    if (target.empty())
        throw std::runtime_error("cpio symlink target must not be empty");
    validateSingleLineField(target, "cpio symlink target");
    for (char &ch : target) {
        if (ch == '\\')
            ch = '/';
    }
    if (target.front() == '/' ||
        (target.size() >= 2 && std::isalpha(static_cast<unsigned char>(target[0])) &&
         target[1] == ':')) {
        throw std::runtime_error("cpio symlink target must be relative: " + target);
    }
    const std::filesystem::path resolved =
        (std::filesystem::path(linkPath).parent_path() / target).lexically_normal();
    const std::string text = resolved.generic_string();
    if (text.empty() || text == "." || text == ".." || text.rfind("../", 0) == 0)
        throw std::runtime_error("cpio symlink target escapes archive root: " + target);
    return target;
}

/// @brief Append a zero-padded, fixed-width octal header field.
/// @details Formats @p value as exactly @p width octal digits and appends them to
///          @p out, throwing if the value does not fit (no overflow indicator
///          exists in the format, so silent truncation must be avoided).
/// @param out Output buffer to append to.
/// @param value Value to encode.
/// @param width Exact field width in octal digits.
/// @param path Entry path used for error context.
/// @throws std::runtime_error When the requested width or value is unrepresentable.
void appendOctalField(std::vector<uint8_t> &out,
                      uint64_t value,
                      size_t width,
                      const std::string &path) {
    char field[16];
    if (width >= sizeof(field))
        throw std::runtime_error("cpio octal field width is too large");
    std::snprintf(field,
                  sizeof(field),
                  "%0*llo",
                  static_cast<int>(width),
                  static_cast<unsigned long long>(value));
    if (std::strlen(field) != width)
        throw std::runtime_error("cpio field value out of range for entry: " + path);
    out.insert(out.end(), field, field + width);
}

/// @brief Append one complete "070707" CPIO record (header + name + data).
/// @details Writes the 76-byte fixed octal header (with zeroed dev/ino/uid/gid/
///          rdev fields), the NUL-terminated name, and the payload bytes. Used
///          for files, directories, symlinks, and the TRAILER!!! sentinel.
/// @param out Output buffer to append to.
/// @param path Entry name written into the record (NUL-terminated).
/// @param mode Mode bits including the file-type field.
/// @param nlink Link count field (2 for directories, 1 otherwise).
/// @param mtime Modification time (Unix timestamp).
/// @param data Payload bytes (file contents or symlink target; may be null when empty).
/// @param dataSize Payload length in bytes.
/// @throws std::runtime_error when the entry or path exceeds the format limits.
void appendEntry(std::vector<uint8_t> &out,
                 const std::string &path,
                 uint32_t mode,
                 uint32_t nlink,
                 uint32_t mtime,
                 const uint8_t *data,
                 size_t dataSize) {
    if (dataSize > 077777777777ull)
        throw std::runtime_error("cpio entry too large: " + path);
    if (path.size() + 1 > 077777ull)
        throw std::runtime_error("cpio path too long: " + path);

    const char *magic = "070707";
    out.insert(out.end(), magic, magic + 6);
    appendOctalField(out, 0, 6, path); // dev
    appendOctalField(out, 0, 6, path); // ino
    appendOctalField(out, mode, 6, path);
    appendOctalField(out, 0, 6, path); // uid
    appendOctalField(out, 0, 6, path); // gid
    appendOctalField(out, nlink, 6, path);
    appendOctalField(out, 0, 6, path); // rdev
    appendOctalField(out, mtime, 11, path);
    appendOctalField(out, path.size() + 1, 6, path);
    appendOctalField(out, dataSize, 11, path);
    out.insert(out.end(), path.begin(), path.end());
    out.push_back(0);
    if (dataSize != 0) {
        if (data == nullptr)
            throw std::runtime_error("cpio file data pointer is null: " + path);
        out.insert(out.end(), data, data + dataSize);
    }
}

} // namespace

/// @brief Add a normalized directory record unless that path already exists.
/// @param path Archive-relative directory path; `"."` denotes the archive root.
/// @param mode Permission bits combined with the directory type field.
/// @param mtime Unix modification timestamp.
/// @details Duplicate directory paths are idempotent and leave the first record
///          unchanged. Successful insertion copies all metadata into the writer.
/// @throws std::runtime_error When path sanitization fails.
void CpioWriter::addDirectory(const std::string &path, uint32_t mode, uint32_t mtime) {
    const std::string clean = normalizeCpioPath(path, true);
    if (!seenPaths_.insert(clean).second)
        return;
    Entry entry;
    entry.kind = EntryKind::Directory;
    entry.path = clean;
    entry.mode = 0040000u | (mode & 07777u);
    entry.mtime = mtime;
    entries_.push_back(std::move(entry));
}

/// @brief Add a normalized regular-file record with copied payload bytes.
/// @param path Archive-relative path other than the root.
/// @param data Payload pointer, nullable only for zero @p size.
/// @param size Number of payload bytes to copy.
/// @param mode Permission bits combined with the regular-file type field.
/// @param mtime Unix modification timestamp.
/// @throws std::runtime_error For unsafe/root paths, duplicates, or null data.
void CpioWriter::addFile(
    const std::string &path, const uint8_t *data, size_t size, uint32_t mode, uint32_t mtime) {
    const std::string clean = normalizeCpioPath(path, false);
    if (clean == ".")
        throw std::runtime_error("cpio file path must not be archive root");
    if (!seenPaths_.insert(clean).second)
        throw std::runtime_error("duplicate cpio entry path: " + clean);
    Entry entry;
    entry.kind = EntryKind::File;
    entry.path = clean;
    if (size != 0) {
        if (data == nullptr)
            throw std::runtime_error("cpio file data pointer is null: " + clean);
        entry.data.assign(data, data + size);
    }
    entry.mode = 0100000u | (mode & 07777u);
    entry.mtime = mtime;
    entries_.push_back(std::move(entry));
}

/// @brief Add a regular file from a byte vector.
/// @param path Archive-relative file path.
/// @param data Payload vector copied into writer storage.
/// @param mode File permission bits.
/// @param mtime Unix modification timestamp.
void CpioWriter::addFileVec(const std::string &path,
                            const std::vector<uint8_t> &data,
                            uint32_t mode,
                            uint32_t mtime) {
    addFile(path, data.data(), data.size(), mode, mtime);
}

/// @brief Add a regular file from string bytes.
/// @param path Archive-relative file path.
/// @param content Payload string copied without a terminator.
/// @param mode File permission bits.
/// @param mtime Unix modification timestamp.
void CpioWriter::addFileString(const std::string &path,
                               const std::string &content,
                               uint32_t mode,
                               uint32_t mtime) {
    addFile(path, reinterpret_cast<const uint8_t *>(content.data()), content.size(), mode, mtime);
}

/// @brief Add a symbolic link whose resolution remains inside the archive root.
/// @param path Archive-relative link path other than the root.
/// @param target Relative link target stored as the record payload.
/// @param mtime Unix modification timestamp.
/// @details Normalizes backslashes in the target and fixes link permissions at
///          `0777`, combined with the symlink type bits.
/// @throws std::runtime_error For unsafe/root paths, duplicates, or escaping targets.
void CpioWriter::addSymlink(const std::string &path, const std::string &target, uint32_t mtime) {
    const std::string clean = normalizeCpioPath(path, false);
    if (clean == ".")
        throw std::runtime_error("cpio symlink path must not be archive root");
    if (!seenPaths_.insert(clean).second)
        throw std::runtime_error("duplicate cpio entry path: " + clean);
    Entry entry;
    entry.kind = EntryKind::Symlink;
    entry.path = clean;
    entry.symlinkTarget = normalizeCpioSymlinkTarget(clean, target);
    entry.mode = 0120000u | 0777u;
    entry.mtime = mtime;
    entries_.push_back(std::move(entry));
}

/// @brief Serialize accumulated entries as a complete portable-ASCII CPIO archive.
/// @return Archive bytes ending in `TRAILER!!!` and zero-padded to 512 bytes.
/// @details Emits records in insertion order and leaves writer state unchanged,
///          allowing deterministic repeated serialization.
/// @throws std::runtime_error When an entry cannot fit a fixed-width format field.
std::vector<uint8_t> CpioWriter::finish() const {
    std::vector<uint8_t> out;
    out.reserve(entries_.size() * 128u);
    for (const Entry &entry : entries_) {
        if (entry.kind == EntryKind::Symlink) {
            appendEntry(out,
                        entry.path,
                        entry.mode,
                        1,
                        entry.mtime,
                        reinterpret_cast<const uint8_t *>(entry.symlinkTarget.data()),
                        entry.symlinkTarget.size());
        } else {
            appendEntry(out,
                        entry.path,
                        entry.mode,
                        entry.kind == EntryKind::Directory ? 2u : 1u,
                        entry.mtime,
                        entry.data.data(),
                        entry.data.size());
        }
    }
    appendEntry(out, "TRAILER!!!", 0, 1, 0, nullptr, 0);
    while ((out.size() % 512u) != 0)
        out.push_back(0);
    return out;
}

} // namespace zanna::pkg
