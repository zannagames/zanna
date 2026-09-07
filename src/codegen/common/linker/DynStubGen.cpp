//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/DynStubGen.cpp
// Purpose: Generate synthetic AArch64 object files containing stub trampolines
//          for dynamic symbols and ObjC selector dispatch.
// Key invariants:
//   - Stub ObjFiles use ELF relocation format so the reloc applier dispatches
//     through elfA64Action() for type 275 (Page21) and type 286 (LdSt64Off).
//   - ObjC selector stubs transfer symbols from dynamicSyms to local stubs.
//   - Dynamic stubs generate GOT entries filled by dyld at load time.
// Ownership/Lifetime:
//   - Returned ObjFile values are owned by the caller.
// Links: codegen/common/linker/NativeLinker.cpp, codegen/common/linker/RelocApplier.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file DynStubGen.cpp
 * @brief Implements deterministic synthetic stubs, GOT slots, and selector references.
 */

#include "DynStubGen.hpp"
#include "RelocConstants.hpp"
#include "codegen/common/linker/DynamicSymbolPolicy.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace zanna::codegen::linker {

namespace {

/// @brief Appends one 32-bit instruction in little-endian byte order.
/// @param buf Destination section byte buffer.
/// @param v Instruction word to append.
void emitLE32(std::vector<uint8_t> &buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 24));
}

/// @brief Narrow a synthetic symbol-table index to the relocation field width.
/// @details Dynamic-stub objects are generated in memory, but their relocations
///          still store symbol indices in the same 32-bit field used by object
///          files. Throwing here prevents silent wraparound if an unexpectedly
///          large import set ever pushes the synthetic table past that limit.
/// @param index Host-sized symbol-table index.
/// @param context Human-readable generated record used in an exception message.
/// @return The index narrowed to the relocation field width.
/// @throws std::runtime_error If @p index exceeds `uint32_t`.
uint32_t checkedSymbolIndex(size_t index, const char *context) {
    if (index > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::string("dynamic stub symbol index overflow while adding ") +
                                 context);
    }
    return static_cast<uint32_t>(index);
}

/// @brief Checked size multiplication for deterministic synthetic offsets.
/// @details Stub and GOT offsets are derived from fixed record sizes. This
///          helper keeps those calculations from wrapping before the vector
///          append paths get a chance to fail cleanly.
/// @param lhs Left factor.
/// @param rhs Right factor.
/// @param context Human-readable operation used in an exception message.
/// @return The checked product.
/// @throws std::runtime_error If the product exceeds `size_t`.
size_t checkedMulSize(size_t lhs, size_t rhs, const char *context) {
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
        throw std::runtime_error(std::string("dynamic stub size multiplication overflow in ") +
                                 context);
    }
    return lhs * rhs;
}

/// @brief Checked size addition for synthetic section growth.
/// @details Used before vector resize operations so size arithmetic errors are
///          diagnosed as linker bugs instead of relying on implementation-
///          defined overflow behaviour.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param context Human-readable operation used in an exception message.
/// @return The checked sum.
/// @throws std::runtime_error If the sum exceeds `size_t`.
size_t checkedAddSize(size_t lhs, size_t rhs, const char *context) {
    if (lhs > std::numeric_limits<size_t>::max() - rhs) {
        throw std::runtime_error(std::string("dynamic stub size addition overflow in ") + context);
    }
    return lhs + rhs;
}

