//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/ElfSymbolVersions.cpp
// Purpose: Read the version definitions of the shared libraries a link depends
//          on and report the default version of each imported symbol.
// Key invariants:
//   - Every parse step is bounds-checked against the mapped file size; a
//     malformed library contributes nothing instead of faulting.
//   - Only ELF64 little-endian objects matching the target machine are read.
//   - The first library in DT_NEEDED order that defines a name wins, matching
//     the loader's search order.
// Ownership/Lifetime: stateless; all buffers are function-local.
// Links: src/codegen/common/linker/ElfSymbolVersions.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ElfSymbolVersions.cpp
 * @brief Implements default-symbol-version discovery from the linked libraries.
 */

#include "codegen/common/linker/ElfSymbolVersions.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_set>

namespace zanna::codegen::linker {

namespace {

// ELF constants used while walking a shared object's version tables.
constexpr uint32_t kShtDynSym = 11;
constexpr uint32_t kShtGnuVerdef = 0x6ffffffd;
constexpr uint32_t kShtGnuVersym = 0x6fffffff;
constexpr uint16_t kShnUndef = 0;
constexpr uint16_t kVersymHiddenBit = 0x8000;
constexpr uint16_t kVersymIndexMask = 0x7fff;
/// Version indices 0 and 1 are the reserved "local" and "global" pseudo-versions.
constexpr uint16_t kFirstRealVersionIndex = 2;
constexpr uint16_t kEmMachineX86_64 = 62;
constexpr uint16_t kEmMachineAArch64 = 183;

/// @brief Read a little-endian scalar from a byte buffer with bounds checking.
/// @tparam T Trivially copyable scalar type to read.
/// @param data Buffer being parsed.
/// @param offset Byte offset of the value.
/// @param[out] out Receives the decoded value when the read is in range.
/// @return `true` when @p offset admits a whole @p T within @p data.
template <typename T>
bool readScalar(const std::vector<uint8_t> &data, size_t offset, T &out) {
    if (offset > data.size() || data.size() - offset < sizeof(T))
        return false;
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}

/// @brief Read a NUL-terminated string out of a string table.
/// @param data Buffer holding the table.
/// @param tableOffset Byte offset of the string table within @p data.
/// @param tableSize Size of the string table in bytes.
/// @param nameOffset Offset of the string within the table.
/// @param[out] out Receives the string when it is wholly in range.
/// @return `true` when a terminated string was found inside the table.
bool readTableString(const std::vector<uint8_t> &data,
                     uint64_t tableOffset,
                     uint64_t tableSize,
                     uint64_t nameOffset,
                     std::string &out) {
    if (nameOffset >= tableSize)
        return false;
    if (tableOffset > data.size() || data.size() - tableOffset < tableSize)
        return false;
    const char *base = reinterpret_cast<const char *>(data.data() + tableOffset);
    const uint64_t available = tableSize - nameOffset;
    const void *terminator = std::memchr(base + nameOffset, '\0', static_cast<size_t>(available));
    if (!terminator)
        return false;
    out.assign(base + nameOffset);
    return true;
}

/// @brief Split a `PATH`-style colon-separated list into its entries.
/// @param value List to split; empty entries are dropped.
/// @param[out] out Receives the entries in order.
void appendPathList(const char *value, std::vector<std::string> &out) {
    if (!value)
        return;
    std::string current;
    for (const char *p = value; *p != '\0'; ++p) {
        if (*p == ':') {
            if (!current.empty())
                out.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(*p);
    }
    if (!current.empty())
        out.push_back(current);
}

/// @brief Build the directory list searched for a target shared library.
/// @details Mirrors the loader's usual order closely enough to find the same
///          file: an explicit `LD_LIBRARY_PATH` first, then the architecture's
///          multiarch directory, then the plain library directories used by
///          non-multiarch distributions.
/// @param arch Target architecture selecting the multiarch directory name.
/// @return Ordered directory list, without trailing separators.
std::vector<std::string> librarySearchPaths(LinkArch arch) {
    std::vector<std::string> paths;
    appendPathList(std::getenv("LD_LIBRARY_PATH"), paths);

    const char *triple = arch == LinkArch::AArch64 ? "aarch64-linux-gnu" : "x86_64-linux-gnu";
    paths.push_back(std::string("/lib/") + triple);
    paths.push_back(std::string("/usr/lib/") + triple);
    paths.emplace_back("/lib64");
    paths.emplace_back("/usr/lib64");
    paths.emplace_back("/lib");
    paths.emplace_back("/usr/lib");
    return paths;
}

/// @brief Read a whole file into memory.
/// @param path Filesystem path to read.
/// @param[out] out Receives the contents on success.
/// @return `true` when the file was opened and fully read.
bool readWholeFile(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return false;
    const std::streamoff size = stream.tellg();
    if (size <= 0)
        return false;
    stream.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (!stream.read(reinterpret_cast<char *>(out.data()), size))
        return false;
    return true;
}

/// @brief One parsed ELF section header field set needed by this file.
struct SectionRef {
    uint32_t type = 0;
    uint32_t link = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t entsize = 0;
};

/// @brief Decode the section header table of a 64-bit little-endian ELF object.
/// @param data File contents.
/// @param machine Expected `e_machine` value.
/// @param[out] out Receives the section headers in file order.
/// @return `true` when the file is a well-formed ELF64 object for @p machine.
bool readSectionHeaders(const std::vector<uint8_t> &data,
                        uint16_t machine,
                        std::vector<SectionRef> &out) {
    // e_ident: 4 magic bytes, class, data encoding.
    if (data.size() < 64)
        return false;
    if (data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
        return false;
    if (data[4] != 2 /* ELFCLASS64 */ || data[5] != 1 /* ELFDATA2LSB */)
        return false;

    uint16_t fileMachine = 0;
    uint64_t shoff = 0;
    uint16_t shentsize = 0;
    uint16_t shnum = 0;
    if (!readScalar(data, 18, fileMachine) || !readScalar(data, 40, shoff) ||
        !readScalar(data, 58, shentsize) || !readScalar(data, 60, shnum))
        return false;
    if (fileMachine != machine || shentsize < 64 || shnum == 0)
        return false;

    for (uint16_t i = 0; i < shnum; ++i) {
        const uint64_t base = shoff + static_cast<uint64_t>(i) * shentsize;
        if (base > data.size())
            return false;
        SectionRef ref;
        if (!readScalar(data, static_cast<size_t>(base) + 4, ref.type) ||
            !readScalar(data, static_cast<size_t>(base) + 24, ref.offset) ||
            !readScalar(data, static_cast<size_t>(base) + 32, ref.size) ||
            !readScalar(data, static_cast<size_t>(base) + 40, ref.link) ||
            !readScalar(data, static_cast<size_t>(base) + 56, ref.entsize))
            return false;
        out.push_back(ref);
    }
    return true;
}

/// @brief Map version indices to their definition names for one library.
/// @details Walks `.gnu.version_d`, whose entries form a linked list through
///          `vd_next` with each definition's names chained through `vd_aux` /
///          `vda_next`. Only the first auxiliary name is recorded: that is the
///          definition's own name, the later ones being its predecessors.
/// @param data File contents.
/// @param verdef `.gnu.version_d` section header.
/// @param strtabOffset Offset of the associated string table.
/// @param strtabSize Size of the associated string table.
/// @param[out] out Receives index-to-name pairs.
void readVersionDefinitions(const std::vector<uint8_t> &data,
                            const SectionRef &verdef,
                            uint64_t strtabOffset,
                            uint64_t strtabSize,
                            std::unordered_map<uint16_t, std::string> &out) {
    uint64_t cursor = verdef.offset;
    const uint64_t end = verdef.offset + verdef.size;
    // The chain is bounded by the section, but a malformed vd_next of zero
    // would spin forever; cap iterations at one per possible entry.
    for (uint64_t guard = 0; guard < verdef.size / 20 + 1; ++guard) {
        if (cursor >= end)
            return;
        uint16_t version = 0;
        uint16_t ndx = 0;
        uint16_t auxCount = 0;
        uint32_t auxOffset = 0;
        uint32_t nextOffset = 0;
        if (!readScalar(data, static_cast<size_t>(cursor), version) ||
            !readScalar(data, static_cast<size_t>(cursor) + 4, ndx) ||
            !readScalar(data, static_cast<size_t>(cursor) + 6, auxCount) ||
            !readScalar(data, static_cast<size_t>(cursor) + 12, auxOffset) ||
            !readScalar(data, static_cast<size_t>(cursor) + 16, nextOffset))
            return;

        if (auxCount > 0) {
            uint32_t nameOffset = 0;
            if (readScalar(data, static_cast<size_t>(cursor + auxOffset), nameOffset)) {
                std::string name;
                if (readTableString(data, strtabOffset, strtabSize, nameOffset, name))
                    out.emplace(static_cast<uint16_t>(ndx & kVersymIndexMask), std::move(name));
            }
        }

        if (nextOffset == 0)
            return;
        cursor += nextOffset;
    }
}

/// @brief Collect the default version of every exported name in one library.
/// @param data File contents.
/// @param[out] out Receives symbol-name-to-version-name pairs; symbols exported
///        without a version are recorded with an empty version.
void readDefaultVersions(const std::vector<uint8_t> &data,
                         uint16_t machine,
                         std::unordered_map<std::string, std::string> &out) {
    std::vector<SectionRef> sections;
    if (!readSectionHeaders(data, machine, sections))
        return;

    const SectionRef *dynsym = nullptr;
    const SectionRef *versym = nullptr;
    const SectionRef *verdef = nullptr;
    for (const auto &section : sections) {
        if (section.type == kShtDynSym && !dynsym)
            dynsym = &section;
        else if (section.type == kShtGnuVersym && !versym)
            versym = &section;
        else if (section.type == kShtGnuVerdef && !verdef)
            verdef = &section;
    }
    if (!dynsym || dynsym->entsize < 24 || dynsym->link >= sections.size())
        return;

    const SectionRef &dynstr = sections[dynsym->link];
    std::unordered_map<uint16_t, std::string> versionNames;
    if (verdef && verdef->link < sections.size()) {
        const SectionRef &verstr = sections[verdef->link];
        readVersionDefinitions(data, *verdef, verstr.offset, verstr.size, versionNames);
    }

    const uint64_t symbolCount = dynsym->size / dynsym->entsize;
    for (uint64_t i = 0; i < symbolCount; ++i) {
        const uint64_t symOffset = dynsym->offset + i * dynsym->entsize;
        uint32_t nameOffset = 0;
        uint16_t shndx = 0;
        if (!readScalar(data, static_cast<size_t>(symOffset), nameOffset) ||
            !readScalar(data, static_cast<size_t>(symOffset) + 6, shndx))
            return;
        // Undefined entries are this library's own imports, not exports.
        if (shndx == kShnUndef || nameOffset == 0)
            continue;

        std::string name;
        if (!readTableString(data, dynstr.offset, dynstr.size, nameOffset, name))
            continue;

        std::string version;
        if (versym && versym->size >= (i + 1) * sizeof(uint16_t)) {
            uint16_t entry = 0;
            if (!readScalar(data, static_cast<size_t>(versym->offset + i * sizeof(uint16_t)), entry))
                continue;
            // A hidden entry is a compatibility definition kept for binaries
            // that named it explicitly; it is never what a fresh link wants.
            if ((entry & kVersymHiddenBit) != 0)
                continue;
            const uint16_t index = entry & kVersymIndexMask;
            if (index >= kFirstRealVersionIndex) {
                auto found = versionNames.find(index);
                if (found != versionNames.end())
                    version = found->second;
            }
        }
        out.emplace(std::move(name), std::move(version));
    }
}

} // namespace

/// @copydoc resolveElfSymbolVersions
std::unordered_map<std::string, ElfSymbolVersion>
resolveElfSymbolVersions(const std::vector<std::string> &neededLibs,
                         const std::vector<std::string> &dynSymbols,
                         LinkArch arch) {
    std::unordered_map<std::string, ElfSymbolVersion> resolved;
    if (arch != LinkArch::X86_64 && arch != LinkArch::AArch64)
        return resolved;

    const std::unordered_set<std::string> wanted(dynSymbols.begin(), dynSymbols.end());
    if (wanted.empty())
        return resolved;

    const uint16_t machine = arch == LinkArch::AArch64 ? kEmMachineAArch64 : kEmMachineX86_64;
    const std::vector<std::string> searchPaths = librarySearchPaths(arch);

    for (const auto &lib : neededLibs) {
        std::vector<uint8_t> data;
        bool loaded = false;
        for (const auto &dir : searchPaths) {
            if (readWholeFile(dir + "/" + lib, data)) {
                loaded = true;
                break;
            }
        }
        if (!loaded)
            continue;

        std::unordered_map<std::string, std::string> exported;
        readDefaultVersions(data, machine, exported);
        for (const auto &[name, version] : exported) {
            // Earlier DT_NEEDED entries win: that is the order the loader
            // searches, so the first definition found is the one that binds.
            if (version.empty() || wanted.count(name) == 0 || resolved.count(name) != 0)
                continue;
            resolved.emplace(name, ElfSymbolVersion{lib, version});
        }
    }

    return resolved;
}

} // namespace zanna::codegen::linker
