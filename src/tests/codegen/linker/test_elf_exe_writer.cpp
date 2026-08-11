//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/linker/test_elf_exe_writer.cpp
// Purpose: Unit tests for the ELF executable writer — verifies correct ELF
//          header, program headers, section headers, segment layout, W^X
//          enforcement, and multi-section output.
// Key invariants:
//   - ELF magic bytes and header fields match ELF64 spec
//   - One PT_LOAD per non-empty output section
//   - PT_GNU_STACK present with non-executable flags
//   - Section data written at correct file offsets
//   - W^X violation produces error
// Ownership/Lifetime: Standalone test binary.
// Links: codegen/common/linker/ElfExeWriter.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/common/linker/ElfExeWriter.hpp"
#include "codegen/common/linker/ElfSymbolVersions.hpp"
#include "codegen/common/linker/LinkTypes.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

using namespace zanna::codegen::linker;

static int gFail = 0;

static void check(bool cond, const char *msg, int line) {
    if (!cond) {
        std::cerr << "FAIL line " << line << ": " << msg << "\n";
        ++gFail;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

// ─── ELF structures for parsing written files ────────────────────────────

struct Elf64_Ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct Elf64_Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

// ELF constants for verification.
static constexpr uint16_t ET_EXEC = 2;
static constexpr uint16_t EM_X86_64 = 62;
static constexpr uint16_t EM_AARCH64 = 183;
static constexpr uint32_t PT_LOAD = 1;
static constexpr uint32_t PT_DYNAMIC = 2;
static constexpr uint32_t PT_INTERP = 3;
static constexpr uint32_t PT_TLS = 7;
static constexpr uint32_t PT_GNU_STACK = 0x6474E551;
static constexpr uint32_t PF_X = 1;
static constexpr uint32_t PF_W = 2;
static constexpr uint32_t PF_R = 4;
static constexpr uint32_t SHT_STRTAB = 3;
static constexpr uint32_t SHT_PROGBITS = 1;
static constexpr uint32_t SHT_RELA = 4;
static constexpr uint32_t SHT_HASH = 5;
static constexpr uint32_t SHT_DYNAMIC = 6;
static constexpr uint32_t SHT_NOBITS = 8;
static constexpr uint32_t SHT_DYNSYM = 11;
static constexpr uint32_t SHT_GNU_VERNEED = 0x6ffffffe;
static constexpr uint32_t SHT_GNU_VERSYM = 0x6fffffff;
static constexpr uint32_t SHF_ALLOC = 0x2;
static constexpr uint32_t SHF_WRITE = 0x1;
static constexpr uint32_t SHF_EXECINSTR = 0x4;
static constexpr uint32_t SHF_TLS = 0x400;

// ─── Helpers ─────────────────────────────────────────────────────────────

/// Read entire file into a byte vector.
static std::vector<uint8_t> readFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char *>(data.data()), sz);
    return data;
}

static uint64_t readLE64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    return v;
}

/// Create a temporary file path for test output.
static std::string tmpPath(const std::string &name) {
    auto dir = std::filesystem::temp_directory_path() / "zanna_elf_test";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

/// Clean up temp directory.
static void cleanupTmp() {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "zanna_elf_test", ec);
}

/// Build a minimal LinkLayout with the given sections.
static LinkLayout makeLayout(const std::vector<OutputSection> &sections,
                             uint64_t entryAddr = 0x401000,
                             size_t pageSize = 0x1000) {
    LinkLayout layout;
    layout.sections = sections;
    layout.entryAddr = entryAddr;
    layout.pageSize = pageSize;
    return layout;
}

/// Create an OutputSection with the given properties.
static OutputSection makeSec(const std::string &name,
                             size_t size,
                             uint64_t va,
                             bool exec,
                             bool writable,
                             uint8_t fillByte = 0xCC,
                             bool zeroFill = false) {
    OutputSection sec;
    sec.name = name;
    sec.data.resize(size, fillByte);
    sec.virtualAddr = va;
    sec.executable = exec;
    sec.writable = writable;
    sec.zeroFill = zeroFill;
    sec.alignment = 1;
    return sec;
}

