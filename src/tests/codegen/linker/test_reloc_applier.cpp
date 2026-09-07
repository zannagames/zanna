//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/linker/test_reloc_applier.cpp
// Purpose: Unit tests for the native linker's relocation applier — verifies
//          correct patching of PCRel32, Abs64, Branch26, ADRP Page21,
//          undefined symbol errors, and out-of-bounds detection.
// Key invariants:
//   - PCRel32: result = S + A - P
//   - Abs64: result = S + A written as 8 LE bytes
//   - Undefined symbols produce an error (not silent address 0)
//   - Out-of-bounds relocations return false
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
#include <limits>
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

/// Helper: build a minimal ObjFile with one .text section containing \p code,
/// one symbol at index 1, and one relocation.
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

    // Section 0: null (required).
    obj.sections.push_back({});

    // Section 1: .text with the provided code.
    ObjSection text;
    text.name = ".text";
    text.data = code;
    text.executable = true;
    text.alloc = true;
    text.alignment = 4;

    ObjReloc rel;
    rel.offset = relocOff;
    rel.type = relocType;
    rel.symIndex = 1; // Points to symbol at index 1.
    rel.addend = addend;
    text.relocs.push_back(rel);

    obj.sections.push_back(text);

    // Symbol 0: null (required).
    obj.symbols.push_back({});

    // Symbol 1: the target symbol.
    ObjSymbol sym;
    sym.name = symName;
    sym.binding = ObjSymbol::Undefined;
    obj.symbols.push_back(sym);

    return obj;
}