/// @brief Reserve writable storage for a loader-copied data import.
/// @details Unlike a code import, a data import cannot be represented by a
///          trampoline: non-PIC references load through the symbol directly, so
///          the name has to denote storage holding the value. This reserves
///          @p size zero bytes in the synthetic writable section for the value
///          itself, plus a second word holding its address for translation units
///          that reach the import through the GOT.
/// @param stubObj Synthetic object receiving the symbol definitions.
/// @param gotSec Writable synthetic section receiving the storage.
/// @param name Imported symbol name.
/// @param slotOff Byte offset within @p gotSec at which the value slot starts.
/// @param size Storage size in bytes.
/// @throws std::runtime_error If the section size arithmetic would overflow.
void appendCopyRelocSlot(
    ObjFile &stubObj, ObjSection &gotSec, const std::string &name, size_t slotOff, size_t size) {
    gotSec.data.resize(checkedAddSize(slotOff, size, "loader data import slot"), 0);

    ObjSymbol dataSym;
    dataSym.name = name;
    dataSym.binding = ObjSymbol::Global;
    dataSym.sectionIndex = 2;
    dataSym.offset = slotOff;
    stubObj.symbols.push_back(std::move(dataSym));

    // The `__got_` alias must stay a separate word holding the *address* of the
    // storage above. Sharing one word would make it hold the object's value
    // instead, and a translation unit that reaches the import through the GOT
    // (`mov stdout@GOTPCREL(%rip), %rax`) would then dereference one level too
    // far. The executable writer binds this word like any other GOT slot; the
    // copy relocation makes the executable the definition the loader finds, so
    // the bind lands on the storage above rather than the library's own copy.
    const size_t gotOff = gotSec.data.size();
    gotSec.data.resize(checkedAddSize(gotOff, size, "loader data import pointer"), 0);

    ObjSymbol gotSym;
    gotSym.name = "__got_" + name;
    gotSym.binding = ObjSymbol::Global;
    gotSym.sectionIndex = 2;
    gotSym.offset = gotOff;
    stubObj.symbols.push_back(std::move(gotSym));
}

} // namespace