static bool findSectionByName(const std::vector<uint8_t> &data,
                              const Elf64_Ehdr &ehdr,
                              const char *name,
                              Elf64_Shdr &out) {
    if (data.size() < ehdr.e_shoff + static_cast<size_t>(ehdr.e_shnum) * sizeof(Elf64_Shdr))
        return false;

    std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
    std::memcpy(shdrs.data(), data.data() + ehdr.e_shoff, ehdr.e_shnum * sizeof(Elf64_Shdr));
    const Elf64_Shdr &strtabShdr = shdrs[ehdr.e_shstrndx];
    if (data.size() < strtabShdr.sh_offset + strtabShdr.sh_size)
        return false;
    const char *strtab = reinterpret_cast<const char *>(data.data() + strtabShdr.sh_offset);

    for (const auto &shdr : shdrs) {
        if (shdr.sh_name >= strtabShdr.sh_size)
            continue;
        if (std::strcmp(strtab + shdr.sh_name, name) == 0) {
            out = shdr;
            return true;
        }
    }
    return false;
}

// ─── Tests ───────────────────────────────────────────────────────────────

/// Test 1: Single .text section — verify ELF header fields.
static void testSingleTextSection() {
    auto path = tmpPath("single_text.elf");
    auto sec = makeSec(".text", 64, 0x401000, true, false, 0x90); // NOP-filled

    auto layout = makeLayout({sec});
    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, err);
    CHECK(ok);
    CHECK(err.str().empty());

    auto data = readFile(path);
    CHECK(data.size() >= sizeof(Elf64_Ehdr));

    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));

    // ELF magic.
    CHECK(ehdr.e_ident[0] == 0x7F);
    CHECK(ehdr.e_ident[1] == 'E');
    CHECK(ehdr.e_ident[2] == 'L');
    CHECK(ehdr.e_ident[3] == 'F');
    CHECK(ehdr.e_ident[4] == 2); // ELFCLASS64
    CHECK(ehdr.e_ident[5] == 1); // ELFDATA2LSB
    CHECK(ehdr.e_ident[6] == 1); // EV_CURRENT

    // Header fields.
    CHECK(ehdr.e_type == ET_EXEC);
    CHECK(ehdr.e_machine == EM_X86_64);
    CHECK(ehdr.e_entry == 0x401000);
    CHECK(ehdr.e_ehsize == 64);
    CHECK(ehdr.e_phentsize == 56);
    CHECK(ehdr.e_shentsize == 64);

    // Program headers: 1 PT_LOAD + 1 PT_GNU_STACK = 2.
    CHECK(ehdr.e_phnum == 2);
    CHECK(ehdr.e_phoff == 64); // Immediately after ELF header.

    // Section headers: null + 1 section + .note.GNU-stack + .shstrtab = 4.
    CHECK(ehdr.e_shnum == 4);
    CHECK(ehdr.e_shstrndx == 3); // .shstrtab is last.
}

