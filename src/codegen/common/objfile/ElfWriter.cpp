//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/ElfWriter.cpp
// Purpose: Serialize CodeSection data into a valid ELF relocatable object file.
// Key invariants:
//   - All multi-byte fields are little-endian
//   - File layout: ehdr | .text | .rodata | .rela.text | .rela.rodata |
//     .symtab | .strtab | .shstrtab | (section header table)
//   - Section offsets are aligned per sh_addralign
//   - Local symbols precede globals (sh_info = first global index)
// Ownership/Lifetime:
//   - Stateless between write() calls
// Links: codegen/common/objfile/ElfWriter.hpp
//        plans/04-elf-writer.md
//
//===----------------------------------------------------------------------===//

/**
 * @file ElfWriter.cpp
 * @brief Implements ELF64 relocatable-object serialization for AMD64 and AArch64.
 *
 * The writer constructs deterministic section-name, symbol-name, symbol, and
 * RELA tables; remaps encoder symbol indices; resolves cross-section targets;
 * and emits either one combined text section or independently discardable
 * per-function sections. All output uses little-endian ELF64/DWARF-compatible
 * layouts and checked file-offset arithmetic.
 */

#include "codegen/common/objfile/ElfWriter.hpp"
#include "codegen/common/objfile/ObjFileWriterUtil.hpp"
#include "codegen/common/objfile/RelocationValidation.hpp"
#include "codegen/common/objfile/StringTable.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::objfile {

// =============================================================================
// ELF Constants
// =============================================================================

// ELF ident
static constexpr uint8_t kElfMagic[4] = {0x7F, 'E', 'L', 'F'};
static constexpr uint8_t kElfClass64 = 2;
static constexpr uint8_t kElfData2LSB = 1;
static constexpr uint8_t kEvCurrent = 1;

// ELF header fields
static constexpr uint16_t kEtRel = 1;
static constexpr uint16_t kEmX86_64 = 62;
static constexpr uint16_t kEmAarch64 = 183;
static constexpr uint16_t kEhSize = 64;
static constexpr uint16_t kShEntSize = 64;

// Section types
static constexpr uint32_t kShtNull = 0;
static constexpr uint32_t kShtProgbits = 1;
static constexpr uint32_t kShtSymtab = 2;
static constexpr uint32_t kShtStrtab = 3;
static constexpr uint32_t kShtRela = 4;

// Section flags
static constexpr uint64_t kShfWrite = 0x1;
static constexpr uint64_t kShfAlloc = 0x2;
static constexpr uint64_t kShfExecinstr = 0x4;
static constexpr uint64_t kShfInfoLink = 0x40;

// Symbol binding/type
static constexpr uint8_t kStbLocal = 0;
static constexpr uint8_t kStbGlobal = 1;
static constexpr uint8_t kSttNotype = 0;
static constexpr uint8_t kSttObject = 1;
static constexpr uint8_t kSttFunc = 2;
static constexpr uint8_t kSttSection = 3;
static constexpr uint8_t kStvDefault = 0;
static constexpr uint16_t kShnUndef = 0;

// Relocation types — x86_64
static constexpr uint32_t kRX86_64_64 = 1;
static constexpr uint32_t kRX86_64_Pc32 = 2;
static constexpr uint32_t kRX86_64_Plt32 = 4;

// Relocation types — AArch64
static constexpr uint32_t kRAarch64_Abs64 = 257;
static constexpr uint32_t kRAarch64_AdrPrelPgHi21 = 275;
static constexpr uint32_t kRAarch64_AddAbsLo12Nc = 277;
static constexpr uint32_t kRAarch64_CondBr19 = 280;
static constexpr uint32_t kRAarch64_Jump26 = 282;
static constexpr uint32_t kRAarch64_Call26 = 283;
static constexpr uint32_t kRAarch64_LdSt32AbsLo12Nc = 285;
static constexpr uint32_t kRAarch64_LdSt64AbsLo12Nc = 286;
static constexpr uint32_t kRAarch64_LdSt128AbsLo12Nc = 299;

// Section indices (fixed layout)
[[maybe_unused]] static constexpr uint16_t kSecNull = 0;
static constexpr uint16_t kSecText = 1;
static constexpr uint16_t kSecRodata = 2;
[[maybe_unused]] static constexpr uint16_t kSecRelaText = 3;
[[maybe_unused]] static constexpr uint16_t kSecRelaRodata = 4;
static constexpr uint16_t kSecSymtab = 5;
static constexpr uint16_t kSecStrtab = 6;
static constexpr uint16_t kSecShstrtab = 7;
[[maybe_unused]] static constexpr uint16_t kSecNoteGnuStack = 8;

// kNumSections is computed dynamically in write() based on whether debug data is present.

// Helpers: appendLE16/32/64, alignUp, padTo are provided by ObjFileWriterUtil.hpp.

/// @brief Select the ELF symbol type implied by a logical symbol section.
/// @param sym Encoder symbol to classify.
/// @return `STT_FUNC`, `STT_OBJECT`, or `STT_NOTYPE`.
static uint8_t elfSymbolType(const Symbol &sym) {
    switch (sym.section) {
        case SymbolSection::Text:
            return kSttFunc;
        case SymbolSection::Rodata:
        case SymbolSection::Data:
            return kSttObject;
        case SymbolSection::Undefined:
        default:
            return kSttNotype;
    }
}

/// @brief Map an encoder relocation kind to an ABI-specific ELF relocation number.
/// @param kind Architecture-neutral relocation semantics.
/// @param arch Target architecture.
/// @return ELF relocation type, or zero when unsupported.
static uint32_t elfRelocType(RelocKind kind, ObjArch arch) {
    switch (kind) {
        // x86_64
        case RelocKind::PCRel32:
            return kRX86_64_Pc32;
        case RelocKind::Branch32:
            return kRX86_64_Plt32;
        case RelocKind::Abs64:
            return arch == ObjArch::AArch64 ? kRAarch64_Abs64 : kRX86_64_64;
        // AArch64
        case RelocKind::A64Call26:
            return kRAarch64_Call26;
        case RelocKind::A64Jump26:
            return kRAarch64_Jump26;
        case RelocKind::A64AdrpPage21:
            return kRAarch64_AdrPrelPgHi21;
        case RelocKind::A64AddPageOff12:
            return kRAarch64_AddAbsLo12Nc;
        case RelocKind::A64LdSt32Off12:
            return kRAarch64_LdSt32AbsLo12Nc;
        case RelocKind::A64LdSt64Off12:
            return kRAarch64_LdSt64AbsLo12Nc;
        case RelocKind::A64LdSt128Off12:
            return kRAarch64_LdSt128AbsLo12Nc;
        case RelocKind::A64CondBr19:
            return kRAarch64_CondBr19;
    }
    (void)arch;
    return 0;
}

/// Adapter to the shared @ref zanna::codegen::objfile::physicalSymbolValue helper
/// that pins the writerName to "ElfWriter:" so existing call sites compile
/// unchanged.
/// @param section Section supplying logical bias and payload bounds.
/// @param sym Defined symbol to convert.
/// @param sectionName Name used to qualify diagnostics.
/// @param err Stream that receives validation diagnostics.
/// @param out Receives the physical section-relative symbol value.
/// @return `true` when @p sym lies within @p section.
static bool physicalSymbolValue(const CodeSection &section,
                                const Symbol &sym,
                                const char *sectionName,
                                std::ostream &err,
                                uint64_t &out) {
    return zanna::codegen::objfile::physicalSymbolValue(
        section, sym, sectionName, "ElfWriter", err, out);
}

/// @brief Compute the complete ELF file reserve size through the section table.
/// @param offShtab File offset of the section-header table.
/// @param numSections Number of fixed-size section headers.
/// @param err Stream that receives multiplication, addition, or narrowing diagnostics.
/// @param out Receives the representable native allocation size.
/// @return `true` when the final file size fits in `size_t`.
static bool reserveFileBytes(uint64_t offShtab,
                             uint16_t numSections,
                             std::ostream &err,
                             size_t &out) {
    uint64_t shtabBytes = 0;
    uint64_t total = 0;
    if (!checkedMulU64(
            numSections, kShEntSize, "ElfWriter", "section header table", err, shtabBytes) ||
        !checkedAddU64(offShtab, shtabBytes, "ElfWriter", "file reserve size", err, total))
        return false;
    return checkedSizeTFromU64(total, "ElfWriter", "file reserve size", err, out);
}