/// @copydoc generateObjcSelectorStubsAArch64(std::unordered_set<std::string> &)
ObjFile generateObjcSelectorStubsAArch64(std::unordered_set<std::string> &dynamicSyms) {
    ObjFile stubObj;
    stubObj.name = "<objc-stubs>";
    stubObj.synthetic = true;
    stubObj.format = ObjFileFormat::ELF;
    stubObj.is64bit = true;
    stubObj.isLittleEndian = true;
    stubObj.machine = 183; // EM_AARCH64

    // Collect objc_msgSend$selector symbols and remove them from dynamicSyms
    // (they'll be resolved by our generated stubs, not by dyld).
    std::vector<std::string> selectorSyms;
    for (auto it = dynamicSyms.begin(); it != dynamicSyms.end();) {
        if (it->find("objc_msgSend$") == 0) {
            selectorSyms.push_back(*it);
            it = dynamicSyms.erase(it);
        } else
            ++it;
    }
    if (selectorSyms.empty())
        return stubObj;

    std::sort(selectorSyms.begin(), selectorSyms.end());

    // Ensure objc_msgSend itself is in the dynamic symbol set.
    dynamicSyms.insert("objc_msgSend");

    // Section 0: null.
    stubObj.sections.push_back(ObjSection{});

    // Section 1: .text (stub trampolines).
    ObjSection textSec;
    textSec.name = ".text";
    textSec.executable = true;
    textSec.writable = false;
    textSec.alloc = true;
    textSec.alignment = 4;

    // Section 2: __objc_selrefs (selector reference pointers).
    ObjSection dataSec;
    dataSec.name = "__DATA,__objc_selrefs";
    dataSec.executable = false;
    dataSec.writable = true;
    dataSec.alloc = true;
    dataSec.dataSegment = true;
    dataSec.alignment = 8;

    // Section 3: .rodata (selector strings).
    ObjSection rodataSec;
    rodataSec.name = ".rodata";
    rodataSec.executable = false;
    rodataSec.writable = false;
    rodataSec.alloc = true;
    rodataSec.alignment = 1;

    // Symbol 0: null.
    stubObj.symbols.push_back(ObjSymbol{});

    // First pass: build selector strings in rodata and sel ref pointers in data.
    std::vector<size_t> strOffsets;
    for (const auto &sym : selectorSyms) {
        std::string selector = sym.substr(sym.find('$') + 1);

        strOffsets.push_back(rodataSec.data.size());
        rodataSec.data.insert(rodataSec.data.end(), selector.begin(), selector.end());
        rodataSec.data.push_back(0);

        for (int j = 0; j < 8; ++j)
            dataSec.data.push_back(0);
    }

    // Build symbols and relocations.
    for (size_t i = 0; i < selectorSyms.size(); ++i) {
        const size_t stubOff = checkedMulSize(i, 12, "ObjC selector stub offset");
        const size_t selrefOff = checkedMulSize(i, 8, "ObjC selector reference offset");

        // Symbol for the selector string in rodata (section 3).
        const uint32_t strSymIdx =
            checkedSymbolIndex(stubObj.symbols.size(), "ObjC selector string");
        ObjSymbol strSym;
        strSym.name = "__objc_selstr_" + std::to_string(i);
        strSym.binding = ObjSymbol::Local;
        strSym.sectionIndex = 3;
        strSym.offset = strOffsets[i];
        stubObj.symbols.push_back(std::move(strSym));

        // Symbol for the selector reference pointer in data (section 2).
        const uint32_t selrefSymIdx =
            checkedSymbolIndex(stubObj.symbols.size(), "ObjC selector reference");
        ObjSymbol selrefSym;
        selrefSym.name = "__objc_selref_" + std::to_string(i);
        selrefSym.binding = ObjSymbol::Global;
        selrefSym.sectionIndex = 2;
        selrefSym.offset = selrefOff;
        stubObj.symbols.push_back(std::move(selrefSym));

        // Stub entry point symbol (section 1).
        ObjSymbol stubSym;
        stubSym.name = selectorSyms[i];
        stubSym.binding = ObjSymbol::Global;
        stubSym.sectionIndex = 1;
        stubSym.offset = stubOff;
        stubObj.symbols.push_back(std::move(stubSym));

        // Relocation: selref pointer -> selector string (Abs64).
        ObjReloc strReloc;
        strReloc.offset = selrefOff;
        strReloc.type = elf_a64::kAbs64;
        strReloc.symIndex = strSymIdx;
        strReloc.addend = 0;
        dataSec.relocs.push_back(strReloc);

        // Emit stub instructions:
        //   adrp x1, selref@PAGE      -> 0x90000001
        //   ldr  x1, [x1, selref@OFF] -> 0xF9400021
        //   b    objc_msgSend          -> 0x14000000
        emitLE32(textSec.data, 0x90000001);
        emitLE32(textSec.data, 0xF9400021);
        emitLE32(textSec.data, 0x14000000);

        // Relocation: ADRP x1, selref@PAGE
        ObjReloc adrpReloc;
        adrpReloc.offset = stubOff;
        adrpReloc.type = elf_a64::kAdrPrelPgHi21;
        adrpReloc.symIndex = selrefSymIdx;
        adrpReloc.addend = 0;
        textSec.relocs.push_back(adrpReloc);

        // Relocation: LDR x1, [x1, selref@PAGEOFF]
        ObjReloc ldrReloc;
        ldrReloc.offset = stubOff + 4;
        ldrReloc.type = elf_a64::kLdSt64Lo12Nc;
        ldrReloc.symIndex = selrefSymIdx;
        ldrReloc.addend = 0;
        textSec.relocs.push_back(ldrReloc);
    }

    // Add a symbol for objc_msgSend (undefined, resolved by the reloc applier).
    const uint32_t msgSendSymIdx = checkedSymbolIndex(stubObj.symbols.size(), "objc_msgSend");
    ObjSymbol msgSendSym;
    msgSendSym.name = "objc_msgSend";
    msgSendSym.binding = ObjSymbol::Undefined;
    msgSendSym.sectionIndex = 0;
    stubObj.symbols.push_back(std::move(msgSendSym));

    // Add B relocations for each stub -> objc_msgSend.
    for (size_t i = 0; i < selectorSyms.size(); ++i) {
        ObjReloc bReloc;
        bReloc.offset = checkedAddSize(
            checkedMulSize(i, 12, "ObjC branch stub offset"), 8, "ObjC branch relocation offset");
        bReloc.type = elf_a64::kJump26;
        bReloc.symIndex = msgSendSymIdx;
        bReloc.addend = 0;
        textSec.relocs.push_back(bReloc);
    }

    stubObj.sections.push_back(std::move(textSec));
    stubObj.sections.push_back(std::move(dataSec));
    stubObj.sections.push_back(std::move(rodataSec));
    return stubObj;
}

