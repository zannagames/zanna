//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/MachOExeWriter.cpp
// Purpose: Writes a Mach-O executable (MH_EXECUTE) with dynamic library support.
//          Generates __LINKEDIT segment with symbol table, bind opcodes, and
//          optional ad-hoc code signature for arm64.
// Key invariants:
//   - __PAGEZERO: vmaddr=0, vmsize=4GB (must be FIRST segment)
//   - __TEXT: starts at fileoff=0, encompasses header + code + rodata
//   - __DATA: writable segment containing data + GOT entries
//   - __LINKEDIT: contains symtab, strtab, bind opcodes
//   - LC_LOAD_DYLINKER: "/usr/lib/dyld"
//   - LC_LOAD_DYLIB: at least libSystem.B.dylib
//   - LC_DYLD_INFO_ONLY: non-lazy bind opcodes for GOT entries
//   - Section addresses match SectionMerger-assigned VAs exactly
//   - Page alignment: 16KB for arm64, 4KB for x86_64
// Security features:
//   - MH_PIE: ASLR — kernel randomizes load address on each execution
//   - MH_TWOLEVEL: two-level namespace — per-symbol dylib ordinals
//   - __PAGEZERO: null-pointer dereference guard (4GB unmapped region)
//   - Embedded ad-hoc code signature (arm64) in Apple-compatible SuperBlob form
//   - W^X: __TEXT is R+X, __DATA is R+W — no segment is both writable
//     and executable
//   - Non-lazy binding: GOT entries resolved by dyld before main() runs
// Links: codegen/common/linker/MachOExeWriter.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file MachOExeWriter.cpp
 * @brief Implements secure, direct Mach-O executable and `__LINKEDIT` emission.
 */

#include "codegen/common/linker/MachOExeWriter.hpp"
#include "codegen/common/MachOBuildVersion.hpp"
#include "codegen/common/linker/MachOBindRebase.hpp"
#include "codegen/common/linker/MachOCodeSign.hpp"
#include "codegen/common/linker/NameMangling.hpp"

#include "codegen/common/linker/AlignUtil.hpp"
#include "codegen/common/linker/ExeWriterUtil.hpp"

#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::linker {

using encoding::writeLE32;
using encoding::writeLE64;
using encoding::writePad;
using encoding::writeStr;

