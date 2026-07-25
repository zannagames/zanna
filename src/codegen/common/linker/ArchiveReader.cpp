//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/ArchiveReader.cpp
// Purpose: Implementation of Unix ar archive parsing.
//          Handles GNU, BSD, and COFF archive format variants.
// Key invariants:
//   - GNU long names: "/offset" referencing "//" string table member
//   - BSD long names: "#1/N" with N name bytes following the header
//   - COFF: first "/" member is symbol table (big-endian count + offsets)
//   - Data is padded to 2-byte alignment boundary
// Links: codegen/common/linker/ArchiveReader.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ArchiveReader.cpp
 * @brief Implements bounded parsing of GNU, BSD/Darwin, and COFF archives.
 */

#include "codegen/common/linker/ArchiveReader.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>

namespace zanna::codegen::linker {

static constexpr const char *kArMagic = "!<arch>\n";
static constexpr size_t kArMagicLen = 8;
static constexpr size_t kArHeaderLen = 60;

/// @brief Add @p a + @p b into @p out, returning false on size_t overflow.
/// @details Used throughout this reader to harden against malformed archives
///          that claim sizes near SIZE_MAX in their member headers.
/// @param a Left operand.
/// @param b Right operand.
/// @param out Receives the sum when representable.
/// @return `true` when the addition succeeds; otherwise `false`.
static bool checkedAdd(size_t a, size_t b, size_t &out) {
    if (a > std::numeric_limits<size_t>::max() - b)
        return false;
    out = a + b;
    return true;
}

/// @brief Multiply two size_t values into @p out, returning false on overflow.
/// @details Archive symbol tables carry 32-bit counts but are parsed on hosts
///          where size_t may be narrower than the object format's logical
///          limits. Centralizing the multiplication check prevents wraparound
///          before range validation.
/// @param a Left factor.
/// @param b Right factor.
/// @param out Receives the product when representable.
/// @return `true` when the multiplication succeeds; otherwise `false`.
static bool checkedMul(size_t a, size_t b, size_t &out) {
    if (a != 0 && b > std::numeric_limits<size_t>::max() / a)
        return false;
    out = a * b;
    return true;
}

/// @brief Verify that the byte range [@p off, @p off+@p len) fits within @p size.
/// @details Avoids the @c off+len overflow trap by computing the bound as
///          @p size − @p off, which never overflows once @p off ≤ @p size holds.
/// @param off Starting byte offset.
/// @param len Number of bytes in the range.
/// @param size Size of the containing buffer.
/// @return `true` when the complete half-open range is contained.
static bool checkedRange(size_t off, size_t len, size_t size) {
    return off <= size && len <= size - off;
}

/// @brief Reads an unaligned big-endian 32-bit integer.
/// @param p Pointer to at least four readable bytes.
/// @return Decoded host-order value.
static uint32_t readBE32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/// @brief Reads an unaligned little-endian 16-bit integer.
/// @param p Pointer to at least two readable bytes.
/// @return Decoded host-order value.
static uint16_t readLE16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

/// @brief Reads an unaligned little-endian 32-bit integer.
/// @param p Pointer to at least four readable bytes.
/// @return Decoded host-order value.
static uint32_t readLE32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/// Maximum archive member size: 2 GB.
static constexpr size_t kMaxMemberSize = 2ULL * 1024 * 1024 * 1024;

/// @brief Parses the fixed-width decimal size in an archive member header.
/// @details The ten-byte field at offset 48 must contain at least one digit
///          followed only by space padding.  Members larger than
///          `kMaxMemberSize` are rejected.
/// @param header Pointer to a complete 60-byte archive header.
/// @return Parsed member size, or `SIZE_MAX` for malformed, overflowing, or
///         over-limit values.
static size_t parseSize(const uint8_t *header) {
    // Size field is at offset 48, 10 bytes, ASCII decimal, space-padded.
    size_t val = 0;
    bool sawDigit = false;
    bool sawPadding = false;
    for (int i = 48; i < 58; ++i) {
        if (header[i] >= '0' && header[i] <= '9') {
            if (sawPadding)
                return SIZE_MAX;
            sawDigit = true;
            const size_t digit = static_cast<size_t>(header[i] - '0');
            if (val > (std::numeric_limits<size_t>::max() - digit) / 10)
                return SIZE_MAX;
            val = val * 10 + digit;
        } else if (header[i] == ' ') {
            if (sawDigit)
                sawPadding = true;
        } else {
            return SIZE_MAX;
        }
    }
    if (!sawDigit)
        return SIZE_MAX;
    return val > kMaxMemberSize ? SIZE_MAX : val;
}

/// @brief Parses a GNU-style `"/"` archive symbol table.
/// @details The payload contains a big-endian count, that many big-endian
///          member-header offsets, then matching NUL-terminated symbol names.
///          A malformed optional index is ignored rather than treated as an
///          archive-wide failure.
/// @param data Start of the linker-member payload.
/// @param size Payload size in bytes.
/// @param symbols Receives symbol names paired with archive member offsets.
static void parseGnuSymbolTable(const uint8_t *data,
                                size_t size,
                                std::vector<std::pair<std::string, size_t>> &symbols) {
    if (size < 4)
        return;
    const uint32_t count = readBE32(data);
    size_t offsetsBytes = 0;
    if (!checkedMul(static_cast<size_t>(count), 4, offsetsBytes))
        return;
    size_t namesOff = 0;
    if (!checkedAdd(4, offsetsBytes, namesOff) || namesOff > size)
        return;

    // Offsets array.
    std::vector<uint32_t> offsets(count);
    for (uint32_t i = 0; i < count; ++i)
        offsets[i] = readBE32(data + 4 + i * 4);

    // Symbol names follow offsets, NUL-terminated.
    const char *namePtr = reinterpret_cast<const char *>(data + namesOff);
    const char *nameEnd = reinterpret_cast<const char *>(data + size);
    for (uint32_t i = 0; i < count && namePtr < nameEnd; ++i) {
        const char *nul = std::find(namePtr, nameEnd, '\0');
        if (nul == nameEnd)
            break;
        std::string symName(namePtr, nul);
        symbols.emplace_back(std::move(symName), offsets[i]);
        namePtr = nul + 1;
    }
}

/// @brief Parse the BSD/Darwin archive symbol table into symbol/member pairs.
/// @details The first word stores the byte size of the ranlib array, followed by
///          8-byte ranlib entries `(string offset, member offset)`, a string-table
///          byte count, and a NUL-terminated string pool. Malformed offsets or
///          unterminated symbol names are ignored so a bad optional index cannot
///          corrupt member parsing.
/// @param data    Start of the linker-member payload.
/// @param size    Payload size in bytes.
/// @param symbols Receives parsed symbol names paired with archive member offsets.
static void parseBsdSymbolTable(const uint8_t *data,
                                size_t size,
                                std::vector<std::pair<std::string, size_t>> &symbols) {
    if (size < 4)
        return;
    const uint32_t ranlibSize = readLE32(data);
    if ((ranlibSize % 8) != 0)
        return;
    const uint32_t ranlibCount = ranlibSize / 8;
    size_t strSizeOff = 0;
    if (!checkedAdd(4, ranlibSize, strSizeOff) || !checkedRange(strSizeOff, 4, size))
        return;

    const uint8_t *ranlibData = data + 4;
    const uint32_t strSize = readLE32(data + strSizeOff);
    size_t strPoolOff = 0;
    if (!checkedAdd(strSizeOff, 4, strPoolOff) || !checkedRange(strPoolOff, strSize, size))
        return;
    const char *strPool = reinterpret_cast<const char *>(data + strPoolOff);
    const char *strEnd = strPool + strSize;

    for (uint32_t i = 0; i < ranlibCount; ++i) {
        const uint32_t strOff = readLE32(ranlibData + i * 8);
        const uint32_t memberOff = readLE32(ranlibData + i * 8 + 4);
        if (strOff < strSize) {
            const char *symStart = strPool + strOff;
            const char *nul = std::find(symStart, strEnd, '\0');
            if (nul == strEnd)
                continue;
            std::string symName(symStart, nul);
            symbols.emplace_back(std::move(symName), memberOff);
        }
    }
}

/// @brief Parse the Microsoft COFF second linker member.
/// @details This is the preferred COFF archive index:
///          `u32le memberCount`, `u32le offsets[memberCount]`,
///          `u32le symbolCount`, `u16le indices[symbolCount]`, then a packed
///          NUL-terminated name table. The symbol indices are one-based into the
///          member-offset array, so invalid zero/out-of-range entries are skipped.
/// @param data    Start of the linker-member payload.
/// @param size    Payload size in bytes.
/// @param symbols Receives parsed symbol names paired with archive member offsets.
static void parseCoffSecondLinkerMember(const uint8_t *data,
                                        size_t size,
                                        std::vector<std::pair<std::string, size_t>> &symbols) {
    if (size < 8)
        return;

    const uint32_t memberCount = readLE32(data);
    size_t offsetsBytes = 0;
    if (!checkedMul(static_cast<size_t>(memberCount), 4, offsetsBytes))
        return;
    size_t symbolCountOff = 0;
    if (!checkedAdd(4, offsetsBytes, symbolCountOff) || !checkedRange(symbolCountOff, 4, size))
        return;

    const uint8_t *offsets = data + 4;
    const uint8_t *symbolCountPtr = data + symbolCountOff;
    const uint32_t symbolCount = readLE32(symbolCountPtr);
    size_t indexBytes = 0;
    if (!checkedMul(static_cast<size_t>(symbolCount), 2, indexBytes))
        return;
    size_t indexEndOff = 0;
    if (!checkedAdd(symbolCountOff, 4, indexEndOff))
        return;
    size_t namesOff = 0;
    if (!checkedAdd(indexEndOff, indexBytes, namesOff) || namesOff > size)
        return;

    const uint8_t *indices = symbolCountPtr + 4;
    const char *namePtr = reinterpret_cast<const char *>(data + namesOff);
    const char *nameEnd = reinterpret_cast<const char *>(data + size);

    for (uint32_t i = 0; i < symbolCount && namePtr < nameEnd; ++i) {
        const uint16_t memberIndex = readLE16(indices + static_cast<size_t>(i) * 2);
        const char *nul = std::find(namePtr, nameEnd, '\0');
        if (nul == nameEnd)
            break;

        if (memberIndex > 0 && memberIndex <= memberCount) {
            const uint32_t memberOffset =
                readLE32(offsets + static_cast<size_t>(memberIndex - 1) * 4);
            symbols.emplace_back(std::string(namePtr, nul), memberOffset);
        }

        namePtr = nul + 1;
    }
}

/// @copydoc readArchive(const std::string &, Archive &, std::ostream &)
bool readArchive(const std::string &path, Archive &ar, std::ostream &err) {
    ar = Archive{};
    ar.path = path;

    // Read the entire file.
    const auto *utf8Path = reinterpret_cast<const char8_t *>(path.data());
    const std::filesystem::path diskPath(std::u8string_view(utf8Path, path.size()));
    std::ifstream f(diskPath, std::ios::binary | std::ios::ate);
    if (!f) {
        err << "error: cannot open archive '" << path << "'\n";
        return false;
    }
    const std::streampos endPos = f.tellg();
    if (endPos == std::streampos(-1)) {
        err << "error: failed to determine archive size for '" << path << "'\n";
        return false;
    }
    const auto endOff = static_cast<std::streamoff>(endPos);
    if (endOff < 0 ||
        static_cast<uintmax_t>(endOff) >
            static_cast<uintmax_t>(std::numeric_limits<size_t>::max()) ||
        static_cast<uintmax_t>(endOff) >
            static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        err << "error: archive '" << path << "' is too large to read\n";
        return false;
    }
    const auto fileSize = static_cast<size_t>(endOff);
    f.seekg(0);
    ar.data.resize(fileSize);
    f.read(reinterpret_cast<char *>(ar.data.data()), static_cast<std::streamsize>(fileSize));
    if (!f) {
        err << "error: failed to read archive '" << path << "'\n";
        return false;
    }

    // Verify magic.
    if (fileSize < kArMagicLen || std::memcmp(ar.data.data(), kArMagic, kArMagicLen) != 0) {
        err << "error: not an archive file: '" << path << "'\n";
        return false;
    }

    // First pass: find special members (symbol table, long name string table).
    std::string longNames;                                  // GNU "//" long name string table.
    std::vector<std::pair<std::string, size_t>> rawSymbols; // (symbol name, file offset of member).

    // Track member headers and their file offsets for symbol index mapping.
    /// @brief First-pass metadata retaining archive-header offsets and special status.
    struct RawMember {
        std::string name;
        size_t headerOffset = 0;
        size_t dataOffset = 0;
        size_t dataSize = 0;
        size_t bsdNameLen = 0;  // BSD "#1/N" name length (0 if not BSD long name).
        bool isSpecial = false; // true for "/", "//", "__.SYMDEF"
    };

    std::vector<RawMember> rawMembers;

    size_t pos = kArMagicLen;
    size_t coffLinkerMembersSeen = 0;
    while (pos <= fileSize && kArHeaderLen <= fileSize - pos) {
        const uint8_t *header = ar.data.data() + pos;

        // Verify header end magic "'\n".
        if (header[58] != '`' || header[59] != '\n') {
            err << "error: malformed archive member header at offset " << pos << " in '" << path
                << "'\n";
            return false;
        }

        size_t memberSize = parseSize(header);
        if (memberSize == SIZE_MAX) {
            err << "error: archive member at offset " << pos << " has invalid size in '" << path
                << "'\n";
            return false;
        }
        size_t dataStart = 0;
        if (!checkedAdd(pos, kArHeaderLen, dataStart) ||
            !checkedRange(dataStart, memberSize, fileSize)) {
            err << "error: archive member at offset " << pos << " extends beyond file in '" << path
                << "'\n";
            return false;
        }

        // Parse raw name field.
        char rawName[17] = {};
        std::memcpy(rawName, header, 16);
        std::string nameField(rawName, 16);
        while (!nameField.empty() && nameField.back() == ' ')
            nameField.pop_back();

        bool isSpecial = false;
        std::string memberName;

        // Resolve BSD "#1/N" long names BEFORE checking for special members,
        // because macOS archives store "__.SYMDEF SORTED" as a long name.
        std::string resolvedName;
        size_t bsdNameLen = 0;
        if (nameField.size() >= 3 && nameField[0] == '#' && nameField[1] == '1' &&
            nameField[2] == '/') {
            bool sawNameLenDigit = false;
            for (size_t i = 3; i < nameField.size() && nameField[i] >= '0' && nameField[i] <= '9';
                 ++i) {
                sawNameLenDigit = true;
                const size_t digit = static_cast<size_t>(nameField[i] - '0');
                if (bsdNameLen > (std::numeric_limits<size_t>::max() - digit) / 10) {
                    err << "error: archive member at offset " << pos
                        << " has invalid BSD long-name length in '" << path << "'\n";
                    return false;
                }
                bsdNameLen = bsdNameLen * 10 + digit;
            }
            if (!sawNameLenDigit || bsdNameLen > memberSize ||
                !checkedRange(dataStart, bsdNameLen, fileSize)) {
                err << "error: archive member at offset " << pos
                    << " has invalid BSD long-name length in '" << path << "'\n";
                return false;
            }
            const char *nd = reinterpret_cast<const char *>(ar.data.data() + dataStart);
            size_t effLen = bsdNameLen;
            while (effLen > 0 && nd[effLen - 1] == '\0')
                --effLen;
            resolvedName.assign(nd, effLen);
        } else {
            resolvedName = nameField;
            if (resolvedName == "/" || resolvedName == "//") {
                // Keep the COFF/GNU special member names intact.
            } else if (resolvedName.size() > 1 && resolvedName[0] == '/' &&
                       resolvedName[1] >= '0' && resolvedName[1] <= '9') {
                size_t offset = 0;
                for (size_t i = 1;
                     i < resolvedName.size() && resolvedName[i] >= '0' && resolvedName[i] <= '9';
                     ++i) {
                    const size_t digit = static_cast<size_t>(resolvedName[i] - '0');
                    if (offset > (std::numeric_limits<size_t>::max() - digit) / 10) {
                        offset = std::numeric_limits<size_t>::max();
                        break;
                    }
                    offset = offset * 10 + digit;
                }
                if (offset >= longNames.size()) {
                    err << "error: archive member at offset " << pos
                        << " references missing GNU long-name offset " << offset << " in '" << path
                        << "'\n";
                    return false;
                }
                size_t end = offset;
                while (end < longNames.size() && longNames[end] != '\0' && longNames[end] != '\n')
                    ++end;
                resolvedName = longNames.substr(offset, end - offset);
                while (!resolvedName.empty() && resolvedName.back() == '/')
                    resolvedName.pop_back();
            } else {
                // Trim trailing '/' (GNU terminator) for normal short names.
                while (!resolvedName.empty() && resolvedName.back() == '/')
                    resolvedName.pop_back();
            }
        }

        // Check for special members.
        if (resolvedName == "/" || resolvedName == "__.SYMDEF" ||
            resolvedName == "__.SYMDEF SORTED") {
            isSpecial = true;
            // For BSD long names, the symbol data starts AFTER the name bytes.
            size_t symDataOff = 0;
            size_t symDataSize = memberSize - bsdNameLen;
            if (checkedAdd(dataStart, bsdNameLen, symDataOff) &&
                checkedRange(symDataOff, symDataSize, fileSize)) {
                if (resolvedName == "/") {
                    ++coffLinkerMembersSeen;
                    if (coffLinkerMembersSeen == 2)
                        parseCoffSecondLinkerMember(
                            ar.data.data() + symDataOff, symDataSize, rawSymbols);
                    else
                        parseGnuSymbolTable(ar.data.data() + symDataOff, symDataSize, rawSymbols);
                } else {
                    parseBsdSymbolTable(ar.data.data() + symDataOff, symDataSize, rawSymbols);
                }
            }
        } else if (resolvedName == "//" || nameField == "//") {
            isSpecial = true;
            // GNU long name string table.
            if (checkedRange(dataStart, memberSize, fileSize))
                longNames.assign(reinterpret_cast<const char *>(ar.data.data() + dataStart),
                                 memberSize);
        } else {
            // Regular member.
            memberName = resolvedName;
        }

        rawMembers.push_back({memberName, pos, dataStart, memberSize, bsdNameLen, isSpecial});

        // Advance past data, aligned to 2 bytes.
        if (!checkedAdd(dataStart, memberSize, pos)) {
            err << "error: archive member at offset " << dataStart
                << " exceeds addressable size in '" << path << "'\n";
            return false;
        }
        if (pos & 1) {
            if (pos == fileSize) {
                // Final member ended on an odd boundary with the trailing pad
                // omitted at end-of-file — accepted (some archivers skip it).
            } else if (pos >= fileSize || ar.data[pos] != '\n') {
                err << "error: archive member at offset " << dataStart
                    << " is missing its padding byte in '" << path << "'\n";
                return false;
            } else {
                ++pos;
            }
        }
    }
    if (pos != fileSize) {
        err << "error: trailing malformed archive data at offset " << pos << " in '" << path
            << "'\n";
        return false;
    }

    // Second pass: build member list (non-special members only).
    // BSD long names are already resolved; just adjust data offsets.
    for (auto &rm : rawMembers) {
        if (rm.isSpecial)
            continue;

        size_t actualDataOffset = rm.dataOffset + rm.bsdNameLen;
        size_t actualDataSize = rm.dataSize - rm.bsdNameLen;

        ar.members.push_back({rm.name, actualDataOffset, actualDataSize});
    }

    // Build symbol → member index map.
    // rawSymbols has (symbol name, file offset of member header).
    // We need to map file offsets → member indices.
    std::unordered_map<size_t, size_t> headerOffsetToIdx;
    size_t memberIdx = 0;
    for (size_t i = 0; i < rawMembers.size(); ++i) {
        if (!rawMembers[i].isSpecial) {
            headerOffsetToIdx[rawMembers[i].headerOffset] = memberIdx;
            ++memberIdx;
        }
    }

    for (const auto &[symName, fileOffset] : rawSymbols) {
        auto it = headerOffsetToIdx.find(fileOffset);
        if (it != headerOffsetToIdx.end()) {
            ar.symbolIndex.emplace(symName, it->second);
            ar.symbolCandidates[symName].push_back(it->second);
        } else {
            err << "error: archive symbol '" << symName << "' references missing member offset "
                << fileOffset << " in '" << path << "'\n";
            return false;
        }
    }

    // Deduplicate candidate member indices per symbol. MSVC/COFF archives carry
    // two linker members (the GNU-style "/" and the second "/") that list the same
    // symbols, so each candidate would otherwise be recorded twice.
    for (auto &entry : ar.symbolCandidates) {
        auto &members = entry.second;
        std::vector<size_t> deduped;
        deduped.reserve(members.size());
        for (size_t m : members)
            if (std::find(deduped.begin(), deduped.end(), m) == deduped.end())
                deduped.push_back(m);
        members = std::move(deduped);
    }

    return true;
}

// cppcheck-suppress unusedFunction
/// @copydoc extractMember(const Archive &, const ArchiveMember &)
[[maybe_unused]] std::vector<uint8_t> extractMember(const Archive &ar,
                                                    const ArchiveMember &member) {
    if (!checkedRange(member.dataOffset, member.dataSize, ar.data.size()))
        return {};
    return std::vector<uint8_t>(
        ar.data.begin() + static_cast<std::ptrdiff_t>(member.dataOffset),
        ar.data.begin() + static_cast<std::ptrdiff_t>(member.dataOffset + member.dataSize));
}

/// @copydoc memberDataView(const Archive &, const ArchiveMember &)
ArchiveMemberView memberDataView(const Archive &ar, const ArchiveMember &member) {
    if (!checkedRange(member.dataOffset, member.dataSize, ar.data.size()))
        return {};
    return ArchiveMemberView{ar.data.data() + member.dataOffset, member.dataSize};
}

} // namespace zanna::codegen::linker