/// @copydoc generateDynStubsAArch64(const std::unordered_set<std::string> &, bool)
ObjFile generateDynStubsAArch64(const std::unordered_set<std::string> &dynamicSyms,
                                bool copyRelocDataSymbols) {
    ObjFile stubObj;
    stubObj.name = "<dyld-stubs>";
    stubObj.synthetic = true;
    stubObj.format = ObjFileFormat::ELF;
    stubObj.is64bit = true;
    stubObj.isLittleEndian = true;
    stubObj.machine = 183; // EM_AARCH64

    // Section 0: null.
    stubObj.sections.push_back(ObjSection{});

    // Section 1: .text (stub trampolines).
    ObjSection textSec;
    textSec.name = ".text";
    textSec.executable = true;
    textSec.writable = false;
    textSec.alloc = true;
    textSec.alignment = 4;

    // Section 2: .got.zanna_stubs (synthetic dyld GOT slots, writable).
    // Use a dedicated name so the slots stay distinguishable from the user's
    // .data — section-merger still routes both into the data segment, but the
    // chunk's identity in InputChunk::inputSecIndex/name maps it back to this
    // synthetic origin for dead-strip, ICF, and debugger reporting.
    ObjSection gotSec;
    gotSec.name = ".got.zanna_stubs";
    gotSec.executable = false;
    gotSec.writable = true;
    gotSec.alloc = true;
    gotSec.alignment = 8;

    // Symbol 0: null.
    stubObj.symbols.push_back(ObjSymbol{});

    // Sort dynamic symbols for deterministic layout.
    std::vector<std::string> sorted(dynamicSyms.begin(), dynamicSyms.end());
    std::sort(sorted.begin(), sorted.end());

    for (size_t i = 0; i < sorted.size(); ++i) {
        const size_t gotOff = gotSec.data.size();

        // Imported data objects get storage instead of a trampoline: the name
        // must denote the value, not a branch to it. See loaderDataSymbolSize().
        if (copyRelocDataSymbols) {
            const size_t dataSize = loaderDataSymbolSize(sorted[i]);
            if (dataSize != 0) {
                appendCopyRelocSlot(stubObj, gotSec, sorted[i], gotOff, dataSize);
                continue;
            }
        }

        const size_t stubOff = textSec.data.size();

        // GOT entry symbol.
        const uint32_t gotSymIdx =
            checkedSymbolIndex(stubObj.symbols.size(), "AArch64 dynamic GOT symbol");
        ObjSymbol gotSym;
        gotSym.name = "__got_" + sorted[i];
        gotSym.binding = ObjSymbol::Global;
        gotSym.sectionIndex = 2;
        gotSym.offset = gotOff;
        stubObj.symbols.push_back(std::move(gotSym));

        // Stub entry point symbol (overrides Dynamic entry for this symbol).
        ObjSymbol stubSym;
        stubSym.name = sorted[i];
        stubSym.binding = ObjSymbol::Global;
        stubSym.sectionIndex = 1;
        stubSym.offset = stubOff;
        stubObj.symbols.push_back(std::move(stubSym));

        // Emit stub instructions.
        emitLE32(textSec.data, 0x90000010); // adrp x16, #0
        emitLE32(textSec.data, 0xF9400210); // ldr x16, [x16, #0]
        emitLE32(textSec.data, 0xD61F0200); // br x16

        // Relocation: ADRP x16, GOT_entry@PAGE
        ObjReloc adrpReloc;
        adrpReloc.offset = stubOff;
        adrpReloc.type = elf_a64::kAdrPrelPgHi21;
        adrpReloc.symIndex = gotSymIdx;
        adrpReloc.addend = 0;
        textSec.relocs.push_back(adrpReloc);

        // Relocation: LDR x16, [x16, GOT_entry@PAGEOFF]
        ObjReloc ldrReloc;
        ldrReloc.offset = stubOff + 4;
        ldrReloc.type = elf_a64::kLdSt64Lo12Nc;
        ldrReloc.symIndex = gotSymIdx;
        ldrReloc.addend = 0;
        textSec.relocs.push_back(ldrReloc);

        // GOT entry: 8 bytes of zeros (dyld fills at load time).
        for (int j = 0; j < 8; ++j)
            gotSec.data.push_back(0);
    }

    stubObj.sections.push_back(std::move(textSec));
    stubObj.sections.push_back(std::move(gotSec));
    return stubObj;
}

