//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PEBuilder.cpp
// Purpose: Emit PE32+ (x86-64) executables from scratch.
//
// Key invariants:
//   - DOS header at offset 0x0000, e_lfanew = 0x80.
//   - PE signature "PE\0\0" at offset 0x0080.
//   - COFF header at 0x0084, Optional header at 0x0098.
//   - Section headers start at 0x0188 (after 240-byte optional header).
//   - First section data at file offset 0x200 (or later, file-aligned).
//   - All sections virtually aligned to 0x1000.
//   - Import directory: ILT + IAT + HintName entries + DLL name strings.
//   - Resource directory: three-level tree (Type → Name → Language).
//
// Ownership/Lifetime:
//   - Pure function returning byte vector. No file I/O in buildPE.
//
// Links: PEBuilder.hpp, Microsoft PE Format specification
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements dependency-free PE32+ image, import, resource, and manifest serialization.
/// @details Produces deterministic AMD64/ARM64 executables with validated section
///          layout, import tables, Win32 resources, relocations, and optional overlays.

#include "PEBuilder.hpp"
#include "PkgUtils.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace zanna::pkg {

namespace {

constexpr uint32_t kFileAlignment = 0x200;
constexpr uint32_t kSectionAlignment = 0x1000;
constexpr uint64_t kImageBase = 0x0000000140000000ULL;

// DOS header is 64 bytes, DOS stub is 64 bytes = 128 bytes (0x80)
constexpr uint32_t kPESignatureOffset = 0x80;

// PE sig(4) + COFF(20) + OptHdr(240) = 264 bytes
// Section headers start at 0x80 + 264 = 0x0188
constexpr uint32_t kCoffHeaderSize = 20;
constexpr uint32_t kOptionalHeaderSize = 240;
constexpr uint32_t kSectionHeaderSize = 40;
constexpr uint32_t kNumDataDirectories = 16;

/// @brief Round up to alignment boundary.
/// @param value Value to round upward.
/// @param alignment Non-zero power-of-two boundary.
/// @return Smallest aligned value not less than `value`.
/// @throws std::runtime_error If alignment is invalid or rounding overflows.
uint32_t alignUp(uint32_t value, uint32_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0)
        throw std::runtime_error("PEBuilder: alignment must be a non-zero power of two");
    const uint32_t mask = alignment - 1;
    if ((value & mask) == 0)
        return value;
    if (value > std::numeric_limits<uint32_t>::max() - mask)
        throw std::runtime_error("PEBuilder: aligned size overflows 32-bit PE fields");
    return (value + mask) & ~mask;
}