namespace {

// Mach-O constants.
static constexpr uint32_t MH_MAGIC_64 = 0xFEEDFACF;
static constexpr uint32_t MH_EXECUTE = 2;
static constexpr uint32_t MH_NOUNDEFS = 0x1;
static constexpr uint32_t MH_DYLDLINK = 0x4;
static constexpr uint32_t MH_TWOLEVEL = 0x80;
static constexpr uint32_t MH_PIE = 0x200000;
static constexpr uint32_t MH_HAS_TLV_DESCRIPTORS = 0x800000;

// Mach-O section type constants (low 8 bits of flags).
static constexpr uint32_t S_REGULAR = 0x00;
static constexpr uint32_t S_ZEROFILL = 0x01;
static constexpr uint32_t S_CSTRING_LITERALS = 0x02;
static constexpr uint32_t S_LITERAL_POINTERS = 0x05;
static constexpr uint32_t S_MOD_INIT_FUNC_POINTERS = 0x09;
static constexpr uint32_t S_MOD_TERM_FUNC_POINTERS = 0x0A;
static constexpr uint32_t S_THREAD_LOCAL_REGULAR = 0x11;
static constexpr uint32_t S_THREAD_LOCAL_ZEROFILL = 0x12;
static constexpr uint32_t S_THREAD_LOCAL_VARIABLES = 0x13;
static constexpr uint32_t S_COALESCED = 0x0B;
static constexpr uint32_t S_ATTR_PURE_INSTRUCTIONS = 0x80000000u;
static constexpr uint32_t S_ATTR_SOME_INSTRUCTIONS = 0x00000400u;
static constexpr uint32_t S_ATTR_NO_DEAD_STRIP = 0x10000000u;

static constexpr uint32_t CPU_TYPE_X86_64 = 0x01000007;
static constexpr uint32_t CPU_TYPE_ARM64 = 0x0100000C;
static constexpr uint32_t CPU_SUBTYPE_ARM64_ALL = 0;
static constexpr uint32_t CPU_SUBTYPE_X86_64_ALL = 3;

static constexpr uint32_t LC_SEGMENT_64 = 0x19;
static constexpr uint32_t LC_MAIN = 0x80000028;
static constexpr uint32_t LC_LOAD_DYLIB = 0x0C;
static constexpr uint32_t LC_LOAD_DYLINKER = 0x0E;
static constexpr uint32_t LC_SYMTAB = 0x02;
static constexpr uint32_t LC_DYSYMTAB = 0x0B;
static constexpr uint32_t LC_DYLD_INFO_ONLY = 0x80000022;
static constexpr uint32_t LC_BUILD_VERSION = 0x32;
static constexpr uint32_t LC_CODE_SIGNATURE = 0x1D;

static constexpr uint32_t VM_PROT_READ = 1;
static constexpr uint32_t VM_PROT_WRITE = 2;
static constexpr uint32_t VM_PROT_EXECUTE = 4;

static constexpr uint32_t PLATFORM_MACOS = zanna::codegen::macho::kPlatformMacOS;

/// @brief Convert a power-of-two alignment value to its log2 (Mach-O encoding).
/// @details Mach-O `section_64::align` stores alignment as the exponent (e.g.,
///          16-byte alignment is stored as 4). Returns 0 for alignment ≤ 1.
/// @param alignment Section alignment in bytes.
/// @return Base-two exponent used by `section_64::align`.
/// @pre A value greater than one is a power of two.
static uint32_t machoSectionAlignLog2(uint32_t alignment) {
    uint32_t pow2 = 0;
    uint32_t value = (alignment == 0) ? 1 : alignment;
    while (value > 1) {
        value >>= 1;
        ++pow2;
    }
    return pow2;
}

/// @brief Choose the Mach-O section name for an OutputSection.
/// @details Most outputs collapse to __text (executable) or __const (rodata);
///          loader/runtime-discovered sections preserve their original
///          "__SEG,__sect" name.
/// @param sec Output section to encode.
/// @return Mach-O section component without its segment prefix.
static std::string machoSectionNameForOutput(const OutputSection &sec) {
    if (!isObjCSection(sec.name) && !isMachOModInitTermSection(sec.name))
        return sec.executable ? "__text" : "__const";
    const auto comma = sec.name.find(',');
    return (comma != std::string::npos) ? sec.name.substr(comma + 1) : sec.name;
}

/// @brief Extracts the section component from a `segment,section` name.
/// @param name Full combined name or an already bare section name.
/// @return Substring after the first comma, or @p name unchanged.
static std::string machoSectionFieldName(const std::string &name) {
    const auto comma = name.find(',');
    return (comma != std::string::npos) ? name.substr(comma + 1) : name;
}

/// @brief Validates a name against Mach-O's fixed 16-byte segment/section field.
/// @param name Name to validate.
/// @param field Field description used in diagnostics.
/// @param err Diagnostic output stream.
/// @return `true` when @p name fits without truncation.
static bool validateMachOName(const std::string &name, const char *field, std::ostream &err) {
    if (name.size() <= 16)
        return true;
    err << "error: Mach-O " << field << " name '" << name << "' exceeds 16 bytes\n";
    return false;
}

/// @brief Narrows a count or offset to a 32-bit Mach-O field.
/// @param value Value to narrow.
/// @param what Field description used in diagnostics.
/// @param err Diagnostic output stream.
/// @param out Receives the narrowed value.
/// @return `true` when the value fits.
static bool checkedU32(uint64_t value, const char *what, std::ostream &err, uint32_t &out) {
    if (value > std::numeric_limits<uint32_t>::max()) {
        err << "error: Mach-O " << what << " exceeds 32-bit file format limit\n";
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

/// @brief Adds host file sizes with a Mach-O-specific overflow diagnostic.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param what Operation description.
/// @param err Diagnostic output stream.
/// @param out Receives the sum.
/// @return `true` when the sum is representable.
static bool checkedAddSize(
    size_t lhs, size_t rhs, const char *what, std::ostream &err, size_t &out) {
    if (lhs > std::numeric_limits<size_t>::max() - rhs) {
        err << "error: Mach-O " << what << " overflows addressable size\n";
        return false;
    }
    out = lhs + rhs;
    return true;
}

/// @brief Adds virtual-address values with a Mach-O-specific overflow diagnostic.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param what Operation description.
/// @param err Diagnostic output stream.
/// @param out Receives the sum.
/// @return `true` when the sum is representable.
static bool checkedAddU64(
    uint64_t lhs, uint64_t rhs, const char *what, std::ostream &err, uint64_t &out) {
    if (lhs > std::numeric_limits<uint64_t>::max() - rhs) {
        err << "error: Mach-O " << what << " overflows 64-bit address range\n";
        return false;
    }
    out = lhs + rhs;
    return true;
}

/// @brief Aligns a file size while converting alignment exceptions to diagnostics.
/// @param value Size to round upward.
/// @param alignment Required power-of-two alignment.
/// @param what Operation description.
/// @param err Diagnostic output stream.
/// @param out Receives the aligned size.
/// @return `true` when the alignment is valid and representable.
static bool checkedAlignUpSize(
    size_t value, size_t alignment, const char *what, std::ostream &err, size_t &out) {
    try {
        out = alignUp(value, alignment);
    } catch (const std::exception &ex) {
        err << "error: Mach-O " << what << " alignment failed: " << ex.what() << "\n";
        return false;
    }
    return true;
}

/// @brief Accounts for one load command in the Mach-O header totals.
/// @param ncmds Mutable load-command count.
/// @param sizeofcmds Mutable aggregate command byte size.
/// @param cmdSize Size of the new command.
/// @param what Command description used in diagnostics.
/// @param err Diagnostic output stream.
/// @return `true` when both 32-bit header fields remain representable.
static bool appendLoadCommand(
    uint32_t &ncmds, uint32_t &sizeofcmds, uint64_t cmdSize, const char *what, std::ostream &err) {
    uint32_t cmdSize32 = 0;
    if (!checkedU32(cmdSize, what, err, cmdSize32))
        return false;
    if (ncmds == std::numeric_limits<uint32_t>::max()) {
        err << "error: Mach-O load command count exceeds 32-bit file format limit\n";
        return false;
    }
    if (sizeofcmds > std::numeric_limits<uint32_t>::max() - cmdSize32) {
        err << "error: Mach-O load command table size overflows 32-bit file format limit\n";
        return false;
    }
    ++ncmds;
    sizeofcmds += cmdSize32;
    return true;
}

/// @brief Zero-pads a file buffer to an exact planned offset.
/// @param file Mutable output bytes.
/// @param targetOff Required next write offset.
/// @param what Region description used in diagnostics.
/// @param err Diagnostic output stream.
/// @return `false` when already-written data overlaps @p targetOff.
static bool padToExact(std::vector<uint8_t> &file,
                       size_t targetOff,
                       const char *what,
                       std::ostream &err) {
    if (file.size() > targetOff) {
        err << "error: Mach-O " << what << " overlaps previously written data\n";
        return false;
    }
    if (file.size() < targetOff)
        writePad(file, targetOff - file.size());
    return true;
}

/// @brief Map an ObjC metadata section name to its required Mach-O flag bits.
/// @details The ObjC runtime expects S_CSTRING_LITERALS for name pools and
///          S_ATTR_NO_DEAD_STRIP for class/category lists so the loader cannot
///          discard them even if no IL code references them directly.
/// @param machoSecName Bare Mach-O section name.
/// @return Required section type/attribute bits, or `S_REGULAR`.
static uint32_t objcSectionFlags(const std::string &machoSecName) {
    if (machoSecName == "__objc_classname" || machoSecName == "__objc_methname" ||
        machoSecName == "__objc_methtype")
        return S_CSTRING_LITERALS;
    if (machoSecName == "__objc_selrefs")
        return S_LITERAL_POINTERS | S_ATTR_NO_DEAD_STRIP;
    if (machoSecName == "__objc_classrefs" || machoSecName == "__objc_superrefs" ||
        machoSecName == "__objc_classlist" || machoSecName == "__objc_nlclslist" ||
        machoSecName == "__objc_catlist" || machoSecName == "__objc_nlcatlist" ||
        machoSecName == "__objc_methlist")
        return S_ATTR_NO_DEAD_STRIP;
    if (machoSecName == "__objc_protolist")
        return S_COALESCED | S_ATTR_NO_DEAD_STRIP;
    return S_REGULAR;
}

/// @brief Returns dyld's required section type for module lifetime pointers.
/// @param machoSecName Bare Mach-O section name.
/// @return Init/term pointer type, or `S_REGULAR` for other names.
static uint32_t moduleLifetimeSectionFlags(const std::string &machoSecName) {
    if (machoSecName == "__mod_init_func")
        return S_MOD_INIT_FUNC_POINTERS;
    if (machoSecName == "__mod_term_func")
        return S_MOD_TERM_FUNC_POINTERS;
    return S_REGULAR;
}

} // anonymous namespace

/// @copydoc writeMachOExe(const std::string &, const LinkLayout &, LinkArch, const
/// std::vector<DylibImport> &, const std::unordered_set<std::string> &, const
/// std::unordered_map<std::string, uint32_t> &, std::size_t, bool, std::ostream &)
bool writeMachOExe(const std::string &path,
                   const LinkLayout &layout,
                   LinkArch arch,
                   const std::vector<DylibImport> &dylibs,
                   const std::unordered_set<std::string> &dynSyms,
                   const std::unordered_map<std::string, uint32_t> &symOrdinals,
                   std::size_t stackSize,
                   bool emitLocalSymbols,
                   std::ostream &err) {
    const size_t pageSize = layout.pageSize;
    const bool isArm64 = (arch == LinkArch::AArch64);
    const uint32_t cpuType = isArm64 ? CPU_TYPE_ARM64 : CPU_TYPE_X86_64;
    const uint32_t cpuSubtype = isArm64 ? CPU_SUBTYPE_ARM64_ALL : CPU_SUBTYPE_X86_64_ALL;

    // =======================================================================
    // Phase 1: Classify sections and compute data sizes.
    // =======================================================================
    std::vector<size_t> textSections, dataSections;
    classifySections(layout, textSections, dataSections);
    std::vector<size_t> fileBackedDataSections;
    for (size_t idx : dataSections) {
        if (!layout.sections[idx].zeroFill)
            fileBackedDataSections.push_back(idx);
    }

    // Collect non-alloc debug sections.
    std::vector<size_t> debugSections;
    for (size_t i = 0; i < layout.sections.size(); ++i) {
        if (!layout.sections[i].alloc && !layout.sections[i].data.empty())
            debugSections.push_back(i);
    }

    // Compute text/data sizes accounting for VA gaps between sections.
    // Sections within a segment have page-aligned VAs; file layout must mirror this.
    size_t textDataSize = 0;
    size_t dataDataSize = 0;
    try {
        textDataSize = computeSegmentSpan(layout, textSections);
        dataDataSize = computeSegmentSpan(layout, fileBackedDataSections);
    } catch (const std::exception &ex) {
        err << "error: Mach-O segment span computation failed: " << ex.what() << "\n";
        return false;
    }

    // =======================================================================
    // Phase 2: Build __LINKEDIT content.
    // =======================================================================
    // Need LC_DYLD_INFO_ONLY if we have GOT entries, TLV descriptors, or rebase entries.
    // Derive TLV presence from the same tlvDescriptors flag that drives the
    // _tlv_bootstrap bind opcodes, so MH_HAS_TLV_DESCRIPTORS can never disagree
    // with the emitted binds (a mismatch makes dyld abort on the first
    // thread-local access).
    bool hasTLS = false;
    for (const auto &sec : layout.sections)
        if (sec.tlvDescriptors)
            hasTLS = true;
    const bool hasDynamic = !layout.gotEntries.empty() || hasTLS || !layout.rebaseEntries.empty() ||
                            !layout.bindEntries.empty();

    // Symbol `n_sect` ordinals follow section-header order: every __TEXT
    // section, then every __DATA section (zero-fill included).
    std::vector<size_t> symtabSectionOrder = textSections;
    symtabSectionOrder.insert(symtabSectionOrder.end(), dataSections.begin(), dataSections.end());
    std::vector<uint8_t> symtabData, strtabData;
    uint32_t nLocal = 0, nExtDef = 0, nUndef = 0;
    buildSymtab(symtabData,
                strtabData,
                layout,
                symtabSectionOrder,
                emitLocalSymbols,
                dynSyms,
                symOrdinals,
                nLocal,
                nExtDef,
                nUndef);

    // Pad string table to 8-byte alignment so that if it ever precedes
    // pointer-sized structures, offsets remain sane.
    while (strtabData.size() % 8 != 0)
        strtabData.push_back(0);

    // Bind and rebase opcodes (built with correct VAs after layout computation below).
    std::vector<uint8_t> bindData;
    std::vector<uint8_t> rebaseData;

    // =======================================================================
    // Phase 3: Compute file layout.
    //
    // Key: __TEXT segment starts at fileoff=0 and includes the header.
    //      The section data starts after the header, page-aligned.
    //      Section VAs come from SectionMerger (already correct).
    // =======================================================================
    const uint64_t pagezeroSize = 0x100000000ULL;
    const uint64_t textSegVmAddr = pagezeroSize; // __TEXT segment base

    // Count and size load commands.
    uint32_t ncmds = 0;
    uint32_t sizeofcmds = 0;

    if (!appendLoadCommand(ncmds, sizeofcmds, 72, "__PAGEZERO load command size", err))
        return false;

    uint32_t textSecCount = 0;
    if (!checkedU32(textSections.size(), "__TEXT section count", err, textSecCount))
        return false;
    if (!appendLoadCommand(ncmds,
                           sizeofcmds,
                           72ULL + static_cast<uint64_t>(textSecCount) * 80ULL,
                           "__TEXT load command size",
                           err))
        return false;

    uint32_t dataSecCount = 0;
    if (!checkedU32(dataSections.size(), "__DATA section count", err, dataSecCount))
        return false;
    if (!dataSections.empty()) {
        if (!appendLoadCommand(ncmds,
                               sizeofcmds,
                               72ULL + static_cast<uint64_t>(dataSecCount) * 80ULL,
                               "__DATA load command size",
                               err))
            return false;
    }

    uint32_t debugSecCount = 0;
    if (!checkedU32(debugSections.size(), "__DWARF section count", err, debugSecCount))
        return false;
    const bool hasDwarf = !debugSections.empty();
    if (hasDwarf) {
        if (!appendLoadCommand(ncmds,
                               sizeofcmds,
                               72ULL + static_cast<uint64_t>(debugSecCount) * 80ULL,
                               "__DWARF load command size",
                               err))
            return false;
    }

    if (!appendLoadCommand(ncmds, sizeofcmds, 72, "__LINKEDIT load command size", err))
        return false;

    const char *dylinkerPath = "/usr/lib/dyld";
    size_t dylinkerRawSize = 0;
    if (!checkedAddSize(
            12, std::strlen(dylinkerPath) + 1, "LC_LOAD_DYLINKER size", err, dylinkerRawSize))
        return false;
    size_t dylinkerCmdSize = 0;
    if (!checkedAlignUpSize(dylinkerRawSize, 8, "LC_LOAD_DYLINKER size", err, dylinkerCmdSize))
        return false;
    if (!appendLoadCommand(
            ncmds, sizeofcmds, dylinkerCmdSize, "LC_LOAD_DYLINKER command size", err))
        return false;

    if (!appendLoadCommand(ncmds, sizeofcmds, 24, "LC_MAIN load command size", err))
        return false;

    std::vector<size_t> dylibCmdSizes;
    for (const auto &dl : dylibs) {
        size_t dylibRawSize = 0;
        if (!checkedAddSize(24, dl.path.size() + 1, "LC_LOAD_DYLIB size", err, dylibRawSize))
            return false;
        size_t cmdSize = 0;
        if (!checkedAlignUpSize(dylibRawSize, 8, "LC_LOAD_DYLIB size", err, cmdSize))
            return false;
        dylibCmdSizes.push_back(cmdSize);
        if (!appendLoadCommand(ncmds, sizeofcmds, cmdSize, "LC_LOAD_DYLIB command size", err))
            return false;
    }

    if (!appendLoadCommand(ncmds, sizeofcmds, 24, "LC_SYMTAB load command size", err))
        return false;
    if (!appendLoadCommand(ncmds, sizeofcmds, 80, "LC_DYSYMTAB load command size", err))
        return false;

    if (hasDynamic) {
        if (!appendLoadCommand(ncmds, sizeofcmds, 48, "LC_DYLD_INFO_ONLY load command size", err))
            return false;
    }

    if (!appendLoadCommand(ncmds, sizeofcmds, 24, "LC_BUILD_VERSION load command size", err))
        return false;

    // LC_CODE_SIGNATURE. arm64 macOS requires an ad-hoc signature to exec; x86_64
    // images also need one to run under Rosetta 2 on Apple Silicon (the translator
    // rejects unsigned binaries). Sign both — the SuperBlob layout is identical.
    const bool needsCodeSign = true;
    if (needsCodeSign) {
        if (!appendLoadCommand(ncmds, sizeofcmds, 16, "LC_CODE_SIGNATURE load command size", err))
            return false;
    }

    const size_t headerSize = 32;
    size_t headerAndCmds = 0;
    if (!checkedAddSize(headerSize, sizeofcmds, "header and load-command size", err, headerAndCmds))
        return false;

    // Section data starts at the first page boundary after header+cmds.
    // In standard Mach-O, __TEXT segment fileoff=0 and includes the header.
    size_t textDataFileOff = 0;
    if (!checkedAlignUpSize(
            headerAndCmds, pageSize, "__TEXT data file offset", err, textDataFileOff))
        return false;
    size_t textDataFileSize = 0;
    if (!checkedAlignUpSize(textDataSize, pageSize, "__TEXT data file size", err, textDataFileSize))
        return false;

    // __TEXT segment: fileoff=0, filesize includes header+cmds+section data.
    const size_t textSegFileOff = 0;
    size_t textSegFileSize = 0;
    if (!checkedAddSize(
            textDataFileOff, textDataFileSize, "__TEXT segment file size", err, textSegFileSize))
        return false;

    // Use the first text section's VA from SectionMerger to derive __TEXT segment vmsize.
    // SectionMerger assigns text section VA = baseAddr + pageSize = 0x100004000.
    // __TEXT segment vmaddr = 0x100000000.
    // __TEXT segment vmsize must cover from vmaddr to end of text data.
    uint64_t textLastVA = textSegVmAddr;
    for (size_t idx : textSections) {
        uint64_t end = 0;
        if (!checkedAddU64(layout.sections[idx].virtualAddr,
                           outputSectionMemSize(layout.sections[idx]),
                           "__TEXT section address range",
                           err,
                           end))
            return false;
        if (end > textLastVA)
            textLastVA = end;
    }
    size_t textVmSpan = 0;
    if (textLastVA - textSegVmAddr > std::numeric_limits<size_t>::max()) {
        err << "error: Mach-O __TEXT segment VM span exceeds addressable size\n";
        return false;
    }
    if (!checkedAlignUpSize(static_cast<size_t>(textLastVA - textSegVmAddr),
                            pageSize,
                            "__TEXT segment VM size",
                            err,
                            textVmSpan))
        return false;
    const uint64_t textSegVmSize = textVmSpan;

    // __DATA segment.
    const size_t dataFileOff = textSegFileSize;
    size_t dataFileSize = 0;
    if (!fileBackedDataSections.empty() &&
        !checkedAlignUpSize(dataDataSize, pageSize, "__DATA file size", err, dataFileSize))
        return false;
    uint64_t dataSegVmAddr = 0;
    uint64_t dataSegVmSize = 0;
    if (!dataSections.empty()) {
        dataSegVmAddr = layout.sections[dataSections[0]].virtualAddr;
        uint64_t dataLastVA = dataSegVmAddr;
        for (size_t idx : dataSections) {
            uint64_t end = 0;
            if (!checkedAddU64(layout.sections[idx].virtualAddr,
                               outputSectionMemSize(layout.sections[idx]),
                               "__DATA section address range",
                               err,
                               end))
                return false;
            if (end > dataLastVA)
                dataLastVA = end;
        }
        if (dataLastVA - dataSegVmAddr > std::numeric_limits<size_t>::max()) {
            err << "error: Mach-O __DATA segment VM span exceeds addressable size\n";
            return false;
        }
        size_t dataVmSpan = 0;
        if (!checkedAlignUpSize(static_cast<size_t>(dataLastVA - dataSegVmAddr),
                                pageSize,
                                "__DATA segment VM size",
                                err,
                                dataVmSpan))
            return false;
        dataSegVmSize = dataVmSpan;
    }

    // Build rebase and bind opcodes now that we have correct VAs.
    // __DATA is segment index 2 (0=__PAGEZERO, 1=__TEXT, 2=__DATA).
    {
        // Rebase opcodes: ASLR fixups for internal pointers in writable sections.
        buildRebaseOpcodes(rebaseData, layout, dataSegVmAddr, 2);
        while (rebaseData.size() % 8 != 0)
            rebaseData.push_back(0);

        // Bind opcodes: dynamic symbol resolution + TLV descriptor thunks.
        if (!buildBindOpcodes(
                bindData, layout.gotEntries, layout, dataSegVmAddr, 2, symOrdinals, err))
            return false;
        while (bindData.size() % 8 != 0)
            bindData.push_back(0);
    }
    const size_t linkeditDataSize =
        symtabData.size() + strtabData.size() + rebaseData.size() + bindData.size();

    // __DWARF segment (non-alloc debug sections, placed before __LINKEDIT).
    size_t dwarfFileOff = 0;
    if (!checkedAddSize(dataFileOff, dataFileSize, "__DWARF file offset", err, dwarfFileOff))
        return false;
    size_t dwarfTotalSize = 0;

    struct DwarfSecInfo {
        size_t layoutIdx;
        size_t offset; // Offset within __DWARF segment.
    };

    std::vector<DwarfSecInfo> dwarfSecInfos;
    if (hasDwarf) {
        for (size_t idx : debugSections) {
            const auto &sec = layout.sections[idx];
            size_t padded = 0;
            if (!checkedAlignUpSize(
                    dwarfTotalSize, sec.alignment, "__DWARF section offset", err, padded))
                return false;
            dwarfSecInfos.push_back({idx, padded});
            if (!checkedAddSize(
                    padded, sec.data.size(), "__DWARF section data size", err, dwarfTotalSize))
                return false;
        }
    }
    size_t dwarfFileSize = 0;
    if (hasDwarf &&
        !checkedAlignUpSize(dwarfTotalSize, pageSize, "__DWARF file size", err, dwarfFileSize))
        return false;

    // __LINKEDIT segment.
    size_t linkeditFileOff = 0;
    if (!checkedAddSize(
            dwarfFileOff, dwarfFileSize, "__LINKEDIT file offset", err, linkeditFileOff))
        return false;

    // Code signature lives at end of __LINKEDIT (arm64 only).
    // Pre-compute offset and size so __LINKEDIT filesize includes it.
    const std::string codeSignIdent = std::filesystem::path(path).stem().string();
    size_t codeSignOff = 0;
    size_t codeSignSize = 0;
    if (needsCodeSign) {
        size_t codeSignRawOff = 0;
        if (!checkedAddSize(linkeditFileOff,
                            linkeditDataSize,
                            "code-signature file offset",
                            err,
                            codeSignRawOff))
            return false;
        if (!checkedAlignUpSize(codeSignRawOff, 16, "code-signature file offset", err, codeSignOff))
            return false;
        codeSignSize = estimateCodeSignatureSize(codeSignOff, codeSignIdent, pageSize);
    }

    size_t linkeditTotalSize = linkeditDataSize;
    if (needsCodeSign) {
        if (codeSignOff < linkeditFileOff) {
            err << "error: Mach-O code-signature offset precedes __LINKEDIT\n";
            return false;
        }
        if (!checkedAddSize(codeSignOff - linkeditFileOff,
                            codeSignSize,
                            "__LINKEDIT total size",
                            err,
                            linkeditTotalSize))
            return false;
    }
    // Unlike loadable text/data segments, __LINKEDIT should describe exactly
    // the link-edit records present in the file. Padding the final segment to
    // a page boundary leaves bytes after LC_CODE_SIGNATURE, which Apple tools
    // reject with "link edit information does not fill the __LINKEDIT segment".
    const size_t linkeditFileSize = linkeditTotalSize;
    // __DWARF segment has no VM mapping (vmaddr=0, vmsize=0), so it doesn't
    // shift __LINKEDIT's vmaddr. However, it does occupy file space.
    uint64_t linkeditVmAddr = 0;
    if (!checkedAddU64(textSegVmAddr, textSegVmSize, "__LINKEDIT VM address", err, linkeditVmAddr))
        return false;
    if (!dataSections.empty()) {
        // __LINKEDIT must follow every mapped segment. Use the actual __DATA end
        // (dataSegVmAddr + dataSegVmSize) rather than assuming __DATA is VM-
        // contiguous with __TEXT; a gap between them would otherwise place
        // __LINKEDIT overlapping __DATA, which dyld rejects.
        uint64_t dataSegEnd = 0;
        if (!checkedAddU64(dataSegVmAddr, dataSegVmSize, "__DATA segment end", err, dataSegEnd))
            return false;
        linkeditVmAddr = std::max(linkeditVmAddr, dataSegEnd);
    }
    size_t linkeditVmSizeSize = 0;
    if (!checkedAlignUpSize(
            linkeditTotalSize, pageSize, "__LINKEDIT VM size", err, linkeditVmSizeSize))
        return false;
    const uint64_t linkeditVmSize = linkeditVmSizeSize;

    // Offsets within __LINKEDIT follow Apple's required order:
    //   rebase → bind → symtab → strtab → code signature
    // Apple's strip and codesign validate that LC_DYLD_INFO_ONLY content
    // precedes the symbol table; placing it after causes "dyld_info out of
    // place" errors that prevent both strip and codesign from processing
    // the binary.
    size_t bindOffSize = 0;
    size_t symtabOffSize = 0;
    size_t strtabOffSize = 0;
    if (!checkedAddSize(
            linkeditFileOff, rebaseData.size(), "bind opcode file offset", err, bindOffSize) ||
        !checkedAddSize(
            bindOffSize, bindData.size(), "symbol-table file offset", err, symtabOffSize) ||
        !checkedAddSize(
            symtabOffSize, symtabData.size(), "string-table file offset", err, strtabOffSize))
        return false;
    uint32_t rebaseOff = 0;
    uint32_t bindOff = 0;
    uint32_t symtabOff = 0;
    uint32_t strtabOff = 0;
    if (!checkedU32(linkeditFileOff, "__LINKEDIT file offset", err, rebaseOff) ||
        !checkedU32(bindOffSize, "bind opcode file offset", err, bindOff) ||
        !checkedU32(symtabOffSize, "symbol-table file offset", err, symtabOff) ||
        !checkedU32(strtabOffSize, "string-table file offset", err, strtabOff))
        return false;

    // Entry point: LC_MAIN entryoff = file offset of the resolved entry symbol.
    // The linker stores custom entry symbols in layout.entryAddr; fall back to
    // main/_main for older callers that did not populate it.
    uint64_t mainEntryOff = 0;
    {
        uint64_t entryVA = layout.entryAddr;
        if (entryVA == 0) {
            auto it = layout.globalSyms.find("main");
            if (it == layout.globalSyms.end())
                it = layout.globalSyms.find("_main");
            if (it != layout.globalSyms.end())
                entryVA = it->second.resolvedAddr;
        }
        if (entryVA != 0 && !textSections.empty()) {
            bool foundEntry = false;
            for (size_t idx : textSections) {
                const auto &sec = layout.sections[idx];
                const size_t secMemSize = outputSectionMemSize(sec);
                if (secMemSize > std::numeric_limits<uint64_t>::max() - sec.virtualAddr) {
                    err << "error: Mach-O text section '" << sec.name
                        << "' address range overflows\n";
                    return false;
                }
                const uint64_t secEnd = sec.virtualAddr + secMemSize;
                if (entryVA < sec.virtualAddr || entryVA >= secEnd)
                    continue;
                // __TEXT is mapped at fileoff 0, and each section's bytes are
                // written at (VA - textSegVmAddr). The entry's file offset is
                // therefore entryVA - textSegVmAddr directly — deriving it this way
                // avoids assuming the first text section sits exactly at
                // textSegVmAddr + textDataFileOff.
                if (entryVA < textSegVmAddr) {
                    err << "error: Mach-O entry address precedes __TEXT segment base\n";
                    return false;
                }
                mainEntryOff = entryVA - textSegVmAddr;
                foundEntry = true;
                break;
            }
            if (!foundEntry) {
                err << "error: Mach-O entry address does not resolve to a __TEXT section\n";
                return false;
            }
        }
    }

    // =======================================================================
    // Phase 4: Write Mach-O file.
    // =======================================================================
    std::vector<uint8_t> file;
    file.reserve(linkeditFileOff + linkeditFileSize);

    // --- Mach-O header (32 bytes) ---
    writeLE32(file, MH_MAGIC_64);
    writeLE32(file, cpuType);
    writeLE32(file, cpuSubtype);
    writeLE32(file, MH_EXECUTE);
    writeLE32(file, ncmds);
    writeLE32(file, sizeofcmds);
    // Two-level namespace: each bind opcode specifies the dylib ordinal for its
    // symbol. ObjC class/metaclass symbols use flat lookup (ordinal -2) since
    // their defining framework can't be determined by prefix alone.
    uint32_t mhFlags = MH_PIE | MH_DYLDLINK | MH_TWOLEVEL;
    if (!hasDynamic)
        mhFlags |= MH_NOUNDEFS;
    if (hasTLS)
        mhFlags |= MH_HAS_TLV_DESCRIPTORS;
    writeLE32(file, mhFlags);
    writeLE32(file, 0); // reserved

    // --- __PAGEZERO ---
    writeLE32(file, LC_SEGMENT_64);
    writeLE32(file, 72);
    writeStr(file, "__PAGEZERO", 16);
    writeLE64(file, 0);
    writeLE64(file, pagezeroSize);
    writeLE64(file, 0);
    writeLE64(file, 0);
    writeLE32(file, 0);
    writeLE32(file, 0);
    writeLE32(file, 0);
    writeLE32(file, 0);

    // --- __TEXT ---
    writeLE32(file, LC_SEGMENT_64);
    writeLE32(file, 72 + textSecCount * 80);
    writeStr(file, "__TEXT", 16);
    writeLE64(file, textSegVmAddr);
    writeLE64(file, textSegVmSize);
    writeLE64(file, textSegFileOff);
    writeLE64(file, textSegFileSize);
    writeLE32(file, VM_PROT_READ | VM_PROT_EXECUTE);
    writeLE32(file, VM_PROT_READ | VM_PROT_EXECUTE);
    writeLE32(file, textSecCount);
    writeLE32(file, 0);

    // Section headers — file offsets mirror VA offsets from segment base.
    // Since __TEXT fileoff=0 and vmaddr=textSegVmAddr, each section's
    // file offset is simply: sec.virtualAddr - textSegVmAddr.
    for (size_t idx : textSections) {
        const auto &sec = layout.sections[idx];

        const std::string machoSecName = machoSectionNameForOutput(sec);
        if (!validateMachOName(machoSecName, "section", err))
            return false;

        if (sec.virtualAddr < textSegVmAddr) {
            err << "error: Mach-O section '" << sec.name << "' precedes __TEXT segment base\n";
            return false;
        }
        uint32_t secFileOff = 0;
        if (!checkedU32(sec.virtualAddr - textSegVmAddr, "section file offset", err, secFileOff))
            return false;
        writeStr(file, machoSecName.c_str(), 16);
        writeStr(file, "__TEXT", 16);
        writeLE64(file, sec.virtualAddr); // addr = SectionMerger VA
        writeLE64(file, outputSectionMemSize(sec));
        writeLE32(file, secFileOff);
        writeLE32(file, machoSectionAlignLog2(sec.alignment));
        writeLE32(file, 0);
        writeLE32(file, 0); // reloff, nreloc
        uint32_t flags =
            sec.executable ? (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS) : S_REGULAR;
        if (isObjCSection(sec.name))
            flags = objcSectionFlags(machoSecName);
        writeLE32(file, flags);
        writeLE32(file, 0);
        writeLE32(file, 0);
        writeLE32(file, 0); // reserved
    }

    // --- __DATA ---
    if (!dataSections.empty()) {
        writeLE32(file, LC_SEGMENT_64);
        writeLE32(file, 72 + dataSecCount * 80);
        writeStr(file, "__DATA", 16);
        writeLE64(file, dataSegVmAddr);
        writeLE64(file, dataSegVmSize);
        writeLE64(file, dataFileOff);
        writeLE64(file, dataFileSize);
        writeLE32(file, VM_PROT_READ | VM_PROT_WRITE);
        writeLE32(file, VM_PROT_READ | VM_PROT_WRITE);
        writeLE32(file, dataSecCount);
        writeLE32(file, 0);

        for (size_t idx : dataSections) {
            const auto &sec = layout.sections[idx];
            if (sec.virtualAddr < dataSegVmAddr) {
                err << "error: Mach-O section '" << sec.name << "' precedes __DATA segment base\n";
                return false;
            }
            size_t secFileOffSize = 0;
            if (sec.virtualAddr - dataSegVmAddr > std::numeric_limits<size_t>::max() ||
                !checkedAddSize(dataFileOff,
                                static_cast<size_t>(sec.virtualAddr - dataSegVmAddr),
                                "section file offset",
                                err,
                                secFileOffSize))
                return false;
            uint32_t secFileOff = 0;
            if (!checkedU32(secFileOffSize, "section file offset", err, secFileOff))
                return false;

            // Choose section name and type flags.
            std::string machoSecName = "__data";
            uint32_t secFlags = S_REGULAR;
            bool isZerofill = false;
            if (isMachOModInitTermSection(sec.name)) {
                // dyld discovers global constructors/destructors by both the
                // section name and S_MOD_* section type.
                machoSecName = machoSectionNameForOutput(sec);
                secFlags = moduleLifetimeSectionFlags(machoSecName);
            } else if (isObjCSection(sec.name)) {
                // ObjC metadata: preserve original section name (e.g., __objc_classlist).
                machoSecName = machoSectionNameForOutput(sec);
                secFlags = objcSectionFlags(machoSecName);
            } else if (sec.tls) {
                if (sec.name == ".tdata") {
                    machoSecName = "__thread_vars";
                    secFlags = S_THREAD_LOCAL_VARIABLES;
                } else if (sec.zeroFill) {
                    machoSecName = "__thread_bss";
                    secFlags = S_THREAD_LOCAL_ZEROFILL;
                    isZerofill = true;
                } else {
                    // TLS template data (S_THREAD_LOCAL_REGULAR).
                    machoSecName = "__thread_data";
                    secFlags = S_THREAD_LOCAL_REGULAR;
                }
            } else if (sec.zeroFill) {
                machoSecName = "__bss";
                secFlags = S_ZEROFILL;
                isZerofill = true;
            }

            if (!validateMachOName(machoSecName, "section", err))
                return false;
            writeStr(file, machoSecName.c_str(), 16);
            writeStr(file, "__DATA", 16);
            writeLE64(file, sec.virtualAddr); // addr = SectionMerger VA
            writeLE64(file, outputSectionMemSize(sec));
            writeLE32(file, isZerofill ? 0 : secFileOff);
            writeLE32(file, machoSectionAlignLog2(sec.alignment));
            writeLE32(file, 0);
            writeLE32(file, 0); // reloff, nreloc
            writeLE32(file, secFlags);
            writeLE32(file, 0);
            writeLE32(file, 0);
            writeLE32(file, 0); // reserved
        }
    }

    // --- __DWARF (non-alloc debug sections) ---
    if (hasDwarf) {
        writeLE32(file, LC_SEGMENT_64);
        writeLE32(file, 72 + debugSecCount * 80);
        writeStr(file, "__DWARF", 16);
        writeLE64(file, 0); // vmaddr: no VM mapping
        writeLE64(file, 0); // vmsize: no VM mapping
        writeLE64(file, dwarfFileOff);
        writeLE64(file, dwarfFileSize);
        writeLE32(file, 0); // maxprot: no permissions
        writeLE32(file, 0); // initprot: no permissions
        writeLE32(file, debugSecCount);
        writeLE32(file, 0);

        for (const auto &dsi : dwarfSecInfos) {
            const auto &sec = layout.sections[dsi.layoutIdx];
            // Mach-O debug section names: __debug_line, __debug_info, etc.
            // Strip the leading dot from ELF-style names (.debug_line → __debug_line).
            std::string machoSecName = machoSectionFieldName(sec.name);
            if (!machoSecName.empty() && machoSecName[0] == '.')
                machoSecName[0] = '_';

            if (!validateMachOName(machoSecName, "section", err))
                return false;
            writeStr(file, machoSecName.c_str(), 16);
            writeStr(file, "__DWARF", 16);
            writeLE64(file, 0); // addr: no VM mapping
            writeLE64(file, sec.data.size());
            size_t dwarfSecFileOff = 0;
            if (!checkedAddSize(
                    dwarfFileOff, dsi.offset, "__DWARF section file offset", err, dwarfSecFileOff))
                return false;
            uint32_t dwarfSecFileOff32 = 0;
            if (!checkedU32(dwarfSecFileOff, "__DWARF section file offset", err, dwarfSecFileOff32))
                return false;
            writeLE32(file, dwarfSecFileOff32);
            writeLE32(file, 0); // align
            writeLE32(file, 0);
            writeLE32(file, 0);           // reloff, nreloc
            writeLE32(file, 0x02000000u); // S_ATTR_DEBUG
            writeLE32(file, 0);
            writeLE32(file, 0);
            writeLE32(file, 0); // reserved
        }
    }

    // --- __LINKEDIT ---
    writeLE32(file, LC_SEGMENT_64);
    writeLE32(file, 72);
    writeStr(file, "__LINKEDIT", 16);
    writeLE64(file, linkeditVmAddr);
    writeLE64(file, linkeditVmSize);
    writeLE64(file, linkeditFileOff);
    writeLE64(file, linkeditFileSize);
    writeLE32(file, VM_PROT_READ);
    writeLE32(file, VM_PROT_READ);
    writeLE32(file, 0);
    writeLE32(file, 0);

    // --- LC_LOAD_DYLINKER ---
    writeLE32(file, LC_LOAD_DYLINKER);
    uint32_t dylinkerCmdSize32 = 0;
    if (!checkedU32(dylinkerCmdSize, "LC_LOAD_DYLINKER command size", err, dylinkerCmdSize32))
        return false;
    writeLE32(file, dylinkerCmdSize32);
    writeLE32(file, 12);
    {
        size_t nameLen = std::strlen(dylinkerPath) + 1;
        file.insert(file.end(), dylinkerPath, dylinkerPath + nameLen);
        size_t pad = dylinkerCmdSize - 12 - nameLen;
        if (pad > 0)
            writePad(file, pad);
    }

    // --- LC_MAIN ---
    writeLE32(file, LC_MAIN);
    writeLE32(file, 24);
    writeLE64(file, mainEntryOff);
    writeLE64(file, stackSize);

    // --- LC_LOAD_DYLIB ---
    for (size_t di = 0; di < dylibs.size(); ++di) {
        const auto &dl = dylibs[di];
        uint32_t cmdSize = 0;
        if (!checkedU32(dylibCmdSizes[di], "LC_LOAD_DYLIB command size", err, cmdSize))
            return false;
        writeLE32(file, LC_LOAD_DYLIB);
        writeLE32(file, cmdSize);
        writeLE32(file, 24);       // name offset
        writeLE32(file, 2);        // timestamp
        writeLE32(file, 0x010000); // current_version
        writeLE32(file, 0x010000); // compat_version
        size_t nameLen = dl.path.size() + 1;
        file.insert(file.end(), dl.path.begin(), dl.path.end());
        file.push_back(0);
        size_t pad = cmdSize - 24 - nameLen;
        if (pad > 0)
            writePad(file, pad);
    }

    // --- LC_SYMTAB ---
    uint32_t symCount = 0;
    uint64_t symCount64 = 0;
    uint64_t definedCount64 = 0;
    if (!checkedAddU64(nLocal, nExtDef, "symbol count", err, definedCount64) ||
        !checkedAddU64(definedCount64, nUndef, "symbol count", err, symCount64) ||
        !checkedU32(symCount64, "symbol count", err, symCount))
        return false;
    const uint32_t firstExtDef = nLocal;
    const uint32_t firstUndef = static_cast<uint32_t>(definedCount64);
    writeLE32(file, LC_SYMTAB);
    writeLE32(file, 24);
    writeLE32(file, symtabOff);
    writeLE32(file, symCount);
    writeLE32(file, strtabOff);
    uint32_t strtabSize32 = 0;
    if (!checkedU32(strtabData.size(), "string-table size", err, strtabSize32))
        return false;
    writeLE32(file, strtabSize32);

    // --- LC_DYSYMTAB ---
    writeLE32(file, LC_DYSYMTAB);
    writeLE32(file, 80);
    writeLE32(file, 0);
    writeLE32(file, nLocal); // ilocalsym, nlocalsym
    writeLE32(file, firstExtDef);
    writeLE32(file, nExtDef); // iextdefsym, nextdefsym
    writeLE32(file, firstUndef);
    writeLE32(file, nUndef); // iundefsym, nundefsym
    for (int i = 0; i < 12; ++i)
        writeLE32(file, 0); // remaining fields (toc, modtab, extref, indirect, extrel, locrel)

    // --- LC_DYLD_INFO_ONLY ---
    if (hasDynamic) {
        writeLE32(file, LC_DYLD_INFO_ONLY);
        writeLE32(file, 48);
        writeLE32(file, rebaseData.empty() ? 0 : rebaseOff);
        uint32_t rebaseSize32 = 0;
        uint32_t bindSize32 = 0;
        if (!checkedU32(rebaseData.size(), "rebase opcode size", err, rebaseSize32) ||
            !checkedU32(bindData.size(), "bind opcode size", err, bindSize32))
            return false;
        writeLE32(file, rebaseSize32); // rebase
        writeLE32(file, bindOff);
        writeLE32(file, bindSize32); // bind
        writeLE32(file, 0);
        writeLE32(file, 0); // weak_bind
        writeLE32(file, 0);
        writeLE32(file, 0); // lazy_bind
        writeLE32(file, 0);
        writeLE32(file, 0); // export
    }

    // --- LC_BUILD_VERSION ---
    writeLE32(file, LC_BUILD_VERSION);
    writeLE32(file, 24);
    writeLE32(file, PLATFORM_MACOS);
    writeLE32(file, zanna::codegen::macho::minimumMacOSVersion());
    writeLE32(file, zanna::codegen::macho::macOSSDKVersion());
    writeLE32(file, 0);

    // --- LC_CODE_SIGNATURE ---
    if (needsCodeSign) {
        uint32_t codeSignOff32 = 0;
        uint32_t codeSignSize32 = 0;
        if (!checkedU32(codeSignOff, "code-signature file offset", err, codeSignOff32) ||
            !checkedU32(codeSignSize, "code-signature size", err, codeSignSize32))
            return false;
        writeLE32(file, LC_CODE_SIGNATURE);
        writeLE32(file, 16);
        writeLE32(file, codeSignOff32);
        writeLE32(file, codeSignSize32);
    }

    // =======================================================================
    // Phase 5: Write segment data.
    // =======================================================================

    // Write text sections at their VA-relative file positions.
    // Since __TEXT fileoff=0, each section's file position = sec.VA - textSegVmAddr.
    for (size_t idx : textSections) {
        const auto &sec = layout.sections[idx];
        if (sec.virtualAddr < textSegVmAddr) {
            err << "error: Mach-O section '" << sec.name << "' precedes __TEXT segment base\n";
            return false;
        }
        if (sec.virtualAddr - textSegVmAddr > std::numeric_limits<size_t>::max()) {
            err << "error: Mach-O section '" << sec.name
                << "' file offset exceeds addressable size\n";
            return false;
        }
        size_t targetOff = static_cast<size_t>(sec.virtualAddr - textSegVmAddr);
        if (!padToExact(file, targetOff, sec.name.c_str(), err))
            return false;
        file.insert(file.end(), sec.data.begin(), sec.data.end());
    }

    // Write data sections at their VA-relative file positions within __DATA.
    if (!dataSections.empty()) {
        for (size_t idx : dataSections) {
            const auto &sec = layout.sections[idx];
            if (sec.zeroFill)
                continue;
            if (sec.virtualAddr < dataSegVmAddr) {
                err << "error: Mach-O section '" << sec.name << "' precedes __DATA segment base\n";
                return false;
            }
            if (sec.virtualAddr - dataSegVmAddr > std::numeric_limits<size_t>::max()) {
                err << "error: Mach-O section '" << sec.name
                    << "' file offset exceeds addressable size\n";
                return false;
            }
            size_t targetOff = 0;
            if (!checkedAddSize(dataFileOff,
                                static_cast<size_t>(sec.virtualAddr - dataSegVmAddr),
                                "section file offset",
                                err,
                                targetOff))
                return false;
            if (!padToExact(file, targetOff, sec.name.c_str(), err))
                return false;
            file.insert(file.end(), sec.data.begin(), sec.data.end());
        }
    }

    // Write __DWARF segment data.
    if (hasDwarf) {
        for (const auto &dsi : dwarfSecInfos) {
            const auto &sec = layout.sections[dsi.layoutIdx];
            size_t targetOff = 0;
            if (!checkedAddSize(
                    dwarfFileOff, dsi.offset, "__DWARF section file offset", err, targetOff))
                return false;
            if (!padToExact(file, targetOff, sec.name.c_str(), err))
                return false;
            file.insert(file.end(), sec.data.begin(), sec.data.end());
        }
    }

    // Pad to __LINKEDIT start.
    if (!padToExact(file, linkeditFileOff, "__LINKEDIT", err))
        return false;

    // __LINKEDIT content: rebase → bind → symtab → strtab.
    file.insert(file.end(), rebaseData.begin(), rebaseData.end());
    file.insert(file.end(), bindData.begin(), bindData.end());
    file.insert(file.end(), symtabData.begin(), symtabData.end());
    file.insert(file.end(), strtabData.begin(), strtabData.end());

    // Append native code signature (arm64) or just pad to final page.
    if (needsCodeSign) {
        // Pad to code signature offset (16-byte aligned within __LINKEDIT).
        if (!padToExact(file, codeSignOff, "code signature", err))
            return false;

        // Build linker-signed ad-hoc code signature with CS_LINKER_SIGNED flag.
        auto sig = buildCodeSignature(
            file, codeSignOff, codeSignIdent, textSegFileOff, textSegFileSize, pageSize);
        file.insert(file.end(), sig.begin(), sig.end());
    }

    // Pad only to the declared end of __LINKEDIT. The final segment's file
    // size is intentionally not page-aligned.
    size_t declaredFileSize = 0;
    if (!checkedAddSize(
            linkeditFileOff, linkeditFileSize, "declared file size", err, declaredFileSize))
        return false;
    if (!padToExact(file, declaredFileSize, "declared file size", err))
        return false;

    // =======================================================================
    // Phase 6: Write to disk via atomic replace to avoid stale executable vnode
    // state on macOS after rebuilding a signed binary at the same path.
    // =======================================================================
    return writeBinaryFileAtomically(path, file, true, err);
}

} // namespace zanna::codegen::linker
