//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/MachOReader.cpp
// Purpose: Mach-O 64-bit relocatable object file reader.
// Key invariants:
//   - Magic 0xFEEDFACF (little-endian 64-bit)
//   - Symbol names are stripped of leading '_' (Mach-O convention)
//   - Relocations have NO explicit addend — extracted from instruction bytes
//   - Sections parsed from LC_SEGMENT_64, symbols from LC_SYMTAB
// Links: codegen/common/linker/ObjFileReader.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file MachOReader.cpp
 * @brief Implements bounded x86-64 and AArch64 Mach-O object parsing.
 *
 * Load commands, sections, symbols, and inline relocation addends are
 * translated into the neutral object model. Objects carrying
 * `MH_SUBSECTIONS_VIA_SYMBOLS` are split into per-atom executable sections
 * while preserving symbol and relocation ownership.
 */

#include "codegen/common/AArch64RelocUtil.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"
#include "codegen/common/linker/RelocConstants.hpp"
#include "codegen/common/objfile/ObjFileWriterUtil.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::linker {

using zanna::codegen::objfile::checkedAdd;
using zanna::codegen::objfile::checkedMul;
using zanna::codegen::objfile::checkedRange;
using zanna::codegen::objfile::readLE32;
using zanna::codegen::objfile::readLE64;

namespace macho {
static constexpr uint32_t MH_MAGIC_64 = 0xFEEDFACF;
static constexpr uint32_t MH_OBJECT = 1;
static constexpr uint32_t MH_SUBSECTIONS_VIA_SYMBOLS = 0x2000;

static constexpr uint32_t CPU_TYPE_X86_64 = 0x01000007;
static constexpr uint32_t CPU_TYPE_ARM64 = 0x0100000C;

static constexpr uint32_t LC_SEGMENT_64 = 0x19;
static constexpr uint32_t LC_SYMTAB = 0x02;

// Section type constants (low 8 bits of flags).
[[maybe_unused]] static constexpr uint32_t S_REGULAR = 0x00;
static constexpr uint32_t S_ZEROFILL = 0x01;
static constexpr uint32_t S_NON_LAZY_SYMBOL_POINTERS = 0x06;
static constexpr uint32_t S_LAZY_SYMBOL_POINTERS = 0x07;
static constexpr uint32_t S_THREAD_LOCAL_REGULAR = 0x11;
static constexpr uint32_t S_THREAD_LOCAL_ZEROFILL = 0x12;
static constexpr uint32_t S_THREAD_LOCAL_VARIABLES = 0x13;

// Section attribute flags (high bits).
static constexpr uint32_t S_ATTR_PURE_INSTRUCTIONS = 0x80000000;
static constexpr uint32_t S_ATTR_SOME_INSTRUCTIONS = 0x00000400;
static constexpr uint32_t S_ATTR_DEBUG = 0x02000000;
static constexpr uint32_t S_ATTR_NO_DEAD_STRIP = 0x10000000;

/// @brief Packed Mach-O 64-bit object header.
struct mach_header_64 {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
};

/// @brief Common prefix of every Mach-O load command.
struct load_command {
    uint32_t cmd;
    uint32_t cmdsize;
};

/// @brief Mach-O 64-bit segment load command.
struct segment_command_64 {
    uint32_t cmd;
    uint32_t cmdsize;
    char segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
};

/// @brief Mach-O 64-bit section record.
struct section_64 {
    char sectname[16];
    char segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
};

/// @brief Mach-O 64-bit symbol-table record.
struct nlist_64 {
    uint32_t n_strx;
    uint8_t n_type;
    uint8_t n_sect;
    uint16_t n_desc;
    uint64_t n_value;
};

/// @brief Mach-O scattered-free relocation record.
struct relocation_info {
    int32_t r_address;
    uint32_t r_info; // symbolnum:24 | pcrel:1 | length:2 | extern:1 | type:4
};

} // namespace macho

/// @brief Bounds-checked struct copy at @p offset within a byte buffer.
/// @tparam T Trivially copyable Mach-O record type.
/// @param data Base of the object-file bytes.
/// @param size Total buffer size.
/// @param offset Byte offset of the requested record.
/// @return Copied record, or `std::nullopt` when the range is truncated.
template <typename T>
static std::optional<T> readAt(const uint8_t *data, size_t size, size_t offset) {
    if (offset > size || sizeof(T) > size - offset)
        return std::nullopt;
    T value{};
    std::memcpy(&value, data + offset, sizeof(T));
    return value;
}

/// @brief Copy @p s up to the first NUL or @p maxLen bytes (Mach-O fixed-name fields).
/// @details Mach-O segment/section names occupy fixed 16-byte slots that are
///          NUL-padded; this helper trims to the actual logical length.
/// @param s Fixed-width character field.
/// @param maxLen Maximum field width.
/// @return Logical string without trailing NUL padding.
static std::string trimNul(const char *s, size_t maxLen) {
    size_t len = 0;
    while (len < maxLen && s[len] != '\0')
        ++len;
    return std::string(s, len);
}