/// Test 2: Multi-section layout — .text, .rodata, .data.
static void testMultiSection() {
    auto path = tmpPath("multi_sec.elf");
    auto text = makeSec(".text", 128, 0x401000, true, false, 0xC3);     // RET-filled
    auto rodata = makeSec(".rodata", 64, 0x402000, false, false, 0x42); // 'B'-filled
    auto dataSec = makeSec(".data", 32, 0x403000, false, true, 0xDD);   // data-filled

    auto layout = makeLayout({text, rodata, dataSec});
    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, err);
    CHECK(ok);
    CHECK(err.str().empty());

    auto data = readFile(path);
    CHECK(data.size() >= sizeof(Elf64_Ehdr));

    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));

    // 3 PT_LOAD + 1 PT_GNU_STACK = 4.
    CHECK(ehdr.e_phnum == 4);

    // Section headers: null + 3 sections + .note.GNU-stack + .shstrtab = 6.
    CHECK(ehdr.e_shnum == 6);

    // Verify program headers.
    CHECK(data.size() >= ehdr.e_phoff + ehdr.e_phnum * sizeof(Elf64_Phdr));
    std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
    std::memcpy(phdrs.data(), data.data() + ehdr.e_phoff, ehdr.e_phnum * sizeof(Elf64_Phdr));

    // PT_LOAD[0]: .text — executable.
    CHECK(phdrs[0].p_type == PT_LOAD);
    CHECK(phdrs[0].p_vaddr == 0x401000);
    CHECK(phdrs[0].p_filesz == 128);
    CHECK((phdrs[0].p_flags & PF_R) != 0);
    CHECK((phdrs[0].p_flags & PF_X) != 0);
    CHECK((phdrs[0].p_flags & PF_W) == 0);
    CHECK(phdrs[0].p_align == 0x1000);

    // PT_LOAD[1]: .rodata — read-only.
    CHECK(phdrs[1].p_type == PT_LOAD);
    CHECK(phdrs[1].p_vaddr == 0x402000);
    CHECK(phdrs[1].p_filesz == 64);
    CHECK((phdrs[1].p_flags & PF_R) != 0);
    CHECK((phdrs[1].p_flags & PF_X) == 0);
    CHECK((phdrs[1].p_flags & PF_W) == 0);

    // PT_LOAD[2]: .data — writable.
    CHECK(phdrs[2].p_type == PT_LOAD);
    CHECK(phdrs[2].p_vaddr == 0x403000);
    CHECK(phdrs[2].p_filesz == 32);
    CHECK((phdrs[2].p_flags & PF_R) != 0);
    CHECK((phdrs[2].p_flags & PF_W) != 0);
    CHECK((phdrs[2].p_flags & PF_X) == 0);

    // PT_GNU_STACK: non-executable.
    CHECK(phdrs[3].p_type == PT_GNU_STACK);
    CHECK((phdrs[3].p_flags & PF_R) != 0);
    CHECK((phdrs[3].p_flags & PF_W) != 0);
    CHECK((phdrs[3].p_flags & PF_X) == 0);

    // Verify section data integrity — read .text bytes from file offset.
    CHECK(data.size() >= phdrs[0].p_offset + 128);
    for (size_t i = 0; i < 128; ++i)
        CHECK(data[phdrs[0].p_offset + i] == 0xC3);

    // Verify .rodata data.
    CHECK(data.size() >= phdrs[1].p_offset + 64);
    for (size_t i = 0; i < 64; ++i)
        CHECK(data[phdrs[1].p_offset + i] == 0x42);

    // Verify .data data.
    CHECK(data.size() >= phdrs[2].p_offset + 32);
    for (size_t i = 0; i < 32; ++i)
        CHECK(data[phdrs[2].p_offset + i] == 0xDD);
}

/// Test 3: W^X violation — section with both writable and executable flags.
static void testWxViolation() {
    auto path = tmpPath("wx_violation.elf");
    auto badSec = makeSec(".evil", 64, 0x401000, true, true); // W+X!

    auto layout = makeLayout({badSec});
    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, err);
    CHECK(!ok);
    CHECK(err.str().find("W^X violation") != std::string::npos);
}

/// Test 4: AArch64 architecture — verify EM_AARCH64 in header.
static void testAArch64Machine() {
    auto path = tmpPath("aarch64.elf");
    auto sec = makeSec(".text", 32, 0x401000, true, false);

    auto layout = makeLayout({sec});
    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::AArch64, err);
    CHECK(ok);

    auto data = readFile(path);
    CHECK(data.size() >= sizeof(Elf64_Ehdr));

    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));
    CHECK(ehdr.e_machine == EM_AARCH64);
}

/// Test 5: Entry point propagation — verify e_entry matches layout.entryAddr.
static void testEntryPoint() {
    auto path = tmpPath("entry.elf");
    auto sec = makeSec(".text", 256, 0x401000, true, false);

    auto layout = makeLayout({sec}, 0x401080); // Entry at offset 0x80 into text.
    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, err);
    CHECK(ok);

    auto data = readFile(path);
    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));
    CHECK(ehdr.e_entry == 0x401080);
}

