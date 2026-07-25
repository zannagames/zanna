//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/RelocationValidation.hpp
// Purpose: Shared relocation validation helpers used by every object file
//          writer. Provides RelocKind→name lookup, an arch-compatibility test,
//          and a single validateRelocationShape() that every writer calls so
//          ELF/Mach-O/COFF report identical diagnostics on bad relocations.
// Key invariants:
//   - relocKindName() returns the canonical short name used in error messages.
//   - relocationFixupWidth() returns 4 for all 32-bit fixups and 8 for Abs64.
//   - validateRelocationShape() rejects archs/widths that overrun the section.
// Ownership/Lifetime: Stateless inline helpers — no allocation.
// Links: ElfWriter.cpp, MachOWriter.cpp, CoffWriter.cpp,
//        codegen/common/objfile/Relocation.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file RelocationValidation.hpp
 * @brief Defines shared architecture, bounds, and opcode checks for object fixups.
 *
 * Every concrete object writer calls the same validation entry point before
 * translating a relocation. This keeps malformed encoder metadata diagnostics
 * consistent across ELF, Mach-O, and COFF.
 */

#pragma once

#include "codegen/common/AArch64RelocUtil.hpp"
#include "codegen/common/objfile/ObjectFileWriter.hpp"

#include <cstddef>
#include <ostream>