/// @copydoc generateDynStubsX8664(const std::unordered_set<std::string> &)
ObjFile generateDynStubsX8664(const std::unordered_set<std::string> &dynamicSyms) {
    ObjFile stubObj;
    stubObj.name = "<elf64-dyn-stubs>";
    stubObj.synthetic = true;
    stubObj.format = ObjFileFormat::ELF;
    stubObj.is64bit = true;
    stubObj.isLittleEndian = true;
    stubObj.machine = 62; // EM_X86_64

    stubObj.sections.push_back(ObjSection{});

    ObjSection textSec;
    textSec.name = ".text";
    textSec.executable = true;
    textSec.writable = false;
    textSec.alloc = true;
    textSec.alignment = 16;

    // Same rationale as the AArch64 path above: keep the synthetic dyld GOT
    // slots in a section named separately from the user's .data so their
    // origin remains identifiable.
    ObjSection gotSec;
    gotSec.name = ".got.zanna_stubs";
    gotSec.executable = false;
    gotSec.writable = true;
    gotSec.alloc = true;
    gotSec.alignment = 8;

    stubObj.symbols.push_back(ObjSymbol{});

    std::vector<std::string> sorted(dynamicSyms.begin(), dynamicSyms.end());
    std::sort(sorted.begin(), sorted.end());

    for (size_t i = 0; i < sorted.size(); ++i) {
        const size_t gotOff = gotSec.data.size();

        // Imported data objects get storage instead of a trampoline: the name
        // must denote the value, not a jump to it. See loaderDataSymbolSize().
        const size_t dataSize = loaderDataSymbolSize(sorted[i]);
        if (dataSize != 0) {
            appendCopyRelocSlot(stubObj, gotSec, sorted[i], gotOff, dataSize);
            continue;
        }

        const size_t stubOff = textSec.data.size();

        ObjSymbol gotSym;
        gotSym.name = "__got_" + sorted[i];
        gotSym.binding = ObjSymbol::Global;
        gotSym.sectionIndex = 2;
        gotSym.offset = gotOff;
        const uint32_t gotSymIdx =
            checkedSymbolIndex(stubObj.symbols.size(), "x86_64 dynamic GOT symbol");
        stubObj.symbols.push_back(std::move(gotSym));

        ObjSymbol stubSym;
        stubSym.name = sorted[i];
        stubSym.binding = ObjSymbol::Global;
        stubSym.sectionIndex = 1;
        stubSym.offset = stubOff;
        stubObj.symbols.push_back(std::move(stubSym));

        textSec.data.insert(textSec.data.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});

        ObjReloc reloc;
        reloc.offset = stubOff + 2;
        reloc.type = 2; // R_X86_64_PC32
        reloc.symIndex = gotSymIdx;
        reloc.addend = -4;
        textSec.relocs.push_back(reloc);

        gotSec.data.resize(checkedAddSize(gotOff, 8, "x86_64 dynamic GOT slot"), 0);
    }

    if (!textSec.data.empty())
        stubObj.sections.push_back(std::move(textSec));
    if (!gotSec.data.empty()) {
        if (stubObj.sections.size() == 1)
            stubObj.sections.push_back(ObjSection{});
        stubObj.sections.push_back(std::move(gotSec));
    }

    return stubObj;
}

