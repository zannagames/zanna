//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/DebugLineTable.cpp
// Purpose: Implements DWARF v5 .debug_line encoding.
// Key invariants:
//   - Produces a complete, self-contained .debug_line section.
//   - Uses DWARF v5 format (version=5) with a single compilation unit.
//   - Line number program uses standard opcodes (DW_LNS_*) and special opcodes.
//   - Special opcodes encode (address_advance, line_advance) pairs compactly.
// Links: codegen/common/objfile/DebugLineTable.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file DebugLineTable.cpp
 * @brief Implements collection and DWARF v5 encoding of address/line mappings.
 *
 * The encoder produces one self-contained DWARF32 line-table unit with a v5
 * file table and a compact line-number program. File indices and addresses are
 * validated during collection, while unit/header lengths and target address
 * widths are checked during serialization.
 */

#include "codegen/common/objfile/DebugLineTable.hpp"

#include "codegen/common/linker/ExeWriterUtil.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace zanna::codegen {

using linker::encoding::writeLE16;
using linker::encoding::writeLE32;
using linker::encoding::writeSLEB128;
using linker::encoding::writeULEB128;

/// @copydoc DebugLineTable::addFile
uint32_t DebugLineTable::addFile(const std::string &path) {
    // Check if already registered.
    for (size_t i = 0; i < files_.size(); ++i) {
        if (files_[i] == path)
            return static_cast<uint32_t>(i + 1); // 1-based
    }
    if (files_.size() >= std::numeric_limits<uint32_t>::max())
        throw std::length_error("debug_line: file table exceeds 32-bit index range");
    files_.push_back(path);
    return static_cast<uint32_t>(files_.size()); // 1-based
}

/// @copydoc DebugLineTable::addFileSlot
uint32_t DebugLineTable::addFileSlot(const std::string &path) {
    if (files_.size() >= std::numeric_limits<uint32_t>::max())
        throw std::length_error("debug_line: file table exceeds 32-bit index range");
    files_.push_back(path);
    return static_cast<uint32_t>(files_.size()); // 1-based
}

/// @copydoc DebugLineTable::addEntry
void DebugLineTable::addEntry(uint64_t address,
                              uint32_t fileIndex,
                              uint32_t line,
                              uint32_t column) {
    if (fileIndex == 0 || fileIndex > files_.size()) {
        throw std::runtime_error("debug_line: invalid file index " + std::to_string(fileIndex) +
                                 " (table has " + std::to_string(files_.size()) + " file slot(s))");
    }
    if (line == 0)
        throw std::runtime_error("debug_line: line numbers are 1-based; line 0 is invalid");
    if (!entries_.empty() && address < entries_.back().address) {
        throw std::runtime_error("debug_line: address entries must be added in ascending order");
    }
    entries_.push_back({address, fileIndex, line, column});
}

/// @copydoc DebugLineTable::append
void DebugLineTable::append(const DebugLineTable &other, uint64_t addressBias) {
    std::vector<uint32_t> fileRemap(other.files_.size() + 1, 0);
    for (size_t i = 0; i < other.files_.size(); ++i)
        fileRemap[i + 1] = addFileSlot(other.files_[i]);

    for (const auto &entry : other.entries_) {
        if (entry.fileIndex == 0 || entry.fileIndex >= fileRemap.size()) {
            throw std::runtime_error("debug_line: append saw invalid file index " +
                                     std::to_string(entry.fileIndex) + " (source table has " +
                                     std::to_string(other.files_.size()) + " file slot(s))");
        }
        if (entry.address > std::numeric_limits<uint64_t>::max() - addressBias)
            throw std::length_error("debug_line: rebased address overflows 64-bit range");
        addEntry(entry.address + addressBias, fileRemap[entry.fileIndex], entry.line, entry.column);
    }
}

// --- DWARF v5 .debug_line constants ---

// Standard opcode numbers (DWARF v5 §6.2.5.2).
static constexpr uint8_t DW_LNS_copy = 1;
static constexpr uint8_t DW_LNS_advance_pc = 2;
static constexpr uint8_t DW_LNS_advance_line = 3;
static constexpr uint8_t DW_LNS_set_file = 4;
static constexpr uint8_t DW_LNS_set_column = 5;
// static constexpr uint8_t DW_LNS_negate_stmt = 6;
// static constexpr uint8_t DW_LNS_set_basic_block = 7;
static constexpr uint8_t DW_LNS_const_add_pc = 8;
// static constexpr uint8_t DW_LNS_fixed_advance_pc = 9;
// static constexpr uint8_t DW_LNS_set_prologue_end = 10;
// static constexpr uint8_t DW_LNS_set_epilogue_begin = 11;
// static constexpr uint8_t DW_LNS_set_isa = 12;

