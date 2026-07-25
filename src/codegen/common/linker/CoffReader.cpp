//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/CoffReader.cpp
// Purpose: COFF/PE object file reader for the native linker.
// Key invariants:
//   - Machine field at offset 0: 0x8664 (AMD64), 0xAA64 (ARM64)
//   - COFF relocations are 10 bytes: offset(4) + symIndex(4) + type(2)
//   - No explicit addend — extracted from instruction bytes at reloc site
//   - Symbol names ≤8 chars inline; longer names use string table offset
// Links: codegen/common/linker/ObjFileReader.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file CoffReader.cpp
 * @brief Implements validated parsing of standard and BigObj COFF object files.
 *
 * The reader translates Microsoft x64 and ARM64 section, relocation, symbol,
 * weak-external, common, and COMDAT records into the linker's neutral object
 * model.  All offsets and aggregate byte counts are checked before access.
 */

#include "codegen/common/AArch64RelocUtil.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"
#include "codegen/common/linker/RelocConstants.hpp"
#include "codegen/common/objfile/ObjFileWriterUtil.hpp"

#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace zanna::codegen::linker {

using zanna::codegen::objfile::checkedAdd;
using zanna::codegen::objfile::checkedMul;
using zanna::codegen::objfile::checkedRange;
using zanna::codegen::objfile::readLE16;
using zanna::codegen::objfile::readLE32;
using zanna::codegen::objfile::readLE64;

namespace coff {
static constexpr uint16_t IMAGE_FILE_MACHINE_AMD64 = 0x8664;
static constexpr uint16_t IMAGE_FILE_MACHINE_ARM64 = 0xAA64;

static constexpr uint8_t kBigObjClassId[16] = {
    0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA, 0xA9, 0x4B, 0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8};

static constexpr uint16_t IMAGE_SYM_CLASS_EXTERNAL = 2;
static constexpr uint16_t IMAGE_SYM_CLASS_STATIC = 3;
static constexpr uint16_t IMAGE_SYM_CLASS_WEAK_EXTERNAL = 105;
static constexpr int16_t IMAGE_SYM_UNDEFINED = 0;
static constexpr int16_t IMAGE_SYM_ABSOLUTE = -1;
static constexpr int16_t IMAGE_SYM_DEBUG = -2;

static constexpr uint32_t IMAGE_SCN_CNT_CODE = 0x00000020;
static constexpr uint32_t IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040;
static constexpr uint32_t IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080;
static constexpr uint32_t IMAGE_SCN_LNK_COMDAT = 0x00001000;
static constexpr uint32_t IMAGE_SCN_LNK_NRELOC_OVFL = 0x01000000;
static constexpr uint32_t IMAGE_SCN_MEM_EXECUTE = 0x20000000;
[[maybe_unused]] static constexpr uint32_t IMAGE_SCN_MEM_READ = 0x40000000;
static constexpr uint32_t IMAGE_SCN_MEM_WRITE = 0x80000000;

static constexpr uint8_t IMAGE_COMDAT_SELECT_NODUPLICATES = 1;
static constexpr uint8_t IMAGE_COMDAT_SELECT_ANY = 2;
static constexpr uint8_t IMAGE_COMDAT_SELECT_SAME_SIZE = 3;
static constexpr uint8_t IMAGE_COMDAT_SELECT_EXACT_MATCH = 4;
static constexpr uint8_t IMAGE_COMDAT_SELECT_ASSOCIATIVE = 5;
static constexpr uint8_t IMAGE_COMDAT_SELECT_LARGEST = 6;

#pragma pack(push, 1)

/// @brief Packed standard COFF file-header record.
struct CoffHeader {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
};

/// @brief Packed Microsoft BigObj header with widened section counts.
struct BigObjHeader {
    uint16_t Sig1;
    uint16_t Sig2;
    uint16_t Version;
    uint16_t Machine;
    uint32_t TimeDateStamp;
    uint8_t ClassID[16];
    uint32_t SizeOfData;
    uint32_t Flags;
    uint32_t MetaDataSize;
    uint32_t MetaDataOffset;
    uint32_t NumberOfSections;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
};

/// @brief Packed COFF section-header record.
struct SectionHeader {
    char Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
};

/// @brief Packed COFF relocation record.
struct CoffReloc {
    uint32_t VirtualAddress;
    uint32_t SymbolTableIndex;
    uint16_t Type;
};

/// @brief Packed standard COFF symbol-table record.
struct CoffSymbol {
    union {
        char ShortName[8];

        struct {
            uint32_t Zeros;
            uint32_t Offset;
        } LongName;
    } Name;

    uint32_t Value;
    int16_t SectionNumber;
    uint16_t Type;
    uint8_t StorageClass;
    uint8_t NumberOfAuxSymbols;
};

/// @brief Packed BigObj symbol-table record with a 32-bit section number.
struct CoffSymbolEx {
    union {
        char ShortName[8];

        struct {
            uint32_t Zeros;
            uint32_t Offset;
        } LongName;
    } Name;