/// Helper: set up a LinkLayout with one .text output section at a given VA,
/// containing the object's section data.
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
    // --- PCRel32 (ELF x86_64: R_X86_64_PC32 = type 2) ---
    {
        // Object 0: caller with relocation referencing "target_func".
        std::vector<uint8_t> callerCode(8, 0);
        auto caller = makeObj("caller.o",
                              ObjFileFormat::ELF,
                              callerCode,
                              "target_func",
                              /*R_X86_64_PC32=*/2,
                              /*relocOff=*/0,
                              /*addend=*/-4);

        // Object 1: target defining "target_func" in its own .text section.
        ObjFile target;
        target.name = "target.o";
        target.format = ObjFileFormat::ELF;
        target.sections.push_back({}); // null section
        ObjSection targetText;
        targetText.name = ".text";
        targetText.data.resize(16, 0x90); // NOP sled
        targetText.executable = true;
        targetText.alloc = true;
        targetText.alignment = 4;
        target.sections.push_back(targetText);
        target.symbols.push_back({}); // null symbol
        ObjSymbol targetSym;
        targetSym.name = "target_func";
        targetSym.binding = ObjSymbol::Global;
        targetSym.sectionIndex = 1;
        targetSym.offset = 0;
        target.symbols.push_back(targetSym);

        std::vector<ObjFile> objs = {caller, target};
        auto layout = makeLayout(objs, 0x401000);

        // Register "target_func" in global symbol table.
        // It's in obj 1, section 1 — the first pass of applyRelocations will
        // resolve it from the layout to the correct VA.
        GlobalSymEntry entry;
        entry.name = "target_func";
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = 1;
        entry.secIndex = 1;
        entry.offset = 0;
        entry.resolvedAddr = 0; // Will be resolved by first pass.
        layout.globalSyms["target_func"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(ok);
        CHECK(err.str().empty());

        // target_func is at VA = textVA + callerCode.size() = 0x401000 + 8 = 0x401008
        // P = textVA + relocOff = 0x401000 + 0 = 0x401000
        // Expected: S + A - P = 0x401008 + (-4) - 0x401000 = 4
        uint32_t patched = readLE32(layout.sections[0].data.data());
        CHECK(patched == 4);
    }

    // --- Abs64 (ELF x86_64: R_X86_64_64 = type 1) ---
    {
        // Object 0: code with an Abs64 relocation.
        std::vector<uint8_t> code(16, 0);
        auto caller = makeObj("test_abs64.o",
                              ObjFileFormat::ELF,
                              code,
                              "global_var",
                              /*R_X86_64_64=*/1,
                              /*relocOff=*/0,
                              /*addend=*/0);

        // Object 1: defines "global_var" in a .data section.
        ObjFile dataObj;
        dataObj.name = "data.o";
        dataObj.format = ObjFileFormat::ELF;
        dataObj.sections.push_back({}); // null
        ObjSection dataSec;
        dataSec.name = ".data";
        dataSec.data.resize(8, 0x42);
        dataSec.writable = true;
        dataSec.alloc = true;
        dataSec.alignment = 8;
        dataObj.sections.push_back(dataSec);
        dataObj.symbols.push_back({}); // null
        ObjSymbol dataSym;
        dataSym.name = "global_var";
        dataSym.binding = ObjSymbol::Global;
        dataSym.sectionIndex = 1;
        dataSym.offset = 0;
        dataObj.symbols.push_back(dataSym);

        std::vector<ObjFile> objs = {caller, dataObj};
        auto layout = makeLayout(objs, 0x401000);

        // Register global symbol — first pass resolves from layout.
        GlobalSymEntry entry;
        entry.name = "global_var";
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = 1;
        entry.secIndex = 1;
        entry.offset = 0;
        entry.resolvedAddr = 0;
        layout.globalSyms["global_var"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(ok);

        // global_var is at textVA + callerCode.size() = 0x401000 + 16 = 0x401010
        uint64_t patched = readLE64(layout.sections[0].data.data());
        CHECK(patched == 0x401010);
    }

    // --- Abs64 high-bit addend wraps in the 64-bit field ---
    {
        std::vector<uint8_t> code(16, 0);
        auto caller = makeObj("test_abs64_high_bit.o",
                              ObjFileFormat::MachO,
                              code,
                              "global_var",
                              macho_a64::kUnsigned,
                              /*relocOff=*/0,
                              std::numeric_limits<int64_t>::min());

        ObjFile dataObj;
        dataObj.name = "data.o";
        dataObj.format = ObjFileFormat::MachO;
        dataObj.sections.push_back({});
        ObjSection dataSec;
        dataSec.name = "__DATA,__const";
        dataSec.data.resize(8, 0x42);
        dataSec.alloc = true;
        dataSec.alignment = 8;
        dataObj.sections.push_back(dataSec);
        dataObj.symbols.push_back({});
        ObjSymbol dataSym;
        dataSym.name = "global_var";
        dataSym.binding = ObjSymbol::Global;
        dataSym.sectionIndex = 1;
        dataSym.offset = 0;
        dataObj.symbols.push_back(dataSym);

        std::vector<ObjFile> objs = {caller, dataObj};
        auto layout = makeLayout(objs, 0x401000);

        GlobalSymEntry entry;
        entry.name = "global_var";
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = 1;
        entry.secIndex = 1;
        entry.offset = 0;
        layout.globalSyms["global_var"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::macOS, LinkArch::AArch64, err);
        CHECK(ok);
        uint64_t patched = readLE64(layout.sections[0].data.data());
        CHECK(patched == (uint64_t{0x8000000000000000} + 0x401010));
    }

    // --- Undefined symbol produces error (validates Phase 1A fix) ---
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("test_undef.o",
                           ObjFileFormat::ELF,
                           code,
                           "missing_symbol",
                           /*R_X86_64_PC32=*/2,
                           /*relocOff=*/0,
                           /*addend=*/-4);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x401000);
        // Deliberately do NOT add "missing_symbol" to globalSyms.

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(!ok);
        CHECK(err.str().find("undefined symbol") != std::string::npos);
        CHECK(err.str().find("missing_symbol") != std::string::npos);
    }

    // --- Abs64 out-of-bounds produces error (validates Phase 1B fix) ---
    {
        // Code is only 4 bytes, but Abs64 needs 8.
        std::vector<uint8_t> code(4, 0);
        auto obj = makeObj("test_oob.o",
                           ObjFileFormat::ELF,
                           code,
                           "some_sym",
                           /*R_X86_64_64=*/1,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x401000);

        // Use Dynamic binding so first pass doesn't try to resolve.
        GlobalSymEntry entry;
        entry.name = "some_sym";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x402000;
        layout.globalSyms["some_sym"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(!ok);
        CHECK(err.str().find("out of bounds") != std::string::npos);
    }

    // --- Relocation offset beyond section produces error ---
    {
        std::vector<uint8_t> code(4, 0);
        auto obj = makeObj("test_bad_off.o",
                           ObjFileFormat::ELF,
                           code,
                           "func",
                           /*R_X86_64_PC32=*/2,
                           /*relocOff=*/100,
                           /*addend=*/-4);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x401000);

        GlobalSymEntry entry;
        entry.name = "func";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x402000;
        layout.globalSyms["func"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(!ok);
        CHECK(err.str().find("relocation offset") != std::string::npos);
    }

    // --- Abs32 (ELF x86_64: R_X86_64_32 = type 10) ---
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("test_abs32.o",
                           ObjFileFormat::ELF,
                           code,
                           "small_addr",
                           /*R_X86_64_32=*/10,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x401000);

        // Use Dynamic binding so first pass doesn't recalculate resolvedAddr.
        GlobalSymEntry entry;
        entry.name = "small_addr";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x12345678;
        layout.globalSyms["small_addr"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(ok);

        uint32_t patched = readLE32(layout.sections[0].data.data());
        CHECK(patched == 0x12345678);
    }

    // --- Local defined symbols win over same-name globals ---
    {
        ObjFile obj;
        obj.name = "local_alias.o";
        obj.format = ObjFileFormat::ELF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data.resize(16, 0);
        text.executable = true;
        text.alloc = true;
        text.alignment = 4;
        ObjReloc rel;
        rel.offset = 0;
        rel.type = 1; // R_X86_64_64
        rel.symIndex = 1;
        rel.addend = 0;
        text.relocs.push_back(rel);
        obj.sections.push_back(text);

        obj.symbols.push_back({});
        ObjSymbol local;
        local.name = "dup";
        local.binding = ObjSymbol::Local;
        local.sectionIndex = 1;
        local.offset = 8;
        obj.symbols.push_back(local);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x401000);

        GlobalSymEntry global;
        global.name = "dup";
        global.binding = GlobalSymEntry::Dynamic;
        global.resolvedAddr = 0x500000;
        layout.globalSyms["dup"] = global;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err));
        CHECK(readLE64(layout.sections[0].data.data()) == 0x401008);
    }

    // --- Weak/global same-object definitions prefer the resolved global winner ---
    {
        ObjFile weakObj;
        weakObj.name = "weak_ref.o";
        weakObj.format = ObjFileFormat::ELF;
        weakObj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data.resize(8, 0);
        text.executable = true;
        text.alloc = true;
        ObjReloc rel;
        rel.offset = 0;
        rel.type = elf_x64::kAbs64;
        rel.symIndex = 1;
        text.relocs.push_back(rel);
        weakObj.sections.push_back(text);

        weakObj.symbols.push_back({});
        ObjSymbol weak;
        weak.name = "override_me";
        weak.binding = ObjSymbol::Weak;
        weak.sectionIndex = 1;
        weak.offset = 0;
        weakObj.symbols.push_back(weak);

        ObjFile strongObj;
        strongObj.name = "strong.o";
        strongObj.format = ObjFileFormat::ELF;
        strongObj.sections.push_back({});
        ObjSection strongText;
        strongText.name = ".text";
        strongText.data.resize(8, 0);
        strongText.executable = true;
        strongText.alloc = true;
        strongObj.sections.push_back(strongText);
        strongObj.symbols.push_back({});
        ObjSymbol strong;
        strong.name = "override_me";
        strong.binding = ObjSymbol::Global;
        strong.sectionIndex = 1;
        strongObj.symbols.push_back(strong);

        std::vector<ObjFile> objs = {weakObj, strongObj};
        auto layout = makeLayout(objs, 0x401000);
        layout.globalSyms["override_me"] = {"override_me", GlobalSymEntry::Global, 1, 1, 0, 0};

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err));
        CHECK(readLE64(layout.sections[0].data.data()) == 0x401008);
    }

    // --- Weak undefined references may resolve to zero only for absolute data fixups ---
    {
        auto makeWeakRef = [](uint32_t relocType, size_t width) {
            ObjFile obj;
            obj.name = "weak_undef.o";
            obj.format = ObjFileFormat::ELF;
            obj.sections.push_back({});

            ObjSection text;
            text.name = ".text";
            text.data.resize(width, 0);
            text.executable = true;
            text.alloc = true;
            ObjReloc rel;
            rel.offset = 0;
            rel.type = relocType;
            rel.symIndex = 1;
            rel.addend = relocType == elf_x64::kPC32 ? -4 : 0;
            text.relocs.push_back(rel);
            obj.sections.push_back(text);

            obj.symbols.push_back({});
            ObjSymbol weak;
            weak.name = "maybe_missing";
            weak.binding = ObjSymbol::Undefined;
            weak.weakExternal = true;
            obj.symbols.push_back(weak);
            return obj;
        };

        std::vector<ObjFile> absObjs = {makeWeakRef(elf_x64::kAbs64, 8)};
        auto absLayout = makeLayout(absObjs, 0x401000);
        std::ostringstream absErr;
        std::unordered_set<std::string> dynSyms;
        CHECK(applyRelocations(
            absObjs, absLayout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, absErr));
        CHECK(readLE64(absLayout.sections[0].data.data()) == 0);

        std::vector<ObjFile> branchObjs = {makeWeakRef(elf_x64::kPC32, 4)};
        auto branchLayout = makeLayout(branchObjs, 0x401000);
        std::ostringstream branchErr;
        CHECK(!applyRelocations(
            branchObjs, branchLayout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, branchErr));
        CHECK(branchErr.str().find(
                  "weak undefined symbol 'maybe_missing' cannot resolve to address zero") !=
              std::string::npos);
    }

    // --- Mach-O TLV descriptor offsets must convert to TLS-relative values ---
    {
        ObjFile obj;
        obj.name = "bad_tlv.o";
        obj.format = ObjFileFormat::MachO;
        obj.sections.push_back({});
        ObjSection tdata;
        tdata.name = ".tdata";
        tdata.data.resize(8, 0);
        tdata.alloc = true;
        tdata.writable = true;
        tdata.tls = true;
        ObjReloc rel;
        rel.offset = 0;
        rel.type = macho_a64::kUnsigned;
        rel.symIndex = 1;
        rel.length = 3;
        tdata.relocs.push_back(rel);
        obj.sections.push_back(tdata);
        obj.symbols.push_back({});
        ObjSymbol sym;
        sym.name = "tlv_init";
        sym.binding = ObjSymbol::Local;
        sym.absolute = true;
        sym.offset = 0x1234;
        obj.symbols.push_back(sym);

        std::vector<ObjFile> objs = {obj};
        LinkLayout layout;
        OutputSection out;
        out.name = ".tdata";
        out.data.resize(8, 0);
        out.virtualAddr = 0x100000000;
        out.alloc = true;
        out.writable = true;
        out.tls = true;
        out.chunks.push_back(InputChunk{0, 1, 0, 8});
        layout.sections.push_back(out);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(
            !applyRelocations(objs, layout, dynSyms, LinkPlatform::macOS, LinkArch::AArch64, err));
        CHECK(err.str().find("could not be converted to TLS-relative") != std::string::npos);
    }

    // --- Mach-O GOT pointer rebases include read-only data segments ---
    {
        ObjFile obj;
        obj.name = "got_data_const.o";
        obj.format = ObjFileFormat::MachO;
        obj.sections.push_back({});
        ObjSection data;
        data.name = "__DATA_CONST,__got";
        data.data.resize(8, 0);
        data.alloc = true;
        data.dataSegment = true;
        ObjReloc rel;
        rel.offset = 0;
        rel.type = macho_a64::kPointerToGot;
        rel.symIndex = 1;
        rel.length = 3;
        data.relocs.push_back(rel);
        obj.sections.push_back(data);
        obj.symbols.push_back({});
        ObjSymbol sym;
        sym.name = "target";
        sym.binding = ObjSymbol::Undefined;
        obj.symbols.push_back(sym);

        std::vector<ObjFile> objs = {obj};
        LinkLayout layout;
        OutputSection out;
        out.name = "__DATA_CONST,__got";
        out.data.resize(8, 0);
        out.virtualAddr = 0x100002000;
        out.alloc = true;
        out.dataSegment = true;
        out.chunks.push_back(InputChunk{0, 1, 0, 8});
        layout.sections.push_back(out);
        GlobalSymEntry got;
        got.name = "__got_target";
        got.binding = GlobalSymEntry::Global;
        got.resolvedAddr = 0x100003000;
        got.resolvedAddrValid = true;
        layout.globalSyms["__got_target"] = got;
        GlobalSymEntry targetEntry;
        targetEntry.name = "target";
        targetEntry.binding = GlobalSymEntry::Global;
        targetEntry.resolvedAddr = 0x100004000;
        targetEntry.resolvedAddrValid = true;
        layout.globalSyms["target"] = targetEntry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(applyRelocations(objs, layout, dynSyms, LinkPlatform::macOS, LinkArch::AArch64, err));
        CHECK(readLE64(layout.sections[0].data.data()) == 0x100003000);
        CHECK(layout.rebaseEntries.size() == 1);
    }

    // --- Duplicate input-section chunks are rejected before relocation patching ---
    {
        ObjFile obj;
        obj.name = "duplicate_chunk.o";
        obj.format = ObjFileFormat::ELF;
        obj.sections.push_back({});
        ObjSection text;
        text.name = ".text";
        text.data = {0xC3};
        text.executable = true;
        text.alloc = true;
        obj.sections.push_back(text);
        obj.symbols.push_back({});

        std::vector<ObjFile> objs = {obj};
        LinkLayout layout;
        OutputSection a;
        a.name = ".text.a";
        a.executable = true;
        a.data = text.data;
        a.chunks.push_back({0, 1, 0, text.data.size()});
        layout.sections.push_back(a);
        OutputSection b;
        b.name = ".text.b";
        b.executable = true;
        b.data = text.data;
        b.chunks.push_back({0, 1, 0, text.data.size()});
        layout.sections.push_back(b);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(!applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err));
        CHECK(err.str().find("appears in multiple output chunks") != std::string::npos);
    }

    // --- COFF x86_64 ADDR32NB writes an RVA (S + A - ImageBase) ---
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("test_addr32nb.obj",
                           ObjFileFormat::COFF,
                           code,
                           "unwind_info",
                           /*IMAGE_REL_AMD64_ADDR32NB=*/3,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x140001000ULL);

        GlobalSymEntry entry;
        entry.name = "unwind_info";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x140002000ULL;
        layout.globalSyms["unwind_info"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::X86_64, err);
        CHECK(ok);

        uint32_t patched = readLE32(layout.sections[0].data.data());
        CHECK(patched == 0x2000);
    }

    // --- COFF x86_64 REL32 is relative to the end of the relocated field ---
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("test_rel32.obj",
                           ObjFileFormat::COFF,
                           code,
                           "target",
                           /*IMAGE_REL_AMD64_REL32=*/4,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x140001000ULL);

        GlobalSymEntry entry;
        entry.name = "target";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x140002000ULL;
        layout.globalSyms["target"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::X86_64, err);
        CHECK(ok);

        uint32_t patched = readLE32(layout.sections[0].data.data());
        CHECK(patched == 0x00000FFC);
    }

    // --- Windows __ImageBase must use the PE image base, not a dynamic zero ---
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("test_imagebase_rel32.obj",
                           ObjFileFormat::COFF,
                           code,
                           "__ImageBase",
                           /*IMAGE_REL_AMD64_REL32=*/4,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x140001000ULL);

        // __ImageBase often survives symbol resolution as a dynamic entry.
        // The relocation applier must still resolve it to the PE image base.
        GlobalSymEntry entry;
        entry.name = "__ImageBase";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0;
        layout.globalSyms["__ImageBase"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::X86_64, err);
        CHECK(ok);

        uint32_t patched = readLE32(layout.sections[0].data.data());
        CHECK(patched == 0xFFFFEFFC);
    }

    // --- Mach-O x86_64 SIGNED_4 applies its trailing-instruction bias ---
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("test_macho_signed4.o",
                           ObjFileFormat::MachO,
                           code,
                           "target",
                           macho_x64::kSigned4,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100001000ULL);

        GlobalSymEntry entry;
        entry.name = "target";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x100001010ULL;
        layout.globalSyms["target"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(applyRelocations(objs, layout, dynSyms, LinkPlatform::macOS, LinkArch::X86_64, err));
        CHECK(readLE32(layout.sections[0].data.data()) == 12);
    }

    // --- F1: Mach-O x86_64 SIGNED_1 (corrected constant = 6) uses a 1-byte bias ---
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("test_macho_signed1.o",
                           ObjFileFormat::MachO,
                           code,
                           "target",
                           macho_x64::kSigned1,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x100001000ULL);

        GlobalSymEntry entry;
        entry.name = "target";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x100001010ULL;
        layout.globalSyms["target"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(applyRelocations(objs, layout, dynSyms, LinkPlatform::macOS, LinkArch::X86_64, err));
        // delta = 0x10 (16); extraBias for SIGNED_1 = 1 → 15.
        CHECK(readLE32(layout.sections[0].data.data()) == 15);
    }

    // --- F9: an Abs64 fixup that overflows its input chunk into the next chunk is
    //         rejected, even though it fits within the merged output section ---
    {
        std::vector<uint8_t> code(4, 0); // 4-byte chunk, smaller than the 8-byte fixup
        auto caller = makeObj("test_chunk_overflow.o",
                              ObjFileFormat::ELF,
                              code,
                              "gv",
                              /*R_X86_64_64=*/1,
                              /*relocOff=*/0,
                              /*addend=*/0);

        ObjFile dataObj;
        dataObj.name = "data.o";
        dataObj.format = ObjFileFormat::ELF;
        dataObj.sections.push_back({});
        ObjSection dataSec;
        dataSec.name = ".data";
        dataSec.data.resize(8, 0x42);
        dataSec.writable = true;
        dataSec.alloc = true;
        dataSec.alignment = 8;
        dataObj.sections.push_back(dataSec);
        dataObj.symbols.push_back({});
        ObjSymbol dataSym;
        dataSym.name = "gv";
        dataSym.binding = ObjSymbol::Global;
        dataSym.sectionIndex = 1;
        dataObj.symbols.push_back(dataSym);

        std::vector<ObjFile> objs = {caller, dataObj};
        auto layout = makeLayout(objs, 0x401000); // output section = 4 + 8 = 12 bytes

        GlobalSymEntry entry;
        entry.name = "gv";
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = 1;
        entry.secIndex = 1;
        layout.globalSyms["gv"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(!ok);
        CHECK(err.str().find("out of bounds") != std::string::npos);
    }

    // --- COFF x86_64 REL32_4 includes the trailing-byte bias ---
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("test_rel32_4.obj",
                           ObjFileFormat::COFF,
                           code,
                           "target",
                           /*IMAGE_REL_AMD64_REL32_4=*/8,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x140001000ULL);

        GlobalSymEntry entry;
        entry.name = "target";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x140002000ULL;
        layout.globalSyms["target"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::X86_64, err);
        CHECK(ok);

        uint32_t patched = readLE32(layout.sections[0].data.data());
        CHECK(patched == 0x00000FF8);
    }

    // --- Anonymous unresolved relocation must fail instead of patching address zero ---
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("test_anon_undef.o",
                           ObjFileFormat::ELF,
                           code,
                           "",
                           /*R_X86_64_64=*/1,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x401000);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(!ok);
        CHECK(err.str().find("<anonymous section symbol>") != std::string::npos);
    }

    // --- COFF SECREL uses symbol provenance and accepts end-of-section symbols ---
    {
        ObjFile obj;
        obj.name = "test_secrel_end.obj";
        obj.format = ObjFileFormat::COFF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data.resize(4, 0);
        text.executable = true;
        text.alloc = true;
        ObjReloc rel;
        rel.offset = 0;
        rel.type = 11; // IMAGE_REL_AMD64_SECREL
        rel.symIndex = 1;
        text.relocs.push_back(rel);
        obj.sections.push_back(text);

        ObjSection data;
        data.name = ".data";
        data.data.resize(4, 0xAA);
        data.writable = true;
        data.alloc = true;
        obj.sections.push_back(data);

        obj.symbols.push_back({});
        ObjSymbol endSym;
        endSym.name = "data_end";
        endSym.binding = ObjSymbol::Global;
        endSym.sectionIndex = 2;
        endSym.offset = 4;
        obj.symbols.push_back(endSym);

        std::vector<ObjFile> objs = {obj};
        LinkLayout layout;
        layout.pageSize = 0x1000;
        OutputSection outText;
        outText.name = ".text";
        outText.executable = true;
        outText.virtualAddr = 0x140001000ULL;
        outText.data = text.data;
        outText.chunks.push_back({0, 1, 0, text.data.size()});
        layout.sections.push_back(outText);

        OutputSection outData;
        outData.name = ".data";
        outData.writable = true;
        outData.virtualAddr = 0x140002000ULL;
        outData.data = data.data;
        outData.chunks.push_back({0, 2, 0, data.data.size()});
        layout.sections.push_back(outData);
        layout.globalSyms["data_end"] = {"data_end", GlobalSymEntry::Global, 0, 2, 4, 0};

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::X86_64, err);
        CHECK(ok);
        CHECK(readLE32(layout.sections[0].data.data()) == 4);
    }

    // --- COFF SECTION may patch a 2-byte field at the end of a chunk ---
    {
        ObjFile obj;
        obj.name = "test_section_tail.obj";
        obj.format = ObjFileFormat::COFF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data.resize(4, 0);
        text.executable = true;
        text.alloc = true;
        ObjReloc rel;
        rel.offset = 2;
        rel.type = coff_x64::kSection;
        rel.symIndex = 1;
        text.relocs.push_back(rel);
        obj.sections.push_back(text);

        obj.symbols.push_back({});
        ObjSymbol target;
        target.name = "target";
        target.binding = ObjSymbol::Global;
        target.sectionIndex = 1;
        obj.symbols.push_back(target);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x140001000ULL);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::X86_64, err));
        CHECK(layout.sections[0].data[2] == 1);
        CHECK(layout.sections[0].data[3] == 0);
    }

    // --- Local symbols are bounded by their input chunk, not the merged output section ---
    {
        ObjFile bad;
        bad.name = "bad_local_offset.o";
        bad.format = ObjFileFormat::ELF;
        bad.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data.resize(4, 0);
        text.executable = true;
        text.alloc = true;
        ObjReloc rel;
        rel.offset = 0;
        rel.type = elf_x64::kAbs32;
        rel.symIndex = 1;
        text.relocs.push_back(rel);
        bad.sections.push_back(text);

        bad.symbols.push_back({});
        ObjSymbol local;
        local.name = "bad_local";
        local.binding = ObjSymbol::Local;
        local.sectionIndex = 1;
        local.offset = 6;
        bad.symbols.push_back(local);

        ObjFile filler;
        filler.name = "filler.o";
        filler.format = ObjFileFormat::ELF;
        filler.sections.push_back({});
        ObjSection fillerText;
        fillerText.name = ".text";
        fillerText.data.resize(4, 0);
        fillerText.executable = true;
        fillerText.alloc = true;
        filler.sections.push_back(fillerText);
        filler.symbols.push_back({});

        std::vector<ObjFile> objs = {bad, filler};
        auto layout = makeLayout(objs, 0x401000);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(!applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err));
    }

    // --- Dynamic symbol set creates runtime bind entries even without a synthetic GOT symbol ---
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("test_dynamic_abs64.o",
                           ObjFileFormat::ELF,
                           code,
                           "imported_data",
                           elf_x64::kAbs64,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x401000);
        layout.globalSyms["imported_data"] = {
            "imported_data", GlobalSymEntry::Dynamic, 0, 0, 0, 0x500000};

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms = {"imported_data"};
        CHECK(applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err));
        CHECK(readLE64(layout.sections[0].data.data()) == 0);
        CHECK(layout.bindEntries.size() == 1);
        if (!layout.bindEntries.empty())
            CHECK(layout.bindEntries[0].symbolName == "imported_data");
    }

    // --- ELF x86_64 TPOFF32 uses the end of the merged TLS image as the TP base ---
    {
        ObjFile obj;
        obj.name = "test_tpoff32.o";
        obj.format = ObjFileFormat::ELF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data.resize(8, 0);
        text.executable = true;
        text.alloc = true;
        text.alignment = 4;
        ObjReloc rel;
        rel.offset = 0;
        rel.type = elf_x64::kTpoff32;
        rel.symIndex = 1;
        text.relocs.push_back(rel);
        obj.sections.push_back(text);

        ObjSection tdata;
        tdata.name = ".tdata";
        tdata.data.resize(8, 0xAA);
        tdata.writable = true;
        tdata.alloc = true;
        tdata.tls = true;
        tdata.alignment = 8;
        obj.sections.push_back(tdata);

        ObjSection tbss;
        tbss.name = ".tbss";
        tbss.memSize = 16;
        tbss.writable = true;
        tbss.alloc = true;
        tbss.tls = true;
        tbss.zeroFill = true;
        tbss.alignment = 8;
        obj.sections.push_back(tbss);

        obj.symbols.push_back({});
        ObjSymbol tlsSym;
        tlsSym.name = "tls_var";
        tlsSym.binding = ObjSymbol::Local;
        tlsSym.sectionIndex = 3;
        tlsSym.offset = 0;
        obj.symbols.push_back(tlsSym);

        std::vector<ObjFile> objs = {obj};

        LinkLayout layout;
        layout.pageSize = 0x1000;

        OutputSection outText;
        outText.name = ".text";
        outText.executable = true;
        outText.virtualAddr = 0x401000;
        outText.data = text.data;
        outText.chunks.push_back({0, 1, 0, text.data.size()});
        layout.sections.push_back(outText);

        OutputSection outTdata;
        outTdata.name = ".tdata";
        outTdata.writable = true;
        outTdata.tls = true;
        outTdata.virtualAddr = 0x402000;
        outTdata.data = tdata.data;
        outTdata.chunks.push_back({0, 2, 0, tdata.data.size()});
        layout.sections.push_back(outTdata);

        OutputSection outTbss;
        outTbss.name = ".tbss";
        outTbss.writable = true;
        outTbss.tls = true;
        outTbss.zeroFill = true;
        outTbss.virtualAddr = 0x403000;
        outTbss.memSize = tbss.memSize;
        outTbss.chunks.push_back({0, 3, 0, tbss.memSize});
        layout.sections.push_back(outTbss);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err));
        CHECK(static_cast<int32_t>(readLE32(layout.sections[0].data.data())) == -16);
    }

    // --- ELF x86_64 GOTPCRELX uses synthesized GOT slots for dynamic imports ---
    {
        std::vector<uint8_t> code = {0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00};
        auto obj = makeObj("test_gotpcrelx.o",
                           ObjFileFormat::ELF,
                           code,
                           "stderr",
                           elf_x64::kRexGotPcRelX,
                           /*relocOff=*/3,
                           /*addend=*/-4);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x401000);

        GlobalSymEntry dynEntry;
        dynEntry.name = "stderr";
        dynEntry.binding = GlobalSymEntry::Dynamic;
        dynEntry.resolvedAddr = 0x700000;
        layout.globalSyms["stderr"] = dynEntry;

        GlobalSymEntry gotEntry;
        gotEntry.name = "__got_stderr";
        gotEntry.binding = GlobalSymEntry::Global;
        gotEntry.resolvedAddr = 0x402000;
        gotEntry.resolvedAddrValid = true;
        layout.globalSyms["__got_stderr"] = gotEntry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms = {"stderr"};
        CHECK(applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err));
        CHECK(layout.sections[0].data[1] == 0x8B);
        CHECK(readLE32(layout.sections[0].data.data() + 3) == 0x00000FF9);
    }

    // --- ELF x86_64 GOTPCRELX relaxes local MOV loads into LEA ---
    {
        ObjFile obj;
        obj.name = "test_local_gotpcrelx.o";
        obj.format = ObjFileFormat::ELF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data = {0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00};
        text.executable = true;
        text.alloc = true;
        text.alignment = 4;
        ObjReloc rel;
        rel.offset = 3;
        rel.type = elf_x64::kRexGotPcRelX;
        rel.symIndex = 1;
        rel.addend = -4;
        text.relocs.push_back(rel);
        obj.sections.push_back(text);

        ObjSection data;
        data.name = ".data";
        data.data.resize(8, 0xAA);
        data.writable = true;
        data.alloc = true;
        data.alignment = 8;
        obj.sections.push_back(data);

        obj.symbols.push_back({});
        ObjSymbol local;
        local.name = "local_data";
        local.binding = ObjSymbol::Local;
        local.sectionIndex = 2;
        local.offset = 0;
        obj.symbols.push_back(local);

        std::vector<ObjFile> objs = {obj};
        LinkLayout layout;
        layout.pageSize = 0x1000;

        OutputSection outText;
        outText.name = ".text";
        outText.executable = true;
        outText.virtualAddr = 0x401000;
        outText.data = text.data;
        outText.chunks.push_back({0, 1, 0, text.data.size()});
        layout.sections.push_back(outText);

        OutputSection outData;
        outData.name = ".data";
        outData.writable = true;
        outData.virtualAddr = 0x402000;
        outData.data = data.data;
        outData.chunks.push_back({0, 2, 0, data.data.size()});
        layout.sections.push_back(outData);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err));
        CHECK(layout.sections[0].data[1] == 0x8D);
        CHECK(readLE32(layout.sections[0].data.data() + 3) == 0x00000FF9);
    }

    // --- ELF x86_64 GOTPCRELX rejects non-RIP-relative MOV encodings ---
    {
        ObjFile obj;
        obj.name = "test_bad_gotpcrelx_modrm.o";
        obj.format = ObjFileFormat::ELF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data = {0x48, 0x8B, 0x04, 0x00, 0x00, 0x00, 0x00};
        text.executable = true;
        text.alloc = true;
        ObjReloc rel;
        rel.offset = 3;
        rel.type = elf_x64::kRexGotPcRelX;
        rel.symIndex = 1;
        rel.addend = -4;
        text.relocs.push_back(rel);
        obj.sections.push_back(text);

        ObjSection data;
        data.name = ".data";
        data.data.resize(8, 0xAA);
        data.writable = true;
        data.alloc = true;
        obj.sections.push_back(data);

        obj.symbols.push_back({});
        ObjSymbol local;
        local.name = "local_data";
        local.binding = ObjSymbol::Local;
        local.sectionIndex = 2;
        obj.symbols.push_back(local);

        std::vector<ObjFile> objs = {obj};
        LinkLayout layout;
        layout.pageSize = 0x1000;
        OutputSection outText;
        outText.name = ".text";
        outText.executable = true;
        outText.virtualAddr = 0x401000;
        outText.data = text.data;
        outText.chunks.push_back({0, 1, 0, text.data.size()});
        layout.sections.push_back(outText);
        OutputSection outData;
        outData.name = ".data";
        outData.writable = true;
        outData.virtualAddr = 0x402000;
        outData.data = data.data;
        outData.chunks.push_back({0, 2, 0, data.data.size()});
        layout.sections.push_back(outData);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(!applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err));
        CHECK(err.str().find("requires a disp32(%rip) memory operand") != std::string::npos);
    }

    // --- ELF x86_64 REX_GOTPCRELX rejects MOVs without a REX prefix ---
    {
        ObjFile obj;
        obj.name = "test_bad_rex_gotpcrelx.o";
        obj.format = ObjFileFormat::ELF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data = {0x8B, 0x05, 0x00, 0x00, 0x00, 0x00};
        text.executable = true;
        text.alloc = true;
        ObjReloc rel;
        rel.offset = 2;
        rel.type = elf_x64::kRexGotPcRelX;
        rel.symIndex = 1;
        rel.addend = -4;
        text.relocs.push_back(rel);
        obj.sections.push_back(text);

        ObjSection data;
        data.name = ".data";
        data.data.resize(8, 0xAA);
        data.writable = true;
        data.alloc = true;
        obj.sections.push_back(data);

        obj.symbols.push_back({});
        ObjSymbol local;
        local.name = "local_data";
        local.binding = ObjSymbol::Local;
        local.sectionIndex = 2;
        obj.symbols.push_back(local);

        std::vector<ObjFile> objs = {obj};
        LinkLayout layout;
        layout.pageSize = 0x1000;
        OutputSection outText;
        outText.name = ".text";
        outText.executable = true;
        outText.virtualAddr = 0x401000;
        outText.data = text.data;
        outText.chunks.push_back({0, 1, 0, text.data.size()});
        layout.sections.push_back(outText);
        OutputSection outData;
        outData.name = ".data";
        outData.writable = true;
        outData.virtualAddr = 0x402000;
        outData.data = data.data;
        outData.chunks.push_back({0, 2, 0, data.data.size()});
        layout.sections.push_back(outData);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(!applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err));
        CHECK(err.str().find("with REX_GOTPCRELX requires a REX prefix") != std::string::npos);
    }

    // --- ELF x86_64 GOTPCRELX relaxes local CMP / CALL / JMP forms (psABI B.2) ---
    // Clang 18 emits `cmpq foo@GOTPCREL(%rip), %rax` with REX_GOTPCRELX for
    // pointer comparisons against local objects (rt_rendertarget3d.c); the
    // linker used to reject every non-MOV form.
    {
        struct Case {
            const char *name;
            std::vector<uint8_t> code;
            size_t relocOffset;
            uint32_t relocType;
            std::vector<uint8_t> expectPrefix; // bytes before the 32-bit field
            size_t fieldOffset;
            uint32_t expectField;
            std::vector<uint8_t> expectSuffix; // bytes after the field
        };

        // .text at 0x401000, .data (local_data) at 0x402000.
        const std::vector<Case> cases = {
            // cmpq foo@GOTPCREL(%rip), %r8  ->  cmpq $0x402000, %r8  (REX.R -> REX.B)
            {"cmp",
             {0x4C, 0x3B, 0x05, 0x00, 0x00, 0x00, 0x00},
             3,
             elf_x64::kRexGotPcRelX,
             {0x49, 0x81, 0xF8},
             3,
             0x00402000u,
             {}},
            // testq %rax, foo@GOTPCREL(%rip)  ->  testq $0x402000, %rax
            {"test",
             {0x48, 0x85, 0x05, 0x00, 0x00, 0x00, 0x00},
             3,
             elf_x64::kRexGotPcRelX,
             {0x48, 0xF7, 0xC0},
             3,
             0x00402000u,
             {}},
            // call *foo@GOTPCREL(%rip)  ->  addr32 call foo   (rel32 from 0x401006)
            {"call",
             {0xFF, 0x15, 0x00, 0x00, 0x00, 0x00},
             2,
             elf_x64::kGotPcRelX,
             {0x67, 0xE8},
             2,
             0x00000FFAu,
             {}},
            // jmp *foo@GOTPCREL(%rip)  ->  jmp foo; nop   (rel32 from 0x401005)
            {"jmp",
             {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00},
             2,
             elf_x64::kGotPcRelX,
             {0xE9},
             1,
             0x00000FFBu,
             {0x90}},
        };
        for (const auto &c : cases) {
            ObjFile obj;
            obj.name = std::string("test_local_gotpcrelx_") + c.name + ".o";
            obj.format = ObjFileFormat::ELF;
            obj.sections.push_back({});

            ObjSection text;
            text.name = ".text";
            text.data = c.code;
            text.executable = true;
            text.alloc = true;
            text.alignment = 4;
            ObjReloc rel;
            rel.offset = c.relocOffset;
            rel.type = c.relocType;
            rel.symIndex = 1;
            rel.addend = -4;
            text.relocs.push_back(rel);
            obj.sections.push_back(text);

            ObjSection data;
            data.name = ".data";
            data.data.resize(8, 0xAA);
            data.writable = true;
            data.alloc = true;
            data.alignment = 8;
            obj.sections.push_back(data);

            obj.symbols.push_back({});
            ObjSymbol local;
            local.name = "local_data";
            local.binding = ObjSymbol::Local;
            local.sectionIndex = 2;
            local.offset = 0;
            obj.symbols.push_back(local);

            std::vector<ObjFile> objs = {obj};
            LinkLayout layout;
            layout.pageSize = 0x1000;

            OutputSection outText;
            outText.name = ".text";
            outText.executable = true;
            outText.virtualAddr = 0x401000;
            outText.data = text.data;
            outText.chunks.push_back({0, 1, 0, text.data.size()});
            layout.sections.push_back(outText);

            OutputSection outData;
            outData.name = ".data";
            outData.writable = true;
            outData.virtualAddr = 0x402000;
            outData.data = data.data;
            outData.chunks.push_back({0, 2, 0, data.data.size()});
            layout.sections.push_back(outData);

            std::ostringstream err;
            std::unordered_set<std::string> dynSyms;
            const bool ok =
                applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
            CHECK(ok);
            if (!ok) {
                std::cerr << "GOTPCRELX case '" << c.name << "': " << err.str();
                continue;
            }
            const auto &out = layout.sections[0].data;
            for (size_t i = 0; i < c.expectPrefix.size(); ++i)
                CHECK(out[i] == c.expectPrefix[i]);
            CHECK(readLE32(out.data() + c.fieldOffset) == c.expectField);
            for (size_t i = 0; i < c.expectSuffix.size(); ++i)
                CHECK(out[c.fieldOffset + 4 + i] == c.expectSuffix[i]);
        }
    }

    // --- ELF AArch64 local GOT relaxation rewrites a validated ADRP/LDR pair ---
    {
        ObjFile obj;
        obj.name = "test_a64_local_got_pair.o";
        obj.format = ObjFileFormat::ELF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data = {0x00, 0x00, 0x00, 0x90, 0x01, 0x00, 0x40, 0xF9}; // ADRP x0; LDR x1,[x0]
        text.executable = true;
        text.alloc = true;
        ObjReloc page;
        page.offset = 0;
        page.type = elf_a64::kAdrGotPage;
        page.symIndex = 1;
        ObjReloc off;
        off.offset = 4;
        off.type = elf_a64::kLd64GotLo12Nc;
        off.symIndex = 1;
        text.relocs.push_back(page);
        text.relocs.push_back(off);
        obj.sections.push_back(text);

        ObjSection data;
        data.name = ".data";
        data.data.resize(16, 0xAA);
        data.writable = true;
        data.alloc = true;
        obj.sections.push_back(data);

        obj.symbols.push_back({});
        ObjSymbol local;
        local.name = "local_data";
        local.binding = ObjSymbol::Local;
        local.sectionIndex = 2;
        local.offset = 8;
        obj.symbols.push_back(local);

        std::vector<ObjFile> objs = {obj};
        LinkLayout layout;
        layout.pageSize = 0x1000;
        OutputSection outText;
        outText.name = ".text";
        outText.executable = true;
        outText.virtualAddr = 0x401000;
        outText.data = text.data;
        outText.chunks.push_back({0, 1, 0, text.data.size()});
        layout.sections.push_back(outText);
        OutputSection outData;
        outData.name = ".data";
        outData.writable = true;
        outData.virtualAddr = 0x402000;
        outData.data = data.data;
        outData.chunks.push_back({0, 2, 0, data.data.size()});
        layout.sections.push_back(outData);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err));
        CHECK(readLE32(layout.sections[0].data.data() + 4) == 0x91002001u);
    }

    // --- ELF AArch64 local GOT relaxation requires the matching ADRP relocation ---
    {
        ObjFile obj;
        obj.name = "test_a64_local_got_missing_adrp.o";
        obj.format = ObjFileFormat::ELF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data = {0x00, 0x00, 0x40, 0xF9}; // LDR x0,[x0]
        text.executable = true;
        text.alloc = true;
        ObjReloc rel;
        rel.offset = 0;
        rel.type = elf_a64::kLd64GotLo12Nc;
        rel.symIndex = 1;
        text.relocs.push_back(rel);
        obj.sections.push_back(text);

        ObjSection data;
        data.name = ".data";
        data.data.resize(8, 0xAA);
        data.writable = true;
        data.alloc = true;
        obj.sections.push_back(data);

        obj.symbols.push_back({});
        ObjSymbol local;
        local.name = "local_data";
        local.binding = ObjSymbol::Local;
        local.sectionIndex = 2;
        obj.symbols.push_back(local);

        std::vector<ObjFile> objs = {obj};
        LinkLayout layout;
        layout.pageSize = 0x1000;
        OutputSection outText;
        outText.name = ".text";
        outText.executable = true;
        outText.virtualAddr = 0x401000;
        outText.data = text.data;
        outText.chunks.push_back({0, 1, 0, text.data.size()});
        layout.sections.push_back(outText);
        OutputSection outData;
        outData.name = ".data";
        outData.writable = true;
        outData.virtualAddr = 0x402000;
        outData.data = data.data;
        outData.chunks.push_back({0, 2, 0, data.data.size()});
        layout.sections.push_back(outData);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(
            !applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err));
        CHECK(err.str().find("requires a preceding matching ADRP relocation using the LDR base "
                             "register") != std::string::npos);
    }

    // --- Mach-O AArch64 TLVP page-offset relaxation accepts its PAGE21 pair ---
    {
        ObjFile obj;
        obj.name = "test_macho_tlvp_pair.o";
        obj.format = ObjFileFormat::MachO;
        obj.sections.push_back({});

        ObjSection text;
        text.name = "__TEXT,__text";
        text.data = {0x00, 0x00, 0x00, 0x90, 0x01, 0x00, 0x40, 0xF9}; // ADRP x0; LDR x1,[x0]
        text.executable = true;
        text.alloc = true;
        ObjReloc page;
        page.offset = 0;
        page.type = macho_a64::kTlvpLoadPage21;
        page.symIndex = 1;
        ObjReloc off;
        off.offset = 4;
        off.type = macho_a64::kTlvpLoadPageOff12;
        off.symIndex = 1;
        text.relocs.push_back(page);
        text.relocs.push_back(off);
        obj.sections.push_back(text);

        ObjSection data;
        data.name = "__DATA,__thread_vars";
        data.data.resize(16, 0xAA);
        data.writable = true;
        data.alloc = true;
        data.tls = true;
        obj.sections.push_back(data);

        obj.symbols.push_back({});
        ObjSymbol local;
        local.name = "tls_descriptor";
        local.binding = ObjSymbol::Local;
        local.sectionIndex = 2;
        local.offset = 8;
        obj.symbols.push_back(local);

        std::vector<ObjFile> objs = {obj};
        LinkLayout layout;
        layout.pageSize = 0x4000;
        OutputSection outText;
        outText.name = ".text";
        outText.executable = true;
        outText.virtualAddr = 0x100004000ULL;
        outText.data = text.data;
        outText.chunks.push_back({0, 1, 0, text.data.size()});
        layout.sections.push_back(outText);
        OutputSection outData;
        outData.name = ".tdata";
        outData.writable = true;
        outData.tls = true;
        outData.virtualAddr = 0x100008000ULL;
        outData.data = data.data;
        outData.chunks.push_back({0, 2, 0, data.data.size()});
        layout.sections.push_back(outData);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(applyRelocations(objs, layout, dynSyms, LinkPlatform::macOS, LinkArch::AArch64, err));
        CHECK(readLE32(layout.sections[0].data.data() + 4) == 0x91002001u);
    }

    // --- ELF AArch64 local GOT relaxation requires LDR base to match ADRP result ---
    {
        ObjFile obj;
        obj.name = "test_a64_local_got_mismatched_regs.o";
        obj.format = ObjFileFormat::ELF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.data = {0x01, 0x00, 0x00, 0x90, 0x00, 0x00, 0x40, 0xF9}; // ADRP x1; LDR x0,[x0]
        text.executable = true;
        text.alloc = true;
        ObjReloc page;
        page.offset = 0;
        page.type = elf_a64::kAdrGotPage;
        page.symIndex = 1;
        ObjReloc off;
        off.offset = 4;
        off.type = elf_a64::kLd64GotLo12Nc;
        off.symIndex = 1;
        text.relocs.push_back(page);
        text.relocs.push_back(off);
        obj.sections.push_back(text);

        ObjSection data;
        data.name = ".data";
        data.data.resize(8, 0xAA);
        data.writable = true;
        data.alloc = true;
        obj.sections.push_back(data);

        obj.symbols.push_back({});
        ObjSymbol local;
        local.name = "local_data";
        local.binding = ObjSymbol::Local;
        local.sectionIndex = 2;
        obj.symbols.push_back(local);

        std::vector<ObjFile> objs = {obj};
        LinkLayout layout;
        layout.pageSize = 0x1000;
        OutputSection outText;
        outText.name = ".text";
        outText.executable = true;
        outText.virtualAddr = 0x401000;
        outText.data = text.data;
        outText.chunks.push_back({0, 1, 0, text.data.size()});
        layout.sections.push_back(outText);
        OutputSection outData;
        outData.name = ".data";
        outData.writable = true;
        outData.virtualAddr = 0x402000;
        outData.data = data.data;
        outData.chunks.push_back({0, 2, 0, data.data.size()});
        layout.sections.push_back(outData);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(
            !applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err));
        CHECK(err.str().find("requires a preceding matching ADRP relocation using the LDR base "
                             "register") != std::string::npos);
    }

    // --- COFF AArch64 BRANCH26 patches BL/B using the Windows relocation kind ---
    {
        std::vector<uint8_t> code(4, 0);
        code[3] = 0x94; // BL = 0x94000000
        auto caller = makeObj("test_branch26.obj",
                              ObjFileFormat::COFF,
                              code,
                              "target",
                              /*IMAGE_REL_ARM64_BRANCH26=*/3,
                              /*relocOff=*/0,
                              /*addend=*/0);

        ObjFile target;
        target.name = "target.obj";
        target.format = ObjFileFormat::COFF;
        target.sections.push_back({});
        ObjSection targetText;
        targetText.name = ".text";
        targetText.data.resize(8, 0);
        targetText.executable = true;
        targetText.alloc = true;
        targetText.alignment = 4;
        target.sections.push_back(targetText);
        target.symbols.push_back({});
        ObjSymbol targetSym;
        targetSym.name = "target";
        targetSym.binding = ObjSymbol::Global;
        targetSym.sectionIndex = 1;
        targetSym.offset = 0;
        target.symbols.push_back(targetSym);

        std::vector<ObjFile> objs = {caller, target};
        auto layout = makeLayout(objs, 0x140001000ULL);

        GlobalSymEntry entry;
        entry.name = "target";
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = 1;
        entry.secIndex = 1;
        entry.offset = 0;
        entry.resolvedAddr = 0;
        layout.globalSyms["target"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::AArch64, err);
        CHECK(ok);

        uint32_t patched = readLE32(layout.sections[0].data.data());
        CHECK((patched & 0xFC000000) == 0x94000000);
        CHECK((patched & 0x03FFFFFF) == 1);
    }

    // --- COFF AArch64 BRANCH26 rejects non-branch opcodes ---
    {
        std::vector<uint8_t> code(4, 0);
        code[3] = 0x91; // ADD, not B/BL.
        auto caller = makeObj("test_bad_branch26.obj",
                              ObjFileFormat::COFF,
                              code,
                              "target",
                              coff_a64::kBranch26,
                              /*relocOff=*/0,
                              /*addend=*/0);

        std::vector<ObjFile> objs = {caller};
        auto layout = makeLayout(objs, 0x140001000ULL);
        layout.globalSyms["target"] = {"target", GlobalSymEntry::Dynamic, 0, 0, 0, 0x140001004ULL};

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(!applyRelocations(
            objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::AArch64, err));
        CHECK(err.str().find("not applied to B/BL") != std::string::npos);
    }

    // --- COFF AArch64 PAGEBASE_REL21 rejects non-ADRP opcodes ---
    {
        std::vector<uint8_t> code(4, 0);
        code[3] = 0x91; // ADD, not ADRP.
        auto caller = makeObj("test_bad_adrp.obj",
                              ObjFileFormat::COFF,
                              code,
                              "target",
                              coff_a64::kPageRel21,
                              /*relocOff=*/0,
                              /*addend=*/0);

        std::vector<ObjFile> objs = {caller};
        auto layout = makeLayout(objs, 0x140001000ULL);
        layout.globalSyms["target"] = {"target", GlobalSymEntry::Dynamic, 0, 0, 0, 0x140002000ULL};

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(!applyRelocations(
            objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::AArch64, err));
        CHECK(err.str().find("not applied to ADRP") != std::string::npos);
    }

    // --- COFF AArch64 ADDR64 writes an absolute 64-bit pointer ---
    {
        std::vector<uint8_t> code(8, 0);
        auto caller = makeObj("test_addr64_a64.obj",
                              ObjFileFormat::COFF,
                              code,
                              "target_data",
                              /*IMAGE_REL_ARM64_ADDR64=*/14,
                              /*relocOff=*/0,
                              /*addend=*/0);

        ObjFile target;
        target.name = "target_data.obj";
        target.format = ObjFileFormat::COFF;
        target.sections.push_back({});
        ObjSection targetData;
        targetData.name = ".data";
        targetData.data.resize(16, 0xAB);
        targetData.writable = true;
        targetData.alloc = true;
        targetData.alignment = 8;
        target.sections.push_back(targetData);
        target.symbols.push_back({});
        ObjSymbol targetSym;
        targetSym.name = "target_data";
        targetSym.binding = ObjSymbol::Global;
        targetSym.sectionIndex = 1;
        targetSym.offset = 0;
        target.symbols.push_back(targetSym);

        std::vector<ObjFile> objs = {caller, target};
        auto layout = makeLayout(objs, 0x140001000ULL);

        GlobalSymEntry entry;
        entry.name = "target_data";
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = 1;
        entry.secIndex = 1;
        entry.offset = 0;
        entry.resolvedAddr = 0;
        layout.globalSyms["target_data"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::AArch64, err);
        CHECK(ok);

        uint64_t patched = readLE64(layout.sections[0].data.data());
        CHECK(patched == 0x140001008ULL);
    }

    // --- COFF AArch64 PAGEOFFSET_12L inspects load/store access size ---
    {
        std::vector<uint8_t> code(4, 0);
        // ldr w0, [x0, #0] = 32-bit load, PAGEOFFSET_12L must scale by 4.
        code[0] = 0x00;
        code[1] = 0x00;
        code[2] = 0x40;
        code[3] = 0xB9;
        auto caller = makeObj("test_pageoff12l_w.obj",
                              ObjFileFormat::COFF,
                              code,
                              "target_data",
                              /*IMAGE_REL_ARM64_PAGEOFFSET_12L=*/7,
                              /*relocOff=*/0,
                              /*addend=*/0);

        ObjFile target;
        target.name = "target_data.obj";
        target.format = ObjFileFormat::COFF;
        target.sections.push_back({});
        ObjSection targetData;
        targetData.name = ".data";
        targetData.data.resize(4, 0xAB);
        targetData.writable = true;
        targetData.alloc = true;
        targetData.alignment = 4;
        target.sections.push_back(targetData);
        target.symbols.push_back({});
        ObjSymbol targetSym;
        targetSym.name = "target_data";
        targetSym.binding = ObjSymbol::Global;
        targetSym.sectionIndex = 1;
        targetSym.offset = 0;
        target.symbols.push_back(targetSym);

        std::vector<ObjFile> objs = {caller, target};
        auto layout = makeLayout(objs, 0x140001000ULL);

        GlobalSymEntry entry;
        entry.name = "target_data";
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = 1;
        entry.secIndex = 1;
        entry.offset = 0;
        entry.resolvedAddr = 0;
        layout.globalSyms["target_data"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::AArch64, err);
        CHECK(ok);

        uint32_t patched = readLE32(layout.sections[0].data.data());
        CHECK(((patched >> 10) & 0xFFFu) == 1u);
    }

    // --- COFF AArch64 PAGEOFFSET_12L rejects add/sub opcodes ---
    {
        std::vector<uint8_t> code = {0x00, 0x00, 0x00, 0x91}; // add x0, x0, #0
        auto caller = makeObj("test_bad_pageoff12l_add.obj",
                              ObjFileFormat::COFF,
                              code,
                              "target_data",
                              coff_a64::kPageOff12L,
                              /*relocOff=*/0,
                              /*addend=*/0);

        std::vector<ObjFile> objs = {caller};
        auto layout = makeLayout(objs, 0x140001000ULL);
        layout.globalSyms["target_data"] = {
            "target_data", GlobalSymEntry::Dynamic, 0, 0, 0, 0x140002008ULL};

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(!applyRelocations(
            objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::AArch64, err));
        CHECK(err.str().find("PAGEOFFSET_12L relocation targets an ADD instruction") !=
              std::string::npos);
    }

    // --- COFF AArch64 PAGEOFFSET_12A rejects load/store opcodes ---
    {
        std::vector<uint8_t> code = {0x00, 0x00, 0x40, 0xB9}; // ldr w0, [x0, #0]
        auto caller = makeObj("test_bad_pageoff12a_ldr.obj",
                              ObjFileFormat::COFF,
                              code,
                              "target_data",
                              coff_a64::kPageOff12A,
                              /*relocOff=*/0,
                              /*addend=*/0);

        std::vector<ObjFile> objs = {caller};
        auto layout = makeLayout(objs, 0x140001000ULL);
        layout.globalSyms["target_data"] = {
            "target_data", GlobalSymEntry::Dynamic, 0, 0, 0, 0x140002008ULL};

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(!applyRelocations(
            objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::AArch64, err));
        CHECK(err.str().find("PAGEOFFSET_12A relocation targets a load/store instruction") !=
              std::string::npos);
    }

    // --- COFF AArch64 SECREL_LOW12L rejects non-load/store opcodes ---
    {
        std::vector<uint8_t> code(4, 0);
        // add x0, x0, #0 is not a load/store unsigned-offset instruction.
        code[0] = 0x00;
        code[1] = 0x00;
        code[2] = 0x00;
        code[3] = 0x91;
        auto caller = makeObj("test_bad_secrel_low12l.obj",
                              ObjFileFormat::COFF,
                              code,
                              "target_data",
                              coff_a64::kSecRelLow12L,
                              /*relocOff=*/0,
                              /*addend=*/0);

        ObjFile target;
        target.name = "target_data.obj";
        target.format = ObjFileFormat::COFF;
        target.sections.push_back({});
        ObjSection targetData;
        targetData.name = ".data";
        targetData.data.resize(4, 0xAB);
        targetData.writable = true;
        targetData.alloc = true;
        target.sections.push_back(targetData);
        target.symbols.push_back({});
        ObjSymbol targetSym;
        targetSym.name = "target_data";
        targetSym.binding = ObjSymbol::Global;
        targetSym.sectionIndex = 1;
        target.symbols.push_back(targetSym);

        std::vector<ObjFile> objs = {caller, target};
        auto layout = makeLayout(objs, 0x140001000ULL);
        layout.globalSyms["target_data"] = {"target_data", GlobalSymEntry::Global, 1, 1, 0, 0};

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(!applyRelocations(
            objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::AArch64, err));
        CHECK(err.str().find("not applied to an AArch64 unsigned-offset load/store") !=
              std::string::npos);
    }

    // --- COFF AArch64 SECREL_LOW12A rejects non-add/sub opcodes ---
    {
        std::vector<uint8_t> code = {0x00, 0x00, 0x40, 0xB9}; // ldr w0, [x0, #0]
        auto caller = makeObj("test_bad_secrel_low12a.obj",
                              ObjFileFormat::COFF,
                              code,
                              "target_data",
                              coff_a64::kSecRelLow12A,
                              /*relocOff=*/0,
                              /*addend=*/0);

        ObjFile target;
        target.name = "target_data.obj";
        target.format = ObjFileFormat::COFF;
        target.sections.push_back({});
        ObjSection targetData;
        targetData.name = ".data";
        targetData.data.resize(4, 0xAB);
        targetData.writable = true;
        targetData.alloc = true;
        target.sections.push_back(targetData);
        target.symbols.push_back({});
        ObjSymbol targetSym;
        targetSym.name = "target_data";
        targetSym.binding = ObjSymbol::Global;
        targetSym.sectionIndex = 1;
        target.symbols.push_back(targetSym);

        std::vector<ObjFile> objs = {caller, target};
        auto layout = makeLayout(objs, 0x140001000ULL);
        layout.globalSyms["target_data"] = {"target_data", GlobalSymEntry::Global, 1, 1, 0, 0};

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(!applyRelocations(
            objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::AArch64, err));
        CHECK(err.str().find("SECREL_LOW/HIGH12A relocation is not applied to an AArch64 ADD") !=
              std::string::npos);
    }

    // --- COFF AArch64 PAGEOFFSET_12A rejects unrelated opcodes ---
    {
        std::vector<uint8_t> code = {0x1F, 0x20, 0x03, 0xD5}; // nop
        auto caller = makeObj("test_bad_pageoff12a.obj",
                              ObjFileFormat::COFF,
                              code,
                              "target_data",
                              coff_a64::kPageOff12A,
                              /*relocOff=*/0,
                              /*addend=*/0);

        std::vector<ObjFile> objs = {caller};
        auto layout = makeLayout(objs, 0x140001000ULL);
        layout.globalSyms["target_data"] = {
            "target_data", GlobalSymEntry::Dynamic, 0, 0, 0, 0x140002008ULL};

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(!applyRelocations(
            objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::AArch64, err));
        CHECK(err.str().find("unsupported instruction") != std::string::npos);
    }

    // --- Windows .pdata with trailing bytes is malformed ---
    {
        std::vector<ObjFile> objs;
        LinkLayout layout;
        layout.pageSize = 0x1000;
        OutputSection pdata;
        pdata.name = ".pdata";
        pdata.alloc = true;
        pdata.data.resize(13, 0);
        layout.sections.push_back(std::move(pdata));

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        CHECK(
            !applyRelocations(objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::X86_64, err));
        CHECK(err.str().find("not a multiple of unwind record size") != std::string::npos);
    }

    // --- Symbol address overflow is diagnosed during resolution ---
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("overflow_addr.o",
                           ObjFileFormat::ELF,
                           code,
                           "unused",
                           /*R_X86_64_PC32=*/2,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, std::numeric_limits<uint64_t>::max() - 4);

        GlobalSymEntry entry;
        entry.name = "defined";
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = 0;
        entry.secIndex = 1;
        entry.offset = 8;
        layout.globalSyms["defined"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(!ok);
        CHECK(err.str().find("symbol address overflow") != std::string::npos);
    }

    // --- Unknown reloc type produces error ---
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("test_unk.o",
                           ObjFileFormat::ELF,
                           code,
                           "any_sym",
                           /*unknown type=*/999,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x401000);

        GlobalSymEntry entry;
        entry.name = "any_sym";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = 0x402000;
        layout.globalSyms["any_sym"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::X86_64, err);
        CHECK(!ok);
        CHECK(err.str().find("unknown reloc type") != std::string::npos);
    }

    // --- COFF ADDR32NB honors layout.imageBase override (P1 #3) ---
    // Regression: applyRelocations used to subtract
    // defaultImageBaseForPlatform(Windows) unconditionally. If a future
    // configurable image base is wired through NativeLinkerOptions /
    // LinkLayout, the applier must pick it up from the layout instead so
    // RVAs in instruction bytes match the value the PE writer stamps into
    // the optional header.
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("test_addr32nb_basecfg.obj",
                           ObjFileFormat::COFF,
                           code,
                           "unwind_info",
                           /*IMAGE_REL_AMD64_ADDR32NB=*/3,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        // Use a custom (non-default) image base. The symbol VA is base+0x2000.
        const uint64_t customBase = 0x180000000ULL;
        auto layout = makeLayout(objs, customBase + 0x1000);
        layout.imageBase = customBase;

        GlobalSymEntry entry;
        entry.name = "unwind_info";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = customBase + 0x2000ULL;
        layout.globalSyms["unwind_info"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::X86_64, err);
        CHECK(ok);

        // RVA = S - imageBase = 0x2000. If the applier had still subtracted
        // 0x140000000 (the platform default), the result would be a different
        // very large number that would also overflow checkedU32Value.
        uint32_t patched = readLE32(layout.sections[0].data.data());
        CHECK(patched == 0x2000);
    }

    // --- COFF AArch64 ADDR32NB honors layout.imageBase override (P1 #3) ---
    {
        std::vector<uint8_t> code(8, 0);
        auto obj = makeObj("test_addr32nb_arm64_basecfg.obj",
                           ObjFileFormat::COFF,
                           code,
                           "unwind_info_a64",
                           /*IMAGE_REL_ARM64_ADDR32NB=*/2,
                           /*relocOff=*/0,
                           /*addend=*/0);

        std::vector<ObjFile> objs = {obj};
        const uint64_t customBase = 0x180000000ULL;
        auto layout = makeLayout(objs, customBase + 0x1000);
        layout.imageBase = customBase;

        GlobalSymEntry entry;
        entry.name = "unwind_info_a64";
        entry.binding = GlobalSymEntry::Dynamic;
        entry.resolvedAddr = customBase + 0x3000ULL;
        layout.globalSyms["unwind_info_a64"] = entry;

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Windows, LinkArch::AArch64, err);
        CHECK(ok);

        uint32_t patched = readLE32(layout.sections[0].data.data());
        CHECK(patched == 0x3000);
    }

    // --- AArch64 local GOT relaxation allows non-adjacent ADRP/LDR pair ---
    {
        ObjFile obj;
        obj.name = "a64_got_relax_nonadjacent.o";
        obj.format = ObjFileFormat::ELF;
        obj.sections.push_back({});

        ObjSection text;
        text.name = ".text";
        text.executable = true;
        text.alloc = true;
        text.alignment = 4;
        // adrp x1, target@GOTPAGE; nop; ldr x0, [x1, target@GOTPAGEOFF]
        text.data = {0x01, 0x00, 0x00, 0x90, 0x1F, 0x20, 0x03, 0xD5, 0x20, 0x00, 0x40, 0xF9};
        text.relocs.push_back(
            ObjReloc{.offset = 0, .type = elf_a64::kAdrGotPage, .symIndex = 1, .addend = 0});
        text.relocs.push_back(
            ObjReloc{.offset = 8, .type = elf_a64::kLd64GotLo12Nc, .symIndex = 1, .addend = 0});
        obj.sections.push_back(std::move(text));

        ObjSection dataSec;
        dataSec.name = ".data";
        dataSec.alloc = true;
        dataSec.writable = true;
        dataSec.alignment = 8;
        dataSec.data = {0xAA, 0xBB, 0xCC, 0xDD, 0, 0, 0, 0};
        obj.sections.push_back(std::move(dataSec));

        obj.symbols.push_back({});
        ObjSymbol target;
        target.name = "local_target";
        target.binding = ObjSymbol::Local;
        target.sectionIndex = 2;
        target.offset = 0;
        obj.symbols.push_back(std::move(target));

        std::vector<ObjFile> objs = {obj};
        auto layout = makeLayout(objs, 0x401000);

        std::ostringstream err;
        std::unordered_set<std::string> dynSyms;
        bool ok =
            applyRelocations(objs, layout, dynSyms, LinkPlatform::Linux, LinkArch::AArch64, err);
        CHECK(ok);
        CHECK(err.str().empty());

        const uint32_t relaxed = readLE32(layout.sections[0].data.data() + 8);
        CHECK((relaxed & 0xFFC003E0u) == 0x91000020u); // ADD x0, x1, #imm12
    }

    // --- Result ---
    if (gFail == 0) {
        std::cout << "All RelocApplier tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << gFail << " RelocApplier test(s) FAILED.\n";
    return EXIT_FAILURE;
}
