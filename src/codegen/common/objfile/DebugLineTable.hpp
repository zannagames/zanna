//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/DebugLineTable.hpp
// Purpose: Collects address→(file,line) mappings and encodes them as a
//          DWARF v5 .debug_line section.
// Key invariants:
//   - Entries must be added in ascending address order.
//   - File indices are 1-based (matching SourceLoc::file_id).
//   - encodeDwarf5() produces a self-contained .debug_line section.
// Links: support/source_location.hpp, codegen/common/linker/ExeWriterUtil.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file DebugLineTable.hpp
 * @brief Declares a validated DWARF v5 address-to-source line-table builder.
 *
 * Callers register source-file slots, append monotonically increasing code
 * locations, and serialize the result as a complete `.debug_line` section for
 * an ELF or COFF object.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zanna::codegen {

/// @brief One row of source-location state at a generated code address.
struct AddressLineEntry {
    uint64_t address;   ///< Code offset (relative to .text start).
    uint32_t fileIndex; ///< 1-based file index.
    uint32_t line;      ///< 1-based line number.
    uint32_t column;    ///< 1-based column number (0 = unknown).
};

/// @brief Collects address/line rows and encodes a DWARF v5 line-table unit.
/// @details File indices are one-based and entry addresses must be appended in
///          nondecreasing order, matching the DWARF line-program state machine.
class DebugLineTable {
  public:
    /// @brief Register or reuse a source path.
    /// @param path Source file path stored in the DWARF v5 file table.
    /// @return One-based index of the existing or newly appended file.
    /// @throws std::length_error if the file index would exceed 32 bits.
    uint32_t addFile(const std::string &path);

    /// @brief Append a distinct logical file slot even when its path is duplicated.
    /// @details Used when stable one-based slots must mirror `SourceLoc::file_id`.
    /// @param path Source file path to append.
    /// @return One-based index of the new slot.
    /// @throws std::length_error if the file index would exceed 32 bits.
    uint32_t addFileSlot(const std::string &path);

    /// @brief Append one address-to-source mapping.
    /// @param address Code offset relative to the object text start.
    /// @param fileIndex Valid one-based index returned by a file registration method.
    /// @param line One-based source line number.
    /// @param column One-based column, or zero when unknown.
    /// @throws std::runtime_error for invalid indices, line zero, or decreasing addresses.
    void addEntry(uint64_t address, uint32_t fileIndex, uint32_t line, uint32_t column = 0);

    /// @brief Append another line table while rebasing addresses and file slots.
    /// @param other Source table whose file slots are copied without deduplication.
    /// @param addressBias Value added to every source code address.
    /// @throws std::runtime_error for malformed source file indices.
    /// @throws std::length_error if a rebased address overflows.
    void append(const DebugLineTable &other, uint64_t addressBias = 0);

    /// @brief Test whether no address rows have been recorded.
    /// @return `true` when the entry sequence is empty.
    [[nodiscard]] bool empty() const {
        return entries_.empty();
    }

    /// @brief Encode a complete DWARF v5 `.debug_line` section.
    /// @details Emits a DWARF32 unit, inline path strings, and compact special
    ///          opcodes where address/line deltas fit, falling back to standard
    ///          opcodes otherwise.
    /// @param addressSize Target address width, either four or eight bytes.
    /// @return Serialized line-table bytes including the end-sequence opcode.
    /// @throws std::invalid_argument for unsupported address widths.
    /// @throws std::length_error for addresses or DWARF32 lengths out of range.
    [[nodiscard]] std::vector<uint8_t> encodeDwarf5(uint8_t addressSize = 8) const;

  private:
    std::vector<std::string> files_;        ///< Registered file paths.
    std::vector<AddressLineEntry> entries_; ///< Address→line mappings.
};

} // namespace zanna::codegen
