//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/CoffWriter.cpp
// Purpose: Serialize CodeSection data into a valid COFF relocatable object file
//          for the Windows platform (x86_64 and AArch64).
// Key invariants:
//   - All multi-byte fields are little-endian
//   - File layout: COFF header (20B) | section headers (40B each) |
//     .text data | .text relocs | generated unwind sections | symbol table |
//     string table
//   - Symbol names <= 8 chars are stored inline; longer names use string table
//   - String table starts with a 4-byte size field (includes itself)
//   - COFF relocations are 10 bytes: offset(4) + symIdx(4) + type(2)
//   - No explicit addend field; addends are embedded in instruction bytes
// Ownership/Lifetime:
//   - Stateless between write() calls
// Links: codegen/common/objfile/CoffWriter.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file CoffWriter.cpp
 * @brief Implements little-endian AMD64 and ARM64 COFF object serialization.
 *
 * The writer lays out code, read-only data, initialized data, generated unwind
 * sections, relocation tables, symbols, and the COFF string table. It embeds
 * relocation addends into instruction/data fields, supports relocation-overflow
 * records, and validates every offset that must fit a 32-bit COFF field before
 * writing the object.
 */

#include "codegen/common/objfile/CoffWriter.hpp"
#include "codegen/common/objfile/ObjFileWriterUtil.hpp"
#include "codegen/common/objfile/RelocationValidation.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::objfile {

// =============================================================================
// COFF Constants
// =============================================================================

static constexpr uint16_t kMachineAMD64 = 0x8664;
static constexpr uint16_t kMachineARM64 = 0xAA64;

static constexpr uint16_t kCoffHeaderSize = 20;
static constexpr uint16_t kSectionHeaderSize = 40;

static constexpr uint32_t kImageScnCntCode = 0x00000020;
static constexpr uint32_t kImageScnCntInitData = 0x00000040;
static constexpr uint32_t kImageScnAlignText = 0x00600000;
static constexpr uint32_t kImageScnAlign4 = 0x00300000;
static constexpr uint32_t kImageScnAlign8 = 0x00400000;
static constexpr uint32_t kImageScnLnkNrelocOvfl = 0x01000000;
static constexpr uint32_t kImageScnMemExecute = 0x20000000;
static constexpr uint32_t kImageScnMemDiscardable = 0x02000000;
static constexpr uint32_t kImageScnMemRead = 0x40000000;
static constexpr uint32_t kImageScnMemWrite = 0x80000000;
static constexpr uint32_t kImageScnAlign1 = 0x00100000;

static constexpr uint8_t kImageSymClassExternal = 2;
static constexpr uint8_t kImageSymClassStatic = 3;

static constexpr int16_t kImageSymUndefined = 0;

static constexpr uint32_t kCoffRelocSize = 10;
static constexpr uint32_t kCoffMaxStandardRelocs = 0xFFFFu;

static constexpr uint16_t kImageRelAMD64_Addr64 = 1;
static constexpr uint16_t kImageRelAMD64_Addr32Nb = 3;
static constexpr uint16_t kImageRelAMD64_Rel32 = 4;

static constexpr uint16_t kImageRelARM64_Addr64 = 14;
static constexpr uint16_t kImageRelARM64_Addr32Nb = 2;
static constexpr uint16_t kImageRelARM64_Branch26 = 3;
static constexpr uint16_t kImageRelARM64_PagebaseRel21 = 4;
static constexpr uint16_t kImageRelARM64_Pageoffset12A = 6;
static constexpr uint16_t kImageRelARM64_Pageoffset12L = 7;
static constexpr uint16_t kImageRelARM64_Branch19 = 15;

/// @brief Map an architecture-neutral relocation kind to a COFF relocation type.
/// @param kind Relocation semantics recorded by the encoder.
/// @param arch Target object architecture.
/// @return COFF relocation number, or zero when @p kind is unsupported.
static uint16_t coffRelocType(RelocKind kind, ObjArch arch) {
    switch (kind) {
        case RelocKind::PCRel32:
        case RelocKind::Branch32:
            return kImageRelAMD64_Rel32;
        case RelocKind::Abs64:
            return arch == ObjArch::AArch64 ? kImageRelARM64_Addr64 : kImageRelAMD64_Addr64;
        case RelocKind::A64Call26:
        case RelocKind::A64Jump26:
            return kImageRelARM64_Branch26;
        case RelocKind::A64AdrpPage21:
            return kImageRelARM64_PagebaseRel21;
        case RelocKind::A64AddPageOff12:
            return kImageRelARM64_Pageoffset12A;
        case RelocKind::A64LdSt32Off12:
        case RelocKind::A64LdSt64Off12:
        case RelocKind::A64LdSt128Off12:
            return kImageRelARM64_Pageoffset12L;
        case RelocKind::A64CondBr19:
            return kImageRelARM64_Branch19;
    }
    return 0;
}

/// @brief Overwrite four bytes in an existing vector in little-endian order.
/// @param bytes Destination vector containing a valid four-byte range.
/// @param off Index of the first destination byte.
/// @param value Value to encode.
static void writeLE32At(std::vector<uint8_t> &bytes, size_t off, uint32_t value) {
    bytes[off] = static_cast<uint8_t>(value);
    bytes[off + 1] = static_cast<uint8_t>(value >> 8);
    bytes[off + 2] = static_cast<uint8_t>(value >> 16);
    bytes[off + 3] = static_cast<uint8_t>(value >> 24);
}

/// @brief Overwrite eight bytes in an existing vector in little-endian order.
/// @param bytes Destination vector containing a valid eight-byte range.
/// @param off Index of the first destination byte.
/// @param value Value to encode.
static void writeLE64At(std::vector<uint8_t> &bytes, size_t off, uint64_t value) {
    for (size_t i = 0; i < 8; ++i)
        bytes[off + i] = static_cast<uint8_t>(value >> (i * 8));
}

/// @brief Read four existing vector bytes as a little-endian unsigned value.
/// @param bytes Source vector containing a valid four-byte range.
/// @param off Index of the first source byte.
/// @return Decoded 32-bit value.
static uint32_t readLE32At(const std::vector<uint8_t> &bytes, size_t off) {
    return static_cast<uint32_t>(bytes[off]) | (static_cast<uint32_t>(bytes[off + 1]) << 8) |
           (static_cast<uint32_t>(bytes[off + 2]) << 16) |
           (static_cast<uint32_t>(bytes[off + 3]) << 24);
}

/// @brief Narrow a relocation addend to a signed 32-bit COFF field.
/// @param value Value to validate.
/// @param what Human-readable relocation description.
/// @param err Stream that receives an out-of-range diagnostic.
/// @param out Receives the narrowed value on success.
/// @return `true` when @p value is representable by `int32_t`.
static bool checkedI32(int64_t value, const char *what, std::ostream &err, int32_t &out) {
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        err << "CoffWriter: " << what << " addend " << value << " is outside signed 32-bit range\n";
        return false;
    }
    out = static_cast<int32_t>(value);
    return true;
}

/// @brief Validate and scale an AArch64 branch addend by instruction width.
/// @param value Byte addend to encode.
/// @param bits Width of the signed immediate field after division by four.
/// @param what Human-readable branch kind.
/// @param err Stream that receives alignment or range diagnostics.
/// @param scaled Receives @p value divided by four on success.
/// @return `true` when the addend is instruction aligned and field-representable.
static bool checkedA64BranchAddend(
    int64_t value, unsigned bits, const char *what, std::ostream &err, int64_t &scaled) {
    if ((value & 0x3) != 0) {
        err << "CoffWriter: " << what << " addend " << value << " is not instruction aligned\n";
        return false;
    }
    scaled = value >> 2;
    const int64_t minVal = -(int64_t{1} << (bits - 1));
    const int64_t maxVal = (int64_t{1} << (bits - 1)) - 1;
    if (scaled < minVal || scaled > maxVal) {
        err << "CoffWriter: " << what << " addend " << value << " is out of range\n";
        return false;
    }
    return true;
}

/// @brief Embed a COFF relocation's effective addend in copied section bytes.
/// @details COFF relocation records have no explicit addend. This helper writes
///          x86 displacements, absolute values, or AArch64 immediate fields into
///          a mutable copy while preserving opcode bits.
/// @param section Original section supplying logical bias and range validation.
/// @param rel Relocation describing the fixup encoding and source offset.
/// @param effectiveAddend Addend after section-offset contributions are applied.
/// @param bytes Mutable physical payload receiving the encoded addend.
/// @param err Stream that receives malformed range, alignment, or width diagnostics.
/// @return `true` when the addend was encoded successfully.
static bool patchCoffRelocationAddend(const CodeSection &section,
                                      const Relocation &rel,
                                      int64_t effectiveAddend,
                                      std::vector<uint8_t> &bytes,
                                      std::ostream &err) {
    if (rel.offset < section.logicalOffsetBias()) {
        err << "CoffWriter: relocation offset is before logical section bias\n";
        return false;
    }
    const size_t physicalOffset = rel.offset - section.logicalOffsetBias();

    switch (rel.kind) {
        case RelocKind::PCRel32:
        case RelocKind::Branch32: {
            int32_t encoded = 0;
            if (effectiveAddend > std::numeric_limits<int64_t>::max() - 4 ||
                effectiveAddend < std::numeric_limits<int64_t>::min() + 4 ||
                !checkedI32(effectiveAddend + 4, "x86_64 rel32", err, encoded))
                return false;
            writeLE32At(bytes, physicalOffset, static_cast<uint32_t>(encoded));
            return true;
        }

        case RelocKind::Abs64: {
            if (effectiveAddend == 0)
                return true;
            if (!section.containsOffsetRange(rel.offset, 8)) {
                err << "CoffWriter: 64-bit addend relocation at offset " << rel.offset
                    << " is out of bounds\n";
                return false;
            }
            writeLE64At(bytes, physicalOffset, static_cast<uint64_t>(effectiveAddend));
            return true;
        }

        case RelocKind::A64Call26:
        case RelocKind::A64Jump26: {
            int64_t scaled = 0;
            if (!checkedA64BranchAddend(effectiveAddend, 26, "AArch64 branch26", err, scaled))
                return false;
            uint32_t insn = readLE32At(bytes, physicalOffset);
            insn = (insn & 0xFC000000u) | (static_cast<uint32_t>(scaled) & 0x03FFFFFFu);
            writeLE32At(bytes, physicalOffset, insn);
            return true;
        }

        case RelocKind::A64AdrpPage21: {
            const int64_t pageAddend = effectiveAddend >> 12;
            if (pageAddend < -(int64_t{1} << 20) || pageAddend > ((int64_t{1} << 20) - 1)) {
                err << "CoffWriter: AArch64 ADRP addend " << effectiveAddend
                    << " is out of range\n";
                return false;
            }
            uint32_t insn = readLE32At(bytes, physicalOffset);
            const uint32_t imm = static_cast<uint32_t>(pageAddend);
            const uint32_t immlo = imm & 0x3u;
            const uint32_t immhi = (imm >> 2) & 0x7FFFFu;
            insn = (insn & 0x9F00001Fu) | (immlo << 29) | (immhi << 5);
            writeLE32At(bytes, physicalOffset, insn);
            return true;
        }

        case RelocKind::A64AddPageOff12: {
            const uint32_t pageOff = static_cast<uint32_t>(effectiveAddend) & 0xFFFu;
            uint32_t insn = readLE32At(bytes, physicalOffset);
            insn = (insn & 0xFFC003FFu) | (pageOff << 10);
            writeLE32At(bytes, physicalOffset, insn);
            return true;
        }

        case RelocKind::A64LdSt32Off12:
        case RelocKind::A64LdSt64Off12:
        case RelocKind::A64LdSt128Off12: {
            const uint32_t shift = rel.kind == RelocKind::A64LdSt32Off12
                                       ? 2u
                                       : (rel.kind == RelocKind::A64LdSt64Off12 ? 3u : 4u);
            const uint32_t pageOff = static_cast<uint32_t>(effectiveAddend) & 0xFFFu;
            const uint32_t scale = 1u << shift;
            if ((pageOff & (scale - 1u)) != 0) {
                err << "CoffWriter: AArch64 load/store addend " << effectiveAddend
                    << " has a misaligned page offset\n";
                return false;
            }
            uint32_t insn = readLE32At(bytes, physicalOffset);
            insn = (insn & 0xFFC003FFu) | ((pageOff >> shift) << 10);
            writeLE32At(bytes, physicalOffset, insn);
            return true;
        }

        case RelocKind::A64CondBr19: {
            int64_t scaled = 0;
            if (!checkedA64BranchAddend(effectiveAddend, 19, "AArch64 branch19", err, scaled))
                return false;
            uint32_t insn = readLE32At(bytes, physicalOffset);
            insn = (insn & 0xFF00001Fu) | ((static_cast<uint32_t>(scaled) & 0x7FFFFu) << 5);
            writeLE32At(bytes, physicalOffset, insn);
            return true;
        }
    }

    return true;
}

/// @brief Append a fixed-width, zero-padded COFF name field.
/// @details COFF section headers carry exactly eight raw name bytes. This
///          helper copies at most @p width bytes from @p name and pads the
///          remainder with NUL bytes without indexing past the end of short
///          string literals such as ".text".
/// @param out Destination byte buffer receiving the fixed-size field.
/// @param name Section name bytes to copy.
/// @param width Required field width in bytes.
static void appendFixedNameField(std::vector<uint8_t> &out, std::string_view name, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        const uint8_t byte = i < name.size() ? static_cast<uint8_t>(name[i]) : 0;
        out.push_back(byte);
    }
}

