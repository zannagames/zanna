//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/ExeWriterUtil.hpp
// Purpose: Shared utilities for executable file writers (ELF, Mach-O, PE).
//          Provides endianness encoding, padding, and entry point resolution.
// Key invariants:
//   - All encoding functions append to a vector<uint8_t> buffer
//   - Little-endian: host byte order on supported platforms (x86-64, AArch64)
//   - Big-endian: used for Mach-O code signature (network byte order)
//   - ULEB128: unsigned LEB128 encoding for Mach-O bind/rebase opcodes
// Ownership/Lifetime:
//   - Stateless inline utilities; encoding mutates caller-owned buffers and
//     the file helper creates and replaces filesystem entries.
// Links: codegen/common/linker/ElfExeWriter.cpp,
//        codegen/common/linker/MachOExeWriter.cpp,
//        codegen/common/linker/PeExeWriter.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ExeWriterUtil.hpp
 * @brief Supplies byte encoding, section grouping, entry lookup, and safe
 *        output replacement shared by executable writers.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"
#include "common/Filesystem.hpp"
#include "common/PlatformCapabilities.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Little-endian and big-endian encoding utilities for binary writers.
namespace encoding {

/// @brief Appends a 16-bit value in little-endian byte order.
/// @param buf Destination byte buffer.
/// @param v Value to encode.
inline void writeLE16(std::vector<uint8_t> &buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
}

/// @brief Appends a 32-bit value in little-endian byte order.
/// @param buf Destination byte buffer.
/// @param v Value to encode.
inline void writeLE32(std::vector<uint8_t> &buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 24));
}

/// @brief Appends a 64-bit value in little-endian byte order.
/// @param buf Destination byte buffer.
/// @param v Value to encode.
inline void writeLE64(std::vector<uint8_t> &buf, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        buf.push_back(static_cast<uint8_t>(v >> (i * 8)));
}

/// @brief Appends a 32-bit value in big-endian network byte order.
/// @param buf Destination byte buffer.
/// @param v Value to encode.
inline void writeBE32(std::vector<uint8_t> &buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v >> 24));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v));
}

/// @brief Appends a 64-bit value in big-endian byte order.
/// @param buf Destination byte buffer.
/// @param v Value to encode.
inline void writeBE64(std::vector<uint8_t> &buf, uint64_t v) {
    writeBE32(buf, static_cast<uint32_t>(v >> 32));
    writeBE32(buf, static_cast<uint32_t>(v));
}

/// @brief Appends an unsigned LEB128 value.
/// @param buf Destination byte buffer.
/// @param val Value to encode, consumed a group at a time.
inline void writeULEB128(std::vector<uint8_t> &buf, uint64_t val) {
    do {
        uint8_t byte = val & 0x7F;
        val >>= 7;
        if (val != 0)
            byte |= 0x80;
        buf.push_back(byte);
    } while (val != 0);
}

/// @brief Appends a signed LEB128 value.
/// @param buf Destination byte buffer.
/// @param val Value to encode.
inline void writeSLEB128(std::vector<uint8_t> &buf, int64_t val) {
    bool more = true;
    while (more) {
        uint8_t byte = static_cast<uint8_t>(val) & 0x7F;
        val >>= 7;
        if ((val == 0 && (byte & 0x40) == 0) || (val == -1 && (byte & 0x40) != 0))
            more = false;
        else
            byte |= 0x80;
        buf.push_back(byte);
    }
}

/// @brief Appends a requested number of zero bytes.
/// @param buf Destination byte buffer.
/// @param count Number of padding bytes.
inline void writePad(std::vector<uint8_t> &buf, size_t count) {
    buf.insert(buf.end(), count, 0);
}

/// @brief Appends a C string into a zero-padded fixed-width field.
/// @param buf Destination byte buffer.
/// @param s NUL-terminated source string.
/// @param maxLen Exact field width in bytes; the terminator itself is not copied.
/// @throws std::length_error If the source contains more than @p maxLen bytes.
inline void writeStr(std::vector<uint8_t> &buf, const char *s, size_t maxLen) {
    size_t len = std::strlen(s);
    if (len > maxLen)
        throw std::length_error("fixed-width binary string field overflow");
    buf.insert(buf.end(), s, s + len);
    if (len < maxLen)
        writePad(buf, maxLen - len);
}

