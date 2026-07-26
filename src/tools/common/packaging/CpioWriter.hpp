//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/CpioWriter.hpp
// Purpose: Write portable ASCII CPIO archives for native macOS package payloads.
//
// Key invariants:
//   - Uses the POSIX portable ASCII format (magic "070707", all-octal fields).
//   - Entry paths are normalized to "./"-prefixed relative paths; traversal and
//     duplicate paths are rejected at add time.
//   - finish() appends the "TRAILER!!!" sentinel and pads to a 512-byte boundary.
//
// Ownership/Lifetime:
//   - Single-use accumulator: add entries, then call finish() to serialize.
//
// Links: CpioWriter.cpp, MacOSPackageBuilder.cpp, PkgVerify.cpp (reader side)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares a validated portable-ASCII CPIO archive writer.
/// @details The writer owns normalized filesystem entries for macOS package
///          payloads and serializes the historical `070707` format with a
///          mandatory trailer and 512-byte final padding.

#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief Builds a portable ASCII CPIO archive in memory for macOS .pkg payloads.
/// @details Entries are accumulated by the addXxx() methods and serialized by
///          finish(). Paths are sanitized and de-duplicated on insertion so the
///          resulting archive cannot contain traversal or duplicate entries.
///          Repeated calls to finish are supported and do not consume entries.
/// @ownership Copies all paths, link targets, and file payload bytes.
class CpioWriter {
  public:
    /// @brief Add a directory entry (idempotent for an already-added path).
    /// @param path Archive-relative directory path.
    /// @param mode Permission bits (octal); the directory type bit is added.
    /// @param mtime Modification time (Unix timestamp).
    /// @details Normalizes the path and silently ignores a directory already
    ///          present under the same normalized archive name.
    /// @throws std::runtime_error When path sanitization rejects the input.
    void addDirectory(const std::string &path, uint32_t mode = 0755, uint32_t mtime = 0);

    /// @brief Add a regular file entry from a raw byte buffer.
    /// @param path Archive-relative file path (must not be the archive root).
    /// @param data Pointer to the file bytes (may be null only when @p size is 0).
    /// @param size Number of bytes.
    /// @param mode Permission bits (octal); the regular-file type bit is added.
    /// @param mtime Modification time (Unix timestamp).
    /// @throws std::runtime_error on a root path, duplicate path, or null data.
    /// @note On validation failure after name insertion, the normalized path may
    ///       remain reserved by this writer.
    void addFile(const std::string &path,
                 const uint8_t *data,
                 size_t size,
                 uint32_t mode = 0644,
                 uint32_t mtime = 0);

    /// @brief Convenience: add a regular file entry from a byte vector.
    /// @param path Archive-relative file path.
    /// @param data Payload bytes to copy.
    /// @param mode Permission bits; the regular-file type bit is added.
    /// @param mtime Unix modification timestamp.
    /// @details Delegates validation and copying to @ref addFile.
    void addFileVec(const std::string &path,
                    const std::vector<uint8_t> &data,
                    uint32_t mode = 0644,
                    uint32_t mtime = 0);

    /// @brief Convenience: add a regular file entry from string content.
    /// @param path Archive-relative file path.
    /// @param content String bytes to copy as the payload.
    /// @param mode Permission bits; the regular-file type bit is added.
    /// @param mtime Unix modification timestamp.
    /// @details Delegates validation and copying to @ref addFile.
    void addFileString(const std::string &path,
                       const std::string &content,
                       uint32_t mode = 0644,
                       uint32_t mtime = 0);

    /// @brief Add a symbolic-link entry whose target stays inside the archive.
    /// @param path Archive-relative link path.
    /// @param target Link target (validated as a safe relative path).
    /// @param mtime Modification time (Unix timestamp).
    /// @throws std::runtime_error on a root path, duplicate path, or unsafe target.
    /// @note On target-validation failure, the normalized link path may remain
    ///       reserved by this writer.
    void addSymlink(const std::string &path, const std::string &target, uint32_t mtime = 0);

    /// @brief Serialize all entries into a complete CPIO archive.
    /// @return The archive bytes, including the TRAILER!!! record and 512-byte
    ///         padding.
    /// @throws std::runtime_error When an entry exceeds an octal field limit.
    /// @note Does not modify or consume accumulated entries.
    std::vector<uint8_t> finish() const;

  private:
    /// @brief The kind of filesystem object an entry represents.
    /// @details Selects serialized type bits, link counts, and payload source.
    enum class EntryKind { Directory, File, Symlink };

    /// @brief One pending CPIO entry captured until finish() serializes it.
    struct Entry {
        EntryKind kind{EntryKind::File}; ///< Directory, file, or symlink.
        std::string path;                ///< Normalized "./"-prefixed path.
        std::string symlinkTarget;       ///< Link target (symlink entries only).
        std::vector<uint8_t> data;       ///< File payload (file entries only).
        uint32_t mode{0644};             ///< Mode bits including the type field.
        uint32_t mtime{0};               ///< Modification time (Unix timestamp).
    };

    std::vector<Entry> entries_;       ///< Entries in insertion order.
    std::set<std::string> seenPaths_;  ///< Paths added so far (duplicate guard).
};

} // namespace zanna::pkg