// =============================================================================
// ELF Structs written to the output buffer
// =============================================================================

/// @brief Append one 64-byte `Elf64_Ehdr`.
/// @param out Destination file buffer.
/// @param machine ELF target-machine identifier.
/// @param shoff File offset of the section-header table.
/// @param shnum Number of section headers.
/// @param shstrndx Index of the section-name string table.
static void writeEhdr(std::vector<uint8_t> &out,
                      uint16_t machine,
                      uint64_t shoff,
                      uint16_t shnum,
                      uint16_t shstrndx) {
    // e_ident
    out.insert(out.end(), kElfMagic, kElfMagic + 4);
    out.push_back(kElfClass64);
    out.push_back(kElfData2LSB);
    out.push_back(kEvCurrent);
    out.push_back(0);              // OSABI: ELFOSABI_NONE
    out.resize(out.size() + 8, 0); // padding to byte 16

    appendLE16(out, kEtRel);     // e_type
    appendLE16(out, machine);    // e_machine
    appendLE32(out, 1);          // e_version
    appendLE64(out, 0);          // e_entry
    appendLE64(out, 0);          // e_phoff
    appendLE64(out, shoff);      // e_shoff
    appendLE32(out, 0);          // e_flags
    appendLE16(out, kEhSize);    // e_ehsize
    appendLE16(out, 0);          // e_phentsize
    appendLE16(out, 0);          // e_phnum
    appendLE16(out, kShEntSize); // e_shentsize
    appendLE16(out, shnum);      // e_shnum
    appendLE16(out, shstrndx);   // e_shstrndx
}

/// @brief Append one 64-byte `Elf64_Shdr`.
/// @param out Destination section-header table.
/// @param name Offset of the name in `.shstrtab`.
/// @param type ELF section type.
/// @param flags ELF section flags.
/// @param offset File offset of section contents.
/// @param size Section byte count.
/// @param link Type-dependent linked section index.
/// @param info Type-dependent auxiliary index or count.
/// @param addralign Required file and virtual alignment.
/// @param entsize Fixed entry size, or zero for byte streams.
static void writeShdr(std::vector<uint8_t> &out,
                      uint32_t name,
                      uint32_t type,
                      uint64_t flags,
                      uint64_t offset,
                      uint64_t size,
                      uint32_t link,
                      uint32_t info,
                      uint64_t addralign,
                      uint64_t entsize) {
    appendLE32(out, name);
    appendLE32(out, type);
    appendLE64(out, flags);
    appendLE64(out, 0); // sh_addr
    appendLE64(out, offset);
    appendLE64(out, size);
    appendLE32(out, link);
    appendLE32(out, info);
    appendLE64(out, addralign);
    appendLE64(out, entsize);
}

/// @brief Append one 24-byte `Elf64_Sym`.
/// @param out Destination symbol table.
/// @param name Offset of the name in `.strtab`.
/// @param info Packed binding and symbol type.
/// @param other Visibility and reserved bits.
/// @param shndx Defining section index or `SHN_UNDEF`.
/// @param value Section-relative symbol value.
/// @param size Declared symbol size.
static void writeSym(std::vector<uint8_t> &out,
                     uint32_t name,
                     uint8_t info,
                     uint8_t other,
                     uint16_t shndx,
                     uint64_t value,
                     uint64_t size) {
    appendLE32(out, name);
    out.push_back(info);
    out.push_back(other);
    appendLE16(out, shndx);
    appendLE64(out, value);
    appendLE64(out, size);
}

/// @brief Append one 24-byte `Elf64_Rela` relocation with explicit addend.
/// @param out Destination RELA section.
/// @param offset Section-relative fixup offset.
/// @param info Packed symbol index and relocation type.
/// @param addend Signed relocation addend.
static void writeRela(std::vector<uint8_t> &out, uint64_t offset, uint64_t info, int64_t addend) {
    appendLE64(out, offset);
    appendLE64(out, info);
    appendLE64(out, static_cast<uint64_t>(addend));
}

// =============================================================================
// ElfWriter::write
// =============================================================================