/// @brief Append one 40-byte COFF section header.
/// @param out Destination object buffer.
/// @param name Inline or slash-offset section-name field.
/// @param virtualSize COFF virtual-size field, normally zero in objects.
/// @param virtualAddr COFF virtual-address field, normally zero in objects.
/// @param rawDataSize File-backed section byte count.
/// @param rawDataPtr File offset of raw section data.
/// @param relocPtr File offset of the section relocation table.
/// @param numRelocs Header relocation count, including overflow sentinel semantics.
/// @param characteristics Section contents, alignment, and memory flags.
static void writeSectionHeader(std::vector<uint8_t> &out,
                               std::string_view name,
                               uint32_t virtualSize,
                               uint32_t virtualAddr,
                               uint32_t rawDataSize,
                               uint32_t rawDataPtr,
                               uint32_t relocPtr,
                               uint32_t numRelocs,
                               uint32_t characteristics) {
    appendFixedNameField(out, name, 8);

    appendLE32(out, virtualSize);
    appendLE32(out, virtualAddr);
    appendLE32(out, rawDataSize);
    appendLE32(out, rawDataPtr);
    appendLE32(out, relocPtr);
    appendLE32(out, 0);
    appendLE16(out, static_cast<uint16_t>(numRelocs));
    appendLE16(out, 0);
    appendLE32(out, characteristics);
}

/// @brief Append one 18-byte COFF symbol-table record.
/// @param out Destination symbol-table buffer.
/// @param name Symbol name.
/// @param strTabOffset String-table offset used when @p name exceeds eight bytes.
/// @param value Section-relative symbol value.
/// @param sectionNumber One-based defining section or zero for undefined.
/// @param type COFF type field.
/// @param storageClass COFF symbol storage class.
static void writeSymbol(std::vector<uint8_t> &out,
                        const std::string &name,
                        uint32_t strTabOffset,
                        uint32_t value,
                        int16_t sectionNumber,
                        uint16_t type,
                        uint8_t storageClass) {
    if (name.size() <= 8) {
        for (size_t i = 0; i < 8; ++i) {
            if (i < name.size())
                out.push_back(static_cast<uint8_t>(name[i]));
            else
                out.push_back(0);
        }
    } else {
        appendLE32(out, 0);
        appendLE32(out, strTabOffset);
    }

    appendLE32(out, value);
    appendLE16(out, static_cast<uint16_t>(sectionNumber));
    appendLE16(out, type);
    out.push_back(storageClass);
    out.push_back(0);
}

/// @brief Append one 10-byte COFF relocation record.
/// @param out Destination relocation-table buffer.
/// @param virtualAddr Section-relative fixup offset.
/// @param symbolTableIndex Referenced COFF symbol-table index.
/// @param type Architecture-specific relocation type.
static void writeReloc(std::vector<uint8_t> &out,
                       uint32_t virtualAddr,
                       uint32_t symbolTableIndex,
                       uint16_t type) {
    appendLE32(out, virtualAddr);
    appendLE32(out, symbolTableIndex);
    appendLE16(out, type);
}

/// @brief Narrow a native section/file size to a 32-bit COFF field.
/// @param value Native-size value to validate.
/// @param what Human-readable field description.
/// @param err Stream that receives an overflow diagnostic.
/// @param out Receives the narrowed value.
/// @return `true` when @p value does not exceed `UINT32_MAX`.
static bool checkedU32(size_t value, const char *what, std::ostream &err, uint32_t &out) {
    if (value > UINT32_MAX) {
        err << "CoffWriter: " << what << " exceeds 32-bit COFF limit\n";
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

/// @brief Add two 32-bit COFF offsets without overflow.
/// @param a Left operand.
/// @param b Right operand.
/// @param what Human-readable field description.
/// @param err Stream that receives an overflow diagnostic.
/// @param out Receives the sum.
/// @return `true` when the sum fits in `uint32_t`.
static bool addU32Checked(
    uint32_t a, uint32_t b, const char *what, std::ostream &err, uint32_t &out) {
    if (a > UINT32_MAX - b) {
        err << "CoffWriter: " << what << " exceeds 32-bit COFF limit\n";
        return false;
    }
    out = a + b;
    return true;
}

/// @brief Align a 32-bit COFF offset while checking both native and field limits.
/// @param value Offset to align.
/// @param align Required byte alignment.
/// @param what Human-readable field description.
/// @param err Stream that receives validation diagnostics.
/// @param out Receives the aligned offset.
/// @return `true` when alignment succeeds within the COFF range.
static bool alignU32Checked(
    uint32_t value, uint32_t align, const char *what, std::ostream &err, uint32_t &out) {
    size_t aligned = 0;
    if (!checkedAlignUpSize(static_cast<size_t>(value), align, "CoffWriter", what, err, aligned))
        return false;
    return checkedU32(aligned, what, err, out);
}

/// @brief Convert a logical symbol offset to its physical COFF section value.
/// @param section Section supplying logical bias and payload bounds.
/// @param sym Defined symbol to convert.
/// @param sectionName Name used to qualify diagnostics.
/// @param err Stream that receives bias, bounds, or width diagnostics.
/// @param out Receives the 32-bit physical offset.
/// @return `true` when the definition lies within section contents.
static bool checkedPhysicalSymbolValue(const CodeSection &section,
                                       const Symbol &sym,
                                       const char *sectionName,
                                       std::ostream &err,
                                       uint32_t &out) {
    if (sym.offset < section.logicalOffsetBias()) {
        err << "CoffWriter: symbol '" << sym.name << "' in " << sectionName
            << " is before the section logical offset bias\n";
        return false;
    }
    const size_t physicalOffset = sym.offset - section.logicalOffsetBias();
    if (physicalOffset > section.bytes().size()) {
        err << "CoffWriter: symbol '" << sym.name << "' in " << sectionName
            << " is outside section contents\n";
        return false;
    }
    return checkedU32(physicalOffset, "symbol value", err, out);
}

/// @brief Compute total object buffer reserve size with checked native arithmetic.
/// @param symtabOff File offset at which the symbol table begins.
/// @param symtabSize Serialized symbol-table byte count.
/// @param strtabSize Serialized string-table byte count.
/// @param err Stream that receives an overflow diagnostic.
/// @param out Receives the total byte count.
/// @return `true` when both additions are representable by `size_t`.
static bool checkedCoffReserveSize(
    uint32_t symtabOff, size_t symtabSize, size_t strtabSize, std::ostream &err, size_t &out) {
    size_t total = 0;
    if (!checkedAddSize(static_cast<size_t>(symtabOff),
                        symtabSize,
                        "CoffWriter",
                        "file reserve size",
                        err,
                        total))
        return false;
    return checkedAddSize(total, strtabSize, "CoffWriter", "file reserve size", err, out);
}

/// @brief Validate an already encoded COFF section-header name field.
/// @param name Inline name or slash-prefixed string-table offset.
/// @param what Section description used in diagnostics.
/// @param err Stream that receives an overlength diagnostic.
/// @return `true` when the encoded field fits eight bytes.
static bool validateSectionHeaderName(const std::string &name,
                                      const char *what,
                                      std::ostream &err) {
    if (name.size() > 8) {
        err << "CoffWriter: encoded section name reference for " << what
            << " exceeds COFF's 8-byte section-name field\n";
        return false;
    }
    return true;
}

/// @brief Register a global definition and diagnose duplicate writer output.
/// @param names Set of global names already emitted.
/// @param sym Candidate section symbol.
/// @param sectionName Defining section name used in diagnostics.
/// @param err Stream that receives a duplicate-definition diagnostic.
/// @return `true` for nonglobal or newly registered global symbols.
static bool rememberDefinedGlobal(std::unordered_set<std::string> &names,
                                  const Symbol &sym,
                                  const char *sectionName,
                                  std::ostream &err) {
    if (sym.binding != SymbolBinding::Global)
        return true;
    if (!names.insert(sym.name).second) {
        err << "CoffWriter: duplicate global symbol '" << sym.name << "' in " << sectionName
            << "\n";
        return false;
    }
    return true;
}

/// @brief Prefix a relocation table with COFF's overflow sentinel when required.
/// @param relocBytes Serialized ordinary relocation records, updated in place.
/// @param relocCount Number of ordinary relocation records.
static void addRelocationOverflowRecord(std::vector<uint8_t> &relocBytes, uint32_t relocCount) {
    if (relocCount <= kCoffMaxStandardRelocs)
        return;
    std::vector<uint8_t> withOverflow;
    withOverflow.reserve(relocBytes.size() + kCoffRelocSize);
    writeReloc(withOverflow, relocCount + 1u, 0, 0);
    withOverflow.insert(withOverflow.end(), relocBytes.begin(), relocBytes.end());
    relocBytes.swap(withOverflow);
}

/// @brief Compute the 16-bit section-header relocation count representation.
/// @param relocCount Number of ordinary relocation records.
/// @return @p relocCount when standard, otherwise the `0xffff` overflow marker.
static uint32_t coffHeaderRelocCount(uint32_t relocCount) {
    return relocCount > kCoffMaxStandardRelocs ? kCoffMaxStandardRelocs : relocCount;
}

/// @brief Count serialized records including a required overflow sentinel.
/// @param relocCount Number of ordinary relocation records.
/// @return Physical relocation-record count.
static uint32_t coffRelocRecordCount(uint32_t relocCount) {
    return relocCount + (relocCount > kCoffMaxStandardRelocs ? 1u : 0u);
}

/// @brief Compute a COFF relocation table's serialized byte size.
/// @param relocCount Number of ordinary records.
/// @param what Section description used in diagnostics.
/// @param err Stream that receives a 32-bit-limit diagnostic.
/// @param out Receives bytes for ordinary records plus any overflow sentinel.
/// @return `true` when the table size fits a 32-bit file offset.
static bool coffRelocTableSize(uint32_t relocCount,
                               const char *what,
                               std::ostream &err,
                               uint32_t &out) {
    const uint64_t bytes = static_cast<uint64_t>(coffRelocRecordCount(relocCount)) * kCoffRelocSize;
    if (bytes > UINT32_MAX) {
        err << "CoffWriter: " << what << " relocation table exceeds 32-bit COFF limit\n";
        return false;
    }
    out = static_cast<uint32_t>(bytes);
    return true;
}

/// @brief Deferred generated-symbol record awaiting final section numbering.
struct PendingCoffSymbol {
    ///< Symbol name written inline or through the string table.
    std::string name;
    ///< Section-relative symbol value.
    uint32_t value{0};
    ///< COFF symbol type field.
    uint16_t type{0};
    ///< COFF storage class.
    uint8_t storageClass{kImageSymClassStatic};
};

/// @brief Deferred generated relocation whose symbol index is resolved later.
struct PendingCoffReloc {
    ///< Fixup offset within the generated section.
    uint32_t offset{0};
    ///< Fallback name of the referenced symbol.
    std::string symbolName;
    ///< Architecture-specific COFF relocation type.
    uint16_t type{0};
    ///< Whether exact source symbol identity/index metadata is available.
    bool hasSymbolRef{false};
    ///< Exact CodeSection identity for disambiguating duplicate local names.
    uint64_t symbolSectionIdentity{0};
    ///< Encoder symbol index within the identified section.
    uint32_t symbolIndex{0};
};

/// @brief Validate that a generated `.pdata` fixup covers four existing bytes.
/// @param offset Section-relative relocation offset.
/// @param pdataSize Current `.pdata` byte count.
/// @param err Stream that receives an out-of-bounds diagnostic.
/// @return `true` when the four-byte fixup fits completely.
static bool validatePdataRelocOffset(uint32_t offset, size_t pdataSize, std::ostream &err) {
    if (static_cast<size_t>(offset) > pdataSize || 4 > pdataSize - static_cast<size_t>(offset)) {
        err << "CoffWriter: .pdata relocation at offset " << offset
            << " extends beyond .pdata contents\n";
        return false;
    }
    return true;
}

/// @brief Count two-byte Win64 unwind slots needed for one logical operation.
/// @param code Logical unwind operation to encode.
/// @return Slot count selected by operation and near/far offset form.
static size_t win64UnwindSlotCount(const Win64UnwindCode &code) {
    switch (code.kind) {
        case Win64UnwindCode::Kind::PushNonVol:
            return 1;
        case Win64UnwindCode::Kind::AllocStack:
            if (code.stackOffset <= 128)
                return 1;
            if (code.stackOffset <= 524280)
                return 2;
            return 3;
        case Win64UnwindCode::Kind::SaveNonVol:
            if ((code.stackOffset / 8) <= 0xFFFF)
                return 2;
            return 3;
        case Win64UnwindCode::Kind::SaveXmm128:
            if ((code.stackOffset / 16) <= 0xFFFF)
                return 2;
            return 3;
    }
    return 0;
}

/// @brief Append the Win64 unwind-code nodes for one logical operation.
/// @param out Destination `.xdata` byte stream.
/// @param code Validated logical operation.
static void emitWin64UnwindNodes(std::vector<uint8_t> &out, const Win64UnwindCode &code) {
    constexpr uint8_t kUwopPushNonVol = 0;
    constexpr uint8_t kUwopAllocLarge = 1;
    constexpr uint8_t kUwopAllocSmall = 2;
    constexpr uint8_t kUwopSaveNonVol = 4;
    constexpr uint8_t kUwopSaveNonVolFar = 5;
    constexpr uint8_t kUwopSaveXmm128 = 8;
    constexpr uint8_t kUwopSaveXmm128Far = 9;

    /// Emit the common two-byte unwind node header.
    const auto emitNode = [&](uint8_t codeOffset, uint8_t unwindOp, uint8_t opInfo) {
        out.push_back(codeOffset);
        out.push_back(static_cast<uint8_t>(unwindOp | (opInfo << 4)));
    };

    switch (code.kind) {
        case Win64UnwindCode::Kind::PushNonVol:
            emitNode(code.codeOffset, kUwopPushNonVol, code.reg);
            return;
        case Win64UnwindCode::Kind::AllocStack:
            if (code.stackOffset <= 128) {
                emitNode(code.codeOffset,
                         kUwopAllocSmall,
                         static_cast<uint8_t>((code.stackOffset - 8) / 8));
                return;
            }
            if (code.stackOffset <= 524280) {
                emitNode(code.codeOffset, kUwopAllocLarge, 0);
                appendLE16(out, static_cast<uint16_t>(code.stackOffset / 8));
                return;
            }
            emitNode(code.codeOffset, kUwopAllocLarge, 1);
            appendLE16(out, static_cast<uint16_t>(code.stackOffset & 0xFFFF));
            appendLE16(out, static_cast<uint16_t>(code.stackOffset >> 16));
            return;
        case Win64UnwindCode::Kind::SaveNonVol:
            if ((code.stackOffset / 8) <= 0xFFFF) {
                emitNode(code.codeOffset, kUwopSaveNonVol, code.reg);
                appendLE16(out, static_cast<uint16_t>(code.stackOffset / 8));
                return;
            }
            emitNode(code.codeOffset, kUwopSaveNonVolFar, code.reg);
            appendLE16(out, static_cast<uint16_t>(code.stackOffset & 0xFFFF));
            appendLE16(out, static_cast<uint16_t>(code.stackOffset >> 16));
            return;
        case Win64UnwindCode::Kind::SaveXmm128:
            if ((code.stackOffset / 16) <= 0xFFFF) {
                emitNode(code.codeOffset, kUwopSaveXmm128, code.reg);
                appendLE16(out, static_cast<uint16_t>(code.stackOffset / 16));
                return;
            }
            emitNode(code.codeOffset, kUwopSaveXmm128Far, code.reg);
            appendLE16(out, static_cast<uint16_t>(code.stackOffset & 0xFFFF));
            appendLE16(out, static_cast<uint16_t>(code.stackOffset >> 16));
            return;
    }
}

/// @brief Validate one Win64 unwind operation against its function prologue.
/// @param entry Function-level metadata supplying the prologue length.
/// @param code Operation whose register, offset, and alignment are checked.
/// @param err Stream that receives the first validation diagnostic.
/// @return `true` when the operation has a legal Windows x64 encoding.
static bool validateWin64UnwindCode(const Win64UnwindEntry &entry,
                                    const Win64UnwindCode &code,
                                    std::ostream &err) {
    if (code.codeOffset == 0 || code.codeOffset > entry.prologueSize) {
        err << "CoffWriter: Win64 unwind code offset " << static_cast<unsigned>(code.codeOffset)
            << " is outside the function prologue\n";
        return false;
    }

    switch (code.kind) {
        case Win64UnwindCode::Kind::PushNonVol:
            if (code.reg > 15) {
                err << "CoffWriter: Win64 push unwind register is out of range\n";
                return false;
            }
            return true;
        case Win64UnwindCode::Kind::AllocStack:
            if (code.stackOffset < 8 || (code.stackOffset % 8) != 0) {
                err << "CoffWriter: Win64 stack allocation unwind offset must be >= 8 and "
                       "8-byte aligned\n";
                return false;
            }
            return true;
        case Win64UnwindCode::Kind::SaveNonVol:
            if (code.reg > 15 || (code.stackOffset % 8) != 0) {
                err << "CoffWriter: Win64 nonvolatile save unwind offset must be 8-byte aligned\n";
                return false;
            }
            return true;
        case Win64UnwindCode::Kind::SaveXmm128:
            if (code.reg > 15 || (code.stackOffset % 16) != 0) {
                err << "CoffWriter: Win64 XMM save unwind offset must be 16-byte aligned\n";
                return false;
            }
            return true;
    }
    return true;
}

/// @brief Build generated Win64 `.xdata` records and `.pdata` references.
/// @param text Text section whose per-function unwind entries are consumed.
/// @param xdataNameBase Starting ordinal for generated `$xdata$N` symbols.
/// @param xdataBytes Receives serialized unwind information.
/// @param xdataSymbols Receives symbols at each generated xdata record.
/// @param pdataBytes Receives 12-byte runtime-function records.
/// @param pdataRelocs Receives function-start, function-end, and xdata fixups.
/// @param err Stream that receives malformed metadata or overflow diagnostics.
/// @return `true` when all Windows x64 unwind entries serialize successfully.
static bool buildWin64UnwindSections(const CodeSection &text,
                                     uint32_t xdataNameBase,
                                     std::vector<uint8_t> &xdataBytes,
                                     std::vector<PendingCoffSymbol> &xdataSymbols,
                                     std::vector<uint8_t> &pdataBytes,
                                     std::vector<PendingCoffReloc> &pdataRelocs,
                                     std::ostream &err) {
    const auto &entries = text.win64UnwindEntries();
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto &entry = entries[i];
        if (entry.symbolIndex >= text.symbols().count()) {
            err << "CoffWriter: Win64 unwind entry references unknown symbol index "
                << entry.symbolIndex << "\n";
            return false;
        }
        const auto &funcSym = text.symbols().at(entry.symbolIndex);
        if (funcSym.binding == SymbolBinding::External ||
            funcSym.section == SymbolSection::Undefined) {
            err << "CoffWriter: Win64 unwind entry references undefined symbol '" << funcSym.name
                << "'\n";
            return false;
        }
        if (entry.prologueSize == 0 && !entry.codes.empty()) {
            err << "CoffWriter: Win64 unwind entry has codes but zero prologue size\n";
            return false;
        }
        uint32_t xdataOffset = 0;
        if (!checkedU32(xdataBytes.size(), ".xdata offset", err, xdataOffset))
            return false;
        uint32_t xdataOrdinal = 0;
        uint32_t entryOrdinal = 0;
        if (!checkedU32(i, ".xdata symbol ordinal", err, entryOrdinal) ||
            !addU32Checked(xdataNameBase, entryOrdinal, ".xdata symbol ordinal", err, xdataOrdinal))
            return false;
        const std::string xdataName = "$xdata$" + std::to_string(xdataOrdinal);
        xdataSymbols.push_back({xdataName, xdataOffset, 0, kImageSymClassStatic});

        std::vector<Win64UnwindCode> codes = entry.codes;
        /// Windows requires unwind operations in descending prologue offset;
        /// stability preserves encoder order for equal-offset operations.
        std::stable_sort(codes.begin(), codes.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.codeOffset > rhs.codeOffset;
        });

        size_t codeSlots = 0;
        for (const auto &code : codes) {
            if (!validateWin64UnwindCode(entry, code, err))
                return false;
            codeSlots += win64UnwindSlotCount(code);
        }
        if (codeSlots > std::numeric_limits<uint8_t>::max()) {
            err << "CoffWriter: Win64 unwind code slot count exceeds 255\n";
            return false;
        }

        xdataBytes.push_back(1);
        xdataBytes.push_back(entry.prologueSize);
        xdataBytes.push_back(static_cast<uint8_t>(codeSlots));
        xdataBytes.push_back(0);
        for (const auto &code : codes)
            emitWin64UnwindNodes(xdataBytes, code);
        padTo(xdataBytes, alignUp(xdataBytes.size(), 4));

        uint32_t pdataOffset = 0;
        if (!checkedU32(pdataBytes.size(), ".pdata offset", err, pdataOffset))
            return false;
        appendLE32(pdataBytes, 0);
        appendLE32(pdataBytes, entry.functionLength);
        appendLE32(pdataBytes, 0);

        uint32_t pdataEndOffset = 0;
        uint32_t pdataUnwindOffset = 0;
        if (!addU32Checked(
                pdataOffset, 4, ".pdata unwind end relocation offset", err, pdataEndOffset) ||
            !addU32Checked(
                pdataOffset, 8, ".pdata xdata relocation offset", err, pdataUnwindOffset))
            return false;
        pdataRelocs.push_back({pdataOffset,
                               funcSym.name,
                               kImageRelAMD64_Addr32Nb,
                               true,
                               text.sectionIdentity(),
                               entry.symbolIndex});
        pdataRelocs.push_back({pdataEndOffset,
                               funcSym.name,
                               kImageRelAMD64_Addr32Nb,
                               true,
                               text.sectionIdentity(),
                               entry.symbolIndex});
        pdataRelocs.push_back({pdataUnwindOffset, xdataName, kImageRelAMD64_Addr32Nb});
    }
    return true;
}

