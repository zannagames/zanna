//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_packaging.cpp
// Purpose: Unit tests for the ZAPS packaging subsystem — verifies binary
//          format correctness for ZIP, tar, ar, PE, DEFLATE, ICO/ICNS,
//          .lnk, .desktop, and Info.plist generators.
//
// Key invariants:
//   - Each test verifies structural correctness via magic bytes and offsets.
//   - Round-trip tests ensure compress->decompress produces original data.
//   - Filesystem tests use isolated temporary roots and remove them after each case.
//
// Ownership/Lifetime:
//   - Self-contained test binary.
//
// Links: src/tools/common/packaging/*
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "ArWriter.hpp"
#include "CpioWriter.hpp"
#include "DesktopEntryGenerator.hpp"
#include "IconGenerator.hpp"
#include "InstallerStub.hpp"
#include "InstallerStubGen.hpp"
#include "InstallerStubGenA64.hpp"
#include "LinuxPackageBuilder.hpp"
#include "LinuxRuntimeStubGen.hpp"
#include "LnkWriter.hpp"
#include "MacOSPackageBuilder.hpp"
#include "PEBuilder.hpp"
#include "PackageConfig.hpp"
#include "PkgDeflate.hpp"
#include "PkgGzip.hpp"
#include "PkgHash.hpp"
#include "PkgPNG.hpp"
#include "PkgUtils.hpp"
#include "PkgVerify.hpp"
#include "PkgZlib.hpp"
#include "PlistGenerator.hpp"
#include "TarWriter.hpp"
#include "ToolchainInstallManifest.hpp"
#include "WindowsInstallerMetadata.hpp"
#include "WindowsPEValidation.hpp"
#include "WindowsPackageBuilder.hpp"
#include "XarWriter.hpp"
#include "ZipReader.hpp"
#include "ZipWriter.hpp"
#include "common/Filesystem.hpp"
#include "common/RunProcess.hpp"
#include "zanna/platform/Capabilities.hpp"
#include "zanna/runtime/RuntimeComponentManifest.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

using namespace zanna::pkg;

extern "C" {
uint32_t rt_crc32_compute(const uint8_t *data, size_t len);
}

// ============================================================================
// Helper: read a little-endian uint16 from a byte buffer
// ============================================================================
static uint16_t readLE16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t readLE32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static void writeLE16(uint8_t *p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

static void writeLE32(uint8_t *p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

static WindowsInstallerMetadata sampleWindowsInstallerMetadata() {
    WindowsInstallerMetadata metadata;
    metadata.packageMode = "setup";
    metadata.productKind = "toolchain";
    metadata.identifier = "org.zanna.toolchain";
    metadata.displayName = "Zanna Tools \xE2\x80\x93 Developer Edition";
    metadata.version = "1.2.3-rc.1";
    metadata.publisher = "Zanna Project";
    metadata.description = "Compiler, VM, IDE, and SDK";
    metadata.contact = "support@example.invalid";
    metadata.homepage = "https://example.invalid/zanna";
    metadata.documentationUrl = "https://example.invalid/zanna/docs";
    metadata.architecture = "x64";
    metadata.defaultScope = "user";
    metadata.defaultInstallDir = "Zanna";
    metadata.executableName = "bin/zanna.exe";
    metadata.associationExecutable = "bin/zannastudio.exe";
    metadata.pathRelativePath = "bin";
    metadata.displayIconRelativePath = "share/zanna/zanna.ico";
    metadata.cleanupSha256 = std::string(64, 'c');
    metadata.addToPath = true;
    metadata.registerFileAssociations = true;
    metadata.createShortcuts = true;
    metadata.components.push_back(
        {"core", "Core developer tools", "Required compiler and runtime", true, true, 70});
    metadata.components.push_back(
        {"ide", "Zanna Studio", "Editor, debugger, and language services", false, true, 70});
    metadata.payloadFiles.push_back({"bin/zanna.exe", std::string(64, 'a'), 30, std::string{}});
    metadata.payloadFiles.push_back({"bin/zannastudio.exe", std::string(64, 'b'), 70, "ide"});
    metadata.payloadFiles.push_back(
        {"share/zanna/zanna.ico", std::string(64, 'c'), 0, std::string{}});
    metadata.payloadFiles.push_back({"bin/zannastudio.ico", std::string(64, 'd'), 0, "ide"});
    metadata.outerFiles.push_back(
        {"meta/uninstall.exe", "uninstall.exe", std::string(64, 'e'), 40, std::string{}});
    metadata.shortcuts.push_back({"start-menu",
                                  "Zanna Studio.lnk",
                                  "install",
                                  "bin/zannastudio.exe",
                                  "profile",
                                  {},
                                  {},
                                  {},
                                  "Zanna Studio",
                                  "install",
                                  "bin/zannastudio.ico",
                                  0,
                                  "ide"});
    metadata.associations.push_back(
        {".zia", "Zia source", "text/x-zia", "org.zanna.toolchain.Zia.1", ""});
    metadata.installedSizeBytes = 140;
    return metadata;
}

static void writeBE32(uint8_t *p, uint32_t value) {
    p[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(value & 0xFF);
}

static void writeBE64(uint8_t *p, uint64_t value) {
    for (size_t i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t>((value >> ((7 - i) * 8)) & 0xFF);
}

static uint64_t readLE64(const uint8_t *p) {
    return static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) |
           (static_cast<uint64_t>(p[2]) << 16) | (static_cast<uint64_t>(p[3]) << 24) |
           (static_cast<uint64_t>(p[4]) << 32) | (static_cast<uint64_t>(p[5]) << 40) |
           (static_cast<uint64_t>(p[6]) << 48) | (static_cast<uint64_t>(p[7]) << 56);
}

static void writeBytes(const std::filesystem::path &path, const std::vector<uint8_t> &bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

static void writeTestWindowsPe(const std::filesystem::path &path,
                               const std::string &arch = "x64",
                               std::vector<PEImport> imports = {}) {
    PEBuildParams pe;
    pe.arch = arch;
    pe.textSection = {0xC3};
    pe.imports = std::move(imports);
    writeBytes(path, buildPE(pe));
}

static std::vector<uint8_t> makeMockNativeExecutableHeader() {
#if ZANNA_HOST_WINDOWS
    std::vector<uint8_t> bytes(128, 0);
    bytes[0] = 'M';
    bytes[1] = 'Z';
    writeLE32(bytes.data() + 0x3c, 0x40);
    bytes[0x40] = 'P';
    bytes[0x41] = 'E';
#if defined(_M_ARM64)
    bytes[0x44] = 0x64;
    bytes[0x45] = 0xAA;
#else
    bytes[0x44] = 0x64;
    bytes[0x45] = 0x86;
#endif
    return bytes;
#elif defined(__APPLE__)
    std::vector<uint8_t> bytes(32, 0);
    bytes[0] = 0xcf;
    bytes[1] = 0xfa;
    bytes[2] = 0xed;
    bytes[3] = 0xfe;
#if defined(__aarch64__) || defined(__arm64__)
    writeLE32(bytes.data() + 4, 0x0100000c);
#else
    writeLE32(bytes.data() + 4, 0x01000007);
#endif
    return bytes;
#else
    std::vector<uint8_t> bytes(64, 0);
    bytes[0] = 0x7f;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
#if defined(__aarch64__) || defined(__arm64__)
    bytes[18] = 183;
#else
    bytes[18] = 62;
#endif
    return bytes;
#endif
}

static std::vector<uint8_t> makeSyntheticFatMachO(bool fat64,
                                                  const std::vector<uint32_t> &cpuTypes) {
    const size_t entrySize = fat64 ? 32u : 20u;
    const size_t headerSize = 8u + cpuTypes.size() * entrySize;
    constexpr size_t kSliceSize = 32;
    std::vector<uint8_t> bytes(headerSize + cpuTypes.size() * kSliceSize, 0);
    bytes[0] = 0xCA;
    bytes[1] = 0xFE;
    bytes[2] = 0xBA;
    bytes[3] = fat64 ? 0xBF : 0xBE;
    writeBE32(bytes.data() + 4, static_cast<uint32_t>(cpuTypes.size()));
    for (size_t i = 0; i < cpuTypes.size(); ++i) {
        const size_t entry = 8u + i * entrySize;
        const size_t sliceOffset = headerSize + i * kSliceSize;
        writeBE32(bytes.data() + entry, cpuTypes[i]);
        if (fat64) {
            writeBE64(bytes.data() + entry + 8, sliceOffset);
            writeBE64(bytes.data() + entry + 16, kSliceSize);
        } else {
            writeBE32(bytes.data() + entry + 8, static_cast<uint32_t>(sliceOffset));
            writeBE32(bytes.data() + entry + 12, static_cast<uint32_t>(kSliceSize));
        }
        bytes[sliceOffset] = 0xCF;
        bytes[sliceOffset + 1] = 0xFA;
        bytes[sliceOffset + 2] = 0xED;
        bytes[sliceOffset + 3] = 0xFE;
        writeLE32(bytes.data() + sliceOffset + 4, cpuTypes[i]);
    }
    return bytes;
}

static std::filesystem::path mockToolchainBinaryPath(const std::filesystem::path &stage,
                                                     std::string_view name) {
#if defined(_WIN32)
    return stage / "bin" / (std::string(name) + ".exe");
#else
    return stage / "bin" / std::string(name);
#endif
}

static uint32_t readBE32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

static uint16_t readBE16(const uint8_t *p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

static uint64_t readBE64(const uint8_t *p) {
    return (static_cast<uint64_t>(readBE32(p)) << 32) | readBE32(p + 4);
}

static void appendBE32(std::vector<uint8_t> &buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

static uint32_t testAdler32(const uint8_t *data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void appendPngChunk(std::vector<uint8_t> &buf,
                           const char type[4],
                           const std::vector<uint8_t> &payload) {
    appendBE32(buf, static_cast<uint32_t>(payload.size()));
    const size_t typeOff = buf.size();
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t *>(type),
               reinterpret_cast<const uint8_t *>(type) + 4);
    buf.insert(buf.end(), payload.begin(), payload.end());
    appendBE32(buf, rt_crc32_compute(buf.data() + typeOff, 4 + payload.size()));
}

static std::vector<uint8_t> makeIndexedPng2x1WithTransparency() {
    std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> ihdr;
    appendBE32(ihdr, 2);
    appendBE32(ihdr, 1);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(3); // indexed color
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    appendPngChunk(png, "IHDR", ihdr);
    appendPngChunk(png, "PLTE", {255, 0, 0, 0, 0, 255});
    appendPngChunk(png, "tRNS", {255, 0});

    std::vector<uint8_t> raw = {0, 0, 1};
    auto deflated = deflate(raw.data(), raw.size());
    std::vector<uint8_t> zlib = {0x78, 0x01};
    zlib.insert(zlib.end(), deflated.begin(), deflated.end());
    appendBE32(zlib, testAdler32(raw.data(), raw.size()));
    appendPngChunk(png, "IDAT", zlib);
    appendPngChunk(png, "IEND", {});
    return png;
}

static bool containsUtf16LE(const std::vector<uint8_t> &data, const std::string &text) {
    const auto needle = utf8ToUtf16LEBytes(text, true);
    return std::search(data.begin(), data.end(), needle.begin(), needle.end()) != data.end();
}

static bool containsUtf16LEStringData(const std::vector<uint8_t> &data, const std::string &text) {
    const auto needle = utf8ToUtf16LEBytes(text, false);
    return std::search(data.begin(), data.end(), needle.begin(), needle.end()) != data.end();
}

static bool containsAscii(const std::vector<uint8_t> &data, const std::string &text) {
    return std::search(data.begin(), data.end(), text.begin(), text.end()) != data.end();
}

static std::vector<uint8_t> extractFirstZipOverlay(const std::vector<uint8_t> &pe) {
    const std::array<uint8_t, 4> localHeader{{'P', 'K', 0x03, 0x04}};
    auto it = std::search(pe.begin(), pe.end(), localHeader.begin(), localHeader.end());
    if (it == pe.end())
        return {};
    return std::vector<uint8_t>(it, pe.end());
}

static std::vector<uint8_t> extractPeOverlayZipEntry(const std::vector<uint8_t> &pe,
                                                     const std::string &name) {
    const auto overlay = extractFirstZipOverlay(pe);
    if (overlay.empty())
        return {};
    ZipReader reader(overlay.data(), overlay.size());
    const ZipEntry *entry = reader.find(name);
    if (entry == nullptr)
        return {};
    return reader.extract(*entry);
}

static bool zipContainsEntries(const std::vector<uint8_t> &zipData,
                               const std::vector<std::string> &requiredEntries) {
    ZipReader reader(zipData.data(), zipData.size());
    for (const auto &required : requiredEntries) {
        if (reader.find(required) == nullptr)
            return false;
    }
    return true;
}

static std::vector<uint8_t> extractZipEntry(const std::vector<uint8_t> &zipData,
                                            const std::string &name) {
    ZipReader reader(zipData.data(), zipData.size());
    const ZipEntry *entry = reader.find(name);
    return entry == nullptr ? std::vector<uint8_t>{} : reader.extract(*entry);
}

static bool zipEntryUsesDeflate(const std::vector<uint8_t> &zipData, const std::string &name) {
    ZipReader reader(zipData.data(), zipData.size());
    const ZipEntry *entry = reader.find(name);
    return entry != nullptr && entry->method == 8;
}

static bool zipEntryUnixMode(const std::vector<uint8_t> &zipData,
                             const std::string &name,
                             uint16_t &modeOut) {
    for (size_t offset = 0; offset + 46 <= zipData.size();) {
        if (readLE32(zipData.data() + offset) != 0x02014B50) {
            ++offset;
            continue;
        }
        const uint16_t nameLength = readLE16(zipData.data() + offset + 28);
        const uint16_t extraLength = readLE16(zipData.data() + offset + 30);
        const uint16_t commentLength = readLE16(zipData.data() + offset + 32);
        const size_t recordSize = 46u + nameLength + extraLength + commentLength;
        if (recordSize > zipData.size() - offset)
            return false;
        const std::string entryName(reinterpret_cast<const char *>(zipData.data() + offset + 46),
                                    nameLength);
        if (entryName == name) {
            modeOut = static_cast<uint16_t>(readLE32(zipData.data() + offset + 38) >> 16);
            return true;
        }
        offset += recordSize;
    }
    return false;
}

static uint32_t alignUpTest(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static uint32_t testIatBaseRvaForStub(const StubResult &stub) {
    uint32_t idtSize = static_cast<uint32_t>((stub.imports.size() + 1) * 20);
    uint32_t iltSize = 0;
    for (const auto &imp : stub.imports)
        iltSize += static_cast<uint32_t>((imp.functions.size() + 1) * 8);
    uint32_t hintNameSize = 0;
    for (const auto &imp : stub.imports) {
        for (const auto &fn : imp.functions) {
            const uint32_t entryLen = static_cast<uint32_t>(2 + fn.size() + 1);
            hintNameSize += (entryLen + 1) & ~1u;
        }
    }
    uint32_t dllNameSize = 0;
    for (const auto &imp : stub.imports)
        dllNameSize += static_cast<uint32_t>(imp.dllName.size() + 1);

    const uint32_t iatOff = alignUpTest(idtSize + iltSize + hintNameSize + dllNameSize, 8);
    const uint32_t rdataRVA =
        0x2000u + (alignUpTest(static_cast<uint32_t>(stub.textSection.size()), 0x1000u) - 0x1000u);
    return rdataRVA + iatOff;
}

static size_t countIatCallsTo(const StubResult &stub,
                              const std::string &dllName,
                              const std::string &functionName) {
    const uint32_t iatBaseRva = testIatBaseRvaForStub(stub);
    uint32_t slotOffset = 0;
    bool found = false;
    for (const auto &imp : stub.imports) {
        for (const auto &fn : imp.functions) {
            if (imp.dllName == dllName && fn == functionName) {
                found = true;
                break;
            }
            slotOffset += 8;
        }
        if (found)
            break;
        slotOffset += 8;
    }
    if (!found)
        return 0;

    const int64_t targetRva = static_cast<int64_t>(iatBaseRva + slotOffset);
    size_t calls = 0;
    for (size_t i = 0; i + 6 <= stub.textSection.size(); ++i) {
        if (stub.textSection[i] != 0xFF || stub.textSection[i + 1] != 0x15)
            continue;
        const int32_t disp = static_cast<int32_t>(readLE32(stub.textSection.data() + i + 2));
        const int64_t rip = 0x1000ll + static_cast<int64_t>(i) + 6;
        if (rip + disp == targetRva)
            ++calls;
    }
    return calls;
}

static bool stubHasImport(const StubResult &stub,
                          const std::string &dllName,
                          const std::string &functionName) {
    return std::any_of(stub.imports.begin(), stub.imports.end(), [&](const PEImport &imp) {
        return imp.dllName == dllName &&
               std::find(imp.functions.begin(), imp.functions.end(), functionName) !=
                   imp.functions.end();
    });
}

static std::vector<uint8_t> inflateGzipPayload(const std::vector<uint8_t> &gzipData) {
    return gunzip(gzipData.data(), gzipData.size());
}

static std::string tarFirstEntryName(const std::vector<uint8_t> &tarBytes) {
    if (tarBytes.size() < 512)
        return {};
    const char *name = reinterpret_cast<const char *>(tarBytes.data());
    size_t len = 0;
    while (len < 100 && name[len] != '\0')
        ++len;
    return std::string(name, name + len);
}

static uint64_t parseTarOctal(const uint8_t *field, size_t len) {
    uint64_t value = 0;
    size_t i = 0;
    while (i < len && (field[i] == ' ' || field[i] == '\0'))
        ++i;
    for (; i < len && field[i] >= '0' && field[i] <= '7'; ++i)
        value = (value << 3) + static_cast<uint64_t>(field[i] - '0');
    return value;
}

static bool tarEntryMode(const std::vector<uint8_t> &tarBytes,
                         const std::string &entryName,
                         uint32_t &modeOut) {
    for (size_t off = 0; off + 512 <= tarBytes.size();) {
        bool allZero = true;
        for (size_t i = 0; i < 512; ++i) {
            if (tarBytes[off + i] != 0) {
                allZero = false;
                break;
            }
        }
        if (allZero)
            return false;

        const char *name = reinterpret_cast<const char *>(tarBytes.data() + off);
        size_t nameLen = 0;
        while (nameLen < 100 && name[nameLen] != '\0')
            ++nameLen;
        if (std::string(name, name + nameLen) == entryName) {
            modeOut = static_cast<uint32_t>(parseTarOctal(tarBytes.data() + off + 100, 8));
            return true;
        }

        const uint64_t size = parseTarOctal(tarBytes.data() + off + 124, 12);
        off += 512 + static_cast<size_t>(((size + 511) / 512) * 512);
    }
    return false;
}

static bool tarEntryData(const std::vector<uint8_t> &tarBytes,
                         const std::string &entryName,
                         std::vector<uint8_t> &dataOut) {
    for (size_t off = 0; off + 512 <= tarBytes.size();) {
        bool allZero = true;
        for (size_t i = 0; i < 512; ++i) {
            if (tarBytes[off + i] != 0) {
                allZero = false;
                break;
            }
        }
        if (allZero)
            return false;

        const char *name = reinterpret_cast<const char *>(tarBytes.data() + off);
        size_t nameLen = 0;
        while (nameLen < 100 && name[nameLen] != '\0')
            ++nameLen;
        const uint64_t size = parseTarOctal(tarBytes.data() + off + 124, 12);
        const size_t dataOff = off + 512;
        if (size > tarBytes.size() - dataOff)
            return false;
        if (std::string(name, name + nameLen) == entryName) {
            dataOut.assign(tarBytes.begin() + static_cast<std::ptrdiff_t>(dataOff),
                           tarBytes.begin() + static_cast<std::ptrdiff_t>(dataOff + size));
            return true;
        }
        off += 512 + static_cast<size_t>(((size + 511) / 512) * 512);
    }
    return false;
}

static bool tarEntryLinkTarget(const std::vector<uint8_t> &tarBytes,
                               const std::string &entryName,
                               std::string &targetOut) {
    for (size_t off = 0; off + 512 <= tarBytes.size();) {
        bool allZero = true;
        for (size_t i = 0; i < 512; ++i) {
            if (tarBytes[off + i] != 0) {
                allZero = false;
                break;
            }
        }
        if (allZero)
            return false;

        const char *name = reinterpret_cast<const char *>(tarBytes.data() + off);
        size_t nameLen = 0;
        while (nameLen < 100 && name[nameLen] != '\0')
            ++nameLen;
        const uint64_t size = parseTarOctal(tarBytes.data() + off + 124, 12);
        if (std::string(name, name + nameLen) == entryName && tarBytes[off + 156] == '2') {
            const char *link = reinterpret_cast<const char *>(tarBytes.data() + off + 157);
            size_t linkLen = 0;
            while (linkLen < 100 && link[linkLen] != '\0')
                ++linkLen;
            targetOut.assign(link, link + linkLen);
            return true;
        }
        off += 512 + static_cast<size_t>(((size + 511) / 512) * 512);
    }
    return false;
}

static bool arMemberData(const std::vector<uint8_t> &arBytes,
                         const std::string &memberName,
                         std::vector<uint8_t> &dataOut) {
    if (arBytes.size() < 8 || std::memcmp(arBytes.data(), "!<arch>\n", 8) != 0)
        return false;
    size_t pos = 8;
    while (pos + 60 <= arBytes.size()) {
        std::string name(reinterpret_cast<const char *>(arBytes.data() + pos), 16);
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        const size_t slash = name.find('/');
        if (slash != std::string::npos)
            name = name.substr(0, slash);
        size_t size = 0;
        for (size_t i = 0; i < 10; ++i) {
            const char c = static_cast<char>(arBytes[pos + 48 + i]);
            if (c == ' ')
                break;
            if (c < '0' || c > '9')
                return false;
            size = size * 10 + static_cast<size_t>(c - '0');
        }
        const size_t dataOff = pos + 60;
        if (size > arBytes.size() - dataOff)
            return false;
        if (name == memberName) {
            dataOut.assign(arBytes.begin() + static_cast<std::ptrdiff_t>(dataOff),
                           arBytes.begin() + static_cast<std::ptrdiff_t>(dataOff + size));
            return true;
        }
        pos = dataOff + size + (size % 2);
    }
    return false;
}

static std::string debControlText(const std::vector<uint8_t> &debBytes) {
    std::vector<uint8_t> controlGz;
    if (!arMemberData(debBytes, "control.tar.gz", controlGz))
        return {};
    const auto controlTar = inflateGzipPayload(controlGz);
    std::vector<uint8_t> control;
    if (!tarEntryData(controlTar, "control", control) &&
        !tarEntryData(controlTar, "./control", control))
        return {};
    return std::string(control.begin(), control.end());
}

static std::string debControlEntryText(const std::vector<uint8_t> &debBytes,
                                       const std::string &entryName) {
    std::vector<uint8_t> controlGz;
    if (!arMemberData(debBytes, "control.tar.gz", controlGz))
        return {};
    const auto controlTar = inflateGzipPayload(controlGz);
    std::vector<uint8_t> data;
    if (!tarEntryData(controlTar, entryName, data) &&
        !tarEntryData(controlTar, "./" + entryName, data))
        return {};
    return std::string(data.begin(), data.end());
}

static std::vector<uint8_t> debDataTar(const std::vector<uint8_t> &debBytes) {
    std::vector<uint8_t> dataGz;
    if (!arMemberData(debBytes, "data.tar.gz", dataGz))
        return {};
    return inflateGzipPayload(dataGz);
}

static uint64_t peOptionalHeaderField64(const std::vector<uint8_t> &pe, uint32_t fieldOff) {
    if (pe.size() < 0x40)
        return 0;
    const uint32_t peOff = readLE32(pe.data() + 0x3c);
    const size_t optOff = static_cast<size_t>(peOff) + 4 + 20;
    if (optOff + fieldOff + 8 > pe.size())
        return 0;
    return readLE64(pe.data() + optOff + fieldOff);
}

static std::vector<uint8_t> makeTarGz(
    std::initializer_list<std::pair<std::string, std::string>> files) {
    TarWriter tar;
    tar.addDirectory("./", 0755);
    for (const auto &file : files)
        tar.addFileString(file.first, file.second);
    auto tarBytes = tar.finish();
    return gzip(tarBytes.data(), tarBytes.size());
}

static std::vector<uint8_t> appImagePayloadBytes(const std::vector<uint8_t> &appImage) {
    const std::string marker = std::string(kLinuxRuntimePayloadMarker) + "\n";
    const auto it = std::search(appImage.begin(),
                                appImage.end(),
                                reinterpret_cast<const uint8_t *>(marker.data()),
                                reinterpret_cast<const uint8_t *>(marker.data()) + marker.size());
    if (it == appImage.end())
        return {};
    const auto payload = it + static_cast<std::ptrdiff_t>(marker.size());
    return std::vector<uint8_t>(payload, appImage.end());
}

/**
 * @brief Locate the first byte of the gzip payload appended to a Zanna AppImage.
 *
 * The runtime stub is plain shell text followed by kLinuxRuntimePayloadMarker
 * and then the gzip-compressed tar payload. Returning appImage.size() signals
 * that the marker is absent.
 *
 * @param appImage Complete AppImage byte stream.
 * @return Offset of the payload, or appImage.size() when no payload marker exists.
 */
static size_t appImagePayloadOffset(const std::vector<uint8_t> &appImage) {
    const std::string marker = std::string(kLinuxRuntimePayloadMarker) + "\n";
    const auto it = std::search(appImage.begin(),
                                appImage.end(),
                                reinterpret_cast<const uint8_t *>(marker.data()),
                                reinterpret_cast<const uint8_t *>(marker.data()) + marker.size());
    if (it == appImage.end())
        return appImage.size();
    return static_cast<size_t>(std::distance(appImage.begin(), it)) + marker.size();
}

static std::filesystem::path createMockToolchainStage(const std::filesystem::path &tmpRoot) {
    namespace fs = std::filesystem;
    const fs::path stage = tmpRoot / "stage";
    fs::create_directories(stage / "bin");
    fs::create_directories(stage / "lib" / "cmake" / "Zanna");
    fs::create_directories(stage / "include" / "zanna");
    fs::create_directories(stage / "share" / "man" / "man1");
    fs::create_directories(stage / "share" / "man" / "man7");
    fs::create_directories(stage / "share" / "doc" / "zanna");

#if defined(_WIN32)
    auto binaryName = [](std::string_view base) { return std::string(base) + ".exe"; };
    const std::string exeName = binaryName("zanna");
    auto archiveName = [](std::string_view base) { return std::string(base) + ".lib"; };
#else
    auto binaryName = [](std::string_view base) { return std::string(base); };
    const std::string exeName = "zanna";
    auto archiveName = [](std::string_view base) { return "lib" + std::string(base) + ".a"; };
#endif
    {
        const fs::path exePath = stage / "bin" / exeName;
        writeBytes(exePath, makeMockNativeExecutableHeader());
        fs::permissions(exePath,
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                            fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                            fs::perms::others_exec,
                        fs::perm_options::replace);
    }
    for (const char *tool : {"zia",
                             "zbasic",
                             "ilrun",
                             "il-verify",
                             "il-dis",
                             "zia-server",
                             "zbasic-server",
                             "basic-ast-dump",
                             "basic-lex-dump",
                             "zannastudio"}) {
        const fs::path toolPath = stage / "bin" / binaryName(tool);
        std::ofstream out(toolPath, std::ios::binary);
        out << "stub";
        out.close();
        fs::permissions(toolPath,
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                            fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                            fs::perms::others_exec,
                        fs::perm_options::replace);
    }
    {
        std::ofstream out(stage / "lib" / "cmake" / "Zanna" / "ZannaConfig.cmake");
        out << "# mock\n";
    }
    {
        std::ofstream out(stage / "lib" / "cmake" / "Zanna" / "ZannaTargets.cmake");
        out << "# mock\n";
    }
    {
        std::ofstream out(stage / "lib" / "cmake" / "Zanna" / "ZannaConfigVersion.cmake");
        out << "set(PACKAGE_VERSION \"9.8.7\")\n";
    }
    {
        std::ofstream out(stage / "include" / "zanna" / "version.hpp");
        out << "#define ZANNA_VERSION_STR \"9.8.7-snapshot\"\n"
               "#define ZANNA_SNAPSHOT_STR \"9.8.7-0-g0123456789ab\"\n"
               "#define ZANNA_SOURCE_COMMIT_STR \"0123456789abcdef0123456789abcdef01234567\"\n"
               "#define ZANNA_SOURCE_STATE_STR \"clean\"\n";
    }
    {
        std::ofstream out(stage / "share" / "man" / "man1" / "zanna.1");
        out << ".TH zanna 1\n";
    }
    {
        std::ofstream out(stage / "share" / "man" / "man7" / "zanna.7");
        out << ".TH zanna 7\n";
    }
    {
        std::ofstream out(stage / "share" / "doc" / "zanna" / "README.md");
        out << "Zanna\n";
    }
    {
        std::ofstream out(stage / "LICENSE");
        out << "GPL\n";
    }

    fs::create_directories(stage / "lib");
    for (std::string_view archive : zanna::runtime_manifest::kRuntimeComponentArchives) {
        std::ofstream out(stage / "lib" / archiveName(archive), std::ios::binary);
        out << "ar";
    }
#if ZANNA_BUILD_HAS_GRAPHICS
    {
        std::ofstream out(stage / "lib" / archiveName("zannagfx"), std::ios::binary);
        out << "gfx";
    }
#endif
#if ZANNA_BUILD_HAS_GUI
    {
        std::ofstream out(stage / "lib" / archiveName("zannagui"), std::ios::binary);
        out << "gui";
    }
#endif
#if ZANNA_BUILD_HAS_AUDIO
    {
        std::ofstream out(stage / "lib" / archiveName("zannaaud"), std::ios::binary);
        out << "aud";
    }
#endif
    return stage;
}

/// @brief Make a mock toolchain stage look like a Linux install tree.
/// @details Windows test hosts otherwise leave `.exe` and `.lib` suffixes in
///          cross-target Linux package fixtures, which makes archive-layout
///          assertions host-dependent.
/// @param stage Mock stage root returned by createMockToolchainStage().
static void normalizeMockStageForLinuxToolchain(const std::filesystem::path &stage) {
    namespace fs = std::filesystem;
#if ZANNA_HOST_WINDOWS
    std::vector<fs::path> executables;
    for (const auto &entry : fs::directory_iterator(stage / "bin")) {
        if (entry.is_regular_file() && entry.path().extension() == ".exe")
            executables.push_back(entry.path());
    }
    for (const auto &path : executables)
        fs::rename(path, path.parent_path() / path.stem());

    std::vector<fs::path> archives;
    for (const auto &entry : fs::directory_iterator(stage / "lib")) {
        if (entry.is_regular_file() && entry.path().extension() == ".lib")
            archives.push_back(entry.path());
    }
    for (const auto &path : archives)
        fs::rename(path, path.parent_path() / ("lib" + path.stem().string() + ".a"));

    std::vector<uint8_t> elf(64, 0);
    elf[0] = 0x7F;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2;
    elf[5] = 1;
    elf[6] = 1;
    elf[16] = 3;
    elf[18] = 62;
    elf[20] = 1;
    writeBytes(stage / "bin" / "zanna", elf);
#else
    (void)stage;
#endif
}

/// @brief Make a mock toolchain stage look like a Windows install tree.
/// @details Non-Windows test hosts create the primary mock executable as
///          `bin/zanna`; Windows toolchain installer tests need `bin/zanna.exe`
///          so the manifest and payload names match the target platform.
/// @param stage Mock stage root returned by createMockToolchainStage().
static void normalizeMockStageForWindowsToolchain(const std::filesystem::path &stage) {
    namespace fs = std::filesystem;
#if !defined(_WIN32)
    std::vector<fs::path> extensionless;
    for (const auto &entry : fs::directory_iterator(stage / "bin")) {
        if (entry.is_regular_file() && entry.path().extension().empty())
            extensionless.push_back(entry.path());
    }
    for (const auto &path : extensionless) {
        fs::path renamed = path;
        renamed += ".exe";
        fs::copy_file(path, renamed);
        fs::remove(path);
    }
#else
    (void)stage;
#endif
    const fs::path studio = stage / "bin" / "zannastudio.exe";
    writeTestWindowsPe(studio, "x64");
    const std::vector<uint8_t> studioBytes = readFile(studio.string());
    std::ofstream buildInfo(stage / "bin" / "zannastudio.buildinfo",
                            std::ios::binary | std::ios::trunc);
    buildInfo << "Zanna Studio 9.8.7-snapshot\n"
                 "Schema: 1\n"
                 "Build: 2026-01-01T00:00:00.000Z\n"
                 "Source: 0123456789ab\n"
                 "Architecture: x64\n"
                 "Size: "
              << studioBytes.size()
              << "\nSHA256: " << sha256Hex(studioBytes.data(), studioBytes.size())
              << "\nOutput: C:\\build\\zannastudio.exe\n"
                 "Zanna: C:\\build\\zanna.exe\n";
}

/// @brief Refresh mock Studio provenance after a test changes its PE architecture or bytes.
static void refreshMockWindowsStudioBuildInfo(const std::filesystem::path &stage,
                                              std::string_view architecture) {
    const std::filesystem::path studio = stage / "bin" / "zannastudio.exe";
    const std::vector<uint8_t> studioBytes = readFile(studio.string());
    std::ofstream buildInfo(stage / "bin" / "zannastudio.buildinfo",
                            std::ios::binary | std::ios::trunc);
    buildInfo << "Zanna Studio 9.8.7-snapshot\n"
                 "Schema: 1\n"
                 "Build: 2026-01-01T00:00:00.000Z\n"
                 "Source: 0123456789ab\n"
                 "Architecture: "
              << architecture << "\nSize: " << studioBytes.size()
              << "\nSHA256: " << sha256Hex(studioBytes.data(), studioBytes.size())
              << "\nOutput: C:\\build\\zannastudio.exe\n"
                 "Zanna: C:\\build\\zanna.exe\n";
}

/// @brief Add deterministic branding and samples to exercise optional Windows toolchain payloads.
static void addMockWindowsToolchainBranding(const std::filesystem::path &stage) {
    namespace fs = std::filesystem;
    fs::create_directories(stage / "share" / "zanna" / "branding");
    fs::create_directories(stage / "share" / "zanna" / "samples" / "hello");
    PkgImage logo;
    logo.width = 64;
    logo.height = 64;
    logo.pixels.resize(64u * 64u * 4u);
    for (uint32_t y = 0; y < logo.height; ++y) {
        for (uint32_t x = 0; x < logo.width; ++x) {
            uint8_t *pixel = logo.at(x, y);
            pixel[0] = static_cast<uint8_t>(48u + x * 2u);
            pixel[1] = static_cast<uint8_t>(112u + y * 2u);
            pixel[2] = 54;
            pixel[3] = 255;
        }
    }
    PkgImage wallpaper;
    wallpaper.width = 192;
    wallpaper.height = 128;
    wallpaper.pixels.resize(192u * 128u * 4u);
    for (uint32_t y = 0; y < wallpaper.height; ++y) {
        for (uint32_t x = 0; x < wallpaper.width; ++x) {
            uint8_t *pixel = wallpaper.at(x, y);
            pixel[0] = static_cast<uint8_t>(4u + (x * 19u) / wallpaper.width);
            pixel[1] = static_cast<uint8_t>(24u + (y * 65u) / wallpaper.height);
            pixel[2] = static_cast<uint8_t>(24u + (x * 36u) / wallpaper.width);
            pixel[3] = 255;
        }
    }
    pngWrite((stage / "share" / "zanna" / "branding" / "zannalogo2.png").string(), logo);
    pngWrite((stage / "share" / "zanna" / "branding" / "zannawallpaper2.png").string(), wallpaper);
    std::ofstream sample(stage / "share" / "zanna" / "samples" / "hello" / "main.zia");
    sample << "func main() { print(\"hello\") }\n";
    std::ofstream sampleReadme(stage / "share" / "zanna" / "samples" / "hello" / "README.md");
    sampleReadme << "Sample documentation, not the toolchain README.\n";
    std::ofstream sampleLicense(stage / "share" / "zanna" / "samples" / "hello" / "LICENSE");
    sampleLicense << "Sample-specific terms, not the toolchain license.\n";
}

static void addMockMacOSFileHandler(const std::filesystem::path &stage) {
    namespace fs = std::filesystem;
    fs::create_directories(stage / "libexec" / "zanna");
    const fs::path handlerPath = stage / "libexec" / "zanna" / "zanna-file-handler";
    std::ofstream out(handlerPath, std::ios::binary);
    out << "handler";
    out.close();
    fs::permissions(handlerPath,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                        fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                        fs::perms::others_exec,
                    fs::perm_options::replace);
}

// ============================================================================
// DEFLATE Tests
// ============================================================================

TEST(WindowsInstallerMetadata, RoundTripsCanonicalSchemaWithUnicode) {
    const WindowsInstallerMetadata source = sampleWindowsInstallerMetadata();
    const std::string encoded = serializeWindowsInstallerMetadata(source);
    const WindowsInstallerMetadata parsed = parseWindowsInstallerMetadata(encoded);

    EXPECT_EQ(parsed.schemaVersion, 3U);
    EXPECT_EQ(parsed.productKind, "toolchain");
    EXPECT_EQ(parsed.identifier, source.identifier);
    EXPECT_EQ(parsed.displayName, source.displayName);
    EXPECT_EQ(parsed.version, source.version);
    EXPECT_EQ(parsed.installedSizeBytes, 140U);
    ASSERT_EQ(parsed.components.size(), 2U);
    EXPECT_EQ(parsed.components[0].id, "core");
    EXPECT_EQ(parsed.components[1].id, "ide");
    ASSERT_EQ(parsed.payloadFiles.size(), 4U);
    EXPECT_EQ(parsed.payloadFiles[1].componentId, "ide");
    ASSERT_EQ(parsed.shortcuts.size(), 1U);
    EXPECT_EQ(parsed.shortcuts[0].root, "start-menu");
    ASSERT_EQ(parsed.associations.size(), 1U);
    EXPECT_EQ(parsed.associations[0].progId, "org.zanna.toolchain.Zia.1");
    EXPECT_EQ(serializeWindowsInstallerMetadata(parsed), encoded);
}

TEST(WindowsInstallerMetadata, EscapesDelimitersAndRejectsNonCanonicalEscapes) {
    WindowsInstallerMetadata metadata = sampleWindowsInstallerMetadata();
    metadata.description = "tabs\tnewlines\npercent%";
    const std::string encoded = serializeWindowsInstallerMetadata(metadata);
    EXPECT_TRUE(encoded.find("tabs%09newlines%0Apercent%25") != std::string::npos);
    EXPECT_EQ(parseWindowsInstallerMetadata(encoded).description, metadata.description);

    std::string malformed = encoded;
    const size_t escape = malformed.find("%09");
    ASSERT_TRUE(escape != std::string::npos);
    malformed.replace(escape, 3, "%0g");
    EXPECT_THROWS(parseWindowsInstallerMetadata(malformed), std::runtime_error);
}

TEST(WindowsInstallerMetadata, RejectsTraversalDuplicatesAndSizeMismatch) {
    WindowsInstallerMetadata metadata = sampleWindowsInstallerMetadata();
    metadata.payloadFiles[0].path = "../outside.exe";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);

    metadata = sampleWindowsInstallerMetadata();
    metadata.payloadFiles.push_back(metadata.payloadFiles.front());
    metadata.installedSizeBytes += metadata.payloadFiles.back().sizeBytes;
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);

    metadata = sampleWindowsInstallerMetadata();
    metadata.payloadFiles[1].path = "bin\\zanna.exe";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);

    metadata = sampleWindowsInstallerMetadata();
    metadata.payloadFiles[0].path = "bin/CON.exe";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);

    metadata = sampleWindowsInstallerMetadata();
    metadata.payloadFiles[0].path = "bin/zanna.exe.";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);

    metadata = sampleWindowsInstallerMetadata();
    ++metadata.installedSizeBytes;
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
}

TEST(WindowsInstallerMetadata, RejectsEveryWindowsDeviceAliasAndAmbiguousOsVersion) {
    for (const std::string_view leaf :
         {"CLOCK$", "conin$.txt", "CONOUT$", "COM\xc2\xb9.log", "lpt\xc2\xb2", "COM\xc2\xb3.bin"}) {
        WindowsInstallerMetadata metadata = sampleWindowsInstallerMetadata();
        metadata.payloadFiles[0].path = "bin/" + std::string(leaf);
        EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    }

    WindowsInstallerMetadata metadata = sampleWindowsInstallerMetadata();
    metadata.minimumWindowsVersion = "010.0.19045";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
}

TEST(WindowsInstallerMetadata, RejectsUnknownFieldsAndComponentReferences) {
    const std::string encoded = serializeWindowsInstallerMetadata(sampleWindowsInstallerMetadata());
    std::string unknown = encoded;
    unknown += "surprise\tvalue\n";
    EXPECT_THROWS(parseWindowsInstallerMetadata(unknown), std::runtime_error);

    WindowsInstallerMetadata metadata = sampleWindowsInstallerMetadata();
    metadata.payloadFiles[1].componentId = "missing";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
}

TEST(WindowsInstallerMetadata, RequiresEveryScalarAndSafeExecutablePaths) {
    const std::string encoded = serializeWindowsInstallerMetadata(sampleWindowsInstallerMetadata());
    std::string missing = encoded;
    const std::string publisher = "publisher\tZanna Project\n";
    const size_t publisherOffset = missing.find(publisher);
    ASSERT_TRUE(publisherOffset != std::string::npos);
    missing.erase(publisherOffset, publisher.size());
    EXPECT_THROWS(parseWindowsInstallerMetadata(missing), std::runtime_error);

    WindowsInstallerMetadata metadata = sampleWindowsInstallerMetadata();
    metadata.executableName = "C:/outside.exe";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.displayIconRelativePath = "../icon.ico";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.defaultInstallDir = "Zanna/child";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.defaultInstallDir = "CON.txt";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.defaultInstallDir = "Zanna. ";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.channel = "Preview Channel";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
}

TEST(WindowsInstallerMetadata, RequiresCompletePinnedUpdateIdentity) {
    WindowsInstallerMetadata metadata = sampleWindowsInstallerMetadata();
    metadata.updateManifestUrl = "https://updates.example.test/zanna.txt";
    metadata.updateRsaModulus = "a1" + std::string(509, '0') + "1";
    metadata.updateRsaExponent = "010001";
    EXPECT_NO_THROW(serializeWindowsInstallerMetadata(metadata));

    metadata.updateRsaExponent.clear();
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.updateManifestUrl = "https://updates.example.test/zanna.txt";
    metadata.updateRsaModulus = "a1" + std::string(509, '0') + "1";
    metadata.updateRsaExponent = "010000";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata.updateRsaExponent = "0003";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.updateManifestUrl = "http://updates.example.test/zanna.txt";
    metadata.updateRsaModulus = "a1" + std::string(509, '0') + "1";
    metadata.updateRsaExponent = "010001";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);

    metadata = sampleWindowsInstallerMetadata();
    metadata.updateManifestUrl = "https://updates.example.test/zanna.txt";
    metadata.updateRsaModulus = "71" + std::string(509, '0') + "1";
    metadata.updateRsaExponent = "010001";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);

    metadata.updateRsaModulus = "a1" + std::string(510, '0');
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
}