    uint32_t Value;
    int32_t SectionNumber;
    uint16_t Type;
    uint8_t StorageClass;
    uint8_t NumberOfAuxSymbols;
};

/// @brief Packed auxiliary section-definition record used by COMDAT sections.
struct CoffAuxSectionDefinition {
    uint32_t Length;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t CheckSum;
    int16_t Number;
    uint8_t Selection;
    uint8_t Reserved;
    int16_t HighNumber;
};

/// @brief Packed auxiliary weak-external fallback record.
struct CoffAuxWeakExternal {
    uint32_t TagIndex;
    uint32_t Characteristics;
    uint8_t Unused[10];
};

#pragma pack(pop)
} // namespace coff

/// @brief Copy a NUL-terminated string out of @p data within the @p len-byte window.
/// @param data Base of the containing byte buffer.
/// @param off Offset of the candidate string.
/// @param len Maximum readable bytes beginning at @p off.
/// @param out Receives bytes preceding the terminator.
/// @return `false` if no NUL is found inside the supplied window.
static bool readBoundedString(const uint8_t *data, size_t off, size_t len, std::string &out) {
    const uint8_t *begin = data + off;
    const void *nul = std::memchr(begin, '\0', len);
    if (!nul)
        return false;
    out.assign(reinterpret_cast<const char *>(begin), static_cast<const char *>(nul));
    return true;
}

/// @brief Sign-extend the low @p bits of @p value to a 64-bit signed integer.
/// @details Used to recover signed COFF/AArch64 immediate fields from the raw
///          bit pattern of an instruction word.
/// @param value Unsigned bit pattern containing the encoded immediate.
/// @param bits Width of the signed field.
/// @return The sign-extended field value.
static int64_t signExtend(uint64_t value, unsigned bits) {
    const uint64_t signBit = uint64_t{1} << (bits - 1);
    const uint64_t mask = (uint64_t{1} << bits) - 1;
    value &= mask;
    return static_cast<int64_t>((value ^ signBit) - signBit);
}

/// @brief Bounds-checked typed pointer cast at @p offset within a byte buffer.
/// @tparam T Packed record type to view.
/// @param data Base of the input buffer.
/// @param size Total input-buffer size.
/// @param offset Byte offset of the record.
/// @return Pointer to the in-buffer record, or `nullptr` when reading
///         `sizeof(T)` bytes would exceed @p size.
template <typename T> static const T *coffAt(const uint8_t *data, size_t size, size_t offset) {
    if (offset > size || sizeof(T) > size - offset)
        return nullptr;
    return reinterpret_cast<const T *>(data + offset);
}

/// @brief Normalized view shared by standard and BigObj COFF symbol records.
struct CoffSymbolView {
    char shortName[8]{};
    uint32_t longNameZeros = 0;
    uint32_t longNameOffset = 0;
    uint32_t value = 0;
    int32_t sectionNumber = 0;
    uint16_t type = 0;
    uint8_t storageClass = 0;
    uint8_t auxCount = 0;
};

/// @brief Reads one raw COFF symbol into the format-neutral symbol view.
/// @param data Base of the object-file bytes.
/// @param size Size of the object-file buffer.
/// @param symbolTableOffset Byte offset of the symbol table.
/// @param symbolSize Size of one standard or BigObj symbol record.
/// @param index Raw symbol-table index.
/// @param bigObj Whether records use the BigObj layout.
/// @param out Receives the normalized fields.
/// @return `true` when the complete symbol record is in bounds.
static bool readCoffSymbolView(const uint8_t *data,
                               size_t size,
                               size_t symbolTableOffset,
                               size_t symbolSize,
                               uint32_t index,
                               bool bigObj,
                               CoffSymbolView &out) {
    size_t offset = 0;
    size_t scaled = 0;
    if (!checkedMul(static_cast<size_t>(index), symbolSize, scaled) ||
        !checkedAdd(symbolTableOffset, scaled, offset))
        return false;
    if (bigObj) {
        const auto *sym = coffAt<coff::CoffSymbolEx>(data, size, offset);
        if (!sym)
            return false;
        std::memcpy(out.shortName, sym->Name.ShortName, sizeof(out.shortName));
        out.longNameZeros = sym->Name.LongName.Zeros;
        out.longNameOffset = sym->Name.LongName.Offset;
        out.value = sym->Value;
        out.sectionNumber = sym->SectionNumber;
        out.type = sym->Type;
        out.storageClass = sym->StorageClass;
        out.auxCount = sym->NumberOfAuxSymbols;
        return true;
    }

    const auto *sym = coffAt<coff::CoffSymbol>(data, size, offset);
    if (!sym)
        return false;
    std::memcpy(out.shortName, sym->Name.ShortName, sizeof(out.shortName));
    out.longNameZeros = sym->Name.LongName.Zeros;
    out.longNameOffset = sym->Name.LongName.Offset;
    out.value = sym->Value;
    // MSVC may emit standard COFF objects with section numbers above INT16_MAX.
    // Only the two reserved bit-patterns are negative special section numbers.
    const uint16_t rawSectionNumber = static_cast<uint16_t>(sym->SectionNumber);
    if (rawSectionNumber == 0xFFFF) {
        out.sectionNumber = coff::IMAGE_SYM_ABSOLUTE;
    } else if (rawSectionNumber == 0xFFFE) {
        out.sectionNumber = coff::IMAGE_SYM_DEBUG;
    } else {
        out.sectionNumber = static_cast<int32_t>(rawSectionNumber);
    }
    out.type = sym->Type;
    out.storageClass = sym->StorageClass;
    out.auxCount = sym->NumberOfAuxSymbols;
    return true;
}