/// @brief Build generated Windows ARM64 `.xdata` and `.pdata` records.
/// @param text Text section whose ARM64 unwind entries are consumed.
/// @param xdataNameBase Starting ordinal for generated `$xdata$N` symbols.
/// @param xdataBytes Receives packed ARM64 unwind headers and opcode streams.
/// @param xdataSymbols Receives symbols at each generated xdata record.
/// @param pdataBytes Receives eight-byte ARM64 runtime-function records.
/// @param pdataRelocs Receives function-start and xdata RVA fixups.
/// @param err Stream that receives alignment, range, or metadata diagnostics.
/// @return `true` when all Windows ARM64 unwind entries serialize successfully.
static bool buildWinArm64UnwindSections(const CodeSection &text,
                                        uint32_t xdataNameBase,
                                        std::vector<uint8_t> &xdataBytes,
                                        std::vector<PendingCoffSymbol> &xdataSymbols,
                                        std::vector<uint8_t> &pdataBytes,
                                        std::vector<PendingCoffReloc> &pdataRelocs,
                                        std::ostream &err) {
    const auto &entries = text.winArm64UnwindEntries();
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto &entry = entries[i];
        if (entry.symbolIndex >= text.symbols().count()) {
            err << "CoffWriter: Windows ARM64 unwind entry references unknown symbol index "
                << entry.symbolIndex << "\n";
            return false;
        }
        const auto &funcSym = text.symbols().at(entry.symbolIndex);
        if (funcSym.binding == SymbolBinding::External ||
            funcSym.section == SymbolSection::Undefined) {
            err << "CoffWriter: Windows ARM64 unwind entry references undefined symbol '"
                << funcSym.name << "'\n";
            return false;
        }
        if ((entry.functionLength & 0x3u) != 0) {
            err << "CoffWriter: Windows ARM64 function length must be instruction aligned\n";
            return false;
        }
        const uint32_t functionWords = entry.functionLength / 4;
        if (functionWords > 0x3FFFFu) {
            err << "CoffWriter: Windows ARM64 function length exceeds xdata header range\n";
            return false;
        }
        const size_t codeWordsSize = alignUp(entry.unwindCodes.size(), 4) / 4;
        if (codeWordsSize > 31) {
            err << "CoffWriter: Windows ARM64 unwind code word count exceeds 31\n";
            return false;
        }
        if (entry.packedEpilogInHeader && entry.epilogCodeIndex > 31) {
            err << "CoffWriter: Windows ARM64 epilog code index exceeds header range\n";
            return false;
        }

        uint32_t xdataOffset = 0;
        if (!checkedU32(xdataBytes.size(), ".xdata offset", err, xdataOffset))
            return false;
        uint32_t xdataOrdinal = 0;
        uint32_t entryOrdinal = 0;
        if (!checkedU32(i, ".xdata symbol ordinal", err, entryOrdinal) ||
            !addU32Checked(xdataNameBase, entryOrdinal, ".xdata symbol ordinal", err, xdataOrdinal))
            return false;
        const std::string xdataName = "$xdata$" + std::to_string(xdataOrdinal);
        xdataSymbols.push_back({xdataName, xdataOffset, 0, kImageSymClassStatic});

        const uint32_t epilogField = entry.packedEpilogInHeader ? entry.epilogCodeIndex : 0u;
        const uint32_t header = functionWords | (0u << 18) | // version
                                (0u << 20) |                 // no exception data
                                ((entry.packedEpilogInHeader ? 1u : 0u) << 21) |
                                ((epilogField & 0x1Fu) << 22) |
                                (static_cast<uint32_t>(codeWordsSize) << 27);
        appendLE32(xdataBytes, header);
        xdataBytes.insert(xdataBytes.end(), entry.unwindCodes.begin(), entry.unwindCodes.end());
        padTo(xdataBytes, alignUp(xdataBytes.size(), 4));

        uint32_t pdataOffset = 0;
        if (!checkedU32(pdataBytes.size(), ".pdata offset", err, pdataOffset))
            return false;
        appendLE32(pdataBytes, 0);
        appendLE32(pdataBytes, 0);

        uint32_t pdataUnwindOffset = 0;
        if (!addU32Checked(
                pdataOffset, 4, ".pdata xdata relocation offset", err, pdataUnwindOffset))
            return false;
        pdataRelocs.push_back({pdataOffset,
                               funcSym.name,
                               kImageRelARM64_Addr32Nb,
                               true,
                               text.sectionIdentity(),
                               entry.symbolIndex});
        pdataRelocs.push_back({pdataUnwindOffset, xdataName, kImageRelARM64_Addr32Nb});
    }
    return true;
}