TEST(WindowsInstallerMetadata, RejectsUnsafeShortcutContracts) {
    WindowsInstallerMetadata metadata = sampleWindowsInstallerMetadata();
    metadata.shortcuts.front().targetRoot = "registry";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.shortcuts.front().argumentPrefix = "/k";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.shortcuts.front().iconPath = "../icon.ico";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.shortcuts.front().relativePath = "Zanna Studio.url";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.shortcuts.front().targetPath = "bin/missing.exe";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
}

TEST(WindowsInstallerMetadata, RejectsUnsafeUrlsAndAssociationRegistryNames) {
    WindowsInstallerMetadata metadata = sampleWindowsInstallerMetadata();
    metadata.homepage = "https://example.invalid/\"><broken";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.documentationUrl = "https://user@example.invalid/docs";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.documentationUrl = "https://example..invalid/docs";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.documentationUrl = "https://example.invalid:99999/docs";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.associations.front().extension = ".zia\\Injected";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.associations.front().progId = "org.zanna\\Injected";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.associations.front().arguments = "/safe & calc";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
}

TEST(WindowsInstallerMetadata, RequiresRealIntegrationPayloadsAndBoundsUiRecords) {
    WindowsInstallerMetadata metadata = sampleWindowsInstallerMetadata();
    metadata.addToPath = true;
    metadata.pathRelativePath.clear();
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.createShortcuts = true;
    metadata.shortcuts.clear();
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.executableName = "bin/missing.exe";
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.installedManifestRelativePath = metadata.payloadFiles.front().path;
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.payloadEntry = metadata.outerFiles.front().overlayPath;
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.components.front().label.assign(257U, 'x');
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.components.resize(65U, metadata.components.back());
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
    metadata = sampleWindowsInstallerMetadata();
    metadata.displayName = std::string("\xF0\x28\x8C\x28", 4);
    EXPECT_THROWS(serializeWindowsInstallerMetadata(metadata), std::runtime_error);
}

TEST(Deflate, RoundTripEmpty) {
    std::vector<uint8_t> input;
    auto compressed = deflate(input.data(), input.size());
    auto decompressed = inflate(compressed.data(), compressed.size());
    EXPECT_EQ(decompressed.size(), static_cast<size_t>(0));
}

TEST(Gzip, RoundTripEmptyNullInput) {
    auto compressed = gzip(nullptr, 0);
    auto decompressed = gunzip(compressed.data(), compressed.size());
    EXPECT_EQ(decompressed.size(), static_cast<size_t>(0));
}