/// @brief Maps a Microsoft COMDAT selection byte to the neutral linker policy.
/// @param selection `IMAGE_COMDAT_SELECT_*` value from an auxiliary record.
/// @return Corresponding selection policy, or `ComdatSelection::None` for an
///         unrecognized value.
static ComdatSelection coffComdatSelection(uint8_t selection) {
    switch (selection) {
        case coff::IMAGE_COMDAT_SELECT_NODUPLICATES:
            return ComdatSelection::NoDuplicates;
        case coff::IMAGE_COMDAT_SELECT_ANY:
            return ComdatSelection::Any;
        case coff::IMAGE_COMDAT_SELECT_SAME_SIZE:
            return ComdatSelection::SameSize;
        case coff::IMAGE_COMDAT_SELECT_EXACT_MATCH:
            return ComdatSelection::ExactMatch;
        case coff::IMAGE_COMDAT_SELECT_ASSOCIATIVE:
            return ComdatSelection::Associative;
        case coff::IMAGE_COMDAT_SELECT_LARGEST:
            return ComdatSelection::Largest;
        default:
            return ComdatSelection::None;
    }
}

/// @brief Derives the implicit alignment of a COFF common symbol.
/// @param size Common-symbol size in bytes.
/// @return Smallest power of two not less than @p size, capped at 32 bytes;
///         zero-sized commons use one-byte alignment.
static size_t coffCommonAlignment(uint32_t size) {
    if (size == 0)
        return 1;
    size_t alignment = 1;
    while (alignment < size && alignment < 32)
        alignment <<= 1;
    return alignment;
}

/// @brief Recover a relocation's signed addend from the live instruction bytes.
/// @details COFF, unlike ELF RELA, does not carry an explicit addend; the
///          assembler stores the addend inline in the operand field of the
///          instruction it patches. This helper decodes the addend per
///          relocation type: REL32 reads a literal 32-bit slot, ARM64 BR/B.cond
///          extract their immediate fields and rescale by 4, ADRP recovers the
///          21-bit page delta, and section-index relocations contribute no
///          addend. Unknown relocation types deliberately return a zero addend
///          so the relocation applier can issue its normal unsupported-type
///          diagnostic later; known relocation types must have enough bytes for
///          the encoded operand.
/// @param machine     COFF Machine field (AMD64 or ARM64).
/// @param relocType   Format-native relocation type number.
/// @param sectionData Section bytes the relocation applies to.
/// @param offset      Byte offset of the reloc site within @p sectionData.
/// @param out         Receives the decoded addend on success.
/// @return false when a known addend-bearing relocation has an out-of-bounds or
///         malformed patch site.
static bool extractCoffAddend(uint16_t machine,
                              uint16_t relocType,
                              const std::vector<uint8_t> &sectionData,
                              size_t offset,
                              int64_t &out) {
    out = 0;
    if (machine == coff::IMAGE_FILE_MACHINE_AMD64) {
        if (relocType == coff_x64::kSection)
            return true;
        if (relocType == coff_x64::kAddr64) {
            if (!checkedRange(offset, 8, sectionData.size()))
                return false;
            out = static_cast<int64_t>(readLE64(sectionData.data() + offset));
            return true;
        }
        switch (relocType) {
            case coff_x64::kAddr32:
            case coff_x64::kAddr32Nb:
            case coff_x64::kRel32:
            case coff_x64::kRel32_1:
            case coff_x64::kRel32_2:
            case coff_x64::kRel32_3:
            case coff_x64::kRel32_4:
            case coff_x64::kRel32_5:
            case coff_x64::kSecRel: {
                if (!checkedRange(offset, 4, sectionData.size()))
                    return false;
                int32_t val = 0;
                std::memcpy(&val, sectionData.data() + offset, 4);
                out = val;
                return true;
            }
            default:
                return true;
        }
    }

    if (machine != coff::IMAGE_FILE_MACHINE_ARM64)
        return true;

    if (relocType == coff_a64::kSection)
        return true;

    if (relocType == coff_a64::kAddr64) {
        if (!checkedRange(offset, 8, sectionData.size()))
            return false;
        out = static_cast<int64_t>(readLE64(sectionData.data() + offset));
        return true;
    }

    switch (relocType) {
        case coff_a64::kAddr32:
        case coff_a64::kAddr32Nb:
        case coff_a64::kBranch26:
        case coff_a64::kPageRel21:
        case coff_a64::kPageOff12A:
        case coff_a64::kPageOff12L:
        case coff_a64::kSecRel:
        case coff_a64::kSecRelLow12A:
        case coff_a64::kSecRelHigh12A:
        case coff_a64::kSecRelLow12L:
        case coff_a64::kBranch19:
            if (!checkedRange(offset, 4, sectionData.size()))
                return false;
            break;
        default:
            return true;
    }

    const uint32_t insn = readLE32(sectionData.data() + offset);
    switch (relocType) {
        case coff_a64::kAddr32:
        case coff_a64::kAddr32Nb:
        case coff_a64::kSecRel: {
            int32_t val = 0;
            std::memcpy(&val, sectionData.data() + offset, 4);
            out = val;
            return true;
        }
        case coff_a64::kBranch26:
            out = signExtend(insn & 0x03FFFFFFu, 26) << 2;
            return true;
        case coff_a64::kBranch19:
            out = signExtend((insn >> 5) & 0x7FFFFu, 19) << 2;
            return true;
        case coff_a64::kPageRel21: {
            const uint32_t immlo = (insn >> 29) & 0x3u;
            const uint32_t immhi = (insn >> 5) & 0x7FFFFu;
            // COFF stores the byte addend in ADRP's immediate field. The linker
            // later applies page rounding when it computes PAGE(S + A) - PAGE(P).
            out = signExtend((immhi << 2) | immlo, 21) << 12;
            return true;
        }
        case coff_a64::kPageOff12A:
            out = static_cast<int64_t>((insn >> 10) & 0xFFFu);
            return true;
        case coff_a64::kPageOff12L: {
            uint32_t shift = 0;
            if (!zanna::codegen::a64UnsignedLdStOffsetShift(insn, shift))
                return false;
            out = static_cast<int64_t>(((insn >> 10) & 0xFFFu) << shift);
            return true;
        }
        case coff_a64::kSecRelLow12A:
            out = static_cast<int64_t>((insn >> 10) & 0xFFFu);
            return true;
        case coff_a64::kSecRelHigh12A:
            out = static_cast<int64_t>(((insn >> 10) & 0xFFFu) << 12);
            return true;
        case coff_a64::kSecRelLow12L: {
            uint32_t shift = 0;
            if (!zanna::codegen::a64UnsignedLdStOffsetShift(insn, shift))
                return false;
            out = static_cast<int64_t>(((insn >> 10) & 0xFFFu) << shift);
            return true;
        }
        default:
            return true;
    }
}