/// @copydoc ElfWriter::write(const std::string&, const CodeSection&,
///                           const CodeSection&, std::ostream&)
bool ElfWriter::write(const std::string &path,
                      const CodeSection &text,
                      const CodeSection &rodata,
                      std::ostream &err) {
    try {
        // Writable initialized-data section (.data). Empty unless setDataSection()
        // supplied scalar globals; symbols here satisfy the text section's undefined
        // references to mutable globals via name coalescing (globalNameMap).
        const CodeSection emptyData;
        const CodeSection &data = dataSection_ ? *dataSection_ : emptyData;
        const bool hasData = !data.bytes().empty();

        // --- 1. Build .shstrtab (section name string table) ---
        StringTable shstrtab;
        uint32_t shNameNull = 0; // empty string at offset 0
        uint32_t shNameText = shstrtab.add(".text");
        uint32_t shNameRodata = shstrtab.add(".rodata");
        uint32_t shNameRelaText = shstrtab.add(".rela.text");
        uint32_t shNameRelaRodata = shstrtab.add(".rela.rodata");
        uint32_t shNameSymtab = shstrtab.add(".symtab");
        uint32_t shNameStrtab = shstrtab.add(".strtab");
        uint32_t shNameShstrtab = shstrtab.add(".shstrtab");
        uint32_t shNameNoteGnuStack = shstrtab.add(".note.GNU-stack");

        const bool hasDebugLine = !debugLineData_.empty();
        uint32_t shNameDebugLine = 0;
        if (hasDebugLine)
            shNameDebugLine = shstrtab.add(".debug_line");

        // .data is appended as the LAST section header so the fixed kSec* indices
        // for .rela.*/.symtab/.strtab/.shstrtab/.note remain valid. Its section
        // index is therefore the pre-.data section count.
        uint32_t shNameData = 0;
        const uint16_t secData = hasDebugLine ? 10 : 9;
        if (hasData)
            shNameData = shstrtab.add(".data");

        // --- 2. Build symbol table and .strtab ---
        // ELF requires: null sym, section syms (local), then globals, then externals.
        // We need to remap symbol indices from CodeSection's table to ELF indices.

        StringTable strtab;
        std::vector<uint8_t> symtabBytes;

        // Symbol index 0: null symbol (always first).
        writeSym(symtabBytes, 0, 0, 0, 0, 0, 0);

        // Section symbols for .text and .rodata (local).
        // These are used as targets for cross-section relocations.
        writeSym(symtabBytes, 0, (kStbLocal << 4) | kSttSection, kStvDefault, kSecText, 0, 0);

        writeSym(symtabBytes, 0, (kStbLocal << 4) | kSttSection, kStvDefault, kSecRodata, 0, 0);

        uint32_t elfLocalCount = 3; // null + 2 section symbols

        // Map from CodeSection symbol index → ELF symbol index.
        // We process text symbols then rodata symbols.
        std::unordered_map<uint32_t, uint32_t> textSymMap;
        std::unordered_map<uint32_t, uint32_t> rodataSymMap;

        // First pass: collect locals (defined symbols other than section symbols).
        // Second pass: collect globals and externals.
        // For simplicity, treat all defined symbols as globals (they're exported functions).

        // Process text section symbols.
        /// @brief Deferred encoder symbol awaiting ELF local/global ordering.
        struct PendingSym {
            ///< Original CodeSection symbol index.
            uint32_t origIdx;
            ///< Non-owning pointer to the encoder symbol.
            const Symbol *sym;
            ///< Final defining ELF section index.
            uint16_t shndx;
            ///< Whether the symbol originated in the combined text section.
            bool fromText;        // true = text section
            ///< Whether it originated in the optional writable data section.
            bool fromData = false; // true = writable .data section (overrides fromText)
        };

        std::vector<PendingSym> pendingLocals, pendingGlobals;

        for (uint32_t i = 1; i < text.symbols().count(); ++i) {
            const Symbol &s = text.symbols().at(i);
            PendingSym ps{i, &s, 0, true};

            if (s.binding == SymbolBinding::External) {
                ps.shndx = kShnUndef;
                pendingGlobals.push_back(ps);
            } else if (s.binding == SymbolBinding::Local) {
                ps.shndx = kSecText;
                pendingLocals.push_back(ps);
            } else {
                // Global defined symbol.
                ps.shndx = kSecText;
                pendingGlobals.push_back(ps);
            }
        }

        // Process rodata section symbols.
        for (uint32_t i = 1; i < rodata.symbols().count(); ++i) {
            const Symbol &s = rodata.symbols().at(i);
            PendingSym ps{i, &s, 0, false};

            if (s.binding == SymbolBinding::External) {
                ps.shndx = kShnUndef;
                pendingGlobals.push_back(ps);
            } else if (s.binding == SymbolBinding::Local) {
                ps.shndx = kSecRodata;
                pendingLocals.push_back(ps);
            } else {
                ps.shndx = kSecRodata;
                pendingGlobals.push_back(ps);
            }
        }

        // Process .data section symbols (writable scalar globals). These are defined
        // globals whose names coalesce the text section's undefined references.
        for (uint32_t i = 1; i < data.symbols().count(); ++i) {
            const Symbol &s = data.symbols().at(i);
            PendingSym ps{i, &s, secData, false, true};
            if (s.binding == SymbolBinding::Local)
                pendingLocals.push_back(ps);
            else
                pendingGlobals.push_back(ps); // Global/External defined in .data
        }

        // Write local symbols first.
        for (const auto &ps : pendingLocals) {
            uint32_t nameOff = strtab.add(ps.sym->name);
            uint8_t type = elfSymbolType(*ps.sym);
            uint64_t value = 0;
            const CodeSection &source = ps.fromData ? data : (ps.fromText ? text : rodata);
            const char *secName = ps.fromData ? ".data" : (ps.fromText ? ".text" : ".rodata");
            if (!physicalSymbolValue(source, *ps.sym, secName, err, value))
                return false;
            writeSym(symtabBytes,
                     nameOff,
                     (kStbLocal << 4) | type,
                     kStvDefault,
                     ps.shndx,
                     value,
                     ps.sym->size);
            uint32_t elfIdx = elfLocalCount++;
            if (ps.fromData)
                continue; // .data symbols are referenced by name, not by index map
            if (ps.fromText)
                textSymMap[ps.origIdx] = elfIdx;
            else
                rodataSymMap[ps.origIdx] = elfIdx;
        }

        // sh_info = first non-local symbol index.
        uint32_t firstGlobalIdx = elfLocalCount;

        std::vector<PendingSym> pendingDefinedGlobals;
        std::vector<PendingSym> pendingExternals;
        for (const auto &ps : pendingGlobals) {
            if (ps.sym->binding == SymbolBinding::External)
                pendingExternals.push_back(ps);
            else
                pendingDefinedGlobals.push_back(ps);
        }

        // Write defined globals before undefined references so definitions win.
        std::unordered_map<std::string, uint32_t> globalNameMap;
        uint32_t elfGlobalIdx = elfLocalCount;
        for (const auto &ps : pendingDefinedGlobals) {
            if (globalNameMap.count(ps.sym->name)) {
                err << "ElfWriter: duplicate global symbol '" << ps.sym->name << "'\n";
                return false;
            }
            uint32_t nameOff = strtab.add(ps.sym->name);
            uint8_t type = elfSymbolType(*ps.sym);
            uint16_t shndx = ps.shndx;
            uint64_t value = 0;
            const CodeSection &source = ps.fromData ? data : (ps.fromText ? text : rodata);
            const char *secName = ps.fromData ? ".data" : (ps.fromText ? ".text" : ".rodata");
            if (!physicalSymbolValue(source, *ps.sym, secName, err, value))
                return false;
            writeSym(symtabBytes,
                     nameOff,
                     (kStbGlobal << 4) | type,
                     kStvDefault,
                     shndx,
                     value,
                     ps.sym->size);
            if (ps.fromData)
                ; // .data globals are referenced by name (globalNameMap), no index map
            else if (ps.fromText)
                textSymMap[ps.origIdx] = elfGlobalIdx;
            else
                rodataSymMap[ps.origIdx] = elfGlobalIdx;
            globalNameMap[ps.sym->name] = elfGlobalIdx;
            ++elfGlobalIdx;
        }

        for (const auto &ps : pendingExternals) {
            auto existing = globalNameMap.find(ps.sym->name);
            if (existing != globalNameMap.end()) {
                if (ps.fromText)
                    textSymMap[ps.origIdx] = existing->second;
                else
                    rodataSymMap[ps.origIdx] = existing->second;
                continue;
            }
            uint32_t nameOff = strtab.add(ps.sym->name);
            writeSym(
                symtabBytes, nameOff, (kStbGlobal << 4) | kSttNotype, kStvDefault, kShnUndef, 0, 0);
            if (ps.fromText)
                textSymMap[ps.origIdx] = elfGlobalIdx;
            else
                rodataSymMap[ps.origIdx] = elfGlobalIdx;
            globalNameMap[ps.sym->name] = elfGlobalIdx;
            ++elfGlobalIdx;
        }

        // Build name→ELF index map for defined rodata symbols.
        // Used to redirect text relocations that reference undefined symbols
        // which are actually defined in rodata (cross-section references).
        std::unordered_map<std::string, uint32_t> definedRodataByName;
        for (uint32_t i = 1; i < rodata.symbols().count(); ++i) {
            const Symbol &s = rodata.symbols().at(i);
            if (s.binding != SymbolBinding::External) {
                auto elfIt = rodataSymMap.find(i);
                if (elfIt != rodataSymMap.end()) {
                    auto [it, inserted] = definedRodataByName.emplace(s.name, elfIt->second);
                    if (!inserted)
                        it->second = UINT32_MAX;
                }
            }
        }

        std::unordered_map<std::string, uint32_t> definedTextByName;
        for (uint32_t i = 1; i < text.symbols().count(); ++i) {
            const Symbol &s = text.symbols().at(i);
            if (s.binding != SymbolBinding::External) {
                auto elfIt = textSymMap.find(i);
                if (elfIt != textSymMap.end()) {
                    auto [it, inserted] = definedTextByName.emplace(s.name, elfIt->second);
                    if (!inserted)
                        it->second = UINT32_MAX;
                }
            }
        }

        /// Resolve one encoder relocation to its ELF symbol index and explicit
        /// addend, including cross-section and exact-offset targets.
        auto resolveRelocSym = [&](const Relocation &rel,
                                   const CodeSection &source,
                                   const std::unordered_map<uint32_t, uint32_t> &sourceMap,
                                   const std::unordered_map<std::string, uint32_t> &targetByName,
                                   const char *sectionName,
                                   uint32_t &elfSymIdx,
                                   int64_t &effectiveAddend) -> bool {
            elfSymIdx = 0;
            effectiveAddend = rel.addend;
            if (rel.targetSection != SymbolSection::Undefined) {
                if (rel.targetOffsetValid) {
                    const CodeSection &target =
                        (rel.targetSection == SymbolSection::Text) ? text : rodata;
                    const char *targetName =
                        (rel.targetSection == SymbolSection::Text) ? ".text" : ".rodata";
                    if (rel.targetOffset > target.bytes().size()) {
                        err << "ElfWriter: relocation in " << sectionName << " at offset "
                            << rel.offset << " references " << targetName << " offset "
                            << rel.targetOffset << " beyond section contents\n";
                        return false;
                    }
                    if (!checkedSectionOffsetAddend(rel.addend,
                                                    rel.targetOffset,
                                                    "ElfWriter",
                                                    sectionName,
                                                    rel.offset,
                                                    err,
                                                    effectiveAddend))
                        return false;
                    elfSymIdx = (rel.targetSection == SymbolSection::Text) ? 1u : 2u;
                    return true;
                }
                if (rel.symbolIndex >= source.symbols().count()) {
                    err << "ElfWriter: relocation in " << sectionName << " at offset " << rel.offset
                        << " references unknown symbol index " << rel.symbolIndex << "\n";
                    return false;
                }
                const Symbol &sym = source.symbols().at(rel.symbolIndex);
                auto nameIt = targetByName.find(sym.name);
                if (nameIt == targetByName.end()) {
                    err << "ElfWriter: relocation in " << sectionName << " at offset " << rel.offset
                        << " references missing cross-section target '" << sym.name << "'\n";
                    return false;
                }
                if (nameIt->second == UINT32_MAX) {
                    err << "ElfWriter: relocation in " << sectionName << " at offset " << rel.offset
                        << " references ambiguous cross-section target '" << sym.name << "'\n";
                    return false;
                }
                elfSymIdx = nameIt->second;
                return true;
            }

            auto it = sourceMap.find(rel.symbolIndex);
            if (it == sourceMap.end()) {
                err << "ElfWriter: relocation in " << sectionName << " at offset " << rel.offset
                    << " references unknown symbol index " << rel.symbolIndex << "\n";
                return false;
            }
            elfSymIdx = it->second;
            return true;
        };

        // --- 3. Build .rela.text ---
        std::vector<uint8_t> relaBytes;
        for (const auto &rel : text.relocations()) {
            if (!validateRelocationShape("ElfWriter", arch_, text, rel, ".text", err))
                return false;
            uint32_t elfSymIdx = 0;
            int64_t effectiveAddend = rel.addend;
            const auto &targetMap = (rel.targetSection == SymbolSection::Rodata)
                                        ? definedRodataByName
                                        : definedTextByName;
            if (!resolveRelocSym(
                    rel, text, textSymMap, targetMap, ".text", elfSymIdx, effectiveAddend))
                return false;

            const size_t physicalRelOffset = rel.offset - text.logicalOffsetBias();
            uint32_t relocType = elfRelocType(rel.kind, arch_);
            uint64_t rInfo = (static_cast<uint64_t>(elfSymIdx) << 32) | relocType;
            writeRela(relaBytes, static_cast<uint64_t>(physicalRelOffset), rInfo, effectiveAddend);
        }

        std::vector<uint8_t> relaRodataBytes;
        for (const auto &rel : rodata.relocations()) {
            if (!validateRelocationShape("ElfWriter", arch_, rodata, rel, ".rodata", err))
                return false;
            uint32_t elfSymIdx = 0;
            int64_t effectiveAddend = rel.addend;
            const auto &targetMap = (rel.targetSection == SymbolSection::Text)
                                        ? definedTextByName
                                        : definedRodataByName;
            if (!resolveRelocSym(
                    rel, rodata, rodataSymMap, targetMap, ".rodata", elfSymIdx, effectiveAddend))
                return false;

            const size_t physicalRelOffset = rel.offset - rodata.logicalOffsetBias();
            uint32_t relocType = elfRelocType(rel.kind, arch_);
            uint64_t rInfo = (static_cast<uint64_t>(elfSymIdx) << 32) | relocType;
            writeRela(
                relaRodataBytes, static_cast<uint64_t>(physicalRelOffset), rInfo, effectiveAddend);
        }

        // --- 4. Compute file layout ---
        uint64_t textAlign = (arch_ == ObjArch::X86_64) ? 16 : 4;

        // Section data sizes
        uint64_t textSize = text.bytes().size();
        uint64_t rodataSize = rodata.bytes().size();
        uint64_t relaSize = relaBytes.size();
        uint64_t relaRodataSize = relaRodataBytes.size();
        uint64_t symtabSize = symtabBytes.size();
        uint64_t strtabSize = strtab.size();
        uint64_t shstrtabSize = shstrtab.size();

        // File offsets (sequential, aligned)
        uint64_t offText = 0;
        uint64_t offRodata = 0;
        uint64_t offRelaText = 0;
        uint64_t offRelaRodata = 0;
        uint64_t offSymtab = 0;
        uint64_t offStrtab = 0;
        uint64_t offShstrtab = 0;
        uint64_t cursor = 0;
        if (!checkedAlignUpU64(kEhSize, textAlign, "ElfWriter", ".text offset", err, offText) ||
            !checkedAddU64(offText, textSize, "ElfWriter", ".text data", err, cursor) ||
            !checkedAlignUpU64(cursor, 8, "ElfWriter", ".rodata offset", err, offRodata) ||
            !checkedAddU64(offRodata, rodataSize, "ElfWriter", ".rodata data", err, cursor) ||
            !checkedAlignUpU64(cursor, 8, "ElfWriter", ".rela.text offset", err, offRelaText) ||
            !checkedAddU64(offRelaText, relaSize, "ElfWriter", ".rela.text data", err, cursor) ||
            !checkedAlignUpU64(cursor, 8, "ElfWriter", ".rela.rodata offset", err, offRelaRodata) ||
            !checkedAddU64(
                offRelaRodata, relaRodataSize, "ElfWriter", ".rela.rodata data", err, cursor) ||
            !checkedAlignUpU64(cursor, 8, "ElfWriter", ".symtab offset", err, offSymtab) ||
            !checkedAddU64(offSymtab, symtabSize, "ElfWriter", ".symtab data", err, offStrtab) ||
            !checkedAddU64(offStrtab, strtabSize, "ElfWriter", ".strtab data", err, offShstrtab))
            return false;
        // .note.GNU-stack has zero size, its offset doesn't matter but we place it next.
        uint64_t offNoteGnuStack = 0;
        if (!checkedAddU64(
                offShstrtab, shstrtabSize, "ElfWriter", ".shstrtab data", err, offNoteGnuStack))
            return false;
        // .debug_line (optional, non-alloc) — placed after .note.GNU-stack.
        uint64_t debugLineSize = debugLineData_.size();
        uint64_t offDebugLine = offNoteGnuStack;
        uint64_t debugEnd = 0;
        if (!checkedAddU64(
                offDebugLine, debugLineSize, "ElfWriter", ".debug_line data", err, debugEnd))
            return false;

        // .data (writable globals) — appended after .debug_line, before the SHT.
        uint64_t dataSize = hasData ? data.bytes().size() : 0;
        uint64_t offData = debugEnd;
        uint64_t dataEnd = debugEnd;
        if (hasData) {
            if (!checkedAlignUpU64(debugEnd, 8, "ElfWriter", ".data offset", err, offData) ||
                !checkedAddU64(offData, dataSize, "ElfWriter", ".data data", err, dataEnd))
                return false;
        }

        uint64_t offShtab = 0;
        if (!checkedAlignUpU64(dataEnd, 8, "ElfWriter", "section header table", err, offShtab))
            return false;

        // --- 5. Build the file ---
        const uint16_t numSections = static_cast<uint16_t>((hasDebugLine ? 10 : 9) + (hasData ? 1 : 0));

        std::vector<uint8_t> file;
        size_t reserveSize = 0;
        if (!reserveFileBytes(offShtab, numSections, err, reserveSize))
            return false;
        file.reserve(reserveSize);

        // ELF header (64 bytes)
        uint16_t machine = (arch_ == ObjArch::X86_64) ? kEmX86_64 : kEmAarch64;
        writeEhdr(file, machine, offShtab, numSections, kSecShstrtab);

        // .text
        padTo(file, static_cast<size_t>(offText));
        file.insert(file.end(), text.bytes().begin(), text.bytes().end());

        // .rodata
        padTo(file, static_cast<size_t>(offRodata));
        file.insert(file.end(), rodata.bytes().begin(), rodata.bytes().end());

        // .rela.text
        padTo(file, static_cast<size_t>(offRelaText));
        file.insert(file.end(), relaBytes.begin(), relaBytes.end());

        // .rela.rodata
        padTo(file, static_cast<size_t>(offRelaRodata));
        file.insert(file.end(), relaRodataBytes.begin(), relaRodataBytes.end());

        // .symtab
        padTo(file, static_cast<size_t>(offSymtab));
        file.insert(file.end(), symtabBytes.begin(), symtabBytes.end());

        // .strtab
        padTo(file, static_cast<size_t>(offStrtab));
        {
            const auto &d = strtab.data();
            file.insert(file.end(), d.begin(), d.end());
        }

        // .shstrtab
        padTo(file, static_cast<size_t>(offShstrtab));
        {
            const auto &d = shstrtab.data();
            file.insert(file.end(), d.begin(), d.end());
        }

        // .note.GNU-stack (zero size, no data to append)

        // .debug_line (optional)
        if (hasDebugLine)
            file.insert(file.end(), debugLineData_.begin(), debugLineData_.end());

        // .data (optional, writable)
        if (hasData) {
            padTo(file, static_cast<size_t>(offData));
            file.insert(file.end(), data.bytes().begin(), data.bytes().end());
        }

        // Section header table
        padTo(file, static_cast<size_t>(offShtab));

        // [0] Null section
        writeShdr(file, shNameNull, kShtNull, 0, 0, 0, 0, 0, 0, 0);

        // [1] .text
        writeShdr(file,
                  shNameText,
                  kShtProgbits,
                  kShfAlloc | kShfExecinstr,
                  offText,
                  textSize,
                  0,
                  0,
                  textAlign,
                  0);

        // [2] .rodata
        writeShdr(file, shNameRodata, kShtProgbits, kShfAlloc, offRodata, rodataSize, 0, 0, 8, 0);

        // [3] .rela.text
        writeShdr(file,
                  shNameRelaText,
                  kShtRela,
                  kShfInfoLink,
                  offRelaText,
                  relaSize,
                  kSecSymtab,
                  kSecText,
                  8,
                  24);

        // [4] .rela.rodata
        writeShdr(file,
                  shNameRelaRodata,
                  kShtRela,
                  kShfInfoLink,
                  offRelaRodata,
                  relaRodataSize,
                  kSecSymtab,
                  kSecRodata,
                  8,
                  24);

        // [5] .symtab
        writeShdr(file,
                  shNameSymtab,
                  kShtSymtab,
                  0,
                  offSymtab,
                  symtabSize,
                  kSecStrtab,
                  firstGlobalIdx,
                  8,
                  24);

        // [6] .strtab
        writeShdr(file, shNameStrtab, kShtStrtab, 0, offStrtab, strtabSize, 0, 0, 1, 0);

        // [7] .shstrtab
        writeShdr(file, shNameShstrtab, kShtStrtab, 0, offShstrtab, shstrtabSize, 0, 0, 1, 0);

        // [8] .note.GNU-stack
        writeShdr(file, shNameNoteGnuStack, kShtProgbits, 0, offNoteGnuStack, 0, 0, 0, 1, 0);

        // [9] .debug_line (optional, non-alloc)
        if (hasDebugLine)
            writeShdr(
                file, shNameDebugLine, kShtProgbits, 0, offDebugLine, debugLineSize, 0, 0, 1, 0);

        // [last] .data (optional, writable, alloc) — appended so existing indices hold.
        if (hasData)
            writeShdr(file,
                      shNameData,
                      kShtProgbits,
                      kShfAlloc | kShfWrite,
                      offData,
                      dataSize,
                      0,
                      0,
                      8,
                      0);

        // --- 6. Commit to the selected file or memory sink ---
        return commitOutput(path, file, "ElfWriter", err);
    } catch (const std::exception &ex) {
        err << "ElfWriter: " << ex.what() << "\n";
        return false;
    }
}