TEST(PkgHash, KnownAnswers) {
    const char *abc = "abc";
    EXPECT_EQ(sha1Hex(nullptr, 0), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(sha1Hex(reinterpret_cast<const uint8_t *>(abc), 3),
              "a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_EQ(sha256Hex(nullptr, 0),
              "e3b0c44298fc1c149afbf4c8996fb924"
              "27ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256Hex(reinterpret_cast<const uint8_t *>(abc), 3),
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad");
}

TEST(Zlib, RoundTripAndRejectsChecksumMismatch) {
    const char *msg = "zlib framing around native deflate";
    auto compressed = zlibCompress(reinterpret_cast<const uint8_t *>(msg), std::strlen(msg));
    auto decompressed = zlibDecompress(compressed.data(), compressed.size(), std::strlen(msg));
    EXPECT_EQ(std::string(decompressed.begin(), decompressed.end()), std::string(msg));
    compressed.back() ^= 0x1u;
    EXPECT_THROWS(zlibDecompress(compressed.data(), compressed.size(), std::strlen(msg)),
                  std::runtime_error);
}

TEST(CpioWriter, EmitsPortableAsciiPayloadWithSymlinksAndTrailer) {
    CpioWriter cpio;
    cpio.addDirectory(".");
    cpio.addDirectory("usr/local/bin");
    cpio.addDirectory("usr/local/zanna/bin");
    cpio.addFileString("usr/local/zanna/bin/zanna", "stub", 0755);
    cpio.addSymlink("usr/local/bin/zanna", "../zanna/bin/zanna");
    const auto bytes = cpio.finish();
    ASSERT_GE(bytes.size(), static_cast<size_t>(110));
    EXPECT_TRUE(std::memcmp(bytes.data(), "070707", 6) == 0);
    const std::string text(bytes.begin(), bytes.end());
    EXPECT_TRUE(text.find("./usr/local/zanna/bin/zanna") != std::string::npos);
    EXPECT_TRUE(text.find("TRAILER!!!") != std::string::npos);
    std::ostringstream err;
    EXPECT_TRUE(
        verifyCpioNewcPayload(bytes, {"usr/local/zanna/bin/zanna", "usr/local/bin/zanna"}, err));
}

TEST(XarWriter, EmitsVerifiableSha1ZlibArchive) {
    XarWriter xar;
    xar.addFileString("Distribution", "<installer-gui-script/>", true);
    xar.addFileString("ZannaToolchain.pkg", "component", false);
    const auto bytes = xar.finish();
    ASSERT_GE(bytes.size(), static_cast<size_t>(28));
    EXPECT_EQ(bytes[0], static_cast<uint8_t>('x'));
    EXPECT_EQ(bytes[1], static_cast<uint8_t>('a'));
    EXPECT_EQ(bytes[2], static_cast<uint8_t>('r'));
    EXPECT_EQ(bytes[3], static_cast<uint8_t>('!'));
    const uint16_t headerSize = readBE16(bytes.data() + 4);
    EXPECT_EQ(headerSize, static_cast<uint16_t>(28));
    const uint64_t tocCompressed = readBE64(bytes.data() + 8);
    const uint64_t tocUncompressed = readBE64(bytes.data() + 16);
    const auto toc = zlibDecompress(bytes.data() + headerSize,
                                    static_cast<size_t>(tocCompressed),
                                    static_cast<size_t>(tocUncompressed));
    const std::string tocText(toc.begin(), toc.end());
    EXPECT_TRUE(tocText.find("<name>Distribution</name>") != std::string::npos);
    EXPECT_TRUE(tocText.find("<checksum style=\"sha1\">") != std::string::npos);
    std::ostringstream err;
    EXPECT_TRUE(verifyXar(bytes, err));
}

TEST(ZipWriter, ProducesByteIdenticalArchivesForIdenticalInput) {
    // Reproducible builds: identical input must yield byte-identical archives across runs,
    // independent of wall-clock time or host timezone (the DOS date field is the usual culprit).
    auto build = [] {
        ZipWriter zip;
        zip.addFileString("readme.txt", "hello zanna", 0100644);
        zip.addDirectory("data");
        zip.addFileString("data/payload.bin", std::string(256, 'x'), 0100644);
        return zip.finishToVector();
    };
    const auto first = build();
    const auto second = build();
    ASSERT_GT(first.size(), static_cast<size_t>(0));
    EXPECT_EQ(first.size(), second.size());
    EXPECT_TRUE(first == second);
}

TEST(Deflate, RoundTripSmall) {
    const char *msg = "Hello, ZAPS packaging!";
    auto len = std::strlen(msg);
    auto compressed = deflate(reinterpret_cast<const uint8_t *>(msg), len);
    auto decompressed = inflate(compressed.data(), compressed.size());
    EXPECT_EQ(decompressed.size(), len);
    EXPECT_TRUE(std::memcmp(decompressed.data(), msg, len) == 0);
}

TEST(Deflate, RoundTripMixedDataLevel6) {
    // Mixed data at compression level 6 (LZ77+Huffman)
    std::vector<uint8_t> input;
    for (int i = 0; i < 500; ++i)
        input.push_back(static_cast<uint8_t>(i % 256));
    auto compressed = deflate(input.data(), input.size(), 6);
    auto decompressed = inflate(compressed.data(), compressed.size());
    EXPECT_EQ(decompressed.size(), input.size());
    EXPECT_TRUE(std::memcmp(decompressed.data(), input.data(), input.size()) == 0);
}

TEST(Deflate, RoundTripRepetitiveLevel6) {
    // Highly repetitive data — exercises LZ77 match finding
    std::vector<uint8_t> input(1000, 'A');
    auto compressed = deflate(input.data(), input.size(), 6);
    EXPECT_LT(compressed.size(), input.size()); // Should compress well
    auto decompressed = inflate(compressed.data(), compressed.size());
    EXPECT_EQ(decompressed.size(), input.size());
    EXPECT_TRUE(std::memcmp(decompressed.data(), input.data(), input.size()) == 0);
}

TEST(Deflate, AllLevels) {
    const char *msg = "Test data for compression levels 1 through 9 !!!";
    auto len = std::strlen(msg);
    for (int level = 1; level <= 9; ++level) {
        auto compressed = deflate(reinterpret_cast<const uint8_t *>(msg), len, level);
        auto decompressed = inflate(compressed.data(), compressed.size());
        EXPECT_EQ(decompressed.size(), len);
        EXPECT_TRUE(std::memcmp(decompressed.data(), msg, len) == 0);
    }
}

TEST(Deflate, TruncatedInputThrows) {
    const char *msg = "truncated deflate stream";
    auto compressed = deflate(reinterpret_cast<const uint8_t *>(msg), std::strlen(msg));
    ASSERT_GT(compressed.size(), static_cast<size_t>(1));
    compressed.pop_back();
    EXPECT_THROWS(inflate(compressed.data(), compressed.size()), DeflateError);
}

TEST(Deflate, RejectsTrailingCompressedData) {
    const char *msg = "trailing deflate stream";
    auto compressed = deflate(reinterpret_cast<const uint8_t *>(msg), std::strlen(msg));
    compressed.push_back(0);
    EXPECT_THROWS(inflate(compressed.data(), compressed.size()), DeflateError);
}

TEST(Deflate, ExplicitOutputLimit) {
    const char *msg = "bounded inflate";
    const size_t len = std::strlen(msg);
    auto compressed = deflate(reinterpret_cast<const uint8_t *>(msg), len);
    EXPECT_THROWS(inflate(compressed.data(), compressed.size(), len - 1), DeflateError);
    auto decompressed = inflate(compressed.data(), compressed.size(), len);
    EXPECT_EQ(std::string(decompressed.begin(), decompressed.end()), std::string(msg));
}

// ============================================================================
// ZIP Tests
// ============================================================================

TEST(Zip, EmptyArchive) {
    ZipWriter zip;
    auto data = zip.finishToVector();
    // Minimal ZIP: end-of-central-directory record (22 bytes)
    EXPECT_GE(data.size(), static_cast<size_t>(22));
    // End-of-central-directory signature at the end
    size_t eocdOff = data.size() - 22;
    EXPECT_EQ(readLE32(data.data() + eocdOff), static_cast<uint32_t>(0x06054B50));
}

TEST(Zip, SingleFileLocalHeader) {
    ZipWriter zip;
    const char *content = "Hello ZIP";
    zip.addFile("test.txt", reinterpret_cast<const uint8_t *>(content), std::strlen(content));
    auto data = zip.finishToVector();

    // Local file header signature at offset 0
    EXPECT_EQ(readLE32(data.data()), static_cast<uint32_t>(0x04034B50));

    // Version needed = 20
    EXPECT_EQ(readLE16(data.data() + 4), static_cast<uint16_t>(20));

    // Filename length at offset 26
    uint16_t nameLen = readLE16(data.data() + 26);
    EXPECT_EQ(nameLen, static_cast<uint16_t>(8)); // "test.txt"

    // Verify filename
    EXPECT_TRUE(std::memcmp(data.data() + 30, "test.txt", 8) == 0);
}

TEST(Zip, DirectoryEntry) {
    ZipWriter zip;
    zip.addDirectory("mydir/");
    auto data = zip.finishToVector();

    // Local header present
    EXPECT_EQ(readLE32(data.data()), static_cast<uint32_t>(0x04034B50));

    // Filename should be "mydir/"
    uint16_t nameLen = readLE16(data.data() + 26);
    EXPECT_EQ(nameLen, static_cast<uint16_t>(6));
    EXPECT_TRUE(std::memcmp(data.data() + 30, "mydir/", 6) == 0);

    // Compressed/uncompressed size should be 0 for directory
    EXPECT_EQ(readLE32(data.data() + 18), static_cast<uint32_t>(0)); // compressed
    EXPECT_EQ(readLE32(data.data() + 22), static_cast<uint32_t>(0)); // uncompressed
}

TEST(Zip, RootDirectoryEntryIsNoop) {
    ZipWriter zip;
    zip.addDirectory("./");
    auto data = zip.finishToVector();

    ZipReader reader(data.data(), data.size());
    EXPECT_EQ(reader.entries().size(), static_cast<size_t>(0));
}

TEST(Zip, CentralDirectoryPresent) {
    ZipWriter zip;
    zip.addFileString("a.txt", "alpha");
    zip.addFileString("b.txt", "bravo");
    auto data = zip.finishToVector();

    // Find central directory headers (signature 0x02014B50)
    int centralCount = 0;
    for (size_t i = 0; i + 4 <= data.size(); ++i) {
        if (readLE32(data.data() + i) == 0x02014B50)
            centralCount++;
    }
    EXPECT_EQ(centralCount, 2);

    // End-of-central-directory should report 2 entries
    size_t eocdOff = data.size() - 22;
    EXPECT_EQ(readLE32(data.data() + eocdOff), static_cast<uint32_t>(0x06054B50));
    // Total entries on disk (offset 10 in EOCD)
    EXPECT_EQ(readLE16(data.data() + eocdOff + 10), static_cast<uint16_t>(2));
}

TEST(Zip, UnixPermissionsEncoded) {
    ZipWriter zip;
    zip.addFile("exec", reinterpret_cast<const uint8_t *>("x"), 1, 0100755);
    auto data = zip.finishToVector();

    // Find central directory header
    size_t cdOff = 0;
    for (size_t i = 0; i + 4 <= data.size(); ++i) {
        if (readLE32(data.data() + i) == 0x02014B50) {
            cdOff = i;
            break;
        }
    }
    ASSERT_TRUE(cdOff > 0);

    // version_made_by at offset 4: should be Unix (high byte = 3)
    uint16_t versionMadeBy = readLE16(data.data() + cdOff + 4);
    EXPECT_EQ(versionMadeBy >> 8, 3); // Unix

    // External attributes at offset 38: upper 16 bits = Unix mode
    uint32_t extAttrs = readLE32(data.data() + cdOff + 38);
    uint16_t unixMode = static_cast<uint16_t>(extAttrs >> 16);
    EXPECT_EQ(unixMode, static_cast<uint16_t>(0100755));
}

TEST(Zip, RejectsTooLongNames) {
    ZipWriter zip;
    std::string longName(70000, 'a');
    EXPECT_THROWS(zip.addFileString(longName, "x"), std::runtime_error);
}

TEST(Zip, WriterRejectsTraversalNames) {
    ZipWriter zip;
    EXPECT_THROWS(zip.addFileString("../escape.txt", "x"), std::runtime_error);
    EXPECT_THROWS(zip.addDirectory("/absolute"), std::runtime_error);
}

TEST(Zip, NormalizesBackslashEntryNames) {
    ZipWriter zip;
    zip.addFileString("assets\\config.txt", "x");
    auto data = zip.finishToVector();

    ZipReader reader(data.data(), data.size());
    EXPECT_TRUE(reader.find("assets/config.txt") != nullptr);
    EXPECT_TRUE(reader.find("assets\\config.txt") == nullptr);
}

TEST(Zip, WriterRejectsDuplicateEntryNamesAfterNormalization) {
    ZipWriter zip;
    zip.addFileString("assets/config.txt", "x");
    EXPECT_THROWS(zip.addFileString("assets\\config.txt", "y"), std::runtime_error);
}

TEST(Zip, ReaderRejectsUnsafeCentralDirectoryName) {
    ZipWriter zip;
    zip.addFileString("safe.txt", "x");
    auto data = zip.finishToVector();

    for (size_t i = 0; i + 46 <= data.size(); ++i) {
        if (readLE32(data.data() + i) == 0x02014B50) {
            const char unsafe[] = "../a.txt";
            std::memcpy(data.data() + i + 46, unsafe, sizeof(unsafe) - 1);
            break;
        }
    }

    EXPECT_THROWS(ZipReader(data.data(), data.size()), ZipReadError);
}

TEST(Zip, ReaderRejectsDuplicateCentralDirectoryNames) {
    ZipWriter zip;
    zip.addFileString("aaaa.txt", "a");
    zip.addFileString("bbbb.txt", "b");
    auto data = zip.finishToVector();

    bool first = true;
    for (size_t i = 0; i + 46 <= data.size(); ++i) {
        if (readLE32(data.data() + i) != 0x02014B50)
            continue;
        if (first) {
            first = false;
            continue;
        }
        const char duplicate[] = "aaaa.txt";
        std::memcpy(data.data() + i + 46, duplicate, sizeof(duplicate) - 1);
        break;
    }

    EXPECT_THROWS(ZipReader(data.data(), data.size()), ZipReadError);
}

TEST(Zip, SupportsZeroByteNullDataFile) {
    ZipWriter zip;
    zip.addFile("empty.bin", nullptr, 0);
    auto data = zip.finishToVector();

    ZipReader reader(data.data(), data.size());
    const auto *entry = reader.find("empty.bin");
    ASSERT_TRUE(entry != nullptr);
    auto extracted = reader.extract(*entry);
    EXPECT_EQ(extracted.size(), static_cast<size_t>(0));
}

TEST(Zip, RejectsUseAfterFinish) {
    ZipWriter zip;
    zip.addFileString("a.txt", "a");
    (void)zip.finishToVector();
    EXPECT_THROWS(zip.addFileString("b.txt", "b"), std::runtime_error);
    EXPECT_THROWS(zip.finishToVector(), std::runtime_error);
}

TEST(Zip, RejectsEscapingSymlinkTargets) {
    ZipWriter zip;
    zip.addDirectory("bin");
    EXPECT_THROWS(zip.addSymlink("bin/tool", "../../outside"), std::runtime_error);

    ZipWriter absolute;
    EXPECT_THROWS(absolute.addSymlink("bin/tool", "/usr/bin/tool"), std::runtime_error);
}

TEST(Zip, AllowsInternalSymlinkTargets) {
    ZipWriter zip;
    zip.addDirectory("bin");
    zip.addDirectory("lib");
    zip.addFileString("lib/tool-real", "x");
    zip.addSymlink("bin/tool", "../lib/tool-real");
    auto data = zip.finishToVector();

    ZipReader reader(data.data(), data.size());
    EXPECT_TRUE(reader.find("bin/tool") != nullptr);
}

TEST(Zip, StoresNormalizedSymlinkTarget) {
    ZipWriter zip;
    zip.addDirectory("bin");
    zip.addDirectory("lib");
    zip.addFileString("lib/tool-real", "x");
    zip.addSymlink("bin/tool", "..\\lib\\tool-real");
    auto data = zip.finishToVector();

    ZipReader reader(data.data(), data.size());
    const ZipEntry *entry = reader.find("bin/tool");
    ASSERT_TRUE(entry != nullptr);
    const auto target = reader.extract(*entry);
    EXPECT_EQ(std::string(target.begin(), target.end()), std::string("../lib/tool-real"));
}

// ============================================================================
// Tar Tests
// ============================================================================

TEST(Tar, EmptyArchive) {
    TarWriter tar;
    auto data = tar.finish();
    // Empty tar: just two 512-byte zero blocks = 1024 bytes
    EXPECT_EQ(data.size(), static_cast<size_t>(1024));
    // All zeros
    bool allZero = true;
    for (auto b : data)
        if (b != 0)
            allZero = false;
    EXPECT_TRUE(allZero);
}

TEST(Tar, FileHeaderMagic) {
    TarWriter tar;
    tar.addFileString("./hello.txt", "Hello");
    auto data = tar.finish();

    // USTAR magic at offset 257: "ustar\0" (6 bytes)
    EXPECT_TRUE(std::memcmp(data.data() + 257, "ustar", 5) == 0);
    EXPECT_EQ(data[257 + 5], static_cast<uint8_t>(0)); // NUL-terminated magic
    // Version at offset 263: "00"
    EXPECT_EQ(data[263], '0');
    EXPECT_EQ(data[264], '0');
}

TEST(Tar, FileNameInHeader) {
    TarWriter tar;
    tar.addFileString("./usr/bin/test", "content");
    auto data = tar.finish();

    // Name field at offset 0, 100 bytes
    EXPECT_TRUE(std::memcmp(data.data(), "usr/bin/test", 12) == 0);
}

TEST(Tar, FileModeField) {
    TarWriter tar;
    tar.addFileString("./file", "data", 0755);
    auto data = tar.finish();

    // Mode field at offset 100, 8 bytes octal
    // 0755 octal = "0000755\0"
    EXPECT_TRUE(std::memcmp(data.data() + 100, "0000755", 7) == 0);
}

TEST(Tar, DataAlignment) {
    TarWriter tar;
    // Add a file with content not a multiple of 512
    std::string content = "Short";
    tar.addFileString("./test", content);
    auto data = tar.finish();

    // Total: 512 (header) + 512 (data padded) + 1024 (end blocks) = 2048
    EXPECT_EQ(data.size(), static_cast<size_t>(2048));
}

TEST(Tar, DirectoryTypeflag) {
    TarWriter tar;
    tar.addDirectory("./mydir/", 0755);
    auto data = tar.finish();

    // Typeflag at offset 156: '5' for directory
    EXPECT_EQ(data[156], '5');
}

TEST(Tar, SupportsPaxOversizedPath) {
    TarWriter tar;
    const std::string path = std::string(180, 'a');
    tar.addFileString(std::string("./") + path, "data");
    auto data = tar.finish();

    std::ostringstream err;
    auto gz = gzip(data.data(), data.size());
    EXPECT_TRUE(verifyTarGzPayload(gz, {path}, err));
}

TEST(Tar, RejectsUnsafePath) {
    TarWriter tar;
    EXPECT_THROWS(tar.addFileString("../escape", "data"), std::runtime_error);
}

TEST(Tar, RejectsDuplicateEntryPath) {
    TarWriter tar;
    tar.addFileString("./dup.txt", "one");
    EXPECT_THROWS(tar.addFileString("dup.txt", "two"), std::runtime_error);
}

TEST(Tar, VerifierRejectsAppleDoubleEntryPath) {
    TarWriter tar;
    tar.addFileString("payload/._metadata", "sidecar");
    const auto tarBytes = tar.finish();
    const auto tarGz = gzip(tarBytes.data(), tarBytes.size());

    std::ostringstream err;
    EXPECT_FALSE(verifyTarGzPayload(tarGz, {}, err));
    EXPECT_TRUE(err.str().find("AppleDouble") != std::string::npos);
}

TEST(Tar, SupportsZeroByteNullDataFile) {
    TarWriter tar;
    tar.addFile("./empty", nullptr, 0);
    auto data = tar.finish();
    std::vector<uint8_t> extracted;
    EXPECT_TRUE(tarEntryData(data, "empty", extracted));
    EXPECT_EQ(extracted.size(), static_cast<size_t>(0));
}

TEST(Tar, SplitsLongDirectoryPathWithoutEmptyName) {
    TarWriter tar;
    const std::string longDir = "root/" + std::string(95, 'a') + "/leaf";
    tar.addDirectory(longDir);
    auto data = tar.finish();

    const char *name = reinterpret_cast<const char *>(data.data());
    size_t nameLen = 0;
    while (nameLen < 100 && name[nameLen] != '\0')
        ++nameLen;
    EXPECT_EQ(std::string(name, name + nameLen), std::string("leaf/"));
}

TEST(Tar, AllowsInternalRelativeSymlinkTarget) {
    TarWriter tar;
    tar.addDirectory("./bin", 0755);
    tar.addDirectory("./lib", 0755);
    tar.addFileString("./lib/tool-real", "x", 0755);
    tar.addSymlink("./bin/tool", "../lib/tool-real");
    auto data = tar.finish();

    std::ostringstream err;
    auto gz = gzip(data.data(), data.size());
    EXPECT_TRUE(verifyTarGz(gz, err));
}

TEST(Tar, StoresNormalizedSymlinkTarget) {
    TarWriter tar;
    tar.addDirectory("./bin", 0755);
    tar.addDirectory("./lib", 0755);
    tar.addFileString("./lib/tool-real", "x", 0755);
    tar.addSymlink("./bin/tool", "..\\lib\\tool-real");
    auto data = tar.finish();

    std::string target;
    ASSERT_TRUE(tarEntryLinkTarget(data, "bin/tool", target));
    EXPECT_EQ(target, std::string("../lib/tool-real"));
}

TEST(Tar, RejectsEscapingSymlinkTarget) {
    TarWriter tar;
    EXPECT_THROWS(tar.addSymlink("./bin/tool", "../../outside"), std::runtime_error);
}

TEST(Tar, VerifierRejectsDuplicateEntryPath) {
    TarWriter tar;
    tar.addFileString("./dup.txt", "x");
    auto one = tar.finish();
    ASSERT_GE(one.size(), static_cast<size_t>(2048));

    std::vector<uint8_t> dup;
    dup.insert(dup.end(), one.begin(), one.begin() + 1024);
    dup.insert(dup.end(), one.begin(), one.begin() + 1024);
    dup.insert(dup.end(), one.end() - 1024, one.end());
    auto gz = gzip(dup.data(), dup.size());

    std::ostringstream err;
    EXPECT_FALSE(verifyTarGz(gz, err));
    EXPECT_CONTAINS(err.str(), "duplicate entry");
}

// ============================================================================
// Ar Tests
// ============================================================================

TEST(Ar, GlobalMagic) {
    ArWriter ar;
    ar.addMemberString("test", "data");
    auto data = ar.finish();

    // ar magic: "!<arch>\n" (8 bytes)
    EXPECT_GE(data.size(), static_cast<size_t>(8));
    EXPECT_TRUE(std::memcmp(data.data(), "!<arch>\n", 8) == 0);
}

TEST(Ar, MemberHeader) {
    ArWriter ar;
    ar.addMemberString("hello", "world");
    auto data = ar.finish();

    // Member header starts at offset 8, 60 bytes
    // Name field: "hello/          " (16 bytes, "/" terminated, space padded)
    EXPECT_TRUE(std::memcmp(data.data() + 8, "hello/", 6) == 0);

    // File magic at end of header (offset 8+58): "`\n"
    EXPECT_EQ(data[8 + 58], '`');
    EXPECT_EQ(data[8 + 59], '\n');
}

TEST(Ar, DebianBinaryOrdering) {
    // .deb files must have debian-binary as first member
    ArWriter ar;
    ar.addMemberString("debian-binary", "2.0\n");
    ar.addMemberString("control.tar.gz", "ctrl");
    ar.addMemberString("data.tar.gz", "data");
    auto data = ar.finish();

    // First member name: "debian-binary"
    EXPECT_TRUE(std::memcmp(data.data() + 8, "debian-binary/", 14) == 0);
}

TEST(Ar, RejectsTooLongMemberName) {
    ArWriter ar;
    EXPECT_THROWS(ar.addMemberString("this-name-is-far-too-long", "data"), std::runtime_error);
}

TEST(Ar, SupportsZeroByteNullDataMember) {
    ArWriter ar;
    ar.addMember("empty", nullptr, 0);
    auto data = ar.finish();

    std::vector<uint8_t> member;
    ASSERT_TRUE(arMemberData(data, "empty", member));
    EXPECT_EQ(member.size(), static_cast<size_t>(0));
}

TEST(Ar, RejectsNonEmptyNullDataMember) {
    ArWriter ar;
    EXPECT_THROWS(ar.addMember("bad", nullptr, 1), std::runtime_error);
}

// ============================================================================
// PE Tests
// ============================================================================

TEST(PE, DosHeaderMagic) {
    PEBuildParams params;
    params.textSection = {0xC3}; // ret
    auto pe = buildPE(params);

    // DOS header: "MZ" at offset 0
    EXPECT_GE(pe.size(), static_cast<size_t>(2));
    EXPECT_EQ(pe[0], 'M');
    EXPECT_EQ(pe[1], 'Z');
}

TEST(PE, PESignature) {
    PEBuildParams params;
    params.textSection = {0xC3};
    auto pe = buildPE(params);

    // PE signature at offset 0x80: "PE\0\0"
    ASSERT_GE(pe.size(), static_cast<size_t>(0x84));
    EXPECT_EQ(pe[0x80], 'P');
    EXPECT_EQ(pe[0x81], 'E');
    EXPECT_EQ(pe[0x82], 0);
    EXPECT_EQ(pe[0x83], 0);
}

TEST(PE, MachineAMD64) {
    PEBuildParams params;
    params.textSection = {0xC3};
    params.arch = "x64";
    auto pe = buildPE(params);

    // COFF Machine at offset 0x84: 0x8664 = AMD64
    uint16_t machine = readLE16(pe.data() + 0x84);
    EXPECT_EQ(machine, static_cast<uint16_t>(0x8664));
}

TEST(PE, MachineARM64) {
    PEBuildParams params;
    params.textSection = {0xC0, 0x03, 0x5F, 0xD6}; // ARM64 ret
    params.arch = "arm64";
    auto pe = buildPE(params);

    // COFF Machine at offset 0x84: 0xAA64 = ARM64
    uint16_t machine = readLE16(pe.data() + 0x84);
    EXPECT_EQ(machine, static_cast<uint16_t>(0xAA64));
}

TEST(PE, InstallerManifestEnablesModernControlsAndDpi) {
    const std::string manifest = generateUacManifest("10.0");
    EXPECT_CONTAINS(manifest, "Microsoft.Windows.Common-Controls");
    EXPECT_CONTAINS(manifest, "PerMonitorV2, PerMonitor");
    EXPECT_CONTAINS(manifest, "<longPathAware");
    EXPECT_CONTAINS(manifest, "requireAdministrator");
    EXPECT_CONTAINS(manifest, "{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}");
}

TEST(PE, RejectsEmptyTextSection) {
    PEBuildParams params;
    EXPECT_THROWS(buildPE(params), std::runtime_error);
}

TEST(PE, RejectsEntryPointOutsideTextSection) {
    PEBuildParams params;
    params.textSection = {0x90, 0xC3};
    params.entryPointOffset = 2;
    EXPECT_THROWS(buildPE(params), std::runtime_error);
}

TEST(PE, OptionalHeaderMagic) {
    PEBuildParams params;
    params.textSection = {0xC3};
    auto pe = buildPE(params);

    // Optional header at offset 0x98: PE32+ magic = 0x020B
    uint16_t magic = readLE16(pe.data() + 0x98);
    EXPECT_EQ(magic, static_cast<uint16_t>(0x020B));
}

TEST(PE, OverlayAppended) {
    PEBuildParams params;
    params.textSection = {0xC3};
    std::vector<uint8_t> overlay = {'O', 'V', 'L', 'Y'};
    params.overlay = overlay;
    auto pe = buildPE(params);

    // Overlay should be at the very end of the PE
    ASSERT_GE(pe.size(), static_cast<size_t>(4));
    EXPECT_EQ(pe[pe.size() - 4], 'O');
    EXPECT_EQ(pe[pe.size() - 3], 'V');
    EXPECT_EQ(pe[pe.size() - 2], 'L');
    EXPECT_EQ(pe[pe.size() - 1], 'Y');
}

TEST(PE, DefaultDllCharacteristicsEnableRelocationAslr) {
    PEBuildParams params;
    params.textSection = {0xC3};
    auto pe = buildPE(params);

    const uint16_t dllChars = readLE16(pe.data() + 0x98 + 70);
    EXPECT_EQ(dllChars, static_cast<uint16_t>(0x8160));
    EXPECT_TRUE((dllChars & 0x0040) != 0); // DYNAMIC_BASE
    EXPECT_TRUE((dllChars & 0x0020) != 0); // HIGH_ENTROPY_VA
    EXPECT_TRUE((dllChars & 0x0100) != 0); // NX_COMPAT

    const uint32_t relocRva = readLE32(pe.data() + 0x98 + 112 + 5 * 8);
    const uint32_t relocSize = readLE32(pe.data() + 0x98 + 112 + 5 * 8 + 4);
    EXPECT_NE(relocRva, static_cast<uint32_t>(0));
    EXPECT_EQ(relocSize, static_cast<uint32_t>(12));
}

// ============================================================================
// LNK Tests
// ============================================================================

TEST(Lnk, HeaderSize) {
    LnkParams params;
    params.targetPath = "C:\\test.exe";
    params.workingDir = "C:\\";
    params.description = "Test";
    auto data = generateLnk(params);

    // ShellLinkHeader is always 76 bytes
    ASSERT_GE(data.size(), static_cast<size_t>(76));
    // HeaderSize field at offset 0: 0x0000004C (76)
    EXPECT_EQ(readLE32(data.data()), static_cast<uint32_t>(0x4C));
}

TEST(Lnk, LinkCLSID) {
    LnkParams params;
    params.targetPath = "C:\\test.exe";
    params.workingDir = "C:\\";
    params.description = "Test";
    auto data = generateLnk(params);

    // LinkCLSID at offset 4: {00021401-0000-0000-C000-000000000046}
    // In little-endian binary: 01 14 02 00 00 00 00 00 C0 00 00 00 00 00 00 46
    EXPECT_EQ(data[4], 0x01);
    EXPECT_EQ(data[5], 0x14);
    EXPECT_EQ(data[6], 0x02);
    EXPECT_EQ(data[7], 0x00);
    EXPECT_EQ(data[19], 0x46);
}

TEST(Lnk, HasLinkInfoFlag) {
    LnkParams params;
    params.targetPath = "C:\\Program Files\\App\\app.exe";
    params.workingDir = "C:\\Program Files\\App";
    params.description = "My App";
    auto data = generateLnk(params);

    // LinkFlags at offset 20: should have HasLinkInfo (bit 1 = 0x02)
    uint32_t flags = readLE32(data.data() + 20);
    EXPECT_TRUE((flags & 0x02) != 0); // HasLinkInfo
    EXPECT_TRUE((flags & 0x04) != 0); // HasName
    EXPECT_TRUE((flags & 0x08) != 0); // HasRelativePath
    EXPECT_TRUE((flags & 0x80) != 0); // IsUnicode
}

TEST(Lnk, EnvironmentTargetAddsExpStringBlock) {
    LnkParams params;
    params.targetPath = "%ProgramFiles%\\Test App\\testapp.exe";
    params.workingDir = "%ProgramFiles%\\Test App";
    params.description = "Test App";
    auto data = generateLnk(params);

    const uint32_t flags = readLE32(data.data() + 20);
    EXPECT_TRUE((flags & 0x00000200) != 0); // HasExpString
    bool foundBlock = false;
    for (size_t i = 0; i + 8 <= data.size(); ++i) {
        if (readLE32(data.data() + i) == 0x00000314 &&
            readLE32(data.data() + i + 4) == 0xA0000001) {
            foundBlock = true;
            break;
        }
    }
    EXPECT_TRUE(foundBlock);
}

TEST(Lnk, LinkInfoContainsLocalBasePath) {
    LnkParams params;
    params.targetPath = "C:\\MyApp\\app.exe";
    params.workingDir = "C:\\MyApp";
    params.description = "Test";
    auto data = generateLnk(params);

    // LinkInfo starts right after the 76-byte header
    // LinkInfoSize at offset 76
    ASSERT_GE(data.size(), static_cast<size_t>(80));
    uint32_t liSize = readLE32(data.data() + 76);
    EXPECT_GT(liSize, static_cast<uint32_t>(28)); // At least header + data

    // LinkInfoFlags at offset 76+8: should have VolumeIDAndLocalBasePath (bit 0)
    uint32_t liFlags = readLE32(data.data() + 76 + 8);
    EXPECT_TRUE((liFlags & 0x01) != 0);

    // LocalBasePath should contain the target path somewhere in the LinkInfo
    // LocalBasePathOffset at offset 76+16
    uint32_t lbpOffset = readLE32(data.data() + 76 + 16);
    ASSERT_LT(static_cast<size_t>(76 + lbpOffset + 15), data.size());
    // Check that it starts with "C:\MyApp\app.exe"
    std::string lbp(reinterpret_cast<const char *>(data.data() + 76 + lbpOffset));
    EXPECT_CONTAINS(lbp, "C:\\MyApp\\app.exe");
}

TEST(Lnk, Utf8StringDataUsesUtf16) {
    LnkParams params;
    params.targetPath = "C:\\Program Files\\Cafe\\cafe.exe";
    params.workingDir = "C:\\Program Files\\Cafe";
    params.description = "Cafe \xC3\xA9";
    auto data = generateLnk(params);

    bool foundEAcute = false;
    for (size_t i = 0; i + 1 < data.size(); ++i) {
        if (data[i] == 0xE9 && data[i + 1] == 0x00) {
            foundEAcute = true;
            break;
        }
    }
    EXPECT_TRUE(foundEAcute);
}

TEST(Lnk, EndsWithExtraDataTerminalBlock) {
    LnkParams params;
    params.targetPath = "C:\\test.exe";
    auto data = generateLnk(params);
    ASSERT_GE(data.size(), static_cast<size_t>(4));
    EXPECT_EQ(readLE32(data.data() + data.size() - 4), static_cast<uint32_t>(0));
}

// ============================================================================
// Icon Tests (DEFLATE double-free fixed — full tests now enabled)
// ============================================================================

TEST(Icon, ImageResizeBasic) {
    PkgImage img;
    img.width = 64;
    img.height = 64;
    img.pixels.resize(64 * 64 * 4, 128);

    auto resized = imageResize(img, 32, 32);
    EXPECT_EQ(resized.width, static_cast<uint32_t>(32));
    EXPECT_EQ(resized.height, static_cast<uint32_t>(32));
    EXPECT_EQ(resized.pixels.size(), static_cast<size_t>(32 * 32 * 4));
}

TEST(Icon, IcnsMagic) {
    PkgImage img;
    img.width = 32;
    img.height = 32;
    img.pixels.resize(32 * 32 * 4, 128);

    auto icns = generateIcns(img);
    ASSERT_GE(icns.size(), static_cast<size_t>(8));

    // ICNS magic: "icns"
    EXPECT_EQ(icns[0], 'i');
    EXPECT_EQ(icns[1], 'c');
    EXPECT_EQ(icns[2], 'n');
    EXPECT_EQ(icns[3], 's');

    // Total size field (big-endian) at offset 4
    uint32_t totalSize = readBE32(icns.data() + 4);
    EXPECT_EQ(totalSize, static_cast<uint32_t>(icns.size()));
}

TEST(Icon, IcoHeader) {
    PkgImage img;
    img.width = 32;
    img.height = 32;
    img.pixels.resize(32 * 32 * 4, 200);

    auto ico = generateIco(img);
    ASSERT_GE(ico.size(), static_cast<size_t>(6));

    // ICO header: Reserved=0, Type=1 (ICO), Count>=1
    EXPECT_EQ(readLE16(ico.data()), static_cast<uint16_t>(0));     // Reserved
    EXPECT_EQ(readLE16(ico.data() + 2), static_cast<uint16_t>(1)); // Type = ICO
    uint16_t count = readLE16(ico.data() + 4);
    EXPECT_GE(count, static_cast<uint16_t>(1));
}

TEST(Icon, IcoEntryBitCount) {
    PkgImage img;
    img.width = 64;
    img.height = 64;
    img.pixels.resize(64 * 64 * 4, 100);

    auto ico = generateIco(img);
    uint16_t count = readLE16(ico.data() + 4);
    ASSERT_GE(count, static_cast<uint16_t>(1));

    // First ICONDIRENTRY at offset 6, BitCount at +6: should be 32 (RGBA)
    uint16_t bitCount = readLE16(ico.data() + 6 + 6);
    EXPECT_EQ(bitCount, static_cast<uint16_t>(32));
}

TEST(Icon, RejectsTinyOrNonSquareSources) {
    PkgImage img;
    img.width = 1;
    img.height = 1;
    img.pixels.resize(4, 255);
    EXPECT_THROWS(generateIco(img), PNGError);
    EXPECT_THROWS(generateIcns(img), PNGError);
    EXPECT_THROWS(generateMultiSizePngs(img), PNGError);

    img.width = 64;
    img.height = 32;
    img.pixels.resize(64 * 32 * 4, 255);
    EXPECT_THROWS(generateIco(img), PNGError);
}

TEST(PNG, RejectsBadChunkCrc) {
    PkgImage img;
    img.width = 1;
    img.height = 1;
    img.pixels = {255, 0, 0, 255};
    auto png = pngEncode(img);
    ASSERT_GT(png.size(), static_cast<size_t>(32));
    png[29] ^= 0xFF; // IHDR CRC
    EXPECT_THROWS(pngReadMemory(png.data(), png.size()), PNGError);
}

TEST(PNG, RejectsNullInputBuffer) {
    EXPECT_THROWS(pngReadMemory(nullptr, 8), PNGError);
}

TEST(PNG, RejectsMismatchedPixelBuffer) {
    PkgImage img;
    img.width = 2;
    img.height = 2;
    img.pixels.resize(4);
    EXPECT_THROWS(pngEncode(img), PNGError);
}

TEST(PNG, RejectsTrailingBytesAfterIEND) {
    PkgImage img;
    img.width = 1;
    img.height = 1;
    img.pixels = {0, 0, 0, 255};
    auto png = pngEncode(img);
    png.push_back(0);
    EXPECT_THROWS(pngReadMemory(png.data(), png.size()), PNGError);
}

TEST(PNG, ResizeRejectsOverflowDimensions) {
    PkgImage img;
    img.width = 1;
    img.height = 1;
    img.pixels = {0, 0, 0, 255};
    EXPECT_THROWS(imageResize(img, 0xFFFFFFFFu, 2), PNGError);
}

TEST(PNG, ReadsIndexedPaletteTransparency) {
    const auto png = makeIndexedPng2x1WithTransparency();
    const auto img = pngReadMemory(png.data(), png.size());
    ASSERT_EQ(img.width, static_cast<uint32_t>(2));
    ASSERT_EQ(img.height, static_cast<uint32_t>(1));
    ASSERT_EQ(img.pixels.size(), static_cast<size_t>(8));
    EXPECT_EQ(img.pixels[0], static_cast<uint8_t>(255));
    EXPECT_EQ(img.pixels[1], static_cast<uint8_t>(0));
    EXPECT_EQ(img.pixels[2], static_cast<uint8_t>(0));
    EXPECT_EQ(img.pixels[3], static_cast<uint8_t>(255));
    EXPECT_EQ(img.pixels[4], static_cast<uint8_t>(0));
    EXPECT_EQ(img.pixels[5], static_cast<uint8_t>(0));
    EXPECT_EQ(img.pixels[6], static_cast<uint8_t>(255));
    EXPECT_EQ(img.pixels[7], static_cast<uint8_t>(0));
}

// ============================================================================
// Plist Tests
// ============================================================================

TEST(Plist, ContainsBundleIdentifier) {
    PlistParams params;
    params.executableName = "myapp";
    params.bundleId = "com.example.myapp";
    params.bundleName = "MyApp";
    params.version = "1.0.0";

    auto xml = generatePlist(params);
    EXPECT_CONTAINS(xml, "CFBundleIdentifier");
    EXPECT_CONTAINS(xml, "com.example.myapp");
}

TEST(Plist, ContainsBundleExecutable) {
    PlistParams params;
    params.executableName = "testbin";
    params.bundleId = "com.test";
    params.bundleName = "Test";
    params.version = "2.0";

    auto xml = generatePlist(params);
    EXPECT_CONTAINS(xml, "CFBundleExecutable");
    EXPECT_CONTAINS(xml, "testbin");
}

TEST(Plist, ContainsVersion) {
    PlistParams params;
    params.executableName = "app";
    params.bundleId = "com.test";
    params.bundleName = "App";
    params.version = "3.14.159";

    auto xml = generatePlist(params);
    EXPECT_CONTAINS(xml, "CFBundleVersion");
    EXPECT_CONTAINS(xml, "3.14.159");
}

TEST(Plist, PkgInfoContent) {
    auto pkgInfo = generatePkgInfo();
    EXPECT_EQ(pkgInfo, std::string("APPL????"));
}

// ============================================================================
// Desktop Entry Tests
// ============================================================================

TEST(DesktopEntry, ContainsDesktopEntrySection) {
    DesktopEntryParams params;
    params.name = "MyApp";
    params.execPath = "/usr/bin/myapp";
    params.iconName = "myapp";

    auto content = generateDesktopEntry(params);
    EXPECT_CONTAINS(content, "[Desktop Entry]");
    EXPECT_CONTAINS(content, "Type=Application");
}

TEST(DesktopEntry, ContainsExecAndName) {
    DesktopEntryParams params;
    params.name = "Test App";
    params.execPath = "/usr/bin/testapp";
    params.iconName = "testapp";
    params.comment = "A test application";

    auto content = generateDesktopEntry(params);
    EXPECT_CONTAINS(content, "Name=Test App");
    EXPECT_CONTAINS(content, "Exec=/usr/bin/testapp");
    EXPECT_CONTAINS(content, "Icon=testapp");
    EXPECT_CONTAINS(content, "Comment=A test application");
}

TEST(DesktopEntry, MimeTypeField) {
    DesktopEntryParams params;
    params.name = "Editor";
    params.execPath = "/usr/bin/editor";
    params.iconName = "editor";
    FileAssoc assoc;
    assoc.extension = ".zia";
    assoc.description = "Zia Source";
    assoc.mimeType = "text/x-zia";
    params.fileAssociations.push_back(assoc);

    auto content = generateDesktopEntry(params);
    EXPECT_CONTAINS(content, "MimeType=text/x-zia;");
}

TEST(DesktopEntry, FileAssociationExecAcceptsFileArgument) {
    DesktopEntryParams params;
    params.name = "Editor";
    params.execPath = "/usr/bin/editor";
    params.iconName = "editor";
    FileAssoc assoc;
    assoc.extension = ".zia";
    assoc.description = "Zia Source";
    assoc.mimeType = "text/x-zia";
    params.fileAssociations.push_back(assoc);

    auto content = generateDesktopEntry(params);
    EXPECT_CONTAINS(content, "Exec=/usr/bin/editor %f");
}

TEST(DesktopEntry, CategoryField) {
    DesktopEntryParams params;
    params.name = "Game";
    params.execPath = "/usr/bin/game";
    params.iconName = "game";
    params.categories = "Game;ArcadeGame;";

    auto content = generateDesktopEntry(params);
    EXPECT_CONTAINS(content, "Categories=Game;ArcadeGame;");
}

TEST(DesktopEntry, NormalizesSingleCategoryWithTerminatingSemicolon) {
    DesktopEntryParams params;
    params.name = "Game";
    params.execPath = "/usr/bin/game";
    params.iconName = "game";
    params.categories = "Game";

    auto content = generateDesktopEntry(params);
    EXPECT_CONTAINS(content, "Categories=Game;");
}

TEST(DesktopEntry, RejectsUnknownCategory) {
    DesktopEntryParams params;
    params.name = "Mystery";
    params.execPath = "/usr/bin/mystery";
    params.iconName = "mystery";
    params.categories = "DefinitelyNotARegisteredCategory";
    EXPECT_THROWS(generateDesktopEntry(params), std::runtime_error);
}

TEST(DesktopEntry, RejectsInvalidCategoryInjection) {
    DesktopEntryParams params;
    params.name = "Game";
    params.execPath = "/usr/bin/game";
    params.iconName = "game";
    params.categories = "Game;\nExec=bad";
    EXPECT_THROWS(generateDesktopEntry(params), std::runtime_error);
}

TEST(DesktopEntry, RejectsLineBreakInjection) {
    DesktopEntryParams params;
    params.name = "Bad\nName";
    params.execPath = "/usr/bin/bad";
    params.iconName = "bad";
    EXPECT_THROWS(generateDesktopEntry(params), std::runtime_error);
}

TEST(DesktopEntry, SupportsHiddenMimeHandlerEntry) {
    DesktopEntryParams params;
    params.name = "Editor";
    params.execPath = "/usr/bin/editor";
    params.iconName = "editor";
    params.noDisplay = true;
    params.fileAssociations.push_back({".zia", "Zia Source", "text/x-zia", ""});

    auto content = generateDesktopEntry(params);
    EXPECT_CONTAINS(content, "NoDisplay=true");
    EXPECT_CONTAINS(content, "MimeType=text/x-zia;");
    EXPECT_CONTAINS(content, "Exec=/usr/bin/editor %f");
}

// ============================================================================
// MIME Type XML Tests
// ============================================================================

TEST(MimeXml, ContainsGlobPattern) {
    std::vector<FileAssoc> assocs;
    assocs.push_back({".zia", "Zia Source File", "text/x-zia", ""});

    auto xml = generateMimeTypeXml("mypackage", assocs);
    EXPECT_CONTAINS(xml, "text/x-zia");
    EXPECT_CONTAINS(xml, "*.zia");
}

TEST(MimeXml, EscapesXmlFields) {
    std::vector<FileAssoc> assocs;
    assocs.push_back({".vp", "A & B <C>", "application/x-zanna", ""});

    auto xml = generateMimeTypeXml("mypackage", assocs);
    EXPECT_CONTAINS(xml, "A &amp; B &lt;C&gt;");
}

// ============================================================================
// Verification Tests
// ============================================================================

TEST(Verify, ZipValid) {
    ZipWriter zip;
    zip.addFileString("hello.txt", "Hello");
    auto data = zip.finishToVector();

    std::ostringstream err;
    EXPECT_TRUE(verifyZip(data, err));
}

TEST(Verify, ZipInvalidTooSmall) {
    std::vector<uint8_t> data = {0x50, 0x4B}; // Just "PK" — too small
    std::ostringstream err;
    EXPECT_FALSE(verifyZip(data, err));
    EXPECT_CONTAINS(err.str(), "too small");
}

TEST(Verify, ZipRejectsZip64Sentinel) {
    std::vector<uint8_t> data(22, 0);
    data[0] = 0x50;
    data[1] = 0x4B;
    data[2] = 0x05;
    data[3] = 0x06;
    data[8] = 0xFF;
    data[9] = 0xFF;
    data[10] = 0xFF;
    data[11] = 0xFF;
    data[12] = 0xFF;
    data[13] = 0xFF;
    data[14] = 0xFF;
    data[15] = 0xFF;
    data[16] = 0xFF;
    data[17] = 0xFF;
    data[18] = 0xFF;
    data[19] = 0xFF;

    std::ostringstream err;
    EXPECT_FALSE(verifyZip(data, err));
    EXPECT_CONTAINS(err.str(), "ZIP64");
}

TEST(Verify, ZipRejectsTraversalEntry) {
    ZipWriter zip;
    zip.addFileString("safe.txt", "Hello");
    auto data = zip.finishToVector();

    const std::string from = "safe.txt";
    const std::string to = "../x.txt";
    ASSERT_EQ(from.size(), to.size());
    for (size_t i = 0; i + from.size() <= data.size(); ++i) {
        if (std::memcmp(data.data() + i, from.data(), from.size()) == 0)
            std::memcpy(data.data() + i, to.data(), to.size());
    }

    std::ostringstream err;
    EXPECT_FALSE(verifyZip(data, err));
    EXPECT_CONTAINS(err.str(), "unsafe entry path");
}

TEST(Verify, ZipRejectsCrcMismatch) {
    ZipWriter zip;
    zip.addFileString("hello.txt", "Hello");
    auto data = zip.finishToVector();

    const uint16_t nameLen = readLE16(data.data() + 26);
    const size_t dataOff = 30 + nameLen;
    ASSERT_LT(dataOff, data.size());
    data[dataOff] ^= 0xFF;

    std::ostringstream err;
    EXPECT_FALSE(verifyZip(data, err));
    EXPECT_CONTAINS(err.str(), "CRC");
}

TEST(Verify, ZipRejectsDuplicateNormalizedEntryPath) {
    ZipWriter zip;
    zip.addDirectory("foo/");
    zip.addFileString("bar", "x");
    auto data = zip.finishToVector();

    const std::string from = "bar";
    const std::string to = "foo";
    for (size_t i = 0; i + from.size() <= data.size(); ++i) {
        if (std::memcmp(data.data() + i, from.data(), from.size()) == 0)
            std::memcpy(data.data() + i, to.data(), to.size());
    }

    std::ostringstream err;
    EXPECT_FALSE(verifyZip(data, err));
    EXPECT_CONTAINS(err.str(), "duplicate normalized entry");
}

TEST(Verify, ZipRejectsLocalHeaderSizeMismatch) {
    ZipWriter zip;
    zip.addFileString("hello.txt", "Hello");
    auto data = zip.finishToVector();

    ASSERT_GE(data.size(), static_cast<size_t>(26));
    data[22] = 6; // local uncompressed size, central directory still says 5
    data[23] = 0;
    data[24] = 0;
    data[25] = 0;

    std::ostringstream err;
    EXPECT_FALSE(verifyZip(data, err));
    EXPECT_CONTAINS(err.str(), "local sizes");
}

TEST(Verify, MacOSAppZipRequiresBundlePayload) {
    ZipWriter zip;
    zip.addFileString("App.app/Contents/Info.plist", "<plist/>");
    zip.addFileString("App.app/Contents/PkgInfo", "APPL????");
    zip.addFileString("App.app/Contents/MacOS/app", "bin");
    auto data = zip.finishToVector();

    std::ostringstream err;
    EXPECT_TRUE(verifyMacOSAppZip(data, "App.app", "app", err));
    std::ostringstream missingErr;
    EXPECT_FALSE(verifyMacOSAppZip(data, "App.app", "missing", missingErr));
}

TEST(Verify, RpmRejectsLeadWithoutHeadersAndPayload) {
    std::vector<uint8_t> rpm(112, 0);
    rpm[0] = 0xED;
    rpm[1] = 0xAB;
    rpm[2] = 0xEE;
    rpm[3] = 0xDB;
    rpm[4] = 3;
    rpm[78] = 0;
    rpm[79] = 5;
    std::ostringstream err;
    EXPECT_FALSE(verifyRpm(rpm, err));
    EXPECT_CONTAINS(err.str(), "signature header magic");
}

TEST(Verify, MacOSDmgRecognizesUdifTrailer) {
    std::vector<uint8_t> data(512, 0);
    data[0] = 'k';
    data[1] = 'o';
    data[2] = 'l';
    data[3] = 'y';
    std::ostringstream err;
    EXPECT_TRUE(verifyMacOSDmg(data, err));

    data[0] = 'b';
    std::ostringstream badErr;
    EXPECT_FALSE(verifyMacOSDmg(data, badErr));
    EXPECT_CONTAINS(badErr.str(), "UDIF");
}

TEST(Verify, DebValid) {
    ArWriter ar;
    ar.addMemberString("debian-binary", "2.0\n");
    ar.addMemberVec("control.tar.gz", makeTarGz({{"./control", "Package: test\n"}}));
    ar.addMemberVec("data.tar.gz", makeTarGz({{"./usr/bin/test", "data"}}));
    auto data = ar.finish();

    std::ostringstream err;
    const bool ok = verifyDeb(data, err);
    if (!ok)
        std::cerr << err.str();
    EXPECT_TRUE(ok);
}

TEST(Verify, DebPayloadRequiresExpectedPath) {
    ArWriter ar;
    ar.addMemberString("debian-binary", "2.0\n");
    ar.addMemberVec("control.tar.gz", makeTarGz({{"./control", "Package: test\n"}}));
    ar.addMemberVec("data.tar.gz", makeTarGz({{"./usr/bin/test", "data"}}));
    auto data = ar.finish();

    std::ostringstream err;
    EXPECT_TRUE(verifyDebPayload(data, {"usr/bin/test"}, err));
    std::ostringstream missingErr;
    EXPECT_FALSE(verifyDebPayload(data, {"usr/bin/missing"}, missingErr));
    EXPECT_CONTAINS(missingErr.str(), "missing required payload path");
}

TEST(Verify, DebRejectsMisorderedMembers) {
    ArWriter ar;
    ar.addMemberString("debian-binary", "2.0\n");
    ar.addMemberVec("data.tar.gz", makeTarGz({{"./usr/bin/test", "data"}}));
    ar.addMemberVec("control.tar.gz", makeTarGz({{"./control", "Package: test\n"}}));
    auto data = ar.finish();

    std::ostringstream err;
    EXPECT_FALSE(verifyDeb(data, err));
    EXPECT_CONTAINS(err.str(), "expected 'control.tar.gz'");
}

TEST(Verify, DebRejectsExtraMembers) {
    ArWriter ar;
    ar.addMemberString("debian-binary", "2.0\n");
    ar.addMemberVec("control.tar.gz", makeTarGz({{"./control", "Package: test\n"}}));
    ar.addMemberVec("data.tar.gz", makeTarGz({{"./usr/bin/test", "data"}}));
    ar.addMemberString("extra", "unexpected");
    auto data = ar.finish();

    std::ostringstream err;
    EXPECT_FALSE(verifyDeb(data, err));
    EXPECT_CONTAINS(err.str(), "unexpected extra ar member");
}

TEST(Verify, DebRejectsUngzippedMembers) {
    ArWriter ar;
    ar.addMemberString("debian-binary", "2.0\n");
    ar.addMemberString("control.tar.gz", "ctrl");
    ar.addMemberString("data.tar.gz", "data");
    auto data = ar.finish();

    std::ostringstream err;
    EXPECT_FALSE(verifyDeb(data, err));
    EXPECT_CONTAINS(err.str(), "compressed tar");
}

TEST(Verify, TarGzValid) {
    auto tarGz = makeTarGz({{"./hello.txt", "hello"}});
    std::ostringstream err;
    EXPECT_TRUE(verifyTarGz(tarGz, err));
}

TEST(Verify, TarGzPayloadRequiresExpectedPath) {
    auto tarGz = makeTarGz({{"./hello.txt", "hello"}});
    std::ostringstream err;
    EXPECT_TRUE(verifyTarGzPayload(tarGz, {"hello.txt"}, err));
    std::ostringstream missingErr;
    EXPECT_FALSE(verifyTarGzPayload(tarGz, {"missing.txt"}, missingErr));
}

TEST(Verify, DebMissingMagic) {
    std::vector<uint8_t> data = {0, 0, 0, 0, 0, 0, 0, 0};
    std::ostringstream err;
    EXPECT_FALSE(verifyDeb(data, err));
    EXPECT_CONTAINS(err.str(), "ar magic");
}

TEST(Verify, PEValid) {
    PEBuildParams params;
    params.textSection = {0xC3};
    auto pe = buildPE(params);

    std::ostringstream err;
    EXPECT_TRUE(verifyPE(pe, err));
}

TEST(Verify, PEValidatorReportsValidatedImageMetadata) {
    PEBuildParams params;
    params.textSection = {0xC3};
    auto pe = buildPE(params);

    WindowsPEImageInfo info;
    std::string error;
    EXPECT_TRUE(validateWindowsPEImage(pe.data(), pe.size(), 0x8664U, &info, error));
    EXPECT_EQ(info.machine, static_cast<uint16_t>(0x8664U));
    EXPECT_EQ(info.subsystem, static_cast<uint16_t>(2U));
    EXPECT_NE(info.entryPoint, static_cast<uint32_t>(0U));
    EXPECT_NE(info.sizeOfImage, static_cast<uint32_t>(0U));
    EXPECT_NE(info.sizeOfHeaders, static_cast<uint32_t>(0U));
    EXPECT_TRUE(error.empty());
}

TEST(Verify, PEValidatorRejectsWrongRequiredArchitecture) {
    PEBuildParams params;
    params.textSection = {0xC3};
    auto pe = buildPE(params);

    std::string error;
    EXPECT_FALSE(validateWindowsPEImage(pe.data(), pe.size(), 0xAA64U, nullptr, error));
    EXPECT_CONTAINS(error, "required architecture");
}

TEST(Verify, PEValidatorRejectsInvalidImageAndOptionalHeaders) {
    PEBuildParams params;
    params.textSection = {0xC3};
    const auto valid = buildPE(params);
    const size_t peOffset = readLE32(valid.data() + 60U);
    const size_t coffOffset = peOffset + 4U;
    const size_t optionalOffset = coffOffset + 20U;
    std::string error;

    auto mutated = valid;
    writeLE16(mutated.data() + coffOffset + 2U, 0U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "section count");

    mutated = valid;
    writeLE16(mutated.data() + coffOffset + 18U, 0U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "non-DLL executable");

    mutated = valid;
    writeLE16(mutated.data() + coffOffset + 18U,
              static_cast<uint16_t>(readLE16(mutated.data() + coffOffset + 18U) | 0x2000U));
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "non-DLL executable");

    mutated = valid;
    writeLE16(mutated.data() + coffOffset + 16U, 111U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "optional header");

    mutated = valid;
    writeLE16(mutated.data() + optionalOffset, 0x010BU);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "PE32+");

    mutated = valid;
    writeLE32(mutated.data() + optionalOffset + 16U, 0U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "entry point");

    mutated = valid;
    writeLE16(mutated.data() + optionalOffset + 68U, 1U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "subsystem");
}

TEST(Verify, PEValidatorRejectsInvalidLoaderAlignment) {
    PEBuildParams params;
    params.textSection = {0xC3};
    const auto valid = buildPE(params);
    const size_t optionalOffset = readLE32(valid.data() + 60U) + 24U;
    std::string error;

    auto mutated = valid;
    writeLE32(mutated.data() + optionalOffset + 32U, 256U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "alignment relationship");

    mutated = valid;
    writeLE32(mutated.data() + optionalOffset + 36U, 768U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "alignment relationship");

    mutated = valid;
    writeLE32(mutated.data() + optionalOffset + 56U,
              readLE32(mutated.data() + optionalOffset + 56U) - 1U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "SizeOfImage");

    mutated = valid;
    writeLE32(mutated.data() + optionalOffset + 60U,
              readLE32(mutated.data() + optionalOffset + 60U) - 1U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "SizeOfHeaders");

    mutated = valid;
    writeLE32(mutated.data() + optionalOffset + 108U, 17U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "data-directory count");
}

TEST(Verify, PEValidatorRejectsInvalidSectionAndEntryPointRanges) {
    PEBuildParams params;
    params.textSection = {0xC3};
    const auto valid = buildPE(params);
    const size_t optionalOffset = readLE32(valid.data() + 60U) + 24U;
    const size_t sectionOffset =
        optionalOffset + readLE16(valid.data() + readLE32(valid.data() + 60U) + 20U);
    std::string error;

    auto mutated = valid;
    writeLE32(mutated.data() + sectionOffset + 12U,
              readLE32(mutated.data() + sectionOffset + 12U) + 1U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "virtual address");

    mutated = valid;
    writeLE32(mutated.data() + sectionOffset + 8U, readLE32(mutated.data() + optionalOffset + 56U));
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "virtual range");

    mutated = valid;
    writeLE32(mutated.data() + sectionOffset + 20U,
              readLE32(mutated.data() + sectionOffset + 20U) + 1U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "file-aligned");

    mutated = valid;
    writeLE32(mutated.data() + sectionOffset + 16U,
              readLE32(mutated.data() + sectionOffset + 16U) - 1U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "file-aligned");

    mutated = valid;
    writeLE32(mutated.data() + optionalOffset + 16U,
              readLE32(mutated.data() + optionalOffset + 56U));
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "outside every executable section");

    mutated = valid;
    writeLE32(mutated.data() + sectionOffset + 36U,
              readLE32(mutated.data() + sectionOffset + 36U) & ~0x20000000U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "non-executable section");
}

