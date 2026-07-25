//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/PeExeWriter.cpp
// Purpose: Write PE32+ executables from a linked layout.
// Key invariants:
//   - Existing section RVAs from SectionMerger are preserved
//   - Generated PE import data lives in a dedicated writable section because
//     the Windows loader writes resolved import addresses into the IAT
//   - Windows x64 gets a tiny startup shim that calls the resolved entry point
//     and then terminates via ExitProcess
//   - ASLR flags are not advertised unless relocations are emitted
// Links: codegen/common/linker/PeExeWriter.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file PeExeWriter.cpp
 * @brief Implements PE32+ headers, sections, imports, TLS, resources, and startup shims.
 */

#include "codegen/common/linker/PeExeWriter.hpp"

#include "codegen/common/linker/AlignUtil.hpp"
#include "codegen/common/linker/ExeWriterUtil.hpp"
#include "codegen/common/objfile/ObjFileWriterUtil.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::linker {

using encoding::padTo;
using encoding::writeLE16;
using encoding::writeLE32;
using encoding::writeLE64;

namespace {

static constexpr uint16_t IMAGE_FILE_MACHINE_AMD64 = 0x8664;
static constexpr uint16_t IMAGE_FILE_MACHINE_ARM64 = 0xAA64;

static constexpr uint16_t kDllCharHighEntropyVA = 0x0020;
static constexpr uint16_t kDllCharDynamicBase = 0x0040;
static constexpr uint16_t kDllCharNXCompat = 0x0100;
static constexpr uint16_t kDllCharTermServerAware = 0x8000;

static constexpr uint16_t kImageRelBasedAbsolute = 0;
static constexpr uint16_t kImageRelBasedDir64 = 10;
static constexpr uint32_t kRelocSectionChars = 0x42000040; // INIT_DATA | DISCARDABLE | READ

/// @brief Owns synthesized import tables and their PE data-directory ranges.
struct ImportLayout {
    std::vector<uint8_t> data;
    uint32_t importDirRva = 0;
    uint32_t importDirSize = 0;
    uint32_t iatRva = 0;
    uint32_t iatSize = 0;
    std::unordered_map<std::string, uint32_t> slotRvas;
    std::vector<std::pair<uint32_t, uint64_t>> slotInitializers;
};

/// @brief Append a fixed-width PE/COFF section-name field.
/// @details PE section headers carry exactly eight name bytes. This helper
///          emits a zero-padded field with explicit bounds so overlong names are
///          truncated intentionally and short names never depend on strncpy's
///          C-string termination behavior.
/// @param file Destination executable byte buffer.
/// @param name Section name to serialize.
/// @param width Required field width in bytes.
void appendFixedNameField(std::vector<uint8_t> &file, std::string_view name, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        const uint8_t byte = i < name.size() ? static_cast<uint8_t>(name[i]) : 0;
        file.push_back(byte);
    }
}

/// @brief Owns a synthesized PE TLS directory and callback terminator.
struct TlsLayout {
    std::vector<uint8_t> data;
    uint32_t directoryRva = 0;
    uint32_t directorySize = 0;
};

/// @brief Describes the published exception-function table range.
struct ExceptionLayout {
    uint32_t directoryRva = 0;
    uint32_t directorySize = 0;
};

/// @brief Owns base-relocation blocks and their data-directory range.
struct BaseRelocLayout {
    std::vector<uint8_t> data;
    uint32_t directoryRva = 0;
    uint32_t directorySize = 0;
};

/// @brief Owns the default manifest resource tree and directory range.
struct ResourceLayout {
    std::vector<uint8_t> data;
    uint32_t directoryRva = 0;
    uint32_t directorySize = 0;
};

/// @brief Planned PE section header plus a non-owning data-buffer reference.
struct PeSection {
    std::string name;
    const std::vector<uint8_t> *data = nullptr;
    uint32_t virtualAddress = 0;
    uint32_t virtualSize = 0;
    uint32_t sizeOfRawData = 0;
    uint32_t pointerToRawData = 0;
    uint32_t characteristics = 0;
    bool alloc = true;
    bool executable = false;
    bool writable = false;
    bool zeroFill = false;
};

using zanna::codegen::objfile::putLE16;
using zanna::codegen::objfile::putLE32;
using zanna::codegen::objfile::putLE64;