/// @copydoc generateStaticGotSlotsX8664
ObjFile generateStaticGotSlotsX8664(const std::vector<ObjFile> &objects,
                                    const std::unordered_set<std::string> &dynamicSyms) {
    // Names defined with global/weak binding by some ELF x86-64 input, and
    // names that already own a `__got_` slot (dynamic stubs, earlier runs).
    std::unordered_set<std::string> defined;
    std::unordered_set<std::string> haveSlot;
    for (const ObjFile &obj : objects) {
        if (obj.format != ObjFileFormat::ELF)
            continue;
        for (const ObjSymbol &sym : obj.symbols) {
            if (sym.name.empty())
                continue;
            if (sym.name.size() > 6 && sym.name.compare(0, 6, "__got_") == 0)
                haveSlot.insert(sym.name.substr(6));
            if (sym.name.size() > 7 && sym.name.compare(0, 7, "__gotl_") == 0)
                haveSlot.insert(sym.name.substr(7));
            if ((sym.binding == ObjSymbol::Global || sym.binding == ObjSymbol::Weak) &&
                (sym.sectionIndex != 0 || sym.absolute))
                defined.insert(sym.name);
        }
    }

    std::unordered_set<std::string> wanted;
    for (const ObjFile &obj : objects) {
        if (obj.format != ObjFileFormat::ELF || obj.synthetic)
            continue;
        for (const ObjSection &sec : obj.sections) {
            for (const ObjReloc &rel : sec.relocs) {
                if (rel.type != elf_x64::kGotPcRel)
                    continue;
                if (rel.symIndex >= obj.symbols.size())
                    continue;
                const ObjSymbol &sym = obj.symbols[rel.symIndex];
                if (sym.name.empty() || sym.binding == ObjSymbol::Local)
                    continue;
                if (dynamicSyms.count(sym.name) || haveSlot.count(sym.name) ||
                    !defined.count(sym.name))
                    continue;
                wanted.insert(sym.name);
            }
        }
    }

    ObjFile slotObj;
    slotObj.name = "<elf64-static-got>";
    slotObj.synthetic = true;
    slotObj.format = ObjFileFormat::ELF;
    slotObj.is64bit = true;
    slotObj.isLittleEndian = true;
    slotObj.machine = 62; // EM_X86_64
    if (wanted.empty())
        return slotObj;

    slotObj.sections.push_back(ObjSection{});
    ObjSection gotSec;
    gotSec.name = ".got.zanna_local";
    gotSec.executable = false;
    gotSec.writable = true;
    gotSec.alloc = true;
    gotSec.alignment = 8;
    slotObj.symbols.push_back(ObjSymbol{});

    std::vector<std::string> sorted(wanted.begin(), wanted.end());
    std::sort(sorted.begin(), sorted.end());
    for (const std::string &name : sorted) {
        const size_t gotOff = gotSec.data.size();

        ObjSymbol gotSym;
        gotSym.name = "__gotl_" + name; // distinct from loader-bound `__got_` slots
        gotSym.binding = ObjSymbol::Global;
        gotSym.sectionIndex = 1;
        gotSym.offset = gotOff;
        gotSym.size = 8;
        slotObj.symbols.push_back(std::move(gotSym));

        // Undefined reference; the applier resolves it by name to the
        // definition elsewhere in the link and writes the absolute address.
        ObjSymbol target;
        target.name = name;
        target.binding = ObjSymbol::Undefined;
        target.sectionIndex = 0;
        const uint32_t targetIdx =
            checkedSymbolIndex(slotObj.symbols.size(), "x86_64 static GOT target");
        slotObj.symbols.push_back(std::move(target));

        ObjReloc reloc;
        reloc.offset = gotOff;
        reloc.type = elf_x64::kAbs64; // R_X86_64_64
        reloc.symIndex = targetIdx;
        reloc.addend = 0;
        gotSec.relocs.push_back(reloc);

        gotSec.data.resize(checkedAddSize(gotOff, 8, "x86_64 static GOT slot"), 0);
    }
    slotObj.sections.push_back(std::move(gotSec));
    return slotObj;
}

} // namespace zanna::codegen::linker