// Extended opcode numbers (DWARF v5 §6.2.5.3).
static constexpr uint8_t DW_LNE_end_sequence = 1;
static constexpr uint8_t DW_LNE_set_address = 2;
// static constexpr uint8_t DW_LNE_define_file = 3; // deprecated in v5

// Line number program parameters (DWARF v5 §6.2.4).
static constexpr int8_t kLineBase = -5;    // Minimum line increment for special opcode.
static constexpr uint8_t kLineRange = 14;  // Range of line increments for special opcode.
static constexpr uint8_t kOpcodeBase = 13; // First special opcode number.
// min_inst_length = 1, max_ops_per_inst = 1 (non-VLIW).

// Content type codes for DWARF v5 file name entry format (§6.2.4.1).
static constexpr uint8_t DW_LNCT_path = 1;
// Form codes.
static constexpr uint8_t DW_FORM_string = 8; // Inline NUL-terminated string.

// Helpers.
/// @brief Append one DWARF line-program extended opcode.
/// @param buf Destination line-table byte vector.
/// @param opcode Extended opcode following the zero escape and length.
/// @param data Opcode payload bytes, excluding the opcode itself.
static void writeExtendedOpcode(std::vector<uint8_t> &buf,
                                uint8_t opcode,
                                const std::vector<uint8_t> &data) {
    buf.push_back(0);                                          // Extended opcode escape.
    writeULEB128(buf, static_cast<uint64_t>(1 + data.size())); // length
    buf.push_back(opcode);
    buf.insert(buf.end(), data.begin(), data.end());
}

/// @brief Append a DWARF inline NUL-terminated string.
/// @param buf Destination byte vector.
/// @param s String bytes to append before the terminator.
static void writeNullTermString(std::vector<uint8_t> &buf, const std::string &s) {
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);
}

