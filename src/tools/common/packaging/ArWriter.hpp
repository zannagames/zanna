//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/ArWriter.hpp
// Purpose: Write ar archives as used by the .deb package format.
//
// Key invariants:
//   - Global header: "!<arch>\n" (8 bytes).
//   - Per member: 60-byte header + data + optional pad byte for odd sizes.
//   - Member name is "/"-terminated, space-padded to 16 chars.
//
// Ownership/Lifetime:
//   - Output returned as std::vector<uint8_t> or written to file.
//
// Links: ArWriter.cpp, LinuxPackageBuilder.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares an in-memory writer for the Unix `ar` archive subset used by Debian packages.
/// @details Members are validated and copied into writer-owned storage, then
///          serialized with the standard global header, fixed-width ASCII member
///          headers, and even-byte padding. Serialization can return bytes or
///          atomically replace a destination file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief Writes ar archives (used as .deb outer container).
/// @details Supports short, directly encoded member names rather than GNU/BSD
///          extended-name tables. Member order is the order of successful
///          @ref addMember calls, and repeated serialization is deterministic.
/// @ownership Owns copied names and payload bytes until writer destruction.
class ArWriter {
  public:
    /// @brief Add a member to the archive.
    /// @param name Member name (max 15 chars, will be "/" terminated).
    /// @param data Member data.
    /// @param size Member data size.
    /// @param mtime Modification time (Unix timestamp), default 0.
    /// @param mode File type/permission bits formatted as octal, default 0100644.
    /// @details Rejects empty, duplicate, overlong, control-containing, whitespace,
    ///          or slash-containing names and null nonempty payloads.
    /// @throws std::runtime_error When validation or archive limits are violated.
    void addMember(const std::string &name,
                   const uint8_t *data,
                   size_t size,
                   uint32_t mtime = 0,
                   uint32_t mode = 0100644);

    /// @brief Convenience: add a member from a string.
    /// @param name Valid short archive member name.
    /// @param content Bytes copied from the string.
    /// @param mtime Unix modification timestamp.
    /// @param mode File type/permission bits formatted as octal.
    /// @details Delegates validation and ownership to @ref addMember.
    void addMemberString(const std::string &name,
                         const std::string &content,
                         uint32_t mtime = 0,
                         uint32_t mode = 0100644);

    /// @brief Convenience: add a member from a vector.
    /// @param name Valid short archive member name.
    /// @param data Payload bytes to copy.
    /// @param mtime Unix modification timestamp.
    /// @param mode File type/permission bits formatted as octal.
    /// @details Delegates validation and ownership to @ref addMember.
    void addMemberVec(const std::string &name,
                      const std::vector<uint8_t> &data,
                      uint32_t mtime = 0,
                      uint32_t mode = 0100644);

    /// @brief Finalize and return the complete ar archive.
    /// @return Newly allocated archive containing all members in insertion order.
    /// @throws std::runtime_error If total size or a fixed-width field overflows.
    /// @note The writer remains unchanged and may be serialized again.
    std::vector<uint8_t> finish() const;

    /// @brief Finalize and write to a file.
    /// @param path Destination path to replace atomically.
    /// @throws std::exception If serialization or atomic file writing fails.
    void finishToFile(const std::string &path) const;

  private:
    /// @brief One pending ar member captured until finish() serializes it.
    struct Member {
        std::string name;          ///< Member name (without the trailing '/').
        std::vector<uint8_t> data; ///< Member payload bytes.
        uint32_t mtime{0};         ///< Modification time (Unix timestamp).
        uint32_t mode{0};          ///< File mode in octal.
    };

    std::vector<Member> members_; ///< Pending members in archive order.
};

} // namespace zanna::pkg