/// @brief Read a NUL-terminated string from a Mach-O LC_SYMTAB strtab.
/// @param data Base of the object-file bytes.
/// @param size Total object-file size.
/// @param off Byte offset of the string table.
/// @param len String-table size.
/// @param pos Offset of the requested string within the table.
/// @return Decoded string, or `std::nullopt` for an invalid table/range or a
///         missing terminator.
static std::optional<std::string> readStringOpt(
    const uint8_t *data, size_t size, size_t off, size_t len, uint32_t pos) {
    if (!checkedRange(off, len, size) || pos >= len)
        return std::nullopt;
    const uint8_t *begin = data + off + pos;
    const uint8_t *end = data + off + len;
    const void *nul = std::memchr(begin, '\0', static_cast<size_t>(end - begin));
    if (!nul)
        return std::nullopt;
    return std::string(reinterpret_cast<const char *>(begin), static_cast<const char *>(nul));
}

/// @brief Splits a monolithic Mach-O text section at symbol-defined atom boundaries.
/// @details Creates per-function sections, remaps symbol offsets and section
///          indices, and redistributes relocations. Alternate-entry symbols do
///          not introduce cuts because they remain inside their owning atom.
/// @param obj Parsed object to rewrite in place.
static void splitMachOTextSubsections(ObjFile &obj) {
    /// @brief Maps a half-open range in an old section to its new section.
    struct Range {
        uint32_t oldSec = 0;
        uint32_t newSec = 0;
        size_t start = 0;
        size_t end = 0;
    };

    std::vector<ObjSection> newSections;
    newSections.push_back(obj.sections.empty() ? ObjSection{} : obj.sections[0]);
    std::vector<uint32_t> directMap(obj.sections.size(), 0);
    std::vector<Range> ranges;

    // Bucket symbol indices by their defining section ONCE. Previously the split
    // and part-naming loops rescanned every symbol per section, which is O(sections
    // x symbols) — quadratic for a single large __text with many symbols.
    std::vector<std::vector<uint32_t>> symbolsBySection(obj.sections.size());
    for (uint32_t i = 0; i < obj.symbols.size(); ++i) {
        const auto &sym = obj.symbols[i];
        if (sym.sectionIndex > 0 && sym.sectionIndex < symbolsBySection.size())
            symbolsBySection[sym.sectionIndex].push_back(i);
    }

    for (uint32_t si = 1; si < obj.sections.size(); ++si) {
        const auto &sec = obj.sections[si];
        const bool splitCandidate =
            sec.executable && sec.name == "__TEXT,__text" && sec.data.size() > 1;

        std::vector<size_t> cuts;
        std::unordered_map<size_t, std::string> nameAtOffset;
        if (splitCandidate) {
            cuts.push_back(0);
            for (uint32_t symIdx : symbolsBySection[si]) {
                const auto &sym = obj.symbols[symIdx];
                if (sym.name.empty() || sym.name.rfind("$sect.", 0) == 0)
                    continue;
                if (sym.offset >= sec.data.size())
                    continue;
                // N_ALT_ENTRY marks an alternate entry point inside an atom; it
                // must NOT start a new subsection (splitting there would fracture
                // the atom and misdistribute its relocations/data).
                if (sym.altEntry)
                    continue;
                cuts.push_back(sym.offset);
                nameAtOffset.emplace(sym.offset, sym.name); // first symbol names it
            }
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        }

        if (cuts.size() < 2 || cuts.front() != 0) {
            directMap[si] = static_cast<uint32_t>(newSections.size());
            newSections.push_back(sec);
            continue;
        }

        for (size_t i = 0; i < cuts.size(); ++i) {
            const size_t start = cuts[i];
            const size_t end = (i + 1 < cuts.size()) ? cuts[i + 1] : sec.data.size();
            if (start >= end)
                continue;

            ObjSection part;
            part.name = "__TEXT,__text";
            part.alignment = sec.alignment;
            part.executable = sec.executable;
            part.writable = sec.writable;
            part.alloc = sec.alloc;
            part.tls = sec.tls;
            part.zeroFill = sec.zeroFill;
            part.dataSegment = sec.dataSegment;
            part.isCStringSection = sec.isCStringSection;
            part.associativeSection = sec.associativeSection;
            part.comdatSelection = sec.comdatSelection;
            part.comdatKey = sec.comdatKey;
            part.stripped = sec.stripped;
            auto nameIt = nameAtOffset.find(start);
            if (nameIt != nameAtOffset.end())
                part.name = "__TEXT,__text." + nameIt->second;
            part.data.assign(sec.data.begin() + static_cast<std::ptrdiff_t>(start),
                             sec.data.begin() + static_cast<std::ptrdiff_t>(end));
            part.memSize = part.data.size();
            const uint32_t newSecIdx = static_cast<uint32_t>(newSections.size());
            ranges.push_back({si, newSecIdx, start, end});
            newSections.push_back(std::move(part));
        }
    }

    if (ranges.empty())
        return;

    // Direct-mapped sections were copied with their original relocation lists
    // above. Once any __text splitting happens, all relocations are redistributed
    // below so offsets can be adjusted for split ranges. Clear the copied lists
    // first to avoid applying direct-mapped section relocations twice.
    for (size_t si = 1; si < newSections.size(); ++si)
        newSections[si].relocs.clear();

    std::vector<std::vector<const Range *>> rangesByOldSec(obj.sections.size());
    for (const auto &range : ranges) {
        if (range.oldSec < rangesByOldSec.size())
            rangesByOldSec[range.oldSec].push_back(&range);
    }

    /// Finds the split range containing an old section-relative offset.
    auto findRange = [&](uint32_t oldSec, size_t off) -> const Range * {
        if (oldSec >= rangesByOldSec.size())
            return nullptr;
        const auto &sectionRanges = rangesByOldSec[oldSec];
        if (sectionRanges.empty())
            return nullptr;

        /// Locates the first range beginning after the requested offset.
        auto it = std::upper_bound(
            sectionRanges.begin(), sectionRanges.end(), off, [](size_t value, const Range *range) {
                return value < range->start;
            });
        if (it != sectionRanges.begin()) {
            --it;
            if (off >= (*it)->start && off < (*it)->end)
                return *it;
        }

        const Range *last = sectionRanges.back();
        if (off == last->end)
            return last;
        return nullptr;
    };

    for (auto &sym : obj.symbols) {
        if (sym.sectionIndex == 0 || sym.sectionIndex >= directMap.size())
            continue;
        if (const Range *range = findRange(sym.sectionIndex, sym.offset)) {
            sym.sectionIndex = range->newSec;
            sym.offset -= range->start;
        } else if (directMap[sym.sectionIndex] != 0) {
            sym.sectionIndex = directMap[sym.sectionIndex];
        }
    }

    for (uint32_t oldSec = 1; oldSec < obj.sections.size(); ++oldSec) {
        for (const auto &rel : obj.sections[oldSec].relocs) {
            if (const Range *range = findRange(oldSec, rel.offset)) {
                ObjReloc adjusted = rel;
                adjusted.offset -= range->start;
                newSections[range->newSec].relocs.push_back(adjusted);
            } else if (oldSec < directMap.size() && directMap[oldSec] != 0) {
                newSections[directMap[oldSec]].relocs.push_back(rel);
            }
        }
    }

    obj.sections = std::move(newSections);
}

