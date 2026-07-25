//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/ObjFileReader.cpp
// Purpose: Format detection and dispatch for object file reading.
// Key invariants:
//   - Auto-detects ELF (7f 45 4c 46), Mach-O (cf fa ed fe), COFF (machine field)
// Links: codegen/common/linker/ObjFileReader.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ObjFileReader.cpp
 * @brief Implements object-format detection, file I/O, and parser dispatch.
 */

#include "codegen/common/linker/ObjFileReader.hpp"

#include "common/Filesystem.hpp"
#include <cstdint>
#include <fstream>
#include <limits>
#include <utility>

namespace zanna::codegen::linker {

/// @brief Reads an unaligned little-endian 16-bit value.
/// @param p Pointer to at least two readable bytes.
/// @return Host-order value.
static uint16_t readLE16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

/// @brief Reads an unaligned little-endian 32-bit value.
/// @param p Pointer to at least four readable bytes.
/// @return Host-order value.
static uint32_t readLE32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/// @copydoc isCoffImportLibraryMember(const uint8_t *, size_t)
bool isCoffImportLibraryMember(const uint8_t *data, size_t size) {
    static constexpr size_t kImportObjectHeaderSize = 20;
    static constexpr uint16_t kImageFileMachineAmd64 = 0x8664;
    static constexpr uint16_t kImageFileMachineArm64 = 0xAA64;

    if (data == nullptr || size < kImportObjectHeaderSize)
        return false;

    if (readLE16(data) != 0 || readLE16(data + 2) != 0xFFFF)
        return false;

    const uint16_t machine = readLE16(data + 6);
    if (machine != kImageFileMachineAmd64 && machine != kImageFileMachineArm64)
        return false;

    const uint32_t payloadSize = readLE32(data + 12);
    if (payloadSize > size - kImportObjectHeaderSize)
        return false;

    const uint16_t typeInfo = readLE16(data + 18);
    const uint16_t importType = typeInfo & 0x3;
    const uint16_t importNameType = (typeInfo >> 2) & 0x7;
    return importType <= 2 && importNameType <= 4;
}

/// @copydoc detectFormat(const uint8_t *, size_t)
ObjFileFormat detectFormat(const uint8_t *data, size_t size) {
    if (size < 4)
        return ObjFileFormat::Unknown;

    // ELF: 0x7F 'E' 'L' 'F'
    if (data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F')
        return ObjFileFormat::ELF;

    // Mach-O 64-bit: 0xFEEDFACF (little-endian: CF FA ED FE)
    if (data[0] == 0xCF && data[1] == 0xFA && data[2] == 0xED && data[3] == 0xFE)
        return ObjFileFormat::MachO;

    // COFF: check for known machine types at offset 0.
    if (size >= 20) {
        const uint16_t machine =
            static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
        const uint16_t sectionCount =
            static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8);
        if (machine == 0 && sectionCount == 0xFFFF)
            return ObjFileFormat::COFF;
        if (machine == 0 && sectionCount != 0)
            return ObjFileFormat::COFF;
        if (machine == 0x8664 || machine == 0xAA64)
            return ObjFileFormat::COFF;
    }

    return ObjFileFormat::Unknown;
}

/// @copydoc readObjFile(const uint8_t *, size_t, const std::string &, ObjFile &, std::ostream &)
bool readObjFile(
    const uint8_t *data, size_t size, const std::string &name, ObjFile &obj, std::ostream &err) {
    const ObjFileFormat fmt = detectFormat(data, size);
    ObjFile parsed;
    bool ok = false;
    switch (fmt) {
        case ObjFileFormat::ELF:
            ok = readElfObj(data, size, name, parsed, err);
            break;
        case ObjFileFormat::MachO:
            ok = readMachOObj(data, size, name, parsed, err);
            break;
        case ObjFileFormat::COFF:
            ok = readCoffObj(data, size, name, parsed, err);
            break;
        case ObjFileFormat::Unknown:
            err << "error: " << name << ": unknown object file format\n";
            return false;
    }
    if (!ok)
        return false;
    obj = std::move(parsed);
    return true;
}

/// @copydoc readObjFile(const std::string &, ObjFile &, std::ostream &)
bool readObjFile(const std::string &path, ObjFile &obj, std::ostream &err) {
    std::ifstream f(zanna::filesystem::pathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!f) {
        err << "error: cannot open object file '" << path << "'\n";
        return false;
    }
    const std::streampos endPos = f.tellg();
    if (endPos == std::streampos(-1)) {
        err << "error: failed to determine object file size for '" << path << "'\n";
        return false;
    }
    const auto endOff = static_cast<std::streamoff>(endPos);
    if (endOff < 0 ||
        static_cast<uintmax_t>(endOff) >
            static_cast<uintmax_t>(std::numeric_limits<size_t>::max()) ||
        static_cast<uintmax_t>(endOff) >
            static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        err << "error: object file '" << path << "' is too large to read\n";
        return false;
    }
    const auto fileSize = static_cast<size_t>(endOff);
    f.seekg(0);
    std::vector<uint8_t> data(fileSize);
    f.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(fileSize));
    if (!f) {
        err << "error: failed to read object file '" << path << "'\n";
        return false;
    }
    return readObjFile(data.data(), data.size(), path, obj, err);
}

} // namespace zanna::codegen::linker