/// @brief Collect xdata/pdata for one text section, advancing @p xdataNameBase
///        by the count of unwind entries emitted. No-op for sections with no
///        unwind entries or for unsupported architectures.
/// @details Shared between the single- and multi-section write() overloads so
///          the per-section unwind iteration lives in one place. The arch is
///          carried in rather than inferred so each overload can pass the same
///          `arch_` value verbatim.
/// @param text Text section whose architecture-specific unwind list is consumed.
/// @param arch Target architecture selecting the unwind representation.
/// @param xdataNameBase In/out ordinal for unique generated xdata symbols.
/// @param xdataBytes Accumulated serialized unwind bytes.
/// @param xdataSymbols Accumulated generated xdata symbols.
/// @param pdataBytes Accumulated runtime-function table bytes.
/// @param pdataRelocs Accumulated runtime-function relocations.
/// @param err Stream that receives serialization diagnostics.
/// @return `true` after appending this section's unwind data or doing nothing.
static bool collectCoffUnwindForSection(const CodeSection &text,
                                        ObjArch arch,
                                        uint32_t &xdataNameBase,
                                        std::vector<uint8_t> &xdataBytes,
                                        std::vector<PendingCoffSymbol> &xdataSymbols,
                                        std::vector<uint8_t> &pdataBytes,
                                        std::vector<PendingCoffReloc> &pdataRelocs,
                                        std::ostream &err) {
    if (arch == ObjArch::X86_64 && !text.win64UnwindEntries().empty()) {
        if (!buildWin64UnwindSections(
                text, xdataNameBase, xdataBytes, xdataSymbols, pdataBytes, pdataRelocs, err))
            return false;
        uint32_t entryCount = 0;
        if (!checkedU32(text.win64UnwindEntries().size(), ".xdata symbol count", err, entryCount) ||
            !addU32Checked(xdataNameBase, entryCount, ".xdata symbol count", err, xdataNameBase))
            return false;
    } else if (arch == ObjArch::AArch64 && !text.winArm64UnwindEntries().empty()) {
        if (!buildWinArm64UnwindSections(
                text, xdataNameBase, xdataBytes, xdataSymbols, pdataBytes, pdataRelocs, err))
            return false;
        uint32_t entryCount = 0;
        if (!checkedU32(
                text.winArm64UnwindEntries().size(), ".xdata symbol count", err, entryCount) ||
            !addU32Checked(xdataNameBase, entryCount, ".xdata symbol count", err, xdataNameBase))
            return false;
    }
    return true;
}

