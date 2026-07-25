//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/LinkTypes.hpp
// Purpose: Core types used throughout the native linker: OutputSection,
//          InputChunk, LinkLayout, GlobalSymEntry, and platform enums.
// Key invariants:
//   - OutputSection owns concatenated section data + reloc list
//   - Virtual addresses assigned during layout phase
//   - Page alignment differs per platform: macOS arm64=16KB, others=4KB
// Ownership/Lifetime:
//   - All types are value types owned by their caller
// Links: codegen/common/linker/ObjFileReader.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file LinkTypes.hpp
 * @brief Defines the platform-neutral data model shared by native linker stages.
 *
 * These types carry input provenance, merged section contents and permissions,
 * symbol resolution state, loader fixups, and finalized image layout between
 * readers, optimization passes, relocation application, and executable writers.
 */

#pragma once

#include "common/PlatformCapabilities.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Target operating-system ABI and executable format.
enum class LinkPlatform : uint8_t {
    Linux,
    macOS,
    Windows,
};

/// @brief Target instruction-set architecture.
enum class LinkArch : uint8_t {
    X86_64,
    AArch64,
};

/// @brief Detects the current build host's native link platform.
/// @return macOS, Windows, or Linux according to generated platform capabilities.
constexpr LinkPlatform detectLinkPlatform() {
    if constexpr (zanna::platform::kHostMacOS)
        return LinkPlatform::macOS;
    if constexpr (zanna::platform::kHostWindows)
        return LinkPlatform::Windows;
    return LinkPlatform::Linux;
}

/// Detect the default native link architecture for this build.
/// @details The generated platform capability header records which native
///          linker backends were compiled. When exactly one backend is present,
///          that backend is the only valid default. Multi-backend builds keep
///          x86_64 as the historical conservative default; callers that need a
///          specific target architecture should set NativeLinkerOptions::arch.
/// @return The sole compiled native backend, or x86-64 for multi-backend builds.
constexpr LinkArch detectLinkArch() {
    if constexpr (zanna::platform::kNativeLinkAArch64 && !zanna::platform::kNativeLinkX86_64)
        return LinkArch::AArch64;
    return LinkArch::X86_64;
}

/// @brief Returns the native linker's conventional fixed image base.
/// @param platform Target executable platform.
/// @return `0x100000000` for macOS, `0x140000000` for Windows, or
///         `0x400000` for Linux.
constexpr uint64_t defaultImageBaseForPlatform(LinkPlatform platform) {
    switch (platform) {
        case LinkPlatform::macOS:
            return 0x100000000ULL;
        case LinkPlatform::Windows:
            return 0x140000000ULL;
        case LinkPlatform::Linux:
        default:
            return 0x400000ULL;
    }
}

/// @brief Records provenance and placement for one merged input-section chunk.
struct InputChunk {
    size_t inputObjIndex = 0; ///< Index into the linker's object file list.
    size_t inputSecIndex = 0; ///< Index into that ObjFile's sections.
    size_t outputOffset = 0;  ///< Byte offset within the output section.
    size_t size = 0;          ///< Size in bytes.
    bool synthetic = false;   ///< True when the bytes were created by the linker.
};

/// @brief Hashable identity of an input section within an object collection.
struct InputSectionKey {
    size_t objIndex = 0;
    size_t secIndex = 0;

    /// @brief Compares both the object and section indices.
    /// @param other Key to compare.
    /// @return `true` when both keys identify the same input section.
    bool operator==(const InputSectionKey &other) const noexcept {
        return objIndex == other.objIndex && secIndex == other.secIndex;
    }
};

