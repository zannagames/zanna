//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/XarWriter.hpp
// Purpose: Minimal XAR writer for native macOS flat package generation.
//
// Key invariants:
//   - Output layout: 28-byte header, zlib-compressed XML TOC, 20-byte SHA-1 of
//     the compressed TOC, then the file-data heap.
//   - File paths are sanitized and de-duplicated on insertion.
//   - Directory structure in the TOC is reconstructed from flat entry paths.
//
// Ownership/Lifetime:
//   - Single-use accumulator: add entries, then finish()/finishToFile().
//
// Links: XarWriter.cpp, MacOSPackageBuilder.cpp, PkgVerify.cpp (reader side)
//
//===----------------------------------------------------------------------===//
#pragma once

/// @file
/// @brief Declares the in-memory XAR writer used for macOS flat packages.

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief Builds a minimal XAR archive (macOS flat-package container) in memory.
/// @details Entries are accumulated by the addXxx() methods and serialized by
///          finish(). The TOC is emitted as an XML file tree reconstructed from
///          the flat entry paths, and each file payload may be zlib-compressed.
class XarWriter {
  public:
    /// @brief Add (or no-op merge) a directory entry.
    /// @param path Archive-relative directory path.
    /// @param mode Permission bits (octal, masked to 0777).
    void addDirectory(const std::string &path, uint32_t mode = 0755);

    /// @brief Add a file entry from a raw byte buffer.
    /// @param name Archive-relative file path.
    /// @param data Pointer to the file bytes (may be null only when @p size is 0).
    /// @param size Number of bytes.
    /// @param compress When true, the payload is zlib-compressed in the heap.
    /// @param mode Permission bits (octal, masked to 0777).
    /// @throws std::runtime_error on an empty/duplicate path or null data.
    void addFile(const std::string &name,
                 const uint8_t *data,
                 size_t size,
                 bool compress = false,
                 uint32_t mode = 0644);

    /// @brief Convenience: add a file entry from a byte vector.
    /// @param name Archive-relative file path.
    /// @param data File bytes to copy.
    /// @param compress Whether to compress the heap payload.
    /// @param mode Permission bits.
    /// @throws std::runtime_error On an unsafe or duplicate path.
    void addFileVec(const std::string &name,
                    const std::vector<uint8_t> &data,
                    bool compress = false,
                    uint32_t mode = 0644);

    /// @brief Convenience: add a file entry from string content.
    /// @param name Archive-relative file path.
    /// @param content File bytes to copy without transcoding.
    /// @param compress Whether to compress the heap payload.
    /// @param mode Permission bits.
    /// @throws std::runtime_error On an unsafe or duplicate path.
    void addFileString(const std::string &name,
                       const std::string &content,
                       bool compress = false,
                       uint32_t mode = 0644);

    /// @brief Serialize all entries into a complete XAR archive.
    /// @return The archive bytes (header + compressed TOC + checksum + heap).
    /// @throws std::runtime_error On path conflicts, compression, or size failure.
    std::vector<uint8_t> finish() const;

    /// @brief Serialize the archive and write it to @p path.
    /// @param path Native destination path replaced atomically.
    /// @throws std::runtime_error on open or write failure.
    void finishToFile(const std::string &path) const;

  private:
    /// @brief Whether a pending entry is a directory or a file.
    enum class EntryKind { Directory, File };

    /// @brief One pending XAR entry captured until finish() serializes it.
    struct Entry {
        EntryKind kind{EntryKind::File}; ///< Directory or file.
        std::string path;                ///< Normalized archive-relative path.
        std::vector<uint8_t> data;       ///< File payload (file entries only).
        bool compress{false};            ///< Whether to zlib-compress the payload.
        uint32_t mode{0644};             ///< Permission bits (octal).
    };

    std::vector<Entry> entries_;      ///< Entries in insertion order.
    std::set<std::string> seenNames_; ///< Paths added so far (duplicate guard).
};

} // namespace zanna::pkg