/// @copydoc CoffWriter::write(const std::string&, const CodeSection&,
///                            const CodeSection&, std::ostream&)
bool CoffWriter::write(const std::string &path,
                       const CodeSection &text,
                       const CodeSection &rodata,
                       std::ostream &err) {
    try {
        std::vector<uint8_t> xdataBytes;
        std::vector<PendingCoffSymbol> xdataSymbols;
        std::vector<uint8_t> pdataBytes;
        std::vector<PendingCoffReloc> pdataRelocs;
        uint32_t xdataNameBase = 0;
        if (!collectCoffUnwindForSection(
                text, arch_, xdataNameBase, xdataBytes, xdataSymbols, pdataBytes, pdataRelocs, err))
            return false;

        // Writable initialized-data section (.data) for scalar globals. Symbols here
        // coalesce the text section's undefined references by name (definedGlobalNameMap).
        const CodeSection emptyData;
        const CodeSection &data = dataSection_ ? *dataSection_ : emptyData;
        const bool hasData = !data.bytes().empty();

        const bool hasRodata = !rodata.empty();
        const bool hasXdata = !xdataBytes.empty();
        const bool hasPdata = !pdataBytes.empty();
        const bool hasDebugLine = !debugLineData_.empty();

        uint32_t nextSecIndex = 1;
        const uint16_t secIdxText = static_cast<uint16_t>(nextSecIndex++);
        const uint16_t secIdxRdata = hasRodata ? static_cast<uint16_t>(nextSecIndex++) : 0;
        const uint16_t secIdxData = hasData ? static_cast<uint16_t>(nextSecIndex++) : 0;
        const uint16_t secIdxXdata = hasXdata ? static_cast<uint16_t>(nextSecIndex++) : 0;
        if (hasPdata)
            ++nextSecIndex;
        const uint32_t sectionCount = (nextSecIndex - 1) + (hasDebugLine ? 1u : 0u);
        if (sectionCount > static_cast<uint32_t>(std::numeric_limits<int16_t>::max())) {
            err << "CoffWriter: section count exceeds standard COFF symbol section-number range; "
                   "BigObj output is not available\n";
            return false;
        }
        const uint16_t numSections = static_cast<uint16_t>(sectionCount);

        std::vector<uint8_t> symtabBytes;
        std::vector<uint8_t> strtabBytes(4, 0);
        std::unordered_map<uint32_t, uint32_t> textSymMap;
        std::unordered_map<uint32_t, uint32_t> rodataSymMap;
        std::unordered_map<std::string, uint32_t> definedNameMap;
        std::unordered_map<std::string, uint32_t> definedGlobalNameMap;
        std::unordered_map<std::string, uint32_t> externalNameMap;
        std::unordered_set<std::string> definedGlobalNames;
        uint32_t coffSymCount = 0;

        /// Determine whether a section needs a synthetic anchor symbol for
        /// exact section-offset relocations targeting @p targetSection.
        auto relocationNeedsAnchor = [](const CodeSection &sec, SymbolSection targetSection) {
            for (const auto &rel : sec.relocations())
                if (rel.targetOffsetValid && rel.targetSection == targetSection)
                    return true;
            return false;
        };
        const bool needTextAnchor = relocationNeedsAnchor(text, SymbolSection::Text) ||
                                    relocationNeedsAnchor(rodata, SymbolSection::Text);
        const bool needRodataAnchor =
            hasRodata && (relocationNeedsAnchor(text, SymbolSection::Rodata) ||
                          relocationNeedsAnchor(rodata, SymbolSection::Rodata));
        uint32_t textAnchorIdx = UINT32_MAX;
        uint32_t rodataAnchorIdx = UINT32_MAX;

        if (needTextAnchor) {
            writeSymbol(symtabBytes,
                        ".text",
                        0,
                        0,
                        static_cast<int16_t>(secIdxText),
                        0,
                        kImageSymClassStatic);
            textAnchorIdx = coffSymCount++;
        }
        if (needRodataAnchor) {
            writeSymbol(symtabBytes,
                        ".rdata",
                        0,
                        0,
                        static_cast<int16_t>(secIdxRdata),
                        0,
                        kImageSymClassStatic);
            rodataAnchorIdx = coffSymCount++;
        }

        /// Deferred undefined symbol whose final COFF index may coalesce with a
        /// definition discovered in another emitted section.
        struct PendingExternal {
            ///< Whether the original encoder symbol belongs to `.text`.
            bool fromText = false;
            ///< Original CodeSection symbol-table index.
            uint32_t origIdx = 0;
            ///< Symbol name used for cross-section coalescing.
            std::string name;
            ///< COFF symbol type, such as function type `0x20`.
            uint16_t type = 0;
        };

        std::vector<PendingExternal> pendingExternals;

        /// Append a NUL-terminated long name to the COFF string table and return
        /// its checked byte offset.
        auto addToStrTab = [&](const std::string &s) -> uint32_t {
            if (strtabBytes.size() > std::numeric_limits<uint32_t>::max())
                throw std::length_error(
                    "CoffWriter: string table offset exceeds 32-bit COFF limit");
            if (s.size() > std::numeric_limits<size_t>::max() - strtabBytes.size() - 1 ||
                strtabBytes.size() + s.size() + 1 > std::numeric_limits<uint32_t>::max()) {
                throw std::length_error("CoffWriter: string table size exceeds 32-bit COFF limit");
            }
            uint32_t offset = static_cast<uint32_t>(strtabBytes.size());
            strtabBytes.insert(strtabBytes.end(), s.begin(), s.end());
            strtabBytes.push_back(0);
            return offset;
        };

        if (hasRodata) {
            for (uint32_t i = 1; i < rodata.symbols().count(); ++i) {
                const Symbol &s = rodata.symbols().at(i);
                int16_t secNum = 0;
                uint32_t value = 0;
                uint8_t storageClass = kImageSymClassExternal;

                if (s.binding == SymbolBinding::External) {
                    pendingExternals.push_back({false, i, s.name, 0});
                    continue;
                } else if (s.binding == SymbolBinding::Local) {
                    secNum = static_cast<int16_t>(secIdxRdata);
                    if (!checkedPhysicalSymbolValue(rodata, s, ".rdata", err, value))
                        return false;
                    storageClass = kImageSymClassStatic;
                } else {
                    if (!rememberDefinedGlobal(definedGlobalNames, s, ".rdata", err))
                        return false;
                    secNum = static_cast<int16_t>(secIdxRdata);
                    if (!checkedPhysicalSymbolValue(rodata, s, ".rdata", err, value))
                        return false;
                }

                uint32_t strOff = 0;
                if (s.name.size() > 8)
                    strOff = addToStrTab(s.name);

                writeSymbol(symtabBytes, s.name, strOff, value, secNum, 0, storageClass);
                const uint32_t coffIdx = coffSymCount++;
                rodataSymMap[i] = coffIdx;
                if (s.binding != SymbolBinding::External) {
                    definedNameMap[s.name] = coffIdx;
                    if (s.binding == SymbolBinding::Global)
                        definedGlobalNameMap[s.name] = coffIdx;
                }
            }
        }

        std::unordered_map<std::string, uint32_t> definedRodataByName;
        if (hasRodata) {
            for (uint32_t i = 1; i < rodata.symbols().count(); ++i) {
                const Symbol &s = rodata.symbols().at(i);
                if (s.binding == SymbolBinding::External)
                    continue;
                auto it = rodataSymMap.find(i);
                if (it != rodataSymMap.end()) {
                    auto [nameIt, inserted] = definedRodataByName.emplace(s.name, it->second);
                    if (!inserted)
                        nameIt->second = UINT32_MAX;
                }
            }
        }

        if (hasXdata) {
            for (const auto &sym : xdataSymbols) {
                uint32_t strOff = 0;
                if (sym.name.size() > 8)
                    strOff = addToStrTab(sym.name);
                writeSymbol(symtabBytes,
                            sym.name,
                            strOff,
                            sym.value,
                            static_cast<int16_t>(secIdxXdata),
                            sym.type,
                            sym.storageClass);
                definedNameMap[sym.name] = coffSymCount++;
            }
        }

        std::unordered_set<uint32_t> crossSectionTextAliases;
        std::unordered_set<uint32_t> undefinedTextRefs;
        for (const auto &rel : text.relocations()) {
            if (rel.symbolIndex >= text.symbols().count())
                continue;
            if (rel.targetSection != SymbolSection::Undefined)
                crossSectionTextAliases.insert(rel.symbolIndex);
            else
                undefinedTextRefs.insert(rel.symbolIndex);
        }

        for (uint32_t i = 1; i < text.symbols().count(); ++i) {
            const Symbol &s = text.symbols().at(i);
            if (s.binding == SymbolBinding::External) {
                if (crossSectionTextAliases.count(i) != 0 && undefinedTextRefs.count(i) == 0) {
                    continue;
                }
                pendingExternals.push_back({true, i, s.name, 0x20});
                continue;
            }

            int16_t secNum = 0;
            uint32_t value = 0;
            uint8_t storageClass = kImageSymClassExternal;

            if (s.binding == SymbolBinding::External) {
                secNum = kImageSymUndefined;
            } else if (s.binding == SymbolBinding::Local) {
                secNum = static_cast<int16_t>(secIdxText);
                if (!checkedPhysicalSymbolValue(text, s, ".text", err, value))
                    return false;
                storageClass = kImageSymClassStatic;
            } else {
                if (!rememberDefinedGlobal(definedGlobalNames, s, ".text", err))
                    return false;
                secNum = static_cast<int16_t>(secIdxText);
                if (!checkedPhysicalSymbolValue(text, s, ".text", err, value))
                    return false;
            }

            uint32_t strOff = 0;
            if (s.name.size() > 8)
                strOff = addToStrTab(s.name);

            writeSymbol(symtabBytes, s.name, strOff, value, secNum, 0x20, storageClass);
            const uint32_t coffIdx = coffSymCount++;
            textSymMap[i] = coffIdx;
            if (s.binding != SymbolBinding::External) {
                definedNameMap[s.name] = coffIdx;
                if (s.binding == SymbolBinding::Global)
                    definedGlobalNameMap[s.name] = coffIdx;
            }
        }

        // Writable scalar globals in .data — defined symbols whose names coalesce the
        // text section's undefined references (handled by the pendingExternals loop).
        if (hasData) {
            for (uint32_t i = 1; i < data.symbols().count(); ++i) {
                const Symbol &s = data.symbols().at(i);
                if (s.binding == SymbolBinding::External)
                    continue; // a .data symbol is always a definition
                uint32_t value = 0;
                uint8_t storageClass = kImageSymClassExternal;
                if (s.binding == SymbolBinding::Local) {
                    if (!checkedPhysicalSymbolValue(data, s, ".data", err, value))
                        return false;
                    storageClass = kImageSymClassStatic;
                } else {
                    if (!rememberDefinedGlobal(definedGlobalNames, s, ".data", err))
                        return false;
                    if (!checkedPhysicalSymbolValue(data, s, ".data", err, value))
                        return false;
                }
                uint32_t strOff = 0;
                if (s.name.size() > 8)
                    strOff = addToStrTab(s.name);
                writeSymbol(symtabBytes,
                            s.name,
                            strOff,
                            value,
                            static_cast<int16_t>(secIdxData),
                            0,
                            storageClass);
                const uint32_t coffIdx = coffSymCount++;
                definedNameMap[s.name] = coffIdx;
                if (s.binding == SymbolBinding::Global)
                    definedGlobalNameMap[s.name] = coffIdx;
            }
        }

        for (const auto &ext : pendingExternals) {
            const auto definedIt = definedGlobalNameMap.find(ext.name);
            if (definedIt != definedGlobalNameMap.end()) {
                if (ext.fromText)
                    textSymMap[ext.origIdx] = definedIt->second;
                else
                    rodataSymMap[ext.origIdx] = definedIt->second;
                continue;
            }

            auto extIt = externalNameMap.find(ext.name);
            if (extIt == externalNameMap.end()) {
                uint32_t strOff = 0;
                if (ext.name.size() > 8)
                    strOff = addToStrTab(ext.name);
                writeSymbol(symtabBytes,
                            ext.name,
                            strOff,
                            0,
                            kImageSymUndefined,
                            ext.type,
                            kImageSymClassExternal);
                extIt = externalNameMap.emplace(ext.name, coffSymCount++).first;
            }

            if (ext.fromText)
                textSymMap[ext.origIdx] = extIt->second;
            else
                rodataSymMap[ext.origIdx] = extIt->second;
        }

        std::unordered_map<std::string, uint32_t> definedTextByName;
        for (uint32_t i = 1; i < text.symbols().count(); ++i) {
            const Symbol &s = text.symbols().at(i);
            if (s.binding == SymbolBinding::External)
                continue;
            auto it = textSymMap.find(i);
            if (it == textSymMap.end())
                continue;
            auto [nameIt, inserted] = definedTextByName.emplace(s.name, it->second);
            if (!inserted)
                nameIt->second = UINT32_MAX;
        }

        uint32_t debugLineStrOff = 0;
        if (hasDebugLine)
            debugLineStrOff = addToStrTab(".debug_line");

        uint32_t strtabSize = 0;
        if (!checkedU32(strtabBytes.size(), "string table size", err, strtabSize))
            return false;
        strtabBytes[0] = static_cast<uint8_t>(strtabSize);
        strtabBytes[1] = static_cast<uint8_t>(strtabSize >> 8);
        strtabBytes[2] = static_cast<uint8_t>(strtabSize >> 16);
        strtabBytes[3] = static_cast<uint8_t>(strtabSize >> 24);

        std::vector<uint8_t> patchedTextBytes = text.bytes();
        std::vector<uint8_t> patchedRodataBytes = rodata.bytes();
        std::vector<uint8_t> textRelocBytes;

        /// Resolve one encoder relocation to a final COFF symbol index and
        /// effective addend, including exact section-offset anchor handling.
        auto resolveRelocSym = [&](const Relocation &rel,
                                   const CodeSection &source,
                                   const std::unordered_map<uint32_t, uint32_t> &sourceMap,
                                   const char *sectionName,
                                   uint32_t &coffSymIdx,
                                   int64_t &effectiveAddend) -> bool {
            coffSymIdx = 0;
            effectiveAddend = rel.addend;
            if (rel.targetSection != SymbolSection::Undefined) {
                if (rel.targetOffsetValid) {
                    const CodeSection &target =
                        (rel.targetSection == SymbolSection::Text) ? text : rodata;
                    const char *targetName =
                        (rel.targetSection == SymbolSection::Text) ? ".text" : ".rdata";
                    const uint32_t anchorIdx = (rel.targetSection == SymbolSection::Text)
                                                   ? textAnchorIdx
                                                   : rodataAnchorIdx;
                    if (anchorIdx == UINT32_MAX) {
                        err << "CoffWriter: missing section anchor for " << targetName << "\n";
                        return false;
                    }
                    if (rel.targetOffset > target.bytes().size()) {
                        err << "CoffWriter: relocation in " << sectionName << " at offset "
                            << rel.offset << " references " << targetName << " offset "
                            << rel.targetOffset << " beyond section contents\n";
                        return false;
                    }
                    if (!checkedSectionOffsetAddend(rel.addend,
                                                    rel.targetOffset,
                                                    "CoffWriter",
                                                    sectionName,
                                                    rel.offset,
                                                    err,
                                                    effectiveAddend))
                        return false;
                    coffSymIdx = anchorIdx;
                    return true;
                }
                if (rel.symbolIndex >= source.symbols().count()) {
                    err << "CoffWriter: relocation in " << sectionName << " at offset "
                        << rel.offset << " references unknown symbol index " << rel.symbolIndex
                        << "\n";
                    return false;
                }
                const Symbol &sym = source.symbols().at(rel.symbolIndex);
                const auto &targetByName = (rel.targetSection == SymbolSection::Rodata)
                                               ? definedRodataByName
                                               : definedTextByName;
                auto nameIt = targetByName.find(sym.name);
                if (nameIt == targetByName.end()) {
                    err << "CoffWriter: relocation in " << sectionName << " at offset "
                        << rel.offset << " references missing cross-section target '" << sym.name
                        << "'\n";
                    return false;
                }
                if (nameIt->second == UINT32_MAX) {
                    err << "CoffWriter: relocation in " << sectionName << " at offset "
                        << rel.offset << " references ambiguous cross-section target '" << sym.name
                        << "'\n";
                    return false;
                }
                coffSymIdx = nameIt->second;
                return true;
            }

            auto it = sourceMap.find(rel.symbolIndex);
            if (it == sourceMap.end()) {
                err << "CoffWriter: relocation in " << sectionName << " at offset " << rel.offset
                    << " references unknown symbol index " << rel.symbolIndex << "\n";
                return false;
            }
            coffSymIdx = it->second;
            return true;
        };

        /// Validate, addend-patch, and serialize every relocation from one
        /// source section into its COFF relocation byte stream.
        auto appendRelocBytes = [&](const CodeSection &source,
                                    const std::unordered_map<uint32_t, uint32_t> &sourceMap,
                                    const char *sectionName,
                                    std::vector<uint8_t> &patchedBytes,
                                    std::vector<uint8_t> &relocBytes) -> bool {
            for (const auto &rel : source.relocations()) {
                if (!validateRelocationShape("CoffWriter", arch_, source, rel, sectionName, err))
                    return false;
                uint32_t coffSymIdx = 0;
                int64_t effectiveAddend = rel.addend;
                if (!resolveRelocSym(
                        rel, source, sourceMap, sectionName, coffSymIdx, effectiveAddend))
                    return false;
                if (!patchCoffRelocationAddend(source, rel, effectiveAddend, patchedBytes, err))
                    return false;
                const size_t physicalRelOffset = rel.offset - source.logicalOffsetBias();
                uint32_t relocOff = 0;
                if (!checkedU32(physicalRelOffset, "relocation offset", err, relocOff))
                    return false;
                writeReloc(relocBytes, relocOff, coffSymIdx, coffRelocType(rel.kind, arch_));
            }
            return true;
        };

        if (!appendRelocBytes(text, textSymMap, ".text", patchedTextBytes, textRelocBytes))
            return false;
        uint32_t numTextRelocs = 0;
        if (!checkedU32(text.relocations().size(), "text relocation count", err, numTextRelocs))
            return false;
        addRelocationOverflowRecord(textRelocBytes, numTextRelocs);

        std::vector<uint8_t> rdataRelocBytes;
        if (!appendRelocBytes(rodata, rodataSymMap, ".rdata", patchedRodataBytes, rdataRelocBytes))
            return false;
        uint32_t numRdataRelocs = 0;
        if (!checkedU32(
                rodata.relocations().size(), ".rdata relocation count", err, numRdataRelocs))
            return false;
        addRelocationOverflowRecord(rdataRelocBytes, numRdataRelocs);

        std::vector<uint8_t> pdataRelocBytes;
        for (const auto &rel : pdataRelocs) {
            if (!validatePdataRelocOffset(rel.offset, pdataBytes.size(), err))
                return false;
            uint32_t targetIdx = 0;
            if (rel.hasSymbolRef) {
                if (!text.matchesSectionIdentity(rel.symbolSectionIdentity)) {
                    err << "CoffWriter: .pdata relocation for '" << rel.symbolName
                        << "' references a different text section identity\n";
                    return false;
                }
                auto symIt = textSymMap.find(rel.symbolIndex);
                if (symIt == textSymMap.end()) {
                    err << "CoffWriter: missing symbol '" << rel.symbolName
                        << "' for .pdata relocation\n";
                    return false;
                }
                targetIdx = symIt->second;
            } else {
                auto it = definedNameMap.find(rel.symbolName);
                if (it == definedNameMap.end()) {
                    err << "CoffWriter: missing symbol '" << rel.symbolName
                        << "' for .pdata relocation\n";
                    return false;
                }
                targetIdx = it->second;
            }
            writeReloc(pdataRelocBytes, rel.offset, targetIdx, rel.type);
        }
        uint32_t numPdataRelocs = 0;
        if (!checkedU32(pdataRelocs.size(), ".pdata relocation count", err, numPdataRelocs))
            return false;
        addRelocationOverflowRecord(pdataRelocBytes, numPdataRelocs);

        uint32_t textSize = 0;
        uint32_t rdataSize = 0;
        uint32_t dataSize = 0;
        uint32_t xdataSize = 0;
        uint32_t pdataSize = 0;
        uint32_t debugLineDataSize = 0;
        if (!checkedU32(patchedTextBytes.size(), ".text size", err, textSize) ||
            !checkedU32(hasRodata ? patchedRodataBytes.size() : 0, ".rdata size", err, rdataSize) ||
            !checkedU32(hasData ? data.bytes().size() : 0, ".data size", err, dataSize) ||
            !checkedU32(hasXdata ? xdataBytes.size() : 0, ".xdata size", err, xdataSize) ||
            !checkedU32(hasPdata ? pdataBytes.size() : 0, ".pdata size", err, pdataSize) ||
            !checkedU32(hasDebugLine ? debugLineData_.size() : 0,
                        ".debug_line size",
                        err,
                        debugLineDataSize))
            return false;

        const uint32_t headerAreaSize = kCoffHeaderSize + numSections * kSectionHeaderSize;
        uint32_t textDataOff = 0;
        if (!alignU32Checked(headerAreaSize, 4, "header area", err, textDataOff))
            return false;
        uint32_t textRelocOff = 0;
        if (!addU32Checked(textDataOff, textSize, ".text relocation offset", err, textRelocOff))
            return false;
        uint32_t textRelocTotalSize = 0;
        if (!coffRelocTableSize(numTextRelocs, ".text", err, textRelocTotalSize))
            return false;
        uint32_t textRelocEnd = 0;
        if (!addU32Checked(
                textRelocOff, textRelocTotalSize, ".text relocation table", err, textRelocEnd))
            return false;
        uint32_t cursor = 0;
        if (!alignU32Checked(textRelocEnd, 4, ".text relocation table", err, cursor))
            return false;

        uint32_t rdataDataOff = 0;
        uint32_t rdataRelocOff = 0;
        if (hasRodata) {
            rdataDataOff = cursor;
            uint32_t end = 0;
            if (!addU32Checked(
                    rdataDataOff, rdataSize, ".rdata relocation offset", err, rdataRelocOff))
                return false;
            uint32_t rdataRelocSize = 0;
            if (!coffRelocTableSize(numRdataRelocs, ".rdata", err, rdataRelocSize))
                return false;
            if (!addU32Checked(rdataRelocOff, rdataRelocSize, ".rdata relocation table", err, end))
                return false;
            if (!alignU32Checked(end, 4, ".rdata relocation table", err, cursor))
                return false;
        }
        uint32_t dataDataOff = 0;
        if (hasData) {
            dataDataOff = cursor;
            uint32_t end = 0;
            if (!addU32Checked(dataDataOff, dataSize, ".data data", err, end))
                return false;
            if (!alignU32Checked(end, 4, ".data data", err, cursor))
                return false;
        }
        uint32_t xdataDataOff = 0;
        if (hasXdata) {
            xdataDataOff = cursor;
            uint32_t end = 0;
            if (!addU32Checked(xdataDataOff, xdataSize, ".xdata data", err, end))
                return false;
            if (!alignU32Checked(end, 4, ".xdata data", err, cursor))
                return false;
        }
        uint32_t pdataDataOff = 0;
        uint32_t pdataRelocOff = 0;
        if (hasPdata) {
            pdataDataOff = cursor;
            if (!addU32Checked(
                    pdataDataOff, pdataSize, ".pdata relocation offset", err, pdataRelocOff))
                return false;
            uint32_t pdataRelocEnd = 0;
            uint32_t pdataRelocSize = 0;
            if (!coffRelocTableSize(numPdataRelocs, ".pdata", err, pdataRelocSize))
                return false;
            if (!addU32Checked(
                    pdataRelocOff, pdataRelocSize, ".pdata relocation table", err, pdataRelocEnd))
                return false;
            if (!alignU32Checked(pdataRelocEnd, 4, ".pdata relocation table", err, cursor))
                return false;
        }
        uint32_t debugLineDataOff = 0;
        if (hasDebugLine) {
            debugLineDataOff = cursor;
            uint32_t end = 0;
            if (!addU32Checked(debugLineDataOff, debugLineDataSize, ".debug_line data", err, end))
                return false;
            if (!alignU32Checked(end, 4, ".debug_line data", err, cursor))
                return false;
        }
        const uint32_t symtabOff = cursor;

        std::vector<uint8_t> file;
        size_t reserveSize = 0;
        if (!checkedCoffReserveSize(
                symtabOff, symtabBytes.size(), strtabBytes.size(), err, reserveSize))
            return false;
        file.reserve(reserveSize);

        const uint16_t machine = (arch_ == ObjArch::X86_64) ? kMachineAMD64 : kMachineARM64;
        appendLE16(file, machine);
        appendLE16(file, numSections);
        appendLE32(file, 0);
        appendLE32(file, symtabOff);
        appendLE32(file, coffSymCount);
        appendLE16(file, 0);
        appendLE16(file, 0);

        uint32_t textChars = kImageScnCntCode | kImageScnMemExecute | kImageScnMemRead;
        textChars |= (arch_ == ObjArch::X86_64) ? kImageScnAlignText : kImageScnAlign4;
        if (numTextRelocs > kCoffMaxStandardRelocs)
            textChars |= kImageScnLnkNrelocOvfl;
        writeSectionHeader(file,
                           ".text",
                           0,
                           0,
                           textSize,
                           textDataOff,
                           (numTextRelocs > 0) ? textRelocOff : 0,
                           coffHeaderRelocCount(numTextRelocs),
                           textChars);

        if (hasRodata) {
            uint32_t rdataChars = kImageScnCntInitData | kImageScnMemRead | kImageScnAlign8;
            if (numRdataRelocs > kCoffMaxStandardRelocs)
                rdataChars |= kImageScnLnkNrelocOvfl;
            writeSectionHeader(file,
                               ".rdata",
                               0,
                               0,
                               rdataSize,
                               rdataDataOff,
                               (numRdataRelocs > 0) ? rdataRelocOff : 0,
                               coffHeaderRelocCount(numRdataRelocs),
                               rdataChars);
        }
        if (hasData) {
            const uint32_t dataChars =
                kImageScnCntInitData | kImageScnMemRead | kImageScnMemWrite | kImageScnAlign8;
            writeSectionHeader(file, ".data", 0, 0, dataSize, dataDataOff, 0, 0, dataChars);
        }
        if (hasXdata) {
            const uint32_t xdataChars = kImageScnCntInitData | kImageScnMemRead | kImageScnAlign4;
            writeSectionHeader(file, ".xdata", 0, 0, xdataSize, xdataDataOff, 0, 0, xdataChars);
        }
        if (hasPdata) {
            const uint32_t pdataChars = kImageScnCntInitData | kImageScnMemRead | kImageScnAlign4;
            uint32_t pdataHeaderChars = pdataChars;
            if (numPdataRelocs > kCoffMaxStandardRelocs)
                pdataHeaderChars |= kImageScnLnkNrelocOvfl;
            writeSectionHeader(file,
                               ".pdata",
                               0,
                               0,
                               pdataSize,
                               pdataDataOff,
                               (numPdataRelocs > 0) ? pdataRelocOff : 0,
                               coffHeaderRelocCount(numPdataRelocs),
                               pdataHeaderChars);
        }
        if (hasDebugLine) {
            const std::string debugSecName = "/" + std::to_string(debugLineStrOff);
            if (!validateSectionHeaderName(debugSecName, ".debug_line", err))
                return false;
            const uint32_t debugChars =
                kImageScnCntInitData | kImageScnMemDiscardable | kImageScnMemRead | kImageScnAlign1;
            writeSectionHeader(
                file, debugSecName, 0, 0, debugLineDataSize, debugLineDataOff, 0, 0, debugChars);
        }

        padTo(file, textDataOff);
        file.insert(file.end(), patchedTextBytes.begin(), patchedTextBytes.end());

        if (!textRelocBytes.empty()) {
            padTo(file, textRelocOff);
            file.insert(file.end(), textRelocBytes.begin(), textRelocBytes.end());
        }
        if (hasRodata) {
            padTo(file, rdataDataOff);
            file.insert(file.end(), patchedRodataBytes.begin(), patchedRodataBytes.end());
            if (!rdataRelocBytes.empty()) {
                padTo(file, rdataRelocOff);
                file.insert(file.end(), rdataRelocBytes.begin(), rdataRelocBytes.end());
            }
        }
        if (hasData) {
            padTo(file, dataDataOff);
            file.insert(file.end(), data.bytes().begin(), data.bytes().end());
        }
        if (hasXdata) {
            padTo(file, xdataDataOff);
            file.insert(file.end(), xdataBytes.begin(), xdataBytes.end());
        }
        if (hasPdata) {
            padTo(file, pdataDataOff);
            file.insert(file.end(), pdataBytes.begin(), pdataBytes.end());
            if (!pdataRelocBytes.empty()) {
                padTo(file, pdataRelocOff);
                file.insert(file.end(), pdataRelocBytes.begin(), pdataRelocBytes.end());
            }
        }
        if (hasDebugLine) {
            padTo(file, debugLineDataOff);
            file.insert(file.end(), debugLineData_.begin(), debugLineData_.end());
        }

        padTo(file, symtabOff);
        file.insert(file.end(), symtabBytes.begin(), symtabBytes.end());
        file.insert(file.end(), strtabBytes.begin(), strtabBytes.end());

        return commitOutput(path, file, "CoffWriter", err);
    } catch (const std::exception &ex) {
        err << "CoffWriter: " << ex.what() << "\n";
        return false;
    }
}