TEST(Verify, PEValidatorRejectsOverlappingSections) {
    PEBuildParams params;
    params.textSection = {0xC3};
    params.rdataSection = {'Z', 'A', 'N', 'N', 'A'};
    const auto valid = buildPE(params);
    const size_t peOffset = readLE32(valid.data() + 60U);
    const size_t optionalOffset = peOffset + 24U;
    const size_t sectionOffset = optionalOffset + readLE16(valid.data() + peOffset + 20U);
    const size_t secondSectionOffset = sectionOffset + 40U;
    ASSERT_LT(secondSectionOffset + 40U, valid.size());
    std::string error;

    auto mutated = valid;
    const uint32_t firstVirtualAddress = readLE32(mutated.data() + sectionOffset + 12U);
    const uint32_t secondVirtualAddress = readLE32(mutated.data() + secondSectionOffset + 12U);
    writeLE32(mutated.data() + sectionOffset + 8U, secondVirtualAddress - firstVirtualAddress + 1U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "overlapping virtual");

    mutated = valid;
    writeLE32(mutated.data() + secondSectionOffset + 20U,
              readLE32(mutated.data() + sectionOffset + 20U));
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "overlapping raw");
}

TEST(Verify, PEValidatorRejectsInvalidDataDirectories) {
    PEBuildParams params;
    params.textSection = {0xC3};
    const auto valid = buildPE(params);
    const size_t optionalOffset = readLE32(valid.data() + 60U) + 24U;
    const size_t firstDirectoryOffset = optionalOffset + 112U;
    const size_t securityDirectoryOffset = firstDirectoryOffset + 4U * 8U;
    std::string error;

    auto mutated = valid;
    writeLE32(mutated.data() + firstDirectoryOffset,
              readLE32(mutated.data() + optionalOffset + 16U));
    writeLE32(mutated.data() + firstDirectoryOffset + 4U, 0U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "partial zero");

    mutated = valid;
    writeLE32(mutated.data() + firstDirectoryOffset,
              readLE32(mutated.data() + optionalOffset + 56U));
    writeLE32(mutated.data() + firstDirectoryOffset + 4U, 8U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "outside mapped");

    mutated = valid;
    writeLE32(mutated.data() + securityDirectoryOffset, 1U);
    writeLE32(mutated.data() + securityDirectoryOffset + 4U, 8U);
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "certificate table");

    mutated = valid;
    const size_t sectionOffset =
        optionalOffset + readLE16(valid.data() + readLE32(valid.data() + 60U) + 20U);
    writeLE32(mutated.data() + securityDirectoryOffset,
              readLE32(mutated.data() + sectionOffset + 20U));
    writeLE32(mutated.data() + securityDirectoryOffset + 4U,
              readLE32(mutated.data() + sectionOffset + 16U));
    EXPECT_FALSE(validateWindowsPEImage(mutated.data(), mutated.size(), 0U, nullptr, error));
    EXPECT_CONTAINS(error, "overlaps PE section");
}

TEST(Verify, PEInvalidMagic) {
    std::vector<uint8_t> data(200, 0);
    data[0] = 'X'; // Not "MZ"
    std::ostringstream err;
    EXPECT_FALSE(verifyPE(data, err));
    EXPECT_CONTAINS(err.str(), "MZ");
}

TEST(Verify, PERejectsOverflowingELfanew) {
    std::vector<uint8_t> data(128, 0);
    data[0] = 'M';
    data[1] = 'Z';
    data[60] = 0xFF;
    data[61] = 0xFF;
    data[62] = 0xFF;
    data[63] = 0xFF;

    std::ostringstream err;
    EXPECT_FALSE(verifyPE(data, err));
    EXPECT_CONTAINS(err.str(), "points past end");
}

TEST(Verify, PEZipOverlayValid) {
    ZipWriter zip;
    zip.addFileString("hello.txt", "Hello");

    PEBuildParams params;
    params.textSection = {0xC3};
    params.overlay = zip.finishToVector();
    auto pe = buildPE(params);

    std::ostringstream err;
    EXPECT_TRUE(verifyPEZipOverlay(pe, err));
}

TEST(Verify, PEZipOverlayIgnoresAuthenticodeCertificateTable) {
    std::vector<uint8_t> pe;
    for (size_t padding = 0; padding < 8U; ++padding) {
        ZipWriter zip;
        zip.addFileString("hello.txt", "Hello");
        zip.addFileString("alignment-padding.bin", std::string(padding, '\0'));
        PEBuildParams params;
        params.textSection = {0xC3};
        params.overlay = zip.finishToVector();
        pe = buildPE(params);
        if ((pe.size() & 7U) == 0U)
            break;
    }
    ASSERT_EQ(pe.size() & 7U, static_cast<size_t>(0U));

    const uint32_t certOff = static_cast<uint32_t>(pe.size());
    const uint32_t certSize = 8;
    pe.push_back(8);
    pe.push_back(0);
    pe.push_back(0);
    pe.push_back(0);
    pe.push_back(0);
    pe.push_back(2);
    pe.push_back(2);
    pe.push_back(0);

    const size_t peOff = readLE32(pe.data() + 60);
    const size_t certDirOff = peOff + 4 + 20 + 112 + 4 * 8;
    ASSERT_LT(certDirOff + 8, pe.size());
    writeLE32(pe.data() + certDirOff, certOff);
    writeLE32(pe.data() + certDirOff + 4, certSize);

    std::ostringstream err;
    EXPECT_TRUE(verifyPEZipOverlay(pe, err));
}

TEST(Verify, PEZipOverlayPayloadRequiresExpectedEntry) {
    ZipWriter zip;
    zip.addFileString("app/app.exe", "MZ");
    zip.addFileString("app/uninstall.exe", "MZ");

    PEBuildParams params;
    params.textSection = {0xC3};
    params.overlay = zip.finishToVector();
    auto pe = buildPE(params);

    std::ostringstream err;
    EXPECT_TRUE(verifyPEZipOverlayPayload(pe, {"app/app.exe", "app/uninstall.exe"}, err));
    std::ostringstream missingErr;
    EXPECT_FALSE(verifyPEZipOverlayPayload(pe, {"app/missing.exe"}, missingErr));
}

TEST(Verify, ZipSha256ManifestAllowsDirectoryEntries) {
    ZipWriter zip;
    zip.addDirectory("app/");
    zip.addFileString("app/app.exe", "Hello");
    zip.addFileString(
        "meta/manifest.sha256",
        "185f8db32271fe25f561a6fc938b2e264306ec304eda518007d1764826381969  app/app.exe\n");

    std::ostringstream err;
    EXPECT_TRUE(verifyZip(zip.finishToVector(), err));
}

TEST(Verify, ZipSha256ManifestRejectsUncoveredFiles) {
    ZipWriter zip;
    zip.addFileString("covered.txt", "Hello");
    zip.addFileString("orphan.txt", "not listed");
    zip.addFileString(
        "meta/manifest.sha256",
        "185f8db32271fe25f561a6fc938b2e264306ec304eda518007d1764826381969  covered.txt\n");

    std::ostringstream err;
    EXPECT_FALSE(verifyZip(zip.finishToVector(), err));
    EXPECT_CONTAINS(err.str(), "does not cover entry");
}

TEST(Verify, ZipSha256ManifestRejectsDuplicateEntries) {
    ZipWriter zip;
    zip.addFileString("covered.txt", "Hello");
    zip.addFileString(
        "meta/manifest.sha256",
        "185f8db32271fe25f561a6fc938b2e264306ec304eda518007d1764826381969  covered.txt\n"
        "185f8db32271fe25f561a6fc938b2e264306ec304eda518007d1764826381969  covered.txt\n");

    std::ostringstream err;
    EXPECT_FALSE(verifyZip(zip.finishToVector(), err));
    EXPECT_CONTAINS(err.str(), "duplicate entry");
}

TEST(Verify, PEZipOverlayMissing) {
    PEBuildParams params;
    params.textSection = {0xC3};
    auto pe = buildPE(params);

    std::ostringstream err;
    EXPECT_FALSE(verifyPEZipOverlay(pe, err));
    EXPECT_CONTAINS(err.str(), "overlay");
}

TEST(PE, ImportsAndCustomRdataCoexist) {
    PEBuildParams params;
    params.textSection = {0xC3};
    params.imports = {{"kernel32.dll", {"ExitProcess"}}};
    params.rdataSection = {'Z', 'A', 'N', 'N', 'A'};
    auto pe = buildPE(params);

    std::ostringstream err;
    EXPECT_TRUE(verifyPE(pe, err));

    bool foundMarker = false;
    for (size_t i = 0; i + 5 <= pe.size(); ++i) {
        if (std::memcmp(pe.data() + i, "ZANNA", 5) == 0) {
            foundMarker = true;
            break;
        }
    }
    EXPECT_TRUE(foundMarker);
}

TEST(PE, EmbedsVersionInfoResource) {
    PEBuildParams params;
    params.textSection = {0xC3};
    params.versionInfo.enabled = true;
    params.versionInfo.fileVersion = {1, 2, 3, 4};
    params.versionInfo.productVersion = {1, 2, 3, 4};
    params.versionInfo.companyName = "Zanna Project";
    params.versionInfo.fileDescription = "Versioned Test Stub";
    params.versionInfo.fileVersionText = "1.2.3.4";
    params.versionInfo.internalName = "versioned.exe";
    params.versionInfo.originalFilename = "versioned.exe";
    params.versionInfo.productName = "Versioned Test";
    params.versionInfo.productVersionText = "1.2.3.4";
    auto pe = buildPE(params);

    std::ostringstream err;
    EXPECT_TRUE(verifyPE(pe, err));
    EXPECT_TRUE(containsUtf16LE(pe, "VS_VERSION_INFO"));
    EXPECT_TRUE(containsUtf16LE(pe, "FileDescription"));
    EXPECT_TRUE(containsUtf16LE(pe, "Versioned Test Stub"));
}

TEST(PackageUtils, SanitizesRelativePaths) {
    EXPECT_EQ(sanitizePackageRelativePath("themes\\\\dark"), std::string("themes/dark"));
    EXPECT_EQ(joinPackageRelativePath("usr/share/app", "themes/dark"),
              std::string("usr/share/app/themes/dark"));
}

TEST(PackageUtils, RejectsArchiveEscapes) {
    EXPECT_THROWS(sanitizePackageRelativePath("../escape"), std::runtime_error);
    EXPECT_THROWS(sanitizePackageRelativePath("/absolute"), std::runtime_error);
    EXPECT_THROWS(sanitizePackageRelativePath("C:/drive"), std::runtime_error);
    EXPECT_THROWS(sanitizePackageRelativePath(std::string("bad") + static_cast<char>(0x7F)),
                  std::runtime_error);
}

TEST(PackageUtils, RejectsInvalidPackageNames) {
    EXPECT_THROWS(normalizeExecName("bad/name"), std::runtime_error);
    EXPECT_THROWS(normalizeDebName("a"), std::runtime_error);
    EXPECT_THROWS(normalizeDebName("-bad"), std::runtime_error);
}

TEST(PackageUtils, RejectsInvalidFileAssociations) {
    EXPECT_NO_THROW(validateFileAssociation(".zia", "Zia Source", "text/x-zia"));
    EXPECT_THROWS(validateFileAssociation(".", "Bad", "text/plain"), std::runtime_error);
    EXPECT_THROWS(validateFileAssociation("zia", "Bad", "text/plain"), std::runtime_error);
    EXPECT_THROWS(validateFileAssociation(".bad", "Bad", "text/with space"), std::runtime_error);
    EXPECT_THROWS(validateFileAssociation("../bad", "Bad", "text/plain"), std::runtime_error);
}

TEST(PackageUtils, RejectsDuplicateFileAssociationExtensions) {
    std::vector<FileAssoc> assocs = {
        {".zia", "Zia Source", "text/x-zia", ""},
        {".ZIA", "Zia Source 2", "text/x-zia-2", ""},
    };
    EXPECT_THROWS(validatePackageFileAssociations(assocs), std::runtime_error);
}

TEST(PackageUtils, NormalizesWindowsSigningThumbprints) {
    EXPECT_EQ(normalizeWindowsCertificateThumbprint(
                  "ABCD EFFE 0011 2233 4455 6677 8899 AABB CCDD EEFF", "thumbprint"),
              std::string("abcdeffe00112233445566778899aabbccddeeff"));
    EXPECT_THROWS(validateWindowsCertificateThumbprint("1234", "thumbprint"), std::runtime_error);
    EXPECT_THROWS(validateWindowsCertificateThumbprint("abcdeffe00112233445566778899aabbccddeefg",
                                                       "thumbprint"),
                  std::runtime_error);
}

TEST(PackageUtils, ValidatesMacOSSigningSemantics) {
    PackageConfig pkg;
    pkg.macosSignMode = "developer-id";
    EXPECT_THROWS(validateMacOSSigningConfig(pkg), std::runtime_error);
    pkg.macosSignIdentity = "Developer ID Application: Example";
    EXPECT_NO_THROW(validateMacOSSigningConfig(pkg));
    pkg.macosNotaryProfile = "notary-profile";
    EXPECT_NO_THROW(validateMacOSSigningConfig(pkg));
    pkg.macosStaple = true;
    EXPECT_NO_THROW(validateMacOSSigningConfig(pkg));

    pkg.macosSignMode = "adhoc";
    EXPECT_THROWS(validateMacOSSigningConfig(pkg), std::runtime_error);
    pkg.macosNotaryProfile.clear();
    EXPECT_THROWS(validateMacOSSigningConfig(pkg), std::runtime_error);
}

#if !defined(_WIN32)
TEST(PackageUtils, SafeDirectoryIterateFollowsInternalSymlinkDirectories) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_safe_iter_symlink_dir";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot / "project" / "assets");
    fs::create_directories(tmpRoot / "project" / "shared");
    {
        std::ofstream out(tmpRoot / "project" / "shared" / "data.txt");
        out << "data";
    }
    fs::create_directory_symlink("../shared", tmpRoot / "project" / "assets" / "linked");

    bool sawLinkedFile = false;
    safeDirectoryIterate(
        tmpRoot / "project" / "assets", tmpRoot / "project", [&](const fs::directory_entry &entry) {
            if (fs::is_regular_file(entry.path()) &&
                entry.path().lexically_relative(tmpRoot / "project" / "assets").generic_string() ==
                    "linked/data.txt") {
                sawLinkedFile = true;
            }
        });
    EXPECT_TRUE(sawLinkedFile);
    fs::remove_all(tmpRoot);
}

TEST(PackageUtils, SafeDirectoryIterateResolvedReportsValidatedReadPath) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_safe_iter_resolved_path";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot / "project" / "assets");
    fs::create_directories(tmpRoot / "project" / "shared");
    {
        std::ofstream out(tmpRoot / "project" / "shared" / "data.txt");
        out << "data";
    }
    fs::create_symlink("../shared/data.txt", tmpRoot / "project" / "assets" / "link.txt");

    bool sawResolvedSymlink = false;
    safeDirectoryIterateResolved(
        tmpRoot / "project" / "assets", tmpRoot / "project", [&](const SafeDirectoryEntry &entry) {
            if (entry.logicalPath.filename() == "link.txt") {
                sawResolvedSymlink =
                    entry.symlink && entry.regularFile &&
                    fs::equivalent(entry.resolvedPath, tmpRoot / "project" / "shared" / "data.txt");
            }
        });
    EXPECT_TRUE(sawResolvedSymlink);
    fs::remove_all(tmpRoot);
}

TEST(PackageUtils, SafeDirectoryIterateRejectsEscapingSymlink) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_safe_iter_escape_symlink";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot / "project" / "assets");
    {
        std::ofstream out(tmpRoot / "outside.txt");
        out << "outside";
    }
    fs::create_symlink(tmpRoot / "outside.txt", tmpRoot / "project" / "assets" / "escape.txt");

    EXPECT_THROWS(safeDirectoryIterateResolved(tmpRoot / "project" / "assets",
                                               tmpRoot / "project",
                                               [](const SafeDirectoryEntry &) {}),
                  std::runtime_error);
    fs::remove_all(tmpRoot);
}
#endif

TEST(PackageUtils, ValidatesPackageUrls) {
    EXPECT_NO_THROW(validatePackageUrl("https://example.com/app", "homepage"));
    EXPECT_NO_THROW(validatePackageUrl("http://localhost:8080", "homepage"));
    EXPECT_THROWS(validatePackageUrl("https:///missing-host", "homepage"), std::runtime_error);
    EXPECT_THROWS(validatePackageUrl("https://bad host.example", "homepage"), std::runtime_error);
    EXPECT_THROWS(validatePackageUrl("1https://example.com", "homepage"), std::runtime_error);
    EXPECT_THROWS(validatePackageUrl("https://example.com:", "homepage"), std::runtime_error);
    EXPECT_NO_THROW(validateHttpsPackageUrl("https://timestamp.example.com", "timestamp URL"));
    EXPECT_THROWS(validateHttpsPackageUrl("http://timestamp.example.com", "timestamp URL"),
                  std::runtime_error);
}

TEST(PackageUtils, ValidatesToolchainArchitecture) {
    EXPECT_NO_THROW(validateToolchainArchitecture("x64"));
    EXPECT_NO_THROW(validateToolchainArchitecture("arm64"));
    EXPECT_THROWS(validateToolchainArchitecture("amd64"), std::runtime_error);
    EXPECT_THROWS(validateToolchainArchitecture(""), std::runtime_error);
}

TEST(PackageUtils, ValidatesToolchainPlatform) {
    EXPECT_NO_THROW(validateToolchainPlatform("windows"));
    EXPECT_NO_THROW(validateToolchainPlatform("macos"));
    EXPECT_NO_THROW(validateToolchainPlatform("linux"));
    EXPECT_THROWS(validateToolchainPlatform("darwin"), std::runtime_error);
    EXPECT_THROWS(validateToolchainPlatform(""), std::runtime_error);
}

TEST(PackageUtils, ValidatesDebDependencyGrammar) {
    EXPECT_NO_THROW(validateDebDependency("libc6 (>= 2.34)"));
    EXPECT_NO_THROW(validateDebDependency("libstdc++6 | libc++1"));
    EXPECT_NO_THROW(validateDebDependency("python3:any"));
    EXPECT_THROWS(validateDebDependency("BadPackage"), std::runtime_error);
    EXPECT_THROWS(validateDebDependency("libc6 (> 2.34)"), std::runtime_error);
    EXPECT_THROWS(validateDebDependency("libc6 | "), std::runtime_error);
}

TEST(PackageUtils, ValidatesDebianVersionGrammar) {
    EXPECT_NO_THROW(validateDebVersion("1.2.3-1", "version"));
    EXPECT_NO_THROW(validateDebVersion("2:1.0~beta+1", "version"));
    EXPECT_THROWS(validateDebVersion("-1.0", "version"), std::runtime_error);
    EXPECT_THROWS(validateDebVersion("1:2:3", "version"), std::runtime_error);
    EXPECT_THROWS(validateDebVersion("1.0-", "version"), std::runtime_error);
}

TEST(PackageUtils, ParsesSourceDateEpochStrictly) {
    EXPECT_EQ(UINT64_C(0), parseSourceDateEpoch("0"));
    EXPECT_EQ(UINT64_C(1700000000), parseSourceDateEpoch("1700000000"));
    EXPECT_EQ(static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
              parseSourceDateEpoch("9223372036854775807"));
    EXPECT_THROWS(parseSourceDateEpoch(""), std::runtime_error);
    EXPECT_THROWS(parseSourceDateEpoch("-1"), std::runtime_error);
    EXPECT_THROWS(parseSourceDateEpoch("1x"), std::runtime_error);
    EXPECT_THROWS(parseSourceDateEpoch("9223372036854775808"), std::runtime_error);
    EXPECT_THROWS(parseSourceDateEpoch("184467440737095516160"), std::runtime_error);
}

TEST(PackageUtils, ValidatesTargetSpecificIdentifiers) {
    EXPECT_NO_THROW(validateMacOSBundleIdentifier("org.zanna.test-app"));
    EXPECT_THROWS(validateMacOSBundleIdentifier("org.zanna.test_app"), std::runtime_error);
    EXPECT_THROWS(validateMacOSBundleIdentifier("org.-zanna.test"), std::runtime_error);
    EXPECT_NO_THROW(validateWindowsProgIdBase("zanna.test_app"));
    EXPECT_THROWS(validateWindowsProgIdBase("zanna."), std::runtime_error);
}

TEST(PackageUtils, ValidatesDottedVersionComponentCountAndRange) {
    EXPECT_NO_THROW(validateDottedNumericVersion("10.0", "version"));
    EXPECT_NO_THROW(validateDottedNumericVersion("1.2.3.4", "version"));
    EXPECT_THROWS(validateDottedNumericVersion("1", "version"), std::runtime_error);
    EXPECT_THROWS(validateDottedNumericVersion("1.2.3.4.5", "version"), std::runtime_error);
    EXPECT_THROWS(validateDottedNumericVersion("1.70000", "version"), std::runtime_error);
}

TEST(PackageUtils, RejectsSourcePathEscape) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_pkg_source_escape_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot / "project");

    EXPECT_THROWS(resolvePackageSourcePath(tmpRoot / "project", "../outside.txt", "asset"),
                  std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(PackageConfig, DetectsNonDefaultPackageFields) {
    PackageConfig pkg;
    EXPECT_FALSE(pkg.hasPackageConfig());
    pkg.license = "GPL-3.0-only";
    EXPECT_TRUE(pkg.hasPackageConfig());

    PackageConfig pkg2;
    pkg2.targetArchitectures.push_back("arm64");
    EXPECT_TRUE(pkg2.hasPackageConfig());

    PackageConfig pkg3;
    pkg3.windowsSignThumbprint = "ABCDEFFE00112233445566778899AABBCCDDEEFF";
    EXPECT_TRUE(pkg3.hasPackageConfig());
}

// ============================================================================
// ZipReader Tests
// ============================================================================

TEST(ZipReader, RoundTripStoredEntries) {
    // Write a ZIP with stored entries, then read it back
    ZipWriter writer;
    writer.addFileString("hello.txt", "Hello, World!");
    writer.addFileString("dir/nested.txt", "Nested content");
    auto zipData = writer.finishToVector();

    ZipReader reader(zipData.data(), zipData.size());
    EXPECT_EQ(reader.entries().size(), static_cast<size_t>(2));

    auto *hello = reader.find("hello.txt");
    ASSERT_TRUE(hello != nullptr);
    auto helloData = reader.extract(*hello);
    EXPECT_EQ(helloData.size(), static_cast<size_t>(13));
    EXPECT_TRUE(std::memcmp(helloData.data(), "Hello, World!", 13) == 0);

    auto *nested = reader.find("dir/nested.txt");
    ASSERT_TRUE(nested != nullptr);
    auto nestedData = reader.extract(*nested);
    EXPECT_EQ(std::string(nestedData.begin(), nestedData.end()), std::string("Nested content"));
}

TEST(ZipReader, DeflateExtractionHonorsDeclaredOutputLimit) {
    ZipWriter writer;
    writer.addFileString("big.txt", std::string(4096, 'A'));
    auto zipData = writer.finishToVector();

    size_t cdOff = 0;
    for (size_t i = 0; i + 4 <= zipData.size(); ++i) {
        if (readLE32(zipData.data() + i) == 0x02014B50) {
            cdOff = i;
            break;
        }
    }
    ASSERT_TRUE(cdOff > 0);
    ASSERT_EQ(readLE16(zipData.data() + cdOff + 10), static_cast<uint16_t>(8));

    // Central-directory uncompressed size drives ZipReader's inflate cap.
    zipData[cdOff + 24] = 1;
    zipData[cdOff + 25] = 0;
    zipData[cdOff + 26] = 0;
    zipData[cdOff + 27] = 0;
    zipData[22] = 1;
    zipData[23] = 0;
    zipData[24] = 0;
    zipData[25] = 0;

    ZipReader reader(zipData.data(), zipData.size());
    const ZipEntry *entry = reader.find("big.txt");
    ASSERT_TRUE(entry != nullptr);
    EXPECT_THROWS(reader.extract(*entry), DeflateError);
}

TEST(ZipReader, FindMissingEntry) {
    ZipWriter writer;
    writer.addFileString("a.txt", "alpha");
    auto zipData = writer.finishToVector();

    ZipReader reader(zipData.data(), zipData.size());
    EXPECT_TRUE(reader.find("nonexistent.txt") == nullptr);
}

TEST(ZipReader, EmptyArchive) {
    ZipWriter writer;
    auto zipData = writer.finishToVector();

    ZipReader reader(zipData.data(), zipData.size());
    EXPECT_EQ(reader.entries().size(), static_cast<size_t>(0));
}

TEST(ZipReader, InvalidDataThrows) {
    std::vector<uint8_t> garbage = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    bool threw = false;
    try {
        ZipReader reader(garbage.data(), garbage.size());
    } catch (const ZipReadError &) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(ZipWriter, StoredModePreservesLocalDataLayout) {
    ZipWriter writer;
    writer.setCompressionEnabled(false);
    writer.addFileString("hello.txt", "Hello stored world");

    const auto &entries = writer.layoutEntries();
    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(entries[0].method, static_cast<uint16_t>(0));
    EXPECT_EQ(entries[0].name, std::string("hello.txt"));
    EXPECT_GT(entries[0].localDataOffset, entries[0].localHeaderOffset);
    EXPECT_EQ(entries[0].compressedSize, entries[0].uncompressedSize);
}

// ============================================================================
// InstallerStubGen Tests
// ============================================================================

TEST(StubGen, PushPopEncoding) {
    InstallerStubGen gen;
    gen.push(X64Reg::RBP); // 55
    gen.push(X64Reg::R12); // 41 54
    gen.pop(X64Reg::R12);  // 41 5C
    gen.pop(X64Reg::RBP);  // 5D
    gen.ret();             // C3

    auto code = gen.finishText(0x1000, 0x2000, {}, 0x3000);
    ASSERT_EQ(code.size(), static_cast<size_t>(7));
    EXPECT_EQ(code[0], static_cast<uint8_t>(0x55)); // push rbp
    EXPECT_EQ(code[1], static_cast<uint8_t>(0x41)); // REX.B
    EXPECT_EQ(code[2], static_cast<uint8_t>(0x54)); // push r12
    EXPECT_EQ(code[3], static_cast<uint8_t>(0x41)); // REX.B
    EXPECT_EQ(code[4], static_cast<uint8_t>(0x5C)); // pop r12
    EXPECT_EQ(code[5], static_cast<uint8_t>(0x5D)); // pop rbp
    EXPECT_EQ(code[6], static_cast<uint8_t>(0xC3)); // ret
}

TEST(StubGen, XorRegReg) {
    InstallerStubGen gen;
    gen.xorRegReg(X64Reg::RCX, X64Reg::RCX); // 48 31 C9
    gen.ret();

    auto code = gen.finishText(0x1000, 0x2000, {}, 0x3000);
    ASSERT_GE(code.size(), static_cast<size_t>(4));
    EXPECT_EQ(code[0], static_cast<uint8_t>(0x48)); // REX.W
    EXPECT_EQ(code[1], static_cast<uint8_t>(0x31)); // XOR
    EXPECT_EQ(code[2], static_cast<uint8_t>(0xC9)); // ModRM: rcx, rcx
}

TEST(StubGen, LabelJumpForward) {
    InstallerStubGen gen;
    auto lbl = gen.newLabel();
    gen.jmp(lbl); // E9 XX XX XX XX (5 bytes)
    gen.nop();    // 90 (1 byte) — this should be skipped
    gen.bindLabel(lbl);
    gen.ret(); // C3

    auto code = gen.finishText(0x1000, 0x2000, {}, 0x3000);
    // jmp should jump over the nop: rel32 = 1 (target at offset 6, fixup at offset 1, +4 = 5,
    // 6-5=1)
    EXPECT_EQ(code[0], static_cast<uint8_t>(0xE9));
    int32_t disp = static_cast<int32_t>(code[1]) | (static_cast<int32_t>(code[2]) << 8) |
                   (static_cast<int32_t>(code[3]) << 16) | (static_cast<int32_t>(code[4]) << 24);
    EXPECT_EQ(disp, 1); // skip 1 byte (the nop)
}

TEST(StubGen, EmbedStringW) {
    InstallerStubGen gen;
    uint32_t off = gen.embedStringW("Hi");
    gen.ret();

    EXPECT_EQ(off, static_cast<uint32_t>(0));
    const auto &data = gen.dataSection();
    // "Hi" in UTF-16LE = 'H' 00 'i' 00 00 00
    ASSERT_EQ(data.size(), static_cast<size_t>(6));
    EXPECT_EQ(data[0], 'H');
    EXPECT_EQ(data[1], 0);
    EXPECT_EQ(data[2], 'i');
    EXPECT_EQ(data[3], 0);
    EXPECT_EQ(data[4], 0); // NUL terminator
    EXPECT_EQ(data[5], 0);
}

TEST(StubGen, EmbedStringWAcceptsUtf8) {
    InstallerStubGen gen;
    uint32_t off = gen.embedStringW("\xE2\x82\xAC");
    EXPECT_EQ(off, static_cast<uint32_t>(0));
    const auto &data = gen.dataSection();
    ASSERT_EQ(data.size(), static_cast<size_t>(4));
    EXPECT_EQ(data[0], static_cast<uint8_t>(0xAC));
    EXPECT_EQ(data[1], static_cast<uint8_t>(0x20));
    EXPECT_EQ(data[2], 0);
    EXPECT_EQ(data[3], 0);
}

TEST(StubGen, EmitsIndexedWordStore) {
    InstallerStubGen gen;
    gen.movMemIndexImm16(X64Reg::RBP, X64Reg::RAX, 1, -16, 0);
    const auto code = gen.finishText(0x1000, 0x2000, {}, 0x3000);
    const std::vector<uint8_t> expected = {
        0x66, 0xC7, 0x84, 0x45, 0xF0, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
    EXPECT_EQ(code, expected);
}

TEST(StubGen, EmitsIndexedWordLoadAndLea) {
    InstallerStubGen gen;
    gen.movzxRegMemIndex16(X64Reg::R11, X64Reg::RBP, X64Reg::RAX, 0, -32);
    gen.leaRegMemIndex(X64Reg::RDX, X64Reg::RBP, X64Reg::R10, 0, -64);
    const auto code = gen.finishText(0x1000, 0x2000, {}, 0x3000);
    const std::vector<uint8_t> expected = {0x44,
                                           0x0F,
                                           0xB7,
                                           0x9C,
                                           0x05,
                                           0xE0,
                                           0xFF,
                                           0xFF,
                                           0xFF,
                                           0x4A,
                                           0x8D,
                                           0x94,
                                           0x15,
                                           0xC0,
                                           0xFF,
                                           0xFF,
                                           0xFF};
    EXPECT_EQ(code, expected);
}

TEST(StubGen, EmitsCodeLabelLeaAndRegisterCall) {
    InstallerStubGen gen;
    const auto callback = gen.newLabel();
    gen.leaCodeLabel(X64Reg::R9, callback);
    gen.callReg(X64Reg::R9);
    gen.ret();
    gen.bindLabel(callback);
    gen.ret();

    const auto code = gen.finishText(0x1000, 0x2000, {}, 0x3000);
    const std::vector<uint8_t> expected = {
        0x4C, 0x8D, 0x0D, 0x04, 0x00, 0x00, 0x00, 0x41, 0xFF, 0xD1, 0xC3, 0xC3};
    EXPECT_EQ(code, expected);
}

TEST(StubGen, EmitsDwordRegisterStore) {
    InstallerStubGen gen;
    gen.movMemReg32(X64Reg::RBP, -12, X64Reg::R8);

    const auto code = gen.finishText(0x1000, 0x2000, {}, 0x3000);
    const std::vector<uint8_t> expected = {0x44, 0x89, 0x85, 0xF4, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(code, expected);
}

TEST(StubGen, EmitsZeroExtendingDwordMemoryLoad) {
    InstallerStubGen gen;
    gen.movRegMem32(X64Reg::R10, X64Reg::RAX, 48);
    const auto code = gen.finishText(0x1000, 0x2000, {}, 0x3000);
    const std::vector<uint8_t> expected = {0x44, 0x8B, 0x90, 0x30, 0x00, 0x00, 0x00};
    EXPECT_EQ(code, expected);
}

TEST(StubGenA64, EmitsBasicInstructionsAndBranchFixup) {
    InstallerStubGenA64 gen;
    const auto done = gen.newLabel();
    gen.movRegImm32(A64Reg::X0, 7);
    gen.b(done);
    gen.nop();
    gen.bindLabel(done);
    gen.ret();

    const auto code = gen.finishText(0x1000, 0x2000, {}, 0x3000);
    const std::vector<uint8_t> expected = {0xE0,
                                           0x00,
                                           0x80,
                                           0xD2,
                                           0x02,
                                           0x00,
                                           0x00,
                                           0x14,
                                           0x1F,
                                           0x20,
                                           0x03,
                                           0xD5,
                                           0xC0,
                                           0x03,
                                           0x5F,
                                           0xD6};
    EXPECT_EQ(code, expected);
}

TEST(StubGenA64, ResolvesDataAndIATAddressFixups) {
    InstallerStubGenA64 gen;
    const uint32_t msgOff = gen.embedStringW("x");
    gen.leaData(A64Reg::X0, msgOff);
    gen.callIATSlot(0);
    gen.ret();

    const auto code = gen.finishText(0x1000, 0x3000, {{"kernel32.dll", {"ExitProcess"}}}, 0x4000);
    const std::vector<uint8_t> expected = {
        0x00, 0x00, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x91, 0x10, 0x00, 0x00, 0xD0, 0x10, 0x02,
        0x00, 0x91, 0x10, 0x02, 0x40, 0xF9, 0x00, 0x02, 0x3F, 0xD6, 0xC0, 0x03, 0x5F, 0xD6};
    EXPECT_EQ(code, expected);
}

// ============================================================================
// InstallerStub Integration Tests
// ============================================================================

TEST(WindowsPackageBuilder, ValidatesCompleteVersionBeforeExtractingResourceCore) {
    EXPECT_EQ(windowsVersionPartsForResource("1.2.3-rc.1+win-x64"),
              (std::array<uint16_t, 4>{1, 2, 3, 0}));
    EXPECT_EQ(windowsVersionPartsForResource("65535.4"), (std::array<uint16_t, 4>{65535, 4, 0, 0}));
    EXPECT_THROWS(windowsVersionPartsForResource("01.2.3"), std::runtime_error);
    EXPECT_THROWS(windowsVersionPartsForResource("1.2.3.4.5"), std::runtime_error);
    EXPECT_THROWS(windowsVersionPartsForResource("1.2.3-rc.01"), std::runtime_error);
    EXPECT_THROWS(windowsVersionPartsForResource("1.2.3-alpha..1"), std::runtime_error);
    EXPECT_THROWS(windowsVersionPartsForResource("1.2.3+"), std::runtime_error);
    EXPECT_THROWS(windowsVersionPartsForResource("1.2.3+meta+extra"), std::runtime_error);
    EXPECT_THROWS(windowsVersionPartsForResource("1.2.3-\xc3\xa4"), std::runtime_error);
    EXPECT_THROWS(windowsVersionPartsForResource("1.65536.0"), std::runtime_error);
}

TEST(InstallerStub, GeneratesValidPE) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.version = "1.0.0";
    layout.executableName = "testapp.exe";
    layout.publisher = "Zanna";
    layout.identifier = "org.zanna.testapp";
    layout.overlayFileOffset = 0x400;
    layout.installDirectories.push_back({WindowsInstallRoot::InstallDir, "themes"});
    layout.uninstallDirectories = layout.installDirectories;
    layout.installFiles.push_back({WindowsInstallRoot::InstallDir, "testapp.exe", 0x80, 16});
    auto stub = buildInstallerStub(layout, "x64");

    // Should produce non-empty .text and imports
    EXPECT_GE(stub.textSection.size(), static_cast<size_t>(8));
    EXPECT_FALSE(stub.imports.empty());

    // Build a PE with the stub and verify it
    PEBuildParams pe;
    pe.textSection = stub.textSection;
    pe.imports = stub.imports;
    pe.manifest = generateUacManifest();
    auto peBytes = buildPE(pe);

    std::ostringstream err;
    EXPECT_TRUE(verifyPE(peBytes, err));
}

TEST(InstallerStub, ImportsRuntimeCrcForOverlayIntegrity) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.executableName = "testapp.exe";
    layout.overlayFileOffset = 0x400;
    layout.installFiles.push_back(
        {WindowsInstallRoot::InstallDir, "testapp.exe", 0x80, 16, 0x12345678});
    auto stub = buildInstallerStub(layout, "x64");

    bool found = false;
    for (const auto &imp : stub.imports) {
        if (imp.dllName == "ntdll.dll" &&
            std::find(imp.functions.begin(), imp.functions.end(), "RtlComputeCrc32") !=
                imp.functions.end()) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(InstallerStub, UsesSetFilePointerExForLargeOverlayOffsets) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.executableName = "testapp.exe";
    layout.overlayFileOffset = 0x100000000ull;
    layout.installFiles.push_back(
        {WindowsInstallRoot::InstallDir, "testapp.exe", 0x80, 16, 0x12345678});
    auto stub = buildInstallerStub(layout, "x64");

    bool found = false;
    for (const auto &imp : stub.imports) {
        if (imp.dllName == "kernel32.dll" &&
            std::find(imp.functions.begin(), imp.functions.end(), "SetFilePointerEx") !=
                imp.functions.end()) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(InstallerStub, EmitsWindowsAddRemoveProgramsMetadata) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.version = "1.2.3";
    layout.executableName = "testapp.exe";
    layout.publisher = "Zanna";
    layout.description = "Test app installer comments";
    layout.contact = "support@example.invalid";
    layout.identifier = "org.zanna.testapp";
    layout.homepage = "https://example.invalid/testapp";
    layout.displayIconRelativePath = "testapp.ico";
    layout.estimatedSizeKb = 42;
    layout.installDate = "20260512";
    layout.overlayFileOffset = 0x400;
    auto stub = buildInstallerStub(layout, "x64");

    EXPECT_TRUE(containsUtf16LE(stub.stubData, "QuietUninstallString"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "DisplayIcon"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "EstimatedSize"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "InstallDate"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "URLInfoAbout"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "URLUpdateInfo"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "HelpLink"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "Comments"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "Contact"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "20260512"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "https://example.invalid/testapp"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "Test app installer comments"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "support@example.invalid"));
}

TEST(InstallerStub, ImportsAndEmbedsNativeWizard) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.executableName = "testapp.exe";
    layout.licenseText = "TEST LICENSE TERMS";
    layout.perUserInstall = true;

    auto installer = buildInstallerStub(layout, "x64");
    auto uninstaller = buildUninstallerStub(layout, "x64");
    layout.perUserInstall = false;
    auto machineInstaller = buildInstallerStub(layout, "x64");

    EXPECT_TRUE(stubHasImport(installer, "comctl32.dll", "InitCommonControlsEx"));
    EXPECT_TRUE(stubHasImport(installer, "user32.dll", "DialogBoxIndirectParamW"));
    EXPECT_TRUE(stubHasImport(installer, "user32.dll", "EndDialog"));
    EXPECT_TRUE(stubHasImport(installer, "user32.dll", "GetDlgItem"));
    EXPECT_TRUE(stubHasImport(installer, "user32.dll", "SendMessageW"));
    EXPECT_TRUE(stubHasImport(installer, "user32.dll", "EnableWindow"));
    EXPECT_TRUE(stubHasImport(installer, "uxtheme.dll", "SetWindowTheme"));
    EXPECT_TRUE(stubHasImport(installer, "dwmapi.dll", "DwmSetWindowAttribute"));
    EXPECT_TRUE(stubHasImport(installer, "gdi32.dll", "SetDCBrushColor"));
    EXPECT_TRUE(stubHasImport(installer, "user32.dll", "CreateDialogIndirectParamW"));
    EXPECT_TRUE(stubHasImport(installer, "user32.dll", "PeekMessageW"));
    EXPECT_TRUE(stubHasImport(installer, "kernel32.dll", "GetPrivateProfileIntW"));
    EXPECT_FALSE(stubHasImport(installer, "gdi32.dll", "CreateDIBSection"));
    EXPECT_FALSE(stubHasImport(installer, "kernel32.dll", "CreateThread"));
    EXPECT_TRUE(stubHasImport(uninstaller, "user32.dll", "DialogBoxIndirectParamW"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "I accept the license agreement"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "msctls_progress32"));
    EXPECT_FALSE(containsUtf16LE(installer.stubData, "< Back"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "Install"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "TEST LICENSE TERMS"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "DarkMode_Explorer"));
    EXPECT_TRUE(containsUtf16LEStringData(installer.stubData, "Current user"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "%LocalAppData%\\TestApp"));
    EXPECT_TRUE(containsUtf16LEStringData(machineInstaller.stubData, "All users"));
    EXPECT_TRUE(containsUtf16LE(machineInstaller.stubData, "%ProgramFiles%\\TestApp"));
    EXPECT_TRUE(containsUtf16LE(uninstaller.stubData, "I understand and want to continue"));
}

TEST(InstallerStub, EmbedsBrandedComponentWizardAndResponsiveProgress) {
    WindowsPackageLayout layout;
    layout.displayName = "Zanna Toolchain";
    layout.installDirName = "Zanna";
    layout.executableName = "zanna.exe";
    layout.licenseText = "GPL-3.0-only";
    layout.wizardImageWidth = 2;
    layout.wizardImageHeight = 1;
    layout.wizardImageRgba = {10, 20, 30, 255, 40, 50, 60, 255};
    layout.optionalComponents = {
        {"zannastudio", "Zanna Studio", "Integrated developer environment", true},
        {"samples", "Samples", "Example projects", true}};
    layout.installFiles.push_back(
        {WindowsInstallRoot::InstallDir, "bin/zannastudio.exe", 0, 4, 0, "", "zannastudio"});
    layout.installFiles.push_back(
        {WindowsInstallRoot::InstallDir, "share/zanna/samples/main.zia", 4, 4, 0, "", "samples"});

    const auto installer = buildInstallerStub(layout, "x64");
    EXPECT_TRUE(stubHasImport(installer, "gdi32.dll", "StretchDIBits"));
    EXPECT_TRUE(stubHasImport(installer, "user32.dll", "SetWindowLongPtrW"));
    EXPECT_TRUE(stubHasImport(installer, "user32.dll", "DispatchMessageW"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "Zanna Studio"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "Samples"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "/no-zannastudio"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "/no-samples"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "ZANNA_INSTALLER_COMPONENT_ZANNASTUDIO"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "ZANNA_INSTALLER_PROGRESS"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData,
                                "Setup stays visible while files are verified and installed. "
                                "Please wait."));
    const std::array<uint8_t, 8> expectedBgra = {30, 20, 10, 255, 60, 50, 40, 255};
    EXPECT_TRUE(std::search(installer.stubData.begin(),
                            installer.stubData.end(),
                            expectedBgra.begin(),
                            expectedBgra.end()) != installer.stubData.end());
}

TEST(InstallerStub, ImportsRegistryQueryForOwnedFileAssociationCleanup) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.executableName = "testapp.exe";
    layout.identifier = "org.zanna.testapp";
    layout.overlayFileOffset = 0x400;
    layout.fileAssociations.push_back(
        {".zia", "Zia Source", "text/x-zia", "org.zanna.testapp.zia", {}});

    auto installer = buildInstallerStub(layout, "x64");
    auto uninstaller = buildUninstallerStub(layout, "x64");

    auto hasImport = [](const StubResult &stub, const std::string &dll, const std::string &fn) {
        return std::any_of(stub.imports.begin(), stub.imports.end(), [&](const PEImport &imp) {
            return imp.dllName == dll &&
                   std::find(imp.functions.begin(), imp.functions.end(), fn) != imp.functions.end();
        });
    };

    EXPECT_TRUE(hasImport(installer, "advapi32.dll", "RegQueryValueExW"));
    EXPECT_TRUE(hasImport(installer, "kernel32.dll", "lstrcmpW"));
    EXPECT_TRUE(hasImport(installer, "kernel32.dll", "lstrcmpiW"));
    EXPECT_TRUE(hasImport(uninstaller, "advapi32.dll", "RegQueryValueExW"));
    EXPECT_TRUE(hasImport(uninstaller, "kernel32.dll", "lstrcmpW"));
    EXPECT_TRUE(hasImport(uninstaller, "kernel32.dll", "lstrcmpiW"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "ZAPSContentTypeOwner"));
    EXPECT_TRUE(containsUtf16LE(uninstaller.stubData, "ZAPSContentTypeOwner"));
}

TEST(InstallerStub, UsesModernKnownFoldersAndRecursiveRegistryCleanup) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.executableName = "testapp.exe";
    layout.identifier = "org.zanna.testapp";
    layout.createDesktopShortcut = true;
    layout.createStartMenuShortcut = true;
    layout.fileAssociations.push_back(
        {".zia", "Zia Source", "text/x-zia", "org.zanna.testapp.zia", {}});

    auto installer = buildInstallerStub(layout, "x64");
    auto uninstaller = buildUninstallerStub(layout, "x64");

    auto hasImport = [](const StubResult &stub, const std::string &dll, const std::string &fn) {
        return std::any_of(stub.imports.begin(), stub.imports.end(), [&](const PEImport &imp) {
            return imp.dllName == dll &&
                   std::find(imp.functions.begin(), imp.functions.end(), fn) != imp.functions.end();
        });
    };

    EXPECT_TRUE(hasImport(installer, "shell32.dll", "SHGetKnownFolderPath"));
    EXPECT_TRUE(hasImport(installer, "ole32.dll", "CoTaskMemFree"));
    EXPECT_TRUE(hasImport(installer, "advapi32.dll", "RegDeleteTreeW"));
    EXPECT_TRUE(hasImport(installer, "kernel32.dll", "GetDateFormatW"));
    EXPECT_TRUE(hasImport(uninstaller, "shell32.dll", "SHGetKnownFolderPath"));
    EXPECT_TRUE(hasImport(uninstaller, "ole32.dll", "CoTaskMemFree"));
    EXPECT_TRUE(hasImport(uninstaller, "advapi32.dll", "RegDeleteTreeW"));
    EXPECT_FALSE(hasImport(installer, "shell32.dll", "SHGetFolderPathW"));
    EXPECT_FALSE(hasImport(uninstaller, "advapi32.dll", "RegDeleteKeyW"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "yyyyMMdd"));
}

TEST(InstallerStub, ImportsPathAndDirectoryFailureChecks) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.executableName = "testapp.exe";
    layout.overlayFileOffset = 0x400;
    layout.installDirectories.push_back({WindowsInstallRoot::InstallDir, "data"});
    auto stub = buildInstallerStub(layout, "x64");

    bool foundGetLastError = false;
    bool foundLstrlen = false;
    for (const auto &imp : stub.imports) {
        if (imp.dllName != "kernel32.dll")
            continue;
        foundGetLastError = foundGetLastError ||
                            std::find(imp.functions.begin(), imp.functions.end(), "GetLastError") !=
                                imp.functions.end();
        foundLstrlen =
            foundLstrlen || std::find(imp.functions.begin(), imp.functions.end(), "lstrlenW") !=
                                imp.functions.end();
    }
    EXPECT_TRUE(foundGetLastError);
    EXPECT_TRUE(foundLstrlen);
}

