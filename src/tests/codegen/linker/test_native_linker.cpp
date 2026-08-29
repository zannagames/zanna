//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/linker/test_native_linker.cpp
// Purpose: Coverage for top-level native linker target gating and diagnostics.
//
//===----------------------------------------------------------------------===//

#include "codegen/common/linker/MachOExeWriter.hpp"
#include "codegen/common/linker/NativeLinker.hpp"
#include "codegen/common/linker/PeExeWriter.hpp"
#include "codegen/common/objfile/CoffWriter.hpp"
#include "codegen/common/objfile/ElfWriter.hpp"
#include "codegen/common/objfile/MachOWriter.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

using namespace zanna::codegen::linker;
using namespace zanna::codegen::objfile;

static int gFail = 0;

static void check(bool cond, const char *msg, int line) {
    if (!cond) {
        std::cerr << "FAIL line " << line << ": " << msg << "\n";
        ++gFail;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

static std::string tmpPath(const std::string &name) {
    namespace fs = std::filesystem;
    const fs::path dir{"build/test-out/native-linker"};
    fs::create_directories(dir);
    return (dir / name).string();
}

static std::vector<uint8_t> readFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return {};
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0)
        in.read(reinterpret_cast<char *>(data.data()), size);
    return data;
}

static uint16_t readLE16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t readLE32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t readLE64(const uint8_t *p) {
    return static_cast<uint64_t>(readLE32(p)) | (static_cast<uint64_t>(readLE32(p + 4)) << 32);
}

static bool containsAscii(const std::vector<uint8_t> &data, const std::string &needle) {
    if (needle.empty() || data.size() < needle.size())
        return false;
    return std::search(data.begin(), data.end(), needle.begin(), needle.end()) != data.end();
}

static bool containsBytes(const std::vector<uint8_t> &data, const std::vector<uint8_t> &needle) {
    if (needle.empty() || data.size() < needle.size())
        return false;
    return std::search(data.begin(), data.end(), needle.begin(), needle.end()) != data.end();
}

static std::string readMachOName(const uint8_t *p, size_t maxLen) {
    size_t len = 0;
    while (len < maxLen && p[len] != 0)
        ++len;
    return std::string(reinterpret_cast<const char *>(p), len);
}

static bool findMachOSection(const std::vector<uint8_t> &data,
                             const std::string &segmentName,
                             const std::string &sectionName,
                             uint32_t &alignLog2,
                             uint32_t &flags) {
    static constexpr uint32_t LC_SEGMENT_64 = 0x19;
    if (data.size() < 32)
        return false;

    const uint32_t ncmds = readLE32(data.data() + 16);
    size_t off = 32;
    for (uint32_t i = 0; i < ncmds; ++i) {
        if (off + 8 > data.size())
            return false;
        const uint32_t cmd = readLE32(data.data() + off);
        const uint32_t cmdsize = readLE32(data.data() + off + 4);
        if (cmd == LC_SEGMENT_64 && off + 72 <= data.size()) {
            const uint32_t nsects = readLE32(data.data() + off + 64);
            size_t secOff = off + 72;
            for (uint32_t si = 0; si < nsects; ++si) {
                if (secOff + 80 > data.size())
                    return false;
                const std::string sect = readMachOName(data.data() + secOff, 16);
                const std::string seg = readMachOName(data.data() + secOff + 16, 16);
                if (seg == segmentName && sect == sectionName) {
                    alignLog2 = readLE32(data.data() + secOff + 52);
                    flags = readLE32(data.data() + secOff + 64);
                    return true;
                }
                secOff += 80;
            }
        }
        if (cmdsize == 0)
            return false;
        off += cmdsize;
    }
    return false;
}

static bool findMachOSectionInfo(const std::vector<uint8_t> &data,
                                 const std::string &segmentName,
                                 const std::string &sectionName,
                                 uint32_t &alignLog2,
                                 uint32_t &flags,
                                 uint32_t &fileOffset,
                                 uint64_t &size) {
    static constexpr uint32_t LC_SEGMENT_64 = 0x19;
    if (data.size() < 32)
        return false;

    const uint32_t ncmds = readLE32(data.data() + 16);
    size_t off = 32;
    for (uint32_t i = 0; i < ncmds; ++i) {
        if (off + 8 > data.size())
            return false;
        const uint32_t cmd = readLE32(data.data() + off);
        const uint32_t cmdsize = readLE32(data.data() + off + 4);
        if (cmd == LC_SEGMENT_64 && off + 72 <= data.size()) {
            const uint32_t nsects = readLE32(data.data() + off + 64);
            size_t secOff = off + 72;
            for (uint32_t si = 0; si < nsects; ++si) {
                if (secOff + 80 > data.size())
                    return false;
                const std::string sect = readMachOName(data.data() + secOff, 16);
                const std::string seg = readMachOName(data.data() + secOff + 16, 16);
                if (seg == segmentName && sect == sectionName) {
                    size = readLE64(data.data() + secOff + 40);
                    fileOffset = readLE32(data.data() + secOff + 48);
                    alignLog2 = readLE32(data.data() + secOff + 52);
                    flags = readLE32(data.data() + secOff + 64);
                    return true;
                }
                secOff += 80;
            }
        }
        if (cmdsize == 0)
            return false;
        off += cmdsize;
    }
    return false;
}

static bool findMachOMainEntryOff(const std::vector<uint8_t> &data, uint64_t &entryOff) {
    static constexpr uint32_t LC_MAIN = 0x80000028;
    if (data.size() < 32)
        return false;
    const uint32_t ncmds = readLE32(data.data() + 16);
    size_t off = 32;
    for (uint32_t i = 0; i < ncmds; ++i) {
        if (off + 16 > data.size())
            return false;
        const uint32_t cmd = readLE32(data.data() + off);
        const uint32_t cmdsize = readLE32(data.data() + off + 4);
        if (cmd == LC_MAIN && cmdsize >= 24 && off + 24 <= data.size()) {
            entryOff = readLE64(data.data() + off + 8);
            return true;
        }
        if (cmdsize == 0)
            return false;
        off += cmdsize;
    }
    return false;
}