/// @brief Pads a buffer with zeros until it reaches a target size.
/// @param buf Destination byte buffer.
/// @param targetSize Desired minimum size.
/// @details Does nothing when the buffer is already at or beyond @p targetSize.
inline void padTo(std::vector<uint8_t> &buf, size_t targetSize) {
    if (buf.size() < targetSize)
        buf.insert(buf.end(), targetSize - buf.size(), 0);
}

} // namespace encoding

/// @brief Resolves the `main` or `_main` entry-point symbol from the layout.
/// @param layout Link layout containing the resolved global symbol table.
/// @return The resolved virtual address, or 0 if not found.
inline uint64_t resolveMainAddress(const LinkLayout &layout) {
    auto it = layout.globalSyms.find("main");
    if (it != layout.globalSyms.end())
        return it->second.resolvedAddr;
    it = layout.globalSyms.find("_main");
    if (it != layout.globalSyms.end())
        return it->second.resolvedAddr;
    return 0;
}

/// @brief Partitions allocatable, nonempty sections into read-only and data groups.
/// @details Writable, explicitly data-segment, zero-fill, and TLS sections go
///          to @p dataIndices; other allocatable sections go to
///          @p textIndices. Existing vector contents are preserved and new
///          indices are appended.
/// @param layout Layout whose sections are classified.
/// @param textIndices Receives executable/read-only section indices.
/// @param dataIndices Receives writable/data/TLS section indices.
inline void classifySections(const LinkLayout &layout,
                             std::vector<size_t> &textIndices,
                             std::vector<size_t> &dataIndices) {
    for (size_t i = 0; i < layout.sections.size(); ++i) {
        if (outputSectionMemSize(layout.sections[i]) == 0)
            continue;
        if (!layout.sections[i].alloc)
            continue; // Skip non-alloc sections (e.g., .debug_line).
        if (layout.sections[i].writable || layout.sections[i].dataSegment ||
            layout.sections[i].zeroFill || layout.sections[i].tls)
            dataIndices.push_back(i);
        else
            textIndices.push_back(i);
    }
}

/// @brief One placed definition selected for an executable's symbol table.
struct LocalSymbolRecord {
    std::string name;       ///< Logical (unmangled) symbol name.
    uint64_t addr = 0;      ///< Final virtual address.
    size_t sectionSlot = 0; ///< Zero-based position in the caller's section order.
    uint64_t size = 0;      ///< Bytes to the next record in the same section or to its end.
};

/// @brief Collects every placed global/weak definition as a local symbol record.
/// @details Only definitions with a valid, section-relative final address that
///          falls inside one of @p sectionOrder's sections are kept. Records are
///          sorted by address then name so the emitted table is deterministic,
///          and each record's size runs to the next record in the same section
///          (or the section end) so ELF consumers get usable extents.
/// @param layout Final layout with resolved global symbols.
/// @param sectionOrder Layout section indices in the order the writer numbers them.
/// @param skipEntry When true, `main` / `_main` are omitted (the writer publishes
///        the entry point separately as an external definition).
/// @return Sorted records; empty when nothing is placed.
inline std::vector<LocalSymbolRecord> collectLocalSymbolRecords(
    const LinkLayout &layout, const std::vector<size_t> &sectionOrder, bool skipEntry) {
    std::vector<LocalSymbolRecord> records;
    for (const auto &[name, entry] : layout.globalSyms) {
        if (name.empty())
            continue;
        if (entry.binding != GlobalSymEntry::Global && entry.binding != GlobalSymEntry::Weak)
            continue;
        if (!entry.resolvedAddrValid || entry.absolute)
            continue;
        if (skipEntry && (name == "main" || name == "_main"))
            continue;
        for (size_t slot = 0; slot < sectionOrder.size(); ++slot) {
            const auto &sec = layout.sections[sectionOrder[slot]];
            const uint64_t size = static_cast<uint64_t>(outputSectionMemSize(sec));
            if (size == 0 || entry.resolvedAddr < sec.virtualAddr ||
                entry.resolvedAddr - sec.virtualAddr >= size)
                continue;
            records.push_back({name, entry.resolvedAddr, slot, 0});
            break;
        }
    }
    std::sort(
        records.begin(), records.end(), [](const LocalSymbolRecord &a, const LocalSymbolRecord &b) {
            if (a.addr != b.addr)
                return a.addr < b.addr;
            return a.name < b.name;
        });
    for (size_t i = 0; i < records.size(); ++i) {
        const auto &sec = layout.sections[sectionOrder[records[i].sectionSlot]];
        uint64_t next = sec.virtualAddr + static_cast<uint64_t>(outputSectionMemSize(sec));
        if (i + 1 < records.size() && records[i + 1].sectionSlot == records[i].sectionSlot &&
            records[i + 1].addr > records[i].addr)
            next = records[i + 1].addr;
        records[i].size = next - records[i].addr;
    }
    return records;
}