/// Test 6: Section header verification — names, types, and flags.
static void testSectionHeaders() {
    auto path = tmpPath("shdrs.elf");
    auto text = makeSec(".text", 64, 0x401000, true, false);
    auto dataSec = makeSec(".data", 32, 0x402000, false, true);

    auto layout = makeLayout({text, dataSec});
    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, err);
    CHECK(ok);

    auto data = readFile(path);
    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));

    // Read section headers.
    CHECK(data.size() >= ehdr.e_shoff + ehdr.e_shnum * sizeof(Elf64_Shdr));
    std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
    std::memcpy(shdrs.data(), data.data() + ehdr.e_shoff, ehdr.e_shnum * sizeof(Elf64_Shdr));

    // shdrs[0]: null section.
    CHECK(shdrs[0].sh_type == 0);
    CHECK(shdrs[0].sh_size == 0);

    // shdrs[1]: .text — SHT_PROGBITS, SHF_ALLOC|SHF_EXECINSTR.
    CHECK(shdrs[1].sh_type == SHT_PROGBITS);
    CHECK((shdrs[1].sh_flags & SHF_ALLOC) != 0);
    CHECK((shdrs[1].sh_flags & SHF_EXECINSTR) != 0);
    CHECK((shdrs[1].sh_flags & SHF_WRITE) == 0);
    CHECK(shdrs[1].sh_addr == 0x401000);
    CHECK(shdrs[1].sh_size == 64);

    // shdrs[2]: .data — SHT_PROGBITS, SHF_ALLOC|SHF_WRITE.
    CHECK(shdrs[2].sh_type == SHT_PROGBITS);
    CHECK((shdrs[2].sh_flags & SHF_ALLOC) != 0);
    CHECK((shdrs[2].sh_flags & SHF_WRITE) != 0);
    CHECK((shdrs[2].sh_flags & SHF_EXECINSTR) == 0);
    CHECK(shdrs[2].sh_addr == 0x402000);
    CHECK(shdrs[2].sh_size == 32);

    // shdrs[last]: .shstrtab — SHT_STRTAB.
    CHECK(shdrs[ehdr.e_shstrndx].sh_type == SHT_STRTAB);
    CHECK(shdrs[ehdr.e_shstrndx].sh_size > 0);

    // Verify .shstrtab contains section names.
    auto &strtabShdr = shdrs[ehdr.e_shstrndx];
    CHECK(data.size() >= strtabShdr.sh_offset + strtabShdr.sh_size);
    const char *strtab = reinterpret_cast<const char *>(data.data() + strtabShdr.sh_offset);

    // .text section name should be in the string table.
    CHECK(shdrs[1].sh_name < strtabShdr.sh_size);
    CHECK(std::strcmp(strtab + shdrs[1].sh_name, ".text") == 0);

    // .data section name should be in the string table.
    CHECK(shdrs[2].sh_name < strtabShdr.sh_size);
    CHECK(std::strcmp(strtab + shdrs[2].sh_name, ".data") == 0);
}

/// Test 7: Page alignment — segments are page-aligned in file.
static void testPageAlignment() {
    auto path = tmpPath("page_align.elf");
    auto text = makeSec(".text", 100, 0x401000, true, false);
    auto dataSec = makeSec(".data", 50, 0x402000, false, true);

    auto layout = makeLayout({text, dataSec});
    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, err);
    CHECK(ok);

    auto data = readFile(path);
    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));

    std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
    std::memcpy(phdrs.data(), data.data() + ehdr.e_phoff, ehdr.e_phnum * sizeof(Elf64_Phdr));

    // Each PT_LOAD segment file offset should be page-aligned.
    for (size_t i = 0; i < phdrs.size(); ++i) {
        if (phdrs[i].p_type == PT_LOAD) {
            CHECK(phdrs[i].p_offset % 0x1000 == 0);
        }
    }
}

/// Test 8: Empty sections skipped — sections with no data don't get PT_LOAD.
static void testEmptySectionSkipped() {
    auto path = tmpPath("empty_sec.elf");
    OutputSection empty;
    empty.name = ".bss";
    empty.virtualAddr = 0x402000;
    empty.writable = true;
    empty.alignment = 1;
    // data is empty — no PT_LOAD should be created.

    auto text = makeSec(".text", 64, 0x401000, true, false);
    auto layout = makeLayout({text, empty});
    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, err);
    CHECK(ok);

    auto data = readFile(path);
    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));

    // Only 1 PT_LOAD (for .text) + 1 PT_GNU_STACK = 2.
    CHECK(ehdr.e_phnum == 2);

    std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
    std::memcpy(phdrs.data(), data.data() + ehdr.e_phoff, ehdr.e_phnum * sizeof(Elf64_Phdr));
    CHECK(phdrs[0].p_type == PT_LOAD);
    CHECK(phdrs[0].p_vaddr == 0x401000);
    CHECK(phdrs[1].p_type == PT_GNU_STACK);
}

