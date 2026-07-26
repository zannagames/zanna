//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/MachOWriter.cpp
// Purpose: Serialize CodeSection data into a valid Mach-O relocatable object
//          file for macOS (x86_64 and AArch64/arm64).
// Key invariants:
//   - File layout: header | load commands | __text | __const | relocs |
//     symtab | strtab
//   - Symbols ordered: locals, external defined, undefined (matches LC_DYSYMTAB)
//   - Darwin underscore prefix applied to all non-local-label symbols
//   - Relocations packed into 8-byte entries, sorted by address descending
//   - x86_64 PC-relative addends (-4) patched into instruction bytes
// Ownership/Lifetime:
//   - Stateless between write() calls
// Links: codegen/common/objfile/MachOWriter.hpp
//        plans/05-macho-writer.md
//
//===----------------------------------------------------------------------===//

/**
 * @file MachOWriter.cpp
 * @brief Implements x86-64 and arm64 Mach-O relocatable-object serialization.
 *
 * The writer produces `MH_OBJECT` files with ordered local/defined/undefined
 * symbols, Darwin name mangling, descending relocation groups, optional data,
 * compact-unwind, and debug-line sections, and embedded REL-style addends.
 * Section-offset targets use synthetic anchors when an arm64 signed addend
 * cannot directly represent the requested offset.
 */

#include "codegen/common/objfile/MachOWriter.hpp"
#include "codegen/common/MachOBuildVersion.hpp"
#include "codegen/common/objfile/ObjFileWriterUtil.hpp"
#include "codegen/common/objfile/RelocationValidation.hpp"
#include "codegen/common/objfile/StringTable.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::objfile {

// =============================================================================
// Mach-O Constants
// =============================================================================

// Header
static constexpr uint32_t kMhMagic64 = 0xFEEDFACF;
static constexpr uint32_t kCpuTypeX86_64 = 0x01000007;
static constexpr uint32_t kCpuTypeArm64 = 0x0100000C;
static constexpr uint32_t kCpuSubtypeX86_64All = 3;
static constexpr uint32_t kCpuSubtypeArm64All = 0;
static constexpr uint32_t kMhObject = 1;
static constexpr uint32_t kMhSubsectionsViaSymbols = 0x00002000;

// Load command types
static constexpr uint32_t kLcSegment64 = 0x19;
static constexpr uint32_t kLcSymtab = 0x02;
static constexpr uint32_t kLcDysymtab = 0x0B;
static constexpr uint32_t kLcBuildVersion = 0x32;

// Section flags
static constexpr uint32_t kSAttrPureInstructions = 0x80000000;
static constexpr uint32_t kSAttrSomeInstructions = 0x00000400;

// Relocation types — x86_64
static constexpr uint32_t kX86_64RelocUnsigned = 0;
static constexpr uint32_t kX86_64RelocSigned = 1;
static constexpr uint32_t kX86_64RelocBranch = 2;

// Relocation types — AArch64
static constexpr uint32_t kArm64RelocBranch26 = 2;
static constexpr uint32_t kArm64RelocPage21 = 3;
static constexpr uint32_t kArm64RelocPageoff12 = 4;
static constexpr uint32_t kArm64RelocAddend = 10;

// nlist_64 type field
static constexpr uint8_t kNUndf = 0x00;
static constexpr uint8_t kNSect = 0x0E;
static constexpr uint8_t kNExt = 0x01;

// Section ordinals (1-based in Mach-O). __data follows __const so existing
// __text/__const symbol `sect` values are unchanged; __compact_unwind and
// __debug_line carry no nlist symbols, so their shifted ordinals are inert.
static constexpr uint8_t kSectText = 1;
static constexpr uint8_t kSectConst = 2;
static constexpr uint8_t kSectData = 3;
static constexpr uint8_t kNoSect = 0;

// Load command sizes (fixed, except segment command which varies with section count)
// Segment cmd size = 72 (header) + nsects * 80 (section headers), computed dynamically.
static constexpr uint32_t kBuildVerCmdSize = 24;
static constexpr uint32_t kSymtabCmdSize = 24;
static constexpr uint32_t kDysymtabCmdSize = 80;
static constexpr uint32_t kHeaderSize = 32;
static constexpr uint32_t kNlistSize = 16;
static constexpr uint32_t kRelocSize = 8;

// Section attribute flags
static constexpr uint32_t kSAttrDebug = 0x02000000;

// Helpers: appendLE16/32/64, alignUp, padTo are provided by ObjFileWriterUtil.hpp.

/// @brief Recognize assembler-local Mach-O label spellings.
/// @param name Unmangled encoder symbol name.
/// @return `true` when the name should remain local and unprefixed.
static bool isMachOLocalLabelName(const std::string &name) {
    return !name.empty() && (name[0] == '.' || name.rfind("L.", 0) == 0 ||
                             name.rfind("Ltmp", 0) == 0 || name.rfind("LBB", 0) == 0);
}

/// @brief Apply Darwin's leading-underscore convention to an nlist symbol.
/// @param sym Encoder symbol whose binding and name determine mangling.
/// @return Original local-label spelling or underscore-prefixed external spelling.
static std::string mangleName(const Symbol &sym) {
    const std::string &name = sym.name;
    if (name.empty())
        return name;
    if (sym.binding == SymbolBinding::Local || isMachOLocalLabelName(name))
        return name;
    return "_" + name;
}

/// @brief Packs a Mach-O relocation information field.
/// @details Uses the little-endian bit-field layout
/// Layout: symbolnum[23:0] | pcrel[24] | length[26:25] | extern[27] | type[31:28]
/// @param symbolnum External symbol index or local section ordinal.
/// @param pcrel Whether the relocation is PC relative.
/// @param length Base-two width exponent of the relocated field.
/// @param ext Whether @p symbolnum is an external symbol-table index.
/// @param type Architecture-specific relocation type.
/// @return Packed 32-bit `relocation_info` bit field.
static uint32_t packRelocInfo(
    uint32_t symbolnum, uint8_t pcrel, uint8_t length, uint8_t ext, uint8_t type) {
    return (symbolnum & 0x00FFFFFF) | (static_cast<uint32_t>(pcrel & 1) << 24) |
           (static_cast<uint32_t>(length & 3) << 25) | (static_cast<uint32_t>(ext & 1) << 27) |
           (static_cast<uint32_t>(type & 0xF) << 28);
}