static bool findPeSection(const std::vector<uint8_t> &data,
                          const std::string &sectionName,
                          uint32_t &virtualSize,
                          uint32_t &rawSize,
                          uint32_t &characteristics) {
    if (data.size() < 0x40)
        return false;
    const uint32_t peOff = readLE32(data.data() + 0x3C);
    if (peOff + 24 > data.size())
        return false;
    const uint16_t numberOfSections = readLE16(data.data() + peOff + 6);
    const uint16_t optHeaderSize = readLE16(data.data() + peOff + 20);
    size_t secOff = peOff + 24 + optHeaderSize;
    for (uint16_t i = 0; i < numberOfSections; ++i) {
        if (secOff + 40 > data.size())
            return false;
        const std::string name = readMachOName(data.data() + secOff, 8);
        if (name == sectionName) {
            virtualSize = readLE32(data.data() + secOff + 8);
            rawSize = readLE32(data.data() + secOff + 16);
            characteristics = readLE32(data.data() + secOff + 36);
            return true;
        }
        secOff += 40;
    }
    return false;
}

static bool countMachOSymbols(const std::vector<uint8_t> &data,
                              const std::string &symbolName,
                              uint32_t &definedCount,
                              uint32_t &undefinedCount) {
    static constexpr uint32_t LC_SYMTAB = 0x02;
    static constexpr uint8_t N_TYPE = 0x0e;
    static constexpr uint8_t N_UNDF = 0x00;
    static constexpr uint8_t N_SECT = 0x0e;

    definedCount = 0;
    undefinedCount = 0;
    if (data.size() < 32)
        return false;

    uint32_t symoff = 0;
    uint32_t nsyms = 0;
    uint32_t stroff = 0;
    uint32_t strsize = 0;
    const uint32_t ncmds = readLE32(data.data() + 16);
    size_t off = 32;
    for (uint32_t i = 0; i < ncmds; ++i) {
        if (off + 8 > data.size())
            return false;
        const uint32_t cmd = readLE32(data.data() + off);
        const uint32_t cmdsize = readLE32(data.data() + off + 4);
        if (cmd == LC_SYMTAB) {
            if (off + 24 > data.size())
                return false;
            symoff = readLE32(data.data() + off + 8);
            nsyms = readLE32(data.data() + off + 12);
            stroff = readLE32(data.data() + off + 16);
            strsize = readLE32(data.data() + off + 20);
            break;
        }
        if (cmdsize == 0)
            return false;
        off += cmdsize;
    }

    if (symoff == 0 || nsyms == 0 || stroff == 0 || stroff + strsize > data.size())
        return false;
    if (symoff + static_cast<size_t>(nsyms) * 16 > data.size())
        return false;

    for (uint32_t i = 0; i < nsyms; ++i) {
        const uint8_t *entry = data.data() + symoff + static_cast<size_t>(i) * 16;
        const uint32_t strx = readLE32(entry);
        if (strx >= strsize)
            return false;
        const char *namePtr = reinterpret_cast<const char *>(data.data() + stroff + strx);
        const size_t maxLen = strsize - strx;
        const std::string name = readMachOName(reinterpret_cast<const uint8_t *>(namePtr), maxLen);
        if (name != symbolName)
            continue;
        const uint8_t type = entry[4] & N_TYPE;
        if (type == N_SECT)
            ++definedCount;
        else if (type == N_UNDF)
            ++undefinedCount;
    }
    return true;
}

/// Locates a named symbol in a Mach-O LC_SYMTAB and reports its nlist fields.
static bool findMachOSymbol(const std::vector<uint8_t> &data,
                            const std::string &symbolName,
                            uint8_t &type,
                            uint8_t &sect,
                            uint64_t &value) {
    static constexpr uint32_t LC_SYMTAB = 0x02;
    if (data.size() < 32)
        return false;
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    const uint32_t ncmds = readLE32(data.data() + 16);
    size_t off = 32;
    for (uint32_t i = 0; i < ncmds; ++i) {
        if (off + 8 > data.size())
            return false;
        const uint32_t cmd = readLE32(data.data() + off);
        const uint32_t cmdsize = readLE32(data.data() + off + 4);
        if (cmd == LC_SYMTAB) {
            if (off + 24 > data.size())
                return false;
            symoff = readLE32(data.data() + off + 8);
            nsyms = readLE32(data.data() + off + 12);
            stroff = readLE32(data.data() + off + 16);
            strsize = readLE32(data.data() + off + 20);
            break;
        }
        if (cmdsize == 0)
            return false;
        off += cmdsize;
    }
    if (symoff == 0 || nsyms == 0 || stroff == 0 || stroff + strsize > data.size())
        return false;
    if (symoff + static_cast<size_t>(nsyms) * 16 > data.size())
        return false;
    for (uint32_t i = 0; i < nsyms; ++i) {
        const uint8_t *entry = data.data() + symoff + static_cast<size_t>(i) * 16;
        const uint32_t strx = readLE32(entry);
        if (strx >= strsize)
            return false;
        const std::string name =
            readMachOName(data.data() + stroff + strx, static_cast<size_t>(strsize - strx));
        if (name != symbolName)
            continue;
        type = entry[4];
        sect = entry[5];
        value = readLE64(entry + 8);
        return true;
    }
    return false;
}

/// Reads LC_DYSYMTAB's (ilocalsym, nlocalsym, iextdefsym, nextdefsym, iundefsym, nundefsym).
static bool readMachODysymtab(const std::vector<uint8_t> &data, uint32_t out[6]) {
    static constexpr uint32_t LC_DYSYMTAB = 0x0B;
    if (data.size() < 32)
        return false;
    const uint32_t ncmds = readLE32(data.data() + 16);
    size_t off = 32;
    for (uint32_t i = 0; i < ncmds; ++i) {
        if (off + 8 > data.size())
            return false;
        const uint32_t cmd = readLE32(data.data() + off);
        const uint32_t cmdsize = readLE32(data.data() + off + 4);
        if (cmd == LC_DYSYMTAB) {
            if (off + 32 > data.size())
                return false;
            for (int k = 0; k < 6; ++k)
                out[k] = readLE32(data.data() + off + 8 + static_cast<size_t>(k) * 4);
            return true;
        }
        if (cmdsize == 0)
            return false;
        off += cmdsize;
    }
    return false;
}