TEST(InstallerStub, SupportsQuietAutomationFlags) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.executableName = "testapp.exe";
    layout.overlayFileOffset = 0x400;
    auto installer = buildInstallerStub(layout, "x64");
    auto uninstaller = buildUninstallerStub(layout, "x64");

    auto hasImport = [](const StubResult &stub, const std::string &dll, const std::string &fn) {
        return std::any_of(stub.imports.begin(), stub.imports.end(), [&](const PEImport &imp) {
            return imp.dllName == dll &&
                   std::find(imp.functions.begin(), imp.functions.end(), fn) != imp.functions.end();
        });
    };

    EXPECT_TRUE(hasImport(installer, "kernel32.dll", "GetCommandLineW"));
    EXPECT_TRUE(hasImport(installer, "shlwapi.dll", "StrStrIW"));
    EXPECT_TRUE(hasImport(uninstaller, "kernel32.dll", "GetCommandLineW"));
    EXPECT_TRUE(hasImport(uninstaller, "shlwapi.dll", "StrStrIW"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "/quiet"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "/silent"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, "/norestart"));
    EXPECT_TRUE(containsUtf16LE(installer.stubData, " /quiet"));
    EXPECT_FALSE(containsUtf16LE(installer.stubData, " /quiet /norestart"));
    EXPECT_TRUE(containsUtf16LE(uninstaller.stubData, "/quiet"));
    EXPECT_TRUE(containsUtf16LE(uninstaller.stubData, "/silent"));
    EXPECT_TRUE(containsUtf16LE(uninstaller.stubData, "/norestart"));
    EXPECT_TRUE(containsUtf16LE(uninstaller.stubData, "I understand and want to continue"));
    const std::array<uint8_t, 5> cancelledExit{{0xB9, 0x42, 0x06, 0x00, 0x00}};
    EXPECT_TRUE(std::search(installer.textSection.begin(),
                            installer.textSection.end(),
                            cancelledExit.begin(),
                            cancelledExit.end()) != installer.textSection.end());
    EXPECT_TRUE(std::search(uninstaller.textSection.begin(),
                            uninstaller.textSection.end(),
                            cancelledExit.begin(),
                            cancelledExit.end()) != uninstaller.textSection.end());
}

TEST(InstallerStubGen, RejectsOutOfRangeIATSlotFixup) {
    InstallerStubGen gen;
    gen.callIATSlot(1);
    std::vector<PEImport> imports = {{"kernel32.dll", {"ExitProcess"}}};
    EXPECT_THROWS(gen.finishText(0x1000, 0x2000, imports, 0x3000), std::runtime_error);
}

TEST(InstallerStub, UninstallerGeneratesValidPE) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.version = "1.0.0";
    layout.executableName = "testapp.exe";
    layout.publisher = "Zanna";
    layout.identifier = "org.zanna.testapp";
    layout.uninstallDirectories.push_back({WindowsInstallRoot::InstallDir, "themes"});
    layout.uninstallFiles.push_back({WindowsInstallRoot::InstallDir, "testapp.exe", 0, 16});
    auto stub = buildUninstallerStub(layout, "x64");

    EXPECT_GT(stub.textSection.size(), static_cast<size_t>(10));
    EXPECT_FALSE(stub.imports.empty());
    bool foundGetLastError = false;
    for (const auto &imp : stub.imports) {
        if (imp.dllName == "kernel32.dll" &&
            std::find(imp.functions.begin(), imp.functions.end(), "GetLastError") !=
                imp.functions.end()) {
            foundGetLastError = true;
        }
    }
    EXPECT_TRUE(foundGetLastError);

    PEBuildParams pe;
    pe.textSection = stub.textSection;
    pe.imports = stub.imports;
    pe.manifest = generateAsInvokerManifest();
    auto peBytes = buildPE(pe);

    std::ostringstream err;
    EXPECT_TRUE(verifyPE(peBytes, err));
}

TEST(InstallerStub, UninstallerDoesNotScheduleInstallRootForRebootDeletion) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.version = "1.0.0";
    layout.executableName = "testapp.exe";
    layout.publisher = "Zanna";
    layout.identifier = "org.zanna.testapp";
    layout.uninstallDirectories.push_back({WindowsInstallRoot::InstallDir, "themes"});
    layout.uninstallFiles.push_back({WindowsInstallRoot::InstallDir, "testapp.exe", 0, 16});
    auto stub = buildUninstallerStub(layout, "x64");

    EXPECT_EQ(countIatCallsTo(stub, "kernel32.dll", "MoveFileExW"), static_cast<size_t>(1));
}

TEST(InstallerStub, ARM64UsesNativeBootstrap) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.executableName = "testapp.exe";
    auto stub = buildInstallerStub(layout, "arm64");
    EXPECT_EQ(stub.peArch, "arm64");
    EXPECT_GT(stub.textSection.size(), static_cast<size_t>(100));
    EXPECT_FALSE(stub.imports.empty());
    EXPECT_TRUE(stubHasImport(stub, "kernel32.dll", "CreateProcessW"));
    EXPECT_TRUE(stubHasImport(stub, "kernel32.dll", "WaitForSingleObject"));
    EXPECT_TRUE(stubHasImport(stub, "kernel32.dll", "GetCommandLineW"));
    EXPECT_TRUE(stubHasImport(stub, "shlwapi.dll", "StrStrIW"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "\\WindowsPowerShell\\v1.0\\powershell.exe"));
    EXPECT_TRUE(containsUtf16LEStringData(stub.stubData, "-EncodedCommand"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "/quiet"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "/silent"));
    EXPECT_TRUE(containsUtf16LE(stub.stubData, "/norestart"));

    PEBuildParams pe;
    pe.arch = stub.peArch;
    pe.textSection = stub.textSection;
    pe.rdataSection = stub.stubData;
    pe.imports = stub.imports;
    const auto peBytes = buildPE(pe);
    EXPECT_EQ(readLE16(peBytes.data() + 0x84), static_cast<uint16_t>(0xAA64));
}

namespace {

/// @brief Decode a standard base64 string into raw bytes ('=' padding tolerated).
std::vector<uint8_t> decodeBase64Bytes(const std::string &in) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    int buffer = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=')
            break;
        const int v = value(c);
        if (v < 0)
            continue;
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

/// @brief Extract the ASCII characters from a UTF-16LE byte buffer (drops the
///        zero high bytes; non-ASCII code units are skipped).
std::string utf16LeToAscii(const std::vector<uint8_t> &data) {
    std::string out;
    for (size_t i = 0; i + 1 < data.size(); i += 2) {
        if (data[i + 1] == 0 && data[i] != 0 && data[i] < 0x80)
            out.push_back(static_cast<char>(data[i]));
    }
    return out;
}

/// @brief Recover the gzip-packed PowerShell source embedded in installer bytes.
std::string decodedEmbeddedPowerShellSource(const std::vector<uint8_t> &bytes) {
    const std::string ascii = utf16LeToAscii(bytes);
    const std::string marker = "-EncodedCommand ";
    const size_t markerPos = ascii.find(marker);
    if (markerPos == std::string::npos)
        return {};

    std::string b64;
    for (size_t pos = markerPos + marker.size(); pos < ascii.size(); ++pos) {
        const char c = ascii[pos];
        const bool isB64 = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
        if (!isB64)
            break;
        b64.push_back(c);
    }
    const std::string bootstrap = utf16LeToAscii(decodeBase64Bytes(b64));
    const std::string packedMarker = "FromBase64String('";
    const size_t packedStart = bootstrap.find(packedMarker);
    if (packedStart == std::string::npos)
        return bootstrap;
    const size_t dataStart = packedStart + packedMarker.size();
    const size_t dataEnd = bootstrap.find("')", dataStart);
    if (dataEnd == std::string::npos)
        return {};
    const auto packed = decodeBase64Bytes(bootstrap.substr(dataStart, dataEnd - dataStart));
    const auto source = gunzip(packed.data(), packed.size());
    return std::string(source.begin(), source.end());
}

/// @brief Recover the PowerShell source embedded in an ARM64 installer stub.
std::string decodedArm64PowerShellSource(const StubResult &stub) {
    return decodedEmbeddedPowerShellSource(stub.stubData);
}

} // namespace

/// @brief The ARM64 installer is a PowerShell launcher; its -EncodedCommand must
///        round-trip from base64+UTF-16LE back to a structurally valid PS script.
///        Without this, a broken encoder ships a stub that fails only at runtime.
TEST(InstallerStub, ARM64EncodedCommandDecodesToValidPowerShell) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.executableName = "testapp.exe";
    layout.version = "1.0.0";
    layout.publisher = "Zanna";
    layout.identifier = "org.zanna.testapp";
    layout.overlayFileOffset = 0x800;
    layout.installFiles.push_back({WindowsInstallRoot::InstallDir, "testapp.exe", 0x800, 16});
    const auto stub = buildInstallerStub(layout, "arm64");

    const std::string script = decodedArm64PowerShellSource(stub);
    ASSERT_TRUE(!script.empty());

    // Windows PowerShell 5.1 rejects a trailing comma before an array-subexpression close.
    EXPECT_TRUE(script.find(",\n)") == std::string::npos);
    EXPECT_CONTAINS(script, "$files=@(\n,@(");
    EXPECT_TRUE(script.find("[IO.File]::CreateNew") == std::string::npos);
    EXPECT_CONTAINS(script, "[IO.FileMode]::CreateNew");

    // The decoded script must carry the install scaffolding and overlay-read loop,
    // proving the encoder produced a complete, uncorrupted program.
    EXPECT_TRUE(script.find("$installLeaf=") != std::string::npos);
    EXPECT_TRUE(script.find("$baseOffset=") != std::string::npos);
    EXPECT_TRUE(script.find("$display=") != std::string::npos);
    EXPECT_TRUE(script.find(".Read(") != std::string::npos);      // overlay byte reader
    EXPECT_TRUE(script.find("testapp.exe") != std::string::npos); // payload file name
    EXPECT_TRUE(script.find("Security.Cryptography.SHA256") != std::string::npos);
    EXPECT_TRUE(script.find("installer overlay checksum mismatch") != std::string::npos);
    EXPECT_TRUE(script.find("RegisterAssoc") != std::string::npos);
    EXPECT_TRUE(script.find("UnregisterAssoc") != std::string::npos);
    EXPECT_TRUE(script.find("BroadcastEnv") != std::string::npos);
    EXPECT_TRUE(script.find("$quiet=($env:ZANNA_INSTALLER_QUIET -eq '1')") != std::string::npos);
    EXPECT_TRUE(script.find("$noRestart=($env:ZANNA_INSTALLER_NORESTART -eq '1')") !=
                std::string::npos);
    EXPECT_TRUE(script.find("if($quiet){return}") != std::string::npos);
    EXPECT_TRUE(script.find("exit 1602") != std::string::npos);
    EXPECT_TRUE(script.find("if(-not $entry){return}") != std::string::npos);
    EXPECT_TRUE(script.find("Remove-Item -LiteralPath $install -Recurse") == std::string::npos);
    EXPECT_TRUE(script.find("function BeginTransaction") != std::string::npos);
    EXPECT_TRUE(script.find("function RestoreTransaction") != std::string::npos);
    EXPECT_TRUE(script.find("function LegacyInstallOwned") != std::string::npos);
    EXPECT_TRUE(script.find("migrating generated legacy installation") != std::string::npos);
    EXPECT_TRUE(script.find("WriteInstallerLog 'ERROR'") != std::string::npos);
    EXPECT_TRUE(script.find("ZANNA_INSTALLER_LOG") != std::string::npos);
    EXPECT_TRUE(script.find("ZANNA_INSTALLER_NATIVE_STAGE") != std::string::npos);
    EXPECT_TRUE(script.find("refusing to replace unowned path") != std::string::npos);
    EXPECT_TRUE(script.find("refusing reparse-point installer parent") != std::string::npos);
    EXPECT_TRUE(script.find("compressed installer payload contains an unmanifested file") !=
                std::string::npos);
    // The layout's identifier must survive into the script (registry/uninstall key).
    EXPECT_TRUE(script.find("org.zanna.testapp") != std::string::npos);
}

TEST(InstallerStub, ARM64LargeLicenseUsesCompactBootstrapNotice) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.executableName = "testapp.exe";
    layout.version = "1.0.0";
    layout.publisher = "Zanna";
    layout.identifier = "org.zanna.testapp";
    layout.overlayFileOffset = 0x800;
    layout.licenseText.assign(25000, 'L');
    layout.installFiles.push_back({WindowsInstallRoot::InstallDir, "testapp.exe", 0x800, 16});

    const auto stub = buildInstallerStub(layout, "arm64");
    const std::string script = decodedArm64PowerShellSource(stub);
    ASSERT_TRUE(!script.empty());
    EXPECT_TRUE(script.find("complete license terms are included") != std::string::npos);
    EXPECT_TRUE(script.find(std::string(4096, 'L')) == std::string::npos);
}

TEST(InstallerStub, ARM64UninstallerUsesDetachedOwnedCleanup) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.executableName = "testapp.exe";
    layout.version = "1.0.0";
    layout.publisher = "Zanna";
    layout.identifier = "org.zanna.testapp";
    layout.installDate = "20000101";

    const auto stub = buildUninstallerStub(layout, "arm64");
    const std::string script = decodedArm64PowerShellSource(stub);
    ASSERT_TRUE(!script.empty());
    EXPECT_TRUE(script.find("$cleanupCommand=") != std::string::npos);
    EXPECT_TRUE(script.find("Start-Process -FilePath $cleanupExe") != std::string::npos);
    EXPECT_TRUE(script.find("RegistryValueKind]::None") != std::string::npos);
    EXPECT_TRUE(script.find("Remove-Item -LiteralPath $install -Recurse") == std::string::npos);
}

TEST(InstallerStub, AllowsZeroBytePayloadFile) {
    WindowsPackageLayout layout;
    layout.displayName = "TestApp";
    layout.installDirName = "TestApp";
    layout.executableName = "testapp.exe";
    layout.overlayFileOffset = 0x400;
    layout.installFiles.push_back({WindowsInstallRoot::InstallDir, "empty.dat", 0x80, 0});
    auto stub = buildInstallerStub(layout, "x64");
    EXPECT_GT(stub.textSection.size(), static_cast<size_t>(10));
}

TEST(WindowsPackageBuilder, BuildsInstallerWithCompressedPayloadOverlay) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_windows_builder_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot / "assets" / "themes");

    writeTestWindowsPe(tmpRoot / "app.exe");
    {
        std::ofstream asset(tmpRoot / "assets" / "themes" / "dark.json", std::ios::binary);
        asset << "{\"theme\":\"dark\"}";
    }
    {
        std::ofstream asset(tmpRoot / "assets" / "themes" / "repeat.txt", std::ios::binary);
        asset << std::string(4096, 'A');
    }
    {
        PkgImage img;
        img.width = 32;
        img.height = 32;
        img.pixels.resize(32 * 32 * 4, 255);
        const auto png = pngEncode(img);
        std::ofstream icon(tmpRoot / "icon.png", std::ios::binary);
        icon.write(reinterpret_cast<const char *>(png.data()),
                   static_cast<std::streamsize>(png.size()));
    }

    PackageConfig pkg;
    pkg.displayName = "Test App";
    pkg.identifier = "org.zanna.testapp";
    pkg.author = "Zanna";
    pkg.shortcutDesktop = true;
    pkg.shortcutMenu = true;
    pkg.iconPath = "icon.png";
    pkg.assets.push_back({"assets", "data"});
    pkg.fileAssociations.push_back({".zia", "Zia Source", "text/x-zia", ""});

    const fs::path outPath = tmpRoot / "test_setup.exe";
    WindowsBuildParams params;
    params.projectName = "testapp";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = outPath.string();
    params.archStr = "x64";

    buildWindowsPackage(params);

    auto pe = readFile(outPath.string());
    std::ostringstream err;
    const bool overlayOk = verifyPEZipOverlayPayload(pe,
                                                     {"meta/payload.zip",
                                                      "meta/install_manifest.next",
                                                      "meta/start_menu.lnk",
                                                      "meta/desktop.lnk",
                                                      "meta/manifest.sha256"},
                                                     err);
    if (!overlayOk)
        std::cerr << err.str();
    EXPECT_TRUE(overlayOk);
    EXPECT_TRUE(containsUtf16LE(pe, "%LocalAppData%\\Test App\\testapp.exe"));
    EXPECT_TRUE(containsUtf16LEStringData(pe, "%LocalAppData%\\Test App\\testapp.ico"));
    EXPECT_TRUE(containsUtf16LE(pe, "Software\\Classes\\.zia"));
    EXPECT_TRUE(containsUtf16LE(pe, "Software\\Classes\\.zia\\OpenWithProgids"));
    EXPECT_TRUE(containsUtf16LE(pe, "org.zanna.testapp.zia"));
    EXPECT_TRUE(containsUtf16LE(pe, "DefaultIcon"));
    EXPECT_TRUE(containsUtf16LE(pe, "QuietUninstallString"));
    const std::string deleteValueImport = "RegDeleteValueW";
    EXPECT_TRUE(
        std::search(pe.begin(), pe.end(), deleteValueImport.begin(), deleteValueImport.end()) !=
        pe.end());
    const std::string createProcessImport = "CreateProcessW";
    EXPECT_TRUE(
        std::search(pe.begin(), pe.end(), createProcessImport.begin(), createProcessImport.end()) !=
        pe.end());
    const auto payloadZip = extractPeOverlayZipEntry(pe, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    EXPECT_TRUE(zipContainsEntries(payloadZip,
                                   {"testapp.exe",
                                    "testapp.ico",
                                    "data/themes/dark.json",
                                    "data/themes/repeat.txt",
                                    "uninstall.exe",
                                    ".zanna-install-manifest.txt"}));
    EXPECT_TRUE(zipEntryUsesDeflate(payloadZip, "data/themes/repeat.txt"));
    const auto nextManifest = extractPeOverlayZipEntry(pe, "meta/install_manifest.next");
    ASSERT_FALSE(nextManifest.empty());
    const std::string nextManifestText(nextManifest.begin(), nextManifest.end());
    EXPECT_CONTAINS(nextManifestText, "testapp.exe\n");
    EXPECT_CONTAINS(nextManifestText, "data/themes/repeat.txt\n");
    EXPECT_CONTAINS(nextManifestText, ".zanna-install-manifest.txt\n");

    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, SupportsUtf8SourceAssetAndOutputPathsOnWindows) {
    namespace fs = std::filesystem;
    const std::string rootName = "zanna_packaging_\xE6\x9D\xB1\xE4\xBA\xAC";
    const std::string executableName = "\xE5\xBF\x9C\xE7\x94\xA8.exe";
    const std::string assetName = "\xE8\xA8\xAD\xE5\xAE\x9A.txt";
    const std::string outputName = "\xE3\x82\xBB\xE3\x83\x83\xE3\x83\x88\xE3\x82\xA2\xE3\x83\x83"
                                   "\xE3\x83\x97.exe";
    const fs::path tmpRoot = fs::temp_directory_path() / zanna::filesystem::pathFromUtf8(rootName);
    const fs::path executable = tmpRoot / zanna::filesystem::pathFromUtf8(executableName);
    const fs::path asset = tmpRoot / zanna::filesystem::pathFromUtf8(assetName);
    const fs::path output = tmpRoot / zanna::filesystem::pathFromUtf8(outputName);
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    writeTestWindowsPe(executable);
    const std::vector<uint8_t> assetBytes{'u', 't', 'f', '8'};
    writeFileAtomic(asset, assetBytes);

    PackageConfig pkg;
    pkg.displayName = "Unicode Paths";
    pkg.identifier = "org.zanna.unicode-paths";
    pkg.author = "Zanna";
    pkg.assets.push_back({assetName, "data"});

    WindowsBuildParams params;
    params.projectName = "unicode_paths";
    params.version = "1.0.0";
    params.executablePath = zanna::filesystem::pathToUtf8(executable);
    params.projectRoot = zanna::filesystem::pathToUtf8(tmpRoot);
    params.pkgConfig = pkg;
    params.outputPath = zanna::filesystem::pathToUtf8(output);
    params.archStr = "x64";

    buildWindowsPackage(params);

    ASSERT_TRUE(fs::is_regular_file(output));
    const auto pe = readFile(params.outputPath);
    const auto payloadZip = extractPeOverlayZipEntry(pe, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    EXPECT_TRUE(zipContainsEntries(payloadZip,
                                   {"unicode_paths.exe",
                                    "data/" + assetName,
                                    "uninstall.exe",
                                    ".zanna-install-manifest.txt"}));
    EXPECT_EQ(readFile(zanna::filesystem::pathToUtf8(asset)), assetBytes);

    fs::remove_all(tmpRoot);
}

TEST(AppImage, BuildsVerifiableApplicationImage) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_appimage_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot / "assets" / "levels");

    // Synthetic ELF executable. buildAppImage only reads the bytes; the
    // Mach-O/ELF/PE format check lives in the CLI layer, not the builder.
    {
        std::vector<uint8_t> elf(128, 0);
        elf[0] = 0x7F;
        elf[1] = 'E';
        elf[2] = 'L';
        elf[3] = 'F';
        elf[4] = 2; // ELFCLASS64
        elf[5] = 1; // ELFDATA2LSB
        std::ofstream exe(tmpRoot / "game", std::ios::binary);
        exe.write(reinterpret_cast<const char *>(elf.data()),
                  static_cast<std::streamsize>(elf.size()));
    }
    {
        std::ofstream asset(tmpRoot / "assets" / "levels" / "level1.dat", std::ios::binary);
        asset << "level-one";
    }
    {
        PkgImage img;
        img.width = 32;
        img.height = 32;
        img.pixels.resize(32 * 32 * 4, 255);
        const auto png = pngEncode(img);
        std::ofstream icon(tmpRoot / "icon.png", std::ios::binary);
        icon.write(reinterpret_cast<const char *>(png.data()),
                   static_cast<std::streamsize>(png.size()));
    }

    PackageConfig pkg;
    pkg.displayName = "Space Game";
    pkg.description = "A test game";
    pkg.iconPath = "icon.png";
    pkg.assets.push_back({"assets", "data"});

    const fs::path outPath = tmpRoot / "SpaceGame-x86_64.AppImage";
    LinuxBuildParams params;
    params.projectName = "spacegame";
    params.version = "1.2.0";
    params.executablePath = (tmpRoot / "game").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = outPath.string();
    params.archStr = "x64";

    buildAppImage(params);

    auto appImage = readFile(outPath.string());
    std::string verifyErr;
    const bool ok = verifyLinuxAppImage(appImage, &verifyErr);
    if (!ok)
        std::cerr << verifyErr;
    EXPECT_TRUE(ok);
    // The FUSE-less self-extractor is a shell script (shebang), not an ELF.
    ASSERT_GT(appImage.size(), static_cast<size_t>(2));
    EXPECT_EQ(appImage[0], static_cast<uint8_t>('#'));
    EXPECT_EQ(appImage[1], static_cast<uint8_t>('!'));
    const std::string appImageRuntime(appImage.begin(), appImage.end());
    EXPECT_CONTAINS(appImageRuntime, "--appimage-help");
    EXPECT_CONTAINS(appImageRuntime, "ZANNA_APPIMAGE_CLEAN_CACHE");

    // The appended gzip payload must contain the expected portable layout.
    const auto payloadGz = appImagePayloadBytes(appImage);
    ASSERT_FALSE(payloadGz.empty());
    const auto tarBytes = gunzip(payloadGz.data(), payloadGz.size());
    ASSERT_FALSE(tarBytes.empty());
    auto tarContains = [&](const std::string &name) {
        return std::search(tarBytes.begin(), tarBytes.end(), name.begin(), name.end()) !=
               tarBytes.end();
    };
    EXPECT_TRUE(tarContains("AppRun"));
    EXPECT_TRUE(tarContains("usr/bin/spacegame"));
    EXPECT_TRUE(tarContains("spacegame.desktop"));
    EXPECT_TRUE(tarContains("spacegame.png"));
    EXPECT_TRUE(tarContains("usr/share/spacegame/data/levels/level1.dat"));

    fs::remove_all(tmpRoot);
}