/// @brief Sign-extend the low @p bits of @p value to a 64-bit signed integer.
/// @param value Unsigned bit pattern containing the field.
/// @param bits Width of the signed field.
/// @return Sign-extended value.
static int64_t signExtend(uint64_t value, unsigned bits) {
    const uint64_t signBit = uint64_t{1} << (bits - 1);
    const uint64_t mask = (uint64_t{1} << bits) - 1;
    value &= mask;
    return static_cast<int64_t>((value ^ signBit) - signBit);
}

/// @brief Extract a relocation addend from inline Mach-O fixup bytes.
/// @details Mach-O relocatable objects do not carry explicit addends for most
///          relocation kinds. The assembler leaves the addend encoded in the
///          data slot or instruction operand that the linker will patch. This
///          helper validates the encoded field width before reading it and
///          decodes the AArch64 instruction forms that store scaled immediates.
///          Unknown relocation types return a zero addend so later relocation
///          classification can produce the normal unsupported-type diagnostic.
/// @param sectionData  Start of the section payload containing the fixup.
/// @param sectionSize  Number of bytes available at @p sectionData.
/// @param offset       Byte offset of the fixup inside the section payload.
/// @param relocType    Mach-O relocation type code.
/// @param relocLength  Mach-O relocation length exponent (`2` = 4 bytes,
///                     `3` = 8 bytes).
/// @param isArm64      Whether to decode ARM64 instruction immediates.
/// @param out          Receives the decoded addend on success.
/// @return false when a known relocation field is truncated or malformed.
static bool extractMachOAddend(const uint8_t *sectionData,
                               size_t sectionSize,
                               size_t offset,
                               uint32_t relocType,
                               uint32_t relocLength,
                               bool isArm64,
                               int64_t &out) {
    out = 0;
    if (isArm64) {
        if (relocType == macho_a64::kUnsigned) {
            const size_t fieldSize = size_t{1} << relocLength;
            if (!checkedRange(offset, fieldSize, sectionSize))
                return false;
            if (fieldSize == 8)
                out = static_cast<int64_t>(readLE64(sectionData + offset));
            else if (fieldSize == 4) {
                int32_t val = 0;
                std::memcpy(&val, sectionData + offset, 4);
                out = val;
            } else if (fieldSize == 2) {
                int16_t val = 0;
                std::memcpy(&val, sectionData + offset, 2);
                out = val;
            } else {
                int8_t val = 0;
                std::memcpy(&val, sectionData + offset, 1);
                out = val;
            }
            return true;
        }
        if (!checkedRange(offset, 4, sectionSize))
            return false;
        const uint32_t insn = readLE32(sectionData + offset);
        switch (relocType) {
            case macho_a64::kBranch26:
                out = signExtend(insn & 0x03FFFFFFu, 26) << 2;
                return true;
            case macho_a64::kPage21:
            case macho_a64::kGotLoadPage21:
            case macho_a64::kTlvpLoadPage21: {
                const uint32_t immlo = (insn >> 29) & 0x3u;
                const uint32_t immhi = (insn >> 5) & 0x7FFFFu;
                out = signExtend((immhi << 2) | immlo, 21) << 12;
                return true;
            }
            case macho_a64::kPageOff12:
            case macho_a64::kGotLoadPageOff12:
            case macho_a64::kTlvpLoadPageOff12: {
                uint32_t pageOff = (insn >> 10) & 0xFFFu;
                uint32_t shift = 0;
                if (zanna::codegen::a64UnsignedLdStOffsetShift(insn, shift))
                    pageOff <<= shift;
                out = static_cast<int64_t>(pageOff);
                return true;
            }
            case macho_a64::kPointerToGot:
                return true;
            default:
                return true;
        }
    }

    const size_t fieldSize = size_t{1} << relocLength;
    if (!checkedRange(offset, fieldSize, sectionSize))
        return false;
    /// Converts Mach-O signed PC-relative inline values to relocation addends.
    auto normalizeX64Addend = [&](int64_t val) {
        switch (relocType) {
            case macho_x64::kSigned:
            case macho_x64::kSigned1:
            case macho_x64::kSigned2:
            case macho_x64::kSigned4:
            case macho_x64::kBranch:
                return val - 4;
            default:
                return val;
        }
    };
    if (fieldSize == 1) {
        int8_t val = 0;
        std::memcpy(&val, sectionData + offset, 1);
        out = normalizeX64Addend(val);
    } else if (fieldSize == 2) {
        int16_t val = 0;
        std::memcpy(&val, sectionData + offset, 2);
        out = normalizeX64Addend(val);
    } else if (fieldSize == 4) {
        int32_t val = 0;
        std::memcpy(&val, sectionData + offset, 4);
        out = normalizeX64Addend(val);
    } else {
        int64_t val = 0;
        std::memcpy(&val, sectionData + offset, 8);
        out = normalizeX64Addend(val);
    }
    return true;
}

