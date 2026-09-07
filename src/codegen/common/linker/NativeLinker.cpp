//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/NativeLinker.cpp
// Purpose: Top-level native linker implementation.
// Key invariants:
//   - Pipeline: parse .o → parse archives → resolve symbols → generate stubs →
//     merge sections → branch trampolines → apply relocations → write executable
//   - For macOS: generates GOT entries and stub trampolines for dynamic symbols,
//     uses non-lazy binding (dyld fills GOT at load time)
//   - Falls back gracefully with clear error messages
// Links: codegen/common/linker/NativeLinker.hpp
// Cross-platform touchpoints: shared link pipeline orchestration, per-platform
//                             import planners, executable writer selection.
//
//===----------------------------------------------------------------------===//

/**
 * @file NativeLinker.cpp
 * @brief Implements cross-platform native link orchestration and synthetic ABI helpers.
 */

#include "codegen/common/linker/NativeLinker.hpp"

#include "codegen/common/linker/ArchiveReader.hpp"
#include "codegen/common/linker/BranchTrampoline.hpp"
#include "codegen/common/linker/DeadStripPass.hpp"
#include "codegen/common/linker/DynStubGen.hpp"
#include "codegen/common/linker/ElfExeWriter.hpp"
#include "codegen/common/linker/ICF.hpp"
#include "codegen/common/linker/MachOExeWriter.hpp"
#include "codegen/common/linker/NameMangling.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"
#include "codegen/common/linker/PeExeWriter.hpp"
#include "codegen/common/linker/PlatformImportPlanner.hpp"
#include "codegen/common/linker/RelocApplier.hpp"
#include "codegen/common/linker/RelocConstants.hpp"
#include "codegen/common/linker/SectionMerger.hpp"
#include "codegen/common/linker/StringDedup.hpp"
#include "codegen/common/linker/SymbolResolver.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::linker {

namespace {

/// @brief One process-wide immutable archive-cache record and invalidation key.
struct ArchiveCacheEntry {
    std::filesystem::file_time_type modified{}; ///< Last-write time used for invalidation.
    std::uintmax_t size = 0;                    ///< File size used for invalidation.
    std::shared_ptr<const Archive> archive{};   ///< Shared immutable parsed archive snapshot.
};

/// @brief Read an archive through the process-wide parsed-archive cache.
/// @details Cache entries are invalidated whenever file size or last-write time
///          changes. Returned archives share an immutable parsed snapshot;
///          symbol resolution records per-link extraction state separately.
///          Sharing avoids both archive reparsing and copies of the raw archive
///          byte buffer during multi-target builds.
/// @param path Archive path to load and validate.
/// @param archive Receives shared ownership of the current parsed snapshot.
/// @param err Diagnostic stream used for filesystem and parse failures.
/// @return True on a cache hit or successful parse; false on input failure.
bool readArchiveCached(const std::string &path,
                       std::shared_ptr<const Archive> &archive,
                       std::ostream &err) {
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, ArchiveCacheEntry> cache;

    std::error_code ec;
    const auto *utf8Path = reinterpret_cast<const char8_t *>(path.data());
    const std::filesystem::path diskPath(std::u8string_view(utf8Path, path.size()));
    const auto modified = std::filesystem::last_write_time(diskPath, ec);
    if (ec) {
        auto parsed = std::make_shared<Archive>();
        if (!readArchive(path, *parsed, err))
            return false;
        archive = std::move(parsed);
        return true;
    }
    const std::uintmax_t size = std::filesystem::file_size(diskPath, ec);
    if (ec) {
        auto parsed = std::make_shared<Archive>();
        if (!readArchive(path, *parsed, err))
            return false;
        archive = std::move(parsed);
        return true;
    }

    std::lock_guard<std::mutex> lock(cacheMutex);
    if (const auto it = cache.find(path);
        it != cache.end() && it->second.modified == modified && it->second.size == size) {
        archive = it->second.archive;
        return true;
    }

    auto parsed = std::make_shared<Archive>();
    if (!readArchive(path, *parsed, err))
        return false;
    cache[path] = ArchiveCacheEntry{modified, size, parsed};
    archive = std::move(parsed);
    return true;
}

/// @brief Emits per-stage elapsed times when `ZANNA_LINKER_STATS` is enabled.
class LinkTiming {
  public:
    /// @brief Creates a timer whose diagnostics are written to @p err.
    /// @param err Timing output stream retained by reference.
    explicit LinkTiming(std::ostream &err)
        /// @brief Determines whether linker timing output is enabled.
        /// @return `true` when `ZANNA_LINKER_STATS` is set to a nonzero value.
        : err_(err), enabled_([]() {
              const char *value = std::getenv("ZANNA_LINKER_STATS");
              return value != nullptr && std::string_view(value) != "0";
          }()),
          last_(std::chrono::steady_clock::now()) {}

    /// @brief Reports elapsed time since construction or the previous mark.
    /// @param stage Human-readable pipeline stage name.
    void mark(const char *stage) {
        if (!enabled_)
            return;
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<double, std::milli> elapsed = now - last_;
        err_ << "[link-time] " << stage << ' ' << elapsed.count() << "ms\n";
        last_ = now;
    }

  private:
    std::ostream &err_;
    bool enabled_ = false;
    std::chrono::steady_clock::time_point last_;
};

/// @brief Installs one defined synthetic symbol unless a static definition already wins.
/// @param sym Synthetic object symbol to consider.
/// @param objIdx Owning object's index in the active object list.
/// @param globalSyms Mutable global resolution table.
/// @return `true` when a global entry was installed or replaced.
bool installSyntheticGlobal(const ObjSymbol &sym,
                            size_t objIdx,
                            std::unordered_map<std::string, GlobalSymEntry> &globalSyms) {
    if (sym.binding == ObjSymbol::Local || sym.binding == ObjSymbol::Undefined || sym.name.empty())
        return false;

    auto existing = globalSyms.find(sym.name);
    if (existing != globalSyms.end() && existing->second.binding != GlobalSymEntry::Undefined &&
        existing->second.binding != GlobalSymEntry::Dynamic)
        return false;

    GlobalSymEntry e;
    e.name = sym.name;
    e.binding = GlobalSymEntry::Global;
    e.objIndex = objIdx;
    e.secIndex = sym.sectionIndex;
    e.offset = sym.offset;
    e.resolvedAddr = sym.absolute ? static_cast<uint64_t>(sym.offset) : 0;
    e.resolvedAddrValid = sym.absolute;
    e.absolute = sym.absolute;
    globalSyms[sym.name] = std::move(e);
    return true;
}

/// @brief Promote every Global/Weak symbol in a synthetic ObjFile into the
///        global symbol table so subsequent passes can resolve references to it.
/// @details Used after we synthesize helper objects (Windows CRT shims, PE
///          import stubs, etc.) so they participate in symbol resolution as if
///          they had been read from a real archive.
/// @param obj Synthetic object whose definitions are registered.
/// @param objIdx Index of @p obj in the active object list.
/// @param globalSyms Mutable global resolution table.
void registerSyntheticSymbols(const ObjFile &obj,
                              size_t objIdx,
                              std::unordered_map<std::string, GlobalSymEntry> &globalSyms) {
    for (size_t i = 1; i < obj.symbols.size(); ++i) {
        const auto &sym = obj.symbols[i];
        installSyntheticGlobal(sym, objIdx, globalSyms);
    }
}

/// @brief Drop @p name from the dynamic-symbol set when a static definition resolves it.
/// @param name Exact symbol name to remove.
/// @param dynamicSyms Mutable unresolved dynamic-symbol set.
[[maybe_unused]] void removeDynamicSymbol(const char *name,
                                          std::unordered_set<std::string> &dynamicSyms) {
    dynamicSyms.erase(name);
}

/// @brief ASCII case-insensitive string compare (used for Windows DLL name dedup).
/// @param lhs First ASCII string.
/// @param rhs Second ASCII string.
/// @return `true` when equal after per-byte ASCII case folding.
bool equalsIgnoreAsciiCase(const std::string &lhs, const std::string &rhs) {
    /// @brief Compares two ASCII bytes after case folding.
    /// @param a Left byte.
    /// @param b Right byte.
    /// @return `true` when the folded bytes are equal.
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](unsigned char a, unsigned char b) {
               return std::tolower(a) == std::tolower(b);
           });
}

/// @brief Ensure the PE import list contains a (DLL, function) pair, dedupe-aware.
/// @details Walks @p imports for an existing entry matching @p dllName (case-
///          insensitive — Windows lookup is case-insensitive); appends to its
///          functions list if absent, otherwise inserts a fresh DllImport.
/// @param imports Mutable import descriptor list.
/// @param dllName Provider DLL name.
/// @param functionName Imported function name.
void ensurePeImportFunction(std::vector<DllImport> &imports,
                            const std::string &dllName,
                            const std::string &functionName) {
    for (DllImport &imp : imports) {
        if (!equalsIgnoreAsciiCase(imp.dllName, dllName))
            continue;

        if (std::find(imp.functions.begin(), imp.functions.end(), functionName) ==
            imp.functions.end()) {
            imp.functions.push_back(functionName);
            std::sort(imp.functions.begin(), imp.functions.end());
        }
        return;
    }

    imports.push_back({dllName, {functionName}, {}});
}

/// @brief Return the image base selected by layout or the platform default.
/// @details SectionMerger seeds LinkLayout::imageBase before relocation and
///          writing. Hand-built test layouts may leave it zero, so callers use
///          this helper to retain the historical default behavior.
/// @param layout Link layout that may contain an explicit base.
/// @param platform Target used for fallback selection.
/// @return Explicit nonzero layout base or the conventional platform default.
uint64_t imageBaseForLayout(const LinkLayout &layout, LinkPlatform platform) {
    return layout.imageBase != 0 ? layout.imageBase : defaultImageBaseForPlatform(platform);
}

/// @brief Narrow a synthetic ObjFile symbol index to COFF's 32-bit relocation field.
/// @details Synthetic Windows helper generation appends symbols and immediately
///          records relocation references to them. This helper keeps that path
///          from silently wrapping if helper generation ever exceeds the object
///          format's symbol-index range.
/// @param index Host-sized synthetic symbol index.
/// @param context Record description used in an exception message.
/// @return Index narrowed to `uint32_t`.
/// @throws std::runtime_error If @p index exceeds the relocation field.
uint32_t checkedSyntheticSymbolIndex(size_t index, const char *context) {
    if (index > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::string("native linker: synthetic helper symbol index "
                                             "overflow while adding ") +
                                 context);
    }
    return static_cast<uint32_t>(index);
}

