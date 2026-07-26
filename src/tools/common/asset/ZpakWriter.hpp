//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/asset/ZpakWriter.hpp
// Purpose: Writer for the ZPAK (Zanna Pack Archive) binary format. Serializes
//          named asset entries into a compact archive with optional per-entry
//          DEFLATE compression. Used for both embedded .rodata blobs and
//          standalone .zpak distribution files.
//
// Key invariants:
//   - Entry names are UTF-8 relative paths with forward slashes.
//   - Each entry's data is 8-byte aligned in the output.
//   - TOC is written at the end; header toc_offset is patched after TOC.
//   - Pre-compressed formats (.png, .jpg, .ogg, etc.) skip compression.
//   - Current output is ZPAK v2 and checksums every original entry payload.
//
// Ownership/Lifetime:
//   - Value type. Entries are copied into internal storage on addEntry().
//   - Output vectors/files are caller-owned after write.
//
// Links: rt_zpak_reader.h (runtime reader), AssetCompiler.hpp (build tool)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the deterministic writer for Zanna Pack Archive version 2.
/// @details Entries are validated and copied into writer-owned storage, then
///          serialized with aligned payloads, a trailing table of contents, and
///          CRC-32 checksums over original bytes. The same writer produces
///          embedded memory blobs and standalone `.zpak` files.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace zanna::asset {

/// @brief Writer for ZPAK (Zanna Pack Archive) binary format.
/// @details Usage:
///   ZpakWriter w;
///   w.addEntry("sprites/hero.png", data, size, /*compress=*/true);
///   auto blob = w.writeToMemory();
///
///          Entry insertion order determines data and TOC order. Serialization
///          is const and can be repeated without changing accumulated entries.
/// @invariant Entry names must not contain backslashes or be empty.
/// @invariant Entry data may be empty (zero-length assets are valid).
/// @ownership Owns entry names and stored payload bytes.
class ZpakWriter {
  public:
    /// @brief Add an asset entry to the archive.
    ///
    /// @param name Relative UTF-8 path using forward slashes.
    /// @param data Raw asset bytes; may be null only when @p size is zero.
    /// @param size Length of @p data in bytes.
    /// @param compress Whether to attempt DEFLATE compression.
    /// @details Compression is skipped for known pre-compressed extensions and
    ///          retained only when it reduces size. Duplicate or unsafe names
    ///          are rejected, and CRC-32 is computed over the original payload.
    /// @throws std::invalid_argument For an invalid name, duplicate, or null data.
    /// @throws std::length_error When name, entry count, or payload is unrepresentable.
    /// @throws std::runtime_error When requested DEFLATE compression fails.
    void addEntry(const std::string &name, const uint8_t *data, size_t size, bool compress);

    /// @brief Write the complete ZPAK archive to a memory buffer.
    /// @details Emits format version 2 with one CRC-32 of the original
    ///          uncompressed bytes on every TOC entry. Data and TOC offsets are
    ///          checked while the vector grows; the writer remains unchanged.
    /// @return ZPAK-format byte vector (header + data + TOC).
    /// @throws std::length_error When the accumulated archive is unrepresentable.
    /// @throws std::bad_alloc When output storage cannot be allocated.
    std::vector<uint8_t> writeToMemory() const;

    /// @brief Write the complete ZPAK archive to a file.
    /// @param path Output file path.
    /// @param err Receives an atomic file-write error; otherwise unchanged.
    /// @return True on success, false on caught file I/O errors.
    /// @throws std::exception If in-memory serialization fails before file writing.
    bool writeToFile(const std::string &path, std::string &err) const;

    /// @brief Number of entries added so far.
    /// @return Count of validated entries owned by the writer.
    size_t entryCount() const noexcept {
        return entries_.size();
    }

    /// @brief Check if any entries have been added.
    /// @return True when @ref entryCount is zero.
    bool empty() const noexcept {
        return entries_.empty();
    }

  private:
    /// @brief Check if a file extension should skip compression.
    /// @param name Entry path whose final extension should be classified.
    /// @return True for a known already-compressed format.
    static bool isPreCompressed(const std::string &name);

    /// @brief Writer-owned representation of one validated archive entry.
    struct Entry {
        std::string name;                ///< Relative entry name (forward slashes).
        std::vector<uint8_t> storedData; ///< Possibly compressed.
        uint64_t originalSize{0};        ///< Uncompressed size.
        uint32_t crc32{0};               ///< CRC-32 of the original uncompressed bytes.
        bool compressed{false};          ///< True if storedData is DEFLATE'd.
    };

    std::vector<Entry> entries_; ///< Entries retained in archive insertion order.
    std::unordered_set<std::string> seenNames_; ///< Names already added, for duplicate rejection.
};

} // namespace zanna::asset