TEST(LinuxAppPackage, DebUsesSharedAppFhsLayout) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_appdeb_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot / "assets" / "levels");
    {
        std::vector<uint8_t> elf(128, 0);
        elf[0] = 0x7F;
        elf[1] = 'E';
        elf[2] = 'L';
        elf[3] = 'F';
        elf[4] = 2;
        elf[5] = 1;
        std::ofstream exe(tmpRoot / "game", std::ios::binary);
        exe.write(reinterpret_cast<const char *>(elf.data()),
                  static_cast<std::streamsize>(elf.size()));
    }
    {
        std::ofstream asset(tmpRoot / "assets" / "levels" / "level1.dat", std::ios::binary);
        asset << "level-one";
    }

    PackageConfig pkg;
    pkg.displayName = "Space Game";
    pkg.description = "A test game";
    pkg.assets.push_back({"assets", "data"});

    const fs::path outPath = tmpRoot / "spacegame.deb";
    LinuxBuildParams params;
    params.projectName = "spacegame";
    params.version = "1.2.0";
    params.executablePath = (tmpRoot / "game").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = outPath.string();
    params.archStr = "amd64";

    buildDebPackage(params);

    auto debBytes = readFile(outPath.string());
    std::ostringstream err;
    const bool ok = verifyDebPayload(debBytes,
                                     {"usr/bin/spacegame",
                                      "usr/share/spacegame/data/levels/level1.dat",
                                      "usr/share/doc/spacegame/README",
                                      "usr/share/doc/spacegame/copyright"},
                                     err);
    if (!ok)
        std::cerr << err.str();
    EXPECT_TRUE(ok);

    fs::remove_all(tmpRoot);
}

TEST(LinuxAppPackage, RpmBuildsOrReportsMissingRpmbuild) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_apprpm_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);
    {
        std::vector<uint8_t> elf(128, 0);
        elf[0] = 0x7F;
        elf[1] = 'E';
        elf[2] = 'L';
        elf[3] = 'F';
        elf[4] = 2;
        elf[5] = 1;
        std::ofstream exe(tmpRoot / "game", std::ios::binary);
        exe.write(reinterpret_cast<const char *>(elf.data()),
                  static_cast<std::streamsize>(elf.size()));
    }

    PackageConfig pkg;
    pkg.displayName = "Space Game";
    pkg.description = "A test game";
    pkg.license = "MIT";

    const fs::path outPath = tmpRoot / "spacegame.rpm";
    LinuxBuildParams params;
    params.projectName = "spacegame";
    params.version = "1.2.0";
    params.executablePath = (tmpRoot / "game").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = outPath.string();
    params.archStr = "x64";

    // rpmbuild is the build engine for RPMs and is not present on every host
    // (e.g. macOS). Either path is correct: a produced .rpm, or a clear diagnostic.
    bool threw = false;
    std::string what;
    try {
        buildRpmPackage(params);
    } catch (const std::exception &ex) {
        threw = true;
        what = ex.what();
    }
    if (threw) {
        EXPECT_CONTAINS(what, "rpmbuild");
    } else {
        auto rpm = readFile(outPath.string());
        ASSERT_GE(rpm.size(), static_cast<size_t>(4));
        EXPECT_EQ(rpm[0], static_cast<uint8_t>(0xED));
        EXPECT_EQ(rpm[1], static_cast<uint8_t>(0xAB));
        EXPECT_EQ(rpm[2], static_cast<uint8_t>(0xEE));
        EXPECT_EQ(rpm[3], static_cast<uint8_t>(0xDB));
    }

    fs::remove_all(tmpRoot);
}

TEST(LinuxPackageSigning, ReportsMissingSigningToolClearly) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_sign_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);
    const fs::path pkgPath = tmpRoot / "pkg.deb";
    {
        std::ofstream f(pkgPath, std::ios::binary);
        f << "stand-in package bytes";
    }

    // dpkg-sig/rpmsign are external tools not present on every host. Either outcome
    // is correct, but both must clearly name the tool: a "not found on PATH"
    // diagnostic (this host) or a signing failure (a host with the tool + bogus key).
    bool threw = false;
    std::string what;
    try {
        signLinuxPackage(pkgPath.string(), "Zanna Test Signing Key", /*isRpm=*/false);
    } catch (const std::exception &ex) {
        threw = true;
        what = ex.what();
    }
    EXPECT_TRUE(threw);
    EXPECT_CONTAINS(what, "dpkg-sig");

    // An empty key is rejected up front, independent of any external tool.
    bool emptyThrew = false;
    try {
        signLinuxPackage(pkgPath.string(), "", /*isRpm=*/true);
    } catch (const std::exception &) {
        emptyThrew = true;
    }
    EXPECT_TRUE(emptyThrew);

    fs::remove_all(tmpRoot);
}

TEST(MacOSAppDmg, BuildsVerifiableDiskImage) {
#if !ZANNA_HOST_MACOS
    // hdiutil is macOS-only; nothing to build or verify on other hosts.
    return;
#else
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_appdmg_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot / "assets");
    {
        // Minimal Mach-O-ish stand-in. The builder reads the bytes and, with sign
        // mode "none", does not invoke codesign, so a real binary is unnecessary.
        std::vector<uint8_t> macho(256, 0);
        macho[0] = 0xCF;
        macho[1] = 0xFA;
        macho[2] = 0xED;
        macho[3] = 0xFE; // MH_MAGIC_64 (little-endian)
        std::ofstream exe(tmpRoot / "game", std::ios::binary);
        exe.write(reinterpret_cast<const char *>(macho.data()),
                  static_cast<std::streamsize>(macho.size()));
    }
    {
        std::ofstream a(tmpRoot / "assets" / "data.bin", std::ios::binary);
        a << "asset";
    }

    PackageConfig pkg;
    pkg.displayName = "Space Game";
    pkg.identifier = "com.example.spacegame";
    pkg.macosSignMode = "none";
    pkg.assets.push_back({"assets", "data"});

    const fs::path outPath = tmpRoot / "SpaceGame.dmg";
    MacOSBuildParams params;
    params.projectName = "spacegame";
    params.version = "1.2.0";
    params.executablePath = (tmpRoot / "game").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = outPath.string();

    buildMacOSAppDmg(params);

    auto dmg = readFile(outPath.string());
    ASSERT_GE(dmg.size(), static_cast<size_t>(512));
    // UDIF images end with a 512-byte "koly" trailer block.
    const size_t n = dmg.size();
    EXPECT_EQ(dmg[n - 512], static_cast<uint8_t>('k'));
    EXPECT_EQ(dmg[n - 511], static_cast<uint8_t>('o'));
    EXPECT_EQ(dmg[n - 510], static_cast<uint8_t>('l'));
    EXPECT_EQ(dmg[n - 509], static_cast<uint8_t>('y'));

    fs::remove_all(tmpRoot);
#endif
}

TEST(MacOSPackageBuilder, PreservesExecutableAssetModeInAppZip) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_macos_asset_mode";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot / "scripts");
    {
        std::ofstream app(tmpRoot / "app", std::ios::binary);
        app << "mock executable";
    }
    {
        std::ofstream helper(tmpRoot / "scripts" / "helper.sh");
        helper << "#!/bin/sh\nexit 0\n";
    }
    fs::permissions(tmpRoot / "scripts" / "helper.sh",
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                        fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                        fs::perms::others_exec,
                    fs::perm_options::replace);

    MacOSBuildParams params;
    params.projectName = "assetmode";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app").string();
    params.projectRoot = tmpRoot.string();
    params.outputPath = (tmpRoot / "assetmode.zip").string();
    params.pkgConfig.displayName = "Asset Mode";
    params.pkgConfig.identifier = "org.zanna.assetmode";
    params.pkgConfig.macosSignMode = "none";
    params.pkgConfig.assets.push_back({"scripts", "tools"});

    buildMacOSPackage(params);
    const std::vector<uint8_t> zip = readFile(params.outputPath);
    uint16_t mode = 0;
    EXPECT_TRUE(zipEntryUnixMode(zip, "Asset Mode.app/Contents/Resources/tools/helper.sh", mode));
    EXPECT_EQ(mode, static_cast<uint16_t>(0100755));
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, ImportedDllNamesFromPeReadsImportTableOnly) {
    PEBuildParams pe;
    pe.textSection = {0xC3};
    pe.imports = {{"kernel32.dll", {"ExitProcess"}}};
    const std::string decoy = "ucrtbased.dll";
    pe.rdataSection.assign(decoy.begin(), decoy.end());
    pe.rdataSection.push_back(0);

    const auto dlls = importedDllNamesFromPe(buildPE(pe));
    EXPECT_TRUE(std::find(dlls.begin(), dlls.end(), "kernel32.dll") != dlls.end());
    EXPECT_TRUE(std::find(dlls.begin(), dlls.end(), "ucrtbased.dll") == dlls.end());
}

TEST(WindowsPackageBuilder, PerUserInstallerUsesLocalAppDataAndBundlesAdjacentDlls) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_windows_user_scope_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    writeTestWindowsPe(tmpRoot / "app.exe", "x64", {{"helper.dll", {"helper_entry"}}});
    {
        std::ofstream dll(tmpRoot / "helper.dll", std::ios::binary);
        dll << "helper";
    }

    PackageConfig pkg;
    pkg.displayName = "User App";
    pkg.identifier = "org.zanna.userapp";
    pkg.author = "Zanna";
    pkg.homepage = "https://example.invalid/userapp";
    pkg.shortcutDesktop = true;
    pkg.shortcutMenu = true;
    pkg.windowsInstallScope = "user";

    const fs::path outPath = tmpRoot / "user_setup.exe";
    WindowsBuildParams params;
    params.projectName = "userapp";
    params.version = "2.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = outPath.string();
    params.archStr = "x64";

    buildWindowsPackage(params);

    auto pe = readFile(outPath.string());
    std::ostringstream err;
    const bool overlayOk = verifyPEZipOverlayPayload(pe,
                                                     {"meta/payload.zip",
                                                      "meta/install_manifest.next",
                                                      "meta/start_menu.lnk",
                                                      "meta/desktop.lnk",
                                                      "meta/manifest.sha256"},
                                                     err);
    if (!overlayOk)
        std::cerr << err.str();
    EXPECT_TRUE(overlayOk);
    EXPECT_TRUE(containsUtf16LE(pe, "%LocalAppData%\\User App\\userapp.exe"));
    EXPECT_TRUE(containsUtf16LE(pe, "Environment"));
    EXPECT_FALSE(
        containsUtf16LE(pe, "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"));
    EXPECT_TRUE(containsUtf16LE(pe, "https://example.invalid/userapp"));
    const auto payloadZip = extractPeOverlayZipEntry(pe, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    EXPECT_TRUE(zipContainsEntries(
        payloadZip, {"userapp.exe", "helper.dll", "uninstall.exe", ".zanna-install-manifest.txt"}));

    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, UsesCustomInstallDirOpenArgsManifestAndVersionInfo) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_windows_custom_dir_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    writeTestWindowsPe(tmpRoot / "app.exe");

    PackageConfig pkg;
    pkg.displayName = "Display App";
    pkg.identifier = "org.zanna.displayapp";
    pkg.author = "Zanna";
    pkg.windowsInstallDir = "DisplayAppCustom";
    pkg.minOsWindows = "10.0";
    pkg.fileAssociations.push_back(
        {".zap", "Zanna App Project", "text/x-zanna-app", "--open-project"});

    WindowsBuildParams params;
    params.projectName = "displayapp";
    params.version = "1.2.3-beta";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "display_setup.exe").string();
    params.archStr = "x64";

    buildWindowsPackage(params);

    auto pe = readFile(params.outputPath);
    std::ostringstream err;
    EXPECT_TRUE(verifyPEZipOverlayPayload(
        pe, {"meta/payload.zip", "meta/install_manifest.next", "meta/manifest.sha256"}, err));
    EXPECT_TRUE(containsUtf16LE(pe, "%LocalAppData%\\DisplayAppCustom\\displayapp.exe"));
    EXPECT_FALSE(containsUtf16LE(pe, "%LocalAppData%\\Display App\\displayapp.exe"));
    EXPECT_TRUE(containsUtf16LE(pe, " --open-project"));
    EXPECT_TRUE(containsUtf16LE(pe, " \"%1\""));
    EXPECT_TRUE(containsUtf16LE(pe, "VS_VERSION_INFO"));
    EXPECT_TRUE(containsUtf16LE(pe, "Display App Setup"));
    EXPECT_TRUE(containsUtf16LE(pe, "1.2.3-beta"));
    EXPECT_TRUE(containsAscii(pe, "{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"));
    const auto payloadZip = extractPeOverlayZipEntry(pe, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    EXPECT_TRUE(zipContainsEntries(
        payloadZip, {"displayapp.exe", "uninstall.exe", ".zanna-install-manifest.txt"}));

    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, BundlesRecursiveDllsAndSkipsSystemNamedLocals) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_packaging_windows_recursive_dll_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    writeTestWindowsPe(tmpRoot / "app.exe",
                       "x64",
                       {{"helper.dll", {"helper_entry"}}, {"kernel32.dll", {"Sleep"}}});
    writeTestWindowsPe(tmpRoot / "helper.dll", "x64", {{"plugin.dll", {"plugin_entry"}}});
    writeTestWindowsPe(tmpRoot / "plugin.dll");
    writeBytes(tmpRoot / "kernel32.dll", {'n', 'o', 't', ' ', 'b', 'u', 'n', 'd', 'l', 'e', 'd'});

    PackageConfig pkg;
    pkg.displayName = "Recursive DLL App";

    WindowsBuildParams params;
    params.projectName = "recursivedll";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "recursive_setup.exe").string();
    params.archStr = "x64";

    buildWindowsPackage(params);

    const auto pe = readFile(params.outputPath);
    std::ostringstream err;
    EXPECT_TRUE(verifyPEZipOverlayPayload(
        pe, {"meta/payload.zip", "meta/install_manifest.next", "meta/manifest.sha256"}, err));
    const auto payloadZip = extractPeOverlayZipEntry(pe, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    ZipReader reader(payloadZip.data(), payloadZip.size());
    EXPECT_TRUE(reader.find("recursivedll.exe") != nullptr);
    EXPECT_TRUE(reader.find("helper.dll") != nullptr);
    EXPECT_TRUE(reader.find("plugin.dll") != nullptr);
    EXPECT_TRUE(reader.find("kernel32.dll") == nullptr);

    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, SkipsKnownWindowsGameRuntimeDlls) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_packaging_windows_game_system_dll_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    writeTestWindowsPe(tmpRoot / "app.exe",
                       "x64",
                       {{"xinput1_4.dll", {"XInputGetState"}},
                        {"iphlpapi.dll", {"GetAdaptersAddresses"}},
                        {"d3dcompiler_47.dll", {"D3DCompile"}}});
    writeBytes(tmpRoot / "xinput1_4.dll", {'l', 'o', 'c', 'a', 'l'});
    writeBytes(tmpRoot / "iphlpapi.dll", {'l', 'o', 'c', 'a', 'l'});
    writeBytes(tmpRoot / "d3dcompiler_47.dll", {'l', 'o', 'c', 'a', 'l'});

    PackageConfig pkg;
    pkg.displayName = "Game Runtime App";

    WindowsBuildParams params;
    params.projectName = "gameruntime";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "game_runtime_setup.exe").string();
    params.archStr = "x64";

    buildWindowsPackage(params);

    const auto pe = readFile(params.outputPath);
    std::ostringstream err;
    EXPECT_TRUE(verifyPEZipOverlayPayload(
        pe, {"meta/payload.zip", "meta/install_manifest.next", "meta/manifest.sha256"}, err));
    const auto payloadZip = extractPeOverlayZipEntry(pe, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    ZipReader reader(payloadZip.data(), payloadZip.size());
    EXPECT_TRUE(reader.find("gameruntime.exe") != nullptr);
    EXPECT_TRUE(reader.find("xinput1_4.dll") == nullptr);
    EXPECT_TRUE(reader.find("iphlpapi.dll") == nullptr);
    EXPECT_TRUE(reader.find("d3dcompiler_47.dll") == nullptr);

    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, RejectsMissingDebugRuntimeDllDependency) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_packaging_windows_debug_runtime_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    writeTestWindowsPe(tmpRoot / "app.exe", "x64", {{"vcruntime140d.dll", {"debug_entry"}}});

    PackageConfig pkg;
    pkg.displayName = "Debug Runtime App";

    WindowsBuildParams params;
    params.projectName = "debugruntime";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "debug_runtime_setup.exe").string();
    params.archStr = "x64";

    EXPECT_THROWS(buildWindowsPackage(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, BundlesAdjacentReleaseRuntimeDllDependencies) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_packaging_windows_release_runtime_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    writeTestWindowsPe(
        tmpRoot / "app.exe",
        "x64",
        {{"vcruntime140.dll", {"runtime_entry"}}, {"msvcp140.dll", {"standard_library_entry"}}});
    writeTestWindowsPe(tmpRoot / "vcruntime140.dll");
    writeTestWindowsPe(tmpRoot / "msvcp140.dll");

    PackageConfig pkg;
    pkg.displayName = "Release Runtime App";

    WindowsBuildParams params;
    params.projectName = "releaseruntime";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "release_runtime_setup.exe").string();
    params.archStr = "x64";

    buildWindowsPackage(params);
    const auto pe = readFile(params.outputPath);
    const auto payloadZip = extractPeOverlayZipEntry(pe, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    EXPECT_TRUE(
        zipContainsEntries(payloadZip, {"releaseruntime.exe", "vcruntime140.dll", "msvcp140.dll"}));
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, BundlesCompilerRuntimeFromNativeHostDirectory) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_packaging_windows_host_runtime_test";
    const fs::path appDir = tmpRoot / "app";
    const fs::path supportDir = tmpRoot / "support";
    fs::remove_all(tmpRoot);
    fs::create_directories(appDir);
    fs::create_directories(supportDir);

    writeTestWindowsPe(appDir / "app.exe", "x64", {{"vcruntime140.dll", {"runtime_entry"}}});
    writeTestWindowsPe(supportDir / "vcruntime140.dll");
    writeTestWindowsPe(supportDir / "zanna-installer-host.exe");
    writeTestWindowsPe(supportDir / "zanna-installer-cleanup.exe");

    PackageConfig pkg;
    pkg.displayName = "Host Runtime App";
    pkg.identifier = "org.zanna.host-runtime-test";

    WindowsBuildParams params;
    params.projectName = "hostruntime";
    params.version = "1.0.0";
    params.executablePath = (appDir / "app.exe").string();
    params.projectRoot = appDir.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "host_runtime_setup.exe").string();
    params.archStr = "x64";
    params.installerHostPath = (supportDir / "zanna-installer-host.exe").string();
    params.installerCleanupPath = (supportDir / "zanna-installer-cleanup.exe").string();

    buildWindowsPackage(params);
    const auto pe = readFile(params.outputPath);
    const auto payloadZip = extractPeOverlayZipEntry(pe, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    EXPECT_TRUE(zipContainsEntries(payloadZip, {"hostruntime.exe", "vcruntime140.dll"}));
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, RejectsMissingReleaseRuntimeDllDependency) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_packaging_windows_missing_release_runtime_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    writeTestWindowsPe(tmpRoot / "app.exe", "x64", {{"vcruntime140.dll", {"runtime_entry"}}});

    PackageConfig pkg;
    pkg.displayName = "Missing Release Runtime App";

    WindowsBuildParams params;
    params.projectName = "missingreleaseruntime";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "missing_release_runtime_setup.exe").string();
    params.archStr = "x64";

    EXPECT_THROWS(buildWindowsPackage(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, SignsOwnedNestedPesBeforePayloadHashing) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_packaging_windows_nested_signing_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    writeTestWindowsPe(tmpRoot / "app.exe", "x64", {{"vcruntime140.dll", {"runtime_entry"}}});
    writeTestWindowsPe(tmpRoot / "vcruntime140.dll");

    PackageConfig pkg;
    pkg.displayName = "Nested Signing App";

    std::vector<std::string> signedNames;
    WindowsBuildParams params;
    params.projectName = "nestedsigning";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "nested_signing_setup.exe").string();
    params.archStr = "x64";
    params.peSigner = [&](std::string_view logicalName, const std::vector<uint8_t> &input) {
        signedNames.emplace_back(logicalName);
        std::vector<uint8_t> output = input;
        const std::string marker = "ZAPS-NESTED-SIGNED";
        output.insert(output.end(), marker.begin(), marker.end());
        return output;
    };

    buildWindowsPackage(params);
    const auto pe = readFile(params.outputPath);
    const auto payloadZip = extractPeOverlayZipEntry(pe, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    const auto app = extractZipEntry(payloadZip, "nestedsigning.exe");
    const auto runtime = extractZipEntry(payloadZip, "vcruntime140.dll");
    const auto uninstaller = extractZipEntry(payloadZip, "uninstall.exe");
    EXPECT_TRUE(containsAscii(app, "ZAPS-NESTED-SIGNED"));
    EXPECT_FALSE(containsAscii(runtime, "ZAPS-NESTED-SIGNED"));
    EXPECT_TRUE(containsAscii(uninstaller, "ZAPS-NESTED-SIGNED"));
    EXPECT_TRUE(std::find(signedNames.begin(), signedNames.end(), "nestedsigning.exe") !=
                signedNames.end());
    EXPECT_TRUE(std::find(signedNames.begin(), signedNames.end(), "uninstall.exe") !=
                signedNames.end());
    EXPECT_TRUE(std::find(signedNames.begin(), signedNames.end(), "vcruntime140.dll") ==
                signedNames.end());
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, BuildsNativeHostSetupAndNonRecursiveMaintenancePayload) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_windows_native_host_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    writeTestWindowsPe(tmpRoot / "app.exe");
    writeTestWindowsPe(tmpRoot / "zanna-installer-host.exe");
    writeTestWindowsPe(tmpRoot / "zanna-installer-cleanup.exe");

    PackageConfig pkg;
    pkg.displayName = "Native Host App";
    pkg.identifier = "org.zanna.native-host-test";
    pkg.windowsPublisher = "Zanna Test Publisher";

    WindowsBuildParams params;
    params.projectName = "nativehostapp";
    params.version = "2.3.4";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "native_host_setup.exe").string();
    params.archStr = "x64";
    params.installerHostPath = (tmpRoot / "zanna-installer-host.exe").string();
    params.installerCleanupPath = (tmpRoot / "zanna-installer-cleanup.exe").string();
    params.peSigner = [](std::string_view logicalName, const std::vector<uint8_t> &input) {
        std::vector<uint8_t> output = input;
        if (logicalName == "uninstall.exe") {
            const std::string marker = "SIGNED-MAINTENANCE-HOST";
            if (output.size() <= 64U + marker.size())
                throw std::runtime_error("test PE is unexpectedly small");
            std::copy(marker.begin(), marker.end(), output.begin() + 64);
        }
        return output;
    };

    buildWindowsPackage(params);
    const auto setup = readFile(params.outputPath);
    std::ostringstream nativeVerifyError;
    EXPECT_TRUE(verifyWindowsNativeInstaller(setup, nativeVerifyError));
    const auto metadataBytes = extractPeOverlayZipEntry(setup, "meta/installer-v2.txt");
    ASSERT_FALSE(metadataBytes.empty());
    const auto metadata = parseWindowsInstallerMetadata(
        std::string(reinterpret_cast<const char *>(metadataBytes.data()), metadataBytes.size()));
    EXPECT_EQ(metadata.packageMode, "setup");
    EXPECT_EQ(metadata.productKind, "application");
    EXPECT_EQ(metadata.identifier, pkg.identifier);
    ASSERT_EQ(metadata.outerFiles.size(), 1U);
    EXPECT_EQ(metadata.outerFiles[0].path, "uninstall.exe");

    const auto uninstaller = extractPeOverlayZipEntry(setup, "meta/uninstall.exe");
    ASSERT_FALSE(uninstaller.empty());
    EXPECT_TRUE(containsAscii(uninstaller, "SIGNED-MAINTENANCE-HOST"));
    const auto maintenanceBytes = extractPeOverlayZipEntry(uninstaller, "meta/installer-v2.txt");
    ASSERT_FALSE(maintenanceBytes.empty());
    const auto maintenance = parseWindowsInstallerMetadata(std::string(
        reinterpret_cast<const char *>(maintenanceBytes.data()), maintenanceBytes.size()));
    EXPECT_EQ(maintenance.packageMode, "maintenance");
    EXPECT_TRUE(maintenance.outerFiles.empty());
    EXPECT_EQ(maintenance.payloadFiles.size(), 2U);
    EXPECT_EQ(maintenance.payloadFiles[0].path, "nativehostapp.exe");

    const auto payload = extractPeOverlayZipEntry(setup, "meta/payload.zip");
    EXPECT_FALSE(extractZipEntry(payload, "nativehostapp.exe").empty());
    EXPECT_TRUE(extractZipEntry(payload, "uninstall.exe").empty());
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, RejectsDynamicRuntimeNativeHostTemplate) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_packaging_windows_dynamic_host_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);
    writeTestWindowsPe(tmpRoot / "app.exe");
    writeTestWindowsPe(tmpRoot / "host.exe", "x64", {{"vcruntime140.dll", {"runtime_entry"}}});
    writeTestWindowsPe(tmpRoot / "cleanup.exe");

    PackageConfig pkg;
    pkg.displayName = "Dynamic Host App";
    pkg.identifier = "org.zanna.dynamic-host-test";
    WindowsBuildParams params;
    params.projectName = "dynamichostapp";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "setup.exe").string();
    params.archStr = "x64";
    params.installerHostPath = (tmpRoot / "host.exe").string();
    params.installerCleanupPath = (tmpRoot / "cleanup.exe").string();
    EXPECT_THROWS(buildWindowsPackage(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, RejectsMissingCustomDllWithMsvcPrefix) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_packaging_windows_msvc_prefix_custom_dll_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    writeTestWindowsPe(tmpRoot / "app.exe", "x64", {{"msvcp_plugin.dll", {"plugin_entry"}}});

    PackageConfig pkg;
    pkg.displayName = "Custom Prefix DLL App";

    WindowsBuildParams params;
    params.projectName = "customprefixdll";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "custom_prefix_setup.exe").string();
    params.archStr = "x64";

    EXPECT_THROWS(buildWindowsPackage(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, RejectsMissingAdjacentDllDependency) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_windows_missing_dll_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    PEBuildParams appPe;
    appPe.textSection = {0xC3};
    appPe.imports = {{"plugin_runtime.dll", {"plugin_entry"}}};
    const auto appBytes = buildPE(appPe);
    {
        std::ofstream exe(tmpRoot / "app.exe", std::ios::binary);
        exe.write(reinterpret_cast<const char *>(appBytes.data()),
                  static_cast<std::streamsize>(appBytes.size()));
    }

    PackageConfig pkg;
    pkg.displayName = "Missing DLL App";

    WindowsBuildParams params;
    params.projectName = "missingdll";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "missing_setup.exe").string();
    params.archStr = "x64";

    EXPECT_THROWS(buildWindowsPackage(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, RejectsNonPePayload) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_windows_non_pe_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);
    writeBytes(tmpRoot / "app.exe", std::vector<uint8_t>{'M', 'Z', 's', 't', 'u', 'b'});

    PackageConfig pkg;
    pkg.displayName = "Valid Name";

    WindowsBuildParams params;
    params.projectName = "nonpe";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "nonpe_setup.exe").string();
    params.archStr = "x64";

    EXPECT_THROWS(buildWindowsPackage(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, RejectsPayloadArchitectureMismatch) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_packaging_windows_arch_mismatch_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);
    writeTestWindowsPe(tmpRoot / "app.exe", "arm64");

    PackageConfig pkg;
    pkg.displayName = "Arch Mismatch";

    WindowsBuildParams params;
    params.projectName = "archmismatch";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "arch_setup.exe").string();
    params.archStr = "x64";

    EXPECT_THROWS(buildWindowsPackage(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, BuildsArm64PayloadPackageWithNativeBootstrap) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_packaging_windows_builder_arm64_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);
    writeTestWindowsPe(tmpRoot / "app.exe", "arm64");

    PackageConfig pkg;
    pkg.displayName = "Test App";

    WindowsBuildParams params;
    params.projectName = "testapp";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "test_setup.exe").string();
    params.archStr = "arm64";

    buildWindowsPackage(params);

    auto pe = readFile(params.outputPath);
    std::ostringstream err;
    EXPECT_TRUE(verifyPEZipOverlay(pe, err));
    EXPECT_EQ(readLE16(pe.data() + 0x84), static_cast<uint16_t>(0xAA64));
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, RejectsUnsafeDisplayName) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_windows_bad_name_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);
    {
        std::ofstream exe(tmpRoot / "app.exe", std::ios::binary);
        exe.write("MZstub", 6);
    }

    PackageConfig pkg;
    pkg.displayName = "Bad/Name";

    WindowsBuildParams params;
    params.projectName = "testapp";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "bad.exe").string();
    params.archStr = "x64";

    EXPECT_THROWS(buildWindowsPackage(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(WindowsPackageBuilder, RejectsMissingAsset) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_packaging_windows_missing_asset_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);
    {
        std::ofstream exe(tmpRoot / "app.exe", std::ios::binary);
        exe.write("MZstub", 6);
    }

    PackageConfig pkg;
    pkg.displayName = "Test App";
    pkg.assets.push_back({"missing", "data"});

    WindowsBuildParams params;
    params.projectName = "testapp";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app.exe").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "bad.exe").string();
    params.archStr = "x64";

    EXPECT_THROWS(buildWindowsPackage(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(LinuxPackageBuilder, TarballDoesNotValidateDebianOnlyMetadata) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_tarball_metadata_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);
    {
        std::ofstream exe(tmpRoot / "app", std::ios::binary);
        exe << "stub";
    }

    PackageConfig pkg;
    pkg.displayName = "Portable App";
    pkg.category = "DefinitelyNotARegisteredCategory";
    pkg.depends.push_back("BadPackage (> 1)");

    LinuxBuildParams params;
    params.projectName = "portableapp";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "portableapp.tar.gz").string();
    params.archStr = "not-a-deb-arch";

    buildTarball(params);
    const auto tarGz = readFile(params.outputPath);
    std::ostringstream err;
    EXPECT_TRUE(verifyTarGzPayload(tarGz, {"portableapp-1.0.0/portableapp"}, err));
    fs::remove_all(tmpRoot);
}

TEST(LinuxPackageBuilder, TarballNormalizesDebianEpochVersionInTopDirectory) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_tarball_epoch_version";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);
    {
        std::ofstream exe(tmpRoot / "app", std::ios::binary);
        exe << "stub";
    }

    LinuxBuildParams params;
    params.projectName = "portableapp";
    params.version = "2:1.0~beta+1";
    params.executablePath = (tmpRoot / "app").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig.displayName = "Portable App";
    params.outputPath = (tmpRoot / "portableapp.tar.gz").string();
    params.archStr = "x64";

    buildTarball(params);
    const auto tarGz = readFile(params.outputPath);
    std::ostringstream err;
    EXPECT_TRUE(verifyTarGzPayload(tarGz, {"portableapp-2_1.0~beta+1/portableapp"}, err));
    fs::remove_all(tmpRoot);
}

#if !defined(_WIN32)
TEST(LinuxPackageBuilder, TarballSingleFileSymlinkAssetKeepsLogicalName) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_tarball_symlink_asset";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot / "assets");
    {
        std::ofstream exe(tmpRoot / "app", std::ios::binary);
        exe << "stub";
    }
    {
        std::ofstream real(tmpRoot / "assets" / "real.txt");
        real << "payload";
    }
    fs::create_symlink("real.txt", tmpRoot / "assets" / "link.txt");

    PackageConfig pkg;
    pkg.displayName = "Portable App";
    pkg.assets.push_back({"assets/link.txt", "data"});

    LinuxBuildParams params;
    params.projectName = "portableapp";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "portableapp.tar.gz").string();
    params.archStr = "x64";

    buildTarball(params);
    const auto tarBytes = inflateGzipPayload(readFile(params.outputPath));
    std::vector<uint8_t> data;
    EXPECT_TRUE(tarEntryData(tarBytes, "portableapp-1.0.0/data/link.txt", data));
    EXPECT_EQ(std::string(data.begin(), data.end()), std::string("payload"));
    std::vector<uint8_t> unexpected;
    EXPECT_FALSE(tarEntryData(tarBytes, "portableapp-1.0.0/data/real.txt", unexpected));
    fs::remove_all(tmpRoot);
}
#endif

