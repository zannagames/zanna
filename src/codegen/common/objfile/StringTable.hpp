//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/StringTable.hpp
// Purpose: Interned NUL-terminated string table for object file serialization.
//          Used for ELF .strtab/.shstrtab and COFF string tables.
// Key invariants:
//   - Offset 0 is always the empty string (single NUL byte)
//   - Strings are deduplicated via hash map
//   - PE/COFF 4-byte size prefix is NOT included; COFF writer prepends it
// Ownership/Lifetime:
//   - Value type, typically lives for the duration of object file writing
// Links: codegen/common/objfile/SymbolTable.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file StringTable.hpp
 * @brief Declares a deduplicating NUL-terminated object-file string table.
 */

#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::objfile {

/// @brief Serializable intern table of NUL-terminated strings.
/// @details Offset zero always names the empty string. Values are deduplicated
///          by exact bytes and constrained to 32-bit object-format offsets.
class StringTable {
  public:
    /// @brief Initialize the table with the empty string at offset zero.
    StringTable();

    /// @brief Intern a string and return its byte offset.
    /// @param str String bytes excluding the required NUL terminator.
    /// @return Existing or newly appended 32-bit table offset.
    /// @throws std::length_error when the table would exceed native or 32-bit limits.
    uint32_t add(std::string_view str);

    /// @brief Find an already interned string.
    /// @param str Exact string bytes to look up.
    /// @return Table offset, or `UINT32_MAX` when absent.
    uint32_t find(std::string_view str) const;

    /// @brief Access the raw NUL-separated table payload.
    /// @return Immutable serialized bytes.
    const std::vector<char> &data() const {
        return data_;
    }

    /// @brief Return the total serialized byte count.
    /// @return Table size including every terminator.
    /// @throws std::length_error if the backing vector exceeds 32-bit range.
    uint32_t size() const {
        if (data_.size() > std::numeric_limits<uint32_t>::max())
            throw std::length_error("StringTable size exceeds 32-bit object-file field range");
        return static_cast<uint32_t>(data_.size());
    }

  private:
    std::vector<char> data_;
    std::unordered_map<std::string, uint32_t> offsets_;
};

} // namespace zanna::codegen::objfile