/// Test 9: Zero-fill sections use SHT_NOBITS and occupy memory, not file bytes.
static void testZeroFillSection() {
    auto path = tmpPath("zerofill.elf");
    auto text = makeSec(".text", 32, 0x401000, true, false, 0x90);
    auto bss = makeSec(".bss", 48, 0x402000, false, true, 0x00, true);

    auto layout = makeLayout({text, bss});
    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, err);
    CHECK(ok);
    CHECK(err.str().empty());

    auto data = readFile(path);
    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));

    std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
    std::memcpy(phdrs.data(), data.data() + ehdr.e_phoff, ehdr.e_phnum * sizeof(Elf64_Phdr));
    CHECK(ehdr.e_phnum == 3); // .text + .bss + GNU-stack
    CHECK(phdrs[1].p_type == PT_LOAD);
    CHECK(phdrs[1].p_vaddr == 0x402000);
    CHECK(phdrs[1].p_filesz == 0);
    CHECK(phdrs[1].p_memsz == 48);
    CHECK((phdrs[1].p_flags & PF_W) != 0);

    std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
    std::memcpy(shdrs.data(), data.data() + ehdr.e_shoff, ehdr.e_shnum * sizeof(Elf64_Shdr));

    auto &strtabShdr = shdrs[ehdr.e_shstrndx];
    const char *strtab = reinterpret_cast<const char *>(data.data() + strtabShdr.sh_offset);

    bool foundBss = false;
    for (const auto &shdr : shdrs) {
        if (shdr.sh_name >= strtabShdr.sh_size)
            continue;
        if (std::strcmp(strtab + shdr.sh_name, ".bss") != 0)
            continue;
        foundBss = true;
        CHECK(shdr.sh_type == SHT_NOBITS);
        CHECK((shdr.sh_flags & SHF_ALLOC) != 0);
        CHECK((shdr.sh_flags & SHF_WRITE) != 0);
        CHECK(shdr.sh_size == 48);
        CHECK(shdr.sh_offset == 0x2000);
        break;
    }
    CHECK(foundBss);
}

/// F3: A zero-fill (.bss) section that follows a file-backed .data section in the
/// same writable segment must not inflate p_filesz. p_filesz must cover only the
/// file-backed bytes; the .data->.bss VA gap and the zero-fill span are p_memsz only.
static void testBssDoesNotInflateFileSize() {
    auto path = tmpPath("data_bss.elf");
    auto text = makeSec(".text", 16, 0x401000, true, false, 0xC3);
    auto dataSec = makeSec(".data", 16, 0x402000, false, true, 0x11);
    auto bss = makeSec(".bss", 64, 0x403000, false, true, 0x00, /*zeroFill=*/true);

    auto layout = makeLayout({text, dataSec, bss});
    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, err);
    CHECK(ok);
    CHECK(err.str().empty());

    auto data = readFile(path);
    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));
    std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
    std::memcpy(phdrs.data(), data.data() + ehdr.e_phoff, ehdr.e_phnum * sizeof(Elf64_Phdr));

    // The writable PT_LOAD holds .data + .bss (same permissions → one segment).
    bool found = false;
    for (const auto &ph : phdrs) {
        if (ph.p_type != PT_LOAD || (ph.p_flags & PF_W) == 0)
            continue;
        found = true;
        CHECK(ph.p_vaddr == 0x402000);
        CHECK(ph.p_filesz == 16);       // only the file-backed .data bytes
        CHECK(ph.p_memsz == 0x1040);    // spans the gap + .bss: 0x403040 - 0x402000
        CHECK(ph.p_filesz < ph.p_memsz);
    }
    CHECK(found);
}

/// Test 10: Large page size (16KB for macOS arm64-style layout).
static void testLargePageSize() {
    auto path = tmpPath("large_page.elf");
    auto text = makeSec(".text", 200, 0x401000, true, false);
    auto dataSec = makeSec(".data", 100, 0x405000, false, true);

    // Use 16KB page size.
    auto layout = makeLayout({text, dataSec}, 0x401000, 0x4000);
    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::AArch64, err);
    CHECK(ok);

    auto data = readFile(path);
    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));

    std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
    std::memcpy(phdrs.data(), data.data() + ehdr.e_phoff, ehdr.e_phnum * sizeof(Elf64_Phdr));

    // PT_LOAD segments should be 16KB-aligned.
    for (size_t i = 0; i < phdrs.size(); ++i) {
        if (phdrs[i].p_type == PT_LOAD) {
            CHECK(phdrs[i].p_offset % 0x4000 == 0);
            CHECK(phdrs[i].p_align == 0x4000);
        }
    }
}

