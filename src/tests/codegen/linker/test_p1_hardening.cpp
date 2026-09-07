//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/linker/test_p1_hardening.cpp
// Purpose: Regression coverage for the P1 native-linker hardening pass.
//          Each test pins a specific bug the audit surfaced so that future
//          refactors can't reintroduce the silent miscompile path:
//            - Mach-O TLV descriptor section size must be a multiple of 24
//              before buildBindOpcodes emits _tlv_bootstrap binds.
//            - SectionMerger marks descriptor outputs with the
//              OutputSection::tlvDescriptors flag instead of relying on the
//              ".tdata" name (which ELF/PE reuse for TLS template data).
//            - DynStubGen's synthetic GOT slots live in a dedicated
//              ".got.zanna_stubs" section instead of colliding with the
//              user's ".data" by name.
// Key invariants:
//   - The tests do not exercise the full link pipeline; each function-under-
//     test is invoked directly with a hand-built LinkLayout / ObjFile so
//     failure modes are deterministic and platform-independent.
//   - Diagnostics are captured into a std::ostringstream and inspected for
//     substring matches that survive future error-message rewording.
// Ownership/Lifetime: Standalone test binary.
// Links: codegen/common/linker/MachOBindRebase.hpp,
//        codegen/common/linker/DynStubGen.hpp,
//        codegen/common/linker/LinkTypes.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/common/linker/DynStubGen.hpp"
#include "codegen/common/linker/LinkTypes.hpp"
#include "codegen/common/linker/MachOBindRebase.hpp"
#include "codegen/common/linker/RelocConstants.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

namespace {

/// Build a LinkLayout that mimics what SectionMerger produces for a Mach-O
/// arm64 link with TLV descriptors but no other dynamic state. The single
/// descriptor section is placed at @p descVA with @p descSize raw bytes.
LinkLayout makeTlvLayout(size_t descSize, uint64_t descVA, bool flagAsDescriptor) {
    LinkLayout layout;
    layout.imageBase = 0x100000000ULL;
    layout.pageSize = 0x4000; // macOS arm64

    OutputSection desc;
    desc.name = ".tdata";
    desc.virtualAddr = descVA;
    desc.tls = true;
    desc.writable = true;
    desc.dataSegment = true;
    desc.data.assign(descSize, 0);
    desc.memSize = descSize;
    desc.tlvDescriptors = flagAsDescriptor;
    layout.sections.push_back(std::move(desc));
    return layout;
}

} // namespace