// =============================================================================
// ElfWriter::write (multi-section)
//
// Emits per-function .text.funcname sections to enable function-level dead
// stripping at link time.  For 0 or 1 text sections, delegates to the
// single-section write() to avoid unnecessary complexity.
// =============================================================================

namespace {
/// @brief Derive a section base-name for each text section: the first global
///        Text-bound symbol, or a synthetic `func_<i>` fallback.
/// @param textSections Function-level code sections to inspect.
/// @return One deterministic base name per input section.
std::vector<std::string> collectElfTextFuncNames(const std::vector<CodeSection> &textSections) {
    const size_t n = textSections.size();
    std::vector<std::string> funcNames(n);
    for (size_t i = 0; i < n; ++i) {
        funcNames[i] = "func_" + std::to_string(i);
        for (uint32_t j = 1; j < textSections[i].symbols().count(); ++j) {
            const auto &s = textSections[i].symbols().at(j);
            if (s.binding == SymbolBinding::Global && s.section == SymbolSection::Text) {
                funcNames[i] = s.name;
                break;
            }
        }
    }
    return funcNames;
}
} // namespace

/// @copydoc ElfWriter::write(const std::string&, const std::vector<CodeSection>&,
///                           const CodeSection&, std::ostream&)
bool ElfWriter::write(const std::string &path,
                      const std::vector<CodeSection> &textSections,
                      const CodeSection &rodata,
                      std::ostream &err) {
    try {
        // For 0 or 1 text sections, delegate to single-section write.
        if (textSections.size() <= 1) {
            if (textSections.size() == 1)
                return write(path, textSections[0], rodata, err);
            CodeSection empty;
            return write(path, empty, rodata, err);
        }

        const bool hasDebugLine = !debugLineData_.empty();

        // Writable initialized-data section (.data); appended LAST so the computed
        // per-function section indices below remain valid.
        const CodeSection emptyData;
        const CodeSection &data = dataSection_ ? *dataSection_ : emptyData;
        const bool hasData = !data.bytes().empty();

        const size_t N = textSections.size();
        const size_t baseSectionCount = 7 + (hasDebugLine ? 1 : 0);
        if (N > (static_cast<size_t>(UINT16_MAX) - baseSectionCount - (hasData ? 1 : 0)) / 2) {
            err << "ElfWriter: too many text sections for ELF writer (" << N << ")\n";
            return false;
        }
        const size_t sectionCountNoData = 2 * N + baseSectionCount;
        const size_t sectionCount = sectionCountNoData + (hasData ? 1 : 0);
        // .data section index (valid only when hasData): the first index past the
        // existing sections.
        const uint16_t secData = static_cast<uint16_t>(sectionCountNoData);

        // --- 1. Extract function names from each text section ---
        const std::vector<std::string> funcNames = collectElfTextFuncNames(textSections);

        // --- 2. Section index layout ---
        // [0] null
        // [1..N] .text.func1 ... .text.funcN
        // [N+1] .rodata
        // [N+2..2N+1] .rela.text.func1 ... .rela.text.funcN
        // [2N+2] .rela.rodata
        // [2N+3] .symtab
        // [2N+4] .strtab
        // [2N+5] .shstrtab
        // [2N+6] .note.GNU-stack
        /// Convert a zero-based function-section ordinal to its ELF section index.
        auto secText = [](size_t i) -> uint16_t { return static_cast<uint16_t>(i + 1); };
        const uint16_t secRodata = static_cast<uint16_t>(N + 1);
        /// Convert a function-section ordinal to its corresponding RELA section index.
        [[maybe_unused]] auto secRelaText = [&](size_t i) -> uint16_t {
            return static_cast<uint16_t>(N + 2 + i);
        };
        [[maybe_unused]] const uint16_t secRelaRodata = static_cast<uint16_t>(2 * N + 2);
        const uint16_t secSymtab = static_cast<uint16_t>(2 * N + 3);
        const uint16_t secStrtab = static_cast<uint16_t>(2 * N + 4);
        const uint16_t secShstrtab = static_cast<uint16_t>(2 * N + 5);
        const uint16_t numSections = static_cast<uint16_t>(sectionCount);

        // --- 3. Build .shstrtab ---
        StringTable shstrtab;

        std::vector<uint32_t> shNameText(N);
        for (size_t i = 0; i < N; ++i)
            shNameText[i] = shstrtab.add(".text." + funcNames[i]);

        uint32_t shNameRodata = shstrtab.add(".rodata");

        std::vector<uint32_t> shNameRelaText(N);
        for (size_t i = 0; i < N; ++i)
            shNameRelaText[i] = shstrtab.add(".rela.text." + funcNames[i]);

        uint32_t shNameRelaRodata = shstrtab.add(".rela.rodata");
        uint32_t shNameSymtab = shstrtab.add(".symtab");
        uint32_t shNameStrtab = shstrtab.add(".strtab");
        uint32_t shNameShstrtab = shstrtab.add(".shstrtab");
        uint32_t shNameNoteGnuStack = shstrtab.add(".note.GNU-stack");

        uint32_t shNameDebugLine = 0;
        if (hasDebugLine)
            shNameDebugLine = shstrtab.add(".debug_line");

        uint32_t shNameData = 0;
        if (hasData)
            shNameData = shstrtab.add(".data");

        // --- 4. Build symbol table ---
        StringTable strtab;
        std::vector<uint8_t> symtabBytes;

        // [0] null symbol
        writeSym(symtabBytes, 0, 0, 0, 0, 0, 0);

        // Section symbols for each text section
        for (size_t i = 0; i < N; ++i)
            writeSym(symtabBytes, 0, (kStbLocal << 4) | kSttSection, kStvDefault, secText(i), 0, 0);

        // Section symbol for .rodata
        writeSym(symtabBytes, 0, (kStbLocal << 4) | kSttSection, kStvDefault, secRodata, 0, 0);

        // null + N text section syms + 1 rodata section sym
        uint32_t elfLocalCount = static_cast<uint32_t>(N + 2);

        // Per-text-section maps: CodeSection sym idx → unified ELF sym idx
        std::vector<std::unordered_map<uint32_t, uint32_t>> textSymMaps(N);
        std::unordered_map<uint32_t, uint32_t> rodataSymMap;

        /// @brief Deferred symbol awaiting ELF local/global ordering and remapping.
        struct PendingSym {
            ///< Original CodeSection symbol index.
            uint32_t origIdx;
            ///< Non-owning pointer to the encoder symbol.
            const Symbol *sym;
            ///< Final defining ELF section index.
            uint16_t shndx;
            ///< Index in @p textSections, or `SIZE_MAX` for rodata/data.
            size_t textIdx;        // index into textSections; SIZE_MAX for rodata/data
            ///< Whether the symbol belongs to the optional writable data section.
            bool fromData = false; // true = writable .data section
        };

        std::vector<PendingSym> pendingLocals, pendingDefinedGlobals, pendingExternals;

        // Collect symbols from all text sections.
        for (size_t ti = 0; ti < N; ++ti) {
            const auto &sec = textSections[ti];
            for (uint32_t i = 1; i < sec.symbols().count(); ++i) {
                const auto &s = sec.symbols().at(i);
                PendingSym ps{i, &s, 0, ti};

                if (s.binding == SymbolBinding::External) {
                    ps.shndx = kShnUndef;
                    pendingExternals.push_back(ps);
                } else if (s.binding == SymbolBinding::Local) {
                    ps.shndx = secText(ti);
                    pendingLocals.push_back(ps);
                } else {
                    ps.shndx = secText(ti);
                    pendingDefinedGlobals.push_back(ps);
                }
            }
        }

        // Collect rodata symbols.
        for (uint32_t i = 1; i < rodata.symbols().count(); ++i) {
            const auto &s = rodata.symbols().at(i);
            PendingSym ps{i, &s, 0, SIZE_MAX};

            if (s.binding == SymbolBinding::External) {
                ps.shndx = kShnUndef;
                pendingExternals.push_back(ps);
            } else if (s.binding == SymbolBinding::Local) {
                ps.shndx = secRodata;
                pendingLocals.push_back(ps);
            } else {
                ps.shndx = secRodata;
                pendingDefinedGlobals.push_back(ps);
            }
        }

        // Collect .data symbols (writable scalar globals). Defined globals whose
        // names coalesce the text sections' undefined references.
        for (uint32_t i = 1; i < data.symbols().count(); ++i) {
            const auto &s = data.symbols().at(i);
            PendingSym ps{i, &s, secData, SIZE_MAX, true};
            if (s.binding == SymbolBinding::Local)
                pendingLocals.push_back(ps);
            else
                pendingDefinedGlobals.push_back(ps);
        }

        // Write local symbols first (ELF requires locals before globals).
        for (const auto &ps : pendingLocals) {
            uint32_t nameOff = strtab.add(ps.sym->name);
            uint8_t type = elfSymbolType(*ps.sym);
            uint64_t value = 0;
            const CodeSection &source =
                ps.fromData ? data : (ps.textIdx != SIZE_MAX) ? textSections[ps.textIdx] : rodata;
            const char *secName =
                ps.fromData ? ".data" : (ps.textIdx != SIZE_MAX) ? ".text" : ".rodata";
            if (!physicalSymbolValue(source, *ps.sym, secName, err, value))
                return false;
            writeSym(symtabBytes,
                     nameOff,
                     (kStbLocal << 4) | type,
                     kStvDefault,
                     ps.shndx,
                     value,
                     ps.sym->size);
            uint32_t elfIdx = elfLocalCount++;
            if (ps.fromData)
                continue; // .data symbols referenced by name, not by index map
            if (ps.textIdx != SIZE_MAX)
                textSymMaps[ps.textIdx][ps.origIdx] = elfIdx;
            else
                rodataSymMap[ps.origIdx] = elfIdx;
        }

        uint32_t firstGlobalIdx = elfLocalCount;

        // Write defined global symbols first (so defined takes priority in dedup map).
        std::unordered_map<std::string, uint32_t> globalNameMap;
        uint32_t elfGlobalIdx = elfLocalCount;

        for (const auto &ps : pendingDefinedGlobals) {
            auto it = globalNameMap.find(ps.sym->name);
            if (it != globalNameMap.end()) {
                err << "ElfWriter: duplicate global symbol '" << ps.sym->name << "'\n";
                return false;
            }
            uint32_t nameOff = strtab.add(ps.sym->name);
            uint64_t value = 0;
            const CodeSection &source =
                ps.fromData ? data : (ps.textIdx != SIZE_MAX) ? textSections[ps.textIdx] : rodata;
            const char *secName =
                ps.fromData ? ".data" : (ps.textIdx != SIZE_MAX) ? ".text" : ".rodata";
            if (!physicalSymbolValue(source, *ps.sym, secName, err, value))
                return false;
            writeSym(symtabBytes,
                     nameOff,
                     (kStbGlobal << 4) | elfSymbolType(*ps.sym),
                     kStvDefault,
                     ps.shndx,
                     value,
                     ps.sym->size);
            globalNameMap[ps.sym->name] = elfGlobalIdx;
            if (ps.fromData)
                ; // .data globals referenced by name (globalNameMap), no index map
            else if (ps.textIdx != SIZE_MAX)
                textSymMaps[ps.textIdx][ps.origIdx] = elfGlobalIdx;
            else
                rodataSymMap[ps.origIdx] = elfGlobalIdx;
            ++elfGlobalIdx;
        }

        // Then write external (undefined) symbols (reuse if name already present).
        for (const auto &ps : pendingExternals) {
            auto it = globalNameMap.find(ps.sym->name);
            if (it != globalNameMap.end()) {
                if (ps.textIdx != SIZE_MAX)
                    textSymMaps[ps.textIdx][ps.origIdx] = it->second;
                else
                    rodataSymMap[ps.origIdx] = it->second;
                continue;
            }
            uint32_t nameOff = strtab.add(ps.sym->name);
            writeSym(
                symtabBytes, nameOff, (kStbGlobal << 4) | kSttNotype, kStvDefault, kShnUndef, 0, 0);
            globalNameMap[ps.sym->name] = elfGlobalIdx;
            if (ps.textIdx != SIZE_MAX)
                textSymMaps[ps.textIdx][ps.origIdx] = elfGlobalIdx;
            else
                rodataSymMap[ps.origIdx] = elfGlobalIdx;
            ++elfGlobalIdx;
        }

        // Build name→ELF index map for defined rodata symbols (cross-section refs).
        std::unordered_map<std::string, uint32_t> definedRodataByName;
        for (uint32_t i = 1; i < rodata.symbols().count(); ++i) {
            const auto &s = rodata.symbols().at(i);
            if (s.binding != SymbolBinding::External) {
                auto elfIt = rodataSymMap.find(i);
                if (elfIt != rodataSymMap.end()) {
                    auto [it, inserted] = definedRodataByName.emplace(s.name, elfIt->second);
                    if (!inserted)
                        it->second = UINT32_MAX;
                }
            }
        }

        std::unordered_map<std::string, uint32_t> definedTextByName;
        for (size_t ti = 0; ti < N; ++ti) {
            const auto &sec = textSections[ti];
            for (uint32_t i = 1; i < sec.symbols().count(); ++i) {
                const auto &s = sec.symbols().at(i);
                if (s.binding == SymbolBinding::External)
                    continue;
                auto elfIt = textSymMaps[ti].find(i);
                if (elfIt == textSymMaps[ti].end())
                    continue;
                auto [it, inserted] = definedTextByName.emplace(s.name, elfIt->second);
                if (!inserted)
                    it->second = UINT32_MAX;
            }
        }

        /// Resolve one multi-section relocation to its final ELF symbol index,
        /// using exact CodeSection identities when text offsets are ambiguous.
        auto resolveRelocSym = [&](const Relocation &rel,
                                   const CodeSection &source,
                                   const std::unordered_map<uint32_t, uint32_t> &sourceMap,
                                   const std::unordered_map<std::string, uint32_t> &targetByName,
                                   const char *sectionName,
                                   size_t sourceTextIndex,
                                   uint32_t &elfSymIdx) -> bool {
            elfSymIdx = 0;
            if (rel.targetSection != SymbolSection::Undefined) {
                if (rel.targetOffsetValid) {
                    const CodeSection *target = nullptr;
                    const char *targetName = nullptr;
                    if (rel.targetSection == SymbolSection::Rodata) {
                        target = &rodata;
                        targetName = ".rodata";
                        elfSymIdx = static_cast<uint32_t>(N + 1);
                    } else {
                        size_t textIdx = sourceTextIndex;
                        if (rel.targetSectionIdentityValid) {
                            textIdx = SIZE_MAX;
                            size_t matches = 0;
                            for (size_t ti = 0; ti < N; ++ti) {
                                if (textSections[ti].matchesSectionIdentity(
                                        rel.targetSectionIdentity)) {
                                    textIdx = ti;
                                    ++matches;
                                }
                            }
                            if (matches > 1) {
                                err << "ElfWriter: relocation in " << sectionName << " at offset "
                                    << rel.offset
                                    << " references duplicate .text section identity\n";
                                return false;
                            }
                        } else if (textIdx == SIZE_MAX || textIdx >= N ||
                                   rel.targetOffset > textSections[textIdx].bytes().size()) {
                            textIdx = SIZE_MAX;
                            size_t matches = 0;
                            for (size_t ti = 0; ti < N; ++ti) {
                                if (rel.targetOffset <= textSections[ti].bytes().size()) {
                                    textIdx = ti;
                                    ++matches;
                                }
                            }
                            if (matches > 1) {
                                err << "ElfWriter: relocation in " << sectionName << " at offset "
                                    << rel.offset << " references ambiguous .text offset "
                                    << rel.targetOffset
                                    << "; use section-identity relocation overload\n";
                                return false;
                            }
                        }
                        if (textIdx == SIZE_MAX || textIdx >= N) {
                            err << "ElfWriter: relocation in " << sectionName << " at offset "
                                << rel.offset << " references .text offset " << rel.targetOffset
                                << " beyond section contents\n";
                            return false;
                        }
                        target = &textSections[textIdx];
                        targetName = ".text";
                        elfSymIdx = static_cast<uint32_t>(1 + textIdx);
                    }
                    if (rel.targetOffset > target->bytes().size()) {
                        err << "ElfWriter: relocation in " << sectionName << " at offset "
                            << rel.offset << " references " << targetName << " offset "
                            << rel.targetOffset << " beyond section contents\n";
                        return false;
                    }
                    return true;
                }
                if (rel.symbolIndex >= source.symbols().count()) {
                    err << "ElfWriter: relocation in " << sectionName << " at offset " << rel.offset
                        << " references unknown symbol index " << rel.symbolIndex << "\n";
                    return false;
                }
                const Symbol &sym = source.symbols().at(rel.symbolIndex);
                auto nameIt = targetByName.find(sym.name);
                if (nameIt == targetByName.end()) {
                    err << "ElfWriter: relocation in " << sectionName << " at offset " << rel.offset
                        << " references missing cross-section target '" << sym.name << "'\n";
                    return false;
                }
                if (nameIt->second == UINT32_MAX) {
                    err << "ElfWriter: relocation in " << sectionName << " at offset " << rel.offset
                        << " references ambiguous cross-section target '" << sym.name << "'\n";
                    return false;
                }
                elfSymIdx = nameIt->second;
                return true;
            }

            auto it = sourceMap.find(rel.symbolIndex);
            if (it == sourceMap.end()) {
                err << "ElfWriter: relocation in " << sectionName << " at offset " << rel.offset
                    << " references unknown symbol index " << rel.symbolIndex << "\n";
                return false;
            }
            elfSymIdx = it->second;
            return true;
        };

        // --- 5. Build .rela.text.* entries ---
        std::vector<std::vector<uint8_t>> allRelaBytes(N);
        for (size_t ti = 0; ti < N; ++ti) {
            for (const auto &rel : textSections[ti].relocations()) {
                if (!validateRelocationShape(
                        "ElfWriter", arch_, textSections[ti], rel, ".text", err))
                    return false;
                uint32_t elfSymIdx = 0;
                const auto &targetMap = (rel.targetSection == SymbolSection::Rodata)
                                            ? definedRodataByName
                                            : definedTextByName;
                if (!resolveRelocSym(
                        rel, textSections[ti], textSymMaps[ti], targetMap, ".text", ti, elfSymIdx))
                    return false;
                int64_t effectiveAddend = rel.addend;
                if (rel.targetOffsetValid) {
                    if (!checkedSectionOffsetAddend(rel.addend,
                                                    rel.targetOffset,
                                                    "ElfWriter",
                                                    ".text",
                                                    rel.offset,
                                                    err,
                                                    effectiveAddend))
                        return false;
                }
                const size_t physicalRelOffset = rel.offset - textSections[ti].logicalOffsetBias();
                uint32_t relocType = elfRelocType(rel.kind, arch_);
                uint64_t rInfo = (static_cast<uint64_t>(elfSymIdx) << 32) | relocType;
                writeRela(allRelaBytes[ti],
                          static_cast<uint64_t>(physicalRelOffset),
                          rInfo,
                          effectiveAddend);
            }
        }

        std::vector<uint8_t> relaRodataBytes;
        for (const auto &rel : rodata.relocations()) {
            if (!validateRelocationShape("ElfWriter", arch_, rodata, rel, ".rodata", err))
                return false;
            uint32_t elfSymIdx = 0;
            const auto &targetMap = (rel.targetSection == SymbolSection::Text)
                                        ? definedTextByName
                                        : definedRodataByName;
            if (!resolveRelocSym(
                    rel, rodata, rodataSymMap, targetMap, ".rodata", SIZE_MAX, elfSymIdx))
                return false;
            int64_t effectiveAddend = rel.addend;
            if (rel.targetOffsetValid) {
                if (!checkedSectionOffsetAddend(rel.addend,
                                                rel.targetOffset,
                                                "ElfWriter",
                                                ".rodata",
                                                rel.offset,
                                                err,
                                                effectiveAddend))
                    return false;
            }
            const size_t physicalRelOffset = rel.offset - rodata.logicalOffsetBias();
            uint32_t relocType = elfRelocType(rel.kind, arch_);
            uint64_t rInfo = (static_cast<uint64_t>(elfSymIdx) << 32) | relocType;
            writeRela(
                relaRodataBytes, static_cast<uint64_t>(physicalRelOffset), rInfo, effectiveAddend);
        }

        // --- 6. Compute file layout ---
        uint64_t textAlign = (arch_ == ObjArch::X86_64) ? 16 : 4;

        // Text section offsets.
        std::vector<uint64_t> textOffsets(N);
        std::vector<uint64_t> textSizes(N);
        uint64_t cursor = kEhSize;
        for (size_t i = 0; i < N; ++i) {
            textSizes[i] = textSections[i].bytes().size();
            if (!checkedAlignUpU64(
                    cursor, textAlign, "ElfWriter", ".text offset", err, textOffsets[i]) ||
                !checkedAddU64(
                    textOffsets[i], textSizes[i], "ElfWriter", ".text data", err, cursor))
                return false;
        }

        uint64_t rodataSize = rodata.bytes().size();
        uint64_t offRodata = 0;
        if (!checkedAlignUpU64(cursor, 8, "ElfWriter", ".rodata offset", err, offRodata) ||
            !checkedAddU64(offRodata, rodataSize, "ElfWriter", ".rodata data", err, cursor))
            return false;

        // .rela.text.* offsets (always allocated, even if empty).
        std::vector<uint64_t> relaOffsets(N);
        std::vector<uint64_t> relaSizes(N);
        for (size_t i = 0; i < N; ++i) {
            relaSizes[i] = allRelaBytes[i].size();
            if (!checkedAlignUpU64(
                    cursor, 8, "ElfWriter", ".rela.text offset", err, relaOffsets[i]) ||
                !checkedAddU64(
                    relaOffsets[i], relaSizes[i], "ElfWriter", ".rela.text data", err, cursor))
                return false;
        }

        uint64_t relaRodataSize = relaRodataBytes.size();
        uint64_t offRelaRodata = 0;
        if (!checkedAlignUpU64(cursor, 8, "ElfWriter", ".rela.rodata offset", err, offRelaRodata) ||
            !checkedAddU64(
                offRelaRodata, relaRodataSize, "ElfWriter", ".rela.rodata data", err, cursor))
            return false;

        uint64_t symtabSize = symtabBytes.size();
        uint64_t offSymtab = 0;
        if (!checkedAlignUpU64(cursor, 8, "ElfWriter", ".symtab offset", err, offSymtab) ||
            !checkedAddU64(offSymtab, symtabSize, "ElfWriter", ".symtab data", err, cursor))
            return false;

        uint64_t strtabSize = strtab.size();
        uint64_t offStrtab = cursor;
        if (!checkedAddU64(offStrtab, strtabSize, "ElfWriter", ".strtab data", err, cursor))
            return false;

        uint64_t shstrtabSize = shstrtab.size();
        uint64_t offShstrtab = cursor;
        if (!checkedAddU64(offShstrtab, shstrtabSize, "ElfWriter", ".shstrtab data", err, cursor))
            return false;

        uint64_t offNoteGnuStack = cursor;
        uint64_t debugLineSize = debugLineData_.size();
        uint64_t offDebugLine = offNoteGnuStack;
        uint64_t debugEnd = 0;
        if (!checkedAddU64(
                offDebugLine, debugLineSize, "ElfWriter", ".debug_line data", err, debugEnd))
            return false;

        // .data (writable globals) — appended after .debug_line, before the SHT.
        uint64_t dataSize = hasData ? data.bytes().size() : 0;
        uint64_t offData = debugEnd;
        uint64_t dataEnd = debugEnd;
        if (hasData) {
            if (!checkedAlignUpU64(debugEnd, 8, "ElfWriter", ".data offset", err, offData) ||
                !checkedAddU64(offData, dataSize, "ElfWriter", ".data data", err, dataEnd))
                return false;
        }

        uint64_t offShtab = 0;
        if (!checkedAlignUpU64(dataEnd, 8, "ElfWriter", "section header table", err, offShtab))
            return false;

        // --- 7. Build the file ---
        std::vector<uint8_t> file;
        size_t reserveSize = 0;
        if (!reserveFileBytes(offShtab, numSections, err, reserveSize))
            return false;
        file.reserve(reserveSize);

        uint16_t machine = (arch_ == ObjArch::X86_64) ? kEmX86_64 : kEmAarch64;
        writeEhdr(file, machine, offShtab, numSections, secShstrtab);

        // .text.* section data
        for (size_t i = 0; i < N; ++i) {
            padTo(file, static_cast<size_t>(textOffsets[i]));
            file.insert(file.end(), textSections[i].bytes().begin(), textSections[i].bytes().end());
        }

        // .rodata
        padTo(file, static_cast<size_t>(offRodata));
        file.insert(file.end(), rodata.bytes().begin(), rodata.bytes().end());

        // .rela.text.* data
        for (size_t i = 0; i < N; ++i) {
            if (!allRelaBytes[i].empty()) {
                padTo(file, static_cast<size_t>(relaOffsets[i]));
                file.insert(file.end(), allRelaBytes[i].begin(), allRelaBytes[i].end());
            }
        }

        if (!relaRodataBytes.empty()) {
            padTo(file, static_cast<size_t>(offRelaRodata));
            file.insert(file.end(), relaRodataBytes.begin(), relaRodataBytes.end());
        }

        // .symtab
        padTo(file, static_cast<size_t>(offSymtab));
        file.insert(file.end(), symtabBytes.begin(), symtabBytes.end());

        // .strtab
        padTo(file, static_cast<size_t>(offStrtab));
        {
            const auto &d = strtab.data();
            file.insert(file.end(), d.begin(), d.end());
        }

        // .shstrtab
        padTo(file, static_cast<size_t>(offShstrtab));
        {
            const auto &d = shstrtab.data();
            file.insert(file.end(), d.begin(), d.end());
        }

        // .note.GNU-stack (zero size — no data to append)

        // .debug_line (optional)
        if (hasDebugLine)
            file.insert(file.end(), debugLineData_.begin(), debugLineData_.end());

        // .data (optional, writable)
        if (hasData) {
            padTo(file, static_cast<size_t>(offData));
            file.insert(file.end(), data.bytes().begin(), data.bytes().end());
        }

        // --- 8. Section header table ---
        padTo(file, static_cast<size_t>(offShtab));

        // [0] Null section
        writeShdr(file, 0, kShtNull, 0, 0, 0, 0, 0, 0, 0);

        // [1..N] .text.funcname sections
        for (size_t i = 0; i < N; ++i) {
            writeShdr(file,
                      shNameText[i],
                      kShtProgbits,
                      kShfAlloc | kShfExecinstr,
                      textOffsets[i],
                      textSizes[i],
                      0,
                      0,
                      textAlign,
                      0);
        }

        // [N+1] .rodata
        writeShdr(file, shNameRodata, kShtProgbits, kShfAlloc, offRodata, rodataSize, 0, 0, 8, 0);

        // [N+2..2N+1] .rela.text.funcname sections
        for (size_t i = 0; i < N; ++i) {
            writeShdr(file,
                      shNameRelaText[i],
                      kShtRela,
                      kShfInfoLink,
                      relaOffsets[i],
                      relaSizes[i],
                      secSymtab,
                      secText(i),
                      8,
                      24);
        }

        // .rela.rodata
        writeShdr(file,
                  shNameRelaRodata,
                  kShtRela,
                  kShfInfoLink,
                  offRelaRodata,
                  relaRodataSize,
                  secSymtab,
                  secRodata,
                  8,
                  24);

        // .symtab
        writeShdr(file,
                  shNameSymtab,
                  kShtSymtab,
                  0,
                  offSymtab,
                  symtabSize,
                  secStrtab,
                  firstGlobalIdx,
                  8,
                  24);

        // .strtab
        writeShdr(file, shNameStrtab, kShtStrtab, 0, offStrtab, strtabSize, 0, 0, 1, 0);

        // .shstrtab
        writeShdr(file, shNameShstrtab, kShtStrtab, 0, offShstrtab, shstrtabSize, 0, 0, 1, 0);

        // .note.GNU-stack
        writeShdr(file, shNameNoteGnuStack, kShtProgbits, 0, offNoteGnuStack, 0, 0, 0, 1, 0);

        // .debug_line (optional, non-alloc)
        if (hasDebugLine)
            writeShdr(
                file, shNameDebugLine, kShtProgbits, 0, offDebugLine, debugLineSize, 0, 0, 1, 0);

        // [last] .data (optional, writable, alloc)
        if (hasData)
            writeShdr(file,
                      shNameData,
                      kShtProgbits,
                      kShfAlloc | kShfWrite,
                      offData,
                      dataSize,
                      0,
                      0,
                      8,
                      0);

        // --- 9. Commit to the selected file or memory sink ---
        return commitOutput(path, file, "ElfWriter", err);
    } catch (const std::exception &ex) {
        err << "ElfWriter: " << ex.what() << "\n";
        return false;
    }
}

} // namespace zanna::codegen::objfile