namespace zanna::codegen::objfile {

/// @brief Read an emitted AArch64 instruction at a logical relocation offset.
/// @param section Section supplying logical bias and physical bytes.
/// @param logicalOffset Valid logical offset of a four-byte instruction.
/// @return Little-endian instruction word.
inline uint32_t readRelocLE32(const CodeSection &section, size_t logicalOffset) {
    const size_t physicalOffset = logicalOffset - section.logicalOffsetBias();
    const auto &bytes = section.bytes();
    return static_cast<uint32_t>(bytes[physicalOffset]) |
           (static_cast<uint32_t>(bytes[physicalOffset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[physicalOffset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[physicalOffset + 3]) << 24);
}

/// @brief Recognize an AArch64 ADD-immediate instruction.
/// @param insn Instruction word to inspect.
/// @return `true` when the instruction accepts an ADD page-offset relocation.
inline bool isA64AddImmediate(uint32_t insn) {
    return zanna::codegen::isA64AddImmediate(insn);
}

/// @brief Decode the scale of an AArch64 unsigned-offset load/store.
/// @param insn Instruction word to inspect.
/// @param shift Receives the base-two byte-scale exponent.
/// @return `true` for a supported unsigned-offset encoding.
inline bool a64UnsignedLdStOffsetShift(uint32_t insn, uint32_t &shift) {
    return zanna::codegen::a64UnsignedLdStOffsetShift(insn, shift);
}

/// @brief Check an AArch64 unsigned-offset load/store's exact byte scale.
/// @param insn Instruction word to inspect.
/// @param expectedShift Required base-two scale exponent.
/// @return `true` when @p insn is compatible with that relocation width.
inline bool isA64UnsignedLdStOffsetWithShift(uint32_t insn, uint32_t expectedShift) {
    return zanna::codegen::isA64UnsignedLdStOffsetWithShift(insn, expectedShift);
}

/// @brief Return the canonical short name for a RelocKind enum value.
/// @details Used in writer diagnostics so ELF/Mach-O/COFF print the same name
///          for the same kind, regardless of which format-specific code emitted it.
/// @param kind Relocation kind to name.
/// @return Stable diagnostic string, or `"<unknown>"` for an invalid value.
inline const char *relocKindName(RelocKind kind) {
    switch (kind) {
        case RelocKind::PCRel32:
            return "PCRel32";
        case RelocKind::Branch32:
            return "Branch32";
        case RelocKind::Abs64:
            return "Abs64";
        case RelocKind::A64Call26:
            return "A64Call26";
        case RelocKind::A64Jump26:
            return "A64Jump26";
        case RelocKind::A64AdrpPage21:
            return "A64AdrpPage21";
        case RelocKind::A64AddPageOff12:
            return "A64AddPageOff12";
        case RelocKind::A64LdSt32Off12:
            return "A64LdSt32Off12";
        case RelocKind::A64LdSt64Off12:
            return "A64LdSt64Off12";
        case RelocKind::A64LdSt128Off12:
            return "A64LdSt128Off12";
        case RelocKind::A64CondBr19:
            return "A64CondBr19";
    }
    return "<unknown>";
}

/// @brief Test whether a RelocKind is legal for the target architecture.
/// @details Catches frontend bugs that would emit (e.g.) A64Call26 into an
///          x86_64 object before the writer would silently encode garbage.
/// @param kind Relocation kind recorded by the encoder.
/// @param arch Target object architecture.
/// @return `true` when the architecture defines @p kind.
inline bool relocationKindMatchesArch(RelocKind kind, ObjArch arch) {
    switch (kind) {
        case RelocKind::PCRel32:
        case RelocKind::Branch32:
            return arch == ObjArch::X86_64;
        case RelocKind::Abs64:
            return arch == ObjArch::X86_64 || arch == ObjArch::AArch64;
        case RelocKind::A64Call26:
        case RelocKind::A64Jump26:
        case RelocKind::A64AdrpPage21:
        case RelocKind::A64AddPageOff12:
        case RelocKind::A64LdSt32Off12:
        case RelocKind::A64LdSt64Off12:
        case RelocKind::A64LdSt128Off12:
        case RelocKind::A64CondBr19:
            return arch == ObjArch::AArch64;
    }
    return false;
}

/// @brief Return the byte-width of the fixup field for a RelocKind.
/// @details All current AArch64 + x86_64 32-bit fixups return 4; Abs64 returns 8.
///          Used to verify that the relocation's offset doesn't overrun the
///          section before any writer-specific patching runs.
/// @param kind Relocation kind to measure.
/// @return Fixup width in bytes, or zero for an invalid value.
inline size_t relocationFixupWidth(RelocKind kind) {
    switch (kind) {
        case RelocKind::PCRel32:
        case RelocKind::Branch32:
        case RelocKind::A64Call26:
        case RelocKind::A64Jump26:
        case RelocKind::A64AdrpPage21:
        case RelocKind::A64AddPageOff12:
        case RelocKind::A64LdSt32Off12:
        case RelocKind::A64LdSt64Off12:
        case RelocKind::A64LdSt128Off12:
        case RelocKind::A64CondBr19:
            return 4;
        case RelocKind::Abs64:
            return 8;
    }
    return 0;
}

/// @brief One-stop shape check used by every object-file writer before patching.
/// @details Verifies (1) the relocation kind matches the architecture, and
///          (2) the fixup window stays inside the section. On any failure
///          writes a self-contained diagnostic to @p err and returns false.
/// @param writerName "ELF writer", "Mach-O writer", "COFF writer" — included in errors.
/// @param arch Target architecture for the object being produced.
/// @param section Section containing the relocation site.
/// @param rel The relocation to validate.
/// @param sectionName Human-readable section name for diagnostics (e.g. "__text").
/// @param err Stream to receive diagnostic output on failure.
/// @return `true` when architecture, byte range, and AArch64 opcode all match.
inline bool validateRelocationShape(const char *writerName,
                                    ObjArch arch,
                                    const CodeSection &section,
                                    const Relocation &rel,
                                    const char *sectionName,
                                    std::ostream &err) {
    if (!relocationKindMatchesArch(rel.kind, arch)) {
        err << writerName << ": relocation kind " << relocKindName(rel.kind)
            << " is not valid for this object architecture in " << sectionName << " at offset "
            << rel.offset << "\n";
        return false;
    }

    const size_t width = relocationFixupWidth(rel.kind);
    if (!section.containsOffsetRange(rel.offset, width)) {
        err << writerName << ": relocation kind " << relocKindName(rel.kind) << " at offset "
            << rel.offset << " extends beyond " << sectionName << " contents\n";
        return false;
    }

    if (arch == ObjArch::AArch64 && width == 4) {
        const uint32_t insn = readRelocLE32(section, rel.offset);

        /// Emit the uniform diagnostic for an opcode/fixup mismatch.
        auto badInstruction = [&]() {
            err << writerName << ": relocation kind " << relocKindName(rel.kind) << " at offset "
                << rel.offset << " does not match the AArch64 instruction in " << sectionName
                << "\n";
            return false;
        };
        switch (rel.kind) {
            case RelocKind::A64Call26:
                if ((insn & 0xFC000000u) != 0x94000000u)
                    return badInstruction();
                break;
            case RelocKind::A64Jump26:
                if ((insn & 0xFC000000u) != 0x14000000u)
                    return badInstruction();
                break;
            case RelocKind::A64CondBr19:
                if ((insn & 0xFF000010u) != 0x54000000u && (insn & 0x7E000000u) != 0x34000000u)
                    return badInstruction();
                break;
            case RelocKind::A64AdrpPage21:
                if ((insn & 0x9F000000u) != 0x90000000u)
                    return badInstruction();
                break;
            case RelocKind::A64AddPageOff12:
                if (!isA64AddImmediate(insn))
                    return badInstruction();
                break;
            case RelocKind::A64LdSt32Off12:
                if (!isA64UnsignedLdStOffsetWithShift(insn, 2))
                    return badInstruction();
                break;
            case RelocKind::A64LdSt64Off12:
                if (!isA64UnsignedLdStOffsetWithShift(insn, 3))
                    return badInstruction();
                break;
            case RelocKind::A64LdSt128Off12:
                if (!isA64UnsignedLdStOffsetWithShift(insn, 4))
                    return badInstruction();
                break;
            default:
                break;
        }
    }

    return true;
}

} // namespace zanna::codegen::objfile