int main() {
    // --- P1 #2: non-multiple-of-24 descriptor size is a hard error ---
    // buildBindOpcodes used to truncate the descriptor count via integer
    // division. A section sized 25 bytes (one whole descriptor + 1 stray
    // byte) would silently emit a single bind opcode and leave the trailing
    // descriptor's thunk field uninitialized — dyld then aborts on first
    // TLV access. The size check makes the link fail loudly instead.
    {
        const uint64_t dataSegVmAddr = 0x100004000ULL;
        auto layout = makeTlvLayout(/*descSize=*/25,
                                    /*descVA=*/dataSegVmAddr,
                                    /*flagAsDescriptor=*/true);

        std::vector<uint8_t> bindData;
        std::unordered_map<std::string, uint32_t> symOrdinals;
        std::ostringstream err;
        const bool ok = buildBindOpcodes(
            bindData, layout.gotEntries, layout, dataSegVmAddr, 2, symOrdinals, err);
        CHECK(!ok);
        CHECK(err.str().find("not a multiple of 24") != std::string::npos);
    }

    // --- P1 #1: descriptor section is selected by the flag, not the name ---
    // A .tdata section that DOES NOT carry the tlvDescriptors flag must not
    // be walked as TLV descriptors. On ELF/PE the merger uses the ".tdata"
    // name for the TLS template image, not descriptors; the bind-opcode
    // emitter has no business binding _tlv_bootstrap into template bytes.
    {
        const uint64_t dataSegVmAddr = 0x100004000ULL;
        // 25 bytes — would fail the size check above if interpreted as
        // descriptors. Because the flag is off, the section is skipped
        // entirely and the emitter succeeds with only the BIND_OPCODE_DONE
        // terminator byte.
        auto layout = makeTlvLayout(/*descSize=*/25,
                                    /*descVA=*/dataSegVmAddr,
                                    /*flagAsDescriptor=*/false);

        std::vector<uint8_t> bindData;
        std::unordered_map<std::string, uint32_t> symOrdinals;
        std::ostringstream err;
        const bool ok = buildBindOpcodes(
            bindData, layout.gotEntries, layout, dataSegVmAddr, 2, symOrdinals, err);
        CHECK(ok);
        CHECK(err.str().empty());
        // Single trailing terminator opcode.
        CHECK(bindData.size() == 1);
        CHECK(bindData[0] == 0x00 /* BIND_OPCODE_DONE */);
    }

    // --- P1 #1/#2: a well-formed descriptor section emits one bind per record ---
    // 48 bytes = exactly 2 descriptors. Each thunk is bound to
    // _tlv_bootstrap from libSystem (ordinal 1). We don't decode the full
    // opcode stream; instead we verify the section's name appears the
    // expected number of times (each emitBindEntry writes the literal
    // "_tlv_bootstrap" once into the bind opcode payload).
    {
        const uint64_t dataSegVmAddr = 0x100004000ULL;
        auto layout = makeTlvLayout(/*descSize=*/48,
                                    /*descVA=*/dataSegVmAddr,
                                    /*flagAsDescriptor=*/true);

        std::vector<uint8_t> bindData;
        std::unordered_map<std::string, uint32_t> symOrdinals;
        std::ostringstream err;
        const bool ok = buildBindOpcodes(
            bindData, layout.gotEntries, layout, dataSegVmAddr, 2, symOrdinals, err);
        CHECK(ok);

        // Count occurrences of "_tlv_bootstrap" in the bind opcode payload.
        const std::string needle = "_tlv_bootstrap";
        size_t count = 0;
        std::string blob(reinterpret_cast<const char *>(bindData.data()), bindData.size());
        for (size_t pos = 0; (pos = blob.find(needle, pos)) != std::string::npos;
             pos += needle.size())
            ++count;
        CHECK(count == 2);
    }

    // --- P1 #6: dyld stub objects publish a dedicated GOT section name ---
    // Synthetic GOT slots used to share the bare ".data" name with user
    // data. Section merger then folded them into the user data segment with
    // no remaining way to recover their origin from the chunk metadata.
    {
        std::unordered_set<std::string> dyn = {"printf", "fwrite"};
        ObjFile stub = generateDynStubsAArch64(dyn, /*copyRelocDataSymbols=*/false);

        // The stub object always carries a null section at index 0, a .text
        // section, and a separate GOT section. We expect the GOT section to
        // be named `.got.zanna_stubs` — never `.data`.
        bool foundGot = false;
        for (size_t i = 1; i < stub.sections.size(); ++i) {
            const auto &s = stub.sections[i];
            CHECK(s.name != ".data");
            if (s.name == ".got.zanna_stubs")
                foundGot = true;
        }
        CHECK(foundGot);
    }
    {
        std::unordered_set<std::string> dyn = {"printf", "fwrite"};
        ObjFile stub = generateDynStubsX8664(dyn);
        bool foundGot = false;
        for (size_t i = 1; i < stub.sections.size(); ++i) {
            const auto &s = stub.sections[i];
            CHECK(s.name != ".data");
            if (s.name == ".got.zanna_stubs")
                foundGot = true;
        }
        CHECK(foundGot);
    }

    // Plain R_X86_64_GOTPCREL to a symbol defined in the link (clang emits it
    // for `pushq foo@GOTPCREL(%rip)`, which the psABI forbids relaxing) gets a
    // link-time GOT slot: `__gotl_foo` in `.got.zanna_local`, filled through an
    // R_X86_64_64 relocation against `foo`. Dynamic, local, undefined, and
    // already-slotted symbols are left alone.
    {
        ObjFile user;
        user.format = ObjFileFormat::ELF;
        user.sections.push_back({});
        ObjSection text;
        text.name = ".text";
        text.executable = true;
        text.data = {0xFF, 0x35, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x35, 0x00, 0x00, 0x00, 0x00,
                     0xFF, 0x35, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x35, 0x00, 0x00, 0x00, 0x00};
        user.symbols.push_back({});
        auto addSym = [&](const char *name, ObjSymbol::Binding binding, uint32_t section) {
            ObjSymbol s;
            s.name = name;
            s.binding = binding;
            s.sectionIndex = section;
            user.symbols.push_back(std::move(s));
            return static_cast<uint32_t>(user.symbols.size() - 1);
        };
        const uint32_t definedIdx = addSym("rt_defined", ObjSymbol::Global, 1);
        const uint32_t dynIdx = addSym("printf", ObjSymbol::Undefined, 0);
        const uint32_t localIdx = addSym("static_helper", ObjSymbol::Local, 1);
        const uint32_t externIdx = addSym("rt_elsewhere", ObjSymbol::Undefined, 0);
        auto addReloc = [&](size_t off, uint32_t sym) {
            ObjReloc r;
            r.offset = off;
            r.type = elf_x64::kGotPcRel;
            r.symIndex = sym;
            r.addend = -4;
            text.relocs.push_back(r);
        };
        addReloc(2, definedIdx);
        addReloc(8, dynIdx);
        addReloc(14, localIdx);
        addReloc(20, externIdx);
        user.sections.push_back(std::move(text));

        // rt_elsewhere is defined by a second object with global binding.
        ObjFile other;
        other.format = ObjFileFormat::ELF;
        other.sections.push_back({});
        ObjSection otherText;
        otherText.name = ".text";
        otherText.executable = true;
        otherText.data = {0xC3};
        other.sections.push_back(std::move(otherText));
        other.symbols.push_back({});
        ObjSymbol elsewhere;
        elsewhere.name = "rt_elsewhere";
        elsewhere.binding = ObjSymbol::Global;
        elsewhere.sectionIndex = 1;
        other.symbols.push_back(std::move(elsewhere));

        std::vector<ObjFile> objs;
        objs.push_back(std::move(user));
        objs.push_back(std::move(other));
        std::unordered_set<std::string> dyn = {"printf"};
        ObjFile slots = generateStaticGotSlotsX8664(objs, dyn);
        CHECK(slots.synthetic);
        CHECK(slots.sections.size() == 2);
        if (slots.sections.size() == 2) {
            const auto &got = slots.sections[1];
            CHECK(got.name == ".got.zanna_local");
            CHECK(got.data.size() == 16); // rt_defined + rt_elsewhere, sorted
            CHECK(got.relocs.size() == 2);
            std::vector<std::string> gotNames;
            std::vector<std::string> targetNames;
            for (size_t i = 1; i < slots.symbols.size(); ++i) {
                const auto &s = slots.symbols[i];
                if (s.binding == ObjSymbol::Global)
                    gotNames.push_back(s.name);
                else if (s.binding == ObjSymbol::Undefined)
                    targetNames.push_back(s.name);
            }
            CHECK(gotNames ==
                  (std::vector<std::string>{"__gotl_rt_defined", "__gotl_rt_elsewhere"}));
            CHECK(targetNames == (std::vector<std::string>{"rt_defined", "rt_elsewhere"}));
            for (const auto &r : got.relocs) {
                CHECK(r.type == elf_x64::kAbs64);
                CHECK(r.addend == 0);
                CHECK(r.symIndex < slots.symbols.size() &&
                      slots.symbols[r.symIndex].binding == ObjSymbol::Undefined);
            }
        }
        // Nothing to do → no sections at all.
        std::unordered_set<std::string> none;
        std::vector<ObjFile> empty;
        CHECK(generateStaticGotSlotsX8664(empty, none).sections.empty());
    }

    if (gFail == 0) {
        std::cout << "All P1 hardening tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << gFail << " P1 hardening test(s) FAILED.\n";
    return EXIT_FAILURE;
}