/// Test 11: GNU-stack section header exists in section headers.
static void testGnuStackSectionHeader() {
    auto path = tmpPath("gnu_stack.elf");
    auto text = makeSec(".text", 32, 0x401000, true, false);

    auto layout = makeLayout({text});
    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, err);
    CHECK(ok);

    auto data = readFile(path);
    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));

    std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
    std::memcpy(shdrs.data(), data.data() + ehdr.e_shoff, ehdr.e_shnum * sizeof(Elf64_Shdr));

    // Read .shstrtab to resolve section names.
    auto &strtabShdr = shdrs[ehdr.e_shstrndx];
    const char *strtab = reinterpret_cast<const char *>(data.data() + strtabShdr.sh_offset);

    // Find .note.GNU-stack section header.
    bool found = false;
    for (size_t i = 0; i < shdrs.size(); ++i) {
        if (shdrs[i].sh_name < strtabShdr.sh_size &&
            std::strcmp(strtab + shdrs[i].sh_name, ".note.GNU-stack") == 0) {
            found = true;
            CHECK(shdrs[i].sh_type == SHT_PROGBITS);
            CHECK(shdrs[i].sh_size == 0); // No data.
            break;
        }
    }
    CHECK(found);
}

/// Test 12: Dynamic import metadata — PT_INTERP/PT_DYNAMIC and ELF dynamic sections.
static void testDynamicImports() {
    auto path = tmpPath("dynamic_imports.elf");
    auto text = makeSec(".text", 32, 0x401000, true, false, 0xC3);
    auto dataSec = makeSec(".data", 16, 0x402000, false, true, 0x00);

    auto layout = makeLayout({text, dataSec});
    layout.gotEntries.push_back({"printf", 0x402000});
    layout.bindEntries.push_back({"printf", 1, 8});

    std::ostringstream err;
    const std::unordered_set<std::string> dynSyms = {"printf"};
    const std::vector<std::string> neededLibs = {"libc.so.6"};
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, neededLibs, dynSyms, err);
    CHECK(ok);
    CHECK(err.str().empty());

    auto data = readFile(path);
    CHECK(data.size() >= sizeof(Elf64_Ehdr));

    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));
    CHECK(ehdr.e_phnum >= 5);

    std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
    std::memcpy(phdrs.data(), data.data() + ehdr.e_phoff, ehdr.e_phnum * sizeof(Elf64_Phdr));

    bool foundInterp = false;
    bool foundDynamic = false;
    bool foundHeaderLoad = false;
    for (const auto &phdr : phdrs) {
        if (phdr.p_type == PT_INTERP)
            foundInterp = true;
        if (phdr.p_type == PT_DYNAMIC)
            foundDynamic = true;
        if (phdr.p_type == PT_LOAD && phdr.p_offset == 0 &&
            phdr.p_filesz >= sizeof(Elf64_Ehdr) + ehdr.e_phnum * sizeof(Elf64_Phdr))
            foundHeaderLoad = true;
    }
    CHECK(foundInterp);
    CHECK(foundDynamic);
    CHECK(foundHeaderLoad);

    Elf64_Shdr interpShdr{};
    Elf64_Shdr dynstrShdr{};
    Elf64_Shdr dynsymShdr{};
    Elf64_Shdr hashShdr{};
    Elf64_Shdr relaShdr{};
    Elf64_Shdr dynamicShdr{};
    CHECK(findSectionByName(data, ehdr, ".interp", interpShdr));
    CHECK(findSectionByName(data, ehdr, ".dynstr", dynstrShdr));
    CHECK(findSectionByName(data, ehdr, ".dynsym", dynsymShdr));
    CHECK(findSectionByName(data, ehdr, ".hash", hashShdr));
    CHECK(findSectionByName(data, ehdr, ".rela.dyn", relaShdr));
    CHECK(findSectionByName(data, ehdr, ".dynamic", dynamicShdr));

    CHECK(interpShdr.sh_type == SHT_PROGBITS);
    CHECK(dynstrShdr.sh_type == SHT_STRTAB);
    CHECK(dynsymShdr.sh_type == SHT_DYNSYM);
    CHECK(hashShdr.sh_type == SHT_HASH);
    CHECK(relaShdr.sh_type == SHT_RELA);
    CHECK(dynamicShdr.sh_type == SHT_DYNAMIC);
    CHECK((dynamicShdr.sh_flags & SHF_WRITE) != 0);

    constexpr std::string_view kLibcName = "libc.so.6";
    constexpr std::string_view kInterpPath = "/lib64/ld-linux-x86-64.so.2";
    CHECK(std::search(data.begin(), data.end(), kLibcName.begin(), kLibcName.end()) != data.end());
    CHECK(std::search(data.begin(), data.end(), kInterpPath.begin(), kInterpPath.end()) !=
          data.end());

    CHECK(relaShdr.sh_size >= 2 * sizeof(uint64_t) * 3);
    const uint8_t *rela = data.data() + relaShdr.sh_offset;
    const uint32_t type0 = static_cast<uint32_t>(readLE64(rela + 8));
    const uint32_t type1 = static_cast<uint32_t>(readLE64(rela + 8 + 24));
    CHECK(type0 == 6); // R_X86_64_GLOB_DAT
    CHECK(type1 == 1); // R_X86_64_64
}