/// @brief Computes the virtual-address span occupied by a section group.
/// @details Uses the first indexed section as the segment base and includes VA
///          gaps when finding the furthest section end.
/// @param layout Layout containing the indexed sections.
/// @param indices Ordered section indices belonging to one segment.
/// @return Byte distance from the first section's VA to the furthest end, or
///         zero when @p indices is empty.
/// @throws std::length_error If a section precedes the selected base or an
///         address/range cannot be represented.
inline size_t computeSegmentSpan(const LinkLayout &layout, const std::vector<size_t> &indices) {
    if (indices.empty())
        return 0;
    uint64_t firstVA = layout.sections[indices.front()].virtualAddr;
    size_t span = 0;
    for (size_t idx : indices) {
        const auto &sec = layout.sections[idx];
        if (sec.virtualAddr < firstVA)
            throw std::length_error("section virtual address precedes segment base");
        const size_t memSize = outputSectionMemSize(sec);
        if (memSize > std::numeric_limits<uint64_t>::max() - sec.virtualAddr)
            throw std::length_error("section virtual address range overflows");
        const uint64_t endVA = sec.virtualAddr + memSize;
        const uint64_t endOff64 = endVA - firstVA;
        if (endOff64 > std::numeric_limits<size_t>::max())
            throw std::length_error("segment span exceeds addressable size");
        size_t endOff = static_cast<size_t>(endOff64);
        if (endOff > span)
            span = endOff;
    }
    return span;
}

