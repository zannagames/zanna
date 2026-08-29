//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/MachOBindRebase.cpp
// Purpose: Mach-O bind/rebase opcode emission and symbol table construction.
//          Encodes GOT bindings, TLV descriptor bindings, ASLR rebases, and
//          the nlist symbol table for __LINKEDIT.
// Key invariants:
//   - Bind opcodes use two-level namespace (MH_TWOLEVEL) with per-symbol ordinals
//   - OBJC_CLASS_$/OBJC_METACLASS_$ symbols use flat lookup (ordinal -2)
//   - Symbol names Mach-O mangled (underscore prefix)
//   - Rebase offsets sorted, run-length encoded for consecutive 8-byte pointers
//   - String table NUL-separated, 4-byte aligned at end
// Ownership/Lifetime:
//   - Stateless builder functions — no persistent state
// Links: codegen/common/linker/MachOBindRebase.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file MachOBindRebase.cpp
 * @brief Implements compact dyld opcode streams and Mach-O `nlist_64` output.
 */

#include "codegen/common/linker/MachOBindRebase.hpp"
#include "codegen/common/linker/ExeWriterUtil.hpp"
#include "codegen/common/linker/NameMangling.hpp"

#include <algorithm>

namespace zanna::codegen::linker {

using encoding::writeLE32;
using encoding::writeLE64;
using encoding::writeULEB128;

namespace {

// Bind opcode constants (high 4 bits = opcode, low 4 bits = immediate).
static constexpr uint8_t BIND_OPCODE_DONE = 0x00;
static constexpr uint8_t BIND_OPCODE_SET_DYLIB_ORDINAL_IMM = 0x10;
static constexpr uint8_t BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB = 0x20;
static constexpr uint8_t BIND_OPCODE_SET_DYLIB_SPECIAL_IMM = 0x30;
static constexpr uint8_t BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM = 0x40;
static constexpr uint8_t BIND_OPCODE_SET_TYPE_IMM = 0x50;
static constexpr uint8_t BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x70;
static constexpr uint8_t BIND_OPCODE_DO_BIND = 0x90;
static constexpr uint8_t BIND_TYPE_POINTER = 1;

// Rebase opcode constants (high 4 bits = opcode, low 4 bits = immediate).
static constexpr uint8_t REBASE_OPCODE_DONE = 0x00;
static constexpr uint8_t REBASE_OPCODE_SET_TYPE_IMM = 0x10;
static constexpr uint8_t REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x20;
static constexpr uint8_t REBASE_OPCODE_DO_REBASE_IMM_TIMES = 0x50;
static constexpr uint8_t REBASE_TYPE_POINTER = 1;

// Symbol table constants.
static constexpr uint8_t N_EXT = 0x01;
static constexpr uint8_t N_UNDF = 0x00;
static constexpr uint8_t N_SECT = 0x0E;

/// @brief Emits one pointer-bind operation for a symbol and segment offset.
/// @param bindData Destination dyld bind stream.
/// @param symbolName Unmangled external symbol name.
/// @param segmentOffset Offset from the selected segment's VM base.
/// @param dataSegIndex Segment load-command index.
/// @param dylibOrdinal One-based `MH_TWOLEVEL` ordinal, or zero for flat lookup.
void emitBindEntry(std::vector<uint8_t> &bindData,
                   const std::string &symbolName,
                   uint64_t segmentOffset,
                   uint32_t dataSegIndex,
                   uint32_t dylibOrdinal) {
    // Two-level namespace: set dylib ordinal for this symbol.
    // Ordinal 0 = flat lookup (BIND_SPECIAL_DYLIB_FLAT_LOOKUP, -2).
    if (dylibOrdinal == 0) {
        bindData.push_back(BIND_OPCODE_SET_DYLIB_SPECIAL_IMM | 0x0E);
    } else if (dylibOrdinal <= 15) {
        bindData.push_back(BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | (dylibOrdinal & 0x0F));
    } else {
        bindData.push_back(BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB);
        writeULEB128(bindData, dylibOrdinal);
    }

    // Symbol name with Mach-O underscore prefix.
    bindData.push_back(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM | 0);
    std::string machoName = machoMangle(symbolName);
    bindData.insert(bindData.end(), machoName.begin(), machoName.end());
    bindData.push_back(0); // NUL terminator

    bindData.push_back(BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER);

    bindData.push_back(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | (dataSegIndex & 0x0F));
    writeULEB128(bindData, segmentOffset);

    bindData.push_back(BIND_OPCODE_DO_BIND);
}

} // anonymous namespace

/// @brief Looks up a symbol's dylib ordinal.
/// @param symName Unmangled dynamic symbol name.
/// @param symOrdinals Explicit symbol-to-ordinal map.
/// @return Mapped value, including zero for flat lookup, or one for libSystem
///         when no entry exists.
static uint32_t lookupOrdinal(const std::string &symName,
                              const std::unordered_map<std::string, uint32_t> &symOrdinals) {
    auto it = symOrdinals.find(symName);
    return (it != symOrdinals.end()) ? it->second : 1;
}

/// @copydoc buildBindOpcodes(std::vector<uint8_t> &, const std::vector<GotEntry> &, const
/// LinkLayout &, uint64_t, uint32_t, const std::unordered_map<std::string, uint32_t> &,
/// std::ostream &)
bool buildBindOpcodes(std::vector<uint8_t> &bindData,
                      const std::vector<GotEntry> &gotEntries,
                      const LinkLayout &layout,
                      uint64_t dataSegVmAddr,
                      uint32_t dataSegIndex,
                      const std::unordered_map<std::string, uint32_t> &symOrdinals,
                      std::ostream &err) {
    // Bind GOT entries for dynamic symbols.
    for (const auto &ge : gotEntries) {
        uint64_t offset = ge.gotAddr - dataSegVmAddr;
        uint32_t ordinal = lookupOrdinal(ge.symbolName, symOrdinals);
        emitBindEntry(bindData, ge.symbolName, offset, dataSegIndex, ordinal);
    }

    // Bind TLV descriptor thunk fields → _tlv_bootstrap.
    // dyld uses these bind events to discover TLV descriptors and initialize
    // per-image TLS metadata. Without this, _tlv_bootstrap (a fail-stub) aborts.
    //
    // Identify descriptor sections through the explicit `tlvDescriptors` flag
    // set by SectionMerger when it merges `__thread_vars` input sections.
    // Matching by name alone would be brittle: ELF `.tdata` holds TLS template
    // bytes, not TLV descriptors, and the section merger reuses the name on
    // Mach-O for descriptors while routing ELF/PE TLS template data through
    // `.tdata_template` / `.tdata`. The flag pins the meaning to the role.
    for (const auto &sec : layout.sections) {
        if (!sec.tlvDescriptors)
            continue;

        // Each TLV descriptor is 24 bytes: {thunk(8), key(8), offset(8)}.
        // A non-multiple-of-24 size means an upstream pass concatenated
        // non-descriptor bytes into the descriptor section. Silently
        // dropping the trailing partial descriptor leaves its thunk field
        // pointing at uninitialized memory; dyld then aborts on first
        // access. Fail loudly instead.
        if (sec.data.size() % 24 != 0) {
            err << "error: Mach-O TLV descriptor section '" << sec.name << "' has size "
                << sec.data.size() << " bytes, which is not a multiple of 24\n";
            return false;
        }

        const size_t numDescriptors = sec.data.size() / 24;
        for (size_t i = 0; i < numDescriptors; ++i) {
            uint64_t descVA = sec.virtualAddr + i * 24;
            uint64_t segOff = descVA - dataSegVmAddr;
            emitBindEntry(bindData, "_tlv_bootstrap", segOff, dataSegIndex, 1);
        }
    }

    // Bind data-pointer references to dynamic symbols (e.g., ObjC classrefs,
    // superrefs, protocol refs). These are Abs64 relocations whose target is an
    // external symbol — the linker writes 0 and dyld fills the actual pointer.
    for (const auto &be : layout.bindEntries) {
        const auto &sec = layout.sections[be.sectionIndex];
        uint64_t addr = sec.virtualAddr + be.offset;
        if (addr >= dataSegVmAddr) {
            uint64_t segOff = addr - dataSegVmAddr;
            uint32_t ordinal = lookupOrdinal(be.symbolName, symOrdinals);
            emitBindEntry(bindData, be.symbolName, segOff, dataSegIndex, ordinal);
        }
    }

    bindData.push_back(BIND_OPCODE_DONE);
    return true;
}

/// @copydoc buildRebaseOpcodes(std::vector<uint8_t> &, const LinkLayout &, uint64_t, uint32_t)
void buildRebaseOpcodes(std::vector<uint8_t> &rebaseData,
                        const LinkLayout &layout,
                        uint64_t dataSegVmAddr,
                        uint32_t dataSegIndex) {
    if (layout.rebaseEntries.empty())
        return;

    // Collect segment-relative offsets for all rebase locations in __DATA.
    std::vector<uint64_t> offsets;
    offsets.reserve(layout.rebaseEntries.size());
    for (const auto &entry : layout.rebaseEntries) {
        const auto &sec = layout.sections[entry.sectionIndex];
        uint64_t addr = sec.virtualAddr + entry.offset;
        if (addr >= dataSegVmAddr)
            offsets.push_back(addr - dataSegVmAddr);
    }
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());