/// @brief Checked size addition for synthetic helper section growth.
/// @details Native helper objects are assembled directly into byte vectors.
///          Their offsets later become relocation sites, so size arithmetic must
///          fail before a vector resize can wrap the computed byte count.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param context Operation description used in an exception message.
/// @return Checked sum.
/// @throws std::runtime_error If the sum exceeds `size_t`.
size_t checkedSyntheticSizeAdd(size_t lhs, size_t rhs, const char *context) {
    if (lhs > std::numeric_limits<size_t>::max() - rhs) {
        throw std::runtime_error(std::string("native linker: synthetic helper size overflow in ") +
                                 context);
    }
    return lhs + rhs;
}

/// @brief Build a synthetic ObjFile that refers to @p symbolName as undefined.
/// @details This is the "GC root" used by DeadStripPass: every section reachable
///          from this synthetic module's reference to `main` (or a custom entry)
///          is kept; everything else is stripped. The returned object inherits
///          @p userObj's format/arch/endianness so it slots cleanly into the
///          object list.
/// @param userObj Object supplying target format and machine metadata.
/// @param symbolName Entry symbol to reference as undefined.
/// @return Synthetic root object containing a null section and undefined symbol.
ObjFile makeUndefinedRootObject(const ObjFile &userObj, const std::string &symbolName) {
    ObjFile root;
    root.name = "<entry-root>";
    root.synthetic = true;
    root.format = userObj.format;
    root.is64bit = userObj.is64bit;
    root.isLittleEndian = userObj.isLittleEndian;
    root.machine = userObj.machine;
    root.sections.push_back(ObjSection{});
    root.symbols.push_back(ObjSymbol{});

    ObjSymbol sym;
    sym.name = symbolName;
    sym.binding = ObjSymbol::Undefined;
    root.symbols.push_back(std::move(sym));
    return root;
}

/// @brief Synthesize a definition of `__dso_handle` (a pointer-sized data
///        symbol).
/// @details Embedding C++ frontend code (fe_zia) drags in libc++'s atexit
///          machinery, which references `__dso_handle` purely as a unique
///          per-image identity cookie passed to `__cxa_atexit` — its contents
///          are never read. A normal crt provides it, but Zanna's crt-less
///          native binaries do not, so we define an 8-byte zero-filled symbol
///          ourselves. A single logical C name is exported; the shared Mach-O
///          fallback resolves its underscore-prefixed object spelling.
/// @param userObj Object supplying target format and machine metadata.
/// @return Synthetic object defining one eight-byte `__dso_handle`.
ObjFile makeDsoHandleObject(const ObjFile &userObj) {
    ObjFile obj;
    obj.name = "<dso-handle>";
    obj.synthetic = true;
    obj.format = userObj.format;
    obj.is64bit = userObj.is64bit;
    obj.isLittleEndian = userObj.isLittleEndian;
    obj.machine = userObj.machine;
    obj.sections.push_back(ObjSection{});

    ObjSection dataSec;
    dataSec.name = ".data";
    dataSec.alloc = true;
    dataSec.writable = true;
    dataSec.zeroFill = true;
    dataSec.alignment = 8;
    dataSec.memSize = 8;

    // A single definition: the resolver's macOS underscore fallback treats
    // `__dso_handle` / `___dso_handle` as the same symbol, so defining both
    // here would be a self-collision.
    obj.symbols.push_back(ObjSymbol{});
    ObjSymbol sym;
    sym.name = "__dso_handle";
    sym.binding = ObjSymbol::Global;
    sym.sectionIndex = 1;
    sym.offset = 0;
    sym.size = 8;
    obj.symbols.push_back(std::move(sym));

    obj.sections.push_back(std::move(dataSec));
    return obj;
}

/// @brief Heuristically detect a debug-flavored Windows CRT in the archive list.
/// @details Inspects each path for the conventional `\Debug\`, `/debug/`, or
///          `*d.lib` suffixes used by MSVC's debug runtime. The result selects
///          ucrtbased.dll / vcruntime140d.dll for IAT generation.
/// @param archivePaths Runtime archive paths to inspect.
/// @return `true` when any path follows a recognized Debug CRT convention.
bool usesDebugWindowsRuntime(const std::vector<std::string> &archivePaths) {
    for (const auto &path : archivePaths) {
        std::string lower = path;
        /// @brief Converts one ASCII byte to lowercase.
        /// @param c Byte to convert.
        /// @return Lowercase character value.
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lower.find("\\debug\\") != std::string::npos ||
            lower.find("/debug/") != std::string::npos ||
            lower.rfind("msvcrtd.lib") != std::string::npos ||
            lower.rfind("ucrtd.lib") != std::string::npos ||
            lower.rfind("vcruntimed.lib") != std::string::npos)
            return true;
    }
    return false;
}

/// @brief Return a human-readable name for a LinkPlatform (for diagnostics).
/// @param platform Platform enum value.
/// @return Process-lifetime display string.
const char *platformName(LinkPlatform platform) {
    switch (platform) {
        case LinkPlatform::Linux:
            return "Linux";
        case LinkPlatform::macOS:
            return "macOS";
        case LinkPlatform::Windows:
            return "Windows";
    }
    return "unknown";
}

/// @brief Return a human-readable name for a LinkArch (for diagnostics).
/// @param arch Architecture enum value.
/// @return Process-lifetime display string.
const char *archName(LinkArch arch) {
    switch (arch) {
        case LinkArch::X86_64:
            return "x86_64";
        case LinkArch::AArch64:
            return "AArch64";
    }
    return "unknown";
}

/// @brief Tests whether an address lies in a nonempty executable output section.
/// @param layout Final merged layout.
/// @param addr Virtual address to test.
/// @return `true` when a valid half-open executable section range contains it.
bool addressInExecutableSection(const LinkLayout &layout, uint64_t addr) {
    for (const auto &sec : layout.sections) {
        const size_t memSize = outputSectionMemSize(sec);
        if (!sec.alloc || !sec.executable || memSize == 0)
            continue;
        if (memSize > std::numeric_limits<uint64_t>::max() - sec.virtualAddr)
            continue;
        const uint64_t end = sec.virtualAddr + memSize;
        if (addr >= sec.virtualAddr && addr < end)
            return true;
    }
    return false;
}

/// @brief Map a target LinkPlatform to its native object-file format.
/// @param platform Target platform.
/// @return ELF, Mach-O, or COFF; invalid enum values map to Unknown.
ObjFileFormat expectedFormat(LinkPlatform platform) {
    switch (platform) {
        case LinkPlatform::Linux:
            return ObjFileFormat::ELF;
        case LinkPlatform::macOS:
            return ObjFileFormat::MachO;
        case LinkPlatform::Windows:
            return ObjFileFormat::COFF;
    }
    return ObjFileFormat::Unknown;
}

/// @brief Compute the machine field expected in input objects for the target.
/// @details Windows uses the IMAGE_FILE_MACHINE_* values (0x8664/0xAA64); ELF
///          and Mach-O use the EM_X86_64 (62) / EM_AARCH64 (183) numbering and
///          MachOReader normalises Mach-O cputype to those same values.
/// @param platform Target object format.
/// @param arch Target architecture.
/// @return Expected object header machine value.
uint16_t expectedMachine(LinkPlatform platform, LinkArch arch) {
    if (platform == LinkPlatform::Windows)
        return arch == LinkArch::AArch64 ? 0xAA64 : 0x8664;
    return arch == LinkArch::AArch64 ? 183 : 62;
}

/// @brief Return a human-readable name for an ObjFileFormat (for diagnostics).
/// @param format Object format enum value.
/// @return Process-lifetime display string.
const char *formatName(ObjFileFormat format) {
    switch (format) {
        case ObjFileFormat::ELF:
            return "ELF";
        case ObjFileFormat::MachO:
            return "Mach-O";
        case ObjFileFormat::COFF:
            return "COFF";
        case ObjFileFormat::Unknown:
            return "unknown";
    }
    return "unknown";
}

/// @brief Reject any input ObjFile whose format/machine does not match the target.
/// @details Synthetic objects are exempt from the check because they were minted
///          with the target's format already. User/archive object names are not
///          trusted for this decision.
/// @param objects Parsed input and extracted objects.
/// @param platform Requested output platform.
/// @param arch Requested output architecture.
/// @param err Diagnostic output stream.
/// @return `true` when every non-synthetic object matches the target.
bool validateInputObjects(const std::vector<ObjFile> &objects,
                          LinkPlatform platform,
                          LinkArch arch,
                          std::ostream &err) {
    const ObjFileFormat wantFormat = expectedFormat(platform);
    const uint16_t wantMachine = expectedMachine(platform, arch);
    for (const auto &obj : objects) {
        if (obj.synthetic)
            continue;
        if (obj.format != wantFormat) {
            err << "error: " << obj.name << ": " << formatName(obj.format)
                << " object cannot be linked into " << platformName(platform) << ' '
                << archName(arch) << " output\n";
            return false;
        }
        const bool allowUnknownWindowsCoff = platform == LinkPlatform::Windows &&
                                             obj.format == ObjFileFormat::COFF && obj.machine == 0;
        if (obj.machine != wantMachine && !allowUnknownWindowsCoff) {
            err << "error: " << obj.name << ": machine 0x" << std::hex << obj.machine << std::dec
                << " does not match target " << archName(arch) << "\n";
            return false;
        }
    }
    return true;
}

/// @brief Append an AArch64 instruction (32-bit little-endian) to a byte stream.
/// @param data Destination text-section bytes.
/// @param insn Instruction word.
void appendArm64Insn(std::vector<uint8_t> &data, uint32_t insn) {
    data.push_back(static_cast<uint8_t>(insn & 0xFF));
    data.push_back(static_cast<uint8_t>((insn >> 8) & 0xFF));
    data.push_back(static_cast<uint8_t>((insn >> 16) & 0xFF));
    data.push_back(static_cast<uint8_t>((insn >> 24) & 0xFF));
}

