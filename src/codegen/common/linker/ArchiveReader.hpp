//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/ArchiveReader.hpp
// Purpose: Parse Unix ar archives (.a files) containing object files.
//          Supports GNU, BSD, and COFF archive variants.
// Key invariants:
//   - Archives start with "!<arch>\n" magic (8 bytes)
//   - Member headers are 60 bytes, data padded to 2-byte boundary
//   - Symbol table format varies: GNU ("/"), BSD ("__.SYMDEF")
// Ownership/Lifetime:
//   - ArchiveReader owns parsed member metadata; raw bytes are memory-mapped
//     or read from the file on demand
// Links: codegen/common/linker/ObjFileReader.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ArchiveReader.hpp
 * @brief Declares the validated GNU, BSD/Darwin, and COFF archive reader.
 *
 * Parsed archives own their complete byte buffer.  Member metadata and views
 * therefore remain valid until the owning Archive is modified or destroyed.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Describes the payload range of one ordinary archive member.
struct ArchiveMember {
    std::string name;      ///< Member name (e.g., "foo.o").
    size_t dataOffset = 0; ///< Byte offset of member data within the archive file.
    size_t dataSize = 0;   ///< Size of the member data in bytes.
};

/// @brief Owns a parsed archive, its ordinary members, and linker symbol indices.
/// @details `symbolIndex` records the first candidate for compatibility, while
///          `symbolCandidates` retains every distinct indexed definition in
///          archive order for extraction algorithms that must try alternatives.
struct Archive {
    std::string path;                                    ///< Path to the archive file.
    std::vector<uint8_t> data;                           ///< Raw archive file contents.
    std::vector<ArchiveMember> members;                  ///< All object file members.
    std::unordered_map<std::string, size_t> symbolIndex; ///< Symbol name → member index.
    std::unordered_map<std::string, std::vector<size_t>>
        symbolCandidates; ///< Symbol name → all indexed member candidates, in archive order.
};

/// @brief Non-owning view over an archive member's bytes.
/// @warning The view is invalidated if the owning Archive's `data` buffer is
///          modified or destroyed.
struct ArchiveMemberView {
    const uint8_t *data{nullptr};
    size_t size{0};
};

/// @brief Parses an archive file (`.a` or `.lib`) into owned metadata and bytes.
/// @details Validates archive magic, headers, payload bounds, padding, long-name
///          encodings, and symbol-index member offsets.  @p ar is reset before
///          the file is opened and may contain partial data after failure.
/// @param path UTF-8 path to the archive.
/// @param ar Destination archive structure.
/// @param err Stream that receives a diagnostic on hard parse or I/O failure.
/// @return `true` on success; `false` after reporting the first hard error.
bool readArchive(const std::string &path, Archive &ar, std::ostream &err);

/// @brief Copies the raw bytes of a specific archive member.
/// @param ar Parsed archive that owns the payload.
/// @param member Member range to copy.
/// @return A byte vector containing the member, or an empty vector when the
///         requested range is outside @p ar.  A valid zero-length member also
///         produces an empty vector.
[[maybe_unused]] std::vector<uint8_t> extractMember(const Archive &ar,
                                                    const ArchiveMember &member);

/// @brief Returns a non-copying view of a specific member's raw bytes.
/// @param ar Parsed archive that owns the payload.
/// @param member Member range to view.
/// @return Pointer/size view into @p ar, or a null, zero-sized view when the
///         requested range is invalid.
/// @warning The returned pointer follows the lifetime of `ar.data`.
ArchiveMemberView memberDataView(const Archive &ar, const ArchiveMember &member);

} // namespace zanna::codegen::linker
