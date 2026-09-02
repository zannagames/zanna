//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/linker/test_reloc_edge_cases.cpp
// Purpose: Edge-case unit tests for the native linker's relocation applier —
//          verifies AArch64 relocation types, multi-reloc sections, PLT32,
//          and out-of-range branch detection.
// Key invariants:
//   - Branch26: ((S + A - P) >> 2) masked to 26 bits, ±128MB range
//   - Page21/ADRP: Page(S+A) - Page(P) encoded in immhi/immlo
//   - PageOff12: 12-bit page offset, scaled by access size
//   - CondBr19: 19-bit conditional branch, ±1MB range
//   - PLT32 treated identically to PC32 for static linking
// Ownership/Lifetime: Standalone test binary.
// Links: codegen/common/linker/RelocApplier.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/common/linker/LinkTypes.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"
#include "codegen/common/linker/RelocApplier.hpp"
#include "codegen/common/linker/RelocConstants.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>

using namespace zanna::codegen::linker;

static int gFail = 0;

static void check(bool cond, const char *msg, int line) {
    if (!cond) {
        std::cerr << "FAIL line " << line << ": " << msg << "\n";
        ++gFail;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

/// Helper: build a minimal ObjFile with one section, one symbol, one relocation.
static ObjFile makeObj(const std::string &name,
                       ObjFileFormat fmt,
                       const std::vector<uint8_t> &code,
                       const std::string &symName,
                       uint32_t relocType,
                       size_t relocOff,
                       int64_t addend) {
    ObjFile obj;
    obj.name = name;
    obj.format = fmt;

    obj.sections.push_back({}); // null section

    ObjSection text;
    text.name = ".text";
    text.data = code;
    text.executable = true;
    text.alloc = true;
    text.alignment = 4;

    ObjReloc rel;
    rel.offset = relocOff;
    rel.type = relocType;
    rel.symIndex = 1;
    rel.addend = addend;
    text.relocs.push_back(rel);

    obj.sections.push_back(text);

    obj.symbols.push_back({}); // null symbol

    ObjSymbol sym;
    sym.name = symName;
    sym.binding = ObjSymbol::Undefined;
    obj.symbols.push_back(sym);

    return obj;
}

/// Helper: build a LinkLayout with one .text output section.
static LinkLayout makeLayout(const std::vector<ObjFile> &objects, uint64_t textVA) {
    LinkLayout layout;
    layout.pageSize = 0x1000;

    OutputSection out;
    out.name = ".text";
    out.executable = true;
    out.virtualAddr = textVA;

    for (size_t oi = 0; oi < objects.size(); ++oi) {
        for (size_t si = 1; si < objects[oi].sections.size(); ++si) {
            InputChunk chunk;
            chunk.inputObjIndex = oi;
            chunk.inputSecIndex = si;
            chunk.outputOffset = out.data.size();
            chunk.size = objects[oi].sections[si].data.size();
            out.chunks.push_back(chunk);
            out.data.insert(out.data.end(),
                            objects[oi].sections[si].data.begin(),
                            objects[oi].sections[si].data.end());
        }
    }

    layout.sections.push_back(out);
    return layout;
}

static uint32_t readLE32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t readLE64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    return v;
}

int main() {
    // --- AArch64 PREL64 (ELF: R_AARCH64_PREL64 = type 260) ---
    // Emits S + A - P into an 8-byte data field, used by unwind metadata such
    // as DW.ref.__gxx_personality_v0 on Linux AArch64.
    {
        std::vector<uint8_t> callerCode(8, 0);
        auto caller = makeObj("a64_prel64.o",
                              ObjFileFormat::ELF,
                              callerCode,
                              "personality_ref",
                              elf_a64::kPrel64,
                              0,
                              0);

        ObjFile target;
        target.name = "personality.o";
        target.format = ObjFileFormat::ELF;
        target.sections.push_back({});
        ObjSection targetText;
        targetText.name = ".text";
        targetText.data.resize(16, 0);
        targetText.executable = true;
        targetText.alloc = true;
        targetText.alignment = 4;
        target.sections.push_back(targetText);
        target.symbols.push_back({});
        ObjSymbol targetSym;
        targetSym.name = "personality_ref";
        targetSym.binding = ObjSymbol::Global;
        targetSym.sectionIndex = 1;
        targetSym.offset = 0;
        target.symbols.push_back(targetSym);

        std::vector<ObjFile> objs = {caller, target};
        auto layout = makeLayout(objs, 0x100000);

        GlobalSymEntry entry;
        entry.name = "personality_ref";
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = 1;
        entry.secIndex = 1;
        entry.offset = 0;
        entry.resolvedAddr = 0;
        layout.globalSyms["personality_ref"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err);
        CHECK(ok);
        if (!err.str().empty())
            std::cerr << "  PREL64 err: " << err.str() << "\n";

        // S = 0x100000 + callerCode.size() = 0x100008
        // P = 0x100000
        CHECK(readLE64(layout.sections[0].data.data()) == 8);
    }

    // --- AArch64 Branch26 (ELF: R_AARCH64_JUMP26 = type 282) ---
    // Encodes ((S + A - P) >> 2) & 0x03FFFFFF into the low 26 bits.
    // Instruction template: B = 0x14000000.
    {
        // caller: B instruction at offset 0.
        // target: at offset 16 (4 instructions later).
        std::vector<uint8_t> callerCode(4, 0); // B placeholder
        callerCode[0] = 0x00;
        callerCode[1] = 0x00;
        callerCode[2] = 0x00;
        callerCode[3] = 0x14; // B (unconditional branch)

        auto caller = makeObj("branch.o",
                              ObjFileFormat::ELF,
                              callerCode,
                              "target_func",
                              /*R_AARCH64_JUMP26=*/282,
                              0,
                              0);

        // Target object: 16 bytes of NOPs starting after caller.
        ObjFile target;
        target.name = "target.o";
        target.format = ObjFileFormat::ELF;
        target.sections.push_back({});
        ObjSection targetText;
        targetText.name = ".text";
        targetText.data.resize(16, 0x00);
        // NOP = 0xD503201F
        for (size_t i = 0; i < 16; i += 4) {
            targetText.data[i] = 0x1F;
            targetText.data[i + 1] = 0x20;
            targetText.data[i + 2] = 0x03;
            targetText.data[i + 3] = 0xD5;
        }
        targetText.executable = true;
        targetText.alloc = true;
        targetText.alignment = 4;
        target.sections.push_back(targetText);
        target.symbols.push_back({});
        ObjSymbol targetSym;
        targetSym.name = "target_func";
        targetSym.binding = ObjSymbol::Global;
        targetSym.sectionIndex = 1;
        targetSym.offset = 0;
        target.symbols.push_back(targetSym);

        std::vector<ObjFile> objs = {caller, target};
        auto layout = makeLayout(objs, 0x100000);

        GlobalSymEntry entry;
        entry.name = "target_func";
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = 1;
        entry.secIndex = 1;
        entry.offset = 0;
        entry.resolvedAddr = 0;
        layout.globalSyms["target_func"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err);
        CHECK(ok);
        if (!err.str().empty())
            std::cerr << "  Branch26 err: " << err.str() << "\n";

        // S = 0x100000 + 4 = 0x100004 (target starts after caller's 4 bytes)
        // P = 0x100000 + 0 = 0x100000
        // displacement = (S - P) >> 2 = 4 >> 2 = 1
        // Expected: 0x14000000 | 1 = 0x14000001
        uint32_t patched = readLE32(layout.sections[0].data.data());
        CHECK((patched & 0xFC000000) == 0x14000000); // opcode preserved
        uint32_t imm26 = patched & 0x03FFFFFF;
        CHECK(imm26 == 1); // forward jump by 1 instruction
    }

    // --- AArch64 Call26 (ELF: R_AARCH64_CALL26 = type 283) ---
    // Same encoding as Branch26 but for BL (call) instructions.
    {
        std::vector<uint8_t> code(4, 0);
        code[3] = 0x94; // BL = 0x94000000

        auto caller =
            makeObj("call.o", ObjFileFormat::ELF, code, "callee", /*R_AARCH64_CALL26=*/283, 0, 0);

        ObjFile target;
        target.name = "callee.o";
        target.format = ObjFileFormat::ELF;
        target.sections.push_back({});
        ObjSection sec;
        sec.name = ".text";
        sec.data.resize(8, 0x00);
        sec.executable = true;
        sec.alloc = true;
        sec.alignment = 4;
        target.sections.push_back(sec);
        target.symbols.push_back({});
        ObjSymbol sym;
        sym.name = "callee";
        sym.binding = ObjSymbol::Global;
        sym.sectionIndex = 1;
        target.symbols.push_back(sym);

        std::vector<ObjFile> objs = {caller, target};
        auto layout = makeLayout(objs, 0x200000);

        GlobalSymEntry e;
        e.name = "callee";
        e.binding = GlobalSymEntry::Global;
        e.objIndex = 1;
        e.secIndex = 1;
        e.offset = 0;
        e.resolvedAddr = 0;
        layout.globalSyms["callee"] = e;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err);
        CHECK(ok);

        uint32_t patched = readLE32(layout.sections[0].data.data());
        CHECK((patched & 0xFC000000) == 0x94000000); // BL opcode preserved
        uint32_t imm26 = patched & 0x03FFFFFF;
        CHECK(imm26 == 1); // forward call by 1 instruction
    }

    // --- AArch64 ADRP + LDR PageOff12 (types 275 + 286) ---
    // ADRP encodes Page(S+A) - Page(P) into immhi:immlo.
    // LDR encodes (S+A)[11:3] (8-byte scaled) into imm12 bits [21:10].
    {
        // Build an object with two relocations in one section:
        //   offset 0: ADRP x0, sym@PAGE     (type 275)
        //   offset 4: LDR  x0, [x0, sym@PAGEOFF] (type 286)
        ObjFile obj;
        obj.name = "adrp_ldr.o";
        obj.format = ObjFileFormat::ELF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        // ADRP x0, #0 = 0x90000000
        // LDR  x0, [x0] = 0xF9400000
        text.data = {0x00,
                     0x00,
                     0x00,
                     0x90, // adrp x0, #0
                     0x00,
                     0x00,
                     0x40,
                     0xF9}; // ldr  x0, [x0, #0]
        text.executable = true;
        text.alloc = true;
        text.alignment = 4;

        // ADRP relocation (type 275 = R_AARCH64_ADR_PREL_PG_HI21).
        ObjReloc adrpReloc;
        adrpReloc.offset = 0;
        adrpReloc.type = 275;
        adrpReloc.symIndex = 1;
        adrpReloc.addend = 0;
        text.relocs.push_back(adrpReloc);

        // LDR PageOff12 relocation (type 286 = R_AARCH64_LDST64_ABS_LO12_NC).
        ObjReloc ldrReloc;
        ldrReloc.offset = 4;
        ldrReloc.type = 286;
        ldrReloc.symIndex = 1;
        ldrReloc.addend = 0;
        text.relocs.push_back(ldrReloc);

        obj.sections.push_back(text);

        obj.symbols.push_back({});
        ObjSymbol sym;
        sym.name = "data_sym";
        sym.binding = ObjSymbol::Undefined;
        obj.symbols.push_back(sym);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100000);

        // Place the symbol on a different page, at a specific offset.
        // sym VA = 0x102040 (page 0x102000, offset 0x40 within page).
        GlobalSymEntry entry;
        entry.name = "data_sym";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x102040;
        layout.globalSyms["data_sym"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err);
        CHECK(ok);

        // ADRP: Page(S) - Page(P) = 0x102000 - 0x100000 = 0x2000 (2 pages)
        // immhi:immlo encode page count (0x2000 >> 12 = 2).
        uint32_t adrp = readLE32(layout.sections[0].data.data());
        CHECK((adrp & 0x9F000000) == 0x90000000); // ADRP opcode
        // Extract page count: immhi = bits[23:5], immlo = bits[30:29]
        int32_t immhi = static_cast<int32_t>((adrp >> 5) & 0x7FFFF);
        int32_t immlo = static_cast<int32_t>((adrp >> 29) & 0x3);
        int32_t pageCount = (immhi << 2) | immlo;
        CHECK(pageCount == 2);

        // LDR: offset within page = 0x40, scaled by 8 = 0x40/8 = 8.
        // Encoded in bits [21:10].
        uint32_t ldr = readLE32(layout.sections[0].data.data() + 4);
        uint32_t imm12 = (ldr >> 10) & 0xFFF;
        CHECK(imm12 == 8); // 0x40 / 8 = 8
    }

    // --- PLT32 (ELF x86_64: R_X86_64_PLT32 = type 4) ---
    // PLT32 is treated identically to PC32 for static linking.
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj(
            "plt_test.o", ObjFileFormat::ELF, code, "target_fn", /*R_X86_64_PLT32=*/4, 0, -4);

        ObjFile target;
        target.name = "target.o";
        target.format = ObjFileFormat::ELF;
        target.sections.push_back({});
        ObjSection sec;
        sec.name = ".text";
        sec.data.resize(8, 0x90);
        sec.executable = true;
        sec.alloc = true;
        sec.alignment = 4;
        target.sections.push_back(sec);
        target.symbols.push_back({});
        ObjSymbol sym;
        sym.name = "target_fn";
        sym.binding = ObjSymbol::Global;
        sym.sectionIndex = 1;
        target.symbols.push_back(sym);

        std::vector<ObjFile> objs = {obj, target};
        auto layout = makeLayout(objs, 0x401000);

        GlobalSymEntry entry;
        entry.name = "target_fn";
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = 1;
        entry.secIndex = 1;
        entry.offset = 0;
        entry.resolvedAddr = 0;
        layout.globalSyms["target_fn"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(ok);

        // S = 0x401000 + 8 = 0x401008, P = 0x401000, A = -4
        // result = S + A - P = 0x401008 - 4 - 0x401000 = 4
        uint32_t patched = readLE32(layout.sections[0].data.data());
        CHECK(patched == 4);
    }

    // --- Multiple relocations in one section ---
    {
        ObjFile obj;
        obj.name = "multi_reloc.o";
        obj.format = ObjFileFormat::ELF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data.resize(16, 0);
        text.executable = true;
        text.alloc = true;
        text.alignment = 4;

        // Two PC32 relocations: one at offset 0, one at offset 8.
        ObjReloc rel1;
        rel1.offset = 0;
        rel1.type = 2; // R_X86_64_PC32
        rel1.symIndex = 1;
        rel1.addend = -4;
        text.relocs.push_back(rel1);

        ObjReloc rel2;
        rel2.offset = 8;
        rel2.type = 2;
        rel2.symIndex = 2;
        rel2.addend = -4;
        text.relocs.push_back(rel2);

        obj.sections.push_back(text);

        obj.symbols.push_back({});

        ObjSymbol sym1;
        sym1.name = "func_a";
        sym1.binding = ObjSymbol::Undefined;
        obj.symbols.push_back(sym1);

        ObjSymbol sym2;
        sym2.name = "func_b";
        sym2.binding = ObjSymbol::Undefined;
        obj.symbols.push_back(sym2);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x401000);

        GlobalSymEntry ea;
        ea.name = "func_a";
        ea.binding = GlobalSymEntry::Dynamic;
        ea.resolvedAddr = 0x402000;
        layout.globalSyms["func_a"] = ea;

        GlobalSymEntry eb;
        eb.name = "func_b";
        eb.binding = GlobalSymEntry::Dynamic;
        eb.resolvedAddr = 0x403000;
        layout.globalSyms["func_b"] = eb;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(ok);

        // Reloc 1: S + A - P = 0x402000 + (-4) - 0x401000 = 0xFFC
        uint32_t p1 = readLE32(layout.sections[0].data.data());
        CHECK(p1 == 0xFFC);

        // Reloc 2: S + A - P = 0x403000 + (-4) - 0x401008 = 0x1FF4
        uint32_t p2 = readLE32(layout.sections[0].data.data() + 8);
        CHECK(p2 == 0x1FF4);
    }

    // --- AArch64 Abs64 (ELF: R_AARCH64_ABS64 = type 257) ---
    {
        std::vector<uint8_t> code(16, 0);
        auto obj = makeObj(
            "a64_abs64.o", ObjFileFormat::ELF, code, "data_ptr", /*R_AARCH64_ABS64=*/257, 0, 0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100000);

        GlobalSymEntry entry;
        entry.name = "data_ptr";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0xDEADBEEF;
        layout.globalSyms["data_ptr"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err);
        CHECK(ok);

        // 8 bytes of absolute address.
        const uint8_t *p = layout.sections[0].data.data();
        uint64_t val = 0;
        for (int i = 0; i < 8; ++i)
            val |= static_cast<uint64_t>(p[i]) << (i * 8);
        CHECK(val == 0xDEADBEEF);
    }

    // --- AArch64 backward Branch26 (negative displacement) ---
    {
        // Target at start, caller 32 bytes later — backward branch.
        ObjFile target;
        target.name = "target.o";
        target.format = ObjFileFormat::ELF;
        target.sections.push_back({});
        ObjSection tSec;
        tSec.name = ".text";
        tSec.data.resize(32, 0x00);
        tSec.executable = true;
        tSec.alloc = true;
        tSec.alignment = 4;
        target.sections.push_back(tSec);
        target.symbols.push_back({});
        ObjSymbol tSym;
        tSym.name = "loop_top";
        tSym.binding = ObjSymbol::Global;
        tSym.sectionIndex = 1;
        tSym.offset = 0;
        target.symbols.push_back(tSym);

        // Caller: B instruction branching backward.
        std::vector<uint8_t> callerCode(4, 0);
        callerCode[3] = 0x14; // B
        auto caller = makeObj("back_br.o",
                              ObjFileFormat::ELF,
                              callerCode,
                              "loop_top",
                              /*R_AARCH64_JUMP26=*/282,
                              0,
                              0);

        std::vector<ObjFile> objs = {target, caller};
        auto layout = makeLayout(objs, 0x100000);

        GlobalSymEntry entry;
        entry.name = "loop_top";
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = 0;
        entry.secIndex = 1;
        entry.offset = 0;
        entry.resolvedAddr = 0;
        layout.globalSyms["loop_top"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err);
        CHECK(ok);

        // S = 0x100000 (target at obj 0, sec 1, offset 0)
        // P = 0x100000 + 32 = 0x100020 (caller starts after target's 32 bytes)
        // displacement = (S - P) >> 2 = (0x100000 - 0x100020) >> 2 = -0x20 >> 2 = -8
        // Signed 26-bit: -8 & 0x03FFFFFF = 0x03FFFFF8
        uint32_t patched = readLE32(layout.sections[0].data.data() + 32);
        uint32_t imm26 = patched & 0x03FFFFFF;
        // Sign-extend 26-bit to 32-bit to check value.
        int32_t disp = static_cast<int32_t>(imm26 << 6) >> 6;
        CHECK(disp == -8);
    }

    // --- AArch64 LdSt32Off (ELF: R_AARCH64_LDST32_ABS_LO12_NC = type 285) ---
    // Offset scaled by 4 (32-bit load/store).
    {
        std::vector<uint8_t> code = {0x00, 0x00, 0x40, 0xB9}; // LDR W0, [x0, #0]
        auto obj = makeObj("ldst32.o",
                           ObjFileFormat::ELF,
                           code,
                           "word_data",
                           /*R_AARCH64_LDST32_ABS_LO12_NC=*/285,
                           0,
                           0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100000);

        // Symbol at page offset 0x80: 0x80 / 4 = 32 (imm12 value).
        GlobalSymEntry entry;
        entry.name = "word_data";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x101080; // page offset = 0x80
        layout.globalSyms["word_data"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err);
        CHECK(ok);

        uint32_t patched = readLE32(layout.sections[0].data.data());
        uint32_t imm12 = (patched >> 10) & 0xFFF;
        CHECK(imm12 == 32); // 0x80 / 4 = 32
    }

    // --- Invalid relocation symbol index is a hard error ---
    {
        std::vector<uint8_t> code(4, 0);
        auto obj = makeObj("bad_sym.o", ObjFileFormat::ELF, code, "missing", 10, 0, 0);
        obj.sections[1].relocs[0].symIndex = 99;
        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100000);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(!ok);
        CHECK(err.str().find("invalid symbol index") != std::string::npos);
    }

    // --- AArch64 Branch26 rejects unaligned targets ---
    {
        std::vector<uint8_t> code(4, 0);
        code[3] = 0x14; // B
        auto obj = makeObj("unaligned_branch.o", ObjFileFormat::ELF, code, "odd", 282, 0, 0);
        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100000);

        GlobalSymEntry entry;
        entry.name = "odd";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x100002;
        layout.globalSyms["odd"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err);
        CHECK(!ok);
        CHECK(err.str().find("not instruction aligned") != std::string::npos);
    }

    // --- AArch64 scaled load/store rejects misaligned page offsets ---
    {
        std::vector<uint8_t> code = {0x00, 0x00, 0x40, 0xF9}; // LDR X0, [x0, #0]
        auto obj = makeObj("misaligned_ldst.o",
                           ObjFileFormat::ELF,
                           code,
                           "qword_data",
                           /*R_AARCH64_LDST64_ABS_LO12_NC=*/286,
                           0,
                           0);
        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100000);

        GlobalSymEntry entry;
        entry.name = "qword_data";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x101002; // not 8-byte aligned within the page
        layout.globalSyms["qword_data"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err);
        CHECK(!ok);
        CHECK(err.str().find("not aligned") != std::string::npos);
    }

    // --- Abs32 rejects overflow instead of truncating ---
    {
        std::vector<uint8_t> code(4, 0);
        auto obj = makeObj("abs32_overflow.o", ObjFileFormat::ELF, code, "too_far", 10, 0, 0);
        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100000);

        GlobalSymEntry entry;
        entry.name = "too_far";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x1'0000'0000ULL;
        layout.globalSyms["too_far"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(!ok);
        CHECK(err.str().find("32-bit absolute relocation out of range") != std::string::npos);
    }

    // --- AArch64 ELF GOT ADRP/LDR relocations use the dynamic GOT slot ---
    {
        ObjFile obj;
        obj.name = "a64_got.o";
        obj.format = ObjFileFormat::ELF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data = {0x00,
                     0x00,
                     0x00,
                     0x90, // adrp x0, #0
                     0x00,
                     0x00,
                     0x40,
                     0xF9}; // ldr x0, [x0, #0]
        text.executable = true;
        text.alloc = true;
        text.alignment = 4;

        ObjReloc gotPage;
        gotPage.offset = 0;
        gotPage.type = elf_a64::kAdrGotPage;
        gotPage.symIndex = 1;
        text.relocs.push_back(gotPage);

        ObjReloc gotOff;
        gotOff.offset = 4;
        gotOff.type = elf_a64::kLd64GotLo12Nc;
        gotOff.symIndex = 1;
        text.relocs.push_back(gotOff);

        obj.sections.push_back(text);
        obj.symbols.push_back({});
        ObjSymbol puts;
        puts.name = "puts";
        puts.binding = ObjSymbol::Undefined;
        obj.symbols.push_back(puts);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100000);

        GlobalSymEntry putsEntry;
        putsEntry.name = "puts";
        putsEntry.binding = GlobalSymEntry::Dynamic;
        putsEntry.resolvedAddr = 0x700000;
        layout.globalSyms["puts"] = putsEntry;

        GlobalSymEntry gotEntry;
        gotEntry.name = "__got_puts";
        gotEntry.binding = GlobalSymEntry::Global;
        gotEntry.resolvedAddr = 0x102040;
        gotEntry.resolvedAddrValid = true;
        layout.globalSyms["__got_puts"] = gotEntry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err);
        CHECK(ok);
        if (!err.str().empty())
            std::cerr << "  GOT err: " << err.str() << "\n";

        const uint32_t adrp = readLE32(layout.sections[0].data.data());
        const int32_t immhi = static_cast<int32_t>((adrp >> 5) & 0x7FFFF);
        const int32_t immlo = static_cast<int32_t>((adrp >> 29) & 0x3);
        CHECK(((immhi << 2) | immlo) == 2);

        const uint32_t ldr = readLE32(layout.sections[0].data.data() + 4);
        CHECK(((ldr >> 10) & 0xFFF) == 8); // 0x40 / 8-byte GOT slot scale
    }

    // --- Mach-O AArch64 POINTER_TO_GOT emits a PC-relative offset to a GOT slot ---
    {
        std::vector<uint8_t> data(8, 0);
        auto obj = makeObj("macho_a64_ptr_to_got.o",
                           ObjFileFormat::MachO,
                           data,
                           "ZTISt9exception",
                           macho_a64::kPointerToGot,
                           4,
                           0);
        obj.sections[1].relocs[0].pcrel = true;
        obj.sections[1].relocs[0].length = 2;

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100000);

        GlobalSymEntry typeinfoEntry;
        typeinfoEntry.name = "ZTISt9exception";
        typeinfoEntry.binding = GlobalSymEntry::Dynamic;
        typeinfoEntry.resolvedAddr = 0x700000;
        layout.globalSyms["ZTISt9exception"] = typeinfoEntry;

        GlobalSymEntry gotEntry;
        gotEntry.name = "__got_ZTISt9exception";
        gotEntry.binding = GlobalSymEntry::Global;
        gotEntry.resolvedAddr = 0x102040;
        gotEntry.resolvedAddrValid = true;
        layout.globalSyms["__got_ZTISt9exception"] = gotEntry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::macOS, LinkArch::AArch64, err);
        CHECK(ok);
        if (!err.str().empty())
            std::cerr << "  Mach-O GOT pointer err: " << err.str() << "\n";

        const int32_t delta = static_cast<int32_t>(readLE32(layout.sections[0].data.data() + 4));
        CHECK(delta == static_cast<int32_t>(0x102040 - 0x100004));
    }

    // --- Mach-O SUBTRACTOR pairs resolve to S(target) + A - S(subtrahend), quad and long ---
    {
        std::vector<uint8_t> data(16, 0);
        auto obj = makeObj("macho_a64_subtractor.o",
                           ObjFileFormat::MachO,
                           data,
                           "fde_target",
                           macho_a64::kUnsigned,
                           0,
                           0x10);
        ObjSymbol base;
        base.name = "fde_base";
        base.binding = ObjSymbol::Undefined;
        obj.symbols.push_back(base); // index 2
        obj.sections[1].relocs[0].length = 3;
        obj.sections[1].relocs[0].subtract = true;
        obj.sections[1].relocs[0].subSymIndex = 2;
        ObjReloc narrow = obj.sections[1].relocs[0];
        narrow.offset = 8;
        narrow.length = 2;
        narrow.addend = -4;
        obj.sections[1].relocs.push_back(narrow);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100000);

        GlobalSymEntry targetEntry;
        targetEntry.name = "fde_target";
        targetEntry.binding = GlobalSymEntry::Global;
        targetEntry.resolvedAddr = 0x100200;
        targetEntry.resolvedAddrValid = true;
        layout.globalSyms["fde_target"] = targetEntry;
        GlobalSymEntry baseEntry;
        baseEntry.name = "fde_base";
        baseEntry.binding = GlobalSymEntry::Global;
        baseEntry.resolvedAddr = 0x100080;
        baseEntry.resolvedAddrValid = true;
        layout.globalSyms["fde_base"] = baseEntry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::macOS, LinkArch::AArch64, err);
        CHECK(ok);
        if (!err.str().empty())
            std::cerr << "  Mach-O subtractor err: " << err.str() << "\n";
        CHECK(readLE64(layout.sections[0].data.data()) == 0x190);
        CHECK(readLE32(layout.sections[0].data.data() + 8) == 0x17C);
        CHECK(layout.rebaseEntries.empty());
        CHECK(layout.bindEntries.empty());
    }

    // --- Mach-O SUBTRACTOR pairs reject an undefined subtrahend ---
    {
        std::vector<uint8_t> data(8, 0);
        auto obj = makeObj("macho_a64_subtractor_undef.o",
                           ObjFileFormat::MachO,
                           data,
                           "fde_target",
                           macho_a64::kUnsigned,
                           0,
                           0);
        ObjSymbol base;
        base.name = "fde_missing";
        base.binding = ObjSymbol::Undefined;
        obj.symbols.push_back(base);
        obj.sections[1].relocs[0].length = 3;
        obj.sections[1].relocs[0].subtract = true;
        obj.sections[1].relocs[0].subSymIndex = 2;

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100000);
        GlobalSymEntry targetEntry;
        targetEntry.name = "fde_target";
        targetEntry.binding = GlobalSymEntry::Global;
        targetEntry.resolvedAddr = 0x100200;
        targetEntry.resolvedAddrValid = true;
        layout.globalSyms["fde_target"] = targetEntry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::macOS, LinkArch::AArch64, err);
        CHECK(!ok);
        CHECK(err.str().find("subtrahend") != std::string::npos);
    }

    // --- AArch64 dynamic GOT LDR pageoff rejects unaligned GOT slots ---
    {
        std::vector<uint8_t> code = {0x00, 0x00, 0x40, 0xF9}; // ldr x0, [x0, #0]
        auto obj = makeObj("a64_bad_got_pageoff.o",
                           ObjFileFormat::ELF,
                           code,
                           "puts",
                           elf_a64::kLd64GotLo12Nc,
                           0,
                           0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100000);

        GlobalSymEntry putsEntry;
        putsEntry.name = "puts";
        putsEntry.binding = GlobalSymEntry::Dynamic;
        putsEntry.resolvedAddr = 0x700000;
        layout.globalSyms["puts"] = putsEntry;

        GlobalSymEntry gotEntry;
        gotEntry.name = "__got_puts";
        gotEntry.binding = GlobalSymEntry::Global;
        gotEntry.resolvedAddr = 0x102042;
        gotEntry.resolvedAddrValid = true;
        layout.globalSyms["__got_puts"] = gotEntry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err);
        CHECK(!ok);
        CHECK(err.str().find("not aligned") != std::string::npos);
    }

    // --- Live alloc sections with relocations must be present in the output layout ---
    {
        std::vector<uint8_t> code(4, 0);
        auto obj = makeObj(
            "dropped_live_section.o", ObjFileFormat::ELF, code, "target", elf_x64::kAbs32, 0, 0);
        std::vector<ObjFile> objs = {obj};
        LinkLayout layout;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(!ok);
        CHECK(err.str().find("was not placed") != std::string::npos);
    }

    // --- COFF ADDR32NB rejects negative/out-of-range RVAs instead of truncating ---
    {
        std::vector<uint8_t> code(4, 0);
        auto obj = makeObj("bad_addr32nb.obj",
                           ObjFileFormat::COFF,
                           code,
                           "below_image",
                           coff_x64::kAddr32Nb,
                           0,
                           0);
        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x140001000ULL);

        GlobalSymEntry entry;
        entry.name = "below_image";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x1000;
        layout.globalSyms["below_image"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::X86_64, err);
        CHECK(!ok);
        CHECK(err.str().find("ADDR32NB relocation out of 32-bit range") != std::string::npos);
    }

    // --- Result ---
    if (gFail == 0) {
        std::cout << "All relocation edge-case tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << gFail << " relocation edge-case test(s) FAILED.\n";
    return EXIT_FAILURE;
}
