//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/ZipReader.hpp
// Purpose: Minimal ZIP archive reader for the packaging library. GC-free.
//          Used by the uninstaller, unit tests, and post-build verification.
//
// Key invariants:
//   - Read-only: does not modify the input buffer.
//   - Supports stored (method 0) and DEFLATE (method 8) entries.
//   - Parses the central directory to enumerate entries.
//
// Ownership/Lifetime:
//   - References external data buffer (caller must keep alive).
//
// Links: ZipWriter.hpp, PkgDeflate.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

/// @file
/// @brief Declares a non-owning, read-only classic ZIP parser and extractor.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief A single entry in a ZIP archive.
struct ZipEntry {
    std::string name;             ///< Entry path as stored in the central directory.
    uint32_t compressedSize{0};   ///< Compressed byte count in the local data record.
    uint32_t uncompressedSize{0}; ///< Original byte count after decompression.
    uint16_t method{0};           ///< Compression method: 0=stored, 8=deflate.
    uint16_t flags{0};            ///< General-purpose bit flags from the local header.
    uint32_t crc32{0};            ///< CRC-32 of the uncompressed data.
    uint32_t localHeaderOffset{
        0}; ///< Offset of the local file header from the start of the archive.
};

/// @brief Error thrown on invalid ZIP data.
class ZipReadError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// @brief Read-only ZIP archive backed by an in-memory buffer.
class ZipReader {
  public:
    /// @brief Open a ZIP from a memory buffer.
    /// @param data Pointer to ZIP data (caller-owned, must stay alive).
    /// @param len  Length of ZIP data.
    /// @throws ZipReadError if the buffer is not a valid ZIP.
    ZipReader(const uint8_t *data, size_t len);

    /// @brief List all entries in the archive.
    /// @return Immutable central-directory entries in archive order.
    const std::vector<ZipEntry> &entries() const {
        return entries_;
    }

    /// @brief Find an entry by name.
    /// @param name Exact case-sensitive entry name.
    /// @return Pointer to entry, or nullptr if not found.
    const ZipEntry *find(const std::string &name) const;

    /// @brief Extract a single entry to a byte vector.
    /// @param entry Descriptor obtained from this reader.
    /// @return Uncompressed caller-owned bytes.
    /// @throws ZipReadError on decompression failure or CRC mismatch.
    std::vector<uint8_t> extract(const ZipEntry &entry) const;

  private:
    const uint8_t *data_;           ///< Non-owning archive buffer.
    size_t len_;                    ///< Archive buffer length.
    std::vector<ZipEntry> entries_; ///< Validated central-directory inventory.

    /// @brief Scan the end-of-central-directory record to locate and parse
    ///        all central directory entries, populating entries_.
    /// @throws ZipReadError On unsupported features or inconsistent records.
    void parseCentralDirectory();
};

} // namespace zanna::pkg