/// @brief Writes a binary through a protected temporary directory and replaces the target.
/// @details A same-directory rename supplies atomic replacement where the host
///          supports it. If direct replacement is refused, the implementation
///          moves the old target aside, installs the new file, and attempts to
///          restore the old target on failure. Temporary paths are retried with
///          unique nonces and cleaned on every completed attempt.
/// @param path UTF-8 destination path.
/// @param data Complete file bytes.
/// @param makeExecutable Whether POSIX execute permissions should be applied.
/// @param err Stream that receives file, permission, and replacement diagnostics.
/// @return `true` only when the final path names the complete new file.
inline bool writeBinaryFileAtomically(const std::string &path,
                                      const std::vector<uint8_t> &data,
                                      bool makeExecutable,
                                      std::ostream &err) {
    namespace fs = std::filesystem;

    /// @brief Write the complete output buffer to @p target and apply final mode bits.
    /// @details The stream API takes a signed byte count, so this rejects files
    ///          larger than std::streamsize can represent before narrowing. On
    ///          non-Windows hosts it also reports chmod-style permission failures
    ///          instead of silently returning a non-executable binary.
    /// @param target Temporary file path to create or truncate.
    /// @return `true` after all bytes and requested permissions are installed.
    auto writeDirect = [&](const fs::path &target) -> bool {
        if (data.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
            err << "error: output file '" << zanna::filesystem::pathToUtf8(target)
                << "' exceeds stream write size limit\n";
            return false;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            err << "error: cannot open '" << zanna::filesystem::pathToUtf8(target)
                << "' for writing\n";
            return false;
        }
        out.write(reinterpret_cast<const char *>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!out) {
            err << "error: write failed to '" << zanna::filesystem::pathToUtf8(target) << "'\n";
            return false;
        }
        out.close();
        if (!out) {
            err << "error: write failed to '" << zanna::filesystem::pathToUtf8(target) << "'\n";
            return false;
        }

        if constexpr (!zanna::platform::kHostWindows) {
            if (!makeExecutable)
                return true;
            std::error_code permEc;
            fs::permissions(target,
                            fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write |
                                fs::perms::group_read | fs::perms::group_exec |
                                fs::perms::others_read | fs::perms::others_exec,
                            permEc);
            if (permEc) {
                err << "error: cannot set executable permissions on '"
                    << zanna::filesystem::pathToUtf8(target) << "': " << permEc.message() << "\n";
                return false;
            }
        } else {
            (void)makeExecutable;
        }
        return true;
    };

    /// @brief Replace @p finalPath with @p tempPath without destroying the old file first.
    /// @details POSIX rename normally replaces the target atomically. When the
    ///          platform refuses to replace an existing target, this falls back to
    ///          moving the old file aside, installing the temp file, and restoring
    ///          the old file if installation fails.
    /// @param tempPath Complete new output file.
    /// @param finalPath Destination to replace.
    /// @return `true` when @p finalPath names the new file.
    auto replaceWithTemp = [&](const fs::path &tempPath, const fs::path &finalPath) -> bool {
        std::error_code renameEc;
        fs::rename(tempPath, finalPath, renameEc);
        if (!renameEc)
            return true;

        std::error_code existsEc;
        if (!fs::exists(finalPath, existsEc) || existsEc) {
            err << "error: cannot replace '" << path << "': " << renameEc.message() << "\n";
            return false;
        }

        fs::path backupPath = tempPath;
        backupPath += ".old";
        std::error_code cleanupEc;
        fs::remove(backupPath, cleanupEc);

        std::error_code backupEc;
        fs::rename(finalPath, backupPath, backupEc);
        if (backupEc) {
            err << "error: cannot move existing output '" << path
                << "' aside for replacement: " << backupEc.message() << "\n";
            return false;
        }

        renameEc.clear();
        fs::rename(tempPath, finalPath, renameEc);
        if (!renameEc) {
            fs::remove(backupPath, cleanupEc);
            return true;
        }

        std::error_code restoreEc;
        fs::rename(backupPath, finalPath, restoreEc);
        err << "error: cannot replace '" << path << "': " << renameEc.message();
        if (restoreEc)
            err << "; additionally failed to restore previous output: " << restoreEc.message();
        err << "\n";
        return false;
    };

    const fs::path finalPath = zanna::filesystem::pathFromUtf8(path);
    fs::path dir = finalPath.parent_path();
    if (dir.empty())
        dir = ".";

    static std::atomic<uint64_t> nonce{0};
    for (uint32_t attempt = 0; attempt < 32; ++attempt) {
        const uint64_t seed =
            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
            nonce.fetch_add(1, std::memory_order_relaxed);
        fs::path tempName = finalPath.filename();
        tempName += ".tmpdir.";
        tempName += std::to_string(seed + attempt);
        const fs::path tempDir = dir / tempName;
        const fs::path tempPath = tempDir / finalPath.filename();

        std::error_code mkdirEc;
        if (!fs::create_directory(tempDir, mkdirEc)) {
            if (!mkdirEc)
                continue;
            err << "error: cannot create temporary output directory '"
                << zanna::filesystem::pathToUtf8(tempDir) << "': " << mkdirEc.message() << "\n";
            return false;
        }

        /// Removes the current attempt's temporary directory on a best-effort basis.
        auto cleanupTempDir = [&]() {
            std::error_code cleanupEc;
            fs::remove_all(tempDir, cleanupEc);
        };

        if constexpr (!zanna::platform::kHostWindows) {
            std::error_code permEc;
            fs::permissions(tempDir,
                            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                            fs::perm_options::replace,
                            permEc);
            if (permEc) {
                err << "error: cannot protect temporary output directory '"
                    << zanna::filesystem::pathToUtf8(tempDir) << "': " << permEc.message() << "\n";
                cleanupTempDir();
                return false;
            }
        }

        std::error_code existsEc;
        if (fs::exists(tempPath, existsEc)) {
            cleanupTempDir();
            continue;
        }

        if (!writeDirect(tempPath)) {
            cleanupTempDir();
            return false;
        }

        if (replaceWithTemp(tempPath, finalPath)) {
            cleanupTempDir();
            return true;
        }

        cleanupTempDir();
        return false;
    }

    err << "error: cannot allocate temporary output path for '" << path << "'\n";
    return false;
}

} // namespace zanna::codegen::linker