/// @brief Parses an x64 or ARM64 COFF object into the neutral object model.
/// @details Supports standard and Microsoft BigObj headers, section-name and
///          symbol-name string-table references, relocation-overflow records,
///          inline relocation addends, COMDAT associations, common symbols,
///          and weak-external fallbacks.  The destination may contain partial
///          results after failure.
/// @param data Pointer to the complete object-file byte buffer.
/// @param size Size of @p data in bytes.
/// @param name Display name used in diagnostics and stored in @p obj.
/// @param obj Destination object representation.
/// @param err Stream that receives the first hard parse diagnostic.
/// @return `true` when the object is supported and fully parsed; otherwise
///         `false`.
bool readCoffObj(
    const uint8_t *data, size_t size, const std::string &name, ObjFile &obj, std::ostream &err) {
    if (size < sizeof(coff::CoffHeader)) {
        err << "error: " << name << ": too small for COFF header\n";
        return false;
    }

    const auto *hdr = coffAt<coff::CoffHeader>(data, size, 0);
    if (!hdr)
        return false;

    const bool bigObj = hdr->Machine == 0 && hdr->NumberOfSections == 0xFFFF;
    uint16_t machine = hdr->Machine;
    uint32_t numberOfSections = hdr->NumberOfSections;
    uint32_t pointerToSymbolTable = hdr->PointerToSymbolTable;
    uint32_t numberOfSymbols = hdr->NumberOfSymbols;
    uint16_t sizeOfOptionalHeader = hdr->SizeOfOptionalHeader;
    size_t headerSize = sizeof(coff::CoffHeader);
    size_t symbolSize = sizeof(coff::CoffSymbol);

    if (bigObj) {
        const auto *big = coffAt<coff::BigObjHeader>(data, size, 0);
        if (!big || big->Sig1 != 0 || big->Sig2 != 0xFFFF || big->Version < 2 ||
            std::memcmp(big->ClassID, coff::kBigObjClassId, sizeof(coff::kBigObjClassId)) != 0) {
            err << "error: " << name << ": malformed COFF BigObj header\n";
            return false;
        }
        machine = big->Machine;
        numberOfSections = big->NumberOfSections;
        pointerToSymbolTable = big->PointerToSymbolTable;
        numberOfSymbols = big->NumberOfSymbols;
        sizeOfOptionalHeader = 0;
        headerSize = sizeof(coff::BigObjHeader);
        symbolSize = sizeof(coff::CoffSymbolEx);
    }

    obj.format = ObjFileFormat::COFF;
    obj.is64bit = true;
    obj.isLittleEndian = true;
    obj.name = name;
    obj.machine = machine;
    obj.symbols.assign(1, ObjSymbol{});
    if (machine != 0 && machine != coff::IMAGE_FILE_MACHINE_AMD64 &&
        machine != coff::IMAGE_FILE_MACHINE_ARM64) {
        err << "error: " << name << ": unsupported COFF machine\n";
        return false;
    }
    if (numberOfSections > kMaxObjSections) {
        err << "error: " << name << ": section count " << numberOfSections << " exceeds limit\n";
        return false;
    }
    if (numberOfSymbols > kMaxObjSymbols) {
        err << "error: " << name << ": symbol count " << numberOfSymbols << " exceeds limit\n";
        return false;
    }

    // Locate string table (immediately after symbol table).
    size_t symtabBytes = 0;
    if (!checkedMul(static_cast<size_t>(numberOfSymbols), symbolSize, symtabBytes)) {
        err << "error: " << name << ": COFF symbol table size overflows address space\n";
        return false;
    }
    if (numberOfSymbols > 0 && pointerToSymbolTable == 0) {
        err << "error: " << name << ": COFF symbol table pointer is missing\n";
        return false;
    }
    if (numberOfSymbols > 0 && !checkedRange(pointerToSymbolTable, symtabBytes, size)) {
        err << "error: " << name << ": COFF symbol table is out of bounds\n";
        return false;
    }
    size_t strTabOff = 0;
    if (numberOfSymbols == 0 && pointerToSymbolTable == 0) {
        strTabOff = size;
    } else if (!checkedAdd(static_cast<size_t>(pointerToSymbolTable), symtabBytes, strTabOff)) {
        err << "error: " << name << ": COFF string table offset overflows address space\n";
        return false;
    }
    uint32_t strTabSize = 0;
    if (checkedRange(strTabOff, 4, size))
        strTabSize = readLE32(data + strTabOff);

    /// Validates a COFF string-table offset and returns its bounded byte window.
    auto validateStringTableRef = [&](uint32_t offset, size_t &pos, size_t &remain) -> bool {
        if (strTabSize < 4 || offset < 4 || offset >= strTabSize) {
            err << "error: " << name << ": COFF long name references invalid string table offset "
                << offset << "\n";
            return false;
        }
        if (!checkedAdd(strTabOff, static_cast<size_t>(offset), pos) ||
            !checkedRange(pos, static_cast<size_t>(strTabSize - offset), size)) {
            err << "error: " << name << ": COFF string table reference is out of bounds\n";
            return false;
        }
        remain = static_cast<size_t>(strTabSize - offset);
        return true;
    };

    /// Reads a NUL-terminated long name from a validated string-table offset.
    auto readLongName = [&](uint32_t offset, std::string &out) -> bool {
        size_t pos = 0;
        size_t remain = 0;
        if (!validateStringTableRef(offset, pos, remain))
            return false;
        if (!readBoundedString(data, pos, remain, out)) {
            err << "error: " << name << ": COFF long name is not NUL-terminated\n";
            return false;
        }
        return true;
    };

    /// Decodes either the inline eight-byte name or its string-table reference.
    auto readSymName = [&](const CoffSymbolView &sym, std::string &out) -> bool {
        if (sym.longNameZeros == 0) {
            // Long name: offset into string table.
            return readLongName(sym.longNameOffset, out);
        }
        // Short name: up to 8 chars, NUL-padded.
        size_t len = 0;
        while (len < 8 && sym.shortName[len] != '\0')
            ++len;
        out.assign(sym.shortName, len);
        return true;
    };

    const size_t secOff = headerSize + sizeOfOptionalHeader;
    size_t secBytes = 0;
    if (!checkedMul(static_cast<size_t>(numberOfSections), sizeof(coff::SectionHeader), secBytes) ||
        !checkedRange(secOff, secBytes, size)) {
        err << "error: " << name << ": COFF section header table is out of bounds\n";
        return false;
    }

    const uint32_t kNoPrimarySymbol = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> primarySymbolByRawIndex(numberOfSymbols, kNoPrimarySymbol);
    for (uint32_t i = 0; i < numberOfSymbols;) {
        CoffSymbolView sym{};
        if (!readCoffSymbolView(data, size, pointerToSymbolTable, symbolSize, i, bigObj, sym)) {
            err << "error: " << name << ": COFF symbol table is truncated\n";
            return false;
        }
        if (sym.auxCount > numberOfSymbols - i - 1) {
            err << "error: " << name << ": COFF auxiliary symbol count exceeds symbol table\n";
            return false;
        }
        for (uint32_t a = 1; a <= sym.auxCount; ++a)
            primarySymbolByRawIndex[i + a] = i;
        i += 1 + sym.auxCount;
    }

    // Parse sections.
    obj.sections.resize(1); // Null section at index 0.
    obj.sections[0].name = "";
    std::vector<uint32_t> sectionCharacteristics(static_cast<size_t>(numberOfSections) + 1, 0);
    size_t materializedBytes = 0;

    for (uint32_t i = 0; i < numberOfSections; ++i) {
        size_t sectionRecordOffset = 0;
        size_t scaledSectionIndex = 0;
        if (!checkedMul(static_cast<size_t>(i), sizeof(coff::SectionHeader), scaledSectionIndex) ||
            !checkedAdd(secOff, scaledSectionIndex, sectionRecordOffset)) {
            err << "error: " << name << ": COFF section header offset overflows address space\n";
            return false;
        }
        const auto *sh = coffAt<coff::SectionHeader>(data, size, sectionRecordOffset);
        if (!sh) {
            err << "error: " << name << ": COFF section header is out of bounds\n";
            return false;
        }

        ObjSection sec;

        // Parse section name (may reference string table if starts with '/').
        if (sh->Name[0] == '/') {
            size_t off = 0;
            if (sh->Name[1] < '0' || sh->Name[1] > '9') {
                err << "error: " << name << ": malformed COFF long section name reference\n";
                return false;
            }
            int c = 1;
            for (; c < 8 && sh->Name[c] >= '0' && sh->Name[c] <= '9'; ++c) {
                const size_t digit = static_cast<size_t>(sh->Name[c] - '0');
                if (off > (std::numeric_limits<size_t>::max() - digit) / 10U) {
                    err << "error: " << name << ": COFF long section name reference overflow\n";
                    return false;
                }
                off = off * 10U + digit;
            }
            for (; c < 8; ++c) {
                if (sh->Name[c] != '\0') {
                    err << "error: " << name << ": malformed COFF long section name reference\n";
                    return false;
                }
            }
            if (off > std::numeric_limits<uint32_t>::max() ||
                !readLongName(static_cast<uint32_t>(off), sec.name))
                return false;
        } else {
            size_t len = 0;
            while (len < 8 && sh->Name[len] != '\0')
                ++len;
            sec.name.assign(sh->Name, len);
        }

        sectionCharacteristics[i + 1] = sh->Characteristics;

        sec.executable = (sh->Characteristics & coff::IMAGE_SCN_MEM_EXECUTE) != 0;
        sec.writable = (sh->Characteristics & coff::IMAGE_SCN_MEM_WRITE) != 0;
        sec.tls = sec.name.rfind(".tls", 0) == 0;
        sec.alloc = (sh->Characteristics &
                     (coff::IMAGE_SCN_CNT_CODE | coff::IMAGE_SCN_CNT_INITIALIZED_DATA |
                      coff::IMAGE_SCN_CNT_UNINITIALIZED_DATA)) != 0;

        // Extract alignment from COFF characteristics (bits 20-23).
        const uint32_t alignBits = (sh->Characteristics >> 20) & 0xF;
        sec.alignment = (alignBits > 0) ? (1u << (alignBits - 1)) : 1;

        // COFF uninitialized-data sections must be materialized as zero-filled
        // storage even if SizeOfRawData/PointerToRawData happen to point at
        // bytes in the object file. Some producers leave non-zero metadata in
        // those fields, and copying it verbatim corrupts zero-init globals.
        if (sh->Characteristics & coff::IMAGE_SCN_CNT_UNINITIALIZED_DATA) {
            sec.zeroFill = true;
            const uint32_t zeroSize = sh->VirtualSize != 0 ? sh->VirtualSize : sh->SizeOfRawData;
            if (zeroSize > kMaxObjSectionBytes) {
                err << "error: " << name << ": COFF section '" << sec.name << "' is too large\n";
                return false;
            }
            sec.memSize = zeroSize;
        } else if (sh->SizeOfRawData > 0 &&
                   checkedRange(sh->PointerToRawData, sh->SizeOfRawData, size)) {
            if (sh->SizeOfRawData > kMaxObjSectionBytes ||
                sh->SizeOfRawData > kMaxObjMaterializedBytes - materializedBytes) {
                err << "error: " << name << ": COFF section '" << sec.name << "' is too large\n";
                return false;
            }
            sec.data.assign(data + sh->PointerToRawData,
                            data + sh->PointerToRawData + sh->SizeOfRawData);
            sec.memSize = sec.data.size();
            materializedBytes += sh->SizeOfRawData;
        } else if (sh->SizeOfRawData > 0) {
            err << "error: " << name << ": COFF section '" << sec.name
                << "' raw data is out of bounds\n";
            return false;
        }

        uint32_t relocCount = sh->NumberOfRelocations;
        uint32_t firstReloc = 0;
        uint32_t totalRelocRecords = relocCount;
        if ((sh->Characteristics & coff::IMAGE_SCN_LNK_NRELOC_OVFL) != 0 &&
            sh->NumberOfRelocations == 0xFFFF) {
            const auto *overflow = coffAt<coff::CoffReloc>(data, size, sh->PointerToRelocations);
            if (!overflow) {
                err << "error: " << name << ": COFF relocation overflow record is out of bounds\n";
                return false;
            }
            if (overflow->VirtualAddress <= 1) {
                err << "error: " << name << ": COFF relocation overflow count is malformed\n";
                return false;
            }
            totalRelocRecords = overflow->VirtualAddress;
            relocCount = totalRelocRecords - 1;
            firstReloc = 1;
        }
        size_t relocBytes = 0;
        if (!checkedMul(
                static_cast<size_t>(totalRelocRecords), sizeof(coff::CoffReloc), relocBytes)) {
            err << "error: " << name << ": COFF relocation table size overflows address space\n";
            return false;
        }
        if (relocCount > 0 && !checkedRange(sh->PointerToRelocations, relocBytes, size)) {
            err << "error: " << name << ": COFF relocation table is out of bounds\n";
            return false;
        }

        // Read relocations.
        for (uint32_t r = firstReloc; r < totalRelocRecords; ++r) {
            size_t relocScaled = 0;
            size_t relocRecordOffset = 0;
            if (!checkedMul(static_cast<size_t>(r), sizeof(coff::CoffReloc), relocScaled) ||
                !checkedAdd(static_cast<size_t>(sh->PointerToRelocations),
                            relocScaled,
                            relocRecordOffset)) {
                err << "error: " << name << ": COFF relocation entry offset overflows\n";
                return false;
            }
            const auto *cr = coffAt<coff::CoffReloc>(data, size, relocRecordOffset);
            if (!cr) {
                err << "error: " << name << ": COFF relocation entry is out of bounds\n";
                return false;
            }

            ObjReloc rel;
            rel.offset = cr->VirtualAddress;
            rel.type = cr->Type;
            // Reject an out-of-section fixup offset up front, for every relocation
            // type. extractCoffAddend range-checks only the types whose addend it
            // decodes; SECTION and any type it passes through would otherwise carry an
            // unvalidated offset into a section that may be stripped before the applier
            // (which independently guards placed sections) ever sees it. Mirror the
            // unconditional bounds check MachOReader performs on r_address.
            if (rel.offset >= sec.data.size()) {
                err << "error: " << name << ": COFF relocation at offset " << rel.offset
                    << " is outside section '" << sec.name << "' (size=" << sec.data.size()
                    << ")\n";
                return false;
            }
            if (cr->SymbolTableIndex >= numberOfSymbols) {
                err << "error: " << name << ": COFF relocation references invalid symbol index "
                    << cr->SymbolTableIndex << "\n";
                return false;
            }
            if (primarySymbolByRawIndex[cr->SymbolTableIndex] != kNoPrimarySymbol) {
                err << "error: " << name << ": COFF relocation references auxiliary symbol index "
                    << cr->SymbolTableIndex << "\n";
                return false;
            }
            rel.symIndex = cr->SymbolTableIndex + 1; // +1 because ObjFile has null sym at 0.
            if (!extractCoffAddend(
                    machine, static_cast<uint16_t>(rel.type), sec.data, rel.offset, rel.addend)) {
                err << "error: " << name << ": COFF relocation addend at offset " << rel.offset
                    << " is malformed or out of bounds in section '" << sec.name << "'\n";
                return false;
            }

            sec.relocs.push_back(rel);
        }

        obj.sections.push_back(std::move(sec));
    }

    // Parse symbols.
    /// Computes the checked byte offset of a raw symbol-table record.
    auto symbolRecordOffset = [&](uint32_t index, size_t &offset) -> bool {
        size_t scaled = 0;
        return checkedMul(static_cast<size_t>(index), symbolSize, scaled) &&
               checkedAdd(static_cast<size_t>(pointerToSymbolTable), scaled, offset);
    };

    std::vector<uint8_t> comdatSelectionBySection(static_cast<size_t>(numberOfSections) + 1, 0);
    std::vector<std::string> comdatKeyBySection(static_cast<size_t>(numberOfSections) + 1);
    std::vector<uint32_t> associativeSectionBySection(static_cast<size_t>(numberOfSections) + 1, 0);

    for (uint32_t i = 0; i < numberOfSymbols;) {
        CoffSymbolView sym{};
        if (!readCoffSymbolView(data, size, pointerToSymbolTable, symbolSize, i, bigObj, sym)) {
            err << "error: " << name << ": COFF symbol table is truncated\n";
            return false;
        }
        if (sym.auxCount > numberOfSymbols - i - 1) {
            err << "error: " << name << ": COFF auxiliary symbol count exceeds symbol table\n";
            return false;
        }

        const bool hasSectionAux =
            sym.sectionNumber > 0 && static_cast<uint32_t>(sym.sectionNumber) <= numberOfSections &&
            sym.storageClass == coff::IMAGE_SYM_CLASS_STATIC && sym.auxCount > 0;
        if (hasSectionAux) {
            const uint32_t sectionNumber = static_cast<uint32_t>(sym.sectionNumber);
            const bool isComdat =
                (sectionCharacteristics[sectionNumber] & coff::IMAGE_SCN_LNK_COMDAT) != 0;
            if (isComdat) {
                size_t auxOffset = 0;
                if (!symbolRecordOffset(i + 1, auxOffset)) {
                    err << "error: " << name << ": COFF auxiliary symbol offset overflows\n";
                    return false;
                }
                const auto *aux = coffAt<coff::CoffAuxSectionDefinition>(data, size, auxOffset);
                if (!aux) {
                    err << "error: " << name << ": COFF COMDAT auxiliary record is truncated\n";
                    return false;
                }
                comdatSelectionBySection[sectionNumber] = aux->Selection;
                if (coffComdatSelection(aux->Selection) == ComdatSelection::None) {
                    err << "error: " << name << ": unsupported COFF COMDAT selection "
                        << static_cast<unsigned>(aux->Selection) << "\n";
                    return false;
                }
                if (!readSymName(sym, comdatKeyBySection[sectionNumber]))
                    return false;
                if (sectionNumber < obj.sections.size() &&
                    comdatKeyBySection[sectionNumber].empty())
                    comdatKeyBySection[sectionNumber] = obj.sections[sectionNumber].name;
                const uint32_t assocSection =
                    static_cast<uint32_t>(static_cast<uint16_t>(aux->Number)) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(aux->HighNumber)) << 16);
                if (aux->Selection == coff::IMAGE_COMDAT_SELECT_ASSOCIATIVE && assocSection > 0 &&
                    assocSection <= numberOfSections)
                    associativeSectionBySection[sectionNumber] = assocSection;
                else if (aux->Selection == coff::IMAGE_COMDAT_SELECT_ASSOCIATIVE) {
                    err << "error: " << name
                        << ": COFF associative COMDAT references invalid section " << assocSection
                        << "\n";
                    return false;
                }
            }
        }

        i += 1 + sym.auxCount;
    }
    for (uint32_t secNo = 1; secNo < associativeSectionBySection.size(); ++secNo) {
        if (secNo < obj.sections.size()) {
            obj.sections[secNo].associativeSection = associativeSectionBySection[secNo];
            obj.sections[secNo].comdatSelection =
                coffComdatSelection(comdatSelectionBySection[secNo]);
            obj.sections[secNo].comdatKey = comdatKeyBySection[secNo];
        }
    }

    for (uint32_t i = 0; i < numberOfSymbols;) {
        CoffSymbolView sym{};
        if (!readCoffSymbolView(data, size, pointerToSymbolTable, symbolSize, i, bigObj, sym)) {
            err << "error: " << name << ": COFF symbol table is truncated\n";
            return false;
        }
        if (sym.auxCount > numberOfSymbols - i - 1) {
            err << "error: " << name << ": COFF auxiliary symbol count exceeds symbol table\n";
            return false;
        }

        ObjSymbol os;
        if (!readSymName(sym, os.name))
            return false;
        bool offsetAlreadySet = false;

        if (sym.sectionNumber == coff::IMAGE_SYM_UNDEFINED &&
            sym.storageClass == coff::IMAGE_SYM_CLASS_EXTERNAL && sym.value > 0) {
            os.binding = ObjSymbol::Global;
            os.common = true;
            os.commonAlignment = coffCommonAlignment(sym.value);
            os.sectionIndex = 0;
            os.offset = 0;
            os.size = static_cast<size_t>(sym.value);
            offsetAlreadySet = true;
        } else if (sym.sectionNumber == coff::IMAGE_SYM_UNDEFINED) {
            os.binding = ObjSymbol::Undefined;
            if (sym.storageClass == coff::IMAGE_SYM_CLASS_WEAK_EXTERNAL) {
                os.weakExternal = true;
                if (sym.auxCount == 0) {
                    err << "error: " << name
                        << ": COFF weak external is missing auxiliary record\n";
                    return false;
                }
                size_t auxOffset = 0;
                if (!symbolRecordOffset(i + 1, auxOffset)) {
                    err << "error: " << name << ": COFF weak auxiliary symbol offset overflows\n";
                    return false;
                }
                const auto *aux = coffAt<coff::CoffAuxWeakExternal>(data, size, auxOffset);
                if (!aux) {
                    err << "error: " << name << ": COFF weak auxiliary symbol is truncated\n";
                    return false;
                }
                if (aux->TagIndex >= numberOfSymbols ||
                    primarySymbolByRawIndex[aux->TagIndex] != kNoPrimarySymbol) {
                    err << "error: " << name
                        << ": COFF weak external references invalid fallback symbol\n";
                    return false;
                }
                if (aux->Characteristics < 1 || aux->Characteristics > 3) {
                    err << "error: " << name
                        << ": COFF weak external has unsupported search characteristic "
                        << aux->Characteristics << "\n";
                    return false;
                }
                os.weakExternalCharacteristics = aux->Characteristics;
                CoffSymbolView fallback{};
                if (!readCoffSymbolView(data,
                                        size,
                                        pointerToSymbolTable,
                                        symbolSize,
                                        aux->TagIndex,
                                        bigObj,
                                        fallback)) {
                    err << "error: " << name << ": COFF weak fallback symbol is truncated\n";
                    return false;
                }
                if (!readSymName(fallback, os.weakDefaultName))
                    return false;
            }
        } else if (sym.sectionNumber == coff::IMAGE_SYM_ABSOLUTE ||
                   sym.sectionNumber == coff::IMAGE_SYM_DEBUG) {
            // An external absolute symbol (e.g. `PUBLIC x = 0x1234` / `.globl`
            // equate) must stay Global so cross-object references resolve; only
            // non-external absolutes/debug symbols are file-local.
            os.binding = (sym.sectionNumber == coff::IMAGE_SYM_ABSOLUTE &&
                          sym.storageClass == coff::IMAGE_SYM_CLASS_EXTERNAL)
                             ? ObjSymbol::Global
                             : ObjSymbol::Local;
            os.absolute = (sym.sectionNumber == coff::IMAGE_SYM_ABSOLUTE);
        } else if (sym.storageClass == coff::IMAGE_SYM_CLASS_EXTERNAL) {
            os.binding = ObjSymbol::Global;
        } else if (sym.storageClass == coff::IMAGE_SYM_CLASS_WEAK_EXTERNAL) {
            os.binding = ObjSymbol::Weak;
        } else {
            os.binding = ObjSymbol::Local;
        }

        // Map 1-based COFF section number to our section index.
        if (sym.sectionNumber > 0) {
            if (static_cast<uint32_t>(sym.sectionNumber) > numberOfSections) {
                err << "error: " << name << ": COFF symbol '" << os.name
                    << "' references invalid section " << sym.sectionNumber << "\n";
                return false;
            }
            os.sectionIndex = static_cast<uint32_t>(sym.sectionNumber);
        }
        if (!offsetAlreadySet)
            os.offset = sym.value;
        if (!os.common && os.sectionIndex > 0 && os.sectionIndex < obj.sections.size()) {
            const size_t secSize = objSectionMemSize(obj.sections[os.sectionIndex]);
            if (os.offset > secSize) {
                err << "error: " << name << ": COFF symbol '" << os.name << "' is outside section '"
                    << obj.sections[os.sectionIndex].name << "'\n";
                return false;
            }
        }

        obj.symbols.push_back(std::move(os));

        // Skip aux symbols.
        i += 1 + sym.auxCount;
        // Pad the symbol array for skipped aux entries.
        for (uint8_t a = 0; a < sym.auxCount; ++a)
            obj.symbols.push_back(ObjSymbol{});
    }

    return true;
}

} // namespace zanna::codegen::linker