/// @brief Append a little-endian uint16 to @p buf.
/// @param buf Destination byte buffer.
/// @param val Value to append.
void appendLE16(std::vector<uint8_t> &buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

/// @brief Append a little-endian uint32 to @p buf.
/// @param buf Destination byte buffer.
/// @param val Value to append.
void appendLE32(std::vector<uint8_t> &buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

/// @brief Build the IMAGE_SCN_* characteristics word for a PE section.
/// @details Picks code/data/rwdata/discardable bits based on the section's
///          allocate/exec/write/zerofill attributes. Discardable applies to
///          non-allocatable sections (.debug_*) so the loader can skip them.
/// @param executable Whether code execution is allowed.
/// @param writable Whether writes are allowed.
/// @param alloc Whether the section belongs in the memory image.
/// @param zeroFill Whether storage is uninitialized fileless data.
/// @return Combined `IMAGE_SCN_*` characteristics.
uint32_t sectionChars(bool executable, bool writable, bool alloc, bool zeroFill = false) {
    if (!alloc)
        return 0x42000040; // CNT_INITIALIZED_DATA | MEM_DISCARDABLE | MEM_READ
    if (executable)
        return 0x60000020; // CNT_CODE | MEM_EXECUTE | MEM_READ
    if (zeroFill)
        return 0xC0000080; // CNT_UNINITIALIZED_DATA | MEM_READ | MEM_WRITE
    if (writable)
        return 0xC0000040; // CNT_INITIALIZED_DATA | MEM_READ | MEM_WRITE
    return 0x40000040;     // CNT_INITIALIZED_DATA | MEM_READ
}

/// @brief Build the .idata import directory blob from a DLL import list.
/// @details Materialises the IDT, ILT, hint/name table, DLL name table, and
///          (when @p externalSlotRvas is empty) a fresh IAT into a single
///          contiguous buffer. When @p externalSlotRvas is provided each
///          import binds to a pre-allocated slot in another section instead,
///          and the writer later patches each slot to the hint-name RVA via
///          @c slotInitializers.
/// @param imports          DLL import groups to serialise.
/// @param sectionRva       RVA at which the resulting buffer will be placed.
/// @param externalSlotRvas Optional symbol→RVA map for IAT slot reuse.
/// @param result Receives the blob, IDT/IAT ranges, and external-slot initializers.
/// @param err Diagnostic output stream.
/// @return `true` on success.
bool buildImportTables(const std::vector<DllImport> &imports,
                       uint32_t sectionRva,
                       const std::unordered_map<std::string, uint32_t> &externalSlotRvas,
                       ImportLayout &result,
                       std::ostream &err) {
    result = ImportLayout{};
    if (imports.empty())
        return true;

    /// Narrows an import-table value with a contextual diagnostic.
    auto checkedLocalU32 = [&](uint64_t value, const char *what, uint32_t &out) -> bool {
        if (value > std::numeric_limits<uint32_t>::max()) {
            err << "error: PE import table " << what << " exceeds 32-bit file format limit\n";
            return false;
        }
        out = static_cast<uint32_t>(value);
        return true;
    };
    /// Adds import-table offsets with overflow checking.
    auto checkedAddLocal = [&](uint32_t lhs, uint32_t rhs, const char *what, uint32_t &out) {
        if (lhs > std::numeric_limits<uint32_t>::max() - rhs) {
            err << "error: PE import table " << what << " overflows 32-bit file format limit\n";
            return false;
        }
        out = lhs + rhs;
        return true;
    };
    /// Aligns an import-table offset and converts exceptions to diagnostics.
    auto checkedAlignLocal =
        [&](uint32_t value, uint32_t alignment, const char *what, uint32_t &out) {
            size_t aligned = 0;
            try {
                aligned = alignUp(static_cast<size_t>(value), alignment);
            } catch (const std::exception &ex) {
                err << "error: PE import table " << what << " alignment failed: " << ex.what()
                    << "\n";
                return false;
            }
            return checkedLocalU32(aligned, what, out);
        };
    /// Computes bytes for @p count entries plus a terminating record.
    auto checkedCountBytes = [&](size_t count, uint32_t elemSize, const char *what, uint32_t &out) {
        const uint64_t count64 = static_cast<uint64_t>(count);
        if (count64 > (std::numeric_limits<uint64_t>::max() / elemSize) - 1) {
            err << "error: PE import table " << what << " overflows addressable size\n";
            return false;
        }
        return checkedLocalU32((count64 + 1) * elemSize, what, out);
    };

    uint32_t idtSize = 0;
    if (!checkedCountBytes(imports.size(), 20, "directory size", idtSize))
        return false;
    uint32_t iltSize = 0;
    uint32_t hintNameSize = 0;
    uint32_t dllNameSize = 0;
    bool useExternalSlots = !externalSlotRvas.empty();

    for (const auto &imp : imports) {
        uint32_t importIltSize = 0;
        if (!checkedCountBytes(imp.functions.size(), 8, "lookup-table size", importIltSize) ||
            !checkedAddLocal(iltSize, importIltSize, "lookup-table size", iltSize))
            return false;
        uint32_t dllNameBytes = 0;
        if (imp.dllName.size() == std::numeric_limits<size_t>::max() ||
            !checkedLocalU32(imp.dllName.size() + 1, "DLL-name table size", dllNameBytes) ||
            !checkedAddLocal(dllNameSize, dllNameBytes, "DLL-name table size", dllNameSize))
            return false;
        for (const auto &fn : imp.functions) {
            auto importIt = imp.importNames.find(fn);
            const std::string &importName =
                (importIt != imp.importNames.end()) ? importIt->second : fn;
            if (importName.size() > std::numeric_limits<uint32_t>::max() - 3) {
                err << "error: PE import table hint-name entry exceeds 32-bit file format limit\n";
                return false;
            }
            uint32_t rawHintSize = static_cast<uint32_t>(2 + importName.size() + 1);
            uint32_t alignedHintSize = 0;
            if (!checkedAlignLocal(rawHintSize, 2, "hint-name table size", alignedHintSize) ||
                !checkedAddLocal(
                    hintNameSize, alignedHintSize, "hint-name table size", hintNameSize))
                return false;
        }
    }

    uint32_t idtOff = 0;
    uint32_t iltOff = 0;
    uint32_t hintOff = 0;
    uint32_t dllNameOff = 0;
    if (!checkedAddLocal(idtOff, idtSize, "lookup-table offset", iltOff) ||
        !checkedAddLocal(iltOff, iltSize, "hint-name table offset", hintOff) ||
        !checkedAddLocal(hintOff, hintNameSize, "DLL-name table offset", dllNameOff))
        return false;
    uint32_t iatOff = 0;
    uint32_t iatSize = 0;
    uint32_t totalRawSize = 0;
    uint32_t totalSize = 0;
    if (!checkedAddLocal(dllNameOff, dllNameSize, "total size", totalRawSize) ||
        !checkedAlignLocal(totalRawSize, 8, "total size", totalSize))
        return false;
    if (!useExternalSlots) {
        iatOff = totalSize;
        iatSize = iltSize;
        if (!checkedAddLocal(iatOff, iatSize, "total size", totalSize))
            return false;
    }

    result.data.resize(totalSize, 0);

    uint32_t curIltOff = iltOff;
    uint32_t curHintOff = hintOff;
    uint32_t curDllNameOff = dllNameOff;
    uint32_t curIatOff = iatOff;
    uint32_t minIatRva = UINT32_MAX;
    uint32_t maxIatEndRva = 0;

    for (size_t i = 0; i < imports.size(); ++i) {
        const auto &imp = imports[i];
        uint32_t idtEntryDelta = 0;
        uint32_t idtEntryOff = 0;
        if (!checkedLocalU32(
                static_cast<uint64_t>(i) * 20ULL, "directory entry offset", idtEntryDelta) ||
            !checkedAddLocal(idtOff, idtEntryDelta, "directory entry offset", idtEntryOff))
            return false;
        uint32_t firstThunkRva = 0;
        if (useExternalSlots) {
            if (!imp.functions.empty()) {
                const auto firstIt = externalSlotRvas.find(imp.functions.front());
                if (firstIt != externalSlotRvas.end())
                    firstThunkRva = firstIt->second;
            }
        } else {
            if (!checkedAddLocal(sectionRva, curIatOff, "IAT RVA", firstThunkRva))
                return false;
        }

        uint32_t iltRva = 0;
        uint32_t dllNameRva = 0;
        if (!checkedAddLocal(sectionRva, curIltOff, "lookup-table RVA", iltRva) ||
            !checkedAddLocal(sectionRva, curDllNameOff, "DLL-name RVA", dllNameRva))
            return false;
        putLE32(result.data, idtEntryOff + 0, iltRva);
        putLE32(result.data, idtEntryOff + 4, 0);
        putLE32(result.data, idtEntryOff + 8, 0);
        putLE32(result.data, idtEntryOff + 12, dllNameRva);
        putLE32(result.data, idtEntryOff + 16, firstThunkRva);

        std::memcpy(
            result.data.data() + curDllNameOff, imp.dllName.c_str(), imp.dllName.size() + 1);
        uint32_t dllNameBytes = 0;
        if (!checkedLocalU32(imp.dllName.size() + 1, "DLL-name table cursor", dllNameBytes) ||
            !checkedAddLocal(curDllNameOff, dllNameBytes, "DLL-name table cursor", curDllNameOff))
            return false;

        for (const auto &fn : imp.functions) {
            auto importIt = imp.importNames.find(fn);
            const std::string &importName =
                (importIt != imp.importNames.end()) ? importIt->second : fn;
            uint32_t hintNameRva = 0;
            if (!checkedAddLocal(sectionRva, curHintOff, "hint-name RVA", hintNameRva))
                return false;
            putLE16(result.data, curHintOff, 0);
            std::memcpy(
                result.data.data() + curHintOff + 2, importName.c_str(), importName.size() + 1);
            uint32_t rawHintSize = static_cast<uint32_t>(2 + importName.size() + 1);
            uint32_t alignedHintSize = 0;
            if (!checkedAlignLocal(rawHintSize, 2, "hint-name table cursor", alignedHintSize) ||
                !checkedAddLocal(curHintOff, alignedHintSize, "hint-name table cursor", curHintOff))
                return false;

            putLE32(result.data, curIltOff, hintNameRva);
            putLE32(result.data, curIltOff + 4, 0);
            if (!checkedAddLocal(curIltOff, 8, "lookup-table cursor", curIltOff))
                return false;

            if (useExternalSlots) {
                const auto slotIt = externalSlotRvas.find(fn);
                if (slotIt != externalSlotRvas.end()) {
                    result.slotRvas[fn] = slotIt->second;
                    result.slotInitializers.push_back({slotIt->second, hintNameRva});
                    minIatRva = std::min(minIatRva, slotIt->second);
                    uint32_t slotEnd = 0;
                    if (!checkedAddLocal(slotIt->second, 8, "external IAT slot range", slotEnd))
                        return false;
                    maxIatEndRva = std::max(maxIatEndRva, slotEnd);
                }
            } else {
                putLE32(result.data, curIatOff, hintNameRva);
                putLE32(result.data, curIatOff + 4, 0);
                uint32_t slotRva = 0;
                if (!checkedAddLocal(sectionRva, curIatOff, "IAT slot RVA", slotRva) ||
                    !checkedAddLocal(curIatOff, 8, "IAT cursor", curIatOff))
                    return false;
                result.slotRvas[fn] = slotRva;
            }
        }

        if (!checkedAddLocal(curIltOff, 8, "lookup-table null terminator", curIltOff))
            return false;
        if (!useExternalSlots) {
            if (!checkedAddLocal(curIatOff, 8, "IAT null terminator", curIatOff))
                return false;
        } else if (firstThunkRva != 0) {
            minIatRva = std::min(minIatRva, firstThunkRva);
            uint32_t thunkSpan = 0;
            uint32_t thunkEnd = 0;
            if (!checkedCountBytes(imp.functions.size(), 8, "external IAT range", thunkSpan) ||
                !checkedAddLocal(firstThunkRva, thunkSpan, "external IAT range", thunkEnd))
                return false;
            maxIatEndRva = std::max(maxIatEndRva, thunkEnd);
        }
    }

    if (!checkedAddLocal(sectionRva, idtOff, "directory RVA", result.importDirRva))
        return false;
    result.importDirSize = idtSize;
    if (useExternalSlots) {
        if (minIatRva != UINT32_MAX) {
            result.iatRva = minIatRva;
            result.iatSize = maxIatEndRva - minIatRva;
        }
    } else {
        if (!checkedAddLocal(sectionRva, iatOff, "IAT RVA", result.iatRva))
            return false;
        result.iatSize = iatSize;
    }
    return true;
}

/// @brief Test whether @p value fits in a signed 32-bit field (PE PC-rel reach).
/// @param value Signed displacement.
/// @return `true` when representable as `int32_t`.
bool fitsInt32(int64_t value) {
    return value >= static_cast<int64_t>(INT32_MIN) && value <= static_cast<int64_t>(INT32_MAX);
}

/// @brief Test whether @p value starts with the C-string @p prefix.
/// @param value String to inspect.
/// @param prefix NUL-terminated prefix.
/// @return `true` on a prefix match.
bool hasPrefix(const std::string &value, const char *prefix) {
    return value.rfind(prefix, 0) == 0;
}

/// @brief Narrows a PE format field with diagnostics.
/// @param value Value to narrow.
/// @param what Field description.
/// @param err Diagnostic output stream.
/// @param out Receives the value.
/// @return `true` when representable.
bool checkedU32(uint64_t value, const char *what, std::ostream &err, uint32_t &out) {
    if (value > std::numeric_limits<uint32_t>::max()) {
        err << "error: PE " << what << " exceeds 32-bit file format limit\n";
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

/// @brief Narrows a host size to a PE 32-bit field.
/// @param value Host size.
/// @param what Field description.
/// @param err Diagnostic output stream.
/// @param out Receives the value.
/// @return `true` when representable.
bool checkedSizeU32(size_t value, const char *what, std::ostream &err, uint32_t &out) {
    if (value > std::numeric_limits<uint32_t>::max()) {
        err << "error: PE " << what << " exceeds 32-bit file format limit\n";
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

/// @brief Adds two PE 32-bit offsets with diagnostics.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param what Operation description.
/// @param err Diagnostic output stream.
/// @param out Receives the sum.
/// @return `true` on success.
bool checkedAddU32(uint32_t lhs, uint32_t rhs, const char *what, std::ostream &err, uint32_t &out) {
    if (lhs > std::numeric_limits<uint32_t>::max() - rhs) {
        err << "error: PE " << what << " overflows 32-bit file format limit\n";
        return false;
    }
    out = lhs + rhs;
    return true;
}

/// @brief Converts an absolute virtual address to a checked 32-bit RVA.
/// @param imageBase PE image base.
/// @param virtualAddr Absolute address.
/// @param what Section description.
/// @param err Diagnostic output stream.
/// @param out Receives the RVA.
/// @return `true` when the address is at/above the base and within range.
bool checkedRva(uint64_t imageBase,
                uint64_t virtualAddr,
                const std::string &what,
                std::ostream &err,
                uint32_t &out) {
    if (virtualAddr < imageBase) {
        err << "error: PE section '" << what << "' is below the image base\n";
        return false;
    }
    return checkedU32(virtualAddr - imageBase, "RVA", err, out);
}

/// @brief Return the PE image base selected by the link layout.
/// @details SectionMerger normally seeds LinkLayout::imageBase before assigning
///          virtual addresses. Hand-built tests may leave it zero; in that case
///          the writer keeps the historical Windows default.
/// @param layout Link layout.
/// @return Explicit layout base or the conventional Windows default.
uint64_t peImageBaseForLayout(const LinkLayout &layout) {
    return layout.imageBase != 0 ? layout.imageBase
                                 : defaultImageBaseForPlatform(LinkPlatform::Windows);
}

/// @brief Aligns a value and narrows it to a PE 32-bit field.
/// @param value Value to align.
/// @param alignment Required alignment.
/// @param what Field description.
/// @param err Diagnostic output stream.
/// @param out Receives the aligned value.
/// @return `true` when valid and representable.
bool checkedAlignUpU32(
    uint64_t value, uint32_t alignment, const char *what, std::ostream &err, uint32_t &out) {
    if (value > std::numeric_limits<size_t>::max()) {
        err << "error: PE " << what << " exceeds addressable size\n";
        return false;
    }
    size_t aligned = 0;
    try {
        aligned = alignUp(static_cast<size_t>(value), alignment);
    } catch (const std::exception &ex) {
        err << "error: PE " << what << " alignment failed: " << ex.what() << "\n";
        return false;
    }
    return checkedSizeU32(aligned, what, err, out);
}

/// @brief Encode a power-of-two alignment as the COFF section-alignment bit field.
/// @details COFF stores alignment in bits 20-23 of Characteristics. A value of
///          1 means 1-byte (no alignment), 2 means 2-byte, etc. up to 8192.
/// @param alignment Required byte alignment.
/// @return Encoded characteristics bits.
uint32_t encodeCoffAlignment(uint32_t alignment) {
    if (alignment <= 1)
        return 0;

    uint32_t power = 1;
    uint32_t bits = 1;
    while (power < alignment && bits < 0xF) {
        power <<= 1;
        ++bits;
    }
    return bits << 20;
}

/// @brief Detect whether the layout contains any TLS-section data.
/// @details Used to gate emission of the IMAGE_DIRECTORY_ENTRY_TLS data directory.
/// @param layout Final merged layout.
/// @return `true` for at least one nonempty allocated TLS section.
bool layoutHasTls(const LinkLayout &layout) {
    for (const auto &sec : layout.sections) {
        if (sec.alloc && sec.tls && !sec.data.empty())
            return true;
    }
    return false;
}

/// @brief Synthesise a Windows x64 _start shim that calls main and ExitProcess.
/// @details The shim runs *before* main, aligns RSP, calls the resolved entry
///          point, then tail-jumps through the IAT slot for kernel32!ExitProcess
///          with main's return value as the exit code. Must reach both
///          @p entryAddr and @p exitProcessIatRva via 32-bit PC-relative
///          displacement, which the caller checks with @c fitsInt32.
/// @param imageBase PE image base.
/// @param entryAddr Absolute address of `main`.
/// @param stubRva RVA assigned to the shim.
/// @param exitProcessIatRva RVA of the `ExitProcess` IAT slot.
/// @param err Diagnostic output stream.
/// @return Encoded shim, or an empty vector when either target is unreachable.
std::vector<uint8_t> buildX64StartupStub(uint64_t imageBase,
                                         uint64_t entryAddr,
                                         uint32_t stubRva,
                                         uint32_t exitProcessIatRva,
                                         std::ostream &err) {
    std::vector<uint8_t> stub = {
        0x48,
        0x83,
        0xEC,
        0x28, // sub rsp, 40
        0xE8,
        0x00,
        0x00,
        0x00,
        0x00, // call entry
        0x89,
        0xC1, // mov ecx, eax
        0xFF,
        0x15,
        0x00,
        0x00,
        0x00,
        0x00, // call [rip+disp32] ; ExitProcess
        0xCC, // int3 if ExitProcess returns
    };

    const uint64_t stubVa = imageBase + stubRva;
    const int64_t callDisp = static_cast<int64_t>(entryAddr) - static_cast<int64_t>(stubVa + 9);
    const int64_t iatDisp =
        static_cast<int64_t>(exitProcessIatRva) - static_cast<int64_t>(stubRva + 17);

    if (!fitsInt32(callDisp)) {
        err << "error: PE startup stub cannot reach entry point\n";
        return {};
    }
    if (!fitsInt32(iatDisp)) {
        err << "error: PE startup stub cannot reach ExitProcess IAT slot\n";
        return {};
    }

    putLE32(stub, 5, static_cast<uint32_t>(static_cast<int32_t>(callDisp)));
    putLE32(stub, 13, static_cast<uint32_t>(static_cast<int32_t>(iatDisp)));
    return stub;
}

/// @brief Synthesizes an AArch64 entry shim that calls `main` then branches to ExitProcess.
/// @param imageBase PE image base.
/// @param entryAddr Absolute address of `main`.
/// @param stubRva RVA assigned to the shim.
/// @param exitProcessIatRva RVA of the `ExitProcess` IAT slot.
/// @param err Diagnostic output stream.
/// @return Encoded shim, or an empty vector when an ADRP target is unreachable.
std::vector<uint8_t> buildAArch64StartupStub(uint64_t imageBase,
                                             uint64_t entryAddr,
                                             uint32_t stubRva,
                                             uint32_t exitProcessIatRva,
                                             std::ostream &err) {
    std::vector<uint8_t> stub = {
        0x10, 0x00, 0x00, 0x90, // adrp x16, entry
        0x10, 0x02, 0x00, 0x91, // add  x16, x16, #entry_lo12
        0x00, 0x02, 0x3F, 0xD6, // blr  x16
        0x10, 0x00, 0x00, 0x90, // adrp x16, __imp_ExitProcess
        0x10, 0x02, 0x40, 0xF9, // ldr  x16, [x16, #iat_lo12]
        0x00, 0x02, 0x1F, 0xD6, // br   x16
        0x00, 0x00, 0x20, 0xD4, // brk  #0
    };

    /// Patches one ADRP to the page containing an absolute target.
    auto patchAdrp = [&](size_t offset, uint64_t targetVa) {
        const uint64_t pcVa = imageBase + stubRva + static_cast<uint32_t>(offset);
        const uint64_t pageTarget = targetVa & ~0xFFFULL;
        const uint64_t pagePc = pcVa & ~0xFFFULL;
        const int64_t pageDelta = static_cast<int64_t>(pageTarget) - static_cast<int64_t>(pagePc);
        const int64_t immHiLo = pageDelta >> 12;
        if (immHiLo > 0xFFFFF || immHiLo < -0x100000) {
            err << "error: PE ARM64 startup stub ADRP target out of range\n";
            return false;
        }

        uint32_t insn = 0x90000010U;
        const uint32_t immlo = static_cast<uint32_t>(immHiLo) & 0x3;
        const uint32_t immhi = (static_cast<uint32_t>(immHiLo) >> 2) & 0x7FFFF;
        insn = (insn & 0x9F00001F) | (immlo << 29) | (immhi << 5);
        putLE32(stub, offset, insn);
        return true;
    };

    /// Patches an ADD immediate with a target's low twelve address bits.
    auto patchAddLo12 = [&](size_t offset, uint64_t targetVa) {
        const uint32_t pageOff = static_cast<uint32_t>(targetVa & 0xFFFULL);
        uint32_t insn = 0x91000210U;
        insn = (insn & 0xFFC003FF) | (pageOff << 10);
        putLE32(stub, offset, insn);
    };

    /// Patches a 64-bit LDR scaled offset with a target's page offset.
    auto patchLdrLo12 = [&](size_t offset, uint64_t targetVa) {
        const uint32_t pageOff = static_cast<uint32_t>((targetVa & 0xFFFULL) >> 3);
        uint32_t insn = 0xF9400210U;
        insn = (insn & 0xFFC003FF) | (pageOff << 10);
        putLE32(stub, offset, insn);
    };

    if (!patchAdrp(0, entryAddr))
        return {};
    patchAddLo12(4, entryAddr);

    const uint64_t exitProcessIatVa = imageBase + static_cast<uint64_t>(exitProcessIatRva);
    if (!patchAdrp(12, exitProcessIatVa))
        return {};
    patchLdrLo12(16, exitProcessIatVa);
    return stub;
}

/// @brief Maps a neutral output section to its canonical short PE name.
/// @param sec Output section and attributes.
/// @return `.tls`, `.bss`, metadata name, `.text`, `.data`, or `.rdata`.
std::string sectionNameFor(const OutputSection &sec) {
    if (sec.tls)
        return ".tls";
    if (sec.zeroFill)
        return ".bss";
    if (hasPrefix(sec.name, ".pdata"))
        return ".pdata";
    if (hasPrefix(sec.name, ".xdata"))
        return ".xdata";
    if (sec.executable)
        return ".text";
    if (sec.writable)
        return ".data";
    return ".rdata";
}

/// @brief Builds the PE TLS directory spanning all allocated TLS sections.
/// @param imageBase PE image base.
/// @param layout Final sections and `_tls_index` symbol.
/// @param sectionRva RVA where the synthesized directory will be placed.
/// @param err Diagnostic output stream.
/// @return TLS bytes/range, or an empty layout when absent or invalid.
TlsLayout buildTlsDirectory(uint64_t imageBase,
                            const LinkLayout &layout,
                            uint32_t sectionRva,
                            std::ostream &err) {
    TlsLayout tls{};

    uint32_t tlsStartRva = UINT32_MAX;
    uint32_t tlsRawEndRva = 0;
    uint32_t tlsMemEndRva = 0;
    uint32_t tlsAlignment = 1;
    bool sawRawTemplateBytes = false;
    for (const auto &sec : layout.sections) {
        const size_t secMemSize = outputSectionMemSize(sec);
        if (!sec.alloc || !sec.tls || secMemSize == 0)
            continue;
        uint32_t secRva = 0;
        if (!checkedRva(imageBase, sec.virtualAddr, sec.name, err, secRva))
            return {};
        uint32_t secSize = 0;
        if (!checkedSizeU32(secMemSize, "TLS section size", err, secSize))
            return {};
        uint32_t secEnd = 0;
        if (!checkedAddU32(secRva, secSize, "TLS section RVA range", err, secEnd))
            return {};
        tlsStartRva = std::min(tlsStartRva, secRva);
        tlsMemEndRva = std::max(tlsMemEndRva, secEnd);
        if (!sec.zeroFill) {
            sawRawTemplateBytes = true;
            tlsRawEndRva = std::max(tlsRawEndRva, secEnd);
        }
        tlsAlignment = std::max(tlsAlignment, sec.alignment);
    }

    if (tlsStartRva == UINT32_MAX)
        return tls;

    const auto tlsIndexIt = layout.globalSyms.find("_tls_index");
    if (tlsIndexIt == layout.globalSyms.end() || tlsIndexIt->second.resolvedAddr == 0) {
        err << "error: PE TLS image is missing a resolved _tls_index symbol\n";
        return {};
    }

    const uint32_t tlsEndRva = sawRawTemplateBytes ? tlsRawEndRva : tlsStartRva;
    const uint32_t tlsZeroFill = tlsMemEndRva > tlsEndRva ? tlsMemEndRva - tlsEndRva : 0;
    uint32_t callbackArrayRva = 0;
    if (!checkedAddU32(sectionRva, 40, "TLS callback-array RVA", err, callbackArrayRva))
        return {};

    tls.data.resize(48, 0);
    putLE64(tls.data, 0, imageBase + tlsStartRva);
    putLE64(tls.data, 8, imageBase + tlsEndRva);
    putLE64(tls.data, 16, tlsIndexIt->second.resolvedAddr);
    putLE64(tls.data, 24, imageBase + callbackArrayRva); // Null-terminated callback array.
    putLE32(tls.data, 32, tlsZeroFill);
    putLE32(tls.data, 36, encodeCoffAlignment(tlsAlignment));
    tls.directoryRva = sectionRva;
    tls.directorySize = 40;
    return tls;
}

/// @brief Locates and sizes the `.pdata` exception-function table.
/// @param sections Planned PE sections.
/// @param arch Target architecture controlling record width.
/// @return Directory range trimmed to a whole number of runtime-function records.
ExceptionLayout findExceptionDirectory(const std::vector<PeSection> &sections, LinkArch arch) {
    // RUNTIME_FUNCTION is 12 bytes on x64 (begin/end/unwind RVAs) and 8 bytes on
    // ARM64 (function RVA + xdata/packed word). RtlLookupFunctionEntry divides the
    // directory size by this record size, so the published size must be an exact
    // multiple — trim any section alignment padding tail instead of exposing a
    // partial, garbage trailing entry.
    const uint32_t recordSize = (arch == LinkArch::AArch64) ? 8u : 12u;
    ExceptionLayout result{};
    for (const auto &sec : sections) {
        if (sec.alloc && sec.name == ".pdata") {
            result.directoryRva = sec.virtualAddress;
            result.directorySize = (sec.virtualSize / recordSize) * recordSize;
            break;
        }
    }
    return result;
}

/// @brief Encodes page-grouped `IMAGE_REL_BASED_DIR64` relocation blocks.
/// @param rvas Absolute-pointer RVAs, accepted in any order.
/// @param forceNonEmpty Whether to emit an empty eight-byte block when no RVAs exist.
/// @return Deduplicated, sorted base-relocation section bytes.
std::vector<uint8_t> buildBaseRelocationBlocks(std::vector<uint32_t> rvas, bool forceNonEmpty) {
    std::sort(rvas.begin(), rvas.end());
    rvas.erase(std::unique(rvas.begin(), rvas.end()), rvas.end());

    std::vector<uint8_t> data;
    if (rvas.empty()) {
        if (forceNonEmpty) {
            appendLE32(data, 0);
            appendLE32(data, 8);
        }
        return data;
    }

    size_t i = 0;
    while (i < rvas.size()) {
        const uint32_t pageRva = rvas[i] & ~0xFFFu;
        std::vector<uint16_t> entries;
        while (i < rvas.size() && (rvas[i] & ~0xFFFu) == pageRva) {
            entries.push_back(
                static_cast<uint16_t>((kImageRelBasedDir64 << 12) | (rvas[i] & 0x0FFFu)));
            ++i;
        }
        if ((entries.size() & 1u) != 0)
            entries.push_back(static_cast<uint16_t>(kImageRelBasedAbsolute << 12));

        appendLE32(data, pageRva);
        appendLE32(data, static_cast<uint32_t>(8 + entries.size() * sizeof(uint16_t)));
        for (uint16_t entry : entries)
            appendLE16(data, entry);
    }
    return data;
}

/// @brief Builds a three-level resource tree containing an as-invoker UTF-8 manifest.
/// @param sectionRva RVA assigned to the resource section.
/// @return Resource bytes and data-directory range.
ResourceLayout buildDefaultManifestResource(uint32_t sectionRva) {
    static constexpr uint16_t kRtManifest = 24;
    static constexpr uint16_t kExeManifestId = 1;
    static constexpr uint16_t kLangEnUs = 0x0409;

    static const char kManifest[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
        "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">\n"
        "    <security><requestedPrivileges>\n"
        "      <requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/>\n"
        "    </requestedPrivileges></security>\n"
        "  </trustInfo>\n"
        "</assembly>\n";

    ResourceLayout result{};

    const uint32_t rootDirOff = 0;
    const uint32_t rootEntryOff = rootDirOff + 16;
    const uint32_t typeDirOff = rootEntryOff + 8;
    const uint32_t typeEntryOff = typeDirOff + 16;
    const uint32_t nameDirOff = typeEntryOff + 8;
    const uint32_t langEntryOff = nameDirOff + 16;
    const uint32_t dataEntryOff = langEntryOff + 8;
    const uint32_t manifestOff = static_cast<uint32_t>(alignUp(dataEntryOff + 16, 4));
    const uint32_t manifestSize = static_cast<uint32_t>(sizeof(kManifest) - 1);
    const uint32_t totalSize = static_cast<uint32_t>(alignUp(manifestOff + manifestSize, 4));

    std::vector<uint8_t> data(totalSize, 0);
    /// Initializes one resource-directory header with ID-only children.
    auto putDir = [&](uint32_t off, uint16_t idEntryCount) {
        putLE16(data, off + 12, 0);            // NumberOfNamedEntries
        putLE16(data, off + 14, idEntryCount); // NumberOfIdEntries
    };

    putDir(rootDirOff, 1);
    putLE32(data, rootEntryOff + 0, kRtManifest);
    putLE32(data, rootEntryOff + 4, 0x80000000u | typeDirOff);

    putDir(typeDirOff, 1);
    putLE32(data, typeEntryOff + 0, kExeManifestId);
    putLE32(data, typeEntryOff + 4, 0x80000000u | nameDirOff);

    putDir(nameDirOff, 1);
    putLE32(data, langEntryOff + 0, kLangEnUs);
    putLE32(data, langEntryOff + 4, dataEntryOff);

    putLE32(data, dataEntryOff + 0, sectionRva + manifestOff);
    putLE32(data, dataEntryOff + 4, manifestSize);
    putLE32(data, dataEntryOff + 8, 65001); // UTF-8
    std::memcpy(data.data() + manifestOff, kManifest, manifestSize);

    result.data = std::move(data);
    result.directoryRva = sectionRva;
    result.directorySize = totalSize;
    return result;
}

} // namespace

/// @copydoc writePeExe(const std::string &, const LinkLayout &, LinkArch, const std::vector<DllImport> &, const std::unordered_map<std::string, uint32_t> &, bool, std::size_t, std::ostream &)
bool writePeExe(const std::string &path,
                const LinkLayout &layout,
                LinkArch arch,
                const std::vector<DllImport> &imports,
                const std::unordered_map<std::string, uint32_t> &slotRvas,
                bool emitStartupStub,
                std::size_t stackSize,
                std::ostream &err) {
    const uint16_t machine =
        (arch == LinkArch::AArch64) ? IMAGE_FILE_MACHINE_ARM64 : IMAGE_FILE_MACHINE_AMD64;
    const uint64_t imageBase = peImageBaseForLayout(layout);
    const uint32_t sectionAlignment = 0x1000;
    const uint32_t fileAlignment = 0x200;
    const uint64_t stackReserve =
        stackSize != 0 ? static_cast<uint64_t>(alignUp(stackSize, static_cast<std::size_t>(0x1000)))
                       : 0x100000ULL;
    const uint64_t stackCommit = std::min<uint64_t>(stackReserve, 0x1000ULL);

    if (layout.entryAddr == 0) {
        err << "error: no PE entry point was resolved\n";
        return false;
    }

    std::vector<std::vector<uint8_t>> ownedSectionData;
    ownedSectionData.reserve(layout.sections.size());

    std::vector<PeSection> sections;
    sections.reserve(layout.sections.size() + 4);

    uint32_t nextGeneratedRva = sectionAlignment;
    for (const auto &sec : layout.sections) {
        // PE images cannot carry ELF/Mach-O style non-alloc debug sections.
        // Emitting them with RVA 0 makes Windows reject the executable.
        if (!sec.alloc)
            continue;
        if (outputSectionMemSize(sec) == 0)
            continue;

        PeSection ps;
        ps.name = sectionNameFor(sec);
        if (ps.name.size() > 8) {
            err << "error: PE section name '" << ps.name << "' exceeds 8 bytes\n";
            return false;
        }
        ownedSectionData.push_back(sec.data);
        ps.data = &ownedSectionData.back();
        ps.alloc = sec.alloc;
        ps.executable = sec.executable;
        ps.writable = sec.writable;
        ps.zeroFill = sec.zeroFill;
        if (!checkedRva(imageBase, sec.virtualAddr, sec.name, err, ps.virtualAddress))
            return false;
        if (!checkedSizeU32(outputSectionMemSize(sec), "section virtual size", err, ps.virtualSize))
            return false;
        ps.characteristics = sectionChars(sec.executable, sec.writable, sec.alloc, sec.zeroFill);
        sections.push_back(ps);

        if (sec.alloc) {
            uint32_t alignedVirtualSize = 0;
            if (!checkedAlignUpU32(ps.virtualSize,
                                   sectionAlignment,
                                   "section virtual size",
                                   err,
                                   alignedVirtualSize))
                return false;
            uint32_t end = 0;
            if (!checkedAddU32(
                    ps.virtualAddress, alignedVirtualSize, "section RVA range", err, end))
                return false;
            nextGeneratedRva = std::max(nextGeneratedRva, end);
        }
    }

    std::vector<uint8_t> generatedImportData;
    std::vector<uint8_t> generatedStubData;
    std::vector<uint8_t> generatedTlsData;
    std::vector<uint8_t> generatedBaseRelocData;
    std::vector<uint8_t> generatedResourceData;
    ImportLayout importLayout{};
    TlsLayout tlsLayout{};
    BaseRelocLayout baseRelocLayout{};
    ResourceLayout resourceLayout{};

    if (!imports.empty()) {
        uint32_t importRva = 0;
        if (!checkedAlignUpU32(
                nextGeneratedRva, sectionAlignment, "import section RVA", err, importRva))
            return false;
        if (!buildImportTables(imports, importRva, slotRvas, importLayout, err))
            return false;
        generatedImportData = importLayout.data;

        /// Writes a loader hint-name thunk into a preallocated external IAT slot.
        auto patchExternalIatSlot = [&](uint32_t slotRva, uint64_t thunkValue) -> bool {
            for (auto &sec : sections) {
                if (!sec.alloc || !sec.writable || sec.data == nullptr)
                    continue;
                const uint64_t slotEnd = static_cast<uint64_t>(slotRva) + 8;
                const uint64_t secEnd = static_cast<uint64_t>(sec.virtualAddress) + sec.virtualSize;
                if (slotRva < sec.virtualAddress || slotEnd > secEnd)
                    continue;
                auto *bytes = const_cast<std::vector<uint8_t> *>(sec.data);
                const size_t off = static_cast<size_t>(slotRva - sec.virtualAddress);
                if (off + 8 > bytes->size())
                    return false;
                putLE64(*bytes, off, thunkValue);
                return true;
            }
            return false;
        };

        for (const auto &[slotRva, thunkValue] : importLayout.slotInitializers) {
            if (!patchExternalIatSlot(slotRva, thunkValue)) {
                err << "error: PE import slot RVA 0x" << std::hex << slotRva << std::dec
                    << " does not map to a writable output section\n";
                return false;
            }
        }

        PeSection importSec;
        importSec.name = ".idata";
        importSec.data = &generatedImportData;
        importSec.virtualAddress = importRva;
        if (!checkedSizeU32(
                generatedImportData.size(), "import section size", err, importSec.virtualSize))
            return false;
        importSec.writable = true;
        importSec.characteristics = sectionChars(false, true, true);
        sections.push_back(importSec);
        uint32_t alignedImportSize = 0;
        if (!checkedAlignUpU32(generatedImportData.size(),
                               sectionAlignment,
                               "import section size",
                               err,
                               alignedImportSize) ||
            !checkedAddU32(
                importRva, alignedImportSize, "import section RVA range", err, nextGeneratedRva))
            return false;
    }

    const bool haveTls = layoutHasTls(layout);
    uint32_t tlsDirRva = 0;
    if (!checkedAlignUpU32(nextGeneratedRva, sectionAlignment, "TLS directory RVA", err, tlsDirRva))
        return false;
    tlsLayout = buildTlsDirectory(imageBase, layout, tlsDirRva, err);
    if (haveTls && tlsLayout.directorySize == 0)
        return false;
    if (tlsLayout.directorySize != 0) {
        generatedTlsData = tlsLayout.data;

        PeSection tlsMetaSec;
        tlsMetaSec.name = ".rdata";
        tlsMetaSec.data = &generatedTlsData;
        tlsMetaSec.virtualAddress = tlsDirRva;
        if (!checkedSizeU32(
                generatedTlsData.size(), "TLS metadata section size", err, tlsMetaSec.virtualSize))
            return false;
        tlsMetaSec.characteristics = sectionChars(false, false, true);
        sections.push_back(tlsMetaSec);
        uint32_t alignedTlsSize = 0;
        if (!checkedAlignUpU32(generatedTlsData.size(),
                               sectionAlignment,
                               "TLS metadata section size",
                               err,
                               alignedTlsSize) ||
            !checkedAddU32(
                tlsDirRva, alignedTlsSize, "TLS metadata section RVA range", err, nextGeneratedRva))
            return false;
    }

    uint32_t entryRva = 0;
    if (!checkedRva(imageBase, layout.entryAddr, "<entry>", err, entryRva))
        return false;
    if (emitStartupStub && !imports.empty()) {
        const auto exitIt = importLayout.slotRvas.find("ExitProcess");
        if (exitIt == importLayout.slotRvas.end()) {
            err << "error: PE writer requires ExitProcess for startup stub\n";
            return false;
        }

        uint32_t stubRva = 0;
        if (!checkedAlignUpU32(
                nextGeneratedRva, sectionAlignment, "startup stub RVA", err, stubRva))
            return false;
        if (arch == LinkArch::AArch64) {
            generatedStubData =
                buildAArch64StartupStub(imageBase, layout.entryAddr, stubRva, exitIt->second, err);
        } else {
            generatedStubData =
                buildX64StartupStub(imageBase, layout.entryAddr, stubRva, exitIt->second, err);
        }
        if (generatedStubData.empty())
            return false;

        PeSection stubSec;
        stubSec.name = ".text";
        stubSec.data = &generatedStubData;
        stubSec.virtualAddress = stubRva;
        if (!checkedSizeU32(
                generatedStubData.size(), "startup stub section size", err, stubSec.virtualSize))
            return false;
        stubSec.executable = true;
        stubSec.characteristics = sectionChars(true, false, true);
        sections.push_back(stubSec);
        entryRva = stubRva;
        uint32_t alignedStubSize = 0;
        if (!checkedAlignUpU32(generatedStubData.size(),
                               sectionAlignment,
                               "startup stub section size",
                               err,
                               alignedStubSize) ||
            !checkedAddU32(
                stubRva, alignedStubSize, "startup stub RVA range", err, nextGeneratedRva))
            return false;
    }

    std::vector<uint32_t> baseRelocRvas;
    for (const auto &entry : layout.rebaseEntries) {
        if (entry.sectionIndex >= layout.sections.size()) {
            err << "error: PE base relocation references missing section index "
                << entry.sectionIndex << "\n";
            return false;
        }
        const auto &sec = layout.sections[entry.sectionIndex];
        if (!sec.alloc) {
            err << "error: PE base relocation references non-alloc section '" << sec.name << "'\n";
            return false;
        }
        if (entry.offset > sec.data.size() || sec.data.size() - entry.offset < 8) {
            err << "error: PE base relocation in section '" << sec.name
                << "' extends beyond initialized section data\n";
            return false;
        }
        if (sec.virtualAddr < imageBase) {
            err << "error: PE base relocation in section '" << sec.name
                << "' is below image base\n";
            return false;
        }
        if (entry.offset > std::numeric_limits<uint64_t>::max() - (sec.virtualAddr - imageBase)) {
            err << "error: PE base relocation RVA overflows 64-bit range in section '" << sec.name
                << "'\n";
            return false;
        }
        const uint64_t rva64 = (sec.virtualAddr - imageBase) + entry.offset;
        if (rva64 > UINT32_MAX) {
            err << "error: PE base relocation RVA exceeds 32-bit range in section '" << sec.name
                << "'\n";
            return false;
        }
        baseRelocRvas.push_back(static_cast<uint32_t>(rva64));
    }
    if (tlsLayout.directorySize != 0) {
        baseRelocRvas.push_back(tlsLayout.directoryRva + 0);
        baseRelocRvas.push_back(tlsLayout.directoryRva + 8);
        baseRelocRvas.push_back(tlsLayout.directoryRva + 16);
        baseRelocRvas.push_back(tlsLayout.directoryRva + 24);
    }

    // Always emit a .reloc section (empty when there are no fixups) so the image
    // can advertise DYNAMICBASE/HIGH_ENTROPY_VA and get ASLR on both x64 and
    // ARM64. Every absolute pointer is recorded in rebaseEntries by the applier,
    // so an empty .reloc means there is genuinely nothing to fix up under slide.
    generatedBaseRelocData = buildBaseRelocationBlocks(std::move(baseRelocRvas), /*forceNonEmpty=*/
                                                       true);
    if (!generatedBaseRelocData.empty()) {
        uint32_t relocRva = 0;
        if (!checkedAlignUpU32(
                nextGeneratedRva, sectionAlignment, "base relocation section RVA", err, relocRva))
            return false;

        PeSection relocSec;
        relocSec.name = ".reloc";
        relocSec.data = &generatedBaseRelocData;
        relocSec.virtualAddress = relocRva;
        if (!checkedSizeU32(generatedBaseRelocData.size(),
                            "base relocation section size",
                            err,
                            relocSec.virtualSize))
            return false;
        relocSec.characteristics = kRelocSectionChars;
        sections.push_back(relocSec);

        baseRelocLayout.data = generatedBaseRelocData;
        baseRelocLayout.directoryRva = relocRva;
        baseRelocLayout.directorySize = relocSec.virtualSize;
        uint32_t alignedRelocSize = 0;
        if (!checkedAlignUpU32(generatedBaseRelocData.size(),
                               sectionAlignment,
                               "base relocation section size",
                               err,
                               alignedRelocSize) ||
            !checkedAddU32(relocRva,
                           alignedRelocSize,
                           "base relocation section RVA range",
                           err,
                           nextGeneratedRva))
            return false;
    }

    uint32_t resourceRva = 0;
    if (!checkedAlignUpU32(
            nextGeneratedRva, sectionAlignment, "resource section RVA", err, resourceRva))
        return false;
    resourceLayout = buildDefaultManifestResource(resourceRva);
    generatedResourceData = resourceLayout.data;
    if (!generatedResourceData.empty()) {
        PeSection resourceSec;
        resourceSec.name = ".rsrc";
        resourceSec.data = &generatedResourceData;
        resourceSec.virtualAddress = resourceRva;
        if (!checkedSizeU32(generatedResourceData.size(),
                            "resource section size",
                            err,
                            resourceSec.virtualSize))
            return false;
        resourceSec.characteristics = sectionChars(false, false, true);
        sections.push_back(resourceSec);

        uint32_t alignedResourceSize = 0;
        if (!checkedAlignUpU32(generatedResourceData.size(),
                               sectionAlignment,
                               "resource section size",
                               err,
                               alignedResourceSize) ||
            !checkedAddU32(resourceRva,
                           alignedResourceSize,
                           "resource section RVA range",
                           err,
                           nextGeneratedRva))
            return false;
    }

    /// Orders sections by RVA while retaining deterministic order for ties.
    std::stable_sort(sections.begin(), sections.end(), [](const PeSection &a, const PeSection &b) {
        if (a.alloc != b.alloc)
            return a.alloc > b.alloc;
        if (!a.alloc)
            return a.name < b.name;
        return a.virtualAddress < b.virtualAddress;
    });

    const ExceptionLayout exceptionLayout = findExceptionDirectory(sections, arch);

    if (sections.size() > std::numeric_limits<uint16_t>::max()) {
        err << "error: PE section count exceeds 16-bit file format limit\n";
        return false;
    }
    const uint16_t numSections = static_cast<uint16_t>(sections.size());
    if (sections.size() > std::numeric_limits<size_t>::max() / 40) {
        err << "error: PE section table size overflows addressable size\n";
        return false;
    }
    const size_t sectionTableBytes = sections.size() * 40;
    if (sectionTableBytes > std::numeric_limits<size_t>::max() - (64 + 4 + 20 + 240)) {
        err << "error: PE header size overflows addressable size\n";
        return false;
    }
    const size_t headersWithoutPadding = 64 + 4 + 20 + 240 + sectionTableBytes;
    uint32_t sizeOfHeaders = 0;
    if (!checkedAlignUpU32(headersWithoutPadding, fileAlignment, "header size", err, sizeOfHeaders))
        return false;

    uint32_t currentFileOff = sizeOfHeaders;
    uint32_t sizeOfCode = 0;
    uint32_t sizeOfInitData = 0;
    uint32_t sizeOfUninitData = 0;
    uint32_t baseOfCode = 0;
    uint32_t sizeOfImage = sectionAlignment;

    for (auto &sec : sections) {
        if (sec.name.size() > 8) {
            err << "error: PE section name '" << sec.name << "' exceeds 8 bytes\n";
            return false;
        }
        if (sec.zeroFill) {
            sec.sizeOfRawData = 0;
        } else if (!checkedAlignUpU32(sec.virtualSize,
                                      fileAlignment,
                                      "section raw size",
                                      err,
                                      sec.sizeOfRawData)) {
            return false;
        }
        sec.pointerToRawData = sec.sizeOfRawData == 0 ? 0 : currentFileOff;
        if (!checkedAddU32(currentFileOff, sec.sizeOfRawData, "file offset", err, currentFileOff))
            return false;

        if (sec.alloc) {
            uint32_t alignedVirtualSize = 0;
            if (!checkedAlignUpU32(sec.virtualSize,
                                   sectionAlignment,
                                   "section virtual size",
                                   err,
                                   alignedVirtualSize))
                return false;
            uint32_t secEnd = 0;
            if (!checkedAddU32(
                    sec.virtualAddress, alignedVirtualSize, "section RVA range", err, secEnd))
                return false;
            sizeOfImage = std::max(sizeOfImage, secEnd);
            if (sec.executable) {
                if (!checkedAddU32(sizeOfCode, sec.sizeOfRawData, "SizeOfCode", err, sizeOfCode))
                    return false;
                if (baseOfCode == 0)
                    baseOfCode = sec.virtualAddress;
            } else if (sec.zeroFill) {
                if (!checkedAddU32(sizeOfUninitData,
                                   alignedVirtualSize,
                                   "SizeOfUninitializedData",
                                   err,
                                   sizeOfUninitData))
                    return false;
            } else {
                if (!checkedAddU32(sizeOfInitData,
                                   sec.sizeOfRawData,
                                   "SizeOfInitializedData",
                                   err,
                                   sizeOfInitData))
                    return false;
            }
        }
    }

    std::vector<uint8_t> file;
    file.resize(64, 0);
    file[0] = 'M';
    file[1] = 'Z';
    putLE32(file, 0x3C, 64);

    uint16_t dllCharacteristics = kDllCharNXCompat | kDllCharTermServerAware;
    if (baseRelocLayout.directoryRva != 0)
        dllCharacteristics |= kDllCharDynamicBase | kDllCharHighEntropyVA;

    writeLE32(file, 0x00004550); // "PE\0\0"

    writeLE16(file, machine);
    writeLE16(file, numSections);
    writeLE32(file, 0);
    writeLE32(file, 0);
    writeLE32(file, 0);
    writeLE16(file, 240);
    writeLE16(file, 0x0022); // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE

    const size_t optHeaderStart = file.size();
    writeLE16(file, 0x020B);
    writeLE16(file, 0);
    writeLE32(file, sizeOfCode);
    writeLE32(file, sizeOfInitData);
    writeLE32(file, sizeOfUninitData);
    writeLE32(file, entryRva);
    writeLE32(file, baseOfCode);
    writeLE64(file, imageBase);
    writeLE32(file, sectionAlignment);
    writeLE32(file, fileAlignment);
    writeLE16(file, 6);
    writeLE16(file, 0);
    writeLE16(file, 0);
    writeLE16(file, 0);
    writeLE16(file, 6);
    writeLE16(file, 0);
    writeLE32(file, 0);
    writeLE32(file, sizeOfImage);
    writeLE32(file, sizeOfHeaders);
    writeLE32(file, 0);
    writeLE16(file, 3); // IMAGE_SUBSYSTEM_WINDOWS_CUI
    writeLE16(file, dllCharacteristics);
    writeLE64(file, stackReserve);
    writeLE64(file, stackCommit);
    writeLE64(file, 0x100000);
    writeLE64(file, 0x1000);
    writeLE32(file, 0);
    writeLE32(file, 16);

    for (int i = 0; i < 16; ++i) {
        writeLE32(file, 0);
        writeLE32(file, 0);
    }

    if (!imports.empty()) {
        putLE32(file, optHeaderStart + 112 + 1 * 8 + 0, importLayout.importDirRva);
        putLE32(file, optHeaderStart + 112 + 1 * 8 + 4, importLayout.importDirSize);
        putLE32(file, optHeaderStart + 112 + 12 * 8 + 0, importLayout.iatRva);
        putLE32(file, optHeaderStart + 112 + 12 * 8 + 4, importLayout.iatSize);
    }
    if (resourceLayout.directoryRva != 0) {
        putLE32(file, optHeaderStart + 112 + 2 * 8 + 0, resourceLayout.directoryRva);
        putLE32(file, optHeaderStart + 112 + 2 * 8 + 4, resourceLayout.directorySize);
    }
    if (exceptionLayout.directoryRva != 0) {
        putLE32(file, optHeaderStart + 112 + 3 * 8 + 0, exceptionLayout.directoryRva);
        putLE32(file, optHeaderStart + 112 + 3 * 8 + 4, exceptionLayout.directorySize);
    }
    if (baseRelocLayout.directoryRva != 0) {
        putLE32(file, optHeaderStart + 112 + 5 * 8 + 0, baseRelocLayout.directoryRva);
        putLE32(file, optHeaderStart + 112 + 5 * 8 + 4, baseRelocLayout.directorySize);
    }
    if (tlsLayout.directoryRva != 0) {
        putLE32(file, optHeaderStart + 112 + 9 * 8 + 0, tlsLayout.directoryRva);
        putLE32(file, optHeaderStart + 112 + 9 * 8 + 4, tlsLayout.directorySize);
    }

    for (const auto &sec : sections) {
        if (sec.name.size() > 8) {
            err << "error: PE section name '" << sec.name
                << "' exceeds the 8-byte executable section-header limit\n";
            return false;
        }
        appendFixedNameField(file, sec.name, 8);

        writeLE32(file, sec.virtualSize);
        writeLE32(file, sec.virtualAddress);
        writeLE32(file, sec.sizeOfRawData);
        writeLE32(file, sec.pointerToRawData);
        writeLE32(file, 0);
        writeLE32(file, 0);
        writeLE16(file, 0);
        writeLE16(file, 0);
        writeLE32(file, sec.characteristics);
    }

    padTo(file, sizeOfHeaders);

    for (const auto &sec : sections) {
        if (sec.sizeOfRawData == 0)
            continue;
        padTo(file, sec.pointerToRawData);
        file.insert(file.end(), sec.data->begin(), sec.data->end());
        padTo(file, alignUp(file.size(), fileAlignment));
    }

    return writeBinaryFileAtomically(path, file, true, err);
}

} // namespace zanna::codegen::linker