/// @brief Initialize a Windows COFF "helpers" ObjFile scaffold with the given
///        synthetic name and machine code; shared by the x64 and arm64 builders.
/// @details Both Windows-helpers generators want the same ObjFile flags
///          (synthetic, 64-bit COFF, little-endian) with a placeholder section[0]
///          / symbol[0] slot already pushed. Centralising the boilerplate here
///          means a future third architecture only changes one place.
/// @param name Synthetic object display name.
/// @param machine COFF machine value.
/// @return Initialized synthetic object with null section and symbol entries.
static ObjFile makeWindowsHelpersObj(const char *name, uint16_t machine) {
    ObjFile obj;
    obj.name = name;
    obj.synthetic = true;
    obj.format = ObjFileFormat::COFF;
    obj.is64bit = true;
    obj.isLittleEndian = true;
    obj.machine = machine;
    obj.sections.push_back(ObjSection{});
    obj.symbols.push_back(ObjSymbol{});
    return obj;
}

/// @brief Construct the canonical executable `.text` ObjSection used by the
///        Windows-helpers builders (16-byte aligned, allocated, executable).
/// @return Empty configured text section.
static ObjSection makeWindowsHelpersTextSec() {
    ObjSection s;
    s.name = ".text";
    s.executable = true;
    s.alloc = true;
    s.alignment = 16;
    return s;
}

/// @brief Construct the canonical writable `.data` ObjSection used by the
///        Windows-helpers builders (8-byte aligned, allocated, writable).
/// @return Empty configured data section.
static ObjSection makeWindowsHelpersDataSec() {
    ObjSection s;
    s.name = ".data";
    s.writable = true;
    s.alloc = true;
    s.alignment = 8;
    return s;
}