/// @brief Cast `size_t` to `uint32_t`, throwing if the value exceeds 2^32-1.
/// PE32+ fields (VirtualSize, SizeOfRawData, offsets) are all 32-bit; any section or
/// blob that exceeds this limit would silently truncate without this guard.
/// @param value Host-size value to convert.
/// @param what Human-readable field description used in diagnostics.
/// @return Value represented as uint32_t.
/// @throws std::runtime_error If `value` exceeds the PE field width.
uint32_t checkedU32Size(size_t value, const char *what) {
    if (value > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error(std::string("PEBuilder: ") + what +
                                 " exceeds 32-bit PE field limits");
    return static_cast<uint32_t>(value);
}

/// @brief Add two `uint32_t` values, throwing on overflow.
/// Used when accumulating section VAs and file offsets to detect a pathological
/// image that would exceed the 4 GB PE32+ address space.
/// @param lhs First addend.
/// @param rhs Second addend.
/// @param what Human-readable quantity used in diagnostics.
/// @return Checked sum.
/// @throws std::runtime_error If the addition overflows.
uint32_t checkedAddU32(uint32_t lhs, uint32_t rhs, const char *what) {
    if (lhs > std::numeric_limits<uint32_t>::max() - rhs)
        throw std::runtime_error(std::string("PEBuilder: ") + what +
                                 " overflows 32-bit PE field limits");
    return lhs + rhs;
}

/// @brief Write a little-endian uint16_t to a buffer at given offset.
/// @param buf Mutable destination buffer.
/// @param offset Starting byte offset.
/// @param val Value to encode.
void putLE16(std::vector<uint8_t> &buf, size_t offset, uint16_t val) {
    buf[offset + 0] = static_cast<uint8_t>(val & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

/// @brief Write a little-endian uint32_t to a buffer at given offset.
/// @param buf Mutable destination buffer.
/// @param offset Starting byte offset.
/// @param val Value to encode.
void putLE32(std::vector<uint8_t> &buf, size_t offset, uint32_t val) {
    buf[offset + 0] = static_cast<uint8_t>(val & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    buf[offset + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    buf[offset + 3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

/// @brief Write a little-endian uint64_t to a buffer at given offset.
/// @param buf Mutable destination buffer.
/// @param offset Starting byte offset.
/// @param val Value to encode.
void putLE64(std::vector<uint8_t> &buf, size_t offset, uint64_t val) {
    putLE32(buf, offset, static_cast<uint32_t>(val & 0xFFFFFFFF));
    putLE32(buf, offset + 4, static_cast<uint32_t>(val >> 32));
}

/// @brief Append a little-endian uint16_t.
/// @param buf Destination buffer.
/// @param val Value to append.
void appendLE16(std::vector<uint8_t> &buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

/// @brief Append a little-endian uint32_t.
/// @param buf Destination buffer.
/// @param val Value to append.
void appendLE32(std::vector<uint8_t> &buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

/// @brief Pad buffer to alignment boundary with zeros.
/// @param buf Buffer to extend.
/// @param alignment Non-zero power-of-two byte alignment.
/// @throws std::runtime_error If alignment is invalid or the aligned size overflows.
void padTo(std::vector<uint8_t> &buf, uint32_t alignment) {
    size_t aligned = alignUp(checkedU32Size(buf.size(), "buffer size"), alignment);
    buf.resize(aligned, 0);
}

/// @brief Section descriptor during layout.
struct SectionLayout {
    char name[8]{};
    uint32_t virtualSize{0};
    uint32_t virtualAddress{0};
    uint32_t rawDataSize{0};
    uint32_t rawDataOffset{0};
    uint32_t characteristics{0};
    std::vector<uint8_t> data;
};

/// @brief Build the import directory tables for the .rdata section.
///
/// Layout within .rdata:
///   1. Import Directory Table: (N+1) entries × 20 bytes (null-terminated)
///   2. Import Lookup Tables: per-DLL array of 8-byte entries (null-terminated)
///   3. Hint/Name entries: per-function Hint(2) + ASCII name + pad
///   4. DLL name strings: null-terminated ASCII
///   5. Import Address Tables: mirrors ILT layout
///
/// Returns: (rdataBytes, importDirRVA, importDirSize, iatRVA, iatSize)
struct ImportResult {
    std::vector<uint8_t> data;
    uint32_t importDirRVA{0};
    uint32_t importDirSize{0};
    uint32_t iatRVA{0};
    uint32_t iatSize{0};
};

/// @brief Build the import directory tables (IDT, ILT, Hint/Name, DLL name strings, IAT)
/// for the .rdata section. Returns the complete rdata bytes plus RVA/size metadata.
/// @param imports Ordered DLL/function imports.
/// @param rdataRVA RVA assigned to the resulting `.rdata` section.
/// @return Serialized tables and import/IAT directory metadata.
/// @throws std::runtime_error If an encoded size or offset exceeds PE limits.
ImportResult buildImportTables(const std::vector<PEImport> &imports, uint32_t rdataRVA) {
    ImportResult result{};
    if (imports.empty())
        return result;

    auto &buf = result.data;

    // Count total functions
    size_t totalFuncs = 0;
    for (const auto &imp : imports)
        totalFuncs += imp.functions.size();
    if (totalFuncs > std::numeric_limits<uint32_t>::max() / 8u)
        throw std::runtime_error("PE import table has too many imported functions");

    // Phase 1: Calculate layout sizes
    uint32_t idtSize = static_cast<uint32_t>((imports.size() + 1) * 20); // +1 null

    // ILT: per DLL, (numFuncs + 1) * 8 bytes (null-terminated array of 8-byte entries)
    uint32_t iltSize = 0;
    for (const auto &imp : imports)
        iltSize += static_cast<uint32_t>((imp.functions.size() + 1) * 8);

    // Hint/Name: per function, 2 + strlen + 1 + pad
    uint32_t hintNameSize = 0;
    for (const auto &imp : imports)
        for (const auto &fn : imp.functions)
            hintNameSize = checkedAddU32(
                hintNameSize,
                alignUp(checkedU32Size(2 + fn.size() + 1, "import hint/name entry"), 2),
                "import hint/name table size");

    // DLL name strings
    uint32_t dllNameSize = 0;
    for (const auto &imp : imports)
        dllNameSize += static_cast<uint32_t>(imp.dllName.size() + 1);

    // IAT: same layout as ILT
    uint32_t iatSize = iltSize;

    // Offsets within rdata section (all relative to section start)
    uint32_t idtOff = 0;
    uint32_t iltOff = idtOff + idtSize;
    uint32_t hintOff = iltOff + iltSize;
    uint32_t dllNameOff = hintOff + hintNameSize;
    uint32_t iatOff = dllNameOff + dllNameSize;
    // Align IAT to 8 bytes
    iatOff = alignUp(iatOff, 8);
    uint32_t totalSize = iatOff + iatSize;

    buf.resize(totalSize, 0);

    // Phase 2: Write ILT entries, Hint/Name entries, DLL name strings
    uint32_t curIltOff = iltOff;
    uint32_t curHintOff = hintOff;
    uint32_t curDllNameOff = dllNameOff;
    uint32_t curIatOff = iatOff;

    for (size_t i = 0; i < imports.size(); i++) {
        const auto &imp = imports[i];

        // IDT entry (20 bytes at idtOff + i*20)
        uint32_t idtEntryOff = idtOff + static_cast<uint32_t>(i) * 20;
        putLE32(buf, idtEntryOff + 0, rdataRVA + curIltOff);      // OriginalFirstThunk (ILT)
        putLE32(buf, idtEntryOff + 4, 0);                         // TimeDateStamp
        putLE32(buf, idtEntryOff + 8, 0);                         // ForwarderChain
        putLE32(buf, idtEntryOff + 12, rdataRVA + curDllNameOff); // Name RVA
        putLE32(buf, idtEntryOff + 16, rdataRVA + curIatOff);     // FirstThunk (IAT)

        // DLL name string
        std::memcpy(buf.data() + curDllNameOff, imp.dllName.c_str(), imp.dllName.size() + 1);
        curDllNameOff += static_cast<uint32_t>(imp.dllName.size() + 1);

        // ILT + IAT + Hint/Name entries for this DLL
        for (const auto &fn : imp.functions) {
            // Hint/Name entry at curHintOff
            uint32_t hintNameRVA = rdataRVA + curHintOff;
            putLE16(buf, curHintOff, 0); // Hint = 0 (lookup by name)
            std::memcpy(buf.data() + curHintOff + 2, fn.c_str(), fn.size() + 1);
            uint32_t entryLen =
                alignUp(checkedU32Size(2 + fn.size() + 1, "import hint/name entry"), 2);
            curHintOff += entryLen;

            // ILT entry (8 bytes): bit 63 = 0 (import by name), bits 30:0 = HintName RVA
            putLE32(buf, curIltOff, hintNameRVA);
            putLE32(buf, curIltOff + 4, 0);
            curIltOff += 8;

            // IAT entry (mirrors ILT before binding)
            putLE32(buf, curIatOff, hintNameRVA);
            putLE32(buf, curIatOff + 4, 0);
            curIatOff += 8;
        }

        // Null terminator for ILT and IAT
        curIltOff += 8; // already zeroed
        curIatOff += 8;
    }

    // IDT null terminator entry (20 zeros) — already zeroed

    result.importDirRVA = rdataRVA + idtOff;
    result.importDirSize = idtSize;
    result.iatRVA = rdataRVA + iatOff;
    result.iatSize = iatSize;

    return result;
}

/// @brief Build a .rsrc section with RT_MANIFEST and optional RT_ICON resources.
///
/// Resource Directory is a three-level tree: Type → Name/ID → Language.
/// Each level: Directory header (16 bytes) + N entries (8 bytes each).
/// Leaf entries point to Resource Data Entries (16 bytes) which hold RVA+size.
///
/// When iconData is provided, we parse the ICO header and embed:
///   - RT_ICON (type 3): one resource per icon image, IDs 1..N
///   - RT_GROUP_ICON (type 14): GRPICONDIR header linking to RT_ICONs
///   - RT_MANIFEST (type 24): UAC manifest XML
struct ResourceResult {
    std::vector<uint8_t> data;
    uint32_t rsrcRVA{0}; // filled in by caller
    uint32_t rsrcSize{0};
};

/// @brief A single resource data item to embed.
struct ResItem {
    uint16_t typeId{0};        ///< RT_ICON=3, RT_GROUP_ICON=14, RT_MANIFEST=24
    uint16_t nameId{0};        ///< Resource name/ID within the type
    std::vector<uint8_t> data; ///< Raw resource data
};

/// @brief Append a VS_VERSIONINFO node header (length placeholder + key).
/// @details Writes a zero wLength placeholder (back-patched later by
///          patchVersionLength), the wValueLength, the wType, the UTF-16
///          NUL-terminated key, and 32-bit padding.
/// @param buf Resource buffer being built.
/// @param valueLength wValueLength field (in words for text, bytes for binary).
/// @param type wType field (0 = binary value, 1 = text value).
/// @param key Node key (e.g. "VS_VERSION_INFO", "CompanyName").
/// @return Byte offset of this node's header (for the matching patchVersionLength).
size_t appendVersionHeader(std::vector<uint8_t> &buf,
                           uint16_t valueLength,
                           uint16_t type,
                           const std::string &key) {
    const size_t start = buf.size();
    appendLE16(buf, 0);
    appendLE16(buf, valueLength);
    appendLE16(buf, type);
    for (char ch : key)
        appendLE16(buf, static_cast<uint16_t>(static_cast<unsigned char>(ch)));
    appendLE16(buf, 0);
    padTo(buf, 4);
    return start;
}

/// @brief Back-patch a node's wLength field once all its children are written.
/// @param buf Resource buffer being built.
/// @param start Offset returned by the matching appendVersionHeader call.
/// @throws std::runtime_error if the node exceeds the 16-bit length limit.
void patchVersionLength(std::vector<uint8_t> &buf, size_t start) {
    const size_t len = buf.size() - start;
    if (len > std::numeric_limits<uint16_t>::max())
        throw std::runtime_error("PEBuilder: VERSIONINFO resource is too large");
    putLE16(buf, start, static_cast<uint16_t>(len));
}

/// @brief Append a UTF-16LE, NUL-terminated value string to the resource buffer.
/// @param buf Resource buffer to extend.
/// @param value UTF-8 text to transcode.
void appendUtf16Value(std::vector<uint8_t> &buf, const std::string &value) {
    for (uint16_t ch : utf8ToUtf16CodeUnits(value))
        appendLE16(buf, ch);
    appendLE16(buf, 0);
}

/// @brief Pack two of the four version words into a 32-bit DWORD (high<<16|low).
/// @param parts The four-element version (a.b.c.d).
/// @param high Index of the high word (0 for a.b, 2 for c.d).
uint32_t versionDword(const std::array<uint16_t, 4> &parts, size_t high) {
    return (static_cast<uint32_t>(parts[high]) << 16) | parts[high + 1];
}

/// @brief Append a "String" node (key/value text pair) to the StringTable.
/// @details No-op for an empty value. Writes the header with the value's UTF-16
///          word count, the value, padding, and back-patches the node length.
/// @param buf Resource buffer being built.
/// @param key String name (e.g. "CompanyName").
/// @param value Text value to store.
void appendVersionString(std::vector<uint8_t> &buf,
                         const std::string &key,
                         const std::string &value) {
    if (value.empty())
        return;
    const size_t valueUnits = utf16CodeUnitCountFromUtf8(value) + 1;
    if (valueUnits > std::numeric_limits<uint16_t>::max())
        throw std::runtime_error("PEBuilder: VERSIONINFO string is too large");
    const size_t start = appendVersionHeader(buf, static_cast<uint16_t>(valueUnits), 1, key);
    appendUtf16Value(buf, value);
    padTo(buf, 4);
    patchVersionLength(buf, start);
}

/// @brief Build a complete VS_VERSIONINFO resource blob from @p info.
/// @details Emits the root node with the fixed VS_FIXEDFILEINFO block (file and
///          product versions), a StringFileInfo/StringTable carrying the textual
///          metadata fields, and a VarFileInfo Translation block (US English,
///          Unicode). All node lengths are back-patched after their children.
/// @param info Version metadata to encode.
/// @return The RT_VERSION resource bytes.
std::vector<uint8_t> buildVersionInfoResource(const PEVersionInfo &info) {
    std::vector<uint8_t> buf;
    const size_t root = appendVersionHeader(buf, 52, 0, "VS_VERSION_INFO");
    appendLE32(buf, 0xFEEF04BDu);
    appendLE32(buf, 0x00010000u);
    appendLE32(buf, versionDword(info.fileVersion, 0));
    appendLE32(buf, versionDword(info.fileVersion, 2));
    appendLE32(buf, versionDword(info.productVersion, 0));
    appendLE32(buf, versionDword(info.productVersion, 2));
    appendLE32(buf, 0x0000003Fu);
    appendLE32(buf, 0);
    appendLE32(buf, 0x00000004u);
    appendLE32(buf, 0);
    appendLE32(buf, 0);
    appendLE32(buf, 0);
    appendLE32(buf, 0);
    padTo(buf, 4);

    const size_t stringFileInfo = appendVersionHeader(buf, 0, 1, "StringFileInfo");
    const size_t stringTable = appendVersionHeader(buf, 0, 1, "040904b0");
    appendVersionString(buf, "CompanyName", info.companyName);
    appendVersionString(buf, "FileDescription", info.fileDescription);
    appendVersionString(buf, "FileVersion", info.fileVersionText);
    appendVersionString(buf, "InternalName", info.internalName);
    appendVersionString(buf, "OriginalFilename", info.originalFilename);
    appendVersionString(buf, "ProductName", info.productName);
    appendVersionString(buf, "ProductVersion", info.productVersionText);
    patchVersionLength(buf, stringTable);
    patchVersionLength(buf, stringFileInfo);

    const size_t varFileInfo = appendVersionHeader(buf, 0, 1, "VarFileInfo");
    const size_t translation = appendVersionHeader(buf, 4, 0, "Translation");
    appendLE16(buf, 0x0409);
    appendLE16(buf, 0x04b0);
    padTo(buf, 4);
    patchVersionLength(buf, translation);
    patchVersionLength(buf, varFileInfo);
    patchVersionLength(buf, root);
    return buf;
}

/// @brief Parse ICO data into RT_ICON + RT_GROUP_ICON resource items.
/// @param ico Complete candidate ICO bytes.
/// @param items Resource list extended only when the ICO structure is valid.
void parseIcoToResources(const std::vector<uint8_t> &ico, std::vector<ResItem> &items) {
    if (ico.size() < 6)
        return;
    if (ico[0] != 0 || ico[1] != 0 || ico[2] != 1 || ico[3] != 0)
        return;

    // ICONDIR: reserved(2) + type(2) + count(2)
    uint16_t count = static_cast<uint16_t>(ico[4] | (ico[5] << 8));
    if (count == 0 || ico.size() < 6u + count * 16u)
        return;

    struct IconImage {
        size_t entryOff;
        uint32_t size;
        uint32_t offset;
        uint16_t id;
    };

    std::vector<IconImage> images;
    images.reserve(count);
    for (uint16_t i = 0; i < count; i++) {
        const size_t entryOff = 6 + static_cast<size_t>(i) * 16u;
        const uint32_t imgSize =
            static_cast<uint32_t>(ico[entryOff + 8] | (ico[entryOff + 9] << 8) |
                                  (ico[entryOff + 10] << 16) | (ico[entryOff + 11] << 24));
        const uint32_t imgOffset =
            static_cast<uint32_t>(ico[entryOff + 12] | (ico[entryOff + 13] << 8) |
                                  (ico[entryOff + 14] << 16) | (ico[entryOff + 15] << 24));
        const uint64_t end = static_cast<uint64_t>(imgOffset) + imgSize;
        if (imgSize == 0 || imgOffset > ico.size() || end > ico.size())
            return;
        images.push_back(IconImage{entryOff, imgSize, imgOffset, static_cast<uint16_t>(i + 1)});
    }

    // Build GRPICONDIR: same as ICONDIR but entries use nID instead of dwImageOffset
    // GRPICONDIR = ICONDIR(6) + GRPICONDIRENTRY[count](14 each)
    std::vector<uint8_t> grpIcon;
    // Copy ICONDIR header (6 bytes)
    grpIcon.insert(grpIcon.end(), ico.begin(), ico.begin() + 6);

    for (const auto &image : images) {
        size_t entryOff = image.entryOff;
        // ICONDIRENTRY: w(1) h(1) colorCount(1) reserved(1) planes(2) bitCount(2)
        //               sizeInBytes(4) fileOffset(4) = 16 bytes
        // GRPICONDIRENTRY: same first 12 bytes, but last 2 bytes = nID (uint16)
        // instead of last 4 bytes = dwImageOffset
        grpIcon.insert(grpIcon.end(), ico.begin() + entryOff, ico.begin() + entryOff + 12);
        uint16_t iconId = image.id;
        grpIcon.push_back(static_cast<uint8_t>(iconId & 0xFF));
        grpIcon.push_back(static_cast<uint8_t>((iconId >> 8) & 0xFF));

        // Extract the actual icon image data
        ResItem icon;
        icon.typeId = 3; // RT_ICON
        icon.nameId = iconId;
        icon.data.assign(ico.begin() + image.offset, ico.begin() + image.offset + image.size);
        items.push_back(std::move(icon));
    }

    // Add RT_GROUP_ICON resource
    ResItem grp;
    grp.typeId = 14; // RT_GROUP_ICON
    grp.nameId = 1;
    grp.data = std::move(grpIcon);
    items.push_back(std::move(grp));
}

/// @brief Build the Win32 three-level resource directory tree (.rsrc section).
/// Tree: Type → Name/ID → Language (always 0x0409 en-US).
/// Items are sorted by type ID because the Windows loader binary-searches the type directory;
/// unsorted entries cause lookup failures at runtime.
/// @param manifest Optional RT_MANIFEST XML bytes.
/// @param iconData Optional ICO file split into group/icon resources.
/// @param versionInfo Optional VERSIONINFO metadata.
/// @param rsrcRVA RVA assigned to the resource section.
/// @return Serialized resource directory, data entries, blobs, and size metadata.
/// @throws std::runtime_error If resource counts, sizes, or offsets exceed format limits.
ResourceResult buildResourceSection(const std::string &manifest,
                                    const std::vector<uint8_t> &iconData,
                                    const PEVersionInfo &versionInfo,
                                    uint32_t rsrcRVA) {
    ResourceResult result{};

    // Collect all resource items
    std::vector<ResItem> items;

    if (!iconData.empty())
        parseIcoToResources(iconData, items);

    if (versionInfo.enabled) {
        ResItem version;
        version.typeId = 16; // RT_VERSION
        version.nameId = 1;
        version.data = buildVersionInfoResource(versionInfo);
        items.push_back(std::move(version));
    }

    if (!manifest.empty()) {
        ResItem man;
        man.typeId = 24; // RT_MANIFEST
        man.nameId = 1;
        man.data.assign(manifest.begin(), manifest.end());
        items.push_back(std::move(man));
    }

    if (items.empty())
        return result;

    // Group items by type
    struct TypeGroup {
        uint16_t typeId{0};
        std::vector<size_t> itemIndices; // indices into items[]
    };

    std::vector<TypeGroup> types;
    for (size_t i = 0; i < items.size(); i++) {
        bool found = false;
        for (auto &tg : types) {
            if (tg.typeId == items[i].typeId) {
                tg.itemIndices.push_back(i);
                found = true;
                break;
            }
        }
        if (!found)
            types.push_back({items[i].typeId, {i}});
    }

    /// @brief Order resource-type groups by the numeric identifier required by Windows.
    /// @param a Left-hand resource-type group.
    /// @param b Right-hand resource-type group.
    /// @return `true` when `a` must precede `b`.
    std::sort(types.begin(), types.end(), [](const TypeGroup &a, const TypeGroup &b) {
        return a.typeId < b.typeId;
    });

    // Calculate layout:
    // Level 1 (Type dir):    16 + types.size()*8
    // Level 2 (Name dirs):   per type: 16 + numItems*8
    // Level 3 (Lang dirs):   per item: 16 + 1*8
    // Data entries:          per item: 16
    // Data blobs:            per item: alignUp(size, 4)

    uint32_t numTypes = static_cast<uint32_t>(types.size());
    uint32_t totalItems = static_cast<uint32_t>(items.size());

    uint32_t typeDirSize = 16 + numTypes * 8;
    uint32_t nameDirsSize = 0;
    for (const auto &tg : types)
        nameDirsSize += 16 + static_cast<uint32_t>(tg.itemIndices.size()) * 8;
    uint32_t langDirsSize = totalItems * (16 + 8); // one lang dir per item
    uint32_t dataEntriesSize = totalItems * 16;
    uint32_t dirTreeSize = typeDirSize + nameDirsSize + langDirsSize + dataEntriesSize;

    // Calculate total data size
    uint32_t dataSize = 0;
    for (const auto &item : items)
        dataSize = checkedAddU32(dataSize,
                                 alignUp(checkedU32Size(item.data.size(), "resource data item"), 4),
                                 "resource data size");

    uint32_t totalSize = dirTreeSize + dataSize;
    auto &buf = result.data;
    buf.resize(totalSize, 0);

    // ─── Write directory tree ──────────────────────────────────────────

    uint32_t off = 0; // current write offset

    // Level 1: Type directory
    uint32_t typeDirOff = off;
    putLE16(buf, typeDirOff + 14, static_cast<uint16_t>(numTypes));
    off += 16;

    // Reserve space for type entries (filled below after we know name dir offsets)
    uint32_t typeEntriesOff = off;
    off += numTypes * 8;

    // Level 2: Name directories (one per type)
    std::vector<uint32_t> nameDirOffsets(numTypes);
    std::vector<std::vector<uint32_t>> langDirOffsetsPerType(numTypes);

    for (uint32_t t = 0; t < numTypes; t++) {
        nameDirOffsets[t] = off;
        uint32_t numNames = static_cast<uint32_t>(types[t].itemIndices.size());
        putLE16(buf, off + 14, static_cast<uint16_t>(numNames));
        off += 16;

        // Reserve space for name entries
        langDirOffsetsPerType[t].resize(numNames);
        off += numNames * 8;
    }

    // Level 3: Language directories (one per item)
    for (uint32_t t = 0; t < numTypes; t++) {
        for (uint32_t n = 0; n < types[t].itemIndices.size(); n++) {
            langDirOffsetsPerType[t][n] = off;
            putLE16(buf, off + 14, 1);      // 1 language entry
            putLE32(buf, off + 16, 0x0409); // en-US
            off += 16 + 8;                  // dir header + 1 entry
        }
    }

    // Data entries (one per item)
    uint32_t dataEntryBaseOff = off;
    off += totalItems * 16;

    // Data blobs
    uint32_t dataBlobOff = off;

    // ─── Fill in offsets ───────────────────────────────────────────────

    // Type entries → name directories
    for (uint32_t t = 0; t < numTypes; t++) {
        uint32_t entryOff = typeEntriesOff + t * 8;
        putLE32(buf, entryOff, types[t].typeId);
        putLE32(buf, entryOff + 4, nameDirOffsets[t] | 0x80000000);
    }

    // Name entries → language directories
    uint32_t itemIdx = 0;
    for (uint32_t t = 0; t < numTypes; t++) {
        uint32_t nameEntryOff = nameDirOffsets[t] + 16;
        for (uint32_t n = 0; n < types[t].itemIndices.size(); n++) {
            size_t ii = types[t].itemIndices[n];
            putLE32(buf, nameEntryOff, items[ii].nameId);
            putLE32(buf, nameEntryOff + 4, langDirOffsetsPerType[t][n] | 0x80000000);
            nameEntryOff += 8;
        }

        // Lang entries → data entries
        for (uint32_t n = 0; n < types[t].itemIndices.size(); n++) {
            uint32_t langEntryOff = langDirOffsetsPerType[t][n] + 16 + 4;
            // Point to data entry (no high bit = leaf)
            putLE32(buf, langEntryOff, dataEntryBaseOff + itemIdx * 16);
            itemIdx++;
        }
    }

    // Data entries → data blobs
    uint32_t curBlobOff = dataBlobOff;
    for (uint32_t i = 0; i < totalItems; i++) {
        // Resolve flat index: iterate types in order
        size_t flatIdx = 0;
        for (uint32_t t = 0; t < numTypes; t++) {
            for (size_t n = 0; n < types[t].itemIndices.size(); n++) {
                if (flatIdx == i) {
                    size_t ii = types[t].itemIndices[n];
                    uint32_t deOff = dataEntryBaseOff + i * 16;
                    putLE32(buf, deOff + 0, rsrcRVA + curBlobOff); // RVA
                    putLE32(buf,
                            deOff + 4,
                            checkedU32Size(items[ii].data.size(), "resource data item"));
                    // Copy data
                    std::memcpy(
                        buf.data() + curBlobOff, items[ii].data.data(), items[ii].data.size());
                    curBlobOff = checkedAddU32(
                        curBlobOff,
                        alignUp(checkedU32Size(items[ii].data.size(), "resource data item"), 4),
                        "resource blob offset");
                    goto nextItem;
                }
                flatIdx++;
            }
        }
    nextItem:;
    }

    result.rsrcSize = totalSize;
    return result;
}

/// @brief Build a minimal base-relocation table.
///
/// The installer stub is position-independent: it calls imports and embedded
/// data through RIP-relative addressing and PE metadata stores RVAs, not VAs.
/// Windows still expects a relocation directory when DYNAMIC_BASE is set, so an
/// ABSOLUTE-only block is sufficient and gives the loader a valid table with no
/// fixups to apply.
std::vector<uint8_t> buildRelocSection() {
    std::vector<uint8_t> data;
    data.reserve(12);
    appendLE32(data, 0);  // Page RVA
    appendLE32(data, 12); // Block size
    appendLE16(data, 0);  // IMAGE_REL_BASED_ABSOLUTE
    appendLE16(data, 0);  // Padding entry keeps the block 4-byte aligned
    return data;
}

} // namespace

/// @brief Assemble a complete PE32+ binary from `params`.
/// Sections: `.text` (always), `.rdata` (if imports or rdataSection), `.rsrc` (if manifest or
/// icon),
/// `.reloc` (enabled by default).
/// RVAs and file offsets are computed in a single layout pass, then headers are filled in.
/// An optional raw overlay (e.g. a ZIP payload) is appended after all sections.
/// @param params Code/data, imports, resources, architecture, header settings, and overlay.
/// @return Complete PE image bytes.
/// @throws std::runtime_error If required input is absent, the entry point is invalid,
///         or any section/layout/resource value exceeds its PE representation.
std::vector<uint8_t> buildPE(const PEBuildParams &params) {
    // ─── Section planning ──────────────────────────────────────────────
    if (params.textSection.empty())
        throw std::runtime_error("PEBuilder: .text section must not be empty");
    if (params.entryPointOffset >= params.textSection.size())
        throw std::runtime_error("PEBuilder: entry point offset is outside the .text section");

    // Count sections: .text is required; .rdata if imports; .rsrc if manifest
    uint32_t numSections = 1; // .text
    bool hasRdata = !params.imports.empty() || !params.rdataSection.empty();
    bool hasRsrc =
        !params.manifest.empty() || !params.iconData.empty() || params.versionInfo.enabled;
    bool hasReloc = params.emitRelocations;
    if (hasRdata)
        numSections++;
    if (hasRsrc)
        numSections++;
    if (hasReloc)
        numSections++;

    // Headers size: DOS(0x80) + PE sig(4) + COFF(20) + OptHdr(240) + Sections(40*N)
    uint32_t headersRaw = kPESignatureOffset + 4 + kCoffHeaderSize + kOptionalHeaderSize +
                          kSectionHeaderSize * numSections;
    uint32_t headersAligned = alignUp(headersRaw, kFileAlignment);
    uint32_t headersVirtual = alignUp(headersRaw, kSectionAlignment);

    // ─── Layout sections ───────────────────────────────────────────────

    std::vector<SectionLayout> sections;

    // .text section
    {
        SectionLayout s{};
        std::memcpy(s.name, ".text\0\0\0", 8);
        s.data = params.textSection;
        s.virtualSize = checkedU32Size(s.data.size(), ".text section size");
        s.rawDataSize = alignUp(s.virtualSize, kFileAlignment);
        s.characteristics = 0x60000020; // CODE | EXECUTE | READ
        sections.push_back(std::move(s));
    }

    // .rdata section (import tables)
    ImportResult importResult{};
    if (hasRdata) {
        SectionLayout s{};
        std::memcpy(s.name, ".rdata\0\0", 8);

        // Calculate RVA for .rdata section
        uint32_t rdataVA = headersVirtual;
        for (const auto &sec : sections)
            rdataVA += alignUp(sec.virtualSize, kSectionAlignment);

        if (!params.imports.empty()) {
            importResult = buildImportTables(params.imports, rdataVA);
            s.data = importResult.data;
        }
        if (!params.rdataSection.empty()) {
            s.data.insert(s.data.end(), params.rdataSection.begin(), params.rdataSection.end());
        }
        s.virtualSize = checkedU32Size(s.data.size(), ".rdata section size");
        s.rawDataSize = alignUp(s.virtualSize, kFileAlignment);
        s.characteristics = 0x40000040; // INITIALIZED_DATA | READ
        sections.push_back(std::move(s));
    }

    // .rsrc section (manifest)
    uint32_t rsrcIdx = 0;
    ResourceResult rsrcResult{};
    if (hasRsrc) {
        rsrcIdx = static_cast<uint32_t>(sections.size());
        SectionLayout s{};
        std::memcpy(s.name, ".rsrc\0\0\0", 8);

        // Calculate RVA for .rsrc section
        uint32_t rsrcVA = headersVirtual;
        for (const auto &sec : sections)
            rsrcVA += alignUp(sec.virtualSize, kSectionAlignment);

        rsrcResult =
            buildResourceSection(params.manifest, params.iconData, params.versionInfo, rsrcVA);
        s.data = rsrcResult.data;
        s.virtualSize = checkedU32Size(s.data.size(), ".rsrc section size");
        s.rawDataSize = alignUp(s.virtualSize, kFileAlignment);
        s.characteristics = 0x40000040; // INITIALIZED_DATA | READ
        sections.push_back(std::move(s));
    }

    // .reloc section (base relocation directory)
    uint32_t relocIdx = 0;
    if (hasReloc) {
        relocIdx = static_cast<uint32_t>(sections.size());
        SectionLayout s{};
        std::memcpy(s.name, ".reloc\0\0", 8);
        s.data = buildRelocSection();
        s.virtualSize = checkedU32Size(s.data.size(), ".reloc section size");
        s.rawDataSize = alignUp(s.virtualSize, kFileAlignment);
        s.characteristics = 0x42000040; // INITIALIZED_DATA | READ | DISCARDABLE
        sections.push_back(std::move(s));
    }

    // Assign virtual addresses and file offsets
    uint32_t nextVA = headersVirtual;
    uint32_t nextFileOff = headersAligned;
    for (auto &s : sections) {
        s.virtualAddress = nextVA;
        s.rawDataOffset = nextFileOff;
        nextVA = checkedAddU32(nextVA, alignUp(s.virtualSize, kSectionAlignment), "image size");
        nextFileOff = checkedAddU32(nextFileOff, s.rawDataSize, "file size");
    }

    uint32_t sizeOfImage = nextVA;

    // ─── Build PE buffer ───────────────────────────────────────────────

    std::vector<uint8_t> pe;
    pe.resize(nextFileOff, 0);

    // ─── DOS Header (64 bytes at offset 0) ─────────────────────────────

    putLE16(pe, 0x00, 0x5A4D);             // e_magic = "MZ"
    putLE32(pe, 0x3C, kPESignatureOffset); // e_lfanew

    // DOS stub message (at offset 0x40, 64 bytes)
    const char *dosStub = "This program cannot be run in DOS mode.\r\n$";
    std::memcpy(pe.data() + 0x40, dosStub, std::strlen(dosStub));

    // ─── PE Signature (4 bytes at 0x80) ────────────────────────────────

    putLE32(pe, kPESignatureOffset, 0x00004550); // "PE\0\0"

    // ─── COFF Header (20 bytes at 0x84) ────────────────────────────────

    uint32_t coffOff = kPESignatureOffset + 4;
    uint16_t machineType =
        (params.arch == "arm64") ? static_cast<uint16_t>(0xAA64) : static_cast<uint16_t>(0x8664);
    putLE16(pe, coffOff + 0, machineType); // Machine = AMD64 or ARM64
    putLE16(pe, coffOff + 2, static_cast<uint16_t>(numSections));
    putLE32(pe, coffOff + 4, 0);                    // TimeDateStamp
    putLE32(pe, coffOff + 8, 0);                    // PointerToSymbolTable
    putLE32(pe, coffOff + 12, 0);                   // NumberOfSymbols
    putLE16(pe, coffOff + 16, kOptionalHeaderSize); // SizeOfOptionalHeader
    putLE16(pe, coffOff + 18, 0x0022);              // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE

    // ─── Optional Header PE32+ (240 bytes at 0x98) ─────────────────────

    uint32_t optOff = coffOff + kCoffHeaderSize;

    putLE16(pe, optOff + 0, 0x020B); // Magic = PE32+
    pe[optOff + 2] = 0x0E;           // MajorLinkerVersion
    pe[optOff + 3] = 0x00;           // MinorLinkerVersion

    // SizeOfCode = sum of .text section raw sizes
    uint32_t sizeOfCode = sections[0].rawDataSize;
    putLE32(pe, optOff + 4, sizeOfCode);

    // SizeOfInitializedData
    uint32_t sizeOfInitData = 0;
    for (size_t i = 1; i < sections.size(); i++)
        sizeOfInitData += sections[i].rawDataSize;
    putLE32(pe, optOff + 8, sizeOfInitData);

    putLE32(pe, optOff + 12, 0); // SizeOfUninitializedData

    // AddressOfEntryPoint (RVA)
    putLE32(pe, optOff + 16, sections[0].virtualAddress + params.entryPointOffset);

    putLE32(pe, optOff + 20, sections[0].virtualAddress); // BaseOfCode

    putLE64(pe, optOff + 24, kImageBase);        // ImageBase
    putLE32(pe, optOff + 32, kSectionAlignment); // SectionAlignment
    putLE32(pe, optOff + 36, kFileAlignment);    // FileAlignment

    putLE16(pe, optOff + 40, 6); // MajorOperatingSystemVersion
    putLE16(pe, optOff + 42, 0); // MinorOperatingSystemVersion
    putLE16(pe, optOff + 44, 0); // MajorImageVersion
    putLE16(pe, optOff + 46, 0); // MinorImageVersion
    putLE16(pe, optOff + 48, 6); // MajorSubsystemVersion
    putLE16(pe, optOff + 50, 0); // MinorSubsystemVersion

    putLE32(pe, optOff + 52, 0);              // Win32VersionValue
    putLE32(pe, optOff + 56, sizeOfImage);    // SizeOfImage
    putLE32(pe, optOff + 60, headersAligned); // SizeOfHeaders
    putLE32(pe, optOff + 64, 0);              // CheckSum (not required for EXE)

    putLE16(pe, optOff + 68, params.subsystem);
    putLE16(pe, optOff + 70, params.dllCharacteristics);

    putLE64(pe, optOff + 72, params.stackReserve);
    putLE64(pe, optOff + 80, params.stackCommit);
    putLE64(pe, optOff + 88, params.heapReserve);
    putLE64(pe, optOff + 96, params.heapCommit);

    putLE32(pe, optOff + 104, 0);                   // LoaderFlags
    putLE32(pe, optOff + 108, kNumDataDirectories); // NumberOfRvaAndSizes

    // Data Directories (16 × 8 = 128 bytes starting at optOff + 112)
    uint32_t ddOff = optOff + 112;
    // [0] Export = 0,0 (already zeroed)
    // [1] Import Directory
    if (hasRdata && !params.imports.empty()) {
        putLE32(pe, ddOff + 1 * 8 + 0, importResult.importDirRVA);
        putLE32(pe, ddOff + 1 * 8 + 4, importResult.importDirSize);
    }
    // [2] Resource Table
    if (hasRsrc) {
        putLE32(pe, ddOff + 2 * 8 + 0, sections[rsrcIdx].virtualAddress);
        putLE32(pe, ddOff + 2 * 8 + 4, rsrcResult.rsrcSize);
    }
    // [5] Base Relocation Table
    if (hasReloc) {
        putLE32(pe, ddOff + 5 * 8 + 0, sections[relocIdx].virtualAddress);
        putLE32(pe, ddOff + 5 * 8 + 4, sections[relocIdx].virtualSize);
    }
    // [12] IAT
    if (hasRdata && !params.imports.empty()) {
        putLE32(pe, ddOff + 12 * 8 + 0, importResult.iatRVA);
        putLE32(pe, ddOff + 12 * 8 + 4, importResult.iatSize);
    }

    // ─── Section Headers ───────────────────────────────────────────────

    uint32_t secHdrOff = optOff + kOptionalHeaderSize;
    for (size_t i = 0; i < sections.size(); i++) {
        const auto &s = sections[i];
        uint32_t off = secHdrOff + static_cast<uint32_t>(i) * kSectionHeaderSize;

        std::memcpy(pe.data() + off, s.name, 8);
        putLE32(pe, off + 8, s.virtualSize);     // VirtualSize
        putLE32(pe, off + 12, s.virtualAddress); // VirtualAddress
        putLE32(pe, off + 16, s.rawDataSize);    // SizeOfRawData
        putLE32(pe, off + 20, s.rawDataOffset);  // PointerToRawData
        putLE32(pe, off + 24, 0);                // PointerToRelocations
        putLE32(pe, off + 28, 0);                // PointerToLinenumbers
        putLE16(pe, off + 32, 0);                // NumberOfRelocations
        putLE16(pe, off + 34, 0);                // NumberOfLinenumbers
        putLE32(pe, off + 36, s.characteristics);
    }

    // ─── Section Data ──────────────────────────────────────────────────

    for (const auto &s : sections) {
        if (!s.data.empty()) {
            std::memcpy(pe.data() + s.rawDataOffset, s.data.data(), s.data.size());
        }
    }

    // ─── Overlay ───────────────────────────────────────────────────────

    if (!params.overlay.empty()) {
        pe.insert(pe.end(), params.overlay.begin(), params.overlay.end());
    }

    return pe;
}

/// @brief Write a PE image to `path`. Throws `std::runtime_error` if the file cannot
/// be created or if the write fails partway through.
/// @param pe Complete PE image bytes.
/// @param path Destination file path.
/// @throws std::runtime_error If the atomic file write fails.
void writePEToFile(const std::vector<uint8_t> &pe, const std::string &path) {
    writeFileAtomic(path, pe);
}

namespace {

/// @brief Parse a `major.minor[.build[.revision]]` version string into a numeric vector.
/// Throws if the component count is not in [2, 4], as required by the Windows compatibility
/// manifest.
/// @param version Dotted-numeric Windows version.
/// @return Two through four numeric components.
/// @throws std::runtime_error If syntax or component count is invalid.
std::vector<unsigned> parseDottedVersion(const std::string &version) {
    const auto parsed =
        parseDottedNumericVersionParts(version, "Windows compatibility manifest version");
    if (parsed.size() < 2 || parsed.size() > 4)
        throw std::runtime_error(
            "Windows compatibility manifest version must have 2 to 4 numeric components: " +
            version);
    std::vector<unsigned> parts(parsed.begin(), parsed.end());
    return parts;
}

/// @brief Compare a dotted-version string against a fixed version tuple.
/// Returns -1, 0, or +1 (lhs < / == / > rhs). Missing trailing components are treated as 0.
/// @param lhs Dotted-numeric version string.
/// @param rhs Fixed numeric version tuple.
/// @return Negative, zero, or positive according to version ordering.
int compareDottedVersion(const std::string &lhs, std::initializer_list<unsigned> rhs) {
    const auto left = parseDottedVersion(lhs);
    const size_t n = std::max(left.size(), rhs.size());
    auto rhsIt = rhs.begin();
    for (size_t i = 0; i < n; ++i) {
        const unsigned l = i < left.size() ? left[i] : 0;
        const unsigned r = rhsIt != rhs.end() ? *rhsIt++ : 0;
        if (l < r)
            return -1;
        if (l > r)
            return 1;
    }
    return 0;
}

/// @brief Build the `<compatibility>` XML block for a Windows application manifest.
/// Emits a `<supportedOS>` GUID entry for every Windows version from `minOsWindows` through 10/11.
/// Returns an empty string when `minOsWindows` is empty (block omitted).
/// @param minOsWindows Optional minimum supported dotted-numeric Windows version.
/// @return Compatibility XML fragment, or an empty string.
std::string windowsCompatibilityXml(const std::string &minOsWindows) {
    if (minOsWindows.empty())
        return {};
    std::string ids;
    /// @brief Append one Windows supported-operating-system GUID element.
    /// @param id GUID text to place in the element's `Id` attribute.
    auto add = [&](std::string_view id) {
        ids += "      <supportedOS Id=\"";
        ids += id;
        ids += "\"/>\n";
    };
    if (compareDottedVersion(minOsWindows, {6, 0}) <= 0)
        add("{e2011457-1546-43c5-a5fe-008deee3d3f0}"); // Vista
    if (compareDottedVersion(minOsWindows, {6, 1}) <= 0)
        add("{35138b9a-5d96-4fbd-8e2d-a2440225f93a}"); // Windows 7
    if (compareDottedVersion(minOsWindows, {6, 2}) <= 0)
        add("{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}"); // Windows 8
    if (compareDottedVersion(minOsWindows, {6, 3}) <= 0)
        add("{1f676c76-80e1-4239-95bb-83d0f6d0da78}"); // Windows 8.1
    add("{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}");     // Windows 10/11
    return "  <compatibility xmlns=\"urn:schemas-microsoft-com:compatibility.v1\">\n"
           "    <application>\n" +
           ids +
           "    </application>\n"
           "  </compatibility>\n";
}

/// @brief Generate a complete Windows application manifest XML with the given
/// `requestedExecutionLevel`. `level` maps directly to the UAC attribute value
/// (`"requireAdministrator"` or `"asInvoker"`).
/// @param level UAC requestedExecutionLevel attribute value.
/// @param minOsWindows Optional minimum Windows version for compatibility declarations.
/// @return Complete application-manifest XML.
std::string generateManifestWithExecutionLevel(const std::string &level,
                                               const std::string &minOsWindows) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
           "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" "
           "xmlns:asmv3=\"urn:schemas-microsoft-com:asm.v3\" manifestVersion=\"1.0\">\n"
           "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">\n"
           "    <security><requestedPrivileges>\n"
           "      <requestedExecutionLevel level=\"" +
           level +
           "\" uiAccess=\"false\"/>\n"
           "    </requestedPrivileges></security>\n"
           "  </trustInfo>\n"
           "  <dependency>\n"
           "    <dependentAssembly>\n"
           "      <assemblyIdentity type=\"win32\" "
           "name=\"Microsoft.Windows.Common-Controls\" version=\"6.0.0.0\" "
           "processorArchitecture=\"*\" publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>\n"
           "    </dependentAssembly>\n"
           "  </dependency>\n"
           "  <asmv3:application>\n"
           "    <asmv3:windowsSettings>\n"
           "      <dpiAware xmlns=\"http://schemas.microsoft.com/SMI/2005/WindowsSettings\">"
           "true/pm</dpiAware>\n"
           "      <dpiAwareness xmlns=\"http://schemas.microsoft.com/SMI/2016/WindowsSettings\">"
           "PerMonitorV2, PerMonitor</dpiAwareness>\n"
           "      <longPathAware xmlns=\"http://schemas.microsoft.com/SMI/2016/WindowsSettings\">"
           "true</longPathAware>\n"
           "    </asmv3:windowsSettings>\n"
           "  </asmv3:application>\n" +
           windowsCompatibilityXml(minOsWindows) + "</assembly>\n";
}

} // namespace

/// @brief Generate a UAC-elevating manifest (`requireAdministrator`, no OS compat block).
/// Embed this in the installer PE so Windows prompts for elevation on launch.
std::string generateUacManifest() {
    return generateManifestWithExecutionLevel("requireAdministrator", {});
}

/// @brief Generate a UAC-elevating manifest with a Windows OS compatibility block.
/// Use this overload when the installer needs to opt in to Windows 8+ DPI behaviors
/// gated on `supportedOS` declarations.
/// @param minOsWindows Minimum supported dotted-numeric Windows version.
/// @return Complete `requireAdministrator` application-manifest XML.
std::string generateUacManifest(const std::string &minOsWindows) {
    return generateManifestWithExecutionLevel("requireAdministrator", minOsWindows);
}

/// @brief Generate a non-elevating manifest (`asInvoker`, no OS compat block).
/// Used for app executables that run at the user's current privilege level.
std::string generateAsInvokerManifest() {
    return generateManifestWithExecutionLevel("asInvoker", {});
}

/// @brief Generate a non-elevating manifest with a Windows OS compatibility block.
/// @param minOsWindows Minimum supported dotted-numeric Windows version.
/// @return Complete `asInvoker` application-manifest XML.
std::string generateAsInvokerManifest(const std::string &minOsWindows) {
    return generateManifestWithExecutionLevel("asInvoker", minOsWindows);
}

} // namespace zanna::pkg