    // Emit rebase opcodes.
    rebaseData.push_back(REBASE_OPCODE_SET_TYPE_IMM | REBASE_TYPE_POINTER);

    size_t i = 0;
    while (i < offsets.size()) {
        // Count consecutive pointers (each 8 bytes apart).
        size_t runLen = 1;
        while (i + runLen < offsets.size() && offsets[i + runLen] == offsets[i] + runLen * 8)
            ++runLen;

        // Set segment and offset.
        rebaseData.push_back(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | (dataSegIndex & 0x0F));
        writeULEB128(rebaseData, offsets[i]);

        // Rebase the run of consecutive pointers (max 15 per opcode).
        size_t remaining = runLen;
        while (remaining > 0) {
            uint8_t count = static_cast<uint8_t>(std::min<size_t>(remaining, 15));
            rebaseData.push_back(REBASE_OPCODE_DO_REBASE_IMM_TIMES | count);
            remaining -= count;
        }

        i += runLen; // Advance past the run we just emitted.
    }

    rebaseData.push_back(REBASE_OPCODE_DONE);
}

/// @copydoc buildSymtab(std::vector<uint8_t> &, std::vector<uint8_t> &, const LinkLayout &, const
/// std::vector<size_t> &, bool, const std::unordered_set<std::string> &, const
/// std::unordered_map<std::string, uint32_t> &, uint32_t &, uint32_t &, uint32_t &)
void buildSymtab(std::vector<uint8_t> &symtabData,
                 std::vector<uint8_t> &strtabData,
                 const LinkLayout &layout,
                 const std::vector<size_t> &sectionOrder,
                 bool emitLocalSymbols,
                 const std::unordered_set<std::string> &dynSyms,
                 const std::unordered_map<std::string, uint32_t> &symOrdinals,
                 uint32_t &nLocal,
                 uint32_t &nExtDef,
                 uint32_t &nUndef) {
    strtabData.push_back(0); // String table starts with NUL.

    /// Appends one string and returns its starting offset in the string table.
    auto addString = [&](const std::string &s) -> uint32_t {
        uint32_t off = static_cast<uint32_t>(strtabData.size());
        strtabData.insert(strtabData.end(), s.begin(), s.end());
        strtabData.push_back(0);
        return off;
    };

    /// Appends one serialized 64-bit Mach-O symbol-table record.
    auto writeNlist =
        [&](uint32_t strx, uint8_t type, uint8_t sect, uint16_t desc, uint64_t value) {
            writeLE32(symtabData, strx);
            symtabData.push_back(type);
            symtabData.push_back(sect);
            symtabData.push_back(static_cast<uint8_t>(desc));
            symtabData.push_back(static_cast<uint8_t>(desc >> 8));
            writeLE64(symtabData, value);
        };

    // Non-external definitions first (LC_DYSYMTAB requires locals, then
    // external definitions, then undefined imports). `n_sect` is a byte, so
    // definitions beyond the 255th section ordinal cannot be described and
    // are left out rather than mis-attributed.
    nLocal = 0;
    if (emitLocalSymbols) {
        for (const auto &rec : collectLocalSymbolRecords(layout, sectionOrder, true)) {
            if (rec.sectionSlot + 1 > 255)
                continue;
            const uint32_t strx = addString(machoMangle(rec.name));
            writeNlist(strx, N_SECT, static_cast<uint8_t>(rec.sectionSlot + 1), 0, rec.addr);
            nLocal++;
        }
    }

    // External defined: _main.
    nExtDef = 0;
    {
        auto it = layout.globalSyms.find("main");
        if (it == layout.globalSyms.end())
            it = layout.globalSyms.find("_main");
        if (it != layout.globalSyms.end()) {
            uint32_t strx = addString("_main");
            writeNlist(strx, N_EXT | N_SECT, 1, 0, it->second.resolvedAddr);
            nExtDef++;
        }
    }

    // Undefined: dynamic imports with MH_TWOLEVEL library ordinals.
    // nlist n_desc bits [15:8] encode the 1-based dylib ordinal.
    // DYNAMIC_LOOKUP_ORDINAL (0xFE) is used for flat-lookup symbols.
    static constexpr uint32_t DYNAMIC_LOOKUP_ORDINAL = 0xFE;
    nUndef = 0;
    std::vector<std::string> sortedDyn(dynSyms.begin(), dynSyms.end());
    std::sort(sortedDyn.begin(), sortedDyn.end());
    for (const auto &sym : sortedDyn) {
        if (sym.size() > 6 && sym.substr(0, 6) == "__got_")
            continue;
        uint32_t strx = addString(machoMangle(sym));
        uint32_t ordinal = lookupOrdinal(sym, symOrdinals);
        if (ordinal == 0)
            ordinal = DYNAMIC_LOOKUP_ORDINAL;
        uint16_t desc = static_cast<uint16_t>(ordinal << 8);
        writeNlist(strx, N_EXT | N_UNDF, 0, desc, 0);
        nUndef++;
    }

    while (strtabData.size() % 4 != 0)
        strtabData.push_back(0);
}

} // namespace zanna::codegen::linker