TEST(LinuxPackageBuilder, TarballRejectsDuplicateAssetOutputPath) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_tarball_dup_asset_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot / "a");
    fs::create_directories(tmpRoot / "b");
    {
        std::ofstream exe(tmpRoot / "app", std::ios::binary);
        exe << "stub";
    }
    {
        std::ofstream file(tmpRoot / "a" / "config.txt");
        file << "one";
    }
    {
        std::ofstream file(tmpRoot / "b" / "config.txt");
        file << "two";
    }

    PackageConfig pkg;
    pkg.displayName = "Portable App";
    pkg.assets.push_back({"a/config.txt", "data"});
    pkg.assets.push_back({"b/config.txt", "data"});

    LinuxBuildParams params;
    params.projectName = "portableapp";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "portableapp.tar.gz").string();
    params.archStr = "x64";

    EXPECT_THROWS(buildTarball(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(LinuxPackageBuilder, DebPreservesHiddenMimeDesktopEntryAndEmptyAssetDir) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_deb_mime_empty_dir";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot / "empty-assets");
    {
        std::ofstream exe(tmpRoot / "app", std::ios::binary);
        exe << "stub";
    }

    PackageConfig pkg;
    pkg.displayName = "Empty App";
    pkg.shortcutMenu = false;
    pkg.shortcutDesktop = false;
    pkg.assets.push_back({"empty-assets", "data/empty"});
    pkg.fileAssociations.push_back({".zia", "Zia Source", "text/x-zia", ""});

    LinuxBuildParams params;
    params.projectName = "emptyapp";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig = pkg;
    params.outputPath = (tmpRoot / "emptyapp.deb").string();
    params.archStr = "amd64";

    buildDebPackage(params);
    const auto deb = readFile(params.outputPath);
    std::ostringstream err;
    EXPECT_TRUE(verifyDebPayload(deb,
                                 {"usr/bin/emptyapp",
                                  "usr/share/applications/emptyapp.desktop",
                                  "usr/share/mime/packages/emptyapp.xml",
                                  "usr/share/emptyapp/data/empty"},
                                 err));
    fs::remove_all(tmpRoot);
}

TEST(LinuxPackageBuilder, DebPreservesExecutableAssetMode) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_packaging_deb_asset_mode";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot / "scripts");
    {
        std::ofstream exe(tmpRoot / "app", std::ios::binary);
        exe << "stub";
    }
    {
        std::ofstream script(tmpRoot / "scripts" / "helper.sh");
        script << "#!/bin/sh\n";
    }
    fs::permissions(tmpRoot / "scripts" / "helper.sh",
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                        fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                        fs::perms::others_exec,
                    fs::perm_options::replace);

    LinuxBuildParams params;
    params.projectName = "assetmode";
    params.version = "1.0.0";
    params.executablePath = (tmpRoot / "app").string();
    params.projectRoot = tmpRoot.string();
    params.pkgConfig.displayName = "Asset Mode";
    params.pkgConfig.assets.push_back({"scripts", "tools"});
    params.outputPath = (tmpRoot / "assetmode.deb").string();
    params.archStr = "amd64";

    buildDebPackage(params);
    const auto dataTar = debDataTar(readFile(params.outputPath));
    uint32_t mode = 0;
    EXPECT_TRUE(tarEntryMode(dataTar, "usr/share/assetmode/tools/helper.sh", mode));
    EXPECT_EQ(mode, static_cast<uint32_t>(0755));
    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, GatherAndValidateMockStage) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_manifest_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);

    const auto manifest = gatherToolchainInstallManifest(stage);
    EXPECT_EQ(manifest.version, std::string("9.8.7"));
    EXPECT_EQ(manifest.productVersion, std::string("9.8.7-snapshot"));
    EXPECT_EQ(manifest.snapshot, std::string("9.8.7-0-g0123456789ab"));
    EXPECT_EQ(manifest.sourceCommit, std::string("0123456789abcdef0123456789abcdef01234567"));
    EXPECT_EQ(manifest.sourceState, std::string("clean"));
    EXPECT_TRUE(manifest.platform == "windows" || manifest.platform == "macos" ||
                manifest.platform == "linux");
    EXPECT_TRUE(manifest.totalSizeBytes() > 0);
    EXPECT_TRUE(std::any_of(
        manifest.files.begin(), manifest.files.end(), [](const ToolchainFileEntry &entry) {
            return entry.kind == ToolchainFileKind::Binary &&
                   entry.stagedRelativePath.find("bin/") == 0;
        }));
    EXPECT_TRUE(std::any_of(
        manifest.files.begin(), manifest.files.end(), [](const ToolchainFileEntry &entry) {
            return entry.kind == ToolchainFileKind::CMakeConfig &&
                   entry.stagedRelativePath == "lib/cmake/Zanna/ZannaTargets.cmake";
        }));
    for (const std::string &binary : requiredToolchainBinaryNames()) {
        EXPECT_TRUE(std::any_of(
            manifest.files.begin(), manifest.files.end(), [&](const ToolchainFileEntry &entry) {
                return entry.kind == ToolchainFileKind::Binary &&
                       entry.stagedRelativePath.find("bin/" + binary) == 0;
            }));
    }

    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, PreservesUtf8StagePaths) {
    namespace fs = std::filesystem;
    const std::string rootName = "zanna_manifest_\xE6\x9D\xB1\xE4\xBA\xAC";
    const std::string relativePath = "share/zanna/\xE8\xB3\x87\xE6\x96\x99.txt";
    const fs::path tmpRoot = fs::temp_directory_path() / zanna::filesystem::pathFromUtf8(rootName);
    fs::remove_all(tmpRoot);
    // Manifest gathering canonicalizes the stage root, so derive the expected
    // absolute path from the canonical form (macOS tmp lives behind /var ->
    // /private/var, and the raw form would never compare equal).
    const fs::path stage = fs::weakly_canonical(createMockToolchainStage(tmpRoot));
    const fs::path extra = stage / zanna::filesystem::pathFromUtf8(relativePath);
    fs::create_directories(extra.parent_path());
    writeFileAtomic(extra, std::vector<uint8_t>{'d', 'a', 't', 'a'});

    const auto manifest = gatherToolchainInstallManifest(stage);
    EXPECT_TRUE(std::any_of(
        manifest.files.begin(), manifest.files.end(), [&](const ToolchainFileEntry &entry) {
            return entry.stagedRelativePath == relativePath && entry.stagedAbsolutePath == extra;
        }));

    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, RequiresAllToolchainBinaries) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_manifest_no_tool";
    for (const std::string &binary : requiredToolchainBinaryNames()) {
        fs::remove_all(tmpRoot);
        const fs::path stage = createMockToolchainStage(tmpRoot);
        for (const auto &entry : fs::directory_iterator(stage / "bin")) {
            const std::string filename = entry.path().filename().string();
            if (filename == binary || filename == binary + ".exe") {
                fs::remove(entry.path());
                break;
            }
        }
        EXPECT_THROWS(gatherToolchainInstallManifest(stage), std::runtime_error);
    }
    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, RequiresDetectedVersion) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_manifest_no_version";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    fs::remove(stage / "lib" / "cmake" / "Zanna" / "ZannaConfigVersion.cmake");
    fs::remove(stage / "include" / "zanna" / "version.hpp");

    EXPECT_THROWS(gatherToolchainInstallManifest(stage), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, AllowsUniversalOnlyForMacOS) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_manifest_universal";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    auto manifest = gatherToolchainInstallManifest(stage);

    manifest.platform = "macos";
    manifest.arch = "universal";
    EXPECT_NO_THROW(validateToolchainInstallManifest(manifest));

    manifest.platform = "linux";
    EXPECT_THROWS(validateToolchainInstallManifest(manifest), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, ParsesFat32AndFat64MachOSlices) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_manifest_fat_macho";
    for (const bool fat64 : {false, true}) {
        fs::remove_all(tmpRoot);
        const fs::path stage = createMockToolchainStage(tmpRoot);
        writeBytes(mockToolchainBinaryPath(stage, "zanna"),
                   makeSyntheticFatMachO(fat64, {0x01000007, 0x0100000c}));

        const auto manifest = gatherToolchainInstallManifest(stage);
        EXPECT_EQ(manifest.platform, std::string("macos"));
        EXPECT_EQ(manifest.arch, std::string("universal"));
    }
    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, DoesNotMislabelSingleSliceFatMachOAsUniversal) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_manifest_fat_single";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    writeBytes(mockToolchainBinaryPath(stage, "zanna"), makeSyntheticFatMachO(false, {0x01000007}));

    const auto manifest = gatherToolchainInstallManifest(stage);
    EXPECT_EQ(manifest.platform, std::string("macos"));
    EXPECT_EQ(manifest.arch, std::string("x64"));
    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, RejectsMismatchedMacOSPayloadArchitecture) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_manifest_macho_mismatch";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    writeBytes(mockToolchainBinaryPath(stage, "zanna"),
               makeSyntheticFatMachO(false, {0x01000007, 0x0100000c}));
    std::vector<uint8_t> thinX64(32, 0);
    thinX64[0] = 0xCF;
    thinX64[1] = 0xFA;
    thinX64[2] = 0xED;
    thinX64[3] = 0xFE;
    writeLE32(thinX64.data() + 4, 0x01000007);
    writeBytes(mockToolchainBinaryPath(stage, "zia"), thinX64);

    EXPECT_THROWS(gatherToolchainInstallManifest(stage), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, RejectsFatMachOSliceOutsideFile) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_manifest_fat_bounds";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    auto malformed = makeSyntheticFatMachO(false, {0x01000007, 0x0100000c});
    malformed.resize(48);
    writeBytes(mockToolchainBinaryPath(stage, "zanna"), malformed);

    EXPECT_THROWS(gatherToolchainInstallManifest(stage), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, InstallPathMappingPreservesRelativeLayout) {
    ToolchainFileEntry file;
    file.kind = ToolchainFileKind::CMakeConfig;
    file.stagedRelativePath = "lib/cmake/Zanna/ZannaConfig.cmake";

    EXPECT_EQ(mapInstallPath(file, InstallPathPolicy::PortableArchive),
              std::string("lib/cmake/Zanna/ZannaConfig.cmake"));
    EXPECT_EQ(mapInstallPath(file, InstallPathPolicy::MacOSUsrLocalZannaRoot),
              std::string("/usr/local/zanna/lib/cmake/Zanna/ZannaConfig.cmake"));
    EXPECT_EQ(mapInstallPath(file, InstallPathPolicy::LinuxUsrRoot),
              std::string("/usr/lib/cmake/Zanna/ZannaConfig.cmake"));
    EXPECT_EQ(mapInstallPath(file, InstallPathPolicy::WindowsProgramFilesRoot),
              std::string("C:\\Program Files\\Zanna\\lib\\cmake\\Zanna\\ZannaConfig.cmake"));

    ToolchainFileEntry doc;
    doc.kind = ToolchainFileKind::Doc;
    doc.stagedRelativePath = "LICENSE";
    EXPECT_EQ(mapInstallPath(doc, InstallPathPolicy::LinuxUsrRoot),
              std::string("/usr/share/doc/zanna/LICENSE"));
}

TEST(ToolchainInstallManifest, ValidationAcceptsCaseVariantCMakeConfigPath) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_manifest_cmake_case";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    auto manifest = gatherToolchainInstallManifest(stage);
    for (auto &entry : manifest.files) {
        if (entry.stagedRelativePath == "lib/cmake/Zanna/ZannaConfig.cmake")
            entry.stagedRelativePath = "lib/cmake/zanna/ZannaConfig.cmake";
        else if (entry.stagedRelativePath == "lib/cmake/Zanna/ZannaTargets.cmake")
            entry.stagedRelativePath = "lib/cmake/zanna/ZannaTargets.cmake";
    }
    EXPECT_NO_THROW(validateToolchainInstallManifest(manifest));
    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, TotalSizeDetectsOverflow) {
    ToolchainInstallManifest manifest;
    ToolchainFileEntry a;
    a.sizeBytes = std::numeric_limits<uint64_t>::max();
    ToolchainFileEntry b;
    b.sizeBytes = 1;
    manifest.files = {a, b};
    EXPECT_THROWS(manifest.totalSizeBytes(), std::overflow_error);
}

TEST(ToolchainInstallManifest, RejectsInvalidArchitecture) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_manifest_bad_arch";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.arch = "amd64";

    EXPECT_THROWS(validateToolchainInstallManifest(manifest), std::runtime_error);
    LinuxToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "bad.deb").string();
    EXPECT_THROWS(buildToolchainDebPackage(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, RequiresOptionalLibraryOnlyWhenStagedMetadataReferencesIt) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_manifest_optional_refs";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    {
        std::ofstream targets(stage / "lib" / "cmake" / "Zanna" / "ZannaTargets.cmake",
                              std::ios::app);
        targets << "zannagfx\n";
    }
    std::error_code ec;
    fs::remove(stage / "lib" / "libzannagfx.a", ec);
    ec.clear();
    fs::remove(stage / "lib" / "zannagfx.lib", ec);

    EXPECT_THROWS(gatherToolchainInstallManifest(stage), std::runtime_error);
    fs::remove_all(tmpRoot);
}

#if !defined(_WIN32)
TEST(ToolchainInstallManifest, RejectsSymlinkEscapingStage) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_manifest_symlink_escape";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    {
        std::ofstream outside(tmpRoot / "outside.txt");
        outside << "outside";
    }
    fs::create_symlink(tmpRoot / "outside.txt", stage / "share" / "doc" / "zanna" / "escape.txt");

    EXPECT_THROWS(gatherToolchainInstallManifest(stage), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, PreservesInternalSymlinkTargets) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_toolchain_manifest_symlink_internal";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    fs::create_symlink("../share/doc/zanna/README.md", stage / "bin" / "zanna-readme");

    const auto manifest = gatherToolchainInstallManifest(stage);
    auto it = std::find_if(
        manifest.files.begin(), manifest.files.end(), [](const ToolchainFileEntry &entry) {
            return entry.stagedRelativePath == "bin/zanna-readme";
        });
    ASSERT_TRUE(it != manifest.files.end());
    EXPECT_TRUE(it->symlink);
    EXPECT_EQ(it->symlinkTarget, std::string("../share/doc/zanna/README.md"));
    fs::remove_all(tmpRoot);
}

TEST(ToolchainInstallManifest, HandlesSymlinkedStageDirWithAbsoluteInstallManifest) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_manifest_stage_alias";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    const fs::path stageLink = tmpRoot / "stage-link";
    fs::create_directory_symlink(stage, stageLink);
    const fs::path installManifest = tmpRoot / "install_manifest.txt";
    {
        std::ofstream out(installManifest);
        for (fs::recursive_directory_iterator it(stageLink);
             it != fs::recursive_directory_iterator();
             ++it) {
            if (it->is_regular_file() || it->is_symlink()) {
                const fs::path rel = it->path().lexically_relative(stageLink);
                out << (stageLink / rel).string() << "\n";
            }
        }
    }

    const auto manifest = gatherToolchainInstallManifest(stageLink, installManifest);
    EXPECT_TRUE(std::any_of(
        manifest.files.begin(), manifest.files.end(), [](const ToolchainFileEntry &entry) {
            return entry.stagedRelativePath == "bin/zanna" ||
                   entry.stagedRelativePath == "bin/zanna.exe";
        }));
    EXPECT_TRUE(std::any_of(
        manifest.files.begin(), manifest.files.end(), [](const ToolchainFileEntry &entry) {
            return entry.stagedRelativePath == "lib/cmake/Zanna/ZannaConfig.cmake";
        }));
    fs::remove_all(tmpRoot);
}

TEST(ToolchainWindowsPackageBuilder, RejectsDirectorySymlinkDereference) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_windows_dir_symlink";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    fs::create_directory_symlink("../share/doc", stage / "bin" / "doc-link");
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.arch = "x64";
    manifest.platform = "windows";

    WindowsToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "bad.exe").string();
    params.archStr = "x64";
    EXPECT_THROWS(buildWindowsToolchainInstaller(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}
#endif

TEST(ToolchainLinuxPackageBuilder, BuildsDebFromManifest) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_deb_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    normalizeMockStageForLinuxToolchain(stage);
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.platform = "linux";

    LinuxToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna_9.8.7_amd64.deb").string();
    buildToolchainDebPackage(params);

    const auto debBytes = readFile(params.outputPath);
    std::ostringstream err;
    EXPECT_TRUE(verifyDeb(debBytes, err));
    std::ostringstream payloadErr;
    std::vector<std::string> requiredDebPayload;
    for (const std::string &binary : requiredToolchainBinaryNames())
        requiredDebPayload.push_back("usr/bin/" + binary);
    requiredDebPayload.push_back("usr/share/applications/zannastudio.desktop");
    requiredDebPayload.push_back("usr/share/applications/zanna-source.desktop");
    requiredDebPayload.push_back("usr/share/applications/zanna-il.desktop");
    requiredDebPayload.push_back("usr/share/icons/hicolor/256x256/apps/zanna.png");
    requiredDebPayload.push_back("usr/share/mime/packages/zanna.xml");
    requiredDebPayload.push_back("usr/share/doc/zanna/LICENSE");
    EXPECT_TRUE(verifyDebPayload(debBytes, requiredDebPayload, payloadErr));
    const std::string control = debControlText(debBytes);
    EXPECT_CONTAINS(control, "Depends: libc6, libstdc++6 | libc++1, libgcc-s1");
    EXPECT_CONTAINS(control,
                    "Recommends: cmake, g++ | clang, make, desktop-file-utils, "
                    "shared-mime-info, man-db");
#if ZANNA_BUILD_HAS_GRAPHICS || ZANNA_BUILD_HAS_GUI
    EXPECT_CONTAINS(control, "libx11-6");
#endif
#if ZANNA_BUILD_HAS_AUDIO
    EXPECT_CONTAINS(control, "libasound2 | libasound2t64");
#endif
    const std::string postrm = debControlEntryText(debBytes, "postrm");
    EXPECT_CONTAINS(postrm, "mandb");
    EXPECT_CONTAINS(postrm, "update-mime-database");
    fs::remove_all(tmpRoot);
}

// Overwrite the staged primary binary with a synthetic ELF whose body names @p soname, so the
// C++-runtime detector has something concrete to scan. The header is a complete ELF64
// (little-endian, x86-64) so the manifest's architecture detector also accepts it.
static void writeSyntheticElfBinary(const std::filesystem::path &stage, const std::string &soname) {
    namespace fs = std::filesystem;
    fs::path binPath = stage / "bin" / "zanna";
    if (!fs::exists(binPath))
        binPath = stage / "bin" / "zanna.exe";
    unsigned char header[64] = {0};
    header[0] = 0x7F;
    header[1] = 'E';
    header[2] = 'L';
    header[3] = 'F';
    header[4] = 2;   // EI_CLASS = ELFCLASS64
    header[5] = 1;   // EI_DATA  = ELFDATA2LSB
    header[6] = 1;   // EI_VERSION = EV_CURRENT
    header[16] = 3;  // e_type = ET_DYN
    header[18] = 62; // e_machine = EM_X86_64
    header[20] = 1;  // e_version = EV_CURRENT
    std::ofstream out(binPath, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(header), sizeof(header));
    const std::string body = std::string(16, '\0') + soname + std::string(8, '\0');
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

TEST(ToolchainLinuxPackageBuilder, DebUsesConfiguredMaintainerAndHomepage) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_deb_maintainer";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.platform = "linux";
    manifest.maintainer = "Acme Tools";
    manifest.maintainerEmail = "dev@acme.test";
    manifest.homepage = "https://zanna.example.com";

    LinuxToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna_9.8.7_amd64.deb").string();
    buildToolchainDebPackage(params);

    const std::string control = debControlText(readFile(params.outputPath));
    EXPECT_CONTAINS(control, "Maintainer: Acme Tools <dev@acme.test>");
    EXPECT_CONTAINS(control, "Homepage: https://zanna.example.com");
    fs::remove_all(tmpRoot);
}

TEST(ToolchainLinuxPackageBuilder, DebDefaultsToProjectContactAndHomepage) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_deb_default_maint";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.platform = "linux";

    LinuxToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna_9.8.7_amd64.deb").string();
    buildToolchainDebPackage(params);

    const std::string control = debControlText(readFile(params.outputPath));
    EXPECT_CONTAINS(control, "Maintainer: Zanna Project <splanck@users.noreply.github.com>");
    EXPECT_CONTAINS(control, "Homepage: https://github.com/zannagames/zanna");
    fs::remove_all(tmpRoot);
}

TEST(ToolchainLinuxPackageBuilder, DebNarrowsDependsToLibstdcxxForElf) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_deb_libstdcxx";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    writeSyntheticElfBinary(stage, "libstdc++.so.6");
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.platform = "linux";

    LinuxToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna_9.8.7_amd64.deb").string();
    buildToolchainDebPackage(params);

    const std::string control = debControlText(readFile(params.outputPath));
    EXPECT_CONTAINS(control, "Depends: libc6, libstdc++6, libgcc-s1");
    fs::remove_all(tmpRoot);
}

TEST(ToolchainLinuxPackageBuilder, DebNarrowsDependsToLibcxxForElf) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_deb_libcxx";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    writeSyntheticElfBinary(stage, "libc++.so.1");
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.platform = "linux";

    LinuxToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna_9.8.7_amd64.deb").string();
    buildToolchainDebPackage(params);

    const std::string control = debControlText(readFile(params.outputPath));
    EXPECT_CONTAINS(control, "Depends: libc6, libc++1, libgcc-s1");
    fs::remove_all(tmpRoot);
}

#if defined(__APPLE__)
TEST(MacOSToolchainDmgBuilder, WrapsPkgIntoValidUdifImage) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_dmg_builder";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);
    const fs::path pkgPath = tmpRoot / "fake.pkg";
    {
        std::ofstream out(pkgPath, std::ios::binary);
        out << "fake pkg payload";
    }

    MacOSToolchainDmgParams params;
    params.pkgPath = pkgPath.string();
    params.volumeName = "Zanna Toolchain Test";
    params.outputPath = pkgPath.string();
    EXPECT_THROWS(buildMacOSToolchainDmg(params), std::runtime_error);
    params.outputPath = (tmpRoot / "zanna.dmg").string();
    buildMacOSToolchainDmg(params);

    ASSERT_TRUE(fs::is_regular_file(params.outputPath));
    const auto bytes = readFile(params.outputPath);
    ASSERT_GE(bytes.size(), static_cast<size_t>(512));
    // UDIF disk images end with a 512-byte trailer whose magic is "koly".
    const size_t off = bytes.size() - 512;
    EXPECT_TRUE(bytes[off] == 'k' && bytes[off + 1] == 'o' && bytes[off + 2] == 'l' &&
                bytes[off + 3] == 'y');
    fs::remove_all(tmpRoot);
}
#endif

TEST(ToolchainLinuxPackageBuilder, BuildsTarballFromManifest) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_tar_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    {
        const fs::path helperPath = stage / "share" / "doc" / "zanna" / "helper.sh";
        std::ofstream helper(helperPath);
        helper << "#!/bin/sh\n";
        helper.close();
        fs::permissions(helperPath,
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                            fs::perms::group_read | fs::perms::group_exec,
                        fs::perm_options::replace);
    }
    normalizeMockStageForLinuxToolchain(stage);
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.platform = "linux";
    const auto helperEntry =
        std::find_if(manifest.files.begin(), manifest.files.end(), [](const auto &file) {
            return file.stagedRelativePath == "share/doc/zanna/helper.sh";
        });
    ASSERT_TRUE(helperEntry != manifest.files.end());
    helperEntry->unixMode = 0100750u;

    LinuxToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-9.8.7-linux-x64.tar.gz").string();
    buildToolchainTarball(params);

    const auto tarGz = readFile(params.outputPath);
    ASSERT_TRUE(tarGz.size() >= 2);
    EXPECT_EQ(tarGz[0], static_cast<uint8_t>(0x1F));
    EXPECT_EQ(tarGz[1], static_cast<uint8_t>(0x8B));
    const auto tarBytes = inflateGzipPayload(tarGz);
    const std::string topDir =
        std::string("zanna-9.8.7-") + manifest.platform + "-" + manifest.arch + "/";
    EXPECT_EQ(tarFirstEntryName(tarBytes), topDir);
    std::ostringstream payloadErr;
    std::vector<std::string> requiredTarballPayload;
    for (const std::string &binary : requiredToolchainBinaryNames())
        requiredTarballPayload.push_back(topDir + "bin/" + binary);
    requiredTarballPayload.push_back(topDir + "share/applications/zannastudio.desktop");
    requiredTarballPayload.push_back(topDir + "share/applications/zanna-source.desktop");
    requiredTarballPayload.push_back(topDir + "share/applications/zanna-il.desktop");
    requiredTarballPayload.push_back(topDir + "share/icons/hicolor/256x256/apps/zanna.png");
    requiredTarballPayload.push_back(topDir + "share/mime/packages/zanna.xml");
    requiredTarballPayload.push_back(topDir + "share/zanna/install_manifest.txt");
    requiredTarballPayload.push_back(topDir + "install.sh");
    requiredTarballPayload.push_back(topDir + "uninstall.sh");
    requiredTarballPayload.push_back(topDir + "README.install");
    EXPECT_TRUE(verifyTarGzPayload(tarGz, requiredTarballPayload, payloadErr));
    std::vector<uint8_t> installScript;
    ASSERT_TRUE(tarEntryData(tarBytes, topDir + "install.sh", installScript));
    EXPECT_CONTAINS(std::string(installScript.begin(), installScript.end()), "DESTDIR");
    EXPECT_CONTAINS(std::string(installScript.begin(), installScript.end()),
                    "validate_manifest_relpath");
    EXPECT_CONTAINS(std::string(installScript.begin(), installScript.end()), "--dry-run");
    EXPECT_CONTAINS(std::string(installScript.begin(), installScript.end()), "--force");
    EXPECT_CONTAINS(std::string(installScript.begin(), installScript.end()), "rollback_install");
    EXPECT_CONTAINS(std::string(installScript.begin(), installScript.end()),
                    "refusing to replace unowned path");
    uint32_t installMode = 0;
    EXPECT_TRUE(tarEntryMode(tarBytes, topDir + "install.sh", installMode));
    EXPECT_EQ(installMode, static_cast<uint32_t>(0755));
    std::vector<uint8_t> uninstallScript;
    ASSERT_TRUE(tarEntryData(tarBytes, topDir + "uninstall.sh", uninstallScript));
    EXPECT_CONTAINS(std::string(uninstallScript.begin(), uninstallScript.end()),
                    "install_manifest.txt");
    EXPECT_CONTAINS(std::string(uninstallScript.begin(), uninstallScript.end()),
                    "check_no_symlink_parents \"$destination\"");
    EXPECT_CONTAINS(std::string(uninstallScript.begin(), uninstallScript.end()),
                    "rollback_uninstall");
    EXPECT_CONTAINS(std::string(uninstallScript.begin(), uninstallScript.end()),
                    "install_metadata");
    uint32_t uninstallMode = 0;
    EXPECT_TRUE(tarEntryMode(tarBytes, topDir + "uninstall.sh", uninstallMode));
    EXPECT_EQ(uninstallMode, static_cast<uint32_t>(0755));
    std::vector<uint8_t> installManifest;
    ASSERT_TRUE(
        tarEntryData(tarBytes, topDir + "share/zanna/install_manifest.txt", installManifest));
    const std::string installManifestText(installManifest.begin(), installManifest.end());
    EXPECT_CONTAINS(installManifestText, "bin/zanna\n");
    EXPECT_CONTAINS(installManifestText, "share/zanna/install_manifest.txt\n");
    EXPECT_CONTAINS(installManifestText, "share/zanna/install_metadata\n");
    EXPECT_CONTAINS(installManifestText, "share/zanna/uninstall.sh\n");
    std::vector<uint8_t> sourceDesktop;
    ASSERT_TRUE(
        tarEntryData(tarBytes, topDir + "share/applications/zanna-source.desktop", sourceDesktop));
    EXPECT_CONTAINS(std::string(sourceDesktop.begin(), sourceDesktop.end()), "Exec=zanna run %f");
    std::vector<uint8_t> ilDesktop;
    ASSERT_TRUE(tarEntryData(tarBytes, topDir + "share/applications/zanna-il.desktop", ilDesktop));
    EXPECT_CONTAINS(std::string(ilDesktop.begin(), ilDesktop.end()), "Exec=zanna -run %f");
    uint32_t helperMode = 0;
    EXPECT_TRUE(tarEntryMode(tarBytes, topDir + "share/doc/zanna/helper.sh", helperMode));
    EXPECT_EQ(helperMode, static_cast<uint32_t>(0750));
    fs::remove_all(tmpRoot);
}

TEST(ToolchainLinuxPackageBuilder, TarballScriptsProtectConflictsAndRemoveOwnedFiles) {
#if ZANNA_HOST_WINDOWS
    return;
#else
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_tar_lifecycle";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    {
        std::ofstream oldOnly(stage / "share" / "doc" / "zanna" / "old-only.txt");
        oldOnly << "removed by upgrade";
    }
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.platform = "linux";

    LinuxToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-toolchain.tar.gz").string();
    buildToolchainTarball(params);

    const fs::path extractRoot = tmpRoot / "extract";
    fs::create_directories(extractRoot);
    const RunResult extract =
        run_process({"tar", "xzf", params.outputPath, "-C", extractRoot.string()});
    ASSERT_EQ(extract.exit_code, 0);
    const fs::path topDir = extractRoot / ("zanna-9.8.7-linux-" + manifest.arch);
    const fs::path installScript = topDir / "install.sh";

    const fs::path canonicalRoot = fs::weakly_canonical(tmpRoot) / "target-root";
    fs::create_directories(canonicalRoot / "usr" / "bin");
    {
        std::ofstream conflict(canonicalRoot / "usr" / "bin" / "zanna");
        conflict << "unowned";
    }
    const std::vector<std::pair<std::string, std::string>> env = {
        {"DESTDIR", canonicalRoot.string()}, {"PREFIX", "/usr"}, {"NO_COLOR", "1"}};
    const RunResult conflict = run_process({installScript.string()}, topDir.string(), env);
    EXPECT_NE(conflict.exit_code, 0);
    EXPECT_CONTAINS(conflict.err, "refusing to replace unowned path");

    const RunResult install =
        run_process({installScript.string(), "--force"}, topDir.string(), env);
    if (install.exit_code != 0)
        std::cerr << install.out << install.err;
    ASSERT_EQ(install.exit_code, 0);
    EXPECT_TRUE(fs::is_regular_file(canonicalRoot / "usr" / "bin" / "zanna"));
    const fs::path uninstaller = canonicalRoot / "usr" / "share" / "zanna" / "uninstall.sh";
    EXPECT_TRUE(fs::is_regular_file(uninstaller));
    EXPECT_TRUE(
        fs::is_regular_file(canonicalRoot / "usr" / "share" / "zanna" / "install_metadata"));
    EXPECT_TRUE(
        fs::is_regular_file(canonicalRoot / "usr" / "share" / "doc" / "zanna" / "old-only.txt"));

    const fs::path unrelated = canonicalRoot / "usr" / "share" / "zanna" / "user.keep";
    {
        std::ofstream keep(unrelated);
        keep << "preserve me";
    }

    fs::remove(stage / "share" / "doc" / "zanna" / "old-only.txt");
    auto upgradeManifest = gatherToolchainInstallManifest(stage);
    upgradeManifest.platform = "linux";
    LinuxToolchainBuildParams upgradeParams;
    upgradeParams.manifest = upgradeManifest;
    upgradeParams.outputPath = (tmpRoot / "zanna-toolchain-upgrade.tar.gz").string();
    buildToolchainTarball(upgradeParams);
    const fs::path upgradeExtractRoot = tmpRoot / "upgrade-extract";
    fs::create_directories(upgradeExtractRoot);
    const RunResult extractUpgrade =
        run_process({"tar", "xzf", upgradeParams.outputPath, "-C", upgradeExtractRoot.string()});
    ASSERT_EQ(extractUpgrade.exit_code, 0);
    const fs::path upgradeTopDir =
        upgradeExtractRoot / ("zanna-9.8.7-linux-" + upgradeManifest.arch);
    const RunResult upgrade =
        run_process({(upgradeTopDir / "install.sh").string()}, upgradeTopDir.string(), env);
    if (upgrade.exit_code != 0)
        std::cerr << upgrade.out << upgrade.err;
    ASSERT_EQ(upgrade.exit_code, 0);
    EXPECT_FALSE(fs::exists(canonicalRoot / "usr" / "share" / "doc" / "zanna" / "old-only.txt"));
    EXPECT_TRUE(fs::is_regular_file(unrelated));

    const RunResult dryUninstall =
        run_process({uninstaller.string(), "--dry-run"}, upgradeTopDir.string(), env);
    EXPECT_EQ(dryUninstall.exit_code, 0);
    EXPECT_TRUE(fs::exists(canonicalRoot / "usr" / "bin" / "zanna"));

    const RunResult uninstall = run_process({uninstaller.string()}, upgradeTopDir.string(), env);
    if (uninstall.exit_code != 0)
        std::cerr << uninstall.out << uninstall.err;
    EXPECT_EQ(uninstall.exit_code, 0);
    EXPECT_FALSE(fs::exists(canonicalRoot / "usr" / "bin" / "zanna"));
    EXPECT_FALSE(fs::exists(uninstaller));
    EXPECT_TRUE(fs::is_regular_file(unrelated));
    fs::remove_all(tmpRoot);
#endif
}

TEST(LinuxRuntimeStubGen, BuildsVerifiableSelfExtractingLayout) {
    TarWriter tar;
    tar.addDirectory("./", 0755);
    tar.addFileString("AppRun", "#!/bin/sh\nexit 0\n", 0755);
    const auto tarBytes = tar.finish();
    const auto tarGz = gzip(tarBytes.data(), tarBytes.size());

    LinuxRuntimeStubParams params;
    params.cacheName = "zanna-test-linux-x64";
    params.entryPath = "AppRun";
    const auto appImage = buildLinuxAppImage(params, tarGz);

    std::string err;
    EXPECT_TRUE(verifyLinuxAppImage(appImage, &err));
    const std::string hashField = "payload_sha256='";
    EXPECT_TRUE(std::search(appImage.begin(),
                            appImage.end(),
                            reinterpret_cast<const uint8_t *>(hashField.data()),
                            reinterpret_cast<const uint8_t *>(hashField.data()) +
                                hashField.size()) != appImage.end());
    EXPECT_TRUE(containsAscii(appImage, "cache_key=$cache_name-$payload_sha256"));
    EXPECT_TRUE(containsAscii(appImage, "check_no_symlink_components"));
    EXPECT_TRUE(containsAscii(appImage, "while ! mkdir \"$lock_dir\""));
    EXPECT_TRUE(containsAscii(appImage, "ZANNA_BUNDLE_REFRESH"));
    EXPECT_TRUE(containsAscii(appImage, "--zanna-bundle-extract"));
    EXPECT_TRUE(containsAscii(appImage, "./zanna-bundle-root"));
    const auto payload = appImagePayloadBytes(appImage);
    ASSERT_TRUE(payload.size() >= 2);
    EXPECT_EQ(payload[0], static_cast<uint8_t>(0x1F));
    EXPECT_EQ(payload[1], static_cast<uint8_t>(0x8B));
    auto tamperedAppImage = appImage;
    const size_t payloadOffset = appImagePayloadOffset(tamperedAppImage);
    ASSERT_LT(payloadOffset, tamperedAppImage.size());
    tamperedAppImage[payloadOffset] ^= static_cast<uint8_t>(0x01);
    err.clear();
    EXPECT_FALSE(verifyLinuxAppImage(tamperedAppImage, &err));
    EXPECT_CONTAINS(err, "payload SHA-256 mismatch");
    LinuxRuntimeStubParams invalidNameParams;
    invalidNameParams.cacheName = "../bad";
    invalidNameParams.entryPath = "AppRun";
    EXPECT_THROWS(buildLinuxRuntimeStub(invalidNameParams), std::runtime_error);
    LinuxRuntimeStubParams invalidHashParams;
    invalidHashParams.cacheName = "zanna-test-linux-x64";
    invalidHashParams.entryPath = "AppRun";
    invalidHashParams.payloadSha256 = "not-a-sha256";
    EXPECT_THROWS(buildLinuxRuntimeStub(invalidHashParams), std::runtime_error);
    EXPECT_THROWS(buildLinuxAppImage(params, {0x1F, 0x8B, 0x00, 0x00}), std::runtime_error);
}

TEST(LinuxRuntimeStubGen, ExecutesFromContentAddressedPrivateCache) {
#if ZANNA_HOST_WINDOWS
    return;
#else
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_linux_bundle_runtime_test";
    fs::remove_all(tmpRoot);
    fs::create_directories(tmpRoot);

    TarWriter tar;
    tar.addDirectory("./", 0755);
    tar.addFileString("AppRun", "#!/bin/sh\nprintf 'bundle-ok\\n'\n", 0755);
    const auto tarBytes = tar.finish();
    const auto tarGz = gzip(tarBytes.data(), tarBytes.size());
    LinuxRuntimeStubParams params;
    params.cacheName = "zanna-runtime-test";
    params.entryPath = "AppRun";
    const std::vector<uint8_t> bundle = buildLinuxAppImage(params, tarGz);
    const fs::path bundlePath = tmpRoot / "zanna-runtime-test.run";
    writeBytes(bundlePath, bundle);
    fs::permissions(bundlePath,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);

    const fs::path cacheRoot = fs::weakly_canonical(tmpRoot) / "cache";
    const std::vector<std::pair<std::string, std::string>> env = {
        {"XDG_CACHE_HOME", cacheRoot.string()}, {"ZANNA_BUNDLE_QUIET", "1"}};
    const RunResult first = run_process({bundlePath.string()}, std::nullopt, env);
    EXPECT_EQ(first.exit_code, 0);
    EXPECT_CONTAINS(first.out, "bundle-ok");
    const RunResult second = run_process({bundlePath.string()}, std::nullopt, env);
    EXPECT_EQ(second.exit_code, 0);

    size_t stampCount = 0;
    for (const auto &entry : fs::recursive_directory_iterator(cacheRoot)) {
        if (entry.path().filename() == ".payload.sha256")
            ++stampCount;
    }
    EXPECT_EQ(stampCount, static_cast<size_t>(1));
    const RunResult cachePath =
        run_process({bundlePath.string(), "--zanna-bundle-cache"}, std::nullopt, env);
    EXPECT_EQ(cachePath.exit_code, 0);
    EXPECT_CONTAINS(cachePath.out, "zanna-runtime-test-");

    fs::create_directories(tmpRoot / "real-cache");
    fs::create_directory_symlink(tmpRoot / "real-cache", tmpRoot / "cache-link");
    const RunResult symlinked = run_process(
        {bundlePath.string()},
        std::nullopt,
        {{"XDG_CACHE_HOME", (tmpRoot / "cache-link").string()}, {"ZANNA_BUNDLE_QUIET", "1"}});
    EXPECT_NE(symlinked.exit_code, 0);
    EXPECT_CONTAINS(symlinked.err, "symlinked cache path component");

    fs::remove_all(tmpRoot);
#endif
}