/// @brief Synthesise a COFF object that supplies common Windows runtime stubs.
/// @details Windows binaries normally pick up many tiny support routines (the
///          security cookie, TLS index, vm_trap, integer-divide helpers, etc.)
///          from the vcruntime/crt static libraries. Rather than depending on
///          those archives we build a minimal "x64 helpers" COFF on the fly so
///          undefined references resolve cleanly.
/// @param dynamicSyms     Currently undefined symbols — guides which helpers to emit.
/// @param haveVmTrapDefault When true, Zanna's runtime already provides vm_trap.
/// @param needTlsIndex    When true, emit `_tls_index` and `_tls_used` placeholders.
/// @param debugWindowsRuntime When false, suppress Debug-CRT-only report and validation hooks
///        retained by Debug-built archives so release-runtime package links stay redistributable.
/// @return Synthetic AMD64 COFF object containing only required definitions.
/// @throws std::runtime_error If generated size or symbol indices overflow.
ObjFile generateWindowsX64Helpers(const std::unordered_set<std::string> &dynamicSyms,
                                  bool haveVmTrapDefault,
                                  bool needTlsIndex,
                                  bool debugWindowsRuntime) {
    ObjFile obj = makeWindowsHelpersObj("<win64-helpers>", 0x8664);
    ObjSection textSec = makeWindowsHelpersTextSec();
    ObjSection dataSec = makeWindowsHelpersDataSec();

    /// @brief Tests both direct and `__imp_` spellings for an unresolved helper.
    /// @param name Canonical helper symbol name.
    /// @return `true` when either spelling remains unresolved.
    auto needsHelper = [&](const std::string &name) {
        return dynamicSyms.count(name) || dynamicSyms.count("__imp_" + name);
    };

    /// @brief Appends a complete x64 helper body and publishes its global symbol.
    /// @param name Published helper name.
    /// @param bytes Encoded helper body.
    /// @return Synthetic symbol-table index.
    auto addRetFn = [&](const std::string &name, std::initializer_list<uint8_t> bytes) {
        const size_t off = textSec.data.size();
        textSec.data.insert(textSec.data.end(), bytes.begin(), bytes.end());
        ObjSymbol sym;
        sym.name = name;
        sym.binding = ObjSymbol::Global;
        sym.sectionIndex = 1;
        sym.offset = off;
        obj.symbols.push_back(std::move(sym));
        return checkedSyntheticSymbolIndex(obj.symbols.size() - 1, name.c_str());
    };

    /// @brief Appends aligned helper data and publishes its global symbol.
    /// @param name Published data symbol name.
    /// @param bytes Initial data bytes.
    /// @param align Required byte alignment.
    /// @return Synthetic symbol-table index.
    auto addData = [&](const std::string &name, const std::vector<uint8_t> &bytes, uint32_t align) {
        while ((dataSec.data.size() % align) != 0)
            dataSec.data.push_back(0);
        const size_t off = dataSec.data.size();
        dataSec.data.insert(dataSec.data.end(), bytes.begin(), bytes.end());
        ObjSymbol sym;
        sym.name = name;
        sym.binding = ObjSymbol::Global;
        sym.sectionIndex = 2;
        sym.offset = off;
        obj.symbols.push_back(std::move(sym));
        return checkedSyntheticSymbolIndex(obj.symbols.size() - 1, name.c_str());
    };

    /// @brief Adds an eight-byte data pointer and an absolute target relocation.
    /// @param name Published pointer symbol name.
    /// @param align Required byte alignment.
    /// @param targetSymIdx Relocation target symbol index.
    /// @return Synthetic pointer symbol index.
    auto addAbs64DataRef = [&](const std::string &name, uint32_t align, uint32_t targetSymIdx) {
        while ((dataSec.data.size() % align) != 0)
            dataSec.data.push_back(0);
        const size_t off = dataSec.data.size();
        dataSec.data.resize(checkedSyntheticSizeAdd(off, 8, name.c_str()), 0);
        ObjSymbol sym;
        sym.name = name;
        sym.binding = ObjSymbol::Global;
        sym.sectionIndex = 2;
        sym.offset = off;
        obj.symbols.push_back(std::move(sym));

        ObjReloc reloc;
        reloc.offset = off;
        reloc.type = coff_x64::kAddr64;
        reloc.symIndex = targetSymIdx;
        reloc.addend = 0;
        dataSec.relocs.push_back(reloc);
        return checkedSyntheticSymbolIndex(obj.symbols.size() - 1, name.c_str());
    };

    /// @brief Publishes `__imp_name` as a pointer alias to a generated definition.
    /// @param name Canonical generated definition name.
    /// @param targetSymIdx Generated definition's symbol index.
    auto addImportAlias = [&](const std::string &name, uint32_t targetSymIdx) {
        if (dynamicSyms.count("__imp_" + name))
            addAbs64DataRef("__imp_" + name, 8, targetSymIdx);
    };

    /// @brief Adds an x64 relative-jump forwarding helper and its target relocation.
    /// @param name Published forwarding helper name.
    /// @param target Undefined symbol receiving the jump.
    /// @return Synthetic helper symbol index.
    auto addJmpFn = [&](const std::string &name, const std::string &target) {
        const size_t off = textSec.data.size();
        textSec.data.insert(textSec.data.end(), {0xE9, 0x00, 0x00, 0x00, 0x00});

        ObjSymbol sym;
        sym.name = name;
        sym.binding = ObjSymbol::Global;
        sym.sectionIndex = 1;
        sym.offset = off;
        obj.symbols.push_back(std::move(sym));
        const uint32_t idx = checkedSyntheticSymbolIndex(obj.symbols.size() - 1, name.c_str());

        ObjSymbol targetSym;
        targetSym.name = target;
        targetSym.binding = ObjSymbol::Undefined;
        const uint32_t targetIdx = checkedSyntheticSymbolIndex(obj.symbols.size(), target.c_str());
        obj.symbols.push_back(std::move(targetSym));

        ObjReloc reloc;
        reloc.offset = off + 1;
        reloc.type = coff_x64::kRel32;
        reloc.symIndex = targetIdx;
        reloc.addend = 0;
        textSec.relocs.push_back(reloc);

        addImportAlias(name, idx);
        return idx;
    };

    if (needsHelper("_fltused")) {
        const uint32_t idx = addData("_fltused", {1, 0, 0, 0}, 4);
        addImportAlias("_fltused", idx);
    }
    if (needsHelper("__security_cookie")) {
        const uint32_t idx =
            addData("__security_cookie", {0x32, 0xA2, 0xDF, 0x2D, 0x99, 0x2B, 0x00, 0x00}, 8);
        addImportAlias("__security_cookie", idx);
    }
    if (needsHelper("__security_cookie_complement")) {
        const uint32_t idx = addData(
            "__security_cookie_complement", {0xCD, 0x5D, 0x20, 0xD2, 0x66, 0xD4, 0xFF, 0xFF}, 8);
        addImportAlias("__security_cookie_complement", idx);
    }
    if (needTlsIndex || needsHelper("_tls_index")) {
        const uint32_t idx = addData("_tls_index", {0, 0, 0, 0}, 4);
        addImportAlias("_tls_index", idx);
    }
    if (needsHelper("_is_c_termination_complete")) {
        const uint32_t idx = addData("_is_c_termination_complete", {0, 0, 0, 0}, 4);
        addImportAlias("_is_c_termination_complete", idx);
    }
    if (needsHelper("__isa_available")) {
        const uint32_t idx = addData("__isa_available", {0, 0, 0, 0}, 4);
        addImportAlias("__isa_available", idx);
    }
    if (needsHelper("?_OptionsStorage@?1??__local_stdio_printf_options@@9@9")) {
        const uint32_t idx = addData(
            "?_OptionsStorage@?1??__local_stdio_printf_options@@9@9", {0, 0, 0, 0, 0, 0, 0, 0}, 8);
        addImportAlias("?_OptionsStorage@?1??__local_stdio_printf_options@@9@9", idx);
    }
    if (needsHelper("?_OptionsStorage@?1??__local_stdio_scanf_options@@9@9")) {
        const uint32_t idx = addData(
            "?_OptionsStorage@?1??__local_stdio_scanf_options@@9@9", {0, 0, 0, 0, 0, 0, 0, 0}, 8);
        addImportAlias("?_OptionsStorage@?1??__local_stdio_scanf_options@@9@9", idx);
    }

    if (needsHelper("__security_check_cookie")) {
        const uint32_t idx = addRetFn("__security_check_cookie", {0xC3});
        addImportAlias("__security_check_cookie", idx);
    }
    if (needsHelper("__security_init_cookie")) {
        const uint32_t idx = addRetFn("__security_init_cookie", {0xC3});
        addImportAlias("__security_init_cookie", idx);
    }
    if (needsHelper("__GSHandlerCheck")) {
        const uint32_t idx = addRetFn("__GSHandlerCheck", {0xC3});
        addImportAlias("__GSHandlerCheck", idx);
    }
    if (needsHelper("__GSHandlerCheck_EH4")) {
        const uint32_t idx = addRetFn("__GSHandlerCheck_EH4", {0xC3});
        addImportAlias("__GSHandlerCheck_EH4", idx);
    }
    if (needsHelper("__CxxFrameHandler4")) {
        const uint32_t idx = addRetFn("__CxxFrameHandler4", {0xC3});
        addImportAlias("__CxxFrameHandler4", idx);
    }
    if (!debugWindowsRuntime && needsHelper("_CrtDbgReport")) {
        const uint32_t idx = addRetFn("_CrtDbgReport", {0x31, 0xC0, 0xC3});
        addImportAlias("_CrtDbgReport", idx);
    }
    if (!debugWindowsRuntime && needsHelper("_CrtDbgReportW")) {
        const uint32_t idx = addRetFn("_CrtDbgReportW", {0x31, 0xC0, 0xC3});
        addImportAlias("_CrtDbgReportW", idx);
    }
    if (!debugWindowsRuntime && needsHelper("_invalid_parameter")) {
        const uint32_t idx = addRetFn("_invalid_parameter", {0xC3});
        addImportAlias("_invalid_parameter", idx);
    }
    if (!debugWindowsRuntime && needsHelper("invalid_parameter")) {
        const uint32_t idx = addRetFn("invalid_parameter", {0xC3});
        addImportAlias("invalid_parameter", idx);
    }
    if (needsHelper("??_7type_info@@6B@")) {
        const uint32_t idx = addData("??_7type_info@@6B@", {0, 0, 0, 0, 0, 0, 0, 0}, 8);
        addImportAlias("??_7type_info@@6B@", idx);
    }
    if (needsHelper("??2@YAPEAX_K@Z")) {
        addJmpFn("??2@YAPEAX_K@Z", "malloc");
    }
    if (needsHelper("??2@YAPEAX_KAEBUnothrow_t@std@@@Z")) {
        addJmpFn("??2@YAPEAX_KAEBUnothrow_t@std@@@Z", "malloc");
    }
    if (needsHelper("??3@YAXPEAX@Z")) {
        addJmpFn("??3@YAXPEAX@Z", "free");
    }
    if (needsHelper("??3@YAXPEAX_K@Z")) {
        addJmpFn("??3@YAXPEAX_K@Z", "free");
    }
    if (needsHelper("_RTC_CheckStackVars")) {
        const uint32_t idx = addRetFn("_RTC_CheckStackVars", {0xC3});
        addImportAlias("_RTC_CheckStackVars", idx);
    }
    if (needsHelper("_RTC_InitBase")) {
        const uint32_t idx = addRetFn("_RTC_InitBase", {0x31, 0xC0, 0xC3});
        addImportAlias("_RTC_InitBase", idx);
    }
    if (needsHelper("_RTC_Shutdown")) {
        const uint32_t idx = addRetFn("_RTC_Shutdown", {0xC3});
        addImportAlias("_RTC_Shutdown", idx);
    }
    if (needsHelper("_RTC_UninitUse")) {
        const uint32_t idx = addRetFn("_RTC_UninitUse", {0xC3});
        addImportAlias("_RTC_UninitUse", idx);
    }
    if (needsHelper("IID_ID3D11Texture2D")) {
        const uint32_t idx = addData("IID_ID3D11Texture2D",
                                     {0xF2,
                                      0xAA,
                                      0x15,
                                      0x6F,
                                      0x08,
                                      0xD2,
                                      0x89,
                                      0x4E,
                                      0x9A,
                                      0xB4,
                                      0x48,
                                      0x95,
                                      0x35,
                                      0xD3,
                                      0x4F,
                                      0x9C},
                                     4);
        addImportAlias("IID_ID3D11Texture2D", idx);
    }
    if (needsHelper("__report_rangecheckfailure")) {
        const uint32_t idx = addRetFn("__report_rangecheckfailure", {0xCC, 0xC3});
        addImportAlias("__report_rangecheckfailure", idx);
    }
    if (needsHelper("__chkstk")) {
        std::vector<uint8_t> chkstk = {
            0x49, 0x89, 0xC2,                         // mov r10, rax
            0x49, 0x89, 0xE3,                         // mov r11, rsp
            0x49, 0x81, 0xFA, 0x00, 0x10, 0x00, 0x00, // cmp r10, 0x1000
            0x72, 0x1B,                               // jb tail
            0x49, 0x81, 0xEB, 0x00, 0x10, 0x00, 0x00, // sub r11, 0x1000
            0x41, 0xF6, 0x03, 0x00,                   // test byte ptr [r11], 0
            0x49, 0x81, 0xEA, 0x00, 0x10, 0x00, 0x00, // sub r10, 0x1000
            0x49, 0x81, 0xFA, 0x00, 0x10, 0x00, 0x00, // cmp r10, 0x1000
            0x73, 0xE5,                               // jae probe_loop
            0x4D, 0x29, 0xD3,                         // sub r11, r10
            0x41, 0xF6, 0x03, 0x00,                   // test byte ptr [r11], 0
            0xC3,                                     // ret
        };
        const size_t off = textSec.data.size();
        textSec.data.insert(textSec.data.end(), chkstk.begin(), chkstk.end());
        ObjSymbol sym;
        sym.name = "__chkstk";
        sym.binding = ObjSymbol::Global;
        sym.sectionIndex = 1;
        sym.offset = off;
        obj.symbols.push_back(std::move(sym));
        const uint32_t idx = checkedSyntheticSymbolIndex(obj.symbols.size() - 1, "__chkstk");
        addImportAlias("__chkstk", idx);
    }
    if (needsHelper("rt_audio_shutdown")) {
        const uint32_t idx = addRetFn("rt_audio_shutdown", {0xC3});
        addImportAlias("rt_audio_shutdown", idx);
    }
    if (needsHelper("__vcrt_initialize")) {
        const uint32_t idx = addRetFn("__vcrt_initialize", {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
        addImportAlias("__vcrt_initialize", idx);
    }
    if (needsHelper("__vcrt_thread_attach")) {
        const uint32_t idx = addRetFn("__vcrt_thread_attach", {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
        addImportAlias("__vcrt_thread_attach", idx);
    }
    if (needsHelper("__vcrt_thread_detach")) {
        const uint32_t idx = addRetFn("__vcrt_thread_detach", {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
        addImportAlias("__vcrt_thread_detach", idx);
    }
    if (needsHelper("__vcrt_uninitialize")) {
        const uint32_t idx = addRetFn("__vcrt_uninitialize", {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
        addImportAlias("__vcrt_uninitialize", idx);
    }
    if (needsHelper("__vcrt_uninitialize_critical")) {
        const uint32_t idx = addRetFn("__vcrt_uninitialize_critical", {0xC3});
        addImportAlias("__vcrt_uninitialize_critical", idx);
    }
    if (needsHelper("__acrt_initialize")) {
        const uint32_t idx = addRetFn("__acrt_initialize", {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
        addImportAlias("__acrt_initialize", idx);
    }
    if (needsHelper("__acrt_thread_attach")) {
        const uint32_t idx = addRetFn("__acrt_thread_attach", {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
        addImportAlias("__acrt_thread_attach", idx);
    }
    if (needsHelper("__acrt_thread_detach")) {
        const uint32_t idx = addRetFn("__acrt_thread_detach", {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
        addImportAlias("__acrt_thread_detach", idx);
    }
    if (needsHelper("__acrt_uninitialize")) {
        const uint32_t idx = addRetFn("__acrt_uninitialize", {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
        addImportAlias("__acrt_uninitialize", idx);
    }
    if (needsHelper("__acrt_uninitialize_critical")) {
        const uint32_t idx = addRetFn("__acrt_uninitialize_critical", {0xC3});
        addImportAlias("__acrt_uninitialize_critical", idx);
    }
    if (needsHelper("__isa_available_init")) {
        const uint32_t idx = addRetFn("__isa_available_init", {0xC3});
        addImportAlias("__isa_available_init", idx);
    }
    if (needsHelper("__scrt_exe_initialize_mta")) {
        const uint32_t idx = addRetFn("__scrt_exe_initialize_mta", {0x31, 0xC0, 0xC3});
        addImportAlias("__scrt_exe_initialize_mta", idx);
    }

    if (needsHelper("__guard_dispatch_icall_fptr")) {
        const uint32_t dispatchIdx = addRetFn("__guard_dispatch_icall_stub", {0xFF, 0xE0});
        const uint32_t ptrIdx = addAbs64DataRef("__guard_dispatch_icall_fptr", 8, dispatchIdx);
        addImportAlias("__guard_dispatch_icall_fptr", ptrIdx);
    }

    if (dynamicSyms.count("vm_trap")) {
        const size_t off = textSec.data.size();
        textSec.data.insert(textSec.data.end(), {0xE9, 0x00, 0x00, 0x00, 0x00});

        ObjSymbol vmTrap;
        vmTrap.name = "vm_trap";
        vmTrap.binding = ObjSymbol::Global;
        vmTrap.sectionIndex = 1;
        vmTrap.offset = off;
        obj.symbols.push_back(std::move(vmTrap));

        ObjSymbol target;
        target.name = haveVmTrapDefault ? "vm_trap_default" : "rt_abort";
        target.binding = ObjSymbol::Undefined;
        const uint32_t targetIdx =
            checkedSyntheticSymbolIndex(obj.symbols.size(), target.name.c_str());
        obj.symbols.push_back(std::move(target));

        ObjReloc reloc;
        reloc.offset = off + 1;
        reloc.type = coff_x64::kRel32;
        reloc.symIndex = targetIdx;
        reloc.addend = 0;
        textSec.relocs.push_back(reloc);
    }

    if (!textSec.data.empty())
        obj.sections.push_back(std::move(textSec));
    if (!dataSec.data.empty()) {
        if (obj.sections.size() == 1)
            obj.sections.push_back(ObjSection{});
        obj.sections.push_back(std::move(dataSec));
    }
    return obj;
}

/// @brief Synthesizes the required Windows AArch64 CRT and ABI helper definitions.
/// @param dynamicSyms Current unresolved direct and import-pointer symbol names.
/// @param haveVmTrapDefault Whether a static `vm_trap_default` target exists.
/// @param needTlsIndex Whether TLS index placeholders must be emitted.
/// @param debugWindowsRuntime Whether Debug CRT-only compatibility hooks are allowed.
/// @return Synthetic AArch64 COFF object containing only required definitions.
/// @throws std::runtime_error If generated size or symbol indices overflow.
ObjFile generateWindowsArm64Helpers(const std::unordered_set<std::string> &dynamicSyms,
                                    bool haveVmTrapDefault,
                                    bool needTlsIndex,
                                    bool debugWindowsRuntime) {
    ObjFile obj = makeWindowsHelpersObj("<winarm64-helpers>", 0xAA64);
    ObjSection textSec = makeWindowsHelpersTextSec();
    ObjSection dataSec = makeWindowsHelpersDataSec();

    /// @brief Tests both direct and `__imp_` spellings for an unresolved helper.
    /// @param name Canonical helper symbol name.
    /// @return `true` when either spelling remains unresolved.
    auto needsHelper = [&](const std::string &name) {
        return dynamicSyms.count(name) || dynamicSyms.count("__imp_" + name);
    };

    /// @brief Encodes an AArch64 instruction sequence and publishes its global symbol.
    /// @param name Published helper name.
    /// @param insns AArch64 instruction words.
    /// @return Synthetic symbol-table index.
    auto addTextFn = [&](const std::string &name, const std::vector<uint32_t> &insns) {
        const size_t off = textSec.data.size();
        for (uint32_t insn : insns)
            appendArm64Insn(textSec.data, insn);

        ObjSymbol sym;
        sym.name = name;
        sym.binding = ObjSymbol::Global;
        sym.sectionIndex = 1;
        sym.offset = off;
        obj.symbols.push_back(std::move(sym));
        return checkedSyntheticSymbolIndex(obj.symbols.size() - 1, name.c_str());
    };

    /// @brief Adds a helper consisting only of `ret`.
    /// @param name Published helper name.
    /// @return Synthetic symbol-table index.
    auto addRetFn = [&](const std::string &name) {
        return addTextFn(name, {0xD65F03C0U}); // ret
    };

    /// @brief Adds a helper returning a small immediate in `x0`.
    /// @param name Published helper name.
    /// @param imm Immediate return value.
    /// @return Synthetic symbol-table index.
    auto addRetImmFn = [&](const std::string &name, uint16_t imm) {
        return addTextFn(name, {0xD2800000U | (static_cast<uint32_t>(imm) << 5), 0xD65F03C0U});
    };

    /// @brief Appends aligned helper data and publishes its global symbol.
    /// @param name Published data symbol name.
    /// @param bytes Initial data bytes.
    /// @param align Required byte alignment.
    /// @return Synthetic symbol-table index.
    auto addData = [&](const std::string &name, const std::vector<uint8_t> &bytes, uint32_t align) {
        while ((dataSec.data.size() % align) != 0)
            dataSec.data.push_back(0);
        const size_t off = dataSec.data.size();
        dataSec.data.insert(dataSec.data.end(), bytes.begin(), bytes.end());
        ObjSymbol sym;
        sym.name = name;
        sym.binding = ObjSymbol::Global;
        sym.sectionIndex = 2;
        sym.offset = off;
        obj.symbols.push_back(std::move(sym));
        return checkedSyntheticSymbolIndex(obj.symbols.size() - 1, name.c_str());
    };

    /// @brief Adds an eight-byte data pointer and an absolute target relocation.
    /// @param name Published pointer symbol name.
    /// @param align Required byte alignment.
    /// @param targetSymIdx Relocation target symbol index.
    /// @return Synthetic pointer symbol index.
    auto addAbs64DataRef = [&](const std::string &name, uint32_t align, uint32_t targetSymIdx) {
        while ((dataSec.data.size() % align) != 0)
            dataSec.data.push_back(0);
        const size_t off = dataSec.data.size();
        dataSec.data.resize(checkedSyntheticSizeAdd(off, 8, name.c_str()), 0);
        ObjSymbol sym;
        sym.name = name;
        sym.binding = ObjSymbol::Global;
        sym.sectionIndex = 2;
        sym.offset = off;
        obj.symbols.push_back(std::move(sym));

        ObjReloc reloc;
        reloc.offset = off;
        reloc.type = coff_a64::kAddr64;
        reloc.symIndex = targetSymIdx;
        reloc.addend = 0;
        dataSec.relocs.push_back(reloc);
        return checkedSyntheticSymbolIndex(obj.symbols.size() - 1, name.c_str());
    };

    /// @brief Publishes `__imp_name` as a pointer alias to a generated definition.
    /// @param name Canonical generated definition name.
    /// @param targetSymIdx Generated definition's symbol index.
    auto addImportAlias = [&](const std::string &name, uint32_t targetSymIdx) {
        if (dynamicSyms.count("__imp_" + name))
            addAbs64DataRef("__imp_" + name, 8, targetSymIdx);
    };

    if (needsHelper("_fltused")) {
        const uint32_t idx = addData("_fltused", {1, 0, 0, 0}, 4);
        addImportAlias("_fltused", idx);
    }
    if (needsHelper("__security_cookie")) {
        const uint32_t idx =
            addData("__security_cookie", {0x32, 0xA2, 0xDF, 0x2D, 0x99, 0x2B, 0x00, 0x00}, 8);
        addImportAlias("__security_cookie", idx);
    }
    if (needsHelper("__security_cookie_complement")) {
        const uint32_t idx = addData(
            "__security_cookie_complement", {0xCD, 0x5D, 0x20, 0xD2, 0x66, 0xD4, 0xFF, 0xFF}, 8);
        addImportAlias("__security_cookie_complement", idx);
    }
    if (needTlsIndex || needsHelper("_tls_index")) {
        const uint32_t idx = addData("_tls_index", {0, 0, 0, 0}, 4);
        addImportAlias("_tls_index", idx);
    }
    if (needsHelper("_is_c_termination_complete")) {
        const uint32_t idx = addData("_is_c_termination_complete", {0, 0, 0, 0}, 4);
        addImportAlias("_is_c_termination_complete", idx);
    }
    if (needsHelper("__isa_available")) {
        const uint32_t idx = addData("__isa_available", {0, 0, 0, 0}, 4);
        addImportAlias("__isa_available", idx);
    }
    if (needsHelper("?_OptionsStorage@?1??__local_stdio_printf_options@@9@9")) {
        const uint32_t idx = addData(
            "?_OptionsStorage@?1??__local_stdio_printf_options@@9@9", {0, 0, 0, 0, 0, 0, 0, 0}, 8);
        addImportAlias("?_OptionsStorage@?1??__local_stdio_printf_options@@9@9", idx);
    }
    if (needsHelper("?_OptionsStorage@?1??__local_stdio_scanf_options@@9@9")) {
        const uint32_t idx = addData(
            "?_OptionsStorage@?1??__local_stdio_scanf_options@@9@9", {0, 0, 0, 0, 0, 0, 0, 0}, 8);
        addImportAlias("?_OptionsStorage@?1??__local_stdio_scanf_options@@9@9", idx);
    }

    if (needsHelper("__security_check_cookie")) {
        const uint32_t idx = addRetFn("__security_check_cookie");
        addImportAlias("__security_check_cookie", idx);
    }
    if (needsHelper("__security_init_cookie")) {
        const uint32_t idx = addRetFn("__security_init_cookie");
        addImportAlias("__security_init_cookie", idx);
    }
    if (needsHelper("__GSHandlerCheck")) {
        const uint32_t idx = addRetFn("__GSHandlerCheck");
        addImportAlias("__GSHandlerCheck", idx);
    }
    if (needsHelper("__GSHandlerCheck_EH4")) {
        const uint32_t idx = addRetFn("__GSHandlerCheck_EH4");
        addImportAlias("__GSHandlerCheck_EH4", idx);
    }
    if (!debugWindowsRuntime && needsHelper("_CrtDbgReport")) {
        const uint32_t idx = addRetImmFn("_CrtDbgReport", 0);
        addImportAlias("_CrtDbgReport", idx);
    }
    if (!debugWindowsRuntime && needsHelper("_CrtDbgReportW")) {
        const uint32_t idx = addRetImmFn("_CrtDbgReportW", 0);
        addImportAlias("_CrtDbgReportW", idx);
    }
    if (!debugWindowsRuntime && needsHelper("_invalid_parameter")) {
        const uint32_t idx = addRetFn("_invalid_parameter");
        addImportAlias("_invalid_parameter", idx);
    }
    if (!debugWindowsRuntime && needsHelper("invalid_parameter")) {
        const uint32_t idx = addRetFn("invalid_parameter");
        addImportAlias("invalid_parameter", idx);
    }
    if (needsHelper("_RTC_CheckStackVars")) {
        const uint32_t idx = addRetFn("_RTC_CheckStackVars");
        addImportAlias("_RTC_CheckStackVars", idx);
    }
    if (needsHelper("_RTC_InitBase")) {
        const uint32_t idx = addRetImmFn("_RTC_InitBase", 0);
        addImportAlias("_RTC_InitBase", idx);
    }
    if (needsHelper("_RTC_Shutdown")) {
        const uint32_t idx = addRetFn("_RTC_Shutdown");
        addImportAlias("_RTC_Shutdown", idx);
    }
    if (needsHelper("_RTC_UninitUse")) {
        const uint32_t idx = addRetFn("_RTC_UninitUse");
        addImportAlias("_RTC_UninitUse", idx);
    }
    if (needsHelper("IID_ID3D11Texture2D")) {
        const uint32_t idx = addData("IID_ID3D11Texture2D",
                                     {0xF2,
                                      0xAA,
                                      0x15,
                                      0x6F,
                                      0x08,
                                      0xD2,
                                      0x89,
                                      0x4E,
                                      0x9A,
                                      0xB4,
                                      0x48,
                                      0x95,
                                      0x35,
                                      0xD3,
                                      0x4F,
                                      0x9C},
                                     4);
        addImportAlias("IID_ID3D11Texture2D", idx);
    }
    if (needsHelper("__report_rangecheckfailure")) {
        const uint32_t idx = addTextFn("__report_rangecheckfailure", {0xD4200000U, 0xD65F03C0U});
        addImportAlias("__report_rangecheckfailure", idx);
    }
    if (needsHelper("__chkstk")) {
        const uint32_t idx = addRetFn("__chkstk");
        addImportAlias("__chkstk", idx);
    }
    if (needsHelper("_InterlockedCompareExchange")) {
        const uint32_t idx = addTextFn("_InterlockedCompareExchange",
                                       {0x885FFC03U,
                                        0x6B02007FU,
                                        0x54000061U,
                                        0x8804FC01U,
                                        0x35FFFF84U,
                                        0x2A0303E0U,
                                        0xD65F03C0U});
        addImportAlias("_InterlockedCompareExchange", idx);
    }
    if (needsHelper("_InterlockedCompareExchange64")) {
        const uint32_t idx = addTextFn("_InterlockedCompareExchange64",
                                       {0xC85FFC03U,
                                        0xEB02007FU,
                                        0x54000061U,
                                        0xC804FC01U,
                                        0x35FFFF84U,
                                        0xAA0303E0U,
                                        0xD65F03C0U});
        addImportAlias("_InterlockedCompareExchange64", idx);
    }
    if (needsHelper("_InterlockedCompareExchangePointer")) {
        const uint32_t idx = addTextFn("_InterlockedCompareExchangePointer",
                                       {0xC85FFC03U,
                                        0xEB02007FU,
                                        0x54000061U,
                                        0xC804FC01U,
                                        0x35FFFF84U,
                                        0xAA0303E0U,
                                        0xD65F03C0U});
        addImportAlias("_InterlockedCompareExchangePointer", idx);
    }
    if (needsHelper("_InterlockedExchange")) {
        const uint32_t idx =
            addTextFn("_InterlockedExchange",
                      {0x885FFC02U, 0x8803FC01U, 0x35FFFFC3U, 0x2A0203E0U, 0xD65F03C0U});
        addImportAlias("_InterlockedExchange", idx);
    }
    if (needsHelper("_InterlockedExchange64")) {
        const uint32_t idx =
            addTextFn("_InterlockedExchange64",
                      {0xC85FFC02U, 0xC803FC01U, 0x35FFFFC3U, 0xAA0203E0U, 0xD65F03C0U});
        addImportAlias("_InterlockedExchange64", idx);
    }
    if (needsHelper("_InterlockedExchangePointer")) {
        const uint32_t idx =
            addTextFn("_InterlockedExchangePointer",
                      {0xC85FFC02U, 0xC803FC01U, 0x35FFFFC3U, 0xAA0203E0U, 0xD65F03C0U});
        addImportAlias("_InterlockedExchangePointer", idx);
    }
    if (needsHelper("_InterlockedExchange8")) {
        const uint32_t idx =
            addTextFn("_InterlockedExchange8",
                      {0x085FFC02U, 0x0803FC01U, 0x35FFFFC3U, 0x2A0203E0U, 0xD65F03C0U});
        addImportAlias("_InterlockedExchange8", idx);
    }
    if (needsHelper("_InterlockedExchangeAdd")) {
        const uint32_t idx = addTextFn(
            "_InterlockedExchangeAdd",
            {0x885FFC02U, 0x0B010044U, 0x8803FC04U, 0x35FFFFA3U, 0x2A0203E0U, 0xD65F03C0U});
        addImportAlias("_InterlockedExchangeAdd", idx);
    }
    if (needsHelper("_InterlockedExchangeAdd64")) {
        const uint32_t idx = addTextFn(
            "_InterlockedExchangeAdd64",
            {0xC85FFC02U, 0x8B010044U, 0xC803FC04U, 0x35FFFFA3U, 0xAA0203E0U, 0xD65F03C0U});
        addImportAlias("_InterlockedExchangeAdd64", idx);
    }
    if (needsHelper("_InterlockedIncrement64")) {
        const uint32_t idx = addTextFn(
            "_InterlockedIncrement64",
            {0xC85FFC01U, 0x91000421U, 0xC802FC01U, 0x35FFFFA2U, 0xAA0103E0U, 0xD65F03C0U});
        addImportAlias("_InterlockedIncrement64", idx);
    }
    if (needsHelper("_InterlockedDecrement")) {
        const uint32_t idx = addTextFn(
            "_InterlockedDecrement",
            {0x885FFC02U, 0x51000441U, 0x8803FC01U, 0x35FFFFA3U, 0x2A0103E0U, 0xD65F03C0U});
        addImportAlias("_InterlockedDecrement", idx);
    }
    if (needsHelper("_InterlockedOr")) {
        const uint32_t idx = addTextFn(
            "_InterlockedOr",
            {0x885FFC02U, 0x2A010044U, 0x8803FC04U, 0x35FFFFA3U, 0x2A0203E0U, 0xD65F03C0U});
        addImportAlias("_InterlockedOr", idx);
    }
    if (needsHelper("__RTC_memset")) {
        // MSVC's ARM64 runtime-check instrumentation may call __RTC_memset before
        // spilling incoming argument registers or the hidden aggregate-return
        // pointer in x8. Keep x3-x8 intact.
        const uint32_t idx = addTextFn(
            "__RTC_memset",
            {0xAA0003E9U, 0xB4000082U, 0x38001521U, 0xF1000442U, 0x54FFFFC1U, 0xD65F03C0U});
        addImportAlias("__RTC_memset", idx);
    }
    if (needsHelper("__security_push_cookie")) {
        // MSVC ARM64 /GS helpers reserve a stack cookie slot between saved
        // FP/LR and the function's local frame. The linker stubs validation,
        // but it still has to preserve that stack contract.
        const uint32_t idx = addTextFn("__security_push_cookie",
                                       {0xD10043FFU, 0xD65F03C0U}); // sub sp, sp, #0x10; ret
        addImportAlias("__security_push_cookie", idx);
    }
    if (needsHelper("__security_pop_cookie")) {
        const uint32_t idx = addTextFn("__security_pop_cookie",
                                       {0x910043FFU, 0xD65F03C0U}); // add sp, sp, #0x10; ret
        addImportAlias("__security_pop_cookie", idx);
    }
    if (needsHelper("rt_audio_shutdown")) {
        const uint32_t idx = addRetFn("rt_audio_shutdown");
        addImportAlias("rt_audio_shutdown", idx);
    }
    if (needsHelper("__vcrt_initialize")) {
        const uint32_t idx = addRetImmFn("__vcrt_initialize", 1);
        addImportAlias("__vcrt_initialize", idx);
    }
    if (needsHelper("__vcrt_thread_attach")) {
        const uint32_t idx = addRetImmFn("__vcrt_thread_attach", 1);
        addImportAlias("__vcrt_thread_attach", idx);
    }
    if (needsHelper("__vcrt_thread_detach")) {
        const uint32_t idx = addRetImmFn("__vcrt_thread_detach", 1);
        addImportAlias("__vcrt_thread_detach", idx);
    }
    if (needsHelper("__vcrt_uninitialize")) {
        const uint32_t idx = addRetImmFn("__vcrt_uninitialize", 1);
        addImportAlias("__vcrt_uninitialize", idx);
    }
    if (needsHelper("__vcrt_uninitialize_critical")) {
        const uint32_t idx = addRetFn("__vcrt_uninitialize_critical");
        addImportAlias("__vcrt_uninitialize_critical", idx);
    }
    if (needsHelper("__acrt_initialize")) {
        const uint32_t idx = addRetImmFn("__acrt_initialize", 1);
        addImportAlias("__acrt_initialize", idx);
    }
    if (needsHelper("__acrt_thread_attach")) {
        const uint32_t idx = addRetImmFn("__acrt_thread_attach", 1);
        addImportAlias("__acrt_thread_attach", idx);
    }
    if (needsHelper("__acrt_thread_detach")) {
        const uint32_t idx = addRetImmFn("__acrt_thread_detach", 1);
        addImportAlias("__acrt_thread_detach", idx);
    }
    if (needsHelper("__acrt_uninitialize")) {
        const uint32_t idx = addRetImmFn("__acrt_uninitialize", 1);
        addImportAlias("__acrt_uninitialize", idx);
    }
    if (needsHelper("__acrt_uninitialize_critical")) {
        const uint32_t idx = addRetFn("__acrt_uninitialize_critical");
        addImportAlias("__acrt_uninitialize_critical", idx);
    }
    if (needsHelper("__isa_available_init")) {
        const uint32_t idx = addRetFn("__isa_available_init");
        addImportAlias("__isa_available_init", idx);
    }
    if (needsHelper("__scrt_exe_initialize_mta")) {
        const uint32_t idx = addRetImmFn("__scrt_exe_initialize_mta", 0);
        addImportAlias("__scrt_exe_initialize_mta", idx);
    }

    if (needsHelper("__guard_dispatch_icall_fptr")) {
        const uint32_t dispatchIdx = addTextFn("__guard_dispatch_icall_stub", {0xD61F0200U});
        const uint32_t ptrIdx = addAbs64DataRef("__guard_dispatch_icall_fptr", 8, dispatchIdx);
        addImportAlias("__guard_dispatch_icall_fptr", ptrIdx);
    }

    if (dynamicSyms.count("vm_trap")) {
        const size_t off = textSec.data.size();
        appendArm64Insn(textSec.data, 0x14000000U); // b target

        ObjSymbol vmTrap;
        vmTrap.name = "vm_trap";
        vmTrap.binding = ObjSymbol::Global;
        vmTrap.sectionIndex = 1;
        vmTrap.offset = off;
        obj.symbols.push_back(std::move(vmTrap));

        ObjSymbol target;
        target.name = haveVmTrapDefault ? "vm_trap_default" : "rt_abort";
        target.binding = ObjSymbol::Undefined;
        const uint32_t targetIdx =
            checkedSyntheticSymbolIndex(obj.symbols.size(), target.name.c_str());
        obj.symbols.push_back(std::move(target));

        ObjReloc reloc;
        reloc.offset = off;
        reloc.type = coff_a64::kBranch26;
        reloc.symIndex = targetIdx;
        reloc.addend = 0;
        textSec.relocs.push_back(reloc);
    }

    if (!textSec.data.empty())
        obj.sections.push_back(std::move(textSec));
    if (!dataSec.data.empty()) {
        if (obj.sections.size() == 1)
            obj.sections.push_back(ObjSection{});
        obj.sections.push_back(std::move(dataSec));
    }
    return obj;
}

/// @brief Read every static archive at @p paths into @p outArchives.
/// @details A discrete "read archives" pipeline stage extracted from
///          @ref nativeLink so the driver can read as a sequence of named
///          stages rather than inlining 10+ LOC of archive iteration. Writes
///          an error to @p err and returns false on any per-archive failure.
/// @param paths Archive paths to read in order.
/// @param outArchives Destination shared archive snapshots; existing entries are preserved.
/// @param err Diagnostic output stream.
/// @return `true` after every archive is loaded.
static bool readArchiveFiles(const std::vector<std::string> &paths,
                             std::vector<std::shared_ptr<const Archive>> &outArchives,
                             std::ostream &err) {
    for (const auto &arPath : paths) {
        std::shared_ptr<const Archive> ar;
        if (!readArchiveCached(arPath, ar, err)) {
            err << "error: failed to read archive '" << arPath << "'\n";
            return false;
        }
        outArchives.push_back(std::move(ar));
    }
    return true;
}

/// @brief Force-load every member of every archive at @p paths as an
///        @ref ObjFile, appending the parsed members to @p extraObjects.
/// @details Extracted from @ref nativeLink as a discrete pipeline stage so
///          the strong-override-weak resolution rationale for force-loading
///          archives has a single named entry point. Empty members are
///          skipped silently; any read or parse failure returns false with
///          an error message written to @p err.
/// @param paths Force-load archive paths.
/// @param extraObjects Destination object list; parsed members are appended.
/// @param err Diagnostic output stream.
/// @return `true` after all nonempty members are parsed.
static bool loadForceLoadArchiveMembers(const std::vector<std::string> &paths,
                                        std::vector<ObjFile> &extraObjects,
                                        std::ostream &err) {
    for (const auto &arPath : paths) {
        std::shared_ptr<const Archive> forceAr;
        if (!readArchiveCached(arPath, forceAr, err)) {
            err << "error: failed to read force-load archive '" << arPath << "'\n";
            return false;
        }
        for (const auto &member : forceAr->members) {
            const ArchiveMemberView view = memberDataView(*forceAr, member);
            if (view.data == nullptr || view.size == 0)
                continue;
            ObjFile memberObj;
            if (!readObjFile(
                    view.data, view.size, arPath + "(" + member.name + ")", memberObj, err)) {
                err << "error: failed to parse force-load member '" << member.name << "' in '"
                    << arPath << "'\n";
                return false;
            }
            extraObjects.push_back(std::move(memberObj));
        }
    }
    return true;
}

} // namespace

/// @brief Run the full native link pipeline: parse objects → resolve symbols →
///        merge sections → apply relocations → write executable.
/// @details Supports ELF (Linux), Mach-O (macOS), and PE (Windows). The pipeline
///          reads the user's .o and runtime .a archives, resolves all symbols
///          (including dynamic stubs on macOS), dead-strips unreachable code,
///          performs ICF, inserts branch trampolines, applies relocations, and
///          writes the final executable. Zero external tool dependencies.
/// @param opts Link request and target configuration.
/// @param err Diagnostic and optional timing output stream.
/// @return Zero on success; one after a diagnosed failure.
int nativeLink(const NativeLinkerOptions &opts, std::ostream & /*out*/, std::ostream &err) {
    LinkTiming timing(err);

    // Step 1: Read the user's object file.
    ObjFile userObj;
    const bool readUserObject =
        opts.objData
            ? readObjFile(opts.objData->data(), opts.objData->size(), opts.objPath, userObj, err)
            : readObjFile(opts.objPath, userObj, err);
    if (!readUserObject) {
        err << "error: failed to read object file '" << opts.objPath << "'\n";
        return 1;
    }

    // Step 1b: Read extra object files (e.g., asset blob).
    std::vector<ObjFile> extraObjects;
    for (const auto &extraPath : opts.extraObjPaths) {
        ObjFile extraObj;
        if (!readObjFile(extraPath, extraObj, err)) {
            err << "error: failed to read extra object '" << extraPath << "'\n";
            return 1;
        }
        extraObjects.push_back(std::move(extraObj));
    }

    // Step 1c: Force-load archives — materialize every member so its strong
    // definitions participate in resolution unconditionally. Step 3's
    // demand-driven extraction would otherwise let weak runtime stubs (e.g.
    // rt_zia_* in zanna_rt_base) satisfy the symbols first, so the strong
    // editor-service definitions would never be pulled. Loading them as
    // initial objects lets SymbolResolver's "Strong overrides Weak" rule make
    // them win. Special members (symbol/string tables) are already excluded
    // from Archive::members by the archive reader.
    if (!loadForceLoadArchiveMembers(opts.forceLoadArchivePaths, extraObjects, err))
        return 1;
    timing.mark("read-input-objects");

    // Step 2: Read all archive files.
    std::vector<std::shared_ptr<const Archive>> archiveOwners;
    if (!readArchiveFiles(opts.archivePaths, archiveOwners, err))
        return 1;
    std::vector<const Archive *> archives;
    archives.reserve(archiveOwners.size());
    for (const auto &archive : archiveOwners)
        archives.push_back(archive.get());
    timing.mark("read-archives");

    // Step 3: Symbol resolution (iterative archive extraction).
    std::vector<ObjFile> initialObjects = {userObj};
    for (auto &extra : extraObjects)
        initialObjects.push_back(std::move(extra));
    if (!opts.entrySymbol.empty())
        initialObjects.push_back(makeUndefinedRootObject(userObj, opts.entrySymbol));
    // Embedding C++ editor services pulls in libc++ atexit code that references
    // `__dso_handle`; crt-less Zanna binaries must define it themselves.
    if (!opts.forceLoadArchivePaths.empty())
        initialObjects.push_back(makeDsoHandleObject(userObj));
    std::unordered_map<std::string, GlobalSymEntry> globalSyms;
    std::vector<ObjFile> allObjects;
    std::unordered_set<std::string> dynamicSyms;
    const bool debugWindowsRuntime =
        opts.platform == LinkPlatform::Windows &&
        opts.windowsDebugRuntime.value_or(usesDebugWindowsRuntime(opts.archivePaths));

    if (!resolveSymbols(
            initialObjects, archives, globalSyms, allObjects, dynamicSyms, err, opts.platform)) {
        err << "error: symbol resolution failed\n";
        return 1;
    }
    if (!validateInputObjects(allObjects, opts.platform, opts.arch, err))
        return 1;
    timing.mark("resolve-symbols");
    // Step 3.5a: Generate ObjC selector stubs (macOS — objc_msgSend$selector symbols).
    // Must come before dynamic stubs since it moves symbols from dynamicSyms and
    // ensures objc_msgSend itself is in the dynamic set.
    if (opts.arch == LinkArch::AArch64 && opts.platform == LinkPlatform::macOS) {
        ObjFile objcStubs;
        try {
            objcStubs = generateObjcSelectorStubsAArch64(dynamicSyms);
        } catch (const std::exception &ex) {
            err << "error: failed to generate ObjC selector stubs: " << ex.what() << "\n";
            return 1;
        }
        if (!objcStubs.sections.empty()) {
            const size_t objcIdx = allObjects.size();
            allObjects.push_back(std::move(objcStubs));

            const auto &stubs = allObjects[objcIdx];
            for (size_t i = 1; i < stubs.symbols.size(); ++i) {
                const auto &sym = stubs.symbols[i];
                installSyntheticGlobal(sym, objcIdx, globalSyms);
            }
        }
    }

    // Step 3.5b: Generate dynamic symbol stubs (macOS/ELF — needed for shared library imports).
    std::vector<DllImport> peImports;
    std::unordered_map<std::string, uint32_t> peImportSlotRvas;
    if (opts.platform == LinkPlatform::Windows) {
        dynamicSyms.erase("__ImageBase");
        const bool haveVmTrapDefault = globalSyms.find("vm_trap_default") != globalSyms.end();
        /// @brief Tests whether an object contains an allocated TLS section.
        /// @param obj Object file to inspect.
        /// @return `true` when any section is allocated TLS data.
        const bool needTlsIndex =
            globalSyms.find("_tls_index") != globalSyms.end() || dynamicSyms.count("_tls_index") ||
            dynamicSyms.count("__imp__tls_index") ||
            std::any_of(allObjects.begin(), allObjects.end(), [](const ObjFile &obj) {
                /// @brief Tests whether a section is allocated TLS data.
                /// @param sec Object section to inspect.
                /// @return `true` when the section is both allocated and TLS.
                return std::any_of(obj.sections.begin(),
                                   obj.sections.end(),
                                   [](const ObjSection &sec) { return sec.alloc && sec.tls; });
            });

        if (opts.arch == LinkArch::X86_64 || opts.arch == LinkArch::AArch64) {
            /// @brief Tests direct and import-pointer spellings before adding CRT dependencies.
            /// @param name Canonical dynamic symbol name.
            /// @return `true` when either spelling remains unresolved.
            auto needsDynamicSym = [&](const std::string &name) {
                return dynamicSyms.count(name) || dynamicSyms.count("__imp_" + name);
            };
            if (opts.arch == LinkArch::X86_64 &&
                (needsDynamicSym("??2@YAPEAX_K@Z") ||
                 needsDynamicSym("??2@YAPEAX_KAEBUnothrow_t@std@@@Z"))) {
                dynamicSyms.insert("malloc");
            }
            if (opts.arch == LinkArch::X86_64 &&
                (needsDynamicSym("??3@YAXPEAX@Z") || needsDynamicSym("??3@YAXPEAX_K@Z"))) {
                dynamicSyms.insert("free");
            }

            ObjFile helperObj;
            try {
                helperObj =
                    (opts.arch == LinkArch::AArch64)
                        ? generateWindowsArm64Helpers(
                              dynamicSyms, haveVmTrapDefault, needTlsIndex, debugWindowsRuntime)
                        : generateWindowsX64Helpers(
                              dynamicSyms, haveVmTrapDefault, needTlsIndex, debugWindowsRuntime);
            } catch (const std::exception &ex) {
                err << "error: failed to generate Windows helper object: " << ex.what() << "\n";
                return 1;
            }
            if (!helperObj.sections.empty()) {
                const size_t helperIdx = allObjects.size();
                allObjects.push_back(std::move(helperObj));
                registerSyntheticSymbols(allObjects[helperIdx], helperIdx, globalSyms);
                for (const auto &sym : allObjects[helperIdx].symbols) {
                    if (sym.binding == ObjSymbol::Local || sym.binding == ObjSymbol::Undefined ||
                        sym.name.empty())
                        continue;
                    dynamicSyms.erase(sym.name);
                }
            }
        }

        if (opts.entrySymbol == "main")
            dynamicSyms.insert("ExitProcess");

        WindowsImportPlan importPlan;
        if (!generateWindowsImports(opts.arch, dynamicSyms, debugWindowsRuntime, importPlan, err)) {
            return 1;
        }
        peImports = importPlan.imports;
        if (!importPlan.obj.sections.empty()) {
            const size_t importIdx = allObjects.size();
            allObjects.push_back(std::move(importPlan.obj));
            registerSyntheticSymbols(allObjects[importIdx], importIdx, globalSyms);
            for (const auto &imp : peImports) {
                for (const auto &fn : imp.functions) {
                    dynamicSyms.erase(fn);
                    dynamicSyms.erase("__imp_" + fn);
                }
            }
        }
    }

    const bool supportsDynamicStubs =
        (opts.platform == LinkPlatform::macOS && opts.arch == LinkArch::AArch64) ||
        (opts.platform == LinkPlatform::Linux &&
         (opts.arch == LinkArch::X86_64 || opts.arch == LinkArch::AArch64));
    if (!dynamicSyms.empty() && supportsDynamicStubs) {
        ObjFile stubObj;
        try {
            stubObj = (opts.platform == LinkPlatform::Linux && opts.arch == LinkArch::X86_64)
                          ? generateDynStubsX8664(dynamicSyms)
                          : generateDynStubsAArch64(dynamicSyms,
                                                    /*copyRelocDataSymbols=*/opts.platform ==
                                                        LinkPlatform::Linux);
        } catch (const std::exception &ex) {
            err << "error: failed to generate dynamic stubs: " << ex.what() << "\n";
            return 1;
        }
        const size_t stubObjIdx = allObjects.size();
        allObjects.push_back(std::move(stubObj));

        // Manually register stub and GOT symbols in globalSyms.
        // This overrides Dynamic entries with Global entries pointing to stubs.
        const auto &stubs = allObjects[stubObjIdx];
        for (size_t i = 1; i < stubs.symbols.size(); ++i) {
            const auto &sym = stubs.symbols[i];
            installSyntheticGlobal(sym, stubObjIdx, globalSyms);
        }
    }

    if (opts.platform == LinkPlatform::Windows) {
        dynamicSyms.erase("__ImageBase");
        dynamicSyms.erase("vm_trap");
    }

    if (!dynamicSyms.empty() && !supportsDynamicStubs) {
        std::vector<std::string> unsupported(dynamicSyms.begin(), dynamicSyms.end());
        std::sort(unsupported.begin(), unsupported.end());

        err << "error: native linker does not support dynamic imports on "
            << platformName(opts.platform) << ' ' << archName(opts.arch) << "\n";
        err << "error: supported dynamic-import targets are Windows x86_64, Windows AArch64, "
               "macOS AArch64, Linux x86_64, and Linux AArch64\n";
        err << "error: unresolved dynamic symbols:";
        for (const auto &sym : unsupported)
            err << ' ' << sym;
        err << "\n";
        return 1;
    }
    // Plain R_X86_64_GOTPCREL (the non-relaxable form, e.g. `pushq
    // foo@GOTPCREL(%rip)`) to a symbol defined in the link still needs a real
    // GOT slot. Synthesize link-time slots; they are filled by ordinary
    // relocation application and never reach the loader.
    if (opts.platform == LinkPlatform::Linux && opts.arch == LinkArch::X86_64) {
        ObjFile slotObj;
        try {
            slotObj = generateStaticGotSlotsX8664(allObjects, dynamicSyms);
        } catch (const std::exception &ex) {
            err << "error: failed to generate static GOT slots: " << ex.what() << "\n";
            return 1;
        }
        if (!slotObj.sections.empty()) {
            const size_t slotObjIdx = allObjects.size();
            allObjects.push_back(std::move(slotObj));
            registerSyntheticSymbols(allObjects[slotObjIdx], slotObjIdx, globalSyms);
        }
    }
    timing.mark("generate-stubs");

    // Step 3.5c: Dead-strip unused sections from all non-synthetic input
    // objects, rooting only entry points and always-live metadata.
    deadStrip(allObjects,
              initialObjects.size(),
              globalSyms,
              opts.entrySymbol,
              opts.platform,
              opts.preserveDebugSections,
              err);
    timing.mark("dead-strip");

    if (!opts.fastLink) {
        // Step 3.5d: Deduplicate identical rodata strings across object files.
        deduplicateStrings(allObjects, globalSyms);
        timing.mark("string-dedup");

        // Step 3.5d2: Fold identical .text sections (Identical Code Folding).
        foldIdenticalCode(allObjects, globalSyms);
        timing.mark("icf");
    }

    // Step 3.5e: Remove global symbols that reference explicitly stripped sections.
    {
        std::vector<std::string> deadSyms;
        for (const auto &[name, entry] : globalSyms) {
            if (entry.binding == GlobalSymEntry::Dynamic)
                continue; // Dynamic symbols have no section reference.
            if (entry.objIndex < allObjects.size() &&
                entry.secIndex < allObjects[entry.objIndex].sections.size() &&
                allObjects[entry.objIndex].sections[entry.secIndex].stripped) {
                deadSyms.push_back(name);
            }
        }
        for (const auto &name : deadSyms)
            globalSyms.erase(name);
    }
    timing.mark("dead-symbol-cleanup");

    // Step 4: Merge sections and compute layout.
    LinkLayout layout;
    layout.globalSyms = std::move(globalSyms);
    if (!mergeSections(allObjects, opts.platform, opts.arch, layout, err)) {
        err << "error: section merging failed\n";
        return 1;
    }
    timing.mark("merge-sections");

    // Step 5: Insert branch trampolines for out-of-range AArch64 B/BL instructions.
    if (!insertBranchTrampolines(allObjects, layout, opts.arch, opts.platform, err)) {
        err << "error: branch trampoline insertion failed\n";
        return 1;
    }
    timing.mark("branch-trampolines");

    // Step 6: Apply relocations. This also resolves final symbol addresses.
    if (!applyRelocations(allObjects, layout, dynamicSyms, opts.platform, opts.arch, err)) {
        err << "error: relocation application failed\n";
        return 1;
    }
    timing.mark("apply-relocations");

    // Step 6.25: Resolve the final entry point after symbol addresses are known.
    {
        auto it = findWithPlatformFallback(layout.globalSyms, opts.entrySymbol, opts.platform);
        if (it == layout.globalSyms.end() || !it->second.resolvedAddrValid) {
            err << "error: entry symbol '" << opts.entrySymbol << "' was not resolved\n";
            return 1;
        }
        layout.entryAddr = it->second.resolvedAddr;
        if (!addressInExecutableSection(layout, layout.entryAddr)) {
            err << "error: entry symbol '" << opts.entrySymbol
                << "' does not resolve to an executable section\n";
            return 1;
        }
    }

    // Step 6.5: Build GOT entry table for the executable writer (needed for bind opcodes).
    for (const auto &[name, entry] : layout.globalSyms) {
        if (name.size() > 6 && name.substr(0, 6) == "__got_") {
            GotEntry ge;
            ge.symbolName = name.substr(6); // Remove "__got_" prefix → original symbol name.
            ge.gotAddr = entry.resolvedAddr;
            layout.gotEntries.push_back(std::move(ge));
        }
    }
    /// @brief Stabilizes loader GOT metadata independently of unordered symbol iteration.
    /// @param a Left GOT entry.
    /// @param b Right GOT entry.
    /// @return `true` when `a` has the lexically earlier symbol name.
    std::sort(layout.gotEntries.begin(),
              layout.gotEntries.end(),
              [](const GotEntry &a, const GotEntry &b) { return a.symbolName < b.symbolName; });
    timing.mark("final-symbols");

    // Step 7: Write executable.
    bool writeOk = false;
    switch (opts.platform) {
        case LinkPlatform::Linux: {
            LinuxImportPlan importPlan;
            if (!planLinuxImports(dynamicSyms, importPlan, err))
                return 1;
            writeOk = writeElfExe(opts.exePath,
                                  layout,
                                  opts.arch,
                                  importPlan.neededLibs,
                                  dynamicSyms,
                                  opts.stackSize,
                                  opts.entrySymbol == "main",
                                  opts.emitLocalSymbols,
                                  err);
            break;
        }
        case LinkPlatform::macOS: {
            MacImportPlan importPlan;
            if (!planMacImports(dynamicSyms, importPlan, err))
                return 1;

            writeOk = writeMachOExe(opts.exePath,
                                    layout,
                                    opts.arch,
                                    importPlan.dylibs,
                                    dynamicSyms,
                                    importPlan.symOrdinals,
                                    opts.stackSize,
                                    opts.emitLocalSymbols,
                                    err);
            break;
        }
        case LinkPlatform::Windows: {
            const bool emitStartupStub = opts.entrySymbol == "main";
            if (emitStartupStub)
                ensurePeImportFunction(peImports, "kernel32.dll", "ExitProcess");
            const uint64_t imageBase = imageBaseForLayout(layout, LinkPlatform::Windows);
            for (const auto &imp : peImports) {
                for (const auto &fn : imp.functions) {
                    auto it = layout.globalSyms.find("__imp_" + fn);
                    if (it != layout.globalSyms.end()) {
                        if (it->second.resolvedAddr < imageBase ||
                            it->second.resolvedAddr - imageBase >
                                std::numeric_limits<uint32_t>::max()) {
                            err << "error: PE import slot RVA for '" << fn
                                << "' is outside 32-bit range\n";
                            return 1;
                        }
                        peImportSlotRvas[fn] =
                            static_cast<uint32_t>(it->second.resolvedAddr - imageBase);
                    }
                }
            }
            writeOk = writePeExe(opts.exePath,
                                 layout,
                                 opts.arch,
                                 peImports,
                                 peImportSlotRvas,
                                 emitStartupStub,
                                 opts.stackSize,
                                 err);
            break;
        }
    }

    if (!writeOk) {
        err << "error: failed to write executable '" << opts.exePath << "'\n";
        return 1;
    }
    timing.mark("write-exe");

    return 0;
}

} // namespace zanna::codegen::linker