/// @copydoc DebugLineTable::encodeDwarf5
std::vector<uint8_t> DebugLineTable::encodeDwarf5(uint8_t addressSize) const {
    if (addressSize != 4 && addressSize != 8)
        throw std::invalid_argument("debug_line: address size must be 4 or 8 bytes");

    std::vector<uint8_t> out;

    // We build the header and program body separately, then fix up the unit_length field.
    // Structure: unit_length (4 bytes) | version (2) | address_size (1) | segment_selector_size (1)
    //            | header_length (4) | header_contents | line_number_program

    // --- Placeholder for unit_length (patched at end) ---
    const size_t unitLenOff = out.size();
    writeLE32(out, 0); // patched later

    // --- Version ---
    writeLE16(out, 5); // DWARF v5

    // --- Address size and segment selector size ---
    out.push_back(addressSize);
    out.push_back(0); // segment_selector_size

    // --- Placeholder for header_length (patched after header) ---
    const size_t headerLenOff = out.size();
    writeLE32(out, 0); // patched later

    const size_t headerStart = out.size();

    // --- Header fields ---
    out.push_back(1);                               // minimum_instruction_length
    out.push_back(1);                               // maximum_operations_per_instruction
    out.push_back(1);                               // default_is_stmt
    out.push_back(static_cast<uint8_t>(kLineBase)); // line_base (signed)
    out.push_back(kLineRange);                      // line_range
    out.push_back(kOpcodeBase);                     // opcode_base

    // standard_opcode_lengths (one per opcode, 1..opcode_base-1):
    // DW_LNS_copy(0), advance_pc(1), advance_line(1), set_file(1),
    // set_column(1), negate_stmt(0), set_basic_block(0), const_add_pc(0),
    // fixed_advance_pc(1), set_prologue_end(0), set_epilogue_begin(0), set_isa(1)
    static constexpr uint8_t kStdOpLengths[12] = {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
    out.insert(out.end(), kStdOpLengths, kStdOpLengths + 12);

    // --- Directory entry format (DWARF v5) ---
    // directory_entry_format_count = 0 (no directories — paths are absolute).
    out.push_back(0);
    // directories_count = 0.
    writeULEB128(out, 0);

    // --- File name entry format (DWARF v5) ---
    // file_name_entry_format_count = 1 (just DW_LNCT_path, DW_FORM_string).
    out.push_back(1);
    writeULEB128(out, DW_LNCT_path);
    writeULEB128(out, DW_FORM_string);

    // file_names_count.
    writeULEB128(out, static_cast<uint64_t>(files_.size()));
    for (const auto &f : files_)
        writeNullTermString(out, f);

    // --- Patch header_length ---
    const size_t headerEnd = out.size();
    if (headerEnd - headerStart > std::numeric_limits<uint32_t>::max())
        throw std::length_error("debug_line: header length exceeds DWARF32 range");
    const uint32_t headerLen = static_cast<uint32_t>(headerEnd - headerStart);
    out[headerLenOff + 0] = static_cast<uint8_t>(headerLen);
    out[headerLenOff + 1] = static_cast<uint8_t>(headerLen >> 8);
    out[headerLenOff + 2] = static_cast<uint8_t>(headerLen >> 16);
    out[headerLenOff + 3] = static_cast<uint8_t>(headerLen >> 24);

    // --- Line number program ---
    // State machine registers (DWARF v5 §6.2.2).
    uint64_t curAddr = 0;
    uint32_t curFile = 1;
    uint32_t curLine = 1;
    uint32_t curColumn = 0;
    bool firstEntry = true;

    for (const auto &entry : entries_) {
        if (addressSize == 4 && entry.address > std::numeric_limits<uint32_t>::max())
            throw std::length_error("debug_line: address exceeds 32-bit address size");

        // Set address for the first entry (or when address decreases, which shouldn't happen).
        if (firstEntry) {
            std::vector<uint8_t> addrData;
            if (addressSize == 8) {
                for (int i = 0; i < 8; ++i)
                    addrData.push_back(static_cast<uint8_t>(entry.address >> (i * 8)));
            } else {
                for (int i = 0; i < 4; ++i)
                    addrData.push_back(static_cast<uint8_t>(entry.address >> (i * 8)));
            }
            writeExtendedOpcode(out, DW_LNE_set_address, addrData);
            curAddr = entry.address;
            firstEntry = false;
        }

        // Advance address.
        const uint64_t addrDelta = entry.address - curAddr;
        const int64_t lineDelta = static_cast<int64_t>(entry.line) - static_cast<int64_t>(curLine);

        // Set file if changed.
        if (entry.fileIndex != curFile) {
            out.push_back(DW_LNS_set_file);
            writeULEB128(out, entry.fileIndex);
            curFile = entry.fileIndex;
        }

        // Set column if changed.
        if (entry.column != curColumn) {
            out.push_back(DW_LNS_set_column);
            writeULEB128(out, entry.column);
            curColumn = entry.column;
        }

        // Try special opcode encoding: opcode = (line_delta - line_base) + (line_range *
        // addr_delta) + opcode_base Valid when: line_delta in [line_base, line_base + line_range -
        // 1] and the opcode fits in [opcode_base, 255].
        if (lineDelta >= kLineBase && lineDelta < kLineBase + kLineRange) {
            const uint64_t specialOp = static_cast<uint64_t>(lineDelta - kLineBase) +
                                       (kLineRange * addrDelta) + kOpcodeBase;
            if (specialOp <= 255) {
                out.push_back(static_cast<uint8_t>(specialOp));
                curAddr = entry.address;
                curLine = entry.line;
                continue;
            }
        }

        // Fall back to standard opcodes.
        if (addrDelta > 0) {
            // Try const_add_pc first (advances by (255 - opcode_base) / line_range).
            const uint64_t constAddPcDelta = (255 - kOpcodeBase) / kLineRange;
            if (addrDelta == constAddPcDelta) {
                out.push_back(DW_LNS_const_add_pc);
            } else {
                out.push_back(DW_LNS_advance_pc);
                writeULEB128(out, addrDelta);
            }
        }

        if (lineDelta != 0) {
            out.push_back(DW_LNS_advance_line);
            writeSLEB128(out, lineDelta);
        }

        out.push_back(DW_LNS_copy);

        curAddr = entry.address;
        curLine = entry.line;
    }

    // End sequence.
    writeExtendedOpcode(out, DW_LNE_end_sequence, {});

    // --- Patch unit_length (total size minus the 4-byte length field itself) ---
    if (out.size() - 4 > std::numeric_limits<uint32_t>::max())
        throw std::length_error("debug_line: unit length exceeds DWARF32 range");
    const uint32_t unitLen = static_cast<uint32_t>(out.size() - 4);
    out[unitLenOff + 0] = static_cast<uint8_t>(unitLen);
    out[unitLenOff + 1] = static_cast<uint8_t>(unitLen >> 8);
    out[unitLenOff + 2] = static_cast<uint8_t>(unitLen >> 16);
    out[unitLenOff + 3] = static_cast<uint8_t>(unitLen >> 24);

    return out;
}

} // namespace zanna::codegen
