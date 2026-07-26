//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/TarWriter.hpp
// Purpose: Write USTAR tar archives for the .deb data/control tarballs.
//
// Key invariants:
//   - 512-byte headers with USTAR magic "ustar\0" and version "00".
//   - File data is padded to 512-byte boundaries.
//   - Archive ends with two zero-filled 512-byte blocks.
//   - Checksum: sum of all 512 header bytes, treating checksum field as spaces.
//
// Ownership/Lifetime:
//   - Output returned as std::vector<uint8_t>.
//
// Links: TarWriter.cpp
//
//===----------------------------------------------------------------------===//
#pragma once

/// @file
/// @brief Declares an in-memory USTAR writer with safe paths and PAX extensions.

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief Writes USTAR tar archives.
/// @details The writer copies each pending entry, rejects duplicate normalized
///          paths, and can serialize the same accumulated state more than once.
class TarWriter {
  public:
    /// @brief Add a regular file to the archive.
    /// @param path File path within the archive (e.g. "./usr/bin/hello").
    /// @param data File contents.
    /// @param size File content size.
    /// @param mode Unix file mode (default 0644).
    /// @param mtime Modification time (Unix timestamp, default 0).
    void addFile(const std::string &path,
                 const uint8_t *data,
                 size_t size,
                 uint32_t mode = 0644,
                 uint32_t mtime = 0);

    /// @brief Convenience: add a file from a string.
    /// @param path Archive-relative file path.
    /// @param content File bytes to copy.
    /// @param mode Unix permission bits.
    /// @param mtime Unix modification timestamp.
    /// @throws std::runtime_error If the path is invalid or duplicate.
    void addFileString(const std::string &path,
                       const std::string &content,
                       uint32_t mode = 0644,
                       uint32_t mtime = 0);

    /// @brief Convenience: add a file from a vector.
    /// @param path Archive-relative file path.
    /// @param data File bytes to copy.
    /// @param mode Unix permission bits.
    /// @param mtime Unix modification timestamp.
    /// @throws std::runtime_error If the path is invalid or duplicate.
    void addFileVec(const std::string &path,
                    const std::vector<uint8_t> &data,
                    uint32_t mode = 0644,
                    uint32_t mtime = 0);

    /// @brief Add a directory entry to the archive.
    /// @param path Directory path (e.g. "./usr/bin/"). Trailing "/" added if missing.
    /// @param mode Unix directory mode (default 0755).
    /// @param mtime Modification time.
    void addDirectory(const std::string &path, uint32_t mode = 0755, uint32_t mtime = 0);

    /// @brief Add a symbolic link entry.
    /// @param path Symlink path within the archive.
    /// @param target Symlink target.
    /// @param mtime Modification time.
    void addSymlink(const std::string &path, const std::string &target, uint32_t mtime = 0);

    /// @brief Finalize and return the complete tar archive.
    /// Appends two zero-filled 512-byte end-of-archive blocks.
    /// @return Complete caller-owned USTAR byte stream.
    /// @throws std::runtime_error If a size or header field overflows.
    std::vector<uint8_t> finish() const;

  private:
    /// @brief One pending tar entry captured until finish() serializes it.
    struct Entry {
        std::string path;          ///< Archive-relative entry path.
        std::string linkTarget;    ///< Link target (symlink entries only).
        std::vector<uint8_t> data; ///< File payload (regular files only).
        uint32_t mode{0};          ///< Unix permission bits.
        uint32_t mtime{0};         ///< Modification time (Unix timestamp).
        char typeflag{'\0'};       ///< '0'=file, '5'=dir, '2'=symlink.
    };

    std::vector<Entry> entries_;      ///< Entries in insertion order.
    std::set<std::string> seenPaths_; ///< Paths added so far (duplicate guard).
};

} // namespace zanna::pkg