/// @brief Parses a Mach-O 64-bit relocatable object into the neutral linker model.
/// @details Accepts x86-64 and AArch64 `MH_OBJECT` files, validates all load
///          command/table ranges and resource limits, materializes sections,
///          normalizes external names, decodes relocations, and optionally
///          performs subsection splitting. The destination may be partial on failure.
/// @param data Pointer to complete object-file bytes.
/// @param size Size of @p data.
/// @param name Display name used in diagnostics and stored in @p obj.
/// @param obj Destination object representation.
/// @param err Stream that receives the first hard parse diagnostic.
/// @return `true` when the object is supported and fully parsed.
bool readMachOObj(
    const uint8_t *data, size_t size, const std::string &name, ObjFile &obj, std::ostream &err) {
    const auto hdrValue = readAt<macho::mach_header_64>(data, size, 0);
    if (!hdrValue || hdrValue->magic != macho::MH_MAGIC_64) {
        err << "error: " << name << ": invalid Mach-O magic\n";
        return false;
    }
    const auto *hdr = &*hdrValue;

    obj.format = ObjFileFormat::MachO;
    obj.is64bit = true;
    obj.isLittleEndian = true;
    obj.name = name;
    obj.symbols.assign(1, ObjSymbol{});

    const bool isArm64 = (hdr->cputype == macho::CPU_TYPE_ARM64);
    if (hdr->filetype != macho::MH_OBJECT ||
        (hdr->cputype != macho::CPU_TYPE_ARM64 && hdr->cputype != macho::CPU_TYPE_X86_64)) {
        err << "error: " << name << ": unsupported Mach-O object format\n";
        return false;
    }
    obj.machine = isArm64 ? 183 : 62; // Map to ELF machine constants for uniformity.

    if (!checkedRange(sizeof(macho::mach_header_64), hdr->sizeofcmds, size)) {
        err << "error: " << name << ": Mach-O load commands are out of bounds\n";
        return false;
    }
    size_t loadCommandsEnd = 0;
    if (!checkedAdd(
            sizeof(macho::mach_header_64), static_cast<size_t>(hdr->sizeofcmds), loadCommandsEnd)) {
        err << "error: " << name << ": Mach-O load command span overflows address space\n";
        return false;
    }

    // Parse load commands.
    size_t lcOff = sizeof(macho::mach_header_64);
    bool haveSymtab = false;
    size_t symtabLcOff = 0;
    size_t materializedBytes = 0;

    // First pass: collect sections and find LC_SYMTAB.
    obj.sections.resize(1); // Null section at index 0.
    obj.sections[0].name = "";
    // Track section base addresses for converting n_value to section-relative offsets.
    // Mach-O n_value is an absolute address within the .o; we need (n_value - secAddr).
    std::vector<uint64_t> secAddrs;
    secAddrs.push_back(0); // Null section.
    // Map Mach-O 1-based section index → ObjFile section index.
    // Skipped sections (debug, etc.) map to 0 (unmapped).
    std::vector<uint32_t> machoSecMap;
    machoSecMap.push_back(0); // Null section (index 0).
    size_t tmpOff = lcOff;
    for (uint32_t c = 0; c < hdr->ncmds; ++c) {
        const auto lcValue = readAt<macho::load_command>(data, size, tmpOff);
        if (!lcValue) {
            err << "error: " << name << ": truncated Mach-O load command\n";
            return false;
        }
        const auto *lc = &*lcValue;
        size_t commandEnd = 0;
        if (lc->cmdsize < sizeof(macho::load_command) || !checkedRange(tmpOff, lc->cmdsize, size) ||
            !checkedAdd(tmpOff, static_cast<size_t>(lc->cmdsize), commandEnd) ||
            commandEnd > loadCommandsEnd) {
            err << "error: " << name << ": malformed Mach-O load command\n";
            return false;
        }

        if (lc->cmd == macho::LC_SEGMENT_64) {
            if (lc->cmdsize < sizeof(macho::segment_command_64)) {
                err << "error: " << name << ": malformed Mach-O segment command size\n";
                return false;
            }
            const auto segValue = readAt<macho::segment_command_64>(data, size, tmpOff);
            if (!segValue) {
                err << "error: " << name << ": truncated Mach-O segment command\n";
                return false;
            }
            const auto *seg = &*segValue;
            size_t secTableOff = 0;
            size_t secTableSize = 0;
            size_t secTableEnd = 0;
            if (!checkedAdd(tmpOff, sizeof(macho::segment_command_64), secTableOff) ||
                !checkedMul(
                    static_cast<size_t>(seg->nsects), sizeof(macho::section_64), secTableSize) ||
                !checkedAdd(secTableOff, secTableSize, secTableEnd) ||
                !checkedRange(secTableOff, secTableSize, size) || secTableEnd > commandEnd) {
                err << "error: " << name << ": Mach-O section table is out of bounds\n";
                return false;
            }

            size_t secOff = secTableOff;
            for (uint32_t s = 0; s < seg->nsects; ++s) {
                const auto secValue = readAt<macho::section_64>(data, size, secOff);
                if (!secValue) {
                    err << "error: " << name << ": truncated Mach-O section header\n";
                    return false;
                }
                const auto *sec = &*secValue;

                std::string segName = trimNul(sec->segname, 16);
                std::string secName = trimNul(sec->sectname, 16);
                const bool isDebugSection =
                    (segName == "__DWARF" || (sec->flags & macho::S_ATTR_DEBUG) != 0);

                ObjSection os;
                os.name = segName + "," + secName;
                if (sec->align > 30) {
                    err << "error: " << name << ": Mach-O section '" << os.name
                        << "' has unsupported alignment exponent " << sec->align << "\n";
                    return false;
                }
                os.alignment = 1u << sec->align;
                os.executable = (sec->flags & (macho::S_ATTR_PURE_INSTRUCTIONS |
                                               macho::S_ATTR_SOME_INSTRUCTIONS)) != 0;
                os.alloc = !isDebugSection;

                // Infer writability from Mach-O segment name and section type.
                // .o files don't have segment permission bits, but each section header
                // carries the intended segment name (__TEXT vs __DATA).
                const uint32_t secType = sec->flags & 0xFF;
                const bool dataSegment = segName == "__DATA" || segName == "__DATA_CONST" ||
                                         segName == "__DATA_DIRTY" || segName == "__AUTH" ||
                                         segName == "__AUTH_CONST";
                const bool writableDataSegment =
                    segName == "__DATA" || segName == "__DATA_DIRTY" || segName == "__AUTH";
                os.dataSegment = !isDebugSection && dataSegment;
                os.writable =
                    !isDebugSection && (writableDataSegment || (secType == macho::S_ZEROFILL) ||
                                        (secType == macho::S_THREAD_LOCAL_REGULAR) ||
                                        (secType == macho::S_THREAD_LOCAL_ZEROFILL) ||
                                        (secType == macho::S_THREAD_LOCAL_VARIABLES) ||
                                        (secType == macho::S_NON_LAZY_SYMBOL_POINTERS) ||
                                        (secType == macho::S_LAZY_SYMBOL_POINTERS));

                os.tls = (secType == macho::S_THREAD_LOCAL_REGULAR ||
                          secType == macho::S_THREAD_LOCAL_ZEROFILL ||
                          secType == macho::S_THREAD_LOCAL_VARIABLES);

                // S_CSTRING_LITERALS (0x02) sections contain only NUL-terminated
                // C strings and are safe for cross-module string deduplication.
                os.isCStringSection = (secType == 0x02);

                // Zerofill sections (S_ZEROFILL, S_THREAD_LOCAL_ZEROFILL) have
                // offset=0 in .o files — they carry no file data. Reading from
                // offset 0 would incorrectly copy the Mach-O header bytes.
                const bool isZerofill =
                    (secType == macho::S_ZEROFILL) || (secType == macho::S_THREAD_LOCAL_ZEROFILL);
                os.zeroFill = isZerofill;

                // S_ATTR_NO_DEAD_STRIP keeps the section alive regardless of
                // reachability (the section-level form of __attribute__((used))).
                os.noDeadStrip = (sec->flags & macho::S_ATTR_NO_DEAD_STRIP) != 0;

                if (sec->size > static_cast<uint64_t>(SIZE_MAX) ||
                    sec->size > kMaxObjSectionBytes) {
                    err << "error: " << name << ": Mach-O section is too large\n";
                    return false;
                }
                if (!isZerofill && sec->size > kMaxObjMaterializedBytes - materializedBytes) {
                    err << "error: " << name
                        << ": Mach-O materialized section data exceeds limit\n";
                    return false;
                }
                if (isZerofill && sec->size > 0) {
                    os.memSize = static_cast<size_t>(sec->size);
                } else if (sec->size > 0 &&
                           checkedRange(sec->offset, static_cast<size_t>(sec->size), size)) {
                    os.data.assign(data + sec->offset, data + sec->offset + sec->size);
                    os.memSize = os.data.size();
                    materializedBytes += static_cast<size_t>(sec->size);
                } else if (sec->size > 0) {
                    err << "error: " << name << ": Mach-O section '" << os.name
                        << "' contents are out of bounds\n";
                    return false;
                }

                // Parse section relocations.
                // ARM64_RELOC_ADDEND (type 10) is a paired relocation:
                // its r_symbolnum carries the addend for the NEXT relocation.
                int64_t pendingAddend = 0;
                bool hasPendingAddend = false;
                size_t relocTableBytes = 0;
                if (!checkedMul(static_cast<size_t>(sec->nreloc),
                                sizeof(macho::relocation_info),
                                relocTableBytes)) {
                    err << "error: " << name
                        << ": Mach-O relocation table size overflows address space\n";
                    return false;
                }
                if (sec->nreloc > 0 && !checkedRange(sec->reloff, relocTableBytes, size)) {
                    err << "error: " << name << ": Mach-O relocation table is out of bounds\n";
                    return false;
                }
                for (uint32_t r = 0; r < sec->nreloc; ++r) {
                    size_t relocScaled = 0;
                    size_t relocRecordOff = 0;
                    if (!checkedMul(
                            static_cast<size_t>(r), sizeof(macho::relocation_info), relocScaled) ||
                        !checkedAdd(
                            static_cast<size_t>(sec->reloff), relocScaled, relocRecordOff)) {
                        err << "error: " << name
                            << ": Mach-O relocation entry offset overflows address space\n";
                        return false;
                    }
                    const auto riValue = readAt<macho::relocation_info>(data, size, relocRecordOff);
                    if (!riValue) {
                        err << "error: " << name << ": truncated Mach-O relocation entry\n";
                        return false;
                    }
                    const auto *ri = &*riValue;

                    // Scattered relocations carry a different payload layout. Do not
                    // silently drop them; unresolved fixups would corrupt the link.
                    if (ri->r_address & 0x80000000) {
                        err << "error: " << name
                            << ": Mach-O scattered relocations are not supported\n";
                        return false;
                    }

                    const uint32_t info = ri->r_info;
                    const uint32_t symbolNum = info & 0x00FFFFFF;
                    const bool isPcRel = ((info >> 24) & 1) != 0;
                    const bool isExtern = ((info >> 27) & 1) != 0;
                    const uint32_t relLength = (info >> 25) & 0x3;
                    const uint32_t relType = (info >> 28) & 0xF;

                    // ARM64_RELOC_ADDEND (type 10): stores addend for next relocation.
                    if (isArm64 && relType == 10) {
                        if (hasPendingAddend) {
                            err << "error: " << name
                                << ": consecutive ARM64_RELOC_ADDEND entries in Mach-O section "
                                << os.name << "\n";
                            return false;
                        }
                        pendingAddend = signExtend(symbolNum, 24);
                        hasPendingAddend = true;
                        continue;
                    }

                    // SUBTRACTOR relocations (paired with a following UNSIGNED) encode a
                    // symbol difference B - A. Compilers emit them for label-difference
                    // expressions such as C++ exception/typeinfo tables and jump tables.
                    // The applier has no lowering for symbol differences yet, so reject
                    // the pair explicitly here instead of misreading the SUBTRACTOR record
                    // as an ordinary relocation and silently corrupting the fixup.
                    if ((isArm64 && relType == macho_a64::kSubtractor) ||
                        (!isArm64 && relType == macho_x64::kSubtractor)) {
                        err << "error: " << name
                            << ": Mach-O SUBTRACTOR relocations (symbol-difference fixups, e.g. "
                               "C++ exception tables or jump tables) are not yet supported in "
                               "section "
                            << os.name << "\n";
                        return false;
                    }

                    if ((relType == macho_a64::kUnsigned || relType == macho_x64::kUnsigned) &&
                        relLength != 2 && relLength != 3) {
                        err << "error: " << name
                            << ": unsupported Mach-O unsigned relocation length " << relLength
                            << " in section " << os.name << "\n";
                        return false;
                    }
                    const size_t fixupSize =
                        isArm64 && relType != macho_a64::kUnsigned ? 4u : (size_t{1} << relLength);
                    if (!checkedRange(
                            static_cast<size_t>(ri->r_address), fixupSize, os.data.size())) {
                        err << "error: " << name << ": Mach-O relocation at offset "
                            << ri->r_address << " extends beyond section " << os.name << "\n";
                        return false;
                    }

                    ObjReloc rel;
                    rel.offset = static_cast<size_t>(ri->r_address);
                    rel.symIndex = symbolNum; // nlist index or section ordinal.
                    rel.type = relType;
                    rel.pcrel = isPcRel;
                    rel.length = static_cast<uint8_t>(relLength);
                    rel.sectionRelative = !isExtern;

                    if (hasPendingAddend) {
                        rel.addend = pendingAddend;
                        hasPendingAddend = false;
                    } else {
                        // Extract addend from instruction bytes.
                        if (!extractMachOAddend(os.data.data(),
                                                os.data.size(),
                                                rel.offset,
                                                rel.type,
                                                relLength,
                                                isArm64,
                                                rel.addend)) {
                            err << "error: " << name
                                << ": Mach-O relocation addend is malformed or out of bounds in "
                                << "section " << os.name << "\n";
                            return false;
                        }
                    }

                    os.relocs.push_back(rel);
                }
                if (hasPendingAddend) {
                    err << "error: " << name << ": dangling ARM64_RELOC_ADDEND in Mach-O section "
                        << os.name << "\n";
                    return false;
                }

                machoSecMap.push_back(static_cast<uint32_t>(obj.sections.size()));
                secAddrs.push_back(sec->addr);
                obj.sections.push_back(std::move(os));
                if (!checkedAdd(secOff, sizeof(macho::section_64), secOff)) {
                    err << "error: " << name
                        << ": Mach-O section header offset overflows address space\n";
                    return false;
                }
            }
        } else if (lc->cmd == macho::LC_SYMTAB) {
            if (lc->cmdsize < 24) {
                err << "error: " << name << ": malformed Mach-O LC_SYMTAB command size\n";
                return false;
            }
            symtabLcOff = tmpOff;
            haveSymtab = true;
        }

        tmpOff = commandEnd;
    }

    // Validate section count.
    if (obj.sections.size() > kMaxObjSections) {
        err << "error: " << name << ": section count " << obj.sections.size() << " exceeds limit\n";
        return false;
    }

    // Parse symbols from LC_SYMTAB.
    if (haveSymtab) {
        // LC_SYMTAB layout: cmd(4) + cmdsize(4) + symoff(4) + nsyms(4) + stroff(4) + strsize(4)
        if (!checkedRange(symtabLcOff, 24, size)) {
            err << "error: " << name << ": Mach-O LC_SYMTAB is out of bounds\n";
            return false;
        }

        const uint32_t symoff = readLE32(data + symtabLcOff + 8);
        const uint32_t nsyms = readLE32(data + symtabLcOff + 12);
        const uint32_t stroff = readLE32(data + symtabLcOff + 16);
        const uint32_t strsize = readLE32(data + symtabLcOff + 20);

        if (nsyms > kMaxObjSymbols) {
            err << "error: " << name << ": symbol count " << nsyms << " exceeds limit\n";
            return false;
        }
        size_t symtabBytes = 0;
        if (!checkedMul(static_cast<size_t>(nsyms), sizeof(macho::nlist_64), symtabBytes)) {
            err << "error: " << name << ": Mach-O symbol table size overflows address space\n";
            return false;
        }
        if (!checkedRange(symoff, symtabBytes, size) || !checkedRange(stroff, strsize, size)) {
            err << "error: " << name << ": Mach-O symbol table is out of bounds\n";
            return false;
        }

        // Build symbol index map: Mach-O nlist index → ObjFile symbol index.
        // Mach-O relocations reference nlist indices directly.
        std::vector<uint32_t> symMap(nsyms, 0);

        for (uint32_t i = 0; i < nsyms; ++i) {
            size_t symScaled = 0;
            size_t symRecordOff = 0;
            if (!checkedMul(static_cast<size_t>(i), sizeof(macho::nlist_64), symScaled) ||
                !checkedAdd(static_cast<size_t>(symoff), symScaled, symRecordOff)) {
                err << "error: " << name
                    << ": Mach-O symbol table entry offset overflows address space\n";
                return false;
            }
            const auto nlValue = readAt<macho::nlist_64>(data, size, symRecordOff);
            if (!nlValue)
                break;
            const auto *nl = &*nlValue;

            ObjSymbol os;

            // Read name from string table.
            auto symName = readStringOpt(data, size, stroff, strsize, nl->n_strx);
            if (!symName && nl->n_strx != 0) {
                err << "error: " << name << ": Mach-O symbol name offset " << nl->n_strx
                    << " is invalid\n";
                return false;
            }
            os.name = symName.value_or("");
            // Strip leading underscore (Mach-O convention).
            if (!os.name.empty() && os.name[0] == '_')
                os.name.erase(0, 1);

            // Parse binding from n_type.
            const uint8_t nType = nl->n_type & 0x0E; // N_TYPE mask.
            const bool isExtern = (nl->n_type & 0x01) != 0;

            if (nType == 0x00) // N_UNDF
            {
                if (isExtern && nl->n_value != 0) {
                    // Common (tentative) symbol: N_UNDF with a non-zero n_value.
                    // n_value is the required byte size and GET_COMM_ALIGN(n_desc) =
                    // (n_desc >> 8) & 0x0F is the log2 alignment. The linker coalesces
                    // these like ELF SHN_COMMON / COFF common symbols. Without this,
                    // gcc -fcommon tentative definitions would masquerade as ordinary
                    // undefined imports and never coalesce.
                    os.binding = ObjSymbol::Global;
                    os.common = true;
                    os.size = static_cast<size_t>(nl->n_value);
                    const uint32_t alignLog2 = (nl->n_desc >> 8) & 0x0F;
                    os.commonAlignment = size_t{1} << alignLog2;
                    os.sectionIndex = 0;
                } else {
                    os.binding = ObjSymbol::Undefined;
                    // Check for weak imports.
                    if (nl->n_desc & 0x0040) // N_WEAK_REF
                        os.weakExternal = true;
                }
            } else if (nType == 0x02) // N_ABS
            {
                os.binding = isExtern ? ObjSymbol::Global : ObjSymbol::Local;
                os.absolute = true;
            } else if (nType == 0x0E) // N_SECT
            {
                if (isExtern) {
                    if (nl->n_desc & 0x0080) // N_WEAK_DEF
                        os.binding = ObjSymbol::Weak;
                    else
                        os.binding = ObjSymbol::Global;
                } else
                    os.binding = ObjSymbol::Local;
            } else {
                os.binding = ObjSymbol::Local;
            }

            // N_ALT_ENTRY (0x0008): an alternate entry point inside an existing
            // atom. Recorded so subsection splitting does not cut the atom here.
            os.altEntry = (nl->n_desc & 0x0008) != 0;
            // N_NO_DEAD_STRIP (0x0020): __attribute__((used)); its section must be
            // kept alive by dead-strip even if nothing references it.
            os.noDeadStrip = (nl->n_desc & 0x0020) != 0;

            // Map Mach-O 1-based section number to our section index.
            // machoSecMap translates Mach-O indices (which include skipped debug
            // sections) to ObjFile section indices.
            //
            // An n_sect that is past the number of sections we actually saw in
            // the load commands indicates a truncated or malformed Mach-O. We
            // must not silently fall through with sectionIndex = 0 because the
            // symbol would then masquerade as undefined and could turn into a
            // bogus dynamic import downstream. (n_sect == 0 with N_SECT type
            // is also illegal per the Mach-O ABI.) machoSecMap[n_sect] == 0
            // for a non-debug section is legitimate — it means the section
            // was skipped (e.g. it was a debug section) — so let that case
            // pass through with os.sectionIndex left at 0.
            if (nl->n_sect > 0 && nl->n_sect < machoSecMap.size()) {
                os.sectionIndex = machoSecMap[nl->n_sect];
            } else if (nType == 0x0E) {
                err << "error: " << name << ": Mach-O symbol '" << os.name
                    << "' references section " << static_cast<unsigned>(nl->n_sect)
                    << " which is outside the parsed section table (size=" << machoSecMap.size()
                    << ")\n";
                return false;
            }
            // Convert Mach-O absolute n_value to section-relative offset.
            // n_value is an address in the .o's virtual space; subtract section base.
            if (nl->n_sect > 0 && nl->n_sect < secAddrs.size() && os.sectionIndex > 0) {
                const uint64_t base = secAddrs[nl->n_sect];
                if (nl->n_value < base) {
                    err << "error: " << name << ": Mach-O symbol '" << os.name
                        << "' value is before its section base\n";
                    return false;
                }
                const uint64_t relValue = nl->n_value - base;
                if (relValue > static_cast<uint64_t>(SIZE_MAX)) {
                    err << "error: " << name << ": Mach-O symbol '" << os.name
                        << "' offset exceeds addressable size\n";
                    return false;
                }
                const size_t secSize = objSectionMemSize(obj.sections[os.sectionIndex]);
                if (relValue > static_cast<uint64_t>(secSize)) {
                    err << "error: " << name << ": Mach-O symbol '" << os.name
                        << "' is outside section '" << obj.sections[os.sectionIndex].name << "'\n";
                    return false;
                }
                os.offset = static_cast<size_t>(relValue);
            } else if (!os.common)
                // Common symbols keep offset 0; their n_value is the size, not an
                // in-section offset (handled above).
                os.offset = static_cast<size_t>(nl->n_value);

            symMap[i] = static_cast<uint32_t>(obj.symbols.size());
            obj.symbols.push_back(std::move(os));
        }

        // Fix up relocation symbol indices from nlist indices to ObjFile indices.
        std::vector<uint32_t> sectionSymMap(machoSecMap.size(), 0);
        /// Returns or synthesizes a local section symbol for a non-external relocation.
        auto sectionSymbolFor = [&](uint32_t machoSectionOrdinal) -> uint32_t {
            if (machoSectionOrdinal >= machoSecMap.size())
                return 0;
            const uint32_t objSecIdx = machoSecMap[machoSectionOrdinal];
            if (objSecIdx == 0)
                return 0;
            uint32_t &cached = sectionSymMap[machoSectionOrdinal];
            if (cached != 0)
                return cached;
            ObjSymbol sym;
            sym.name = "$sect." + std::to_string(machoSectionOrdinal);
            sym.binding = ObjSymbol::Local;
            sym.sectionIndex = objSecIdx;
            sym.offset = 0;
            cached = static_cast<uint32_t>(obj.symbols.size());
            obj.symbols.push_back(std::move(sym));
            return cached;
        };
        for (auto &sec : obj.sections) {
            for (auto &rel : sec.relocs) {
                if (rel.sectionRelative) {
                    const uint32_t mapped = sectionSymbolFor(rel.symIndex);
                    if (mapped == 0) {
                        err << "error: " << name
                            << ": relocation references unmapped Mach-O section " << rel.symIndex
                            << "\n";
                        return false;
                    }
                    rel.symIndex = mapped;
                    rel.sectionRelative = false;
                } else if (rel.symIndex < symMap.size()) {
                    rel.symIndex = symMap[rel.symIndex];
                } else {
                    err << "error: " << name << ": relocation references invalid symbol index "
                        << rel.symIndex << "\n";
                    return false;
                }
            }
        }
    } else {
        for (const auto &sec : obj.sections) {
            if (!sec.relocs.empty()) {
                err << "error: " << name
                    << ": Mach-O relocations require LC_SYMTAB for symbol mapping\n";
                return false;
            }
        }
    }

    if ((hdr->flags & macho::MH_SUBSECTIONS_VIA_SYMBOLS) != 0)
        splitMachOTextSubsections(obj);

    return true;
}

} // namespace zanna::codegen::linker
