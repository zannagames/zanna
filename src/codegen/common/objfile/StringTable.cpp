//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/StringTable.cpp
// Purpose: Implementation of interned string table for object files.
// Key invariants:
//   - Offset 0 is always the empty string (single NUL byte)
//   - Deduplication via offsets_ map
// Ownership/Lifetime:
//   - See StringTable.hpp
// Links: codegen/common/objfile/StringTable.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file StringTable.cpp
 * @brief Implements bounded interning for object-file string tables.
 */

#include "codegen/common/objfile/StringTable.hpp"

#include <limits>
#include <stdexcept>

namespace zanna::codegen::objfile {

/// @copydoc StringTable::StringTable
StringTable::StringTable() {
    // ELF convention: offset 0 = empty string (single NUL byte).
    data_.push_back('\0');
    offsets_[""] = 0;
}

/// @copydoc StringTable::add
uint32_t StringTable::add(std::string_view str) {
    std::string key(str);
    auto it = offsets_.find(key);
    if (it != offsets_.end())
        return it->second;

    if (data_.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("StringTable offset exceeds 32-bit object-file field range");
    if (str.size() > std::numeric_limits<size_t>::max() - data_.size() - 1)
        throw std::length_error("StringTable size overflows addressable size");
    if (data_.size() + str.size() + 1 > std::numeric_limits<uint32_t>::max())
        throw std::length_error("StringTable size exceeds 32-bit object-file field range");

    auto offset = static_cast<uint32_t>(data_.size());
    data_.insert(data_.end(), str.begin(), str.end());
    data_.push_back('\0');
    offsets_[std::move(key)] = offset;
    return offset;
}

/// @copydoc StringTable::find
uint32_t StringTable::find(std::string_view str) const {
    auto it = offsets_.find(std::string(str));
    if (it != offsets_.end())
        return it->second;
    return UINT32_MAX;
}

} // namespace zanna::codegen::objfile