/// Locates a named symbol in an ELF SHT_SYMTAB section and reports its fields.
/// `found` distinguishes "no .symtab" (returns false) from "symbol absent".
static bool findElfSymtabSymbol(const std::vector<uint8_t> &data,
                                const std::string &symbolName,
                                bool &found,
                                uint8_t &info,
                                uint16_t &shndx,
                                uint64_t &value,
                                uint64_t &size) {
    static constexpr uint32_t SHT_SYMTAB = 2;
    found = false;
    if (data.size() < 64 || data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
        return false;
    const uint64_t shoff = readLE64(data.data() + 40);
    const uint16_t shentsize = readLE16(data.data() + 58);
    const uint16_t shnum = readLE16(data.data() + 60);
    if (shentsize < 64 || shoff + static_cast<uint64_t>(shentsize) * shnum > data.size())
        return false;
    for (uint16_t i = 0; i < shnum; ++i) {
        const uint8_t *sh = data.data() + shoff + static_cast<uint64_t>(i) * shentsize;
        if (readLE32(sh + 4) != SHT_SYMTAB)
            continue;
        const uint64_t symOff = readLE64(sh + 24);
        const uint64_t symSize = readLE64(sh + 32);
        const uint32_t link = readLE32(sh + 40);
        const uint64_t entsize = readLE64(sh + 56);
        if (entsize != 24 || link >= shnum || symOff + symSize > data.size())
            return false;
        const uint8_t *strSh = data.data() + shoff + static_cast<uint64_t>(link) * shentsize;
        const uint64_t strOff = readLE64(strSh + 24);
        const uint64_t strSize = readLE64(strSh + 32);
        if (strOff + strSize > data.size())
            return false;
        for (uint64_t e = 0; e + 24 <= symSize; e += 24) {
            const uint8_t *sym = data.data() + symOff + e;
            const uint32_t nameOff = readLE32(sym);
            if (nameOff >= strSize)
                continue;
            const char *namePtr = reinterpret_cast<const char *>(data.data() + strOff + nameOff);
            const std::string name(namePtr,
                                   strnlen(namePtr, static_cast<size_t>(strSize - nameOff)));
            if (name != symbolName)
                continue;
            found = true;
            info = sym[4];
            shndx = readLE16(sym + 6);
            value = readLE64(sym + 8);
            size = readLE64(sym + 16);
            return true;
        }
        return true;
    }
    return false;
}

static bool findElfProgramHeader(const std::vector<uint8_t> &data, uint32_t type) {
    if (data.size() < 64 || data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
        return false;

    const uint64_t phoff = readLE64(data.data() + 32);
    const uint16_t phentsize = readLE16(data.data() + 54);
    const uint16_t phnum = readLE16(data.data() + 56);
    if (phentsize < 56 || phoff + static_cast<uint64_t>(phentsize) * phnum > data.size())
        return false;

    for (uint16_t i = 0; i < phnum; ++i) {
        const uint8_t *ph = data.data() + phoff + static_cast<uint64_t>(i) * phentsize;
        if (readLE32(ph) == type)
            return true;
    }
    return false;
}

int main() {
    {
        LinkLayout layout;
        layout.pageSize = 0x1000;
        layout.entryAddr = 0x140001000ULL;

        OutputSection text;
        text.name = ".text";
        text.data = {0xC3};
        text.virtualAddr = 0x140001000ULL;
        text.alignment = 16;
        text.executable = true;

        OutputSection bss;
        bss.name = ".bss";
        bss.data.resize(64, 0);
        bss.virtualAddr = 0x140002000ULL;
        bss.alignment = 16;
        bss.writable = true;
        bss.zeroFill = true;

        layout.sections = {text, bss};

        const std::string exePath = tmpPath("pe_zerofill.exe");
        const std::unordered_map<std::string, uint32_t> noSlots;
        std::ostringstream err;
        CHECK(writePeExe(exePath, layout, LinkArch::AArch64, {}, noSlots, false, 0, err));
        CHECK(err.str().empty());

        const std::vector<uint8_t> exe = readFile(exePath);
        uint32_t virtualSize = 0;
        uint32_t rawSize = 0;
        uint32_t characteristics = 0;
        CHECK(findPeSection(exe, ".bss", virtualSize, rawSize, characteristics));
        CHECK(virtualSize == 64);
        CHECK(rawSize == 0);
        CHECK((characteristics & 0x00000080U) != 0); // IMAGE_SCN_CNT_UNINITIALIZED_DATA
        CHECK((characteristics & 0x40000000U) != 0); // IMAGE_SCN_MEM_READ
        CHECK((characteristics & 0x80000000U) != 0); // IMAGE_SCN_MEM_WRITE
    }

    {
        LinkLayout layout;
        layout.pageSize = 0x4000;
        layout.entryAddr = 0x100004000ULL;

        OutputSection text;
        text.name = ".text";
        text.data = {0xC0, 0x03, 0x5F, 0xD6}; // ret
        text.virtualAddr = 0x100004000ULL;
        text.alignment = 4;
        text.executable = true;

        OutputSection bss;
        bss.name = ".bss";
        bss.data.resize(64, 0);
        bss.virtualAddr = 0x100008000ULL;
        bss.alignment = 8;
        bss.writable = true;
        bss.zeroFill = true;

        layout.sections = {text, bss};

        const std::string exePath = tmpPath("macho_zerofill");
        std::ostringstream err;
        CHECK(writeMachOExe(
            exePath, layout, LinkArch::AArch64, {{"/usr/lib/libSystem.B.dylib"}}, {}, {}, err));
        CHECK(err.str().empty());

        const std::vector<uint8_t> exe = readFile(exePath);
        uint32_t alignLog2 = 0;
        uint32_t flags = 0;
        uint32_t fileOffset = 0;
        uint64_t size = 0;
        CHECK(findMachOSectionInfo(exe, "__DATA", "__bss", alignLog2, flags, fileOffset, size));
        CHECK(alignLog2 == 3);
        CHECK(flags == 0x01U);
        CHECK(fileOffset == 0);
        CHECK(size == 64);
    }

    {
        LinkLayout layout;
        layout.pageSize = 0x4000;
        layout.entryAddr = 0x100004000ULL;

        OutputSection text;
        text.name = ".text";
        text.data = {0xC0, 0x03, 0x5F, 0xD6}; // ret
        text.virtualAddr = 0x100004000ULL;
        text.alignment = 4;
        text.executable = true;

        OutputSection modInit;
        modInit.name = "__DATA,__mod_init_func";
        modInit.data.resize(8, 0);
        modInit.virtualAddr = 0x100008000ULL;
        modInit.alignment = 8;
        modInit.writable = true;
        modInit.dataSegment = true;

        OutputSection modTerm;
        modTerm.name = "__DATA,__mod_term_func";
        modTerm.data.resize(8, 0);
        modTerm.virtualAddr = 0x100008008ULL;
        modTerm.alignment = 8;
        modTerm.writable = true;
        modTerm.dataSegment = true;

        layout.sections = {text, modInit, modTerm};

        const std::string exePath = tmpPath("macho_module_initializers");
        std::ostringstream err;
        CHECK(writeMachOExe(
            exePath, layout, LinkArch::AArch64, {{"/usr/lib/libSystem.B.dylib"}}, {}, {}, err));
        CHECK(err.str().empty());

        const std::vector<uint8_t> exe = readFile(exePath);
        uint32_t alignLog2 = 0;
        uint32_t flags = 0;
        CHECK(findMachOSection(exe, "__DATA", "__mod_init_func", alignLog2, flags));
        CHECK(alignLog2 == 3);
        CHECK((flags & 0xFFU) == 0x09U); // S_MOD_INIT_FUNC_POINTERS
        CHECK(findMachOSection(exe, "__DATA", "__mod_term_func", alignLog2, flags));
        CHECK(alignLog2 == 3);
        CHECK((flags & 0xFFU) == 0x0AU); // S_MOD_TERM_FUNC_POINTERS
    }

    {
        LinkLayout layout;
        layout.pageSize = 0x4000;

        OutputSection text;
        text.name = ".text";
        text.data = {0x1F, 0x20, 0x03, 0xD5, 0xC0, 0x03, 0x5F, 0xD6}; // nop; ret
        text.virtualAddr = 0x100004000ULL;
        text.alignment = 4;
        text.executable = true;
        layout.sections = {text};
        layout.entryAddr = text.virtualAddr + 4;

        const std::string exePath = tmpPath("macho_custom_entry");
        std::ostringstream err;
        CHECK(writeMachOExe(
            exePath, layout, LinkArch::AArch64, {{"/usr/lib/libSystem.B.dylib"}}, {}, {}, err));
        CHECK(err.str().empty());

        const std::vector<uint8_t> exe = readFile(exePath);
        uint32_t alignLog2 = 0;
        uint32_t flags = 0;
        uint32_t fileOffset = 0;
        uint64_t size = 0;
        CHECK(findMachOSectionInfo(exe, "__TEXT", "__text", alignLog2, flags, fileOffset, size));
        uint64_t entryOff = 0;
        CHECK(findMachOMainEntryOff(exe, entryOff));
        CHECK(entryOff == fileOffset + 4);
    }

    {
        LinkLayout layout;
        layout.pageSize = 0x4000;
        layout.entryAddr = 0x100004000ULL;

        OutputSection textA;
        textA.name = ".text.a";
        textA.data.resize(16, 0x90);
        textA.virtualAddr = 0x100004000ULL;
        textA.alignment = 4;
        textA.executable = true;

        OutputSection textB;
        textB.name = ".text.b";
        textB.data = {0xC0, 0x03, 0x5F, 0xD6};
        textB.virtualAddr = 0x100004004ULL;
        textB.alignment = 4;
        textB.executable = true;

        layout.sections = {textA, textB};

        const std::string exePath = tmpPath("macho_overlapping_text");
        std::ostringstream err;
        CHECK(!writeMachOExe(
            exePath, layout, LinkArch::AArch64, {{"/usr/lib/libSystem.B.dylib"}}, {}, {}, err));
        CHECK(err.str().find("overlaps previously written data") != std::string::npos);
    }

    {
        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::AArch64;
        opts.objPath = "does-not-matter.obj";
        opts.exePath = "does-not-matter.exe";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);

        CHECK(rc != 0);
        CHECK(err.str().find("not implemented yet") == std::string::npos);
        CHECK(err.str().find("failed to read object file") != std::string::npos);
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("main", SymbolBinding::Global, SymbolSection::Text);
        text.emit8(0xC3);

        const std::string objPath = tmpPath("format_mismatch.o");
        std::ostringstream writerErr;
        ElfWriter writer(ObjArch::X86_64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::X86_64;
        opts.objPath = objPath;
        opts.exePath = tmpPath("format_mismatch.exe");
        opts.entrySymbol = "main";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc != 0);
        CHECK(err.str().find("ELF object cannot be linked into Windows x86_64") !=
              std::string::npos);
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("main", SymbolBinding::Global, SymbolSection::Text);
        text.emit8(0xC3);

        const std::string objPath = tmpPath("missing_extra_user.o");
        std::ostringstream writerErr;
        ElfWriter writer(ObjArch::X86_64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Linux;
        opts.arch = LinkArch::X86_64;
        opts.objPath = objPath;
        opts.exePath = tmpPath("missing_extra_user");
        opts.entrySymbol = "main";
        opts.extraObjPaths.push_back(tmpPath("definitely_missing_extra.o"));

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc != 0);
        CHECK(err.str().find("failed to read extra object") != std::string::npos);
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("main", SymbolBinding::Global, SymbolSection::Text);
        const uint32_t chkstkIdx = text.findOrDeclareSymbol("__chkstk");
        text.addRelocation(RelocKind::A64Call26, chkstkIdx, 0);
        text.emit32LE(0x94000000U); // bl __chkstk
        text.emit32LE(0xD2800020U); // mov x0, #1
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("arm64_chkstk.obj");
        const std::string exePath = tmpPath("arm64_chkstk.exe");

        std::ostringstream writerErr;
        CoffWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "main";
        opts.stackSize = 0x200000;

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(exe.size() > 0x40);
        CHECK(exe[0] == 'M');
        CHECK(exe[1] == 'Z');
        if (exe.size() > 0x40) {
            const uint32_t peOff = readLE32(exe.data() + 0x3C);
            CHECK(peOff + 6 <= exe.size());
            if (peOff + 6 <= exe.size()) {
                CHECK(exe[peOff + 0] == 'P');
                CHECK(exe[peOff + 1] == 'E');
                CHECK(exe[peOff + 2] == 0);
                CHECK(exe[peOff + 3] == 0);
                CHECK(readLE16(exe.data() + peOff + 4) == 0xAA64);
                const size_t optHeaderOff = peOff + 24;
                CHECK(optHeaderOff + 80 <= exe.size());
                if (optHeaderOff + 80 <= exe.size())
                    CHECK(readLE64(exe.data() + optHeaderOff + 72) == 0x200000ULL);
            }
        }
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("main", SymbolBinding::Global, SymbolSection::Text);
        const uint32_t memsetIdx = text.findOrDeclareSymbol("__RTC_memset");
        text.addRelocation(RelocKind::A64Call26, memsetIdx, 0);
        text.emit32LE(0x94000000U); // bl __RTC_memset
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("arm64_rtc_memset.obj");
        const std::string exePath = tmpPath("arm64_rtc_memset.exe");

        std::ostringstream writerErr;
        CoffWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "main";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsBytes(exe,
                            {
                                0xE9,
                                0x03,
                                0x00,
                                0xAA, // mov x9, x0
                                0x82,
                                0x00,
                                0x00,
                                0xB4, // cbz x2, done
                                0x21,
                                0x15,
                                0x00,
                                0x38, // strb w1, [x9], #1
                            }));
        CHECK(!containsBytes(exe,
                             {
                                 0xE8,
                                 0x03,
                                 0x00,
                                 0xAA, // mov x8, x0
                                 0x82,
                                 0x00,
                                 0x00,
                                 0xB4,
                                 0x01,
                                 0x15,
                                 0x00,
                                 0x38, // strb w1, [x8], #1
                             }));
        CHECK(!containsBytes(exe,
                             {
                                 0xE3,
                                 0x03,
                                 0x00,
                                 0xAA, // mov x3, x0
                                 0x82,
                                 0x00,
                                 0x00,
                                 0xB4,
                                 0x61,
                                 0x14,
                                 0x00,
                                 0x38, // strb w1, [x3], #1
                             }));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("main", SymbolBinding::Global, SymbolSection::Text);
        const uint32_t pushIdx = text.findOrDeclareSymbol("__security_push_cookie");
        text.addRelocation(RelocKind::A64Call26, pushIdx, 0);
        text.emit32LE(0x94000000U); // bl __security_push_cookie
        const uint32_t popIdx = text.findOrDeclareSymbol("__security_pop_cookie");
        text.addRelocation(RelocKind::A64Call26, popIdx, 0);
        text.emit32LE(0x94000000U); // bl __security_pop_cookie
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("arm64_security_cookie_helpers.obj");
        const std::string exePath = tmpPath("arm64_security_cookie_helpers.exe");

        std::ostringstream writerErr;
        CoffWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "main";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsBytes(exe,
                            {
                                0xFF,
                                0x43,
                                0x00,
                                0xD1, // sub sp, sp, #0x10
                                0xC0,
                                0x03,
                                0x5F,
                                0xD6, // ret
                            }));
        CHECK(containsBytes(exe,
                            {
                                0xFF,
                                0x43,
                                0x00,
                                0x91, // add sp, sp, #0x10
                                0xC0,
                                0x03,
                                0x5F,
                                0xD6, // ret
                            }));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        const uint32_t reportImport = text.findOrDeclareSymbol("__imp__CrtDbgReport");
        text.emit8(0xFF); // call qword ptr [rip+disp32]
        text.emit8(0x15);
        text.addRelocation(RelocKind::PCRel32, reportImport, -4);
        text.emit32LE(0);
        const uint32_t invalidParameterImport =
            text.findOrDeclareSymbol("__imp__invalid_parameter");
        text.emit8(0xFF); // call qword ptr [rip+disp32]
        text.emit8(0x15);
        text.addRelocation(RelocKind::PCRel32, invalidParameterImport, -4);
        text.emit32LE(0);
        text.emit8(0x31); // xor eax, eax
        text.emit8(0xC0);
        text.emit8(0xC3); // ret

        const std::string objPath = tmpPath("win_x64_release_crt_report.obj");
        const std::string exePath = tmpPath("win_x64_release_crt_report.exe");

        std::ostringstream writerErr;
        CoffWriter writer(ObjArch::X86_64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::X86_64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";
        opts.windowsDebugRuntime = false;

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(!containsAscii(exe, "ucrtbased.dll"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("printf");
        text.findOrDeclareSymbol("__imp__CrtDbgReport");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("win_release_printf.obj");
        const std::string exePath = tmpPath("win_release_printf.exe");

        std::ostringstream writerErr;
        CoffWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsAscii(exe, "ucrtbase.dll"));
        CHECK(!containsAscii(exe, "ucrtbased.dll"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("printf");
        text.findOrDeclareSymbol("__imp__CrtDbgReport");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("win_debug_printf.obj");
        const std::string exePath = tmpPath("win_debug_printf.exe");

        std::ostringstream writerErr;
        CoffWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";
        opts.windowsDebugRuntime = true;

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsAscii(exe, "ucrtbased.dll"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("asin");
        text.findOrDeclareSymbol("atan2");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("win_release_math.obj");
        const std::string exePath = tmpPath("win_release_math.exe");

        std::ostringstream writerErr;
        CoffWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsAscii(exe, "ucrtbase.dll"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("_setjmp");
        text.findOrDeclareSymbol("_longjmp");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("win_release_setjmp_aliases.obj");
        const std::string exePath = tmpPath("win_release_setjmp_aliases.exe");

        std::ostringstream writerErr;
        CoffWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsAscii(exe, "ucrtbase.dll"));
        CHECK(containsAscii(exe, "setjmp"));
        CHECK(containsAscii(exe, "longjmp"));
        CHECK(!containsAscii(exe, "_setjmp"));
        CHECK(!containsAscii(exe, "_longjmp"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("freeaddrinfo");
        text.findOrDeclareSymbol("WSAStartup");
        text.findOrDeclareSymbol("WSAGetLastError");
        text.findOrDeclareSymbol("GetAdaptersAddresses");
        text.findOrDeclareSymbol("CertOpenStore");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("win_release_net_tls_imports.obj");
        const std::string exePath = tmpPath("win_release_net_tls_imports.exe");

        std::ostringstream writerErr;
        CoffWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsAscii(exe, "ws2_32.dll"));
        CHECK(containsAscii(exe, "iphlpapi.dll"));
        CHECK(containsAscii(exe, "crypt32.dll"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("XInputGetState");
        text.findOrDeclareSymbol("XInputSetState");
        text.findOrDeclareSymbol("abs");
        text.findOrDeclareSymbol("_pclose");
        text.findOrDeclareSymbol("GetUserNameA");
        text.findOrDeclareSymbol("ResetEvent");
        text.findOrDeclareSymbol("GetComputerNameA");
        text.findOrDeclareSymbol("GetFileSizeEx");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("win_release_xinput_and_abs.obj");
        const std::string exePath = tmpPath("win_release_xinput_and_abs.exe");

        std::ostringstream writerErr;
        CoffWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsAscii(exe, "xinput1_4.dll"));
        CHECK(containsAscii(exe, "ucrtbase.dll"));
        CHECK(containsAscii(exe, "advapi32.dll"));
        CHECK(containsAscii(exe, "kernel32.dll"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("_RTC_UninitUse");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("win_rtc_uninit_helper.obj");
        const std::string exePath = tmpPath("win_rtc_uninit_helper.exe");

        std::ostringstream writerErr;
        CoffWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("D3D11CreateDeviceAndSwapChain");
        text.findOrDeclareSymbol("D3DCompile");
        text.findOrDeclareSymbol("IID_ID3D11Texture2D");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("win_d3d_imports.obj");
        const std::string exePath = tmpPath("win_d3d_imports.exe");

        std::ostringstream writerErr;
        CoffWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsAscii(exe, "d3d11.dll"));
        CHECK(containsAscii(exe, "d3dcompiler_47.dll"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        // F10: a Darwin-exclusive symbol referenced from a Windows link must be a
        // hard undefined-symbol error, not accepted as a dynamic import. (The
        // planner's own "no DLL mapping" path is covered directly in
        // test_platform_import_planners.)
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("mach_timebase_info");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("win_unknown_import.obj");
        const std::string exePath = tmpPath("win_unknown_import.exe");

        std::ostringstream writerErr;
        CoffWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Windows;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";
        opts.windowsDebugRuntime = false;

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc != 0);
        CHECK(err.str().find("undefined symbol") != std::string::npos);
        CHECK(err.str().find("mach_timebase_info") != std::string::npos);
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("NSLog");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("macos_foundation_exact.o");
        const std::string exePath = tmpPath("macos_foundation_exact");

        std::ostringstream writerErr;
        MachOWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::macOS;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsAscii(exe, "Foundation.framework"));
        CHECK(!containsAscii(exe, "Cocoa.framework"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("macho_debug_line_input.o");

        std::ostringstream writerErr;
        MachOWriter writer(ObjArch::AArch64);
        writer.setDebugLineData({0x01, 0x02, 0x03, 0x04});
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        auto linkMachO = [&](bool preserveDebugSections, const std::string &name) {
            NativeLinkerOptions opts;
            opts.platform = LinkPlatform::macOS;
            opts.arch = LinkArch::AArch64;
            opts.objPath = objPath;
            opts.exePath = tmpPath(name);
            opts.entrySymbol = "entry";
            opts.preserveDebugSections = preserveDebugSections;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = nativeLink(opts, out, err);
            CHECK(rc == 0);
            CHECK(err.str().find("error:") == std::string::npos);
            return readFile(opts.exePath);
        };

        uint32_t alignLog2 = 0;
        uint32_t flags = 0;
        const std::vector<uint8_t> stripped = linkMachO(false, "macho_debug_line_stripped");
        CHECK(!findMachOSection(stripped, "__DWARF", "__debug_line", alignLog2, flags));

        const std::vector<uint8_t> preserved = linkMachO(true, "macho_debug_line_preserved");
        CHECK(findMachOSection(preserved, "__DWARF", "__debug_line", alignLog2, flags));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("SecKeychainOpen");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("macos_security_import.o");
        const std::string exePath = tmpPath("macos_security_import");

        std::ostringstream writerErr;
        MachOWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::macOS;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsAscii(exe, "Security.framework"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("CGImageSourceCreateImageAtIndex");
        text.findOrDeclareSymbol("CGContextDrawImage");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("macos_imageio_import.o");
        const std::string exePath = tmpPath("macos_imageio_import");

        std::ostringstream writerErr;
        MachOWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::macOS;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsAscii(exe, "ImageIO.framework"));
        CHECK(containsAscii(exe, "CoreGraphics.framework"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("__CFConstantStringClassReference");
        text.findOrDeclareSymbol("__kCFBooleanTrue");
        text.findOrDeclareSymbol("__darwin_check_fd_set_overflow");
        text.findOrDeclareSymbol("tcgetattr");
        text.findOrDeclareSymbol("tcsetattr");
        text.findOrDeclareSymbol("__strcat_chk");
        text.findOrDeclareSymbol("__strncpy_chk");
        text.findOrDeclareSymbol("select$DARWIN_EXTSN");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("macos_cf_and_libsystem_imports.o");
        const std::string exePath = tmpPath("macos_cf_and_libsystem_imports");

        std::ostringstream writerErr;
        MachOWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::macOS;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsAscii(exe, "CoreFoundation.framework"));
        CHECK(containsAscii(exe, "libSystem.B.dylib"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("closedir");
        text.findOrDeclareSymbol("opendir");
        text.findOrDeclareSymbol("readdir");
        text.findOrDeclareSymbol("fdopen");
        text.findOrDeclareSymbol("fnmatch");
        text.findOrDeclareSymbol("getpwuid");
        text.findOrDeclareSymbol("kevent");
        text.findOrDeclareSymbol("kqueue");
        text.findOrDeclareSymbol("pclose");
        text.findOrDeclareSymbol("popen");
        text.findOrDeclareSymbol("posix_spawn");
        text.findOrDeclareSymbol("posix_spawn_file_actions_addclose");
        text.findOrDeclareSymbol("posix_spawn_file_actions_adddup2");
        text.findOrDeclareSymbol("posix_spawn_file_actions_destroy");
        text.findOrDeclareSymbol("posix_spawn_file_actions_init");
        text.findOrDeclareSymbol("freeaddrinfo");
        text.findOrDeclareSymbol("getaddrinfo");
        text.findOrDeclareSymbol("getnameinfo");
        text.findOrDeclareSymbol("getsockname");
        text.findOrDeclareSymbol("gmtime_r");
        text.findOrDeclareSymbol("localtime_r");
        text.findOrDeclareSymbol("mktime");
        text.findOrDeclareSymbol("strftime");
        text.findOrDeclareSymbol("pthread_cond_timedwait_relative_np");
        text.findOrDeclareSymbol("pthread_detach");
        text.findOrDeclareSymbol("ldexp");
        text.findOrDeclareSymbol("environ");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("macos_posix_demo_imports.o");
        const std::string exePath = tmpPath("macos_posix_demo_imports");

        std::ostringstream writerErr;
        MachOWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::macOS;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsAscii(exe, "libSystem.B.dylib"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("NSMadeUpFunction");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("macos_unknown_ns_import.o");
        const std::string exePath = tmpPath("macos_unknown_ns_import");

        std::ostringstream writerErr;
        MachOWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::macOS;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc != 0);
        CHECK(err.str().find("no dylib mapping") != std::string::npos);
        CHECK(err.str().find("NSMadeUpFunction") != std::string::npos);
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("printf");
        text.findOrDeclareSymbol("cos");
        text.findOrDeclareSymbol("pthread_create");
        text.findOrDeclareSymbol("dlopen");
        text.findOrDeclareSymbol("XOpenDisplay");
        text.findOrDeclareSymbol("snd_pcm_open");
        text.emit8(0x31); // xor eax, eax
        text.emit8(0xC0);
        text.emit8(0xC3); // ret

        const std::string objPath = tmpPath("linux_x64_dynamic_imports.o");
        const std::string exePath = tmpPath("linux_x64_dynamic_imports");

        std::ostringstream writerErr;
        ElfWriter writer(ObjArch::X86_64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::Linux;
        opts.arch = LinkArch::X86_64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(exe.size() > 4);
        CHECK(exe[0] == 0x7F);
        CHECK(exe[1] == 'E');
        CHECK(exe[2] == 'L');
        CHECK(exe[3] == 'F');
        CHECK(findElfProgramHeader(exe, 3)); // PT_INTERP
        CHECK(findElfProgramHeader(exe, 2)); // PT_DYNAMIC
        CHECK(containsAscii(exe, "libc.so.6"));
        CHECK(containsAscii(exe, "libm.so.6"));
        CHECK(containsAscii(exe, "libpthread.so.0"));
        CHECK(containsAscii(exe, "libdl.so.2"));
        CHECK(containsAscii(exe, "libX11.so.6"));
        CHECK(containsAscii(exe, "libasound.so.2"));
        CHECK(containsAscii(exe, "ld-linux-x86-64.so.2"));
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("entry", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("printf");
        text.emit8(0x31); // xor eax, eax
        text.emit8(0xC0);
        text.emit8(0xC3); // ret

        const std::string objPath = tmpPath("macos_x64_printf.o");
        const std::string exePath = tmpPath("macos_x64_printf");

        std::ostringstream writerErr;
        MachOWriter writer(ObjArch::X86_64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::macOS;
        opts.arch = LinkArch::X86_64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "entry";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc != 0);
        CHECK(err.str().find("macOS x86_64") != std::string::npos);
        CHECK(err.str().find("supported dynamic-import targets") != std::string::npos);
        CHECK(err.str().find("Linux x86_64") != std::string::npos);
    }

    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("main", SymbolBinding::Global, SymbolSection::Text);
        text.findOrDeclareSymbol("MTLCreateSystemDefaultDevice");
        text.findOrDeclareSymbol("OBJC_CLASS_$_CAMetalLayer");
        text.findOrDeclareSymbol("objc_msgSend$newCommandQueue");
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("macos_objc_frameworks.o");
        const std::string exePath = tmpPath("macos_objc_frameworks");

        std::ostringstream writerErr;
        MachOWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        NativeLinkerOptions opts;
        opts.platform = LinkPlatform::macOS;
        opts.arch = LinkArch::AArch64;
        opts.objPath = objPath;
        opts.exePath = exePath;
        opts.entrySymbol = "main";

        std::ostringstream out;
        std::ostringstream err;
        const int rc = nativeLink(opts, out, err);
        CHECK(rc == 0);
        CHECK(err.str().find("error:") == std::string::npos);
        CHECK(err.str().find("undefined symbol 'main'") == std::string::npos);
        CHECK(std::filesystem::exists(exePath));

        const std::vector<uint8_t> exe = readFile(exePath);
        CHECK(containsAscii(exe, "Metal.framework"));
        CHECK(containsAscii(exe, "QuartzCore.framework"));

        uint32_t alignLog2 = 0;
        uint32_t flags = 0;
        CHECK(findMachOSection(exe, "__DATA", "__objc_selrefs", alignLog2, flags));
        CHECK(alignLog2 == 3);
        CHECK(flags == 0x10000005U);

        uint32_t definedMain = 0;
        uint32_t undefinedMain = 0;
        CHECK(countMachOSymbols(exe, "_main", definedMain, undefinedMain));
        CHECK(definedMain == 1);
        CHECK(undefinedMain == 0);
    }

    // Executable symbol tables name every placed definition (profiler support):
    // Mach-O publishes non-external N_SECT entries ahead of _main, and
    // LC_DYSYMTAB partitions locals / external definitions / imports.
    {
        CodeSection text;
        CodeSection rodata;
        // main calls both helpers so dead-strip keeps their atoms alive.
        text.defineSymbol("main", SymbolBinding::Global, SymbolSection::Text);
        const uint32_t hotIdx = text.findOrDeclareSymbol("hot_helper");
        text.addRelocation(RelocKind::A64Call26, hotIdx, 0);
        text.emit32LE(0x94000000U); // bl hot_helper
        const uint32_t coldIdx = text.findOrDeclareSymbol("cold_helper");
        text.addRelocation(RelocKind::A64Call26, coldIdx, 4);
        text.emit32LE(0x94000000U); // bl cold_helper
        text.emit32LE(0xD2800000U); // mov x0, #0
        text.emit32LE(0xD65F03C0U); // ret
        text.defineSymbol("hot_helper", SymbolBinding::Global, SymbolSection::Text);
        text.emit32LE(0xD2800020U); // mov x0, #1
        text.emit32LE(0xD65F03C0U); // ret
        text.defineSymbol("cold_helper", SymbolBinding::Global, SymbolSection::Text);
        text.emit32LE(0xD65F03C0U); // ret

        const std::string objPath = tmpPath("macos_local_symbols.o");
        std::ostringstream writerErr;
        MachOWriter writer(ObjArch::AArch64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        for (const bool emitLocals : {true, false}) {
            const std::string exePath =
                tmpPath(emitLocals ? "macos_local_symbols" : "macos_local_symbols_stripped");
            NativeLinkerOptions opts;
            opts.platform = LinkPlatform::macOS;
            opts.arch = LinkArch::AArch64;
            opts.objPath = objPath;
            opts.exePath = exePath;
            opts.entrySymbol = "main";
            opts.emitLocalSymbols = emitLocals;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = nativeLink(opts, out, err);
            CHECK(rc == 0);
            CHECK(err.str().find("error:") == std::string::npos);

            const std::vector<uint8_t> exe = readFile(exePath);
            uint8_t mainType = 0, mainSect = 0;
            uint64_t mainValue = 0;
            CHECK(findMachOSymbol(exe, "_main", mainType, mainSect, mainValue));
            CHECK(mainType == 0x0F); // N_SECT | N_EXT
            CHECK(mainSect == 1);

            uint8_t type = 0, sect = 0;
            uint64_t value = 0;
            const bool hasHot = findMachOSymbol(exe, "_hot_helper", type, sect, value);
            uint32_t dysym[6] = {0, 0, 0, 0, 0, 0};
            CHECK(readMachODysymtab(exe, dysym));
            if (emitLocals) {
                CHECK(hasHot);
                CHECK(type == 0x0E); // N_SECT, not external
                CHECK(sect == 1);
                CHECK(value == mainValue + 16);
                uint8_t coldType = 0, coldSect = 0;
                uint64_t coldValue = 0;
                CHECK(findMachOSymbol(exe, "_cold_helper", coldType, coldSect, coldValue));
                CHECK(coldValue == mainValue + 24);
                CHECK(dysym[0] == 0); // ilocalsym
                CHECK(dysym[1] == 2); // nlocalsym: both helpers
                CHECK(dysym[2] == 2); // iextdefsym follows the locals
                CHECK(dysym[3] == 1); // nextdefsym: _main
                CHECK(dysym[4] == 3); // iundefsym
            } else {
                CHECK(!hasHot);
                CHECK(dysym[1] == 0);
                CHECK(dysym[2] == 0);
                CHECK(dysym[3] == 1);
            }
        }
    }

    // ELF: a .symtab/.strtab pair with local STT_FUNC entries carrying sizes
    // that run to the next definition, absent when stripped.
    {
        CodeSection text;
        CodeSection rodata;
        text.defineSymbol("main", SymbolBinding::Global, SymbolSection::Text);
        text.emit8(0xC3); // ret
        text.defineSymbol("hot_helper", SymbolBinding::Global, SymbolSection::Text);
        text.emit8(0x90); // nop
        text.emit8(0x90); // nop
        text.emit8(0xC3); // ret
        text.defineSymbol("cold_helper", SymbolBinding::Global, SymbolSection::Text);
        text.emit8(0xC3); // ret

        const std::string objPath = tmpPath("linux_local_symbols.o");
        std::ostringstream writerErr;
        ElfWriter writer(ObjArch::X86_64);
        CHECK(writer.write(objPath, text, rodata, writerErr));
        CHECK(writerErr.str().empty());

        for (const bool emitLocals : {true, false}) {
            const std::string exePath =
                tmpPath(emitLocals ? "linux_local_symbols" : "linux_local_symbols_stripped");
            NativeLinkerOptions opts;
            opts.platform = LinkPlatform::Linux;
            opts.arch = LinkArch::X86_64;
            opts.objPath = objPath;
            opts.exePath = exePath;
            opts.entrySymbol = "main";
            opts.emitLocalSymbols = emitLocals;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = nativeLink(opts, out, err);
            CHECK(rc == 0);
            CHECK(err.str().find("error:") == std::string::npos);

            const std::vector<uint8_t> exe = readFile(exePath);
            bool found = false;
            uint8_t info = 0;
            uint16_t shndx = 0;
            uint64_t value = 0, size = 0;
            const bool hasSymtab =
                findElfSymtabSymbol(exe, "hot_helper", found, info, shndx, value, size);
            if (emitLocals) {
                CHECK(hasSymtab);
                CHECK(found);
                CHECK(info == 0x02); // STB_LOCAL | STT_FUNC
                CHECK(shndx != 0);
                CHECK(size == 3);
                bool mainFound = false;
                uint8_t mainInfo = 0;
                uint16_t mainShndx = 0;
                uint64_t mainValue = 0, mainSize = 0;
                CHECK(findElfSymtabSymbol(
                    exe, "main", mainFound, mainInfo, mainShndx, mainValue, mainSize));
                CHECK(mainFound);
                CHECK(mainShndx == shndx);
                CHECK(value == mainValue + 1);
                CHECK(mainSize == 1);
                CHECK(containsAscii(exe, ".symtab"));
                CHECK(containsAscii(exe, ".strtab"));
            } else {
                CHECK(!hasSymtab);
                CHECK(!containsAscii(exe, "hot_helper"));
            }
        }
    }

    if (gFail == 0) {
        std::cout << "All NativeLinker tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << gFail << " NativeLinker test(s) FAILED.\n";
    return EXIT_FAILURE;
}