/// @brief Hash adapter for `InputSectionKey` associative containers.
struct InputSectionKeyHash {
    /// @brief Combines object and section indices.
    /// @param key Input-section identity.
    /// @return Host-sized hash value.
    size_t operator()(const InputSectionKey &key) const noexcept {
        size_t h = std::hash<size_t>{}(key.objIndex);
        h ^= std::hash<size_t>{}(key.secIndex) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

/// @brief Owns one merged output section and its source-chunk provenance.
struct OutputSection {
    std::string name;
    std::vector<uint8_t> data;      ///< Concatenated section bytes.
    size_t memSize = 0;             ///< Logical in-memory size, including zero-fill bytes.
    std::vector<InputChunk> chunks; ///< Provenance info for each chunk.
    uint64_t virtualAddr = 0;       ///< Virtual address after layout.
    uint32_t alignment = 1;         ///< Required alignment.
    bool executable = false;
    bool writable = false;
    bool tls = false;
    bool zeroFill = false;    ///< Occupies memory but has no file backing.
    bool alloc = true;        ///< Section is loadable (false for debug sections).
    bool dataSegment = false; ///< Emit in data segment even when final protections are read-only.
    /// True when this section contains Mach-O TLV descriptors (24-byte
    /// {thunk, key, offset} records). Set by SectionMerger when merging
    /// `__thread_vars` inputs. MachOBindRebase uses this to gate the
    /// `_tlv_bootstrap` thunk-binding loop instead of matching by section
    /// name, which avoids confusing TLV descriptors with TLS template data
    /// (the two had to share the `.tdata` name historically).
    bool tlvDescriptors = false;
};

/// @brief Returns the complete in-memory footprint of an output section.
/// @param sec Section whose materialized and logical sizes are compared.
/// @return The greater of file-backed byte count and declared memory size.
inline size_t outputSectionMemSize(const OutputSection &sec) {
    return std::max(sec.data.size(), sec.memSize);
}

/// @brief Canonical merge class assigned to an input section.
enum class SectionClass : uint8_t {
    Text,      ///< Executable code.
    Rodata,    ///< Read-only data.
    Data,      ///< Read-write data.
    Bss,       ///< Uninitialized data (zero-filled).
    TlsData,   ///< Thread-local initialized data.
    TlsBss,    ///< Thread-local uninitialized data.
    ObjC,      ///< ObjC metadata — preserved with original section name.
    Preserved, ///< Platform metadata preserved with its original section name.
    Other,     ///< Non-allocatable, debug, etc.
};

/// Check whether a Mach-O section name is ObjC metadata that must be preserved.
/// The ObjC runtime locates classes, selectors, protocols, etc. by section name.
/// @param name Mach-O `segment,section` name.
/// @return `true` for recognized Objective-C metadata namespaces.
inline bool isObjCSection(const std::string &name) {
    return name.rfind("__DATA,__objc_", 0) == 0 || name.rfind("__DATA_CONST,__objc_", 0) == 0 ||
           name.rfind("__TEXT,__objc_", 0) == 0 || name.rfind("__OBJC,", 0) == 0;
}

/// Check whether a Windows PE/COFF metadata section name must be preserved.
/// The PE loader and unwinder expect these sections to remain separately
/// addressable so the exe writer can publish the matching data directories.
/// @param name COFF section name.
/// @return `true` for `.pdata*` or `.xdata*`.
inline bool isWindowsMetadataSection(const std::string &name) {
    return name.rfind(".pdata", 0) == 0 || name.rfind(".xdata", 0) == 0;
}

/// @brief Tests for ELF unwind, exception, or note metadata requiring name preservation.
/// @param name ELF section name.
/// @return `true` for `.eh_frame*`, `.gcc_except_table*`, or `.note*`.
inline bool isElfMetadataSection(const std::string &name) {
    return name == ".eh_frame" || name.rfind(".eh_frame.", 0) == 0 || name == ".gcc_except_table" ||
           name.rfind(".gcc_except_table.", 0) == 0 || name.rfind(".note.", 0) == 0 ||
           name == ".note";
}

/// @brief Tests for Mach-O constant/authenticated data segments.
/// @param name Mach-O `segment,section` name.
/// @return `true` for `__DATA_CONST` or `__AUTH_CONST` contributions.
inline bool isMachOConstDataSection(const std::string &name) {
    return name.rfind("__DATA_CONST,", 0) == 0 || name.rfind("__AUTH_CONST,", 0) == 0;
}

/// Check whether a Mach-O section contains dyld-discovered module initializer
/// or terminator pointers. These names and their section-type flags must
/// survive section merging so dyld invokes C/C++ global lifetime functions.
/// @param name Mach-O `segment,section` name.
/// @return `true` for exact `__mod_init_func` or `__mod_term_func` sections.
inline bool isMachOModInitTermSection(const std::string &name) {
    const auto comma = name.find(',');
    if (comma == std::string::npos)
        return false;
    const size_t sectionOffset = comma + 1;
    return name.compare(sectionOffset, std::string::npos, "__mod_init_func") == 0 ||
           name.compare(sectionOffset, std::string::npos, "__mod_term_func") == 0;
}

/// @brief Tests whether any supported platform requires a section's original name.
/// @param name Input section name in its object-format spelling.
/// @return `true` when section merging must retain the name and identity.
inline bool isPreservedNamedSection(const std::string &name) {
    return isObjCSection(name) || isWindowsMetadataSection(name) || isElfMetadataSection(name) ||
           isMachOConstDataSection(name) || isMachOModInitTermSection(name);
}

/// Symbols synthesized by the Windows native linker rather than imported from
/// a DLL or provided by a runtime archive.
/// @param name COFF symbol name.
/// @return `true` for MSVC local stdio option-storage symbols.
inline bool isWindowsStdioOptionsStorageSymbol(const std::string &name) {
    return name.find("__local_stdio_printf_options") != std::string::npos ||
           name.find("__local_stdio_scanf_options") != std::string::npos;
}

/// @brief Tests for an MSVC thread-safe static-initialization guard symbol.
/// @param name COFF decorated symbol name.
/// @return `true` when the name begins with the `?$TSS` guard prefix.
inline bool isMsvcThreadSafeStaticGuardSymbol(const std::string &name) {
    return name.rfind("?$TSS", 0) == 0;
}

/// @brief Tests for a Windows ABI helper synthesized by the native linker.
/// @param name COFF symbol name.
/// @return `true` when the symbol belongs to the linker's built-in helper surface.
inline bool isWindowsLinkerHelperSymbol(const std::string &name) {
    return name == "_fltused" || name == "__ImageBase" || name == "__security_cookie" ||
           name == "__security_check_cookie" || name == "__security_init_cookie" ||
           name == "__GSHandlerCheck" || name == "__GSHandlerCheck_EH4" ||
           name == "_RTC_InitBase" || name == "_RTC_Shutdown" || name == "_RTC_CheckStackVars" ||
           name == "_RTC_UninitUse" || name == "__report_rangecheckfailure" || name == "__chkstk" ||
           name == "_tls_index" || name == "__security_cookie_complement" ||
           name == "__guard_dispatch_icall_fptr" || name == "_is_c_termination_complete" ||
           name == "__vcrt_initialize" || name == "__vcrt_thread_attach" ||
           name == "__vcrt_thread_detach" || name == "__vcrt_uninitialize" ||
           name == "__vcrt_uninitialize_critical" || name == "__isa_available" ||
           name == "__acrt_initialize" || name == "__acrt_thread_attach" ||
           name == "__acrt_thread_detach" || name == "__acrt_uninitialize" ||
           name == "__acrt_uninitialize_critical" || name == "__isa_available_init" ||
           name == "__scrt_exe_initialize_mta" || name == "__CxxFrameHandler4" ||
           name == "??_7type_info@@6B@" || name == "??2@YAPEAX_K@Z" ||
           name == "??2@YAPEAX_KAEBUnothrow_t@std@@@Z" || name == "??3@YAXPEAX@Z" ||
           name == "??3@YAXPEAX_K@Z" || name == "IID_ID3D11Texture2D" ||
           isWindowsStdioOptionsStorageSymbol(name) || name == "vm_trap" ||
           name == "rt_audio_shutdown";
}

/// @brief Classifies an input section for merging and output placement.
/// @details TLS and platform-preserved metadata take precedence over generic
///          permissions. Writable zero-fill becomes BSS; otherwise executable
///          flags and narrowly recognized text names determine code placement.
/// @param name Object-format section name.
/// @param executable Whether the input marks the section executable.
/// @param writable Whether the input marks the section writable.
/// @param tls Whether the input contains thread-local storage.
/// @param zeroFill Whether the section occupies memory without file bytes.
/// @return Canonical merge class.
inline SectionClass classifySection(
    const std::string &name, bool executable, bool writable, bool tls, bool zeroFill) {
    if (tls) {
        if (zeroFill || name.find("bss") != std::string::npos ||
            name.find("zerofill") != std::string::npos)
            return SectionClass::TlsBss;
        return SectionClass::TlsData;
    }
    if (isObjCSection(name))
        return SectionClass::ObjC;
    // Platform metadata must be preserved with its original name because
    // runtimes/loaders locate it by name or dedicated data-directory ranges.
    if (isWindowsMetadataSection(name) || isElfMetadataSection(name) ||
        isMachOConstDataSection(name) || isMachOModInitTermSection(name))
        return SectionClass::Preserved;
    if (executable)
        return SectionClass::Text;
    if (writable) {
        if (zeroFill || name.find("bss") != std::string::npos ||
            name.find("UNINITIALIZED") != std::string::npos)
            return SectionClass::Bss;
        return SectionClass::Data;
    }
    // Read-only: only known text-section spellings are code when producer flags
    // failed to mark them executable. Do not classify arbitrary names containing
    // ".text" as executable code.
    if (name == ".text" || name.rfind(".text.", 0) == 0 || name.rfind(".text$", 0) == 0 ||
        name == "__TEXT,__text")
        return SectionClass::Text;
    return SectionClass::Rodata;
}

/// @brief Tracks one global name from resolution through final address assignment.
struct GlobalSymEntry {
    std::string name;

    enum Binding : uint8_t {
        Undefined,
        Global,
        Weak,
        Dynamic, ///< Provided by a shared library.
    } binding = Undefined;

    size_t objIndex = 0;       ///< Which ObjFile defined this symbol.
    uint32_t secIndex = 0;     ///< Section within that ObjFile.
    size_t offset = 0;         ///< Offset within the section.
    uint64_t resolvedAddr = 0; ///< Final virtual address after layout.
    bool resolvedAddrValid =
        false;             ///< True when resolvedAddr is an intentional address, including zero.
    bool absolute = false; ///< Symbol resolves to offset/resolvedAddr directly.
    bool common = false;   ///< Tentative/common symbol awaiting materialization.
    size_t commonSize = 0; ///< Largest requested tentative definition size.
    size_t commonAlignment = 1; ///< Maximum requested tentative definition alignment.
};

/// @brief Records a loader-populated global-offset-table slot.
struct GotEntry {
    std::string symbolName; ///< External symbol name (e.g., "printf").
    uint64_t gotAddr = 0;   ///< Virtual address of this GOT slot.
};

/// @brief Records an absolute output pointer requiring an ASLR rebase.
struct RebaseEntry {
    size_t sectionIndex = 0; ///< Index into LinkLayout::sections.
    size_t offset = 0;       ///< Byte offset within the output section.
};

/// @brief Records a non-GOT output pointer requiring loader symbol binding.
/// @details Used for references such as Objective-C class pointers to external symbols.
struct BindEntry {
    std::string symbolName;  ///< External symbol name (e.g., "OBJC_CLASS_$_NSColor").
    size_t sectionIndex = 0; ///< Index into LinkLayout::sections.
    size_t offset = 0;       ///< Byte offset within the output section.
};

/// @brief Owns the complete finalized memory and loader-fixup layout.
struct LinkLayout {
    std::vector<OutputSection> sections;                        ///< Merged output sections.
    std::unordered_map<std::string, GlobalSymEntry> globalSyms; ///< All resolved symbols.
    uint64_t entryAddr = 0;                                     ///< Entry point virtual address.
    /// Image base virtual address. SectionMerger seeds this from
    /// `defaultImageBaseForPlatform(platform)`. RelocApplier, MachOBindRebase,
    /// and the executable writers must all read it from here so that any
    /// future per-link override of the image base only has to be plumbed
    /// through one field instead of dozens of `defaultImageBaseForPlatform`
    /// call sites.
    uint64_t imageBase = 0;
    size_t pageSize = 0x1000;               ///< Page size (platform-dependent).
    std::vector<GotEntry> gotEntries;       ///< GOT entries for dynamic linking.
    std::vector<RebaseEntry> rebaseEntries; ///< Locations needing ASLR pointer rebase.
    std::vector<BindEntry> bindEntries;     ///< Non-GOT data pointers needing dyld bind.
};

} // namespace zanna::codegen::linker