/// @brief Derive a unique per-function COFF text-section name.
/// @details Uses the first global text definition when available and falls back
///          to a deterministic index-based name for anonymous sections.
/// @param text Function-level code section to inspect.
/// @param index Fallback ordinal within the multi-section write request.
/// @return A `.text.<function>` section name.
static std::string inferTextSectionName(const CodeSection &text, size_t index) {
    std::string funcName = "func_" + std::to_string(index);
    for (uint32_t i = 1; i < text.symbols().count(); ++i) {
        const Symbol &sym = text.symbols().at(i);
        if (sym.binding == SymbolBinding::Global && sym.section == SymbolSection::Text) {
            funcName = sym.name;
            break;
        }
    }
    return ".text." + funcName;
}

/// @copydoc CoffWriter::write(const std::string&, const std::vector<CodeSection>&,
///                            const CodeSection&, std::ostream&)
bool CoffWriter::write(const std::string &path,
                       const std::vector<CodeSection> &textSections,
                       const CodeSection &rodata,
                       std::ostream &err) {
    try {
        if (textSections.size() <= 1) {
            if (textSections.empty()) {
                CodeSection empty;
                return write(path, empty, rodata, err);
            }
            return write(path, textSections[0], rodata, err);
        }

        std::vector<uint8_t> xdataBytes;
        std::vector<PendingCoffSymbol> xdataSymbols;
        std::vector<uint8_t> pdataBytes;
        std::vector<PendingCoffReloc> pdataRelocs;
        {
            uint32_t xdataNameBase = 0;
            for (const auto &text : textSections) {
                if (!collectCoffUnwindForSection(text,
                                                 arch_,
                                                 xdataNameBase,
                                                 xdataBytes,
                                                 xdataSymbols,
                                                 pdataBytes,
                                                 pdataRelocs,
                                                 err))
                    return false;
            }
        }

        // Writable .data section for scalar globals (coalesces text undefined refs).
        const CodeSection emptyData;
        const CodeSection &data = dataSection_ ? *dataSection_ : emptyData;
        const bool hasData = !data.bytes().empty();

        const bool hasRodata = !rodata.empty();
        const bool hasXdata = !xdataBytes.empty();
        const bool hasPdata = !pdataBytes.empty();
        const bool hasDebugLine = !debugLineData_.empty();

        const size_t textCount = textSections.size();
        std::vector<uint16_t> secIdxText(textCount, 0);
        uint32_t nextSecIndex = 1;
        if (textCount > static_cast<size_t>(std::numeric_limits<int16_t>::max())) {
            err << "CoffWriter: text section count exceeds standard COFF symbol section-number "
                   "range; "
                   "BigObj output is not available\n";
            return false;
        }
        for (size_t i = 0; i < textCount; ++i)
            secIdxText[i] = static_cast<uint16_t>(nextSecIndex++);
        const uint16_t secIdxRdata = hasRodata ? static_cast<uint16_t>(nextSecIndex++) : 0;
        const uint16_t secIdxData = hasData ? static_cast<uint16_t>(nextSecIndex++) : 0;
        const uint16_t secIdxXdata = hasXdata ? static_cast<uint16_t>(nextSecIndex++) : 0;
        if (hasPdata)
            ++nextSecIndex;
        const uint32_t sectionCount = (nextSecIndex - 1) + (hasDebugLine ? 1u : 0u);
        if (sectionCount > static_cast<uint32_t>(std::numeric_limits<int16_t>::max())) {
            err << "CoffWriter: section count exceeds standard COFF symbol section-number range; "
                   "BigObj output is not available\n";
            return false;
        }
        const uint16_t numSections = static_cast<uint16_t>(sectionCount);

        std::vector<uint8_t> symtabBytes;
        std::vector<uint8_t> strtabBytes(4, 0);
        std::vector<std::unordered_map<uint32_t, uint32_t>> textSymMaps(textCount);
        std::unordered_map<uint32_t, uint32_t> rodataSymMap;
        std::unordered_map<std::string, uint32_t> definedNameMap;
        std::unordered_map<std::string, uint32_t> definedGlobalNameMap;
        std::unordered_map<std::string, uint32_t> externalNameMap;
        std::unordered_set<std::string> definedGlobalNames;
        uint32_t coffSymCount = 0;

        /// Append a NUL-terminated long name to the COFF string table and return
        /// its checked byte offset.
        auto addToStrTab = [&](const std::string &s) -> uint32_t {
            if (strtabBytes.size() > std::numeric_limits<uint32_t>::max())
                throw std::length_error(
                    "CoffWriter: string table offset exceeds 32-bit COFF limit");
            if (s.size() > std::numeric_limits<size_t>::max() - strtabBytes.size() - 1 ||
                strtabBytes.size() + s.size() + 1 > std::numeric_limits<uint32_t>::max()) {
                throw std::length_error("CoffWriter: string table size exceeds 32-bit COFF limit");
            }
            const uint32_t offset = static_cast<uint32_t>(strtabBytes.size());
            strtabBytes.insert(strtabBytes.end(), s.begin(), s.end());
            strtabBytes.push_back(0);
            return offset;
        };

        /// Encode a long section name as COFF's slash-prefixed decimal string
        /// table offset while leaving short names inline.
        auto encodeSectionHeaderName = [&](const std::string &name) -> std::string {
            if (name.size() <= 8)
                return name;
            const std::string encoded = "/" + std::to_string(addToStrTab(name));
            return encoded;
        };

        /// Deferred undefined symbol whose index may coalesce with a definition
        /// from rodata, data, or any emitted function section.
        struct PendingExternal {
            ///< Whether the original symbol belongs to a function text section.
            bool fromText = false;
            ///< Function-section index when @ref fromText is true.
            size_t textIdx = SIZE_MAX;
            ///< Original CodeSection symbol-table index.
            uint32_t origIdx = 0;
            ///< Name used for definition coalescing and external interning.
            std::string name;
            ///< COFF symbol type.
            uint16_t type = 0;
        };

        std::vector<PendingExternal> pendingExternals;

        std::vector<std::string> textSectionNames(textCount);
        std::vector<std::string> textHeaderNames(textCount);
        for (size_t i = 0; i < textCount; ++i) {
            textSectionNames[i] = inferTextSectionName(textSections[i], i);
            textHeaderNames[i] = encodeSectionHeaderName(textSectionNames[i]);
            if (!validateSectionHeaderName(textHeaderNames[i], textSectionNames[i].c_str(), err))
                return false;
        }
        std::string debugHeaderName;
        if (hasDebugLine) {
            debugHeaderName = encodeSectionHeaderName(".debug_line");
            if (!validateSectionHeaderName(debugHeaderName, ".debug_line", err))
                return false;
        }

        /// Determine whether a section contains an exact section-offset
        /// relocation requiring a synthetic anchor for @p targetSection.
        auto relocationNeedsAnchor = [](const CodeSection &sec, SymbolSection targetSection) {
            for (const auto &rel : sec.relocations())
                if (rel.targetOffsetValid && rel.targetSection == targetSection)
                    return true;
            return false;
        };
        std::vector<uint32_t> textAnchorIdx(textCount, UINT32_MAX);
        bool needAnyTextAnchor = relocationNeedsAnchor(rodata, SymbolSection::Text);
        for (size_t ti = 0; ti < textCount; ++ti)
            needAnyTextAnchor =
                needAnyTextAnchor || relocationNeedsAnchor(textSections[ti], SymbolSection::Text);
        if (needAnyTextAnchor) {
            for (size_t ti = 0; ti < textCount; ++ti) {
                const uint32_t strOff =
                    (textSectionNames[ti].size() > 8) ? addToStrTab(textSectionNames[ti]) : 0;
                writeSymbol(symtabBytes,
                            textSectionNames[ti],
                            strOff,
                            0,
                            static_cast<int16_t>(secIdxText[ti]),
                            0,
                            kImageSymClassStatic);
                textAnchorIdx[ti] = coffSymCount++;
            }
        }

        uint32_t rodataAnchorIdx = UINT32_MAX;
        const bool needRodataAnchor =
            hasRodata &&
            (relocationNeedsAnchor(rodata, SymbolSection::Rodata) ||
             /// Scan every function section for an exact rodata-offset target.
             std::any_of(textSections.begin(), textSections.end(), [&](const CodeSection &sec) {
                 return relocationNeedsAnchor(sec, SymbolSection::Rodata);
             }));
        if (needRodataAnchor) {
            writeSymbol(symtabBytes,
                        ".rdata",
                        0,
                        0,
                        static_cast<int16_t>(secIdxRdata),
                        0,
                        kImageSymClassStatic);
            rodataAnchorIdx = coffSymCount++;
        }

        if (hasRodata) {
            for (uint32_t i = 1; i < rodata.symbols().count(); ++i) {
                const Symbol &s = rodata.symbols().at(i);
                if (s.binding == SymbolBinding::External) {
                    pendingExternals.push_back({false, SIZE_MAX, i, s.name, 0});
                    continue;
                }

                const int16_t secNum = static_cast<int16_t>(secIdxRdata);
                uint32_t value = 0;
                if (!checkedPhysicalSymbolValue(rodata, s, ".rdata", err, value))
                    return false;
                const uint8_t storageClass = (s.binding == SymbolBinding::Local)
                                                 ? kImageSymClassStatic
                                                 : kImageSymClassExternal;
                if (!rememberDefinedGlobal(definedGlobalNames, s, ".rdata", err))
                    return false;
                const uint32_t strOff = (s.name.size() > 8) ? addToStrTab(s.name) : 0;

                writeSymbol(symtabBytes, s.name, strOff, value, secNum, 0, storageClass);
                const uint32_t coffIdx = coffSymCount++;
                rodataSymMap[i] = coffIdx;
                definedNameMap[s.name] = coffIdx;
                if (s.binding == SymbolBinding::Global)
                    definedGlobalNameMap[s.name] = coffIdx;
            }
        }

        std::unordered_map<std::string, uint32_t> definedRodataByName;
        if (hasRodata) {
            for (uint32_t i = 1; i < rodata.symbols().count(); ++i) {
                const Symbol &s = rodata.symbols().at(i);
                if (s.binding == SymbolBinding::External)
                    continue;
                auto it = rodataSymMap.find(i);
                if (it != rodataSymMap.end()) {
                    auto [nameIt, inserted] = definedRodataByName.emplace(s.name, it->second);
                    if (!inserted)
                        nameIt->second = UINT32_MAX;
                }
            }
        }

        if (hasXdata) {
            for (const auto &sym : xdataSymbols) {
                const uint32_t strOff = (sym.name.size() > 8) ? addToStrTab(sym.name) : 0;
                writeSymbol(symtabBytes,
                            sym.name,
                            strOff,
                            sym.value,
                            static_cast<int16_t>(secIdxXdata),
                            sym.type,
                            sym.storageClass);
                definedNameMap[sym.name] = coffSymCount++;
            }
        }

        std::vector<std::unordered_set<uint32_t>> crossSectionTextAliases(textCount);
        std::vector<std::unordered_set<uint32_t>> undefinedTextRefs(textCount);
        for (size_t ti = 0; ti < textCount; ++ti) {
            const auto &text = textSections[ti];
            for (const auto &rel : text.relocations()) {
                if (rel.symbolIndex >= text.symbols().count())
                    continue;
                if (rel.targetSection != SymbolSection::Undefined)
                    crossSectionTextAliases[ti].insert(rel.symbolIndex);
                else
                    undefinedTextRefs[ti].insert(rel.symbolIndex);
            }
        }

        for (size_t ti = 0; ti < textCount; ++ti) {
            const auto &text = textSections[ti];
            for (uint32_t i = 1; i < text.symbols().count(); ++i) {
                const Symbol &s = text.symbols().at(i);
                if (s.binding == SymbolBinding::External) {
                    if (crossSectionTextAliases[ti].count(i) != 0 &&
                        undefinedTextRefs[ti].count(i) == 0) {
                        continue;
                    }
                    pendingExternals.push_back({true, ti, i, s.name, 0x20});
                    continue;
                }

                const int16_t secNum = static_cast<int16_t>(secIdxText[ti]);
                uint32_t value = 0;
                if (!checkedPhysicalSymbolValue(text, s, textSectionNames[ti].c_str(), err, value))
                    return false;
                const uint8_t storageClass = (s.binding == SymbolBinding::Local)
                                                 ? kImageSymClassStatic
                                                 : kImageSymClassExternal;
                if (!rememberDefinedGlobal(
                        definedGlobalNames, s, textSectionNames[ti].c_str(), err))
                    return false;
                const uint32_t strOff = (s.name.size() > 8) ? addToStrTab(s.name) : 0;

                writeSymbol(symtabBytes, s.name, strOff, value, secNum, 0x20, storageClass);
                const uint32_t coffIdx = coffSymCount++;
                textSymMaps[ti][i] = coffIdx;
                definedNameMap[s.name] = coffIdx;
                if (s.binding == SymbolBinding::Global)
                    definedGlobalNameMap[s.name] = coffIdx;
            }
        }

        // Writable scalar globals in .data — defined symbols coalescing text refs.
        if (hasData) {
            for (uint32_t i = 1; i < data.symbols().count(); ++i) {
                const Symbol &s = data.symbols().at(i);
                if (s.binding == SymbolBinding::External)
                    continue; // a .data symbol is always a definition
                uint32_t value = 0;
                if (!checkedPhysicalSymbolValue(data, s, ".data", err, value))
                    return false;
                const uint8_t storageClass = (s.binding == SymbolBinding::Local)
                                                 ? kImageSymClassStatic
                                                 : kImageSymClassExternal;
                if (s.binding == SymbolBinding::Global &&
                    !rememberDefinedGlobal(definedGlobalNames, s, ".data", err))
                    return false;
                const uint32_t strOff = (s.name.size() > 8) ? addToStrTab(s.name) : 0;
                writeSymbol(symtabBytes,
                            s.name,
                            strOff,
                            value,
                            static_cast<int16_t>(secIdxData),
                            0,
                            storageClass);
                const uint32_t coffIdx = coffSymCount++;
                definedNameMap[s.name] = coffIdx;
                if (s.binding == SymbolBinding::Global)
                    definedGlobalNameMap[s.name] = coffIdx;
            }
        }

        std::unordered_map<std::string, uint32_t> definedTextByName;
        for (size_t ti = 0; ti < textCount; ++ti) {
            const auto &text = textSections[ti];
            for (uint32_t i = 1; i < text.symbols().count(); ++i) {
                const Symbol &s = text.symbols().at(i);
                if (s.binding == SymbolBinding::External)
                    continue;
                auto it = textSymMaps[ti].find(i);
                if (it == textSymMaps[ti].end())
                    continue;
                auto [nameIt, inserted] = definedTextByName.emplace(s.name, it->second);
                if (!inserted)
                    nameIt->second = UINT32_MAX;
            }
        }

        for (const auto &ext : pendingExternals) {
            const auto definedIt = definedGlobalNameMap.find(ext.name);
            if (definedIt != definedGlobalNameMap.end()) {
                if (ext.fromText)
                    textSymMaps[ext.textIdx][ext.origIdx] = definedIt->second;
                else
                    rodataSymMap[ext.origIdx] = definedIt->second;
                continue;
            }

            auto extIt = externalNameMap.find(ext.name);
            if (extIt == externalNameMap.end()) {
                const uint32_t strOff = (ext.name.size() > 8) ? addToStrTab(ext.name) : 0;
                writeSymbol(symtabBytes,
                            ext.name,
                            strOff,
                            0,
                            kImageSymUndefined,
                            ext.type,
                            kImageSymClassExternal);
                extIt = externalNameMap.emplace(ext.name, coffSymCount++).first;
            }

            if (ext.fromText)
                textSymMaps[ext.textIdx][ext.origIdx] = extIt->second;
            else
                rodataSymMap[ext.origIdx] = extIt->second;
        }

        uint32_t strtabSize = 0;
        if (!checkedU32(strtabBytes.size(), "string table size", err, strtabSize))
            return false;
        strtabBytes[0] = static_cast<uint8_t>(strtabSize);
        strtabBytes[1] = static_cast<uint8_t>(strtabSize >> 8);
        strtabBytes[2] = static_cast<uint8_t>(strtabSize >> 16);
        strtabBytes[3] = static_cast<uint8_t>(strtabSize >> 24);

        std::vector<std::vector<uint8_t>> patchedTextBytes(textCount);
        for (size_t ti = 0; ti < textCount; ++ti)
            patchedTextBytes[ti] = textSections[ti].bytes();
        std::vector<uint8_t> patchedRodataBytes = rodata.bytes();
        std::vector<std::vector<uint8_t>> textRelocBytes(textCount);
        std::vector<uint32_t> numTextRelocs(textCount, 0);

        /// Resolve one encoder relocation to a final COFF symbol and effective
        /// addend, using exact section identities to disambiguate text targets.
        auto resolveRelocSym = [&](const Relocation &rel,
                                   const CodeSection &source,
                                   const std::unordered_map<uint32_t, uint32_t> &sourceMap,
                                   const char *sectionName,
                                   size_t sourceTextIndex,
                                   uint32_t &coffSymIdx,
                                   int64_t &effectiveAddend) -> bool {
            coffSymIdx = 0;
            effectiveAddend = rel.addend;
            if (rel.targetSection != SymbolSection::Undefined) {
                if (rel.targetOffsetValid) {
                    if (!checkedSectionOffsetAddend(rel.addend,
                                                    rel.targetOffset,
                                                    "CoffWriter",
                                                    sectionName,
                                                    rel.offset,
                                                    err,
                                                    effectiveAddend))
                        return false;
                    if (rel.targetSection == SymbolSection::Rodata) {
                        if (rodataAnchorIdx == UINT32_MAX) {
                            err << "CoffWriter: missing section anchor for .rdata\n";
                            return false;
                        }
                        if (rel.targetOffset > rodata.bytes().size()) {
                            err << "CoffWriter: relocation in " << sectionName << " at offset "
                                << rel.offset << " references .rdata offset " << rel.targetOffset
                                << " beyond section contents\n";
                            return false;
                        }
                        coffSymIdx = rodataAnchorIdx;
                        return true;
                    }

                    size_t textIdx = sourceTextIndex;
                    if (rel.targetSectionIdentityValid) {
                        textIdx = SIZE_MAX;
                        size_t matches = 0;
                        for (size_t ti = 0; ti < textCount; ++ti) {
                            if (textSections[ti].matchesSectionIdentity(
                                    rel.targetSectionIdentity)) {
                                textIdx = ti;
                                ++matches;
                            }
                        }
                        if (matches > 1) {
                            err << "CoffWriter: relocation in " << sectionName << " at offset "
                                << rel.offset << " references duplicate .text section identity\n";
                            return false;
                        }
                    } else if (textIdx == SIZE_MAX || textIdx >= textCount ||
                               rel.targetOffset > textSections[textIdx].bytes().size()) {
                        textIdx = SIZE_MAX;
                        size_t matches = 0;
                        for (size_t ti = 0; ti < textCount; ++ti) {
                            if (rel.targetOffset <= textSections[ti].bytes().size()) {
                                textIdx = ti;
                                ++matches;
                            }
                        }
                        if (matches > 1) {
                            err << "CoffWriter: relocation in " << sectionName << " at offset "
                                << rel.offset << " references ambiguous .text offset "
                                << rel.targetOffset
                                << "; use section-identity relocation overload\n";
                            return false;
                        }
                    }
                    if (textIdx == SIZE_MAX || textIdx >= textCount ||
                        rel.targetOffset > textSections[textIdx].bytes().size()) {
                        err << "CoffWriter: relocation in " << sectionName << " at offset "
                            << rel.offset << " references .text offset " << rel.targetOffset
                            << " beyond section contents\n";
                        return false;
                    }
                    if (textAnchorIdx[textIdx] == UINT32_MAX) {
                        err << "CoffWriter: missing section anchor for "
                            << textSectionNames[textIdx] << "\n";
                        return false;
                    }
                    coffSymIdx = textAnchorIdx[textIdx];
                    return true;
                }
                if (rel.symbolIndex >= source.symbols().count()) {
                    err << "CoffWriter: relocation in " << sectionName << " at offset "
                        << rel.offset << " references unknown symbol index " << rel.symbolIndex
                        << "\n";
                    return false;
                }
                const Symbol &sym = source.symbols().at(rel.symbolIndex);
                const auto &targetByName = (rel.targetSection == SymbolSection::Rodata)
                                               ? definedRodataByName
                                               : definedTextByName;
                auto nameIt = targetByName.find(sym.name);
                if (nameIt == targetByName.end()) {
                    err << "CoffWriter: relocation in " << sectionName << " at offset "
                        << rel.offset << " references missing cross-section target '" << sym.name
                        << "'\n";
                    return false;
                }
                if (nameIt->second == UINT32_MAX) {
                    err << "CoffWriter: relocation in " << sectionName << " at offset "
                        << rel.offset << " references ambiguous cross-section target '" << sym.name
                        << "'\n";
                    return false;
                }
                coffSymIdx = nameIt->second;
                return true;
            }

            auto it = sourceMap.find(rel.symbolIndex);
            if (it == sourceMap.end()) {
                err << "CoffWriter: relocation in " << sectionName << " at offset " << rel.offset
                    << " references unknown symbol index " << rel.symbolIndex << "\n";
                return false;
            }
            coffSymIdx = it->second;
            return true;
        };

        /// Validate, addend-patch, and serialize all relocations from one
        /// function or rodata section.
        auto appendRelocBytes = [&](const CodeSection &source,
                                    const std::unordered_map<uint32_t, uint32_t> &sourceMap,
                                    const char *sectionName,
                                    size_t sourceTextIndex,
                                    std::vector<uint8_t> &patchedBytes,
                                    std::vector<uint8_t> &relocBytes) -> bool {
            for (const auto &rel : source.relocations()) {
                if (!validateRelocationShape("CoffWriter", arch_, source, rel, sectionName, err))
                    return false;
                uint32_t coffSymIdx = 0;
                int64_t effectiveAddend = rel.addend;
                if (!resolveRelocSym(rel,
                                     source,
                                     sourceMap,
                                     sectionName,
                                     sourceTextIndex,
                                     coffSymIdx,
                                     effectiveAddend))
                    return false;
                if (!patchCoffRelocationAddend(source, rel, effectiveAddend, patchedBytes, err))
                    return false;
                const size_t physicalRelOffset = rel.offset - source.logicalOffsetBias();
                uint32_t relocOff = 0;
                if (!checkedU32(physicalRelOffset, "relocation offset", err, relocOff))
                    return false;
                writeReloc(relocBytes, relocOff, coffSymIdx, coffRelocType(rel.kind, arch_));
            }
            return true;
        };

        for (size_t ti = 0; ti < textCount; ++ti) {
            const auto &text = textSections[ti];
            auto &relocBytes = textRelocBytes[ti];
            if (!appendRelocBytes(
                    text, textSymMaps[ti], ".text", ti, patchedTextBytes[ti], relocBytes))
                return false;
            if (!checkedU32(
                    text.relocations().size(), "text relocation count", err, numTextRelocs[ti]))
                return false;
            addRelocationOverflowRecord(relocBytes, numTextRelocs[ti]);
        }

        std::vector<uint8_t> rdataRelocBytes;
        if (!appendRelocBytes(
                rodata, rodataSymMap, ".rdata", SIZE_MAX, patchedRodataBytes, rdataRelocBytes))
            return false;
        uint32_t numRdataRelocs = 0;
        if (!checkedU32(
                rodata.relocations().size(), ".rdata relocation count", err, numRdataRelocs))
            return false;
        addRelocationOverflowRecord(rdataRelocBytes, numRdataRelocs);

        std::vector<uint8_t> pdataRelocBytes;
        for (const auto &rel : pdataRelocs) {
            if (!validatePdataRelocOffset(rel.offset, pdataBytes.size(), err))
                return false;
            uint32_t targetIdx = 0;
            if (rel.hasSymbolRef) {
                size_t textIdx = SIZE_MAX;
                size_t matches = 0;
                for (size_t ti = 0; ti < textCount; ++ti) {
                    if (textSections[ti].matchesSectionIdentity(rel.symbolSectionIdentity)) {
                        textIdx = ti;
                        ++matches;
                    }
                }
                if (matches > 1) {
                    err << "CoffWriter: .pdata relocation for '" << rel.symbolName
                        << "' references duplicate text section identity\n";
                    return false;
                }
                if (textIdx == SIZE_MAX) {
                    err << "CoffWriter: .pdata relocation for '" << rel.symbolName
                        << "' references missing text section identity\n";
                    return false;
                }
                auto symIt = textSymMaps[textIdx].find(rel.symbolIndex);
                if (symIt == textSymMaps[textIdx].end()) {
                    err << "CoffWriter: missing symbol '" << rel.symbolName
                        << "' for .pdata relocation\n";
                    return false;
                }
                targetIdx = symIt->second;
            } else {
                auto it = definedNameMap.find(rel.symbolName);
                if (it == definedNameMap.end()) {
                    err << "CoffWriter: missing symbol '" << rel.symbolName
                        << "' for .pdata relocation\n";
                    return false;
                }
                targetIdx = it->second;
            }
            writeReloc(pdataRelocBytes, rel.offset, targetIdx, rel.type);
        }
        uint32_t numPdataRelocs = 0;
        if (!checkedU32(pdataRelocs.size(), ".pdata relocation count", err, numPdataRelocs))
            return false;
        addRelocationOverflowRecord(pdataRelocBytes, numPdataRelocs);

        std::vector<uint32_t> textSizes(textCount, 0);
        for (size_t ti = 0; ti < textCount; ++ti) {
            if (!checkedU32(patchedTextBytes[ti].size(), ".text size", err, textSizes[ti]))
                return false;
        }
        uint32_t rdataSize = 0;
        uint32_t dataSize = 0;
        uint32_t xdataSize = 0;
        uint32_t pdataSize = 0;
        uint32_t debugLineDataSize = 0;
        if (!checkedU32(hasRodata ? patchedRodataBytes.size() : 0, ".rdata size", err, rdataSize) ||
            !checkedU32(hasData ? data.bytes().size() : 0, ".data size", err, dataSize) ||
            !checkedU32(hasXdata ? xdataBytes.size() : 0, ".xdata size", err, xdataSize) ||
            !checkedU32(hasPdata ? pdataBytes.size() : 0, ".pdata size", err, pdataSize) ||
            !checkedU32(hasDebugLine ? debugLineData_.size() : 0,
                        ".debug_line size",
                        err,
                        debugLineDataSize))
            return false;

        const uint32_t headerAreaSize = kCoffHeaderSize + numSections * kSectionHeaderSize;
        uint32_t cursor = 0;
        if (!alignU32Checked(headerAreaSize, 4, "header area", err, cursor))
            return false;

        std::vector<uint32_t> textDataOff(textCount, 0);
        std::vector<uint32_t> textRelocOff(textCount, 0);
        for (size_t ti = 0; ti < textCount; ++ti) {
            textDataOff[ti] = cursor;
            if (!addU32Checked(cursor, textSizes[ti], ".text data", err, cursor))
                return false;
            if (numTextRelocs[ti] > 0) {
                textRelocOff[ti] = cursor;
                uint32_t textRelocSize = 0;
                if (!coffRelocTableSize(numTextRelocs[ti], ".text", err, textRelocSize))
                    return false;
                if (!addU32Checked(cursor, textRelocSize, ".text relocation table", err, cursor))
                    return false;
            }
            if (!alignU32Checked(cursor, 4, ".text data", err, cursor))
                return false;
        }

        uint32_t rdataDataOff = 0;
        uint32_t rdataRelocOff = 0;
        if (hasRodata) {
            rdataDataOff = cursor;
            if (!addU32Checked(
                    rdataDataOff, rdataSize, ".rdata relocation offset", err, rdataRelocOff))
                return false;
            uint32_t rdataRelocEnd = 0;
            uint32_t rdataRelocSize = 0;
            if (!coffRelocTableSize(numRdataRelocs, ".rdata", err, rdataRelocSize))
                return false;
            if (!addU32Checked(
                    rdataRelocOff, rdataRelocSize, ".rdata relocation table", err, rdataRelocEnd))
                return false;
            if (!alignU32Checked(rdataRelocEnd, 4, ".rdata relocation table", err, cursor))
                return false;
        }
        uint32_t dataDataOff = 0;
        if (hasData) {
            dataDataOff = cursor;
            uint32_t end = 0;
            if (!addU32Checked(dataDataOff, dataSize, ".data data", err, end))
                return false;
            if (!alignU32Checked(end, 4, ".data data", err, cursor))
                return false;
        }
        uint32_t xdataDataOff = 0;
        if (hasXdata) {
            xdataDataOff = cursor;
            uint32_t end = 0;
            if (!addU32Checked(xdataDataOff, xdataSize, ".xdata data", err, end))
                return false;
            if (!alignU32Checked(end, 4, ".xdata data", err, cursor))
                return false;
        }
        uint32_t pdataDataOff = 0;
        uint32_t pdataRelocOff = 0;
        if (hasPdata) {
            pdataDataOff = cursor;
            if (!addU32Checked(
                    pdataDataOff, pdataSize, ".pdata relocation offset", err, pdataRelocOff))
                return false;
            uint32_t pdataRelocEnd = 0;
            uint32_t pdataRelocSize = 0;
            if (!coffRelocTableSize(numPdataRelocs, ".pdata", err, pdataRelocSize))
                return false;
            if (!addU32Checked(
                    pdataRelocOff, pdataRelocSize, ".pdata relocation table", err, pdataRelocEnd))
                return false;
            if (!alignU32Checked(pdataRelocEnd, 4, ".pdata relocation table", err, cursor))
                return false;
        }
        uint32_t debugLineDataOff = 0;
        if (hasDebugLine) {
            debugLineDataOff = cursor;
            uint32_t end = 0;
            if (!addU32Checked(debugLineDataOff, debugLineDataSize, ".debug_line data", err, end))
                return false;
            if (!alignU32Checked(end, 4, ".debug_line data", err, cursor))
                return false;
        }
        const uint32_t symtabOff = cursor;

        std::vector<uint8_t> file;
        size_t reserveSize = 0;
        if (!checkedCoffReserveSize(
                symtabOff, symtabBytes.size(), strtabBytes.size(), err, reserveSize))
            return false;
        file.reserve(reserveSize);

        const uint16_t machine = (arch_ == ObjArch::X86_64) ? kMachineAMD64 : kMachineARM64;
        appendLE16(file, machine);
        appendLE16(file, numSections);
        appendLE32(file, 0);
        appendLE32(file, symtabOff);
        appendLE32(file, coffSymCount);
        appendLE16(file, 0);
        appendLE16(file, 0);

        for (size_t ti = 0; ti < textCount; ++ti) {
            uint32_t textChars = kImageScnCntCode | kImageScnMemExecute | kImageScnMemRead;
            textChars |= (arch_ == ObjArch::X86_64) ? kImageScnAlignText : kImageScnAlign4;
            if (numTextRelocs[ti] > kCoffMaxStandardRelocs)
                textChars |= kImageScnLnkNrelocOvfl;
            writeSectionHeader(file,
                               textHeaderNames[ti].c_str(),
                               0,
                               0,
                               textSizes[ti],
                               textDataOff[ti],
                               (numTextRelocs[ti] > 0) ? textRelocOff[ti] : 0,
                               coffHeaderRelocCount(numTextRelocs[ti]),
                               textChars);
        }

        if (hasRodata) {
            uint32_t rdataChars = kImageScnCntInitData | kImageScnMemRead | kImageScnAlign8;
            if (numRdataRelocs > kCoffMaxStandardRelocs)
                rdataChars |= kImageScnLnkNrelocOvfl;
            writeSectionHeader(file,
                               ".rdata",
                               0,
                               0,
                               rdataSize,
                               rdataDataOff,
                               (numRdataRelocs > 0) ? rdataRelocOff : 0,
                               coffHeaderRelocCount(numRdataRelocs),
                               rdataChars);
        }
        if (hasData) {
            const uint32_t dataChars =
                kImageScnCntInitData | kImageScnMemRead | kImageScnMemWrite | kImageScnAlign8;
            writeSectionHeader(file, ".data", 0, 0, dataSize, dataDataOff, 0, 0, dataChars);
        }
        if (hasXdata) {
            const uint32_t xdataChars = kImageScnCntInitData | kImageScnMemRead | kImageScnAlign4;
            writeSectionHeader(file, ".xdata", 0, 0, xdataSize, xdataDataOff, 0, 0, xdataChars);
        }
        if (hasPdata) {
            const uint32_t pdataChars = kImageScnCntInitData | kImageScnMemRead | kImageScnAlign4;
            uint32_t pdataHeaderChars = pdataChars;
            if (numPdataRelocs > kCoffMaxStandardRelocs)
                pdataHeaderChars |= kImageScnLnkNrelocOvfl;
            writeSectionHeader(file,
                               ".pdata",
                               0,
                               0,
                               pdataSize,
                               pdataDataOff,
                               (numPdataRelocs > 0) ? pdataRelocOff : 0,
                               coffHeaderRelocCount(numPdataRelocs),
                               pdataHeaderChars);
        }
        if (hasDebugLine) {
            const uint32_t debugChars =
                kImageScnCntInitData | kImageScnMemDiscardable | kImageScnMemRead | kImageScnAlign1;
            writeSectionHeader(
                file, debugHeaderName, 0, 0, debugLineDataSize, debugLineDataOff, 0, 0, debugChars);
        }

        for (size_t ti = 0; ti < textCount; ++ti) {
            padTo(file, textDataOff[ti]);
            file.insert(file.end(), patchedTextBytes[ti].begin(), patchedTextBytes[ti].end());
            if (!textRelocBytes[ti].empty()) {
                padTo(file, textRelocOff[ti]);
                file.insert(file.end(), textRelocBytes[ti].begin(), textRelocBytes[ti].end());
            }
        }

        if (hasRodata) {
            padTo(file, rdataDataOff);
            file.insert(file.end(), patchedRodataBytes.begin(), patchedRodataBytes.end());
            if (!rdataRelocBytes.empty()) {
                padTo(file, rdataRelocOff);
                file.insert(file.end(), rdataRelocBytes.begin(), rdataRelocBytes.end());
            }
        }
        if (hasData) {
            padTo(file, dataDataOff);
            file.insert(file.end(), data.bytes().begin(), data.bytes().end());
        }
        if (hasXdata) {
            padTo(file, xdataDataOff);
            file.insert(file.end(), xdataBytes.begin(), xdataBytes.end());
        }
        if (hasPdata) {
            padTo(file, pdataDataOff);
            file.insert(file.end(), pdataBytes.begin(), pdataBytes.end());
            if (!pdataRelocBytes.empty()) {
                padTo(file, pdataRelocOff);
                file.insert(file.end(), pdataRelocBytes.begin(), pdataRelocBytes.end());
            }
        }
        if (hasDebugLine) {
            padTo(file, debugLineDataOff);
            file.insert(file.end(), debugLineData_.begin(), debugLineData_.end());
        }

        padTo(file, symtabOff);
        file.insert(file.end(), symtabBytes.begin(), symtabBytes.end());
        file.insert(file.end(), strtabBytes.begin(), strtabBytes.end());

        return commitOutput(path, file, "CoffWriter", err);
    } catch (const std::exception &ex) {
        err << "CoffWriter: " << ex.what() << "\n";
        return false;
    }
}

} // namespace zanna::codegen::objfile
