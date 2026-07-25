//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/RelocClassify.hpp
// Purpose: Map format-specific relocation type numbers to format-independent
//          RelocAction categories. Separates relocation classification from
//          the patching logic in RelocApplier.cpp.
// Key invariants:
//   - One mapping function per (format × architecture) combination
//   - classifyReloc() dispatches by format + arch to the correct mapper
//   - Uses named constants from RelocConstants.hpp (no raw integers)
// Links: RelocApplier.cpp, RelocConstants.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file RelocClassify.hpp
 * @brief Classifies ABI-specific relocation numbers into linker patch actions.
 *
 * Relocation numbers overlap between object formats and architectures. These
 * small mapping functions keep that format-dependent namespace boundary out of
 * the byte-patching implementation and provide a single @ref RelocAction
 * vocabulary for the relocation applier.
 */

#pragma once

#include "codegen/common/linker/ObjFileReader.hpp"
#include "codegen/common/linker/RelocConstants.hpp"

#include <cstdint>

namespace zanna::codegen::linker {

/// @brief Format-independent operations performed by the relocation applier.
/// @details Each enumerator describes the mathematical or instruction-level
///          patch to perform after the raw relocation number has been decoded
///          in the context of its object format and target architecture.
enum class RelocAction {
    PCRel32,      // S + A - P (32-bit)
    PCRel64,      // S + A - P (64-bit)
    Abs64,        // S + A (64-bit)
    Abs32,        // S + A (32-bit)
    TlsOffset32,  // ELF x86_64 local-exec TLS: S + A - TP
    GotPCRel32,   // ELF x86_64 GOT slot address relative to RIP
    Branch26,     // AArch64: ((S+A-P)>>2) & 0x3FFFFFF
    Page21,       // AArch64: ADRP page delta
    PageOff12,    // AArch64: ADD page offset
    PageOff12A,   // COFF AArch64: arithmetic page offset
    PageOff12L,   // COFF AArch64: load/store page offset
    LdSt64Off,    // AArch64: LDR/STR 64-bit scaled offset
    LdSt32Off,    // AArch64: LDR/STR 32-bit scaled offset
    LdSt128Off,   // AArch64: LDR/STR 128-bit scaled offset
    CondBr19,     // AArch64: B.cond 19-bit
    GotPage21,    // AArch64: GOT ADRP (relaxable to Page21 for local symbols)
    GotPageOff12, // AArch64: GOT LDR pageoff (relaxable to ADD for local symbols)
    GotPointer,   // AArch64 Mach-O: 32-bit PC-rel or 64-bit absolute pointer to a GOT slot
    TlsA64AddTprelHi12, // ELF AArch64 local-exec TLS: ADD imm bits 23:12
    TlsA64AddTprelLo12, // ELF AArch64 local-exec TLS: ADD imm bits 11:0
    TlsA64AddTprelLo12Nc, // ELF AArch64 local-exec TLS: ADD imm bits 11:0, no overflow check
    Unknown,
};

// ── Per-format mapping functions ─────────────────────────────────────────
//
// Each helper maps a single (format × architecture) relocation-type number to
// a format-independent RelocAction. RelocApplier then performs the patching
// math by RelocAction, never by raw type number. Unknown types fall through
// to RelocAction::Unknown so the caller can report a precise diagnostic.

/// @brief Map an ELF/x86_64 relocation type code to a format-independent action.
/// @param type ELF `R_X86_64_*` relocation number.
/// @return The corresponding patch action, or @ref RelocAction::Unknown when
///         the relocation is not supported by the native linker.
inline RelocAction elfX64Action(uint32_t type) {
    switch (type) {
        case elf_x64::kAbs64:
            return RelocAction::Abs64;
        case elf_x64::kPC32:
            return RelocAction::PCRel32;
        case elf_x64::kPLT32:
            return RelocAction::PCRel32;
        case elf_x64::kGotPcRel:
        case elf_x64::kGotPcRelX:
        case elf_x64::kRexGotPcRelX:
            return RelocAction::GotPCRel32;
        case elf_x64::kAbs32:
            return RelocAction::Abs32;
        case elf_x64::kTpoff32:
            return RelocAction::TlsOffset32;
        default:
            return RelocAction::Unknown;
    }
}

/// @brief Map an ELF/AArch64 relocation type code to a format-independent action.
/// @param type ELF `R_AARCH64_*` relocation number.
/// @return The corresponding patch action, or @ref RelocAction::Unknown when
///         the relocation is not supported by the native linker.
inline RelocAction elfA64Action(uint32_t type) {
    switch (type) {
        case elf_a64::kAbs64:
            return RelocAction::Abs64;
        case elf_a64::kPrel64:
            return RelocAction::PCRel64;
        case elf_a64::kPrel32:
            return RelocAction::PCRel32;
        case elf_a64::kAdrPrelPgHi21:
            return RelocAction::Page21;
        case elf_a64::kAddAbsLo12Nc:
            return RelocAction::PageOff12;
        case elf_a64::kLdSt8Lo12Nc:
            return RelocAction::PageOff12;
        case elf_a64::kCondBr19:
            return RelocAction::CondBr19;
        case elf_a64::kJump26:
            return RelocAction::Branch26;
        case elf_a64::kCall26:
            return RelocAction::Branch26;
        case elf_a64::kLdSt32Lo12Nc:
            return RelocAction::LdSt32Off;
        case elf_a64::kLdSt64Lo12Nc:
            return RelocAction::LdSt64Off;
        case elf_a64::kLdSt128Lo12Nc:
            return RelocAction::LdSt128Off;
        case elf_a64::kAdrGotPage:
            return RelocAction::GotPage21;
        case elf_a64::kLd64GotLo12Nc:
            return RelocAction::GotPageOff12;
        case elf_a64::kTlsLeAddTprelHi12:
            return RelocAction::TlsA64AddTprelHi12;
        case elf_a64::kTlsLeAddTprelLo12:
            return RelocAction::TlsA64AddTprelLo12;
        case elf_a64::kTlsLeAddTprelLo12Nc:
            return RelocAction::TlsA64AddTprelLo12Nc;
        default:
            return RelocAction::Unknown;
    }
}

/// @brief Map a Mach-O/x86_64 relocation type code to a format-independent action.
/// @param type Mach-O `X86_64_RELOC_*` relocation number.
/// @return The corresponding patch action, or @ref RelocAction::Unknown when
///         the relocation is not supported by the native linker.
inline RelocAction machoX64Action(uint32_t type) {
    switch (type) {
        case macho_x64::kUnsigned:
            return RelocAction::Abs64;
        case macho_x64::kSigned:
        case macho_x64::kSigned1:
        case macho_x64::kSigned2:
        case macho_x64::kSigned4:
            return RelocAction::PCRel32;
        case macho_x64::kBranch:
            return RelocAction::PCRel32;
        default:
            return RelocAction::Unknown;
    }
}

/// @brief Map a Mach-O/AArch64 relocation type code to a format-independent action.
/// @param type Mach-O `ARM64_RELOC_*` relocation number.
/// @return The corresponding patch action, or @ref RelocAction::Unknown when
///         the relocation is not supported by the native linker.
inline RelocAction machoA64Action(uint32_t type) {
    switch (type) {
        case macho_a64::kUnsigned:
            return RelocAction::Abs64;
        case macho_a64::kBranch26:
            return RelocAction::Branch26;
        case macho_a64::kPage21:
            return RelocAction::Page21;
        case macho_a64::kPageOff12:
            return RelocAction::PageOff12;
        case macho_a64::kGotLoadPage21:
            return RelocAction::GotPage21;
        case macho_a64::kGotLoadPageOff12:
            return RelocAction::GotPageOff12;
        case macho_a64::kPointerToGot:
            return RelocAction::GotPointer;
        case macho_a64::kTlvpLoadPage21:
            return RelocAction::Page21;
        case macho_a64::kTlvpLoadPageOff12:
            // TLV descriptor address known at link time. Rewrite LDR to ADD
            // (GOT relaxation style) so the code gets a pointer TO the descriptor,
            // not a load FROM it.
            return RelocAction::GotPageOff12;
        default:
            return RelocAction::Unknown;
    }
}

/// @brief Map a COFF/x86_64 relocation type code to a format-independent action.
/// @param type COFF `IMAGE_REL_AMD64_*` relocation number.
/// @return The corresponding patch action, or @ref RelocAction::Unknown when
///         the relocation is not supported by the native linker.
inline RelocAction coffX64Action(uint32_t type) {
    switch (type) {
        case coff_x64::kAddr64:
            return RelocAction::Abs64;
        case coff_x64::kAddr32:
            return RelocAction::Abs32;
        case coff_x64::kRel32:
        case coff_x64::kRel32_1:
        case coff_x64::kRel32_2:
        case coff_x64::kRel32_3:
        case coff_x64::kRel32_4:
        case coff_x64::kRel32_5:
            return RelocAction::PCRel32;
        default:
            return RelocAction::Unknown;
    }
}

/// @brief Map a COFF/AArch64 relocation type code to a format-independent action.
/// @param type COFF `IMAGE_REL_ARM64_*` relocation number.
/// @return The corresponding patch action, or @ref RelocAction::Unknown when
///         the relocation is not supported by the native linker.
inline RelocAction coffA64Action(uint32_t type) {
    switch (type) {
        case coff_a64::kAddr64:
            return RelocAction::Abs64;
        case coff_a64::kAddr32:
            return RelocAction::Abs32;
        case coff_a64::kBranch26:
            return RelocAction::Branch26;
        case coff_a64::kPageRel21:
            return RelocAction::Page21;
        case coff_a64::kPageOff12A:
            return RelocAction::PageOff12A;
        case coff_a64::kPageOff12L:
            return RelocAction::PageOff12L;
        case coff_a64::kBranch19:
            return RelocAction::CondBr19;
        default:
            return RelocAction::Unknown;
    }
}

// ── Top-level dispatcher ─────────────────────────────────────────────────

/// @brief Dispatch a raw relocation number using its format and architecture.
/// @param format Object container format that defines the relocation namespace.
/// @param arch Target architecture that selects the ABI-specific mapping.
/// @param type Raw relocation type number from the object record.
/// @return The format-independent patch action, or @ref RelocAction::Unknown
///         for unsupported formats, architectures, or relocation numbers.
inline RelocAction classifyReloc(ObjFileFormat format, LinkArch arch, uint32_t type) {
    switch (format) {
        case ObjFileFormat::ELF:
            return (arch == LinkArch::X86_64) ? elfX64Action(type) : elfA64Action(type);
        case ObjFileFormat::MachO:
            return (arch == LinkArch::X86_64) ? machoX64Action(type) : machoA64Action(type);
        case ObjFileFormat::COFF:
            return (arch == LinkArch::X86_64) ? coffX64Action(type) : coffA64Action(type);
        default:
            return RelocAction::Unknown;
    }
}

} // namespace zanna::codegen::linker