/// Test 13: TLS sections emit PT_TLS and SHF_TLS.
static void testTlsProgramHeaderAndSectionFlags() {
    auto path = tmpPath("tls.elf");
    auto tdata = makeSec(".tdata", 8, 0x402000, false, true, 0xAA);
    tdata.tls = true;
    tdata.alignment = 8;
    auto tbss = makeSec(".tbss", 16, 0x403000, false, true, 0x00, true);
    tbss.tls = true;
    tbss.alignment = 8;

    auto layout = makeLayout({tdata, tbss});
    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, err);
    CHECK(ok);
    CHECK(err.str().empty());

    auto data = readFile(path);
    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));

    std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
    std::memcpy(phdrs.data(), data.data() + ehdr.e_phoff, ehdr.e_phnum * sizeof(Elf64_Phdr));
    bool foundTlsPhdr = false;
    for (const auto &phdr : phdrs) {
        if (phdr.p_type != PT_TLS)
            continue;
        foundTlsPhdr = true;
        CHECK(phdr.p_vaddr == 0x402000);
        CHECK(phdr.p_filesz == 8);
        CHECK(phdr.p_memsz == 0x1010);
        CHECK(phdr.p_align == 8);
        CHECK((phdr.p_flags & PF_R) != 0);
    }
    CHECK(foundTlsPhdr);

    Elf64_Shdr tdataShdr{};
    Elf64_Shdr tbssShdr{};
    CHECK(findSectionByName(data, ehdr, ".tdata", tdataShdr));
    CHECK(findSectionByName(data, ehdr, ".tbss", tbssShdr));
    CHECK((tdataShdr.sh_flags & SHF_TLS) != 0);
    CHECK((tbssShdr.sh_flags & SHF_TLS) != 0);
    CHECK(tbssShdr.sh_type == SHT_NOBITS);
}