/// @brief Narrow a native size or offset to a 32-bit Mach-O field.
/// @param value Native-size value to validate.
/// @param what Human-readable field description.
/// @param err Stream that receives an overflow diagnostic.
/// @param out Receives the narrowed value.
/// @return `true` when @p value fits in `uint32_t`.
static bool checkedU32(size_t value, const char *what, std::ostream &err, uint32_t &out) {
    if (value > std::numeric_limits<uint32_t>::max()) {
        err << "MachOWriter: " << what << " exceeds 32-bit Mach-O field range\n";
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

/// @brief Validate Mach-O's 24-bit relocation symbol-number field.
/// @param value Symbol table index or packed addend value.
/// @param what Human-readable field description.
/// @param err Stream that receives a range diagnostic.
/// @return `true` when @p value is at most `0x00ffffff`.
static bool checkedRelocSymbolNum(uint32_t value, const char *what, std::ostream &err) {
    if (value > 0x00FFFFFFu) {
        err << "MachOWriter: " << what << " exceeds Mach-O relocation symbol-number range\n";
        return false;
    }
    return true;
}

/// @brief Adapts the shared symbol-value validator to Mach-O writer diagnostics.
///
/// Adapter to the shared @ref zanna::codegen::objfile::physicalSymbolValue helper
/// that pins the writerName to "MachOWriter:" so existing call sites compile
/// unchanged.
/// @param section Section supplying logical bias and payload bounds.
/// @param sym Defined symbol to convert.
/// @param sectionName Name used to qualify diagnostics.
/// @param err Stream that receives validation diagnostics.
/// @param out Receives the physical section-relative value.
/// @return `true` when @p sym lies within @p section.
static bool physicalSymbolValue(const CodeSection &section,
                                const Symbol &sym,
                                const char *sectionName,
                                std::ostream &err,
                                uint64_t &out) {
    return zanna::codegen::objfile::physicalSymbolValue(
        section, sym, sectionName, "MachOWriter", err, out);
}

// =============================================================================
// Mach-O Struct Writers
// =============================================================================

/// @brief Append one 32-byte `mach_header_64`.
/// @param out Destination object buffer.
/// @param cputype Mach-O CPU type.
/// @param cpusubtype Mach-O CPU subtype.
/// @param ncmds Number of following load commands.
/// @param sizeofcmds Combined load-command byte count.
/// @param flags Mach-O header flags.
static void writeMachOHeader(std::vector<uint8_t> &out,
                             uint32_t cputype,
                             uint32_t cpusubtype,
                             uint32_t ncmds,
                             uint32_t sizeofcmds,
                             uint32_t flags) {
    appendLE32(out, kMhMagic64);
    appendLE32(out, cputype);
    appendLE32(out, cpusubtype);
    appendLE32(out, kMhObject);
    appendLE32(out, ncmds);
    appendLE32(out, sizeofcmds);
    appendLE32(out, flags);
    appendLE32(out, 0); // reserved
}

/// @brief Append the 72-byte `LC_SEGMENT_64` command prefix.
/// @param out Destination load-command buffer.
/// @param cmdsize Total command size including section records.
/// @param vmsize Combined in-memory section extent.
/// @param fileoff File offset of the first section payload.
/// @param filesize Combined file-backed section span.
/// @param nsects Number of following `section_64` records.
static void writeSegmentCmd(std::vector<uint8_t> &out,
                            uint32_t cmdsize,
                            uint64_t vmsize,
                            uint64_t fileoff,
                            uint64_t filesize,
                            uint32_t nsects) {
    appendLE32(out, kLcSegment64);
    appendLE32(out, cmdsize);
    // segname: 16 zero bytes (unnamed for .o files)
    for (int i = 0; i < 16; ++i)
        out.push_back(0);
    appendLE64(out, 0); // vmaddr
    appendLE64(out, vmsize);
    appendLE64(out, fileoff);
    appendLE64(out, filesize);
    appendLE32(out, 7); // maxprot = rwx
    appendLE32(out, 7); // initprot = rwx
    appendLE32(out, nsects);
    appendLE32(out, 0); // flags
}

/// @brief Append a fixed-width, zero-padded Mach-O name field.
/// @details Mach-O section_64 headers store section and segment names as raw
///          16-byte fields. This helper uses explicit bounded copying rather
///          than strncpy so callers get deterministic padding and no accidental
///          dependence on C-string termination.
/// @param out Destination object-file buffer.
/// @param name Section or segment name to serialize.
/// @param width Required fixed field width in bytes.
static void appendFixedNameField(std::vector<uint8_t> &out, std::string_view name, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        const uint8_t byte = i < name.size() ? static_cast<uint8_t>(name[i]) : 0;
        out.push_back(byte);
    }
}

/// @brief Append one 80-byte Mach-O `section_64` record.
/// @param out Destination segment command.
/// @param sectname Sixteen-byte section name field.
/// @param segname Sixteen-byte segment name field.
/// @param addr Segment-relative section address.
/// @param size Section byte count.
/// @param offset File offset of section bytes.
/// @param align Base-two alignment exponent.
/// @param reloff File offset of relocation records.
/// @param nreloc Number of relocation records.
/// @param flags Section type and attributes.
static void writeSectionHdr(std::vector<uint8_t> &out,
                            std::string_view sectname,
                            std::string_view segname,
                            uint64_t addr,
                            uint64_t size,
                            uint32_t offset,
                            uint32_t align,
                            uint32_t reloff,
                            uint32_t nreloc,
                            uint32_t flags) {
    // sectname: 16 bytes, zero-padded
    appendFixedNameField(out, sectname, 16);
    // segname: 16 bytes, zero-padded
    appendFixedNameField(out, segname, 16);
    appendLE64(out, addr);
    appendLE64(out, size);
    appendLE32(out, offset);
    appendLE32(out, align); // log2 of alignment
    appendLE32(out, reloff);
    appendLE32(out, nreloc);
    appendLE32(out, flags);
    appendLE32(out, 0); // reserved1
    appendLE32(out, 0); // reserved2
    appendLE32(out, 0); // reserved3
}

/// @brief Append the fixed 24-byte macOS `LC_BUILD_VERSION` command.
/// @param out Destination load-command buffer.
static void writeBuildVersionCmd(std::vector<uint8_t> &out) {
    appendLE32(out, kLcBuildVersion);
    appendLE32(out, kBuildVerCmdSize);
    appendLE32(out, zanna::codegen::macho::kPlatformMacOS);
    appendLE32(out, zanna::codegen::macho::minimumMacOSVersion());
    appendLE32(out, zanna::codegen::macho::macOSSDKVersion());
    appendLE32(out, 0); // ntools
}

/// @brief Append one 24-byte `LC_SYMTAB` command.
/// @param out Destination load-command buffer.
/// @param symoff File offset of `nlist_64` records.
/// @param nsyms Number of symbol records.
/// @param stroff File offset of the string table.
/// @param strsize String-table byte count.
static void writeSymtabCmd(
    std::vector<uint8_t> &out, uint32_t symoff, uint32_t nsyms, uint32_t stroff, uint32_t strsize) {
    appendLE32(out, kLcSymtab);
    appendLE32(out, kSymtabCmdSize);
    appendLE32(out, symoff);
    appendLE32(out, nsyms);
    appendLE32(out, stroff);
    appendLE32(out, strsize);
}

/// @brief Append one 80-byte `LC_DYSYMTAB` command.
/// @param out Destination load-command buffer.
/// @param ilocal First local-symbol index.
/// @param nlocal Number of local symbols.
/// @param iextdef First externally defined symbol index.
/// @param nextdef Number of externally defined symbols.
/// @param iundef First undefined symbol index.
/// @param nundef Number of undefined symbols.
static void writeDysymtabCmd(std::vector<uint8_t> &out,
                             uint32_t ilocal,
                             uint32_t nlocal,
                             uint32_t iextdef,
                             uint32_t nextdef,
                             uint32_t iundef,
                             uint32_t nundef) {
    appendLE32(out, kLcDysymtab);
    appendLE32(out, kDysymtabCmdSize);
    appendLE32(out, ilocal);
    appendLE32(out, nlocal);
    appendLE32(out, iextdef);
    appendLE32(out, nextdef);
    appendLE32(out, iundef);
    appendLE32(out, nundef);
    // Remaining 12 fields (tocoff through nlocrel) are all zero.
    for (int i = 0; i < 12; ++i)
        appendLE32(out, 0);
}

/// @brief Append one 16-byte `nlist_64` symbol record.
/// @param out Destination symbol table.
/// @param strx String-table offset of the name.
/// @param type Packed nlist type and external bits.
/// @param sect One-based defining section ordinal.
/// @param desc Symbol descriptor bits.
/// @param value Segment-relative symbol value.
static void writeNlist(std::vector<uint8_t> &out,
                       uint32_t strx,
                       uint8_t type,
                       uint8_t sect,
                       uint16_t desc,
                       uint64_t value) {
    appendLE32(out, strx);
    out.push_back(type);
    out.push_back(sect);
    appendLE16(out, desc);
    appendLE64(out, value);
}

/// @brief Append one eight-byte Mach-O relocation record.
/// @param out Destination relocation table.
/// @param address Section-relative fixup address.
/// @param packed Packed relocation attributes from @ref packRelocInfo.
static void writeMachoReloc(std::vector<uint8_t> &out, uint32_t address, uint32_t packed) {
    appendLE32(out, address);
    appendLE32(out, packed);
}

// =============================================================================
// Relocation Mapping
// =============================================================================

/// @brief Mach-O encoding attributes selected for one relocation kind.
struct MachoRelocAttrs {
    ///< Architecture-specific relocation type.
    uint8_t type;
    ///< Whether the relocation is PC relative.
    uint8_t pcrel;
    ///< Base-two relocated-field width exponent.
    uint8_t length;
    ///< Whether no Mach-O representation exists.
    bool skip; // true if this reloc kind has no Mach-O equivalent
};

/// @brief One serialized relocation record before final ordering.
struct MachoReloc {
    ///< Section-relative relocation address.
    uint32_t address = 0;
    ///< Packed symbol and relocation attributes.
    uint32_t packed = 0;
};

/// @brief Adjacent relocation records that must remain together at one address.
/// @details arm64 nonzero addends require an `ARM64_RELOC_ADDEND` record
///          immediately preceding the primary relocation.
struct MachoRelocGroup {
    ///< Shared address used to order the group.
    uint32_t address = 0;
    ///< Records in required emission order.
    std::vector<MachoReloc> entries;
};

/// @brief Select Mach-O relocation attributes for an encoder relocation kind.
/// @param kind Architecture-neutral relocation semantics.
/// @return Encoding attributes, with `skip` set when Mach-O has no equivalent.
static MachoRelocAttrs machoRelocAttrs(RelocKind kind) {
    switch (kind) {
        // x86_64
        case RelocKind::PCRel32:
            return {static_cast<uint8_t>(kX86_64RelocSigned), 1, 2, false};
        case RelocKind::Branch32:
            return {static_cast<uint8_t>(kX86_64RelocBranch), 1, 2, false};
        case RelocKind::Abs64:
            return {static_cast<uint8_t>(kX86_64RelocUnsigned), 0, 3, false};
        // AArch64
        case RelocKind::A64Call26:
        case RelocKind::A64Jump26:
            return {static_cast<uint8_t>(kArm64RelocBranch26), 1, 2, false};
        case RelocKind::A64AdrpPage21:
            return {static_cast<uint8_t>(kArm64RelocPage21), 1, 2, false};
        case RelocKind::A64AddPageOff12:
        case RelocKind::A64LdSt32Off12:
        case RelocKind::A64LdSt64Off12:
        case RelocKind::A64LdSt128Off12:
            return {static_cast<uint8_t>(kArm64RelocPageoff12), 0, 2, false};
        case RelocKind::A64CondBr19:
            // Mach-O has no ARM64_RELOC_BRANCH19; conditional branches are
            // always resolved internally by the encoder.
            return {0, 0, 0, true};
    }
    return {0, 0, 0, true};
}

// =============================================================================
// MachOWriter::write
// =============================================================================

/// @copydoc MachOWriter::write(const std::string&, const CodeSection&,
///                             const CodeSection&, std::ostream&)
bool MachOWriter::write(const std::string &path,
                        const CodeSection &text,
                        const CodeSection &rodata,
                        std::ostream &err) {
    try {
        // --- Architecture-specific parameters ---
        uint32_t cputype, cpusubtype;
        uint32_t textAlignLog2;
        if (arch_ == ObjArch::X86_64) {
            cputype = kCpuTypeX86_64;
            cpusubtype = kCpuSubtypeX86_64All;
            textAlignLog2 = 4; // 2^4 = 16
        } else {
            cputype = kCpuTypeArm64;
            cpusubtype = kCpuSubtypeArm64All;
            textAlignLog2 = 2; // 2^2 = 4
        }
        size_t textAlign = static_cast<size_t>(1) << textAlignLog2;
        constexpr uint32_t constAlignLog2 = 3; // 2^3 = 8
        constexpr size_t constAlign = 8;
        constexpr uint32_t dataAlignLog2 = 3; // 2^3 = 8
        constexpr size_t dataAlign = 8;

        // Writable initialized-data section (__DATA,__data). Empty unless a prior
        // setDataSection() supplied scalar globals; symbols here satisfy the text
        // section's undefined references to mutable globals by name coalescing.
        const CodeSection emptyData;
        const CodeSection &data = dataSection_ ? *dataSection_ : emptyData;
        const bool hasData = !data.bytes().empty();

        // --- 1. Build string table with Darwin mangled names ---
        StringTable strtab;
        strtab.add(" "); // Mach-O convention: space at offset 1

        // --- 2. Collect and categorize symbols ---
        /// @brief Deferred nlist symbol awaiting Mach-O category ordering.
        struct PendingSym {
            ///< Original CodeSection symbol index.
            uint32_t encoderIdx = 0;
            ///< Whether the source is the text rather than const/data section.
            bool fromText = false;
            ///< Whether this is a generated exact-offset anchor.
            bool syntheticOffsetAnchor = false;
            ///< String-table offset of the mangled name.
            uint32_t strx = 0;
            ///< Packed nlist type and external bits.
            uint8_t type = 0;
            ///< Defining section ordinal.
            uint8_t sect = 0;
            ///< Section-relative value before final segment rebasing.
            uint64_t value = 0;
            ///< Darwin-mangled name used for global coalescing.
            std::string mangledName;
        };

        std::vector<PendingSym> pendingLocals, pendingExtDef, pendingUndef;

        // Track coalescible non-local names. Local symbols must remain distinct;
        // undefined external symbols are never satisfied by a local same-spelled
        // symbol unless an explicit cross-section relocation target is present.
        std::unordered_map<std::string, uint32_t> definedGlobalNameToMachoIdx;
        std::unordered_map<std::string, uint32_t> undefinedNameToMachoIdx;

        // Map from (CodeSection symbol index) → Mach-O symbol table index.
        std::unordered_map<uint32_t, uint32_t> textSymMap;
        std::unordered_map<uint32_t, uint32_t> rodataSymMap;
        std::unordered_map<uint32_t, uint32_t> dataSymMap;
        std::unordered_map<size_t, uint32_t> textOffsetAnchorMap;
        std::unordered_map<size_t, uint32_t> constOffsetAnchorMap;

        /// Test whether an addend fits signed 24-bit `ARM64_RELOC_ADDEND`.
        auto fitsArm64RelocAddend = [](int64_t value) {
            return value >= -0x800000LL && value <= 0x7FFFFFLL;
        };

        /// Combine a relocation's signed addend with its concrete target offset.
        auto sectionOffsetAddend =
            [&](const Relocation &rel, const char *sectionName, int64_t &out) -> bool {
            return checkedSectionOffsetAddend(
                rel.addend, rel.targetOffset, "MachOWriter", sectionName, rel.offset, err, out);
        };

        /// Determine whether exact offsets into a target class require a base anchor.
        auto relocationNeedsAnchor = [](const CodeSection &sec, SymbolSection targetSection) {
            for (const auto &rel : sec.relocations())
                if (rel.targetOffsetValid && rel.targetSection == targetSection)
                    return true;
            return false;
        };

        /// Determine whether an external encoder symbol has any ordinary
        /// undefined relocation and therefore needs an undefined nlist entry.
        auto externalNeedsUndefinedSymbol = [](const CodeSection &sec, uint32_t symbolIndex) {
            bool sawRelocation = false;
            for (const auto &rel : sec.relocations()) {
                if (rel.symbolIndex != symbolIndex)
                    continue;
                sawRelocation = true;
                if (rel.targetSection == SymbolSection::Undefined)
                    return true;
            }
            return !sawRelocation;
        };

        /// Logical input role used to assign Mach-O section ordinals and names.
        enum class SecRole { Text, Const, Data };

        /// Categorize one section's symbols as local, externally defined, or
        /// undefined while computing physical values and Darwin spellings.
        auto processSymbols = [&](const CodeSection &sec, SecRole role) -> bool {
            const bool isText = role == SecRole::Text;
            const uint8_t definedSect = role == SecRole::Text    ? kSectText
                                        : role == SecRole::Const ? kSectConst
                                                                 : kSectData;
            const char *sectName = role == SecRole::Text    ? "__text"
                                   : role == SecRole::Const ? "__const"
                                                            : "__data";
            for (uint32_t i = 1; i < sec.symbols().count(); ++i) {
                const Symbol &s = sec.symbols().at(i);
                if (s.binding == SymbolBinding::External && !externalNeedsUndefinedSymbol(sec, i))
                    continue;

                std::string mangled = mangleName(s);
                uint32_t strOff = strtab.add(mangled);

                PendingSym ps{};
                ps.encoderIdx = i;
                ps.fromText = isText;
                ps.strx = strOff;
                ps.value = 0;
                ps.mangledName = mangled;

                if (s.binding == SymbolBinding::External) {
                    ps.type = kNUndf | kNExt;
                    ps.sect = kNoSect;
                    pendingUndef.push_back(ps);
                } else if (s.binding == SymbolBinding::Local) {
                    ps.type = kNSect;
                    ps.sect = definedSect;
                    if (!physicalSymbolValue(sec, s, sectName, err, ps.value))
                        return false;
                    pendingLocals.push_back(ps);
                } else {
                    // Global defined.
                    ps.type = kNSect | kNExt;
                    ps.sect = definedSect;
                    if (!physicalSymbolValue(sec, s, sectName, err, ps.value))
                        return false;
                    pendingExtDef.push_back(ps);
                }
            }
            return true;
        };

        if (!processSymbols(text, SecRole::Text) || !processSymbols(rodata, SecRole::Const))
            return false;
        if (hasData && !processSymbols(data, SecRole::Data))
            return false;

        std::unordered_set<size_t> textOffsetAnchors;
        std::unordered_set<size_t> constOffsetAnchors;
        /// Generate local symbols at exact target offsets whose arm64 addends
        /// exceed the signed 24-bit relocation-addend range.
        auto collectLargeOffsetAnchors = [&](const CodeSection &source,
                                             const char *sectionName) -> bool {
            if (arch_ != ObjArch::AArch64)
                return true;
            for (const auto &rel : source.relocations()) {
                if (!rel.targetOffsetValid || rel.targetSection == SymbolSection::Undefined)
                    continue;
                const CodeSection &target =
                    (rel.targetSection == SymbolSection::Text) ? text : rodata;
                const char *targetName =
                    (rel.targetSection == SymbolSection::Text) ? "__text" : "__const";
                if (rel.targetOffset > target.bytes().size()) {
                    err << "MachOWriter: relocation in " << sectionName << " at offset "
                        << rel.offset << " references " << targetName << " offset "
                        << rel.targetOffset << " beyond section contents\n";
                    return false;
                }
                int64_t effectiveAddend = 0;
                if (!sectionOffsetAddend(rel, sectionName, effectiveAddend))
                    return false;
                const bool needsAnchor = !fitsArm64RelocAddend(effectiveAddend);
                if (!needsAnchor)
                    continue;
                auto &seen = (rel.targetSection == SymbolSection::Text) ? textOffsetAnchors
                                                                        : constOffsetAnchors;
                if (!seen.insert(rel.targetOffset).second)
                    continue;
                pendingLocals.push_back(
                    PendingSym{0,
                               rel.targetSection == SymbolSection::Text,
                               true,
                               0,
                               kNSect,
                               rel.targetSection == SymbolSection::Text ? kSectText : kSectConst,
                               static_cast<uint64_t>(rel.targetOffset),
                               ""});
            }
            return true;
        };

        if (!collectLargeOffsetAnchors(text, "__text") ||
            !collectLargeOffsetAnchors(rodata, "__const"))
            return false;

        const bool needTextAnchor = relocationNeedsAnchor(text, SymbolSection::Text) ||
                                    relocationNeedsAnchor(rodata, SymbolSection::Text);
        const bool needConstAnchor = relocationNeedsAnchor(text, SymbolSection::Rodata) ||
                                     relocationNeedsAnchor(rodata, SymbolSection::Rodata);
        if (needTextAnchor)
            pendingLocals.push_back(PendingSym{0, true, false, 0, kNSect, kSectText, 0, ""});
        if (needConstAnchor)
            pendingLocals.push_back(PendingSym{0, false, false, 0, kNSect, kSectConst, 0, ""});

        // --- 3. Assign Mach-O symbol indices ---
        // Order: locals → external defined → undefined.
        uint32_t machoIdx = 0;

        /// Assign the next nlist index and update the source section's remap.
        auto assignFreshIndex = [&](const PendingSym &ps) {
            if (ps.syntheticOffsetAnchor) {
                if (ps.fromText)
                    textOffsetAnchorMap[static_cast<size_t>(ps.value)] = machoIdx;
                else
                    constOffsetAnchorMap[static_cast<size_t>(ps.value)] = machoIdx;
            } else if (ps.sect == kSectData) {
                dataSymMap[ps.encoderIdx] = machoIdx;
            } else if (ps.fromText) {
                textSymMap[ps.encoderIdx] = machoIdx;
            } else {
                rodataSymMap[ps.encoderIdx] = machoIdx;
            }
            ++machoIdx;
        };

        /// Assign indices to all local symbols without name coalescing.
        auto assignLocals = [&](std::vector<PendingSym> &syms) {
            for (auto &ps : syms)
                assignFreshIndex(ps);
        };

        /// Assign externally defined symbols and reject duplicate global names.
        auto assignDefinedGlobals = [&](std::vector<PendingSym> &syms) {
            for (auto &ps : syms) {
                if (definedGlobalNameToMachoIdx.count(ps.mangledName)) {
                    err << "MachOWriter: duplicate global symbol '" << ps.mangledName << "'\n";
                    return false;
                }
                definedGlobalNameToMachoIdx[ps.mangledName] = machoIdx;
                assignFreshIndex(ps);
            }
            return true;
        };

        /// Coalesce undefined names with earlier definitions or undefined entries.
        auto assignUndefineds = [&](std::vector<PendingSym> &syms) {
            for (auto &ps : syms) {
                auto defIt = definedGlobalNameToMachoIdx.find(ps.mangledName);
                if (defIt != definedGlobalNameToMachoIdx.end()) {
                    if (ps.fromText)
                        textSymMap[ps.encoderIdx] = defIt->second;
                    else
                        rodataSymMap[ps.encoderIdx] = defIt->second;
                    continue;
                }
                auto undefIt = undefinedNameToMachoIdx.find(ps.mangledName);
                if (undefIt != undefinedNameToMachoIdx.end()) {
                    if (ps.fromText)
                        textSymMap[ps.encoderIdx] = undefIt->second;
                    else
                        rodataSymMap[ps.encoderIdx] = undefIt->second;
                    continue;
                }
                undefinedNameToMachoIdx[ps.mangledName] = machoIdx;
                assignFreshIndex(ps);
            }
        };

        assignLocals(pendingLocals);
        uint32_t ilocal = 0;
        uint32_t nlocal = machoIdx;

        if (!assignDefinedGlobals(pendingExtDef))
            return false;
        uint32_t iextdef = nlocal;
        uint32_t nextdef = machoIdx - nlocal;

        assignUndefineds(pendingUndef);
        uint32_t iundef = iextdef + nextdef;
        uint32_t nundef = machoIdx - iundef;

        uint32_t nsyms = machoIdx;
        const uint32_t textAnchorIdx =
            needTextAnchor && textSymMap.count(0) ? textSymMap[0] : UINT32_MAX;
        const uint32_t constAnchorIdx =
            needConstAnchor && rodataSymMap.count(0) ? rodataSymMap[0] : UINT32_MAX;

        // Build the final symbol list (deduplicated, in order).
        struct FinalSym {
            uint32_t strx;
            uint8_t type;
            uint8_t sect;
            uint64_t value;
        };

        std::vector<FinalSym> allSyms(nsyms);
        /// Append nlist records in their assigned category order, optionally
        /// omitting undefined names satisfied by emitted global definitions.
        auto emitSyms = [&](const std::vector<PendingSym> &syms, bool skipIfDefined) {
            for (const auto &ps : syms) {
                uint32_t idx = 0;
                if (ps.syntheticOffsetAnchor) {
                    const auto &anchorMap =
                        ps.fromText ? textOffsetAnchorMap : constOffsetAnchorMap;
                    auto anchorIt = anchorMap.find(static_cast<size_t>(ps.value));
                    if (anchorIt == anchorMap.end())
                        continue;
                    idx = anchorIt->second;
                } else if (ps.sect == kSectData) {
                    idx = dataSymMap[ps.encoderIdx];
                } else {
                    idx = (ps.fromText) ? textSymMap[ps.encoderIdx] : rodataSymMap[ps.encoderIdx];
                }
                // When an undefined symbol in text duplicates a defined symbol in
                // rodata, the defined version must win (they share the same Mach-O
                // index). Skip the undefined overwrite when the slot already holds
                // a defined symbol.
                if (skipIfDefined && allSyms[idx].type != 0)
                    continue;
                allSyms[idx] = FinalSym{ps.strx, ps.type, ps.sect, ps.value};
            }
        };
        emitSyms(pendingLocals, false);
        emitSyms(pendingExtDef, false);
        emitSyms(pendingUndef, true);

        std::unordered_map<std::string, uint32_t> definedRodataByName;
        for (uint32_t i = 1; i < rodata.symbols().count(); ++i) {
            const Symbol &sym = rodata.symbols().at(i);
            if (sym.binding == SymbolBinding::External)
                continue;
            auto it = rodataSymMap.find(i);
            if (it != rodataSymMap.end()) {
                auto [nameIt, inserted] = definedRodataByName.emplace(sym.name, it->second);
                if (!inserted)
                    nameIt->second = UINT32_MAX;
            }
        }

        std::unordered_map<std::string, uint32_t> definedTextByName;
        for (uint32_t i = 1; i < text.symbols().count(); ++i) {
            const Symbol &sym = text.symbols().at(i);
            if (sym.binding == SymbolBinding::External)
                continue;
            auto it = textSymMap.find(i);
            if (it != textSymMap.end()) {
                auto [nameIt, inserted] = definedTextByName.emplace(sym.name, it->second);
                if (!inserted)
                    nameIt->second = UINT32_MAX;
            }
        }

        /// Resolve one encoder relocation to a final nlist index and effective
        /// addend, selecting exact-offset anchors when arm64 range requires one.
        auto resolveRelocSym = [&](const Relocation &rel,
                                   const CodeSection &source,
                                   const std::unordered_map<uint32_t, uint32_t> &sourceMap,
                                   const char *sectionName,
                                   uint32_t &symIdx,
                                   int64_t &effectiveAddend) -> bool {
            symIdx = 0;
            effectiveAddend = rel.addend;
            if (rel.targetSection != SymbolSection::Undefined) {
                if (rel.targetOffsetValid) {
                    const CodeSection &target =
                        (rel.targetSection == SymbolSection::Text) ? text : rodata;
                    const char *targetName =
                        (rel.targetSection == SymbolSection::Text) ? "__text" : "__const";
                    const uint32_t anchorIdx =
                        (rel.targetSection == SymbolSection::Text) ? textAnchorIdx : constAnchorIdx;
                    if (anchorIdx == UINT32_MAX) {
                        err << "MachOWriter: missing section anchor for " << targetName << "\n";
                        return false;
                    }
                    if (rel.targetOffset > target.bytes().size()) {
                        err << "MachOWriter: relocation in " << sectionName << " at offset "
                            << rel.offset << " references " << targetName << " offset "
                            << rel.targetOffset << " beyond section contents\n";
                        return false;
                    }
                    const bool haveSectionOffsetAddend =
                        sectionOffsetAddend(rel, sectionName, effectiveAddend);
                    if (!haveSectionOffsetAddend)
                        return false;
                    if (arch_ == ObjArch::AArch64 && !fitsArm64RelocAddend(effectiveAddend)) {
                        const auto &anchorMap = (rel.targetSection == SymbolSection::Text)
                                                    ? textOffsetAnchorMap
                                                    : constOffsetAnchorMap;
                        auto anchorIt = anchorMap.find(rel.targetOffset);
                        if (anchorIt == anchorMap.end()) {
                            err << "MachOWriter: missing section-offset anchor for " << targetName
                                << " offset " << rel.targetOffset << "\n";
                            return false;
                        }
                        symIdx = anchorIt->second;
                        effectiveAddend = rel.addend;
                        return true;
                    }
                    symIdx = anchorIdx;
                    return true;
                }
                if (rel.symbolIndex >= source.symbols().count()) {
                    err << "MachOWriter: relocation in " << sectionName << " at offset "
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
                    err << "MachOWriter: relocation in " << sectionName << " at offset "
                        << rel.offset << " references missing cross-section target '" << sym.name
                        << "'\n";
                    return false;
                }
                if (nameIt->second == UINT32_MAX) {
                    err << "MachOWriter: relocation in " << sectionName << " at offset "
                        << rel.offset << " references ambiguous cross-section target '" << sym.name
                        << "'\n";
                    return false;
                }
                symIdx = nameIt->second;
                return true;
            }

            auto it = sourceMap.find(rel.symbolIndex);
            if (it == sourceMap.end()) {
                err << "MachOWriter: relocation in " << sectionName << " at offset " << rel.offset
                    << " references unknown symbol index " << rel.symbolIndex << "\n";
                return false;
            }
            symIdx = it->second;
            return true;
        };

        /// Validate and translate one source section's relocations into
        /// descending-address Mach-O relocation groups.
        auto appendRelocs = [&](const CodeSection &source,
                                const std::unordered_map<uint32_t, uint32_t> &sourceMap,
                                const char *sectionName,
                                std::vector<MachoReloc> &outRelocs) -> bool {
            std::vector<MachoRelocGroup> groups;
            for (const auto &rel : source.relocations()) {
                if (!validateRelocationShape("MachOWriter", arch_, source, rel, sectionName, err))
                    return false;
                auto attrs = machoRelocAttrs(rel.kind);
                if (attrs.skip) {
                    err << "MachOWriter: relocation kind " << static_cast<int>(rel.kind)
                        << " has no Mach-O encoding for " << sectionName << " at offset "
                        << rel.offset << "\n";
                    return false;
                }

                uint32_t symIdx = 0;
                int64_t effectiveAddend = rel.addend;
                if (!resolveRelocSym(rel, source, sourceMap, sectionName, symIdx, effectiveAddend))
                    return false;
                if (!checkedRelocSymbolNum(symIdx, "relocation symbol index", err))
                    return false;
                const size_t physicalRelOffset = rel.offset - source.logicalOffsetBias();
                uint32_t relocAddress = 0;
                if (!checkedU32(physicalRelOffset, "relocation address", err, relocAddress))
                    return false;

                MachoRelocGroup group;
                group.address = relocAddress;
                if (arch_ == ObjArch::AArch64 && effectiveAddend != 0) {
                    if (effectiveAddend < -0x800000LL || effectiveAddend > 0x7FFFFFLL) {
                        err << "MachOWriter: AArch64 relocation addend " << effectiveAddend
                            << " is outside signed 24-bit ARM64_RELOC_ADDEND range for "
                            << sectionName << " at offset " << rel.offset << "\n";
                        return false;
                    }
                    const uint32_t addend = static_cast<uint32_t>(effectiveAddend) & 0x00FFFFFFu;
                    group.entries.push_back(
                        {relocAddress,
                         packRelocInfo(addend, 0, attrs.length, 0, kArm64RelocAddend)});
                }

                uint32_t packed =
                    packRelocInfo(symIdx, attrs.pcrel, attrs.length, 1 /*extern*/, attrs.type);
                group.entries.push_back({relocAddress, packed});
                groups.push_back(std::move(group));
            }
            /// Mach-O relocation tables are ordered by decreasing address;
            /// stability retains construction order for groups at equal sites.
            std::stable_sort(groups.begin(),
                             groups.end(),
                             [](const MachoRelocGroup &a, const MachoRelocGroup &b) {
                                 return a.address > b.address;
                             });
            for (const auto &group : groups)
                outRelocs.insert(outRelocs.end(), group.entries.begin(), group.entries.end());
            return true;
        };

        std::vector<MachoReloc> textRelocs;
        if (!appendRelocs(text, textSymMap, "__text", textRelocs))
            return false;

        std::vector<MachoReloc> constRelocs;
        if (!appendRelocs(rodata, rodataSymMap, "__const", constRelocs))
            return false;

        // --- 4b. Build compact unwind section data (AArch64 only) ---
        // Collect unwind entries from the text section and generate __compact_unwind
        // section bytes with relocations pointing back to function symbols.
        std::vector<uint8_t> unwindData;
        std::vector<MachoReloc> unwindRelocs;
        const bool hasUnwind = arch_ == ObjArch::AArch64 && !text.unwindEntries().empty();

        if (hasUnwind) {
            unwindData.reserve(text.unwindEntries().size() * 32);
            for (const auto &entry : text.unwindEntries()) {
                const size_t entryOffset = unwindData.size();

                // functionStart (8 bytes) — filled with 0, relocation points to symbol
                appendLE64(unwindData, 0);
                // functionLength (4 bytes)
                appendLE32(unwindData, entry.functionLength);
                // compactEncoding (4 bytes)
                appendLE32(unwindData, entry.encoding);
                // personality (8 bytes) — none for Zanna
                appendLE64(unwindData, 0);
                // lsda (8 bytes) — none for Zanna
                appendLE64(unwindData, 0);

                // Relocation for functionStart → symbol in __text.
                // ARM64_RELOC_UNSIGNED, 8-byte, extern, pointing to the function symbol.
                uint32_t symIdx = 0;
                auto it = textSymMap.find(entry.symbolIndex);
                if (it != textSymMap.end())
                    symIdx = it->second;
                else {
                    err << "MachOWriter: compact unwind entry references unknown symbol index "
                        << entry.symbolIndex << "\n";
                    return false;
                }

                // packRelocInfo: symbolnum, pcrel=0, length=3 (8 bytes), extern=1, type=0
                // (UNSIGNED)
                if (!checkedRelocSymbolNum(symIdx, "compact unwind symbol index", err))
                    return false;
                uint32_t entryOffset32 = 0;
                if (!checkedU32(
                        entryOffset, "compact unwind relocation address", err, entryOffset32))
                    return false;
                uint32_t packed = packRelocInfo(symIdx, 0, 3, 1, 0);
                unwindRelocs.push_back({entryOffset32, packed});
            }

            // Sort unwind relocations by address descending (Mach-O convention).
            /// Apply Mach-O's descending-address convention to unwind fixups.
            std::sort(
                unwindRelocs.begin(),
                unwindRelocs.end(),
                [](const MachoReloc &a, const MachoReloc &b) { return a.address > b.address; });
        }

        // --- 5. Compute file layout ---
        const bool hasDebugLine = !debugLineData_.empty();
        const uint32_t nsects = 2 + (hasData ? 1 : 0) + (hasUnwind ? 1 : 0) + (hasDebugLine ? 1 : 0);
        size_t sectionHeaderBytes = 0;
        size_t segCmdSizeSize = 0;
        size_t sizeOfCmdsSize = 0;
        size_t afterHeaders = 0;
        if (!checkedMulSize(nsects,
                            80,
                            "MachOWriter",
                            "LC_SEGMENT_64 section headers",
                            err,
                            sectionHeaderBytes) ||
            !checkedAddSize(72,
                            sectionHeaderBytes,
                            "MachOWriter",
                            "LC_SEGMENT_64 command size",
                            err,
                            segCmdSizeSize) ||
            !checkedAddSize(segCmdSizeSize,
                            kBuildVerCmdSize,
                            "MachOWriter",
                            "load command size",
                            err,
                            sizeOfCmdsSize) ||
            !checkedAddSize(sizeOfCmdsSize,
                            kSymtabCmdSize,
                            "MachOWriter",
                            "load command size",
                            err,
                            sizeOfCmdsSize) ||
            !checkedAddSize(sizeOfCmdsSize,
                            kDysymtabCmdSize,
                            "MachOWriter",
                            "load command size",
                            err,
                            sizeOfCmdsSize) ||
            !checkedAddSize(
                kHeaderSize, sizeOfCmdsSize, "MachOWriter", "header area", err, afterHeaders))
            return false;
        uint32_t segCmdSize = 0;
        uint32_t sizeOfCmds = 0;
        if (!checkedU32(segCmdSizeSize, "LC_SEGMENT_64 command size", err, segCmdSize) ||
            !checkedU32(sizeOfCmdsSize, "load command size", err, sizeOfCmds))
            return false;

        size_t textSize = text.bytes().size();
        size_t rodataSize = rodata.bytes().size();
        size_t dataSize = hasData ? data.bytes().size() : 0;
        size_t debugLineSize = hasDebugLine ? debugLineData_.size() : 0;

        size_t unwindSize = unwindData.size();

        size_t textFileOff = 0;
        size_t constFileOff = 0;
        size_t cursor = 0;
        if (!checkedAlignUpSize(
                afterHeaders, textAlign, "MachOWriter", "__text file offset", err, textFileOff) ||
            !checkedAddSize(textFileOff, textSize, "MachOWriter", "__text data", err, cursor) ||
            !checkedAlignUpSize(
                cursor, constAlign, "MachOWriter", "__const file offset", err, constFileOff))
            return false;
        size_t constAddr = constFileOff - textFileOff; // virtual address of __const

        // __data (writable globals) goes after __const (8-byte aligned). Advance the
        // cursor past it so __compact_unwind/__debug_line stack correctly afterward.
        size_t dataFileOff = 0;
        size_t dataAddr = 0;
        if (!checkedAddSize(constFileOff, rodataSize, "MachOWriter", "__const data", err, cursor))
            return false;
        if (hasData) {
            if (!checkedAlignUpSize(
                    cursor, dataAlign, "MachOWriter", "__data file offset", err, dataFileOff))
                return false;
            dataAddr = dataFileOff - textFileOff; // virtual address of __data
            if (!checkedAddSize(dataFileOff, dataSize, "MachOWriter", "__data data", err, cursor))
                return false;
        }

        // __compact_unwind goes after __data (8-byte aligned)
        size_t unwindFileOff = 0;
        if (!checkedAlignUpSize(
                cursor, 8, "MachOWriter", "__compact_unwind file offset", err, unwindFileOff))
            return false;
        size_t unwindAddr = unwindFileOff - textFileOff;

        size_t debugLineFileOff = 0;
        if (hasUnwind) {
            if (!checkedAddSize(unwindFileOff,
                                unwindSize,
                                "MachOWriter",
                                "__compact_unwind data",
                                err,
                                debugLineFileOff))
                return false;
        } else {
            debugLineFileOff = cursor;
        }
        size_t debugLineAddr = debugLineFileOff - textFileOff;

        size_t segFileOff = textFileOff;
        // Segment size must encompass all section data.
        size_t lastDataEnd = 0;
        if (!checkedAddSize(debugLineFileOff,
                            debugLineSize,
                            "MachOWriter",
                            "__debug_line data",
                            err,
                            lastDataEnd))
            return false;
        size_t segFileSize = lastDataEnd - textFileOff;
        size_t segVmSize = segFileSize;

        // Relocations go after all section data
        size_t textRelocOff = lastDataEnd;
        const size_t textRelocCount = textRelocs.size();
        uint32_t nTextRelocs = 0;

        size_t textRelocBytes = 0;
        size_t constRelocOff = 0;
        if (!checkedMulSize(textRelocCount,
                            kRelocSize,
                            "MachOWriter",
                            "__text relocation table",
                            err,
                            textRelocBytes) ||
            !checkedAddSize(textRelocOff,
                            textRelocBytes,
                            "MachOWriter",
                            "__text relocation table",
                            err,
                            constRelocOff))
            return false;
        const size_t constRelocCount = constRelocs.size();
        uint32_t nConstRelocs = 0;

        size_t constRelocBytes = 0;
        size_t unwindRelocOff = 0;
        if (!checkedMulSize(constRelocCount,
                            kRelocSize,
                            "MachOWriter",
                            "__const relocation table",
                            err,
                            constRelocBytes) ||
            !checkedAddSize(constRelocOff,
                            constRelocBytes,
                            "MachOWriter",
                            "__const relocation table",
                            err,
                            unwindRelocOff))
            return false;
        const size_t unwindRelocCount = unwindRelocs.size();
        uint32_t nUnwindRelocs = 0;

        size_t unwindRelocBytes = 0;
        size_t symOff = 0;
        size_t nlistBytes = 0;
        size_t strOff = 0;
        size_t totalSize = 0;
        if (!checkedMulSize(unwindRelocCount,
                            kRelocSize,
                            "MachOWriter",
                            "__compact_unwind relocation table",
                            err,
                            unwindRelocBytes) ||
            !checkedAddSize(unwindRelocOff,
                            unwindRelocBytes,
                            "MachOWriter",
                            "__compact_unwind relocation table",
                            err,
                            symOff) ||
            !checkedMulSize(nsyms, kNlistSize, "MachOWriter", "symbol table", err, nlistBytes) ||
            !checkedAddSize(symOff, nlistBytes, "MachOWriter", "symbol table", err, strOff) ||
            !checkedAddSize(strOff, strtab.size(), "MachOWriter", "string table", err, totalSize))
            return false;

        uint32_t textFileOff32 = 0;
        uint32_t constFileOff32 = 0;
        uint32_t dataFileOff32 = 0;
        uint32_t unwindFileOff32 = 0;
        uint32_t debugLineFileOff32 = 0;
        uint32_t textRelocOff32 = 0;
        uint32_t constRelocOff32 = 0;
        uint32_t unwindRelocOff32 = 0;
        uint32_t symOff32 = 0;
        uint32_t strOff32 = 0;
        uint32_t strSize32 = 0;
        if (!checkedU32(textFileOff, "__text file offset", err, textFileOff32) ||
            !checkedU32(constFileOff, "__const file offset", err, constFileOff32) ||
            !checkedU32(dataFileOff, "__data file offset", err, dataFileOff32) ||
            !checkedU32(unwindFileOff, "__compact_unwind file offset", err, unwindFileOff32) ||
            !checkedU32(debugLineFileOff, "__debug_line file offset", err, debugLineFileOff32) ||
            !checkedU32(textRelocOff, "__text relocation offset", err, textRelocOff32) ||
            !checkedU32(constRelocOff, "__const relocation offset", err, constRelocOff32) ||
            !checkedU32(
                unwindRelocOff, "__compact_unwind relocation offset", err, unwindRelocOff32) ||
            !checkedU32(symOff, "symbol table offset", err, symOff32) ||
            !checkedU32(strOff, "string table offset", err, strOff32) ||
            !checkedU32(strtab.size(), "string table size", err, strSize32) ||
            !checkedU32(textRelocs.size(), "__text relocation count", err, nTextRelocs) ||
            !checkedU32(constRelocs.size(), "__const relocation count", err, nConstRelocs) ||
            !checkedU32(
                unwindRelocs.size(), "__compact_unwind relocation count", err, nUnwindRelocs) ||
            !checkedU32(nsyms, "symbol count", err, nsyms)) {
            return false;
        }

        // --- 6. Build the file ---
        std::vector<uint8_t> file;
        file.reserve(totalSize);

        // Mach-O header (32 bytes)
        writeMachOHeader(file, cputype, cpusubtype, 4, sizeOfCmds, kMhSubsectionsViaSymbols);

        // LC_SEGMENT_64 (232 bytes)
        writeSegmentCmd(file, segCmdSize, segVmSize, segFileOff, segFileSize, nsects);

        // __text section header
        writeSectionHdr(file,
                        "__text",
                        "__TEXT",
                        0,
                        textSize,
                        textFileOff32,
                        textAlignLog2,
                        (nTextRelocs > 0) ? textRelocOff32 : 0,
                        nTextRelocs,
                        kSAttrPureInstructions | kSAttrSomeInstructions);

        // __const section header
        writeSectionHdr(file,
                        "__const",
                        "__TEXT",
                        constAddr,
                        rodataSize,
                        constFileOff32,
                        constAlignLog2,
                        (nConstRelocs > 0) ? constRelocOff32 : 0,
                        nConstRelocs,
                        0);

        // __data section header (writable globals; no relocations for scalar data)
        if (hasData) {
            writeSectionHdr(file,
                            "__data",
                            "__DATA",
                            dataAddr,
                            dataSize,
                            dataFileOff32,
                            dataAlignLog2,
                            0,
                            0,
                            0); // S_REGULAR
        }

        // __compact_unwind section header
        if (hasUnwind) {
            writeSectionHdr(file,
                            "__compact_unwind",
                            "__LD",
                            unwindAddr,
                            unwindSize,
                            unwindFileOff32,
                            3, // align = 2^3 = 8
                            (nUnwindRelocs > 0) ? unwindRelocOff32 : 0,
                            nUnwindRelocs,
                            0); // no special flags
        }

        // __debug_line section header (in __DWARF segment)
        if (hasDebugLine) {
            writeSectionHdr(file,
                            "__debug_line",
                            "__DWARF",
                            debugLineAddr,
                            debugLineSize,
                            debugLineFileOff32,
                            0, // align = 2^0 = 1
                            0,
                            0,
                            kSAttrDebug);
        }

        // LC_BUILD_VERSION (24 bytes)
        writeBuildVersionCmd(file);

        // LC_SYMTAB (24 bytes)
        writeSymtabCmd(file, symOff32, nsyms, strOff32, strSize32);

        // LC_DYSYMTAB (80 bytes)
        writeDysymtabCmd(file, ilocal, nlocal, iextdef, nextdef, iundef, nundef);

        // --- Section data ---
        // __text
        padTo(file, textFileOff);
        file.insert(file.end(), text.bytes().begin(), text.bytes().end());

        // __const
        padTo(file, constFileOff);
        file.insert(file.end(), rodata.bytes().begin(), rodata.bytes().end());

        // __data
        if (hasData) {
            padTo(file, dataFileOff);
            file.insert(file.end(), data.bytes().begin(), data.bytes().end());
        }

        // __compact_unwind
        if (hasUnwind) {
            padTo(file, unwindFileOff);
            file.insert(file.end(), unwindData.begin(), unwindData.end());
        }

        // __debug_line
        if (hasDebugLine)
            file.insert(file.end(), debugLineData_.begin(), debugLineData_.end());

        // --- Patch addends into instruction bytes (Mach-O REL convention) ---
        if (arch_ == ObjArch::X86_64) {
            /// Combine an x86-64 REL addend with any concrete target offset.
            auto computeEffectiveAddend = [&](const Relocation &rel,
                                              const char *sectionName,
                                              int64_t &effectiveAddend) -> bool {
                effectiveAddend = rel.addend;
                if (!rel.targetOffsetValid)
                    return true;
                const CodeSection &target =
                    (rel.targetSection == SymbolSection::Text) ? text : rodata;
                const char *targetName =
                    (rel.targetSection == SymbolSection::Text) ? "__text" : "__const";
                if (rel.targetOffset > target.bytes().size()) {
                    err << "MachOWriter: relocation in " << sectionName << " references "
                        << targetName << " offset " << rel.targetOffset
                        << " beyond section contents\n";
                    return false;
                }
                return checkedSectionOffsetAddend(rel.addend,
                                                  rel.targetOffset,
                                                  "MachOWriter",
                                                  sectionName,
                                                  rel.offset,
                                                  err,
                                                  effectiveAddend);
            };

            /// Embed x86-64 PC-relative or absolute addends directly into the
            /// serialized section bytes as required by Mach-O REL records.
            auto patchX64Addends = [&](const CodeSection &section,
                                       size_t sectionFileOff,
                                       const char *sectionName) -> bool {
                for (const auto &rel : section.relocations()) {
                    const size_t physicalRelOffset = rel.offset - section.logicalOffsetBias();
                    int64_t effectiveAddend = rel.addend;
                    if (!computeEffectiveAddend(rel, sectionName, effectiveAddend))
                        return false;
                    if (rel.kind == RelocKind::PCRel32 || rel.kind == RelocKind::Branch32) {
                        if (effectiveAddend > std::numeric_limits<int64_t>::max() - 4 ||
                            effectiveAddend < std::numeric_limits<int64_t>::min() + 4) {
                            err << "MachOWriter: x86_64 rel32 addend " << effectiveAddend
                                << " overflows Mach-O encoded addend adjustment\n";
                            return false;
                        }
                        const int64_t encodedAddend = effectiveAddend + 4;
                        if (encodedAddend < std::numeric_limits<int32_t>::min() ||
                            encodedAddend > std::numeric_limits<int32_t>::max()) {
                            err << "MachOWriter: x86_64 rel32 addend " << encodedAddend
                                << " is outside signed 32-bit range\n";
                            return false;
                        }
                        size_t patchOff = 0;
                        if (!checkedAddSize(sectionFileOff,
                                            physicalRelOffset,
                                            "MachOWriter",
                                            "x86_64 addend patch offset",
                                            err,
                                            patchOff))
                            return false;
                        if (patchOff > file.size() || 4 > file.size() - patchOff) {
                            err << "error: addend patch at offset " << patchOff
                                << " out of bounds (file size=" << file.size() << ")\n";
                            return false;
                        }
                        auto addend = static_cast<int32_t>(encodedAddend);
                        file[patchOff + 0] = static_cast<uint8_t>(addend);
                        file[patchOff + 1] = static_cast<uint8_t>(addend >> 8);
                        file[patchOff + 2] = static_cast<uint8_t>(addend >> 16);
                        file[patchOff + 3] = static_cast<uint8_t>(addend >> 24);
                    } else if (rel.kind == RelocKind::Abs64 && effectiveAddend != 0) {
                        size_t patchOff = 0;
                        if (!checkedAddSize(sectionFileOff,
                                            physicalRelOffset,
                                            "MachOWriter",
                                            "x86_64 addend patch offset",
                                            err,
                                            patchOff))
                            return false;
                        if (patchOff > file.size() || 8 > file.size() - patchOff) {
                            err << "error: addend patch at offset " << patchOff
                                << " out of bounds (file size=" << file.size() << ")\n";
                            return false;
                        }
                        auto addend = static_cast<uint64_t>(effectiveAddend);
                        for (int i = 0; i < 8; ++i)
                            file[patchOff + i] = static_cast<uint8_t>(addend >> (i * 8));
                    }
                }
                return true;
            };
            if (!patchX64Addends(text, textFileOff, "__text") ||
                !patchX64Addends(rodata, constFileOff, "__const"))
                return false;
        }

        // --- Relocation entries ---
        for (const auto &r : textRelocs)
            writeMachoReloc(file, r.address, r.packed);

        // __const relocations
        for (const auto &r : constRelocs)
            writeMachoReloc(file, r.address, r.packed);

        // __compact_unwind relocations
        for (const auto &r : unwindRelocs)
            writeMachoReloc(file, r.address, r.packed);

        // --- Symbol table ---
        for (const auto &sym : allSyms) {
            uint64_t value = sym.value;
            // Rodata/data symbols store offsets within their own section. Mach-O nlist
            // values are segment-relative, so add the section's base address.
            if (sym.sect == kSectConst)
                value += constAddr;
            else if (sym.sect == kSectData)
                value += dataAddr;
            writeNlist(file, sym.strx, sym.type, sym.sect, 0, value);
        }

        // --- String table ---
        {
            const auto &d = strtab.data();
            file.insert(file.end(), d.begin(), d.end());
        }

        // --- 7. Commit to the selected file or memory sink ---
        return commitOutput(path, file, "MachOWriter", err);
    } catch (const std::exception &ex) {
        err << "MachOWriter: " << ex.what() << "\n";
        return false;
    }
}

/// @copydoc MachOWriter::write(const std::string&, const std::vector<CodeSection>&,
///                             const CodeSection&, std::ostream&)
bool MachOWriter::write(const std::string &path,
                        const std::vector<CodeSection> &textSections,
                        const CodeSection &rodata,
                        std::ostream &err) {
    try {
        CodeSection merged;
        std::unordered_set<std::string> seenLocalNames;
        size_t sectionIndex = 0;
        for (const auto &ts : textSections) {
            CodeSection copy = ts;
            for (uint32_t i = 1; i < copy.symbols().count(); ++i) {
                Symbol &sym = copy.symbols().at(i);
                if (sym.binding != SymbolBinding::Local || sym.name.empty())
                    continue;
                if (seenLocalNames.insert(sym.name).second)
                    continue;
                sym.name += "$macho$" + std::to_string(sectionIndex);
            }
            merged.appendSection(copy);
            ++sectionIndex;
        }
        return write(path, merged, rodata, err);
    } catch (const std::exception &ex) {
        err << "MachOWriter: " << ex.what() << "\n";
        return false;
    }
}

} // namespace zanna::codegen::objfile
