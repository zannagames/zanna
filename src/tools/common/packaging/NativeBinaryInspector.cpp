//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/NativeBinaryInspector.cpp
// Purpose: Parse Mach-O (thin and universal), ELF64, and PE headers into
//          format, kind, and architecture facts, and search binaries for
//          NUL-terminated symbol names.
// Key invariants:
//   - Every multi-byte read is bounds-checked against the supplied buffer.
//   - Universal Mach-O slices must lie inside the file and start with a thin
//     Mach-O header; otherwise the file is rejected as malformed.
// Ownership/Lifetime:
//   - Stateless free functions over caller-owned buffers.
// Links: NativeBinaryInspector.hpp, docs/adr/0354-store-depot-packaging.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements header-level inspection of native executables and libraries.

#include "NativeBinaryInspector.hpp"

#include <algorithm>
#include <functional>
#include <set>

namespace zanna::pkg {
namespace {

/// @brief Mach-O CPU type for x86-64.
constexpr uint32_t kCpuTypeX64 = 0x01000007u;
/// @brief Mach-O CPU type for arm64.
constexpr uint32_t kCpuTypeArm64 = 0x0100000Cu;
/// @brief Mach-O file type of a main executable.
constexpr uint32_t kMachOExecute = 0x2u;
/// @brief Mach-O file type of a dynamic library.
constexpr uint32_t kMachODylib = 0x6u;
/// @brief Largest slice count accepted in a universal header.
constexpr uint32_t kMaxFatSlices = 64u;

/// @brief Read a little-endian 16-bit value, or nullopt when out of range.
/// @param data Buffer.
/// @param offset Byte offset.
/// @return Decoded value.
std::optional<uint16_t> readLE16(const std::vector<uint8_t> &data, uint64_t offset) {
    if (offset > data.size() || data.size() - offset < 2)
        return std::nullopt;
    return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
}

/// @brief Read a little-endian 32-bit value, or nullopt when out of range.
/// @param data Buffer.
/// @param offset Byte offset.
/// @return Decoded value.
std::optional<uint32_t> readLE32(const std::vector<uint8_t> &data, uint64_t offset) {
    if (offset > data.size() || data.size() - offset < 4)
        return std::nullopt;
    return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

/// @brief Read a big-endian 32-bit value, or nullopt when out of range.
/// @param data Buffer.
/// @param offset Byte offset.
/// @return Decoded value.
std::optional<uint32_t> readBE32(const std::vector<uint8_t> &data, uint64_t offset) {
    if (offset > data.size() || data.size() - offset < 4)
        return std::nullopt;
    return (static_cast<uint32_t>(data[offset]) << 24) |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
}

/// @brief Read a big-endian 64-bit value, or nullopt when out of range.
/// @param data Buffer.
/// @param offset Byte offset.
/// @return Decoded value.
std::optional<uint64_t> readBE64(const std::vector<uint8_t> &data, uint64_t offset) {
    auto hi = readBE32(data, offset);
    auto lo = readBE32(data, offset + 4);
    if (!hi || !lo)
        return std::nullopt;
    return (static_cast<uint64_t>(*hi) << 32) | *lo;
}

/// @brief Map a Mach-O CPU type to a packaging architecture name.
/// @param cputype Mach-O cputype field.
/// @return "x64", "arm64", or empty for unsupported CPU types.
std::string machOArch(uint32_t cputype) {
    if (cputype == kCpuTypeX64)
        return "x64";
    if (cputype == kCpuTypeArm64)
        return "arm64";
    return {};
}

/// @brief Map a Mach-O file type to a binary kind.
/// @param filetype Mach-O filetype field.
/// @return Kind for executables and dylibs, Unknown otherwise.
NativeBinaryKind machOKind(uint32_t filetype) {
    if (filetype == kMachOExecute)
        return NativeBinaryKind::Executable;
    if (filetype == kMachODylib)
        return NativeBinaryKind::SharedLibrary;
    return NativeBinaryKind::Unknown;
}

/// @brief Parse a thin Mach-O header at @p offset.
/// @param data Buffer.
/// @param offset Header start.
/// @param outArch Receives the architecture name (empty for unsupported CPUs).
/// @param outKind Receives the file kind.
/// @return True when a thin 32- or 64-bit Mach-O header starts at @p offset.
bool parseThinMachO(const std::vector<uint8_t> &data,
                    uint64_t offset,
                    std::string &outArch,
                    NativeBinaryKind &outKind) {
    const auto magic = readLE32(data, offset);
    if (!magic)
        return false;
    const bool little = *magic == 0xFEEDFACFu || *magic == 0xFEEDFACEu;
    const bool big = *magic == 0xCFFAEDFEu || *magic == 0xCEFAEDFEu;
    if (!little && !big)
        return false;
    const auto cputype = little ? readLE32(data, offset + 4) : readBE32(data, offset + 4);
    const auto filetype = little ? readLE32(data, offset + 12) : readBE32(data, offset + 12);
    if (!cputype || !filetype)
        return false;
    outArch = machOArch(*cputype);
    outKind = machOKind(*filetype);
    return true;
}

/// @brief Inspect a Mach-O file (thin or universal).
/// @param data Complete file contents.
/// @return Facts, or nullopt when malformed.
std::optional<NativeBinaryInfo> inspectMachO(const std::vector<uint8_t> &data) {
    NativeBinaryInfo info;
    info.format = NativeBinaryFormat::MachO;
    std::string arch;
    NativeBinaryKind kind = NativeBinaryKind::Unknown;
    if (parseThinMachO(data, 0, arch, kind)) {
        if (!arch.empty())
            info.architectures.push_back(arch);
        info.kind = kind;
        return info;
    }

    const auto magic = readBE32(data, 0);
    if (!magic || (*magic != 0xCAFEBABEu && *magic != 0xCAFEBABFu))
        return std::nullopt;
    const bool fat64 = *magic == 0xCAFEBABFu;
    const auto count = readBE32(data, 4);
    if (!count || *count == 0 || *count > kMaxFatSlices)
        return std::nullopt;
    info.universal = true;
    const uint64_t entrySize = fat64 ? 32u : 20u;
    std::set<std::string> archs;
    std::optional<NativeBinaryKind> commonKind;
    bool kindsAgree = true;
    for (uint32_t i = 0; i < *count; ++i) {
        const uint64_t entry = 8u + static_cast<uint64_t>(i) * entrySize;
        const auto cputype = readBE32(data, entry);
        const auto sliceOffset =
            fat64 ? readBE64(data, entry + 8) : std::optional<uint64_t>(readBE32(data, entry + 8));
        const auto sliceSize = fat64 ? readBE64(data, entry + 16)
                                     : std::optional<uint64_t>(readBE32(data, entry + 12));
        if (!cputype || !sliceOffset || !sliceSize || *sliceOffset > data.size() ||
            *sliceSize > data.size() - *sliceOffset)
            return std::nullopt;
        std::string sliceArch;
        NativeBinaryKind sliceKind = NativeBinaryKind::Unknown;
        if (!parseThinMachO(data, *sliceOffset, sliceArch, sliceKind))
            return std::nullopt;
        if (sliceArch.empty())
            continue;
        archs.insert(sliceArch);
        if (!commonKind)
            commonKind = sliceKind;
        else if (*commonKind != sliceKind)
            kindsAgree = false;
    }
    info.architectures.assign(archs.begin(), archs.end());
    info.kind = (commonKind && kindsAgree) ? *commonKind : NativeBinaryKind::Unknown;
    return info;
}

/// @brief Inspect an ELF file.
/// @param data Complete file contents.
/// @return Facts, or nullopt when not a 64-bit little-endian ELF.
std::optional<NativeBinaryInfo> inspectElf(const std::vector<uint8_t> &data) {
    if (data.size() < 64 || data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
        return std::nullopt;
    if (data[4] != 2 || data[5] != 1)
        return std::nullopt;
    NativeBinaryInfo info;
    info.format = NativeBinaryFormat::ELF;
    const uint16_t type = *readLE16(data, 16);
    const uint16_t machine = *readLE16(data, 18);
    if (machine == 62)
        info.architectures.push_back("x64");
    else if (machine == 183)
        info.architectures.push_back("arm64");
    if (type == 2)
        info.kind = NativeBinaryKind::Executable;
    else if (type == 3)
        info.kind = NativeBinaryKind::SharedLibrary;
    return info;
}

/// @brief Inspect a PE image.
/// @param data Complete file contents.
/// @return Facts, or nullopt when the DOS or PE headers are malformed.
std::optional<NativeBinaryInfo> inspectPe(const std::vector<uint8_t> &data) {
    if (data.size() < 64 || data[0] != 'M' || data[1] != 'Z')
        return std::nullopt;
    const auto peOffset = readLE32(data, 0x3C);
    if (!peOffset)
        return std::nullopt;
    const auto signature = readLE32(data, *peOffset);
    if (!signature || *signature != 0x00004550u)
        return std::nullopt;
    const auto machine = readLE16(data, static_cast<uint64_t>(*peOffset) + 4);
    const auto characteristics = readLE16(data, static_cast<uint64_t>(*peOffset) + 22);
    const auto optionalMagic = readLE16(data, static_cast<uint64_t>(*peOffset) + 24);
    if (!machine || !characteristics || !optionalMagic)
        return std::nullopt;
    if (*optionalMagic != 0x10Bu && *optionalMagic != 0x20Bu)
        return std::nullopt;
    NativeBinaryInfo info;
    info.format = NativeBinaryFormat::PE;
    info.pe32Plus = *optionalMagic == 0x20Bu;
    if (*machine == 0x8664u)
        info.architectures.push_back("x64");
    else if (*machine == 0xAA64u)
        info.architectures.push_back("arm64");
    info.kind = (*characteristics & 0x2000u) != 0 ? NativeBinaryKind::SharedLibrary
                                                  : NativeBinaryKind::Executable;
    return info;
}

} // namespace

/// @brief Inspect native binary bytes.
/// @param data Complete file contents.
/// @return Header facts, or nullopt for unrecognized or malformed input.
std::optional<NativeBinaryInfo> inspectNativeBinary(const std::vector<uint8_t> &data) {
    if (auto elf = inspectElf(data))
        return elf;
    if (auto pe = inspectPe(data))
        return pe;
    return inspectMachO(data);
}

/// @brief Human-readable format name.
/// @param format Container format.
/// @return Stable diagnostic spelling.
std::string nativeBinaryFormatName(NativeBinaryFormat format) {
    switch (format) {
        case NativeBinaryFormat::MachO:
            return "Mach-O";
        case NativeBinaryFormat::ELF:
            return "ELF";
        case NativeBinaryFormat::PE:
            return "PE";
        case NativeBinaryFormat::Unknown:
            break;
    }
    return "unknown";
}

/// @brief Report whether @p name followed by NUL occurs in @p data.
/// @param data Complete file contents.
/// @param name Symbol name.
/// @return True when present.
bool binaryContainsSymbolName(const std::vector<uint8_t> &data, std::string_view name) {
    if (name.empty())
        return false;
    std::vector<uint8_t> needle(name.begin(), name.end());
    needle.push_back(0);
    const std::boyer_moore_horspool_searcher searcher(needle.begin(), needle.end());
    return std::search(data.begin(), data.end(), searcher) != data.end();
}

} // namespace zanna::pkg