TEST(ToolchainLinuxPackageBuilder, BuildsSelfExtractingBundleFromManifest) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_bundle_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    normalizeMockStageForLinuxToolchain(stage);
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.platform = "linux";

    LinuxToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-9.8.7-linux-x64.run").string();
    buildToolchainBundle(params);

    const auto bundle = readFile(params.outputPath);
    std::string err;
    EXPECT_TRUE(verifyLinuxAppImage(bundle, &err));
    const std::string runtimeText(bundle.begin(), bundle.end());
    EXPECT_TRUE(runtimeText.find("--zanna-bundle-help") != std::string::npos);
    EXPECT_TRUE(runtimeText.find("--appimage-help") == std::string::npos);
    EXPECT_TRUE(runtimeText.find("ZANNA_APPIMAGE_CLEAN_CACHE") == std::string::npos);
    const auto payloadGz = appImagePayloadBytes(bundle);
    const auto payloadTar = inflateGzipPayload(payloadGz);
    std::ostringstream payloadErr;
    std::vector<std::string> requiredBundlePayload;
    requiredBundlePayload.push_back("AppRun");
    for (const std::string &binary : requiredToolchainBinaryNames())
        requiredBundlePayload.push_back("bin/" + binary);
    requiredBundlePayload.push_back("share/applications/zannastudio.desktop");
    requiredBundlePayload.push_back("share/applications/zanna-source.desktop");
    requiredBundlePayload.push_back("share/applications/zanna-il.desktop");
    requiredBundlePayload.push_back("share/icons/hicolor/256x256/apps/zanna.png");
    requiredBundlePayload.push_back("share/mime/packages/zanna.xml");
    requiredBundlePayload.push_back(".DirIcon");
    requiredBundlePayload.push_back("zanna.desktop");
    requiredBundlePayload.push_back("zanna.png");
    EXPECT_TRUE(verifyTarGzPayload(payloadGz, requiredBundlePayload, payloadErr));
    std::vector<uint8_t> appRun;
    ASSERT_TRUE(tarEntryData(payloadTar, "AppRun", appRun));
    const std::string appRunText(appRun.begin(), appRun.end());
    EXPECT_CONTAINS(appRunText, "bin/zannastudio");
    EXPECT_CONTAINS(appRunText, "bin/zanna");
    std::vector<uint8_t> desktop;
    ASSERT_TRUE(tarEntryData(payloadTar, "zanna.desktop", desktop));
    EXPECT_CONTAINS(std::string(desktop.begin(), desktop.end()), "Exec=AppRun");
    EXPECT_CONTAINS(std::string(desktop.begin(), desktop.end()), "Terminal=false");
    std::vector<uint8_t> icon;
    ASSERT_TRUE(tarEntryData(payloadTar, "zanna.png", icon));
    ASSERT_TRUE(icon.size() >= 8);
    EXPECT_EQ(icon[0], static_cast<uint8_t>(137));
    EXPECT_EQ(icon[1], static_cast<uint8_t>('P'));
    EXPECT_EQ(icon[2], static_cast<uint8_t>('N'));
    EXPECT_EQ(icon[3], static_cast<uint8_t>('G'));
    std::error_code ec;
    const auto perms = fs::status(params.outputPath, ec).permissions();
    EXPECT_TRUE((perms & fs::perms::owner_exec) != fs::perms::none);
    fs::remove_all(tmpRoot);
}

TEST(ToolchainLinuxPackageBuilder, TarballNormalizesDebianEpochVersionInTopDirectory) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_tar_epoch_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    normalizeMockStageForLinuxToolchain(stage);
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.platform = "linux";
    manifest.version = "2:9.8.7";

    LinuxToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-epoch.tar.gz").string();
    buildToolchainTarball(params);

    const auto tarGz = readFile(params.outputPath);
    const auto tarBytes = inflateGzipPayload(tarGz);
    const std::string topDir =
        std::string("zanna-2_9.8.7-") + manifest.platform + "-" + manifest.arch + "/";
    EXPECT_EQ(tarFirstEntryName(tarBytes), topDir);
    std::ostringstream err;
    EXPECT_TRUE(verifyTarGzPayload(tarGz, {topDir + "bin/zanna"}, err));
    fs::remove_all(tmpRoot);
}

TEST(ToolchainLinuxPackageBuilder, NonLinuxTarballOmitsLinuxDesktopMetadata) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_tar_nonlinux_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.platform = "macos";

    LinuxToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-9.8.7-macos-x64.tar.gz").string();
    buildToolchainTarball(params);

    const auto tarGz = readFile(params.outputPath);
    const auto tarBytes = inflateGzipPayload(tarGz);
    const std::string topDir =
        std::string("zanna-9.8.7-") + manifest.platform + "-" + manifest.arch + "/";
    std::vector<uint8_t> ignored;
    EXPECT_FALSE(
        tarEntryData(tarBytes, topDir + "share/applications/zanna-source.desktop", ignored));
    EXPECT_FALSE(tarEntryData(tarBytes, topDir + "share/mime/packages/zanna.xml", ignored));
    fs::remove_all(tmpRoot);
}

TEST(ToolchainWindowsPackageBuilder, BuildsInstallerFromManifest) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_windows_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    normalizeMockStageForWindowsToolchain(stage);
    addMockWindowsToolchainBranding(stage);
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.arch = "x64";
    manifest.platform = "windows";
    const auto studioBuildInfo =
        std::find_if(manifest.files.begin(), manifest.files.end(), [](const auto &file) {
            return file.stagedRelativePath == "bin/zannastudio.buildinfo";
        });
    ASSERT_TRUE(studioBuildInfo != manifest.files.end());
    EXPECT_TRUE(studioBuildInfo->kind == ToolchainFileKind::Extra);
    EXPECT_FALSE(studioBuildInfo->executable);

    WindowsToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-toolchain-setup.exe").string();
    params.archStr = "x64";
    buildWindowsToolchainInstaller(params);

    const auto pe = readFile(params.outputPath);
    std::ostringstream err;
    EXPECT_TRUE(verifyPEZipOverlay(pe, err));
    std::ostringstream payloadErr;
    EXPECT_TRUE(verifyPEZipOverlayPayload(pe,
                                          {"meta/payload.zip",
                                           "meta/install_manifest.next",
                                           "meta/zanna_developer_prompt.lnk",
                                           "meta/zannastudio.lnk",
                                           "meta/manifest.sha256"},
                                          payloadErr));
    const auto outerZip = extractFirstZipOverlay(pe);
    EXPECT_FALSE(zipContainsEntries(outerZip, {"meta/zanna_vscode_extension.lnk"}));
    const auto licenseMetadata = extractZipEntry(outerZip, "meta/license.txt");
    const auto readmeMetadata = extractZipEntry(outerZip, "meta/readme.txt");
    const std::string licenseMetadataText(licenseMetadata.begin(), licenseMetadata.end());
    const std::string readmeMetadataText(readmeMetadata.begin(), readmeMetadata.end());
    EXPECT_CONTAINS(licenseMetadataText, "GPL");
    EXPECT_TRUE(licenseMetadataText.find("Sample-specific") == std::string::npos);
    EXPECT_CONTAINS(readmeMetadataText, "Zanna");
    EXPECT_TRUE(readmeMetadataText.find("Sample documentation") == std::string::npos);
    EXPECT_GE(peOptionalHeaderField64(pe, 72), static_cast<uint64_t>(0x200000));
    EXPECT_GE(peOptionalHeaderField64(pe, 80), static_cast<uint64_t>(0x100000));
    EXPECT_TRUE(containsUtf16LE(pe, "Environment"));
    EXPECT_TRUE(containsUtf16LEStringData(pe, "%LocalAppData%\\Zanna\\share\\zanna\\zanna.ico"));
    EXPECT_TRUE(containsAscii(pe, "asInvoker"));
    EXPECT_TRUE(containsUtf16LE(pe, "ZAPSOriginalPath"));
    EXPECT_TRUE(containsUtf16LE(pe, "ZAPSPathEntry"));
    EXPECT_TRUE(containsUtf16LE(pe, "bin\\zannastudio.exe"));
    EXPECT_TRUE(containsUtf16LE(pe, "share\\zanna\\zanna.ico"));
    EXPECT_TRUE(containsUtf16LEStringData(pe, "ZANNA_INSTALLER_SELF"));
    EXPECT_TRUE(containsUtf16LEStringData(pe, "ZANNA_INSTALLER_MODE"));
    EXPECT_TRUE(containsUtf16LEStringData(pe, "ZANNA_INSTALLER_LOG"));
    EXPECT_TRUE(containsUtf16LEStringData(pe, "ZANNA_INSTALLER_PROGRESS"));
    EXPECT_TRUE(containsUtf16LEStringData(pe, "ZANNA_INSTALLER_COMPONENT_ZANNASTUDIO"));
    EXPECT_TRUE(containsUtf16LEStringData(pe, "ZANNA_INSTALLER_COMPONENT_SAMPLES"));
    EXPECT_TRUE(containsUtf16LE(pe, "/no-zannastudio"));
    EXPECT_TRUE(containsUtf16LE(pe, "/no-samples"));
    EXPECT_TRUE(containsUtf16LE(pe, "msctls_progress32"));
    EXPECT_TRUE(containsUtf16LEStringData(pe, "ZANNA_INSTALLER_NATIVE_STAGE"));
    EXPECT_TRUE(containsUtf16LEStringData(pe, "PATH registration"));
    EXPECT_TRUE(containsUtf16LE(pe, "DarkMode_Explorer"));
    const std::string transactionScript = decodedEmbeddedPowerShellSource(pe);
    EXPECT_CONTAINS(transactionScript, "function BeginTransaction");
    EXPECT_CONTAINS(transactionScript, "function ExtractPayload");
    EXPECT_CONTAINS(transactionScript, "ZipFile]::OpenRead");
    EXPECT_CONTAINS(transactionScript, "$payloadComponentPrefixes");
    EXPECT_CONTAINS(transactionScript, "$payloadComponents");
    EXPECT_CONTAINS(transactionScript, "function PayloadComponent");
    EXPECT_CONTAINS(transactionScript, "share\\zanna\\samples\\");
    EXPECT_TRUE(transactionScript.find("share\\zanna\\samples\\hello\\main.zia") ==
                std::string::npos);
    EXPECT_CONTAINS(transactionScript, "Installing the selected developer tools");
    EXPECT_CONTAINS(transactionScript, "function LegacyInstallOwned");
    EXPECT_CONTAINS(transactionScript, "migrating generated legacy installation");
    EXPECT_CONTAINS(transactionScript, "WriteInstallerLog 'ERROR'");
    EXPECT_CONTAINS(transactionScript, "Zanna Developer Prompt.lnk");
    EXPECT_CONTAINS(transactionScript, "refusing to replace unowned path");
    EXPECT_TRUE(containsUtf16LEStringData(pe, "install-files"));
    EXPECT_TRUE(containsUtf16LEStringData(pe, "commit"));
    EXPECT_TRUE(containsUtf16LEStringData(pe, "rollback"));
    EXPECT_TRUE(containsUtf16LE(pe, "Software\\Classes\\.zia"));
    const std::string sendMessageImport = "SendMessageTimeoutW";
    EXPECT_TRUE(
        std::search(pe.begin(), pe.end(), sendMessageImport.begin(), sendMessageImport.end()) !=
        pe.end());
    const std::string shellFileOperationImport = "SHFileOperationW";
    EXPECT_TRUE(std::search(pe.begin(),
                            pe.end(),
                            shellFileOperationImport.begin(),
                            shellFileOperationImport.end()) != pe.end());
    const auto payloadZip = extractPeOverlayZipEntry(pe, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    const auto developerPrompt = extractZipEntry(payloadZip, "bin/zanna-dev.cmd");
    ASSERT_FALSE(developerPrompt.empty());
    const std::string developerPromptText(developerPrompt.begin(), developerPrompt.end());
    EXPECT_CONTAINS(developerPromptText, "for %%I in (\"%~dp0..\") do set \"ZANNA_HOME=%%~fI\"");
    EXPECT_CONTAINS(developerPromptText, "set \"Zanna_DIR=%ZANNA_HOME%\\lib\\cmake\\Zanna\"");
    EXPECT_CONTAINS(developerPromptText,
                    "set \"CMAKE_PREFIX_PATH=%ZANNA_HOME%;%CMAKE_PREFIX_PATH%\"");
    std::vector<std::string> requiredWindowsPayload;
    for (const auto &file : manifest.files) {
        requiredWindowsPayload.push_back(sanitizePackageRelativePath(
            file.stagedRelativePath, "windows toolchain test payload path"));
    }
    requiredWindowsPayload.push_back("bin/zanna-dev.cmd");
    requiredWindowsPayload.push_back("share/zanna/README.windows-prerequisites.txt");
    requiredWindowsPayload.push_back("share/zanna/zanna.ico");
    requiredWindowsPayload.push_back("bin/zannastudio.ico");
    requiredWindowsPayload.push_back("uninstall.exe");
    requiredWindowsPayload.push_back(".zanna-install-manifest.txt");
    EXPECT_TRUE(zipContainsEntries(payloadZip, requiredWindowsPayload));
    const auto zannaStudioIcon = extractZipEntry(payloadZip, "bin/zannastudio.ico");
    ASSERT_GE(zannaStudioIcon.size(), static_cast<size_t>(6));
    EXPECT_EQ(readLE16(zannaStudioIcon.data()), static_cast<uint16_t>(0));
    EXPECT_EQ(readLE16(zannaStudioIcon.data() + 2), static_cast<uint16_t>(1));
    EXPECT_GE(readLE16(zannaStudioIcon.data() + 4), static_cast<uint16_t>(4));
    const auto uninstaller = extractZipEntry(payloadZip, "uninstall.exe");
    ASSERT_FALSE(uninstaller.empty());
    std::ostringstream uninstallerErr;
    EXPECT_TRUE(verifyPE(uninstaller, uninstallerErr));
    EXPECT_TRUE(containsUtf16LEStringData(uninstaller, "ZANNA_INSTALLER_MODE"));
    EXPECT_TRUE(containsUtf16LEStringData(uninstaller, "uninstall-files"));
    EXPECT_TRUE(containsUtf16LEStringData(uninstaller, "org.zanna.toolchain"));
    EXPECT_TRUE(containsUtf16LEStringData(uninstaller, ".zanna-install-manifest.txt"));
    const std::string uninstallerScript = decodedEmbeddedPowerShellSource(uninstaller);
    EXPECT_CONTAINS(uninstallerScript, "function RestoreTransaction");
    EXPECT_CONTAINS(uninstallerScript, "if($mode -eq 'uninstall-files')");
    EXPECT_CONTAINS(uninstallerScript, "ZAPSShortcutPaths");
    EXPECT_CONTAINS(uninstallerScript, "selected payload files and shortcuts removed");
    fs::remove_all(tmpRoot);
}

TEST(ToolchainWindowsPackageBuilder, RejectsUnboundZannaStudioArtifacts) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_toolchain_windows_studio_provenance";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    normalizeMockStageForWindowsToolchain(stage);

    auto buildParams = [&]() {
        WindowsToolchainBuildParams params;
        params.manifest = gatherToolchainInstallManifest(stage);
        params.manifest.arch = "x64";
        params.manifest.platform = "windows";
        params.outputPath = (tmpRoot / "zanna-toolchain-setup.exe").string();
        params.archStr = "x64";
        return params;
    };

    fs::remove(stage / "bin" / "zannastudio.buildinfo");
    WindowsToolchainBuildParams missingMetadata = buildParams();
    EXPECT_THROWS(buildWindowsToolchainInstaller(missingMetadata), std::runtime_error);

    normalizeMockStageForWindowsToolchain(stage);
    {
        const fs::path metadata = stage / "bin" / "zannastudio.buildinfo";
        std::vector<uint8_t> bytes = readFile(metadata.string());
        const std::string marker = "SHA256: ";
        const auto position = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
        ASSERT_TRUE(position != bytes.end());
        const size_t hashOffset =
            static_cast<size_t>(std::distance(bytes.begin(), position)) + marker.size();
        ASSERT_LT(hashOffset, bytes.size());
        bytes[hashOffset] = bytes[hashOffset] == '0' ? '1' : '0';
        writeBytes(metadata, bytes);
    }
    WindowsToolchainBuildParams badHash = buildParams();
    EXPECT_THROWS(buildWindowsToolchainInstaller(badHash), std::runtime_error);

    normalizeMockStageForWindowsToolchain(stage);
    {
        const fs::path metadata = stage / "bin" / "zannastudio.buildinfo";
        std::vector<uint8_t> bytes = readFile(metadata.string());
        const std::string expected = "Zanna Studio 9.8.7-snapshot";
        const std::string stale = "Zanna Studio 9.8.6-snapshot";
        const auto position =
            std::search(bytes.begin(), bytes.end(), expected.begin(), expected.end());
        ASSERT_TRUE(position != bytes.end());
        std::copy(stale.begin(), stale.end(), position);
        writeBytes(metadata, bytes);
    }
    WindowsToolchainBuildParams staleVersion = buildParams();
    EXPECT_THROWS(buildWindowsToolchainInstaller(staleVersion), std::runtime_error);

    normalizeMockStageForWindowsToolchain(stage);
    refreshMockWindowsStudioBuildInfo(stage, "arm64");
    WindowsToolchainBuildParams wrongArchitecture = buildParams();
    EXPECT_THROWS(buildWindowsToolchainInstaller(wrongArchitecture), std::runtime_error);

    refreshMockWindowsStudioBuildInfo(stage, "x64");
    WindowsToolchainBuildParams nonExecutable = buildParams();
    const auto studio = std::find_if(nonExecutable.manifest.files.begin(),
                                     nonExecutable.manifest.files.end(),
                                     [](const ToolchainFileEntry &file) {
                                         return file.stagedRelativePath == "bin/zannastudio.exe";
                                     });
    ASSERT_TRUE(studio != nonExecutable.manifest.files.end());
    studio->executable = false;
    EXPECT_THROWS(buildWindowsToolchainInstaller(nonExecutable), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(ToolchainWindowsPackageBuilder, OmitsInstallerBootstrapExecutablesFromDeveloperPayload) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_toolchain_windows_bootstrap_filter_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    normalizeMockStageForWindowsToolchain(stage);
    writeTestWindowsPe(stage / "bin" / "zanna-installer-host.exe");
    writeTestWindowsPe(stage / "bin" / "zanna-installer-cleanup.exe");

    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.arch = "x64";
    manifest.platform = "windows";

    WindowsToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-toolchain-setup.exe").string();
    params.archStr = "x64";
    params.installDirName = "Zanna development";
    params.installerHostPath = (stage / "bin" / "zanna-installer-host.exe").string();
    params.installerCleanupPath = (stage / "bin" / "zanna-installer-cleanup.exe").string();
    buildWindowsToolchainInstaller(params);

    const auto setup = readFile(params.outputPath);
    std::ostringstream verifyError;
    EXPECT_TRUE(verifyWindowsNativeInstaller(setup, verifyError));
    const auto payloadZip = extractPeOverlayZipEntry(setup, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    EXPECT_TRUE(extractZipEntry(payloadZip, "bin/zanna-installer-host.exe").empty());
    EXPECT_TRUE(extractZipEntry(payloadZip, "bin/zanna-installer-cleanup.exe").empty());
    const auto prerequisites =
        extractZipEntry(payloadZip, "share/zanna/README.windows-prerequisites.txt");
    const std::string prerequisitesText(prerequisites.begin(), prerequisites.end());
    EXPECT_CONTAINS(prerequisitesText, "%LOCALAPPDATA%\\Programs\\Zanna development");
    EXPECT_CONTAINS(prerequisitesText, "%ProgramFiles%\\Zanna development");

    const auto metadataBytes = extractPeOverlayZipEntry(setup, "meta/installer-v2.txt");
    ASSERT_FALSE(metadataBytes.empty());
    const auto metadata = parseWindowsInstallerMetadata(
        std::string(reinterpret_cast<const char *>(metadataBytes.data()), metadataBytes.size()));
    for (const WindowsInstallerPayloadMetadata &file : metadata.payloadFiles) {
        EXPECT_NE(file.path, "bin/zanna-installer-host.exe");
        EXPECT_NE(file.path, "bin/zanna-installer-cleanup.exe");
    }
    fs::remove_all(tmpRoot);
}

TEST(ToolchainWindowsPackageBuilder, RejectsMissingAppLocalMsvcRuntime) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_toolchain_windows_missing_runtime_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    normalizeMockStageForWindowsToolchain(stage);
    writeTestWindowsPe(
        stage / "bin" / "zanna.exe",
        "x64",
        {{"vcruntime140.dll", {"runtime_entry"}}, {"msvcp140.dll", {"standard_library_entry"}}});

    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.arch = "x64";
    manifest.platform = "windows";

    WindowsToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-toolchain-setup.exe").string();
    params.archStr = "x64";
    EXPECT_THROWS(buildWindowsToolchainInstaller(params), std::runtime_error);

    writeTestWindowsPe(stage / "bin" / "vcruntime140.dll");
    writeTestWindowsPe(stage / "bin" / "msvcp140.dll");
    params.manifest = gatherToolchainInstallManifest(stage);
    params.manifest.arch = "x64";
    params.manifest.platform = "windows";
    EXPECT_NO_THROW(buildWindowsToolchainInstaller(params));
    fs::remove_all(tmpRoot);
}

TEST(ToolchainWindowsPackageBuilder, SignsAllOwnedPesAndPreservesMicrosoftRuntime) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_toolchain_windows_nested_signing_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    normalizeMockStageForWindowsToolchain(stage);
    writeTestWindowsPe(
        stage / "bin" / "zanna.exe", "x64", {{"vcruntime140.dll", {"runtime_entry"}}});
    writeTestWindowsPe(stage / "bin" / "zannastudio.exe");
    writeTestWindowsPe(stage / "bin" / "vcruntime140.dll");

    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.arch = "x64";
    manifest.platform = "windows";

    std::vector<std::string> signedNames;
    WindowsToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-toolchain-setup.exe").string();
    params.archStr = "x64";
    params.peSigner = [&](std::string_view logicalName, const std::vector<uint8_t> &input) {
        signedNames.emplace_back(logicalName);
        std::vector<uint8_t> output = input;
        const std::string marker = "ZAPS-TOOLCHAIN-SIGNED";
        output.insert(output.end(), marker.begin(), marker.end());
        return output;
    };

    buildWindowsToolchainInstaller(params);
    const auto pe = readFile(params.outputPath);
    const auto payloadZip = extractPeOverlayZipEntry(pe, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    EXPECT_TRUE(
        containsAscii(extractZipEntry(payloadZip, "bin/zanna.exe"), "ZAPS-TOOLCHAIN-SIGNED"));
    const auto installedStudio = extractZipEntry(payloadZip, "bin/zannastudio.exe");
    EXPECT_TRUE(containsAscii(installedStudio, "ZAPS-TOOLCHAIN-SIGNED"));
    const auto installedStudioInfo = extractZipEntry(payloadZip, "bin/zannastudio.buildinfo");
    const std::string installedStudioInfoText(installedStudioInfo.begin(),
                                              installedStudioInfo.end());
    EXPECT_CONTAINS(installedStudioInfoText, "Size: " + std::to_string(installedStudio.size()));
    EXPECT_CONTAINS(installedStudioInfoText,
                    "SHA256: " + sha256Hex(installedStudio.data(), installedStudio.size()));
    EXPECT_TRUE(
        containsAscii(extractZipEntry(payloadZip, "uninstall.exe"), "ZAPS-TOOLCHAIN-SIGNED"));
    EXPECT_FALSE(containsAscii(extractZipEntry(payloadZip, "bin/vcruntime140.dll"),
                               "ZAPS-TOOLCHAIN-SIGNED"));
    EXPECT_TRUE(std::find(signedNames.begin(), signedNames.end(), "bin/zanna.exe") !=
                signedNames.end());
    EXPECT_TRUE(std::find(signedNames.begin(), signedNames.end(), "bin/zannastudio.exe") !=
                signedNames.end());
    EXPECT_TRUE(std::find(signedNames.begin(), signedNames.end(), "uninstall.exe") !=
                signedNames.end());
    EXPECT_TRUE(std::find(signedNames.begin(), signedNames.end(), "bin/vcruntime140.dll") ==
                signedNames.end());
    fs::remove_all(tmpRoot);
}

TEST(ToolchainWindowsPackageBuilder, BuildsAndRecursivelyVerifiesNativeArm64Installer) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot =
        fs::temp_directory_path() / "zanna_toolchain_windows_arm64_license_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    normalizeMockStageForWindowsToolchain(stage);
    std::vector<fs::path> stagedExecutables;
    for (const auto &entry : fs::recursive_directory_iterator(stage)) {
        if (entry.is_regular_file() && entry.path().extension() == ".exe")
            stagedExecutables.push_back(entry.path());
    }
    for (const fs::path &executable : stagedExecutables)
        writeTestWindowsPe(executable, "arm64");
    refreshMockWindowsStudioBuildInfo(stage, "arm64");
    writeTestWindowsPe(stage / "bin" / "zanna-installer-host.exe", "arm64");
    writeTestWindowsPe(stage / "bin" / "zanna-installer-cleanup.exe", "arm64");
    {
        std::ofstream license(stage / "LICENSE", std::ios::binary | std::ios::trunc);
        license << std::string(25000, 'L');
    }
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.arch = "arm64";
    manifest.platform = "windows";

    WindowsToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-toolchain-arm64-setup.exe").string();
    params.archStr = "arm64";
    params.installerHostPath = (stage / "bin" / "zanna-installer-host.exe").string();
    params.installerCleanupPath = (stage / "bin" / "zanna-installer-cleanup.exe").string();
    buildWindowsToolchainInstaller(params);

    const auto pe = readFile(params.outputPath);
    std::ostringstream err;
    EXPECT_TRUE(verifyWindowsNativeInstaller(pe, err));
    EXPECT_EQ(readLE16(pe.data() + 0x84), static_cast<uint16_t>(0xAA64));
    const auto payloadZip = extractPeOverlayZipEntry(pe, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    const auto installedLicense = extractZipEntry(payloadZip, "LICENSE");
    EXPECT_EQ(installedLicense.size(), static_cast<size_t>(25000));
    EXPECT_TRUE(extractZipEntry(payloadZip, "bin/zanna-installer-host.exe").empty());
    EXPECT_TRUE(extractZipEntry(payloadZip, "bin/zanna-installer-cleanup.exe").empty());

    const auto metadataBytes = extractPeOverlayZipEntry(pe, "meta/installer-v2.txt");
    ASSERT_FALSE(metadataBytes.empty());
    const auto metadata = parseWindowsInstallerMetadata(
        std::string(reinterpret_cast<const char *>(metadataBytes.data()), metadataBytes.size()));
    EXPECT_EQ(metadata.architecture, "arm64");

    const auto maintenance = extractPeOverlayZipEntry(pe, "meta/uninstall.exe");
    const auto cleanup = extractPeOverlayZipEntry(pe, "meta/cleanup.exe");
    ASSERT_FALSE(maintenance.empty());
    ASSERT_FALSE(cleanup.empty());
    EXPECT_EQ(readLE16(maintenance.data() + 0x84), static_cast<uint16_t>(0xAA64));
    EXPECT_EQ(readLE16(cleanup.data() + 0x84), static_cast<uint16_t>(0xAA64));
    fs::remove_all(tmpRoot);
}

TEST(ToolchainWindowsPackageBuilder, AddsVSCodeShortcutWhenVSIXIsStaged) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_windows_vscode_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    normalizeMockStageForWindowsToolchain(stage);
    fs::create_directories(stage / "share" / "zanna" / "vscode");
    {
        ZipWriter vsix;
        static constexpr std::string_view kContentTypes = "<Types/>";
        static constexpr std::string_view kManifest = "<PackageManifest/>";
        static constexpr std::string_view kPackage = "{\"name\":\"zanna-lang\"}";
        vsix.addFile("[Content_Types].xml",
                     reinterpret_cast<const uint8_t *>(kContentTypes.data()),
                     kContentTypes.size());
        vsix.addFile("extension.vsixmanifest",
                     reinterpret_cast<const uint8_t *>(kManifest.data()),
                     kManifest.size());
        vsix.addFile("extension/package.json",
                     reinterpret_cast<const uint8_t *>(kPackage.data()),
                     kPackage.size());
        writeBytes(stage / "share" / "zanna" / "vscode" / "zanna-lang.vsix", vsix.finishToVector());
    }
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.arch = "x64";
    manifest.platform = "windows";

    WindowsToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-toolchain-setup.exe").string();
    params.archStr = "x64";
    buildWindowsToolchainInstaller(params);

    const auto pe = readFile(params.outputPath);
    std::ostringstream payloadErr;
    EXPECT_TRUE(verifyPEZipOverlayPayload(pe, {"meta/zanna_vscode_extension.lnk"}, payloadErr));
    const auto payloadZip = extractPeOverlayZipEntry(pe, "meta/payload.zip");
    ASSERT_FALSE(payloadZip.empty());
    EXPECT_TRUE(zipContainsEntries(payloadZip, {"share/zanna/vscode/zanna-lang.vsix"}));
    fs::remove_all(tmpRoot);
}

TEST(ToolchainWindowsPackageBuilder, HonorsMachineScopeAndFileAssociations) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_windows_machine_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    normalizeMockStageForWindowsToolchain(stage);
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.arch = "x64";
    manifest.platform = "windows";

    WindowsToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-toolchain-machine-setup.exe").string();
    params.archStr = "x64";
    params.installScope = "machine";
    params.registerFileAssociations = true;
    params.createStartMenuShortcuts = false;
    buildWindowsToolchainInstaller(params);

    const auto pe = readFile(params.outputPath);
    std::ostringstream err;
    EXPECT_TRUE(verifyPEZipOverlay(pe, err));
    EXPECT_TRUE(
        containsUtf16LE(pe, "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"));
    EXPECT_TRUE(containsAscii(pe, "requireAdministrator"));
    EXPECT_TRUE(containsUtf16LE(pe, "Software\\Classes\\.zia"));
    EXPECT_TRUE(containsUtf16LE(pe, "org.zanna.toolchain.zia"));
    EXPECT_FALSE(containsUtf16LE(pe, " run"));
    EXPECT_FALSE(containsUtf16LE(pe, " -run"));
    EXPECT_TRUE(containsUtf16LE(pe, "bin\\zannastudio.exe"));
    EXPECT_FALSE(containsUtf16LE(pe, "Zanna Developer Prompt"));
    fs::remove_all(tmpRoot);
}

TEST(ToolchainMacOSPackageBuilder, RejectsLossyManifestVersionWithoutOverride) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_pkg_version_lossy";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    addMockMacOSFileHandler(stage);
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.platform = "macos";
    manifest.arch = "x64";
    manifest.version = "1.2.3+build";

    MacOSToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-toolchain.pkg").string();
    EXPECT_THROWS(buildMacOSToolchainPackage(params), std::runtime_error);
    params.packageVersion = "1.2.3.4";
#if defined(__APPLE__)
    EXPECT_NO_THROW(buildMacOSToolchainPackage(params));
#else
    (void)params;
#endif
    fs::remove_all(tmpRoot);
}

TEST(ToolchainMacOSPackageBuilder, RequiresFileHandlerForAssociations) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_pkg_missing_handler";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.platform = "macos";
    manifest.arch = "x64";

    MacOSToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-toolchain.pkg").string();
    EXPECT_THROWS(buildMacOSToolchainPackage(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}

TEST(ToolchainMacOSPackageBuilder, RejectsInvalidMinimumOSVersionBeforePackaging) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_pkg_bad_min_os";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    addMockMacOSFileHandler(stage);
    auto manifest = gatherToolchainInstallManifest(stage);
    manifest.platform = "macos";
    manifest.arch = "x64";

    MacOSToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-toolchain.pkg").string();
    params.minimumMacOSVersion = "not-a-version";
    EXPECT_THROWS(buildMacOSToolchainPackage(params), std::runtime_error);
    fs::remove_all(tmpRoot);
}

#if defined(__APPLE__)
TEST(ToolchainMacOSPackageBuilder, BuildsPkgFromManifest) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_pkg_stage";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    addMockMacOSFileHandler(stage);
    const auto manifest = gatherToolchainInstallManifest(stage);

    MacOSToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-toolchain.pkg").string();
    buildMacOSToolchainPackage(params);

    const auto pkgBytes = readFile(params.outputPath);
    ASSERT_TRUE(pkgBytes.size() >= 4);
    EXPECT_EQ(pkgBytes[0], static_cast<uint8_t>('x'));
    EXPECT_EQ(pkgBytes[1], static_cast<uint8_t>('a'));
    EXPECT_EQ(pkgBytes[2], static_cast<uint8_t>('r'));
    EXPECT_EQ(pkgBytes[3], static_cast<uint8_t>('!'));
    std::vector<std::string> requiredMacPayload;
    for (const std::string &binary : requiredToolchainBinaryNames()) {
        requiredMacPayload.push_back("usr/local/zanna/bin/" + binary);
        requiredMacPayload.push_back("usr/local/bin/" + binary);
    }
    requiredMacPayload.push_back("usr/local/zanna/share/man/man1/zanna.1");
    requiredMacPayload.push_back("usr/local/share/man/man1/zanna.1");
    requiredMacPayload.push_back("usr/local/zanna/share/man/man7/zanna.7");
    requiredMacPayload.push_back("usr/local/share/man/man7/zanna.7");
    requiredMacPayload.push_back("usr/local/zanna/share/zanna/install_manifest.txt");
    requiredMacPayload.push_back("usr/local/zanna/share/zanna/uninstall.sh");
    requiredMacPayload.push_back("usr/local/lib/cmake/Zanna/ZannaConfig.cmake");
    requiredMacPayload.push_back("usr/local/lib/cmake/Zanna/ZannaConfigVersion.cmake");
    requiredMacPayload.push_back("Applications/Zanna Toolchain.app/Contents/Info.plist");
    requiredMacPayload.push_back("Applications/Zanna Toolchain.app/Contents/PkgInfo");
    requiredMacPayload.push_back("Applications/Zanna Toolchain.app/Contents/Resources/Zanna.icns");
    requiredMacPayload.push_back(
        "Applications/Zanna Toolchain.app/Contents/MacOS/zanna-file-handler");
    std::ostringstream err;
    const bool verified = verifyMacOSPkgPayload(pkgBytes, requiredMacPayload, err);
    if (!verified)
        std::cerr << err.str();
    EXPECT_TRUE(verified);
    fs::remove_all(tmpRoot);
}

TEST(ToolchainMacOSPackageBuilder, PkgEmbedsWelcomeAndLicensePanes) {
    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "zanna_toolchain_pkg_branding";
    fs::remove_all(tmpRoot);
    const fs::path stage = createMockToolchainStage(tmpRoot);
    addMockMacOSFileHandler(stage);
    const auto manifest = gatherToolchainInstallManifest(stage);

    MacOSToolchainBuildParams params;
    params.manifest = manifest;
    params.outputPath = (tmpRoot / "zanna-toolchain.pkg").string();
    buildMacOSToolchainPackage(params);

    // Installer panes and light/dark artwork are top-level XAR members alongside Distribution;
    // confirm they are present by decoding the archive's table of contents.
    const auto pkgBytes = readFile(params.outputPath);
    ASSERT_GE(pkgBytes.size(), static_cast<size_t>(28));
    const uint16_t headerSize = readBE16(pkgBytes.data() + 4);
    const uint64_t tocCompressed = readBE64(pkgBytes.data() + 8);
    const uint64_t tocUncompressed = readBE64(pkgBytes.data() + 16);
    const auto toc = zlibDecompress(pkgBytes.data() + headerSize,
                                    static_cast<size_t>(tocCompressed),
                                    static_cast<size_t>(tocUncompressed));
    const std::string tocText(toc.begin(), toc.end());
    EXPECT_TRUE(tocText.find("<name>welcome.html</name>") != std::string::npos);
    EXPECT_TRUE(tocText.find("<name>readme.html</name>") != std::string::npos);
    EXPECT_TRUE(tocText.find("<name>license.txt</name>") != std::string::npos);
    EXPECT_TRUE(tocText.find("<name>conclusion.html</name>") != std::string::npos);
    EXPECT_TRUE(tocText.find("<name>background.png</name>") != std::string::npos);
    EXPECT_TRUE(tocText.find("<name>background-dark.png</name>") != std::string::npos);
    fs::remove_all(tmpRoot);
}
#endif

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
