//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/linker/test_archive_reader.cpp
// Purpose: Regression coverage for COFF archive parsing on Windows.
//
//===----------------------------------------------------------------------===//

#include "codegen/common/LinkerSupport.hpp"
#include "codegen/common/RuntimeComponents.hpp"
#include "codegen/common/linker/ArchiveReader.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"
#include "codegen/common/linker/SymbolResolver.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace zanna::codegen;
using namespace zanna::codegen::common;
using namespace zanna::codegen::linker;

static int gFail = 0;

static void check(bool cond, const char *msg, int line) {
    if (!cond) {
        std::cerr << "FAIL line " << line << ": " << msg << "\n";
        ++gFail;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        check((cond), #cond, __LINE__);                                                            \
        if (!(cond))                                                                               \
            return EXIT_FAILURE;                                                                   \
    } while (0)

static ObjFile makeUndefinedCaller() {
    ObjFile obj;
    obj.name = "caller.obj";
    obj.format = ObjFileFormat::COFF;
    obj.is64bit = true;
    obj.isLittleEndian = true;
    obj.machine = 0x8664;

    obj.sections.push_back({});
    ObjSection text;
    text.name = ".text";
    text.executable = true;
    text.alloc = true;
    text.alignment = 16;
    text.data = {0xC3};
    obj.sections.push_back(std::move(text));

    obj.symbols.push_back({});

    ObjSymbol mainSym;
    mainSym.name = "main";
    mainSym.sectionIndex = 1;
    mainSym.binding = ObjSymbol::Global;
    obj.symbols.push_back(std::move(mainSym));

    ObjSymbol trap;
    trap.name = "rt_trap";
    trap.binding = ObjSymbol::Undefined;
    obj.symbols.push_back(std::move(trap));

    ObjSymbol init;
    init.name = "rt_init_stack_safety";
    init.binding = ObjSymbol::Undefined;
    obj.symbols.push_back(std::move(init));

    return obj;
}

static ObjFile makeWindowsBaseClosureCaller() {
    ObjFile obj;
    obj.name = "base-closure.obj";
    obj.format = ObjFileFormat::COFF;
    obj.is64bit = true;
    obj.isLittleEndian = true;
    obj.machine = 0x8664;

    obj.sections.push_back({});
    ObjSection text;
    text.name = ".text";
    text.executable = true;
    text.alloc = true;
    text.alignment = 16;
    text.data = {0xC3};
    obj.sections.push_back(std::move(text));

    obj.symbols.push_back({});

    ObjSymbol mainSym;
    mainSym.name = "main";
    mainSym.sectionIndex = 1;
    mainSym.binding = ObjSymbol::Global;
    obj.symbols.push_back(std::move(mainSym));

    for (const char *name : {"rt_trap",
                             "rt_init_stack_safety",
                             "rt_seq_with_capacity",
                             "rt_seq_len",
                             "rt_seq_get",
                             "rt_seq_push",
                             "rt_obj_new_i64",
                             "rt_obj_free",
                             "rt_type_registry_init",
                             "rt_type_registry_cleanup",
                             "rt_hash_ensure_seeded_",
                             "rt_siphash_k0_",
                             "rt_siphash_k1_",
                             "rt_siphash_seeded_",
                             "rt_file_channel_fd",
                             "rt_file_channel_get_eof",
                             "rt_file_channel_set_eof",
                             "rt_file_state_cleanup"}) {
        ObjSymbol sym;
        sym.name = name;
        sym.binding = ObjSymbol::Undefined;
        obj.symbols.push_back(std::move(sym));
    }

    return obj;
}

static void appendLE16(std::vector<uint8_t> &buf, uint16_t value) {
    buf.push_back(static_cast<uint8_t>(value & 0xFF));
    buf.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

static void appendLE32(std::vector<uint8_t> &buf, uint32_t value) {
    buf.push_back(static_cast<uint8_t>(value & 0xFF));
    buf.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

static void appendBE32(std::vector<uint8_t> &buf, uint32_t value) {
    buf.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(value & 0xFF));
}

static void appendZeros(std::vector<uint8_t> &buf, size_t count) {
    buf.insert(buf.end(), count, 0);
}

static void appendArField(std::vector<uint8_t> &buf, const std::string &value, size_t width) {
    for (size_t i = 0; i < width; ++i)
        buf.push_back(i < value.size() ? static_cast<uint8_t>(value[i])
                                       : static_cast<uint8_t>(' '));
}

static void appendArHeader(std::vector<uint8_t> &buf,
                           const std::string &name,
                           size_t dataSize,
                           const std::string &sizeOverride = {}) {
    appendArField(buf, name, 16);
    appendArField(buf, "0", 12);
    appendArField(buf, "0", 6);
    appendArField(buf, "0", 6);
    appendArField(buf, "100644", 8);
    appendArField(buf, sizeOverride.empty() ? std::to_string(dataSize) : sizeOverride, 10);
    buf.push_back('`');
    buf.push_back('\n');
}

static void appendArMember(std::vector<uint8_t> &buf,
                           const std::string &name,
                           const std::vector<uint8_t> &data) {
    appendArHeader(buf, name, data.size());
    buf.insert(buf.end(), data.begin(), data.end());
    if (buf.size() & 1)
        buf.push_back('\n');
}

static bool writeBinaryFile(const std::filesystem::path &path, const std::vector<uint8_t> &data) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char *>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

static std::vector<uint8_t> makeSyntheticCoffBssWithBogusRawData() {
    constexpr uint16_t kMachineAmd64 = 0x8664;
    constexpr uint32_t kScnCntUninitializedData = 0x00000080;
    constexpr uint32_t kScnMemRead = 0x40000000;
    constexpr uint32_t kScnMemWrite = 0x80000000;

    std::vector<uint8_t> obj;
    obj.reserve(20 + 40 + 8 + 4);

    // COFF file header.
    appendLE16(obj, kMachineAmd64);
    appendLE16(obj, 1);           // NumberOfSections
    appendLE32(obj, 0);           // TimeDateStamp
    appendLE32(obj, 20 + 40 + 8); // PointerToSymbolTable
    appendLE32(obj, 0);           // NumberOfSymbols
    appendLE16(obj, 0);           // SizeOfOptionalHeader
    appendLE16(obj, 0);           // Characteristics

    // Section header for a bogus .bss that advertises raw bytes.
    const std::string name = ".bss";
    for (size_t i = 0; i < 8; ++i)
        obj.push_back(i < name.size() ? static_cast<uint8_t>(name[i]) : 0);
    appendLE32(obj, 8);       // VirtualSize
    appendLE32(obj, 0);       // VirtualAddress
    appendLE32(obj, 8);       // SizeOfRawData
    appendLE32(obj, 20 + 40); // PointerToRawData
    appendLE32(obj, 0);       // PointerToRelocations
    appendLE32(obj, 0);       // PointerToLinenumbers
    appendLE16(obj, 0);       // NumberOfRelocations
    appendLE16(obj, 0);       // NumberOfLinenumbers
    appendLE32(obj, kScnCntUninitializedData | kScnMemRead | kScnMemWrite);

    // Bogus bytes that must not be copied into the output section.
    obj.insert(obj.end(), {0x2E, 0x74, 0x6C, 0x73, 0x24, 0x58, 0x59, 0x5A});

    // Empty COFF string table.
    appendLE32(obj, 4);
    return obj;
}

static std::vector<uint8_t> makeSyntheticCoffUnknownMachine() {
    constexpr uint32_t kScnCntCode = 0x00000020;
    constexpr uint32_t kScnMemExecute = 0x20000000;
    constexpr uint32_t kScnMemRead = 0x40000000;

    std::vector<uint8_t> obj;
    obj.reserve(20 + 40 + 1 + 4);

    appendLE16(obj, 0);           // Machine: IMAGE_FILE_MACHINE_UNKNOWN.
    appendLE16(obj, 1);           // NumberOfSections
    appendLE32(obj, 0);           // TimeDateStamp
    appendLE32(obj, 20 + 40 + 1); // PointerToSymbolTable
    appendLE32(obj, 0);           // NumberOfSymbols
    appendLE16(obj, 0);           // SizeOfOptionalHeader
    appendLE16(obj, 0);           // Characteristics

    const std::string name = ".text";
    for (size_t i = 0; i < 8; ++i)
        obj.push_back(i < name.size() ? static_cast<uint8_t>(name[i]) : 0);
    appendLE32(obj, 1);       // VirtualSize
    appendLE32(obj, 0);       // VirtualAddress
    appendLE32(obj, 1);       // SizeOfRawData
    appendLE32(obj, 20 + 40); // PointerToRawData
    appendLE32(obj, 0);       // PointerToRelocations
    appendLE32(obj, 0);       // PointerToLinenumbers
    appendLE16(obj, 0);       // NumberOfRelocations
    appendLE16(obj, 0);       // NumberOfLinenumbers
    appendLE32(obj, kScnCntCode | kScnMemExecute | kScnMemRead);
    obj.push_back(0xC3);
    appendLE32(obj, 4); // Empty string table.
    return obj;
}

static std::vector<uint8_t> makeSyntheticBigObj() {
    constexpr uint16_t kMachineAmd64 = 0x8664;
    constexpr uint32_t kScnCntCode = 0x00000020;
    constexpr uint32_t kScnMemExecute = 0x20000000;
    constexpr uint32_t kScnMemRead = 0x40000000;
    constexpr uint8_t kSymClassExternal = 2;

    constexpr uint32_t headerSize = 56;
    constexpr uint32_t sectionTableOff = headerSize;
    constexpr uint32_t rawDataOff = sectionTableOff + 40;
    constexpr uint32_t symbolTableOff = 100;

    std::vector<uint8_t> obj;
    obj.reserve(symbolTableOff + 20 + 4);

    // BigObj file header.
    appendLE16(obj, 0);      // Sig1
    appendLE16(obj, 0xFFFF); // Sig2
    appendLE16(obj, 2);      // Version
    appendLE16(obj, kMachineAmd64);
    appendLE32(obj, 0); // TimeDateStamp
    const uint8_t bigObjClassId[16] = {0xC7,
                                       0xA1,
                                       0xBA,
                                       0xD1,
                                       0xEE,
                                       0xBA,
                                       0xA9,
                                       0x4B,
                                       0xAF,
                                       0x20,
                                       0xFA,
                                       0xF6,
                                       0x6A,
                                       0xA4,
                                       0xDC,
                                       0xB8};
    obj.insert(obj.end(), bigObjClassId, bigObjClassId + sizeof(bigObjClassId));
    appendLE32(obj, 0); // SizeOfData
    appendLE32(obj, 0); // Flags
    appendLE32(obj, 0); // MetaDataSize
    appendLE32(obj, 0); // MetaDataOffset
    appendLE32(obj, 1); // NumberOfSections
    appendLE32(obj, symbolTableOff);
    appendLE32(obj, 1); // NumberOfSymbols
    CHECK(obj.size() == headerSize);

    // One .text section.
    const std::string secName = ".text";
    for (size_t i = 0; i < 8; ++i)
        obj.push_back(i < secName.size() ? static_cast<uint8_t>(secName[i]) : 0);
    appendLE32(obj, 1); // VirtualSize
    appendLE32(obj, 0); // VirtualAddress
    appendLE32(obj, 1); // SizeOfRawData
    appendLE32(obj, rawDataOff);
    appendLE32(obj, 0); // PointerToRelocations
    appendLE32(obj, 0); // PointerToLinenumbers
    appendLE16(obj, 0); // NumberOfRelocations
    appendLE16(obj, 0); // NumberOfLinenumbers
    appendLE32(obj, kScnCntCode | kScnMemExecute | kScnMemRead);
    CHECK(obj.size() == rawDataOff);

    obj.push_back(0xC3);
    appendZeros(obj, symbolTableOff - obj.size());

    // One BigObj symbol named "func".
    const std::string symName = "func";
    for (size_t i = 0; i < 8; ++i)
        obj.push_back(i < symName.size() ? static_cast<uint8_t>(symName[i]) : 0);
    appendLE32(obj, 0); // Value
    appendLE32(obj, 1); // 32-bit SectionNumber in BigObj
    appendLE16(obj, 0x20);
    obj.push_back(kSymClassExternal);
    obj.push_back(0); // Aux count

    appendLE32(obj, 4); // Empty string table.
    return obj;
}

static std::vector<uint8_t> makeSyntheticCoffHighSectionSymbol() {
    constexpr uint16_t kMachineAmd64 = 0x8664;
    constexpr uint32_t kScnCntUninitializedData = 0x00000080;
    constexpr uint32_t kScnMemRead = 0x40000000;
    constexpr uint32_t kScnMemWrite = 0x80000000;
    constexpr uint8_t kSymClassStatic = 3;
    constexpr uint16_t kHighSection = 0x8001;
    constexpr uint32_t sectionTableOff = 20;
    constexpr uint32_t symbolTableOff = sectionTableOff + 40u * kHighSection;

    std::vector<uint8_t> obj;
    obj.reserve(symbolTableOff + 18 + 4);

    appendLE16(obj, kMachineAmd64);
    appendLE16(obj, kHighSection);
    appendLE32(obj, 0);
    appendLE32(obj, symbolTableOff);
    appendLE32(obj, 1);
    appendLE16(obj, 0);
    appendLE16(obj, 0);

    for (uint32_t i = 0; i < kHighSection; ++i) {
        const std::string secName = ".bss";
        for (size_t n = 0; n < 8; ++n)
            obj.push_back(n < secName.size() ? static_cast<uint8_t>(secName[n]) : 0);
        appendLE32(obj, 1);
        appendLE32(obj, 0);
        appendLE32(obj, 1);
        appendLE32(obj, 0);
        appendLE32(obj, 0);
        appendLE32(obj, 0);
        appendLE16(obj, 0);
        appendLE16(obj, 0);
        appendLE32(obj, kScnCntUninitializedData | kScnMemRead | kScnMemWrite);
    }

    const std::string symName = "guard";
    for (size_t i = 0; i < 8; ++i)
        obj.push_back(i < symName.size() ? static_cast<uint8_t>(symName[i]) : 0);
    appendLE32(obj, 0);
    appendLE16(obj, kHighSection);
    appendLE16(obj, 0);
    obj.push_back(kSymClassStatic);
    obj.push_back(0);

    appendLE32(obj, 4);
    return obj;
}

static std::vector<uint8_t> makeSyntheticCoffComdat(uint8_t selection) {
    constexpr uint16_t kMachineAmd64 = 0x8664;
    constexpr uint32_t kScnCntCode = 0x00000020;
    constexpr uint32_t kScnLnkComdat = 0x00001000;
    constexpr uint32_t kScnMemExecute = 0x20000000;
    constexpr uint32_t kScnMemRead = 0x40000000;
    constexpr uint8_t kSymClassExternal = 2;
    constexpr uint8_t kSymClassStatic = 3;
    constexpr uint32_t rawDataOff = 20 + 40;
    constexpr uint32_t symbolTableOff = 64;

    std::vector<uint8_t> obj;
    obj.reserve(symbolTableOff + 18 * 3 + 4);

    appendLE16(obj, kMachineAmd64);
    appendLE16(obj, 1); // NumberOfSections
    appendLE32(obj, 0);
    appendLE32(obj, symbolTableOff);
    appendLE32(obj, 3); // Section symbol + aux + external symbol
    appendLE16(obj, 0);
    appendLE16(obj, 0);

    const std::string secName = ".text$F";
    for (size_t i = 0; i < 8; ++i)
        obj.push_back(i < secName.size() ? static_cast<uint8_t>(secName[i]) : 0);
    appendLE32(obj, 1);
    appendLE32(obj, 0);
    appendLE32(obj, 1);
    appendLE32(obj, rawDataOff);
    appendLE32(obj, 0);
    appendLE32(obj, 0);
    appendLE16(obj, 0);
    appendLE16(obj, 0);
    appendLE32(obj, kScnCntCode | kScnLnkComdat | kScnMemExecute | kScnMemRead);

    obj.push_back(0xC3);
    appendZeros(obj, symbolTableOff - obj.size());

    for (size_t i = 0; i < 8; ++i)
        obj.push_back(i < secName.size() ? static_cast<uint8_t>(secName[i]) : 0);
    appendLE32(obj, 0); // Value
    appendLE16(obj, 1); // SectionNumber
    appendLE16(obj, 0); // Type
    obj.push_back(kSymClassStatic);
    obj.push_back(1); // One section-definition aux record.

    appendLE32(obj, 1); // Aux Length
    appendLE16(obj, 0);
    appendLE16(obj, 0);
    appendLE32(obj, 0);
    appendLE16(obj, 0); // Associative section number, unused for non-associative selections.
    obj.push_back(selection);
    obj.push_back(0);
    appendLE16(obj, 0);

    const std::string symName = "foo";
    for (size_t i = 0; i < 8; ++i)
        obj.push_back(i < symName.size() ? static_cast<uint8_t>(symName[i]) : 0);
    appendLE32(obj, 0);
    appendLE16(obj, 1);
    appendLE16(obj, 0x20);
    obj.push_back(kSymClassExternal);
    obj.push_back(0);

    appendLE32(obj, 4);
    return obj;
}

static std::vector<uint8_t> makeSyntheticCoffCommon(uint32_t commonSize) {
    constexpr uint16_t kMachineAmd64 = 0x8664;
    constexpr uint8_t kSymClassExternal = 2;

    std::vector<uint8_t> obj;
    obj.reserve(20 + 18 + 4);
    appendLE16(obj, kMachineAmd64);
    appendLE16(obj, 0); // NumberOfSections
    appendLE32(obj, 0);
    appendLE32(obj, 20); // PointerToSymbolTable
    appendLE32(obj, 1);  // NumberOfSymbols
    appendLE16(obj, 0);
    appendLE16(obj, 0);

    const std::string symName = "common";
    for (size_t i = 0; i < 8; ++i)
        obj.push_back(i < symName.size() ? static_cast<uint8_t>(symName[i]) : 0);
    appendLE32(obj, commonSize);
    appendLE16(obj, 0); // Undefined section + nonzero value means common.
    appendLE16(obj, 0);
    obj.push_back(kSymClassExternal);
    obj.push_back(0);
    appendLE32(obj, 4);
    return obj;
}

static void runPortableArchiveReaderTests() {
    std::filesystem::create_directories("build/test-out");

    // --- Strict archive size parsing rejects missing decimal digits ---
    {
        std::vector<uint8_t> bytes{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
        appendArHeader(bytes, "bad.o/", 0, "          ");

        const auto path = std::filesystem::path("build/test-out/archive_bad_size.a");
        CHECK(writeBinaryFile(path, bytes));

        Archive ar;
        std::ostringstream err;
        CHECK(!readArchive(path.string(), ar, err));
        CHECK(err.str().find("invalid size") != std::string::npos);
        std::filesystem::remove(path);
    }

    // --- Leading padding before a decimal size remains accepted ---
    {
        std::vector<uint8_t> bytes{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
        appendArHeader(bytes, "empty.o/", 0, "         0");

        const auto path = std::filesystem::path("build/test-out/archive_padded_size.a");
        CHECK(writeBinaryFile(path, bytes));

        Archive ar;
        std::ostringstream err;
        CHECK(readArchive(path.string(), ar, err));
        CHECK(err.str().empty());
        CHECK(ar.members.size() == 1);
        std::filesystem::remove(path);
    }

    // --- UTF-8 API paths open archives without consulting the Windows ANSI code page ---
    {
        std::vector<uint8_t> bytes{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
        appendArHeader(bytes, "empty.o/", 0, "         0");

        const auto directory = std::filesystem::path("build/test-out") / u8"archive-Δ";
        const auto path = directory / "runtime.lib";
        std::filesystem::create_directories(directory);
        CHECK(writeBinaryFile(path, bytes));

        Archive ar;
        std::ostringstream err;
        CHECK(readArchive(pathToUtf8(path), ar, err));
        CHECK(err.str().empty());
        CHECK(ar.members.size() == 1);
        std::filesystem::remove_all(directory);
    }

    // --- GNU long names strip the "/\n" terminator from string-table entries ---
    {
        std::vector<uint8_t> longNames;
        const std::string longNameEntry = "very/long/member.o/\n";
        longNames.insert(longNames.end(), longNameEntry.begin(), longNameEntry.end());

        std::vector<uint8_t> bytes{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
        appendArMember(bytes, "//", longNames);
        appendArMember(bytes, "/0", {0x01, 0x02, 0x03});

        const auto path = std::filesystem::path("build/test-out/archive_gnu_long_name.a");
        CHECK(writeBinaryFile(path, bytes));

        Archive ar;
        std::ostringstream err;
        CHECK(readArchive(path.string(), ar, err));
        CHECK(err.str().empty());
        CHECK(ar.members.size() == 1);
        if (ar.members.size() == 1)
            CHECK(ar.members[0].name == "very/long/member.o");
        std::filesystem::remove(path);
    }

    // --- Duplicate symbols keep every candidate while preserving first-match compatibility ---
    {
        constexpr uint32_t firstMemberOffset = 88;
        constexpr uint32_t secondMemberOffset = 150;

        std::vector<uint8_t> symtab;
        appendBE32(symtab, 2);
        appendBE32(symtab, firstMemberOffset);
        appendBE32(symtab, secondMemberOffset);
        symtab.insert(symtab.end(), {'d', 'u', 'p', '\0', 'd', 'u', 'p', '\0'});

        std::vector<uint8_t> bytes{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
        appendArMember(bytes, "/", symtab);
        CHECK(bytes.size() == firstMemberOffset);
        appendArMember(bytes, "a.o/", {0x01});
        CHECK(bytes.size() == secondMemberOffset);
        appendArMember(bytes, "b.o/", {0x02});

        const auto path = std::filesystem::path("build/test-out/archive_duplicate_symbols.a");
        CHECK(writeBinaryFile(path, bytes));

        Archive ar;
        std::ostringstream err;
        CHECK(readArchive(path.string(), ar, err));
        CHECK(err.str().empty());
        CHECK(ar.symbolIndex["dup"] == 0);
        CHECK(ar.symbolCandidates.count("dup") == 1);
        if (ar.symbolCandidates.count("dup") == 1) {
            CHECK(ar.symbolCandidates["dup"].size() == 2);
            CHECK(ar.symbolCandidates["dup"][0] == 0);
            CHECK(ar.symbolCandidates["dup"][1] == 1);
        }
        std::filesystem::remove(path);
    }

    // --- Malformed archive member headers are hard errors ---
    {
        std::vector<uint8_t> bytes{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
        appendArHeader(bytes, "bad.o/", 0);
        bytes[8 + 58] = '!';

        const auto path = std::filesystem::path("build/test-out/archive_bad_header.a");
        CHECK(writeBinaryFile(path, bytes));

        Archive ar;
        std::ostringstream err;
        CHECK(!readArchive(path.string(), ar, err));
        CHECK(err.str().find("malformed archive member header") != std::string::npos);
        std::filesystem::remove(path);
    }

    // --- Odd-size members must carry the required newline padding byte ---
    {
        std::vector<uint8_t> bytes{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
        appendArHeader(bytes, "odd.o/", 1);
        bytes.push_back(0x01);
        bytes.push_back('x');

        const auto path = std::filesystem::path("build/test-out/archive_bad_padding.a");
        CHECK(writeBinaryFile(path, bytes));

        Archive ar;
        std::ostringstream err;
        CHECK(!readArchive(path.string(), ar, err));
        CHECK(err.str().find("missing its padding byte") != std::string::npos);
        std::filesystem::remove(path);
    }

    // --- Trailing bytes that cannot hold a header are rejected ---
    {
        std::vector<uint8_t> bytes{'!', '<', 'a', 'r', 'c', 'h', '>', '\n', 'x'};

        const auto path = std::filesystem::path("build/test-out/archive_trailing_data.a");
        CHECK(writeBinaryFile(path, bytes));

        Archive ar;
        std::ostringstream err;
        CHECK(!readArchive(path.string(), ar, err));
        CHECK(err.str().find("trailing malformed archive data") != std::string::npos);
        std::filesystem::remove(path);
    }

    // --- Archive symbol indexes must point at real member header offsets ---
    {
        std::vector<uint8_t> symtab;
        appendBE32(symtab, 1);
        appendBE32(symtab, 999);
        symtab.insert(symtab.end(), {'m', 'i', 's', 's', 'i', 'n', 'g', '\0'});

        std::vector<uint8_t> bytes{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
        appendArMember(bytes, "/", symtab);
        appendArMember(bytes, "real.o/", {0x01});

        const auto path = std::filesystem::path("build/test-out/archive_bad_sym_offset.a");
        CHECK(writeBinaryFile(path, bytes));

        Archive ar;
        std::ostringstream err;
        CHECK(!readArchive(path.string(), ar, err));
        CHECK(err.str().find("references missing member offset 999") != std::string::npos);
        std::filesystem::remove(path);
    }

    // --- COFF BigObj section and symbol tables are accepted ---
    {
        const auto bytes = makeSyntheticBigObj();
        ObjFile obj;
        std::ostringstream err;
        CHECK(readObjFile(bytes.data(), bytes.size(), "synthetic-bigobj.obj", obj, err));
        CHECK(err.str().empty());
        CHECK(obj.format == ObjFileFormat::COFF);
        CHECK(obj.machine == 0x8664);
        CHECK(obj.sections.size() == 2);
        CHECK(obj.sections[1].name == ".text");
        CHECK(obj.sections[1].data.size() == 1);
        CHECK(obj.symbols.size() == 2);
        CHECK(obj.symbols[1].name == "func");
        CHECK(obj.symbols[1].sectionIndex == 1);
    }

    // --- MSVC CRT machine-unknown COFF members are still regular objects ---
    {
        const auto bytes = makeSyntheticCoffUnknownMachine();
        ObjFile obj;
        std::ostringstream err;
        CHECK(detectFormat(bytes.data(), bytes.size()) == ObjFileFormat::COFF);
        CHECK(readObjFile(bytes.data(), bytes.size(), "synthetic-machine-unknown.obj", obj, err));
        CHECK(err.str().empty());
        CHECK(obj.format == ObjFileFormat::COFF);
        CHECK(obj.machine == 0);
        CHECK(obj.sections.size() == 2);
        CHECK(obj.sections[1].name == ".text");
        CHECK(obj.sections[1].data.size() == 1);
    }

    // --- Standard COFF high-bit section numbers are real sections, not negative specials ---
    {
        const auto bytes = makeSyntheticCoffHighSectionSymbol();
        ObjFile obj;
        std::ostringstream err;
        CHECK(readObjFile(bytes.data(), bytes.size(), "synthetic-high-section.obj", obj, err));
        CHECK(err.str().empty());
        CHECK(obj.format == ObjFileFormat::COFF);
        CHECK(obj.sections.size() == 0x8002);
        CHECK(obj.symbols.size() == 2);
        CHECK(obj.symbols[1].name == "guard");
        CHECK(obj.symbols[1].binding == ObjSymbol::Local);
        CHECK(obj.symbols[1].sectionIndex == 0x8001);
    }

    // --- COFF COMDAT section-definition aux records set duplicate policy ---
    {
        const auto bytes = makeSyntheticCoffComdat(/*IMAGE_COMDAT_SELECT_ANY=*/2);
        ObjFile obj;
        std::ostringstream err;
        CHECK(readObjFile(bytes.data(), bytes.size(), "synthetic-comdat.obj", obj, err));
        CHECK(err.str().empty());
        CHECK(obj.sections.size() == 2);
        CHECK(obj.sections[1].comdatSelection == ComdatSelection::Any);
        CHECK(obj.sections[1].comdatKey == ".text$F");
    }

    // --- Unsupported COFF COMDAT selections are diagnosed ---
    {
        const auto bytes = makeSyntheticCoffComdat(99);
        ObjFile obj;
        std::ostringstream err;
        CHECK(!readObjFile(bytes.data(), bytes.size(), "synthetic-bad-comdat.obj", obj, err));
        CHECK(err.str().find("unsupported COFF COMDAT selection") != std::string::npos);
    }

    // --- COFF common-symbol alignment follows size, capped at 32 bytes ---
    {
        {
            const auto bytes = makeSyntheticCoffCommon(3);
            ObjFile obj;
            std::ostringstream err;
            CHECK(readObjFile(bytes.data(), bytes.size(), "synthetic-common3.obj", obj, err));
            CHECK(obj.symbols.size() == 2);
            CHECK(obj.symbols[1].common);
            CHECK(obj.symbols[1].commonAlignment == 4);
        }
        {
            const auto bytes = makeSyntheticCoffCommon(64);
            ObjFile obj;
            std::ostringstream err;
            CHECK(readObjFile(bytes.data(), bytes.size(), "synthetic-common64.obj", obj, err));
            CHECK(obj.symbols.size() == 2);
            CHECK(obj.symbols[1].common);
            CHECK(obj.symbols[1].commonAlignment == 32);
        }
    }
}

int main() {
    runPortableArchiveReaderTests();
    if (gFail != 0)
        return EXIT_FAILURE;

#if !defined(_WIN32)
    std::cout << "SKIPPED (non-Windows)\n";
    return EXIT_SUCCESS;
#else
    const auto buildDir = findBuildDir();
    ASSERT(buildDir.has_value());

    const auto archivePath =
        runtimeArchivePath(*buildDir, archiveNameForComponent(RtComponent::Base));
    ASSERT(fileExists(archivePath));

    Archive ar;
    std::ostringstream err;
    ASSERT(readArchive(archivePath.string(), ar, err));
    CHECK(err.str().empty());

    CHECK(ar.symbolIndex.count("rt_trap") == 1);
    CHECK(ar.symbolIndex.count("rt_init_stack_safety") == 1);

    const auto trapIt = ar.symbolIndex.find("rt_trap");
    ASSERT(trapIt != ar.symbolIndex.end());
    ASSERT(trapIt->second < ar.members.size());

    ObjFile trapObj;
    const auto trapMemberBytes = extractMember(ar, ar.members[trapIt->second]);
    ASSERT(!trapMemberBytes.empty());
    ASSERT(readObjFile(trapMemberBytes.data(),
                       trapMemberBytes.size(),
                       ar.members[trapIt->second].name,
                       trapObj,
                       err));

    bool sawTrapDef = false;
    for (const auto &sym : trapObj.symbols) {
        if (sym.name == "rt_trap" && sym.binding != ObjSymbol::Undefined) {
            sawTrapDef = true;
            break;
        }
    }
    CHECK(sawTrapDef);

    auto loadArchiveAt = [&](const std::filesystem::path &path) -> Archive {
        Archive lib;
        if (!fileExists(path)) {
            check(false, "fileExists(path)", __LINE__);
            return lib;
        }
        std::ostringstream loadErr;
        if (!readArchive(path.string(), lib, loadErr)) {
            std::cerr << loadErr.str();
            check(false, "readArchive(path.string(), lib, loadErr)", __LINE__);
            return lib;
        }
        check(loadErr.str().empty(), "loadErr.str().empty()", __LINE__);
        return lib;
    };

    auto loadArchive = [&](RtComponent comp) -> Archive {
        return loadArchiveAt(runtimeArchivePath(*buildDir, archiveNameForComponent(comp)));
    };

    Archive collections = loadArchive(RtComponent::Collections);
    Archive arrays = loadArchive(RtComponent::Arrays);
    Archive game = loadArchive(RtComponent::Game);
    Archive oop = loadArchive(RtComponent::Oop);
    Archive text = loadArchive(RtComponent::Text);
    Archive iofs = loadArchive(RtComponent::IoFs);
    Archive threads = loadArchive(RtComponent::Threads);
    // The regex engine is a shared support library rather than a runtime
    // component archive, but rt_string_advanced/rt_compiled_pattern reference
    // its symbols, so a full resolve needs it exactly as the real link does.
    Archive regexEngine = loadArchiveAt(supportLibraryPath(*buildDir, "zanna_regex_engine"));

    CHECK(collections.symbolIndex.count("rt_seq_with_capacity") == 1);
    CHECK(game.symbolIndex.count("rt_uimenulist_new") == 1);
    CHECK(oop.symbolIndex.count("rt_obj_new_i64") == 1);
    CHECK(text.symbolIndex.count("rt_hash_ensure_seeded_") == 1);
    CHECK(iofs.symbolIndex.count("rt_file_channel_fd") == 1);

    {
        const auto components =
            resolveRequiredComponents(std::vector<std::string>{"rt_uimenulist_new"});
        CHECK(std::find(components.begin(), components.end(), RtComponent::Game) !=
              components.end());
        CHECK(std::find(components.begin(), components.end(), RtComponent::Collections) !=
              components.end());
        CHECK(std::find(components.begin(), components.end(), RtComponent::Arrays) !=
              components.end());
        CHECK(std::find(components.begin(), components.end(), RtComponent::Oop) !=
              components.end());
        CHECK(std::find(components.begin(), components.end(), RtComponent::Threads) !=
              components.end());
    }

    // --- Basic resolve: all runtime components needed for full resolution ---
    std::vector<Archive> archives = {
        ar, collections, arrays, oop, text, iofs, threads, regexEngine};
    std::vector<ObjFile> initialObjects = {makeUndefinedCaller()};
    std::unordered_map<std::string, GlobalSymEntry> globalSyms;
    std::vector<ObjFile> allObjects;
    std::unordered_set<std::string> dynamicSyms;
    std::ostringstream resolveErr;

    const bool resolved =
        resolveSymbols(initialObjects, archives, globalSyms, allObjects, dynamicSyms, resolveErr);
    if (!resolved)
        std::cerr << resolveErr.str();
    ASSERT(resolved);
    CHECK(dynamicSyms.count("rt_trap") == 0);
    CHECK(dynamicSyms.count("rt_init_stack_safety") == 0);
    CHECK(globalSyms.count("rt_trap") == 1);
    CHECK(globalSyms["rt_trap"].binding == GlobalSymEntry::Global);
    CHECK(globalSyms.count("rt_init_stack_safety") == 1);
    CHECK(globalSyms["rt_init_stack_safety"].binding == GlobalSymEntry::Global);

    // --- Transitive resolve: all component archives ---
    archives = {ar, collections, arrays, oop, text, iofs, threads, regexEngine};
    initialObjects = {makeUndefinedCaller()};
    globalSyms.clear();
    allObjects.clear();
    dynamicSyms.clear();
    std::ostringstream transitiveErr;
    const bool transitiveResolved = resolveSymbols(
        initialObjects, archives, globalSyms, allObjects, dynamicSyms, transitiveErr);
    if (!transitiveResolved)
        std::cerr << transitiveErr.str();
    ASSERT(transitiveResolved);
    for (const char *name : {"rt_seq_with_capacity",
                             "rt_seq_len",
                             "rt_seq_get",
                             "rt_seq_push",
                             "rt_obj_new_i64",
                             "rt_obj_free",
                             "rt_type_registry_init",
                             "rt_type_registry_cleanup",
                             "rt_hash_ensure_seeded_",
                             "rt_siphash_k0_",
                             "rt_siphash_k1_",
                             "rt_siphash_seeded_",
                             "rt_file_channel_fd",
                             "rt_file_channel_get_eof",
                             "rt_file_channel_set_eof",
                             "rt_file_state_cleanup"}) {
        CHECK(dynamicSyms.count(name) == 0);
        CHECK(globalSyms.count(name) == 1);
        CHECK(globalSyms[name].binding == GlobalSymEntry::Global);
    }

    initialObjects = {makeWindowsBaseClosureCaller()};
    globalSyms.clear();
    allObjects.clear();
    dynamicSyms.clear();
    std::ostringstream closureErr;
    const bool closureResolved =
        resolveSymbols(initialObjects, archives, globalSyms, allObjects, dynamicSyms, closureErr);
    if (!closureResolved)
        std::cerr << closureErr.str();
    ASSERT(closureResolved);
    for (const char *name : {"rt_seq_with_capacity",
                             "rt_seq_len",
                             "rt_seq_get",
                             "rt_seq_push",
                             "rt_obj_new_i64",
                             "rt_obj_free",
                             "rt_type_registry_init",
                             "rt_type_registry_cleanup",
                             "rt_hash_ensure_seeded_",
                             "rt_siphash_k0_",
                             "rt_siphash_k1_",
                             "rt_siphash_seeded_",
                             "rt_file_channel_fd",
                             "rt_file_channel_get_eof",
                             "rt_file_channel_set_eof",
                             "rt_file_state_cleanup"}) {
        CHECK(dynamicSyms.count(name) == 0);
        CHECK(globalSyms.count(name) == 1);
        CHECK(globalSyms[name].binding == GlobalSymEntry::Global);
    }

    {
        ObjFile syntheticObj;
        std::ostringstream parseErr;
        const auto bytes = makeSyntheticCoffBssWithBogusRawData();
        ASSERT(
            readObjFile(bytes.data(), bytes.size(), "synthetic-bss.obj", syntheticObj, parseErr));
        CHECK(parseErr.str().empty());
        ASSERT(syntheticObj.sections.size() >= 2);
        CHECK(syntheticObj.sections[1].name == ".bss");
        CHECK(syntheticObj.sections[1].zeroFill);
        CHECK(syntheticObj.sections[1].data.empty());
        CHECK(objSectionMemSize(syntheticObj.sections[1]) == 8);
    }

    if (gFail == 0) {
        std::cout << "All ArchiveReader tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << gFail << " ArchiveReader test(s) FAILED.\n";
    return EXIT_FAILURE;
#endif
}