/// Test 14b: Imports name the version their provider defines by default.
///
/// A reference carrying no version does not reliably select a library's current
/// definition where several versions of a name exist, so the writer emits
/// `.gnu.version` / `.gnu.version_r`. Version data comes from the host's own
/// libraries, so this asserts the resolved-versions path only when the host
/// actually supplies them, and asserts the documented fallback — no version
/// sections at all — when it does not.
static void testSymbolVersionRequirements() {
    auto path = tmpPath("symbol_versions.elf");
    auto text = makeSec(".text", 32, 0x401000, true, false, 0xC3);
    auto dataSec = makeSec(".data", 16, 0x402000, false, true, 0x00);

    auto layout = makeLayout({text, dataSec});
    layout.gotEntries.push_back({"pthread_cond_init", 0x402000});

    const std::vector<std::string> neededLibs = {"libc.so.6"};
    const std::unordered_set<std::string> dynSyms = {"pthread_cond_init"};
    const auto versions =
        resolveElfSymbolVersions(neededLibs, {"pthread_cond_init"}, LinkArch::X86_64);

    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, neededLibs, dynSyms, err);
    CHECK(ok);
    CHECK(err.str().empty());

    auto data = readFile(path);
    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data.data(), sizeof(ehdr));

    Elf64_Shdr versymShdr{};
    Elf64_Shdr verneedShdr{};
    const bool hasVersym = findSectionByName(data, ehdr, ".gnu.version", versymShdr);
    const bool hasVerneed = findSectionByName(data, ehdr, ".gnu.version_r", verneedShdr);

    if (versions.empty()) {
        // No inspectable provider: references stay unversioned, as before.
        CHECK(!hasVersym);
        CHECK(!hasVerneed);
        return;
    }

    CHECK(hasVersym);
    CHECK(hasVerneed);
    CHECK(versymShdr.sh_type == SHT_GNU_VERSYM);
    CHECK(verneedShdr.sh_type == SHT_GNU_VERNEED);
    CHECK(verneedShdr.sh_info == 1);

    // One 16-bit index per .dynsym entry: the null symbol plus each import.
    Elf64_Shdr dynsymShdr{};
    CHECK(findSectionByName(data, ehdr, ".dynsym", dynsymShdr));
    CHECK(versymShdr.sh_size == (dynsymShdr.sh_size / dynsymShdr.sh_entsize) * sizeof(uint16_t));

    // The single import must carry a real requirement index, not the reserved
    // "no version requested" one that the loader treats as unversioned.
    uint16_t importIndex = 0;
    std::memcpy(
        &importIndex, data.data() + versymShdr.sh_offset + sizeof(uint16_t), sizeof(importIndex));
    CHECK(importIndex >= 2);

    // ...and that index must be the one the requirement table publishes, with a
    // version name matching what the provider was found to define.
    uint16_t vnCnt = 0;
    uint32_t vnAux = 0;
    std::memcpy(&vnCnt, data.data() + verneedShdr.sh_offset + 2, sizeof(vnCnt));
    std::memcpy(&vnAux, data.data() + verneedShdr.sh_offset + 8, sizeof(vnAux));
    CHECK(vnCnt == 1);

    const uint64_t auxOff = verneedShdr.sh_offset + vnAux;
    uint16_t vnaOther = 0;
    uint32_t vnaName = 0;
    std::memcpy(&vnaOther, data.data() + auxOff + 6, sizeof(vnaOther));
    std::memcpy(&vnaName, data.data() + auxOff + 8, sizeof(vnaName));
    CHECK(vnaOther == importIndex);

    Elf64_Shdr dynstrShdr{};
    CHECK(findSectionByName(data, ehdr, ".dynstr", dynstrShdr));
    const char *versionName =
        reinterpret_cast<const char *>(data.data() + dynstrShdr.sh_offset + vnaName);
    CHECK(versions.at("pthread_cond_init").version == versionName);
}

/// Test 14: Overflowing allocatable section ranges are rejected before placement.
static void testAllocSectionAddressOverflow() {
    auto path = tmpPath("overflow_alloc.elf");
    auto text = makeSec(".text", 4, std::numeric_limits<uint64_t>::max() - 1, true, false, 0xC3);
    auto layout = makeLayout({text}, text.virtualAddr);

    std::ostringstream err;
    bool ok = writeElfExe(path, layout, LinkArch::X86_64, {}, {}, 0, true, err);
    CHECK(!ok);
    CHECK(err.str().find("section address range") != std::string::npos);
}

// ─── Main ────────────────────────────────────────────────────────────────

int main() {
    testSingleTextSection();
    testMultiSection();
    testWxViolation();
    testAArch64Machine();
    testEntryPoint();
    testSectionHeaders();
    testPageAlignment();
    testEmptySectionSkipped();
    testZeroFillSection();
    testBssDoesNotInflateFileSize();
    testLargePageSize();
    testGnuStackSectionHeader();
    testDynamicImports();
    testSymbolVersionRequirements();
    testTlsProgramHeaderAndSectionFlags();
    testAllocSectionAddressOverflow();

    cleanupTmp();

    if (gFail > 0) {
        std::cerr << gFail << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "All ELF exe writer tests passed.\n";
    return 0;
}
