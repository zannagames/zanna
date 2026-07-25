//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/RelocConstants.hpp
// Purpose: Named constants for relocation type numbers across ELF, Mach-O,
//          and COFF formats. Replaces raw integer literals in switch
//          statements and stub generators with self-documenting names.
// Key invariants:
//   - Values match their respective ABI specification exactly
//   - Format-prefixed names prevent collisions (e.g., ELF type 1 vs COFF type 1)
// Links: RelocApplier.cpp, DynStubGen.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file RelocConstants.hpp
 * @brief Defines named native relocation numbers for every supported object ABI.
 *
 * The constants are grouped by both object format and architecture because raw
 * relocation values are meaningful only within that pair. Their numeric values
 * mirror the platform ABI definitions and are shared by object writers,
 * relocation classification, dynamic-stub generation, and patching code.
 */

#pragma once

#include <cstdint>

namespace zanna::codegen::linker {

// ── ELF x86_64 Relocation Types (elf.h / System V AMD64 ABI) ────────────

/// @brief ELF AMD64 `R_X86_64_*` relocation numbers.
namespace elf_x64 {
constexpr uint32_t kAbs64 = 1;         // R_X86_64_64
constexpr uint32_t kPC32 = 2;          // R_X86_64_PC32
constexpr uint32_t kPLT32 = 4;         // R_X86_64_PLT32
constexpr uint32_t kGotPcRel = 9;      // R_X86_64_GOTPCREL
constexpr uint32_t kAbs32 = 10;        // R_X86_64_32
constexpr uint32_t kTpoff32 = 23;      // R_X86_64_TPOFF32
constexpr uint32_t kGotPcRelX = 41;    // R_X86_64_GOTPCRELX
constexpr uint32_t kRexGotPcRelX = 42; // R_X86_64_REX_GOTPCRELX
} // namespace elf_x64

// ── ELF AArch64 Relocation Types (ELF for ARM 64-bit Architecture) ──────

/// @brief ELF AArch64 `R_AARCH64_*` relocation numbers.
namespace elf_a64 {
constexpr uint32_t kAbs64 = 257;         // R_AARCH64_ABS64
constexpr uint32_t kPrel64 = 260;        // R_AARCH64_PREL64
constexpr uint32_t kPrel32 = 261;        // R_AARCH64_PREL32
constexpr uint32_t kAdrPrelPgHi21 = 275; // R_AARCH64_ADR_PREL_PG_HI21
constexpr uint32_t kAddAbsLo12Nc = 277;  // R_AARCH64_ADD_ABS_LO12_NC
constexpr uint32_t kLdSt8Lo12Nc = 278;   // R_AARCH64_LDST8_ABS_LO12_NC
constexpr uint32_t kCondBr19 = 280;      // R_AARCH64_CONDBR19
constexpr uint32_t kJump26 = 282;        // R_AARCH64_JUMP26
constexpr uint32_t kCall26 = 283;        // R_AARCH64_CALL26
constexpr uint32_t kLdSt32Lo12Nc = 285;  // R_AARCH64_LDST32_ABS_LO12_NC
constexpr uint32_t kLdSt64Lo12Nc = 286;  // R_AARCH64_LDST64_ABS_LO12_NC
constexpr uint32_t kLdSt128Lo12Nc = 299; // R_AARCH64_LDST128_ABS_LO12_NC
constexpr uint32_t kAdrGotPage = 311;    // R_AARCH64_ADR_GOT_PAGE
constexpr uint32_t kLd64GotLo12Nc = 312; // R_AARCH64_LD64_GOT_LO12_NC
constexpr uint32_t kTlsLeAddTprelHi12 = 549;    // R_AARCH64_TLSLE_ADD_TPREL_HI12
constexpr uint32_t kTlsLeAddTprelLo12 = 550;    // R_AARCH64_TLSLE_ADD_TPREL_LO12
constexpr uint32_t kTlsLeAddTprelLo12Nc = 551;  // R_AARCH64_TLSLE_ADD_TPREL_LO12_NC
} // namespace elf_a64

// ── Mach-O x86_64 Relocation Types (mach-o/x86_64/reloc.h) ─────────────

/// @brief Mach-O x86-64 `X86_64_RELOC_*` relocation numbers.
namespace macho_x64 {
constexpr uint32_t kUnsigned = 0;   // X86_64_RELOC_UNSIGNED
constexpr uint32_t kSigned = 1;     // X86_64_RELOC_SIGNED
constexpr uint32_t kBranch = 2;     // X86_64_RELOC_BRANCH
constexpr uint32_t kGotLoad = 3;    // X86_64_RELOC_GOT_LOAD
constexpr uint32_t kGot = 4;        // X86_64_RELOC_GOT
constexpr uint32_t kSubtractor = 5; // X86_64_RELOC_SUBTRACTOR
constexpr uint32_t kSigned1 = 6;    // X86_64_RELOC_SIGNED_1
constexpr uint32_t kSigned2 = 7;    // X86_64_RELOC_SIGNED_2
constexpr uint32_t kSigned4 = 8;    // X86_64_RELOC_SIGNED_4
constexpr uint32_t kTlv = 9;        // X86_64_RELOC_TLV
} // namespace macho_x64

// ── Mach-O ARM64 Relocation Types (mach-o/arm64/reloc.h) ────────────────

/// @brief Mach-O arm64 `ARM64_RELOC_*` relocation numbers.
namespace macho_a64 {
constexpr uint32_t kUnsigned = 0;          // ARM64_RELOC_UNSIGNED
constexpr uint32_t kSubtractor = 1;        // ARM64_RELOC_SUBTRACTOR
constexpr uint32_t kBranch26 = 2;          // ARM64_RELOC_BRANCH26
constexpr uint32_t kPage21 = 3;            // ARM64_RELOC_PAGE21
constexpr uint32_t kPageOff12 = 4;         // ARM64_RELOC_PAGEOFF12
constexpr uint32_t kGotLoadPage21 = 5;     // ARM64_RELOC_GOT_LOAD_PAGE21
constexpr uint32_t kGotLoadPageOff12 = 6;  // ARM64_RELOC_GOT_LOAD_PAGEOFF12
constexpr uint32_t kPointerToGot = 7;      // ARM64_RELOC_POINTER_TO_GOT
constexpr uint32_t kTlvpLoadPage21 = 8;    // ARM64_RELOC_TLVP_LOAD_PAGE21
constexpr uint32_t kTlvpLoadPageOff12 = 9; // ARM64_RELOC_TLVP_LOAD_PAGEOFF12
} // namespace macho_a64

// ── COFF AMD64 Relocation Types (winnt.h) ───────────────────────────────

/// @brief PE/COFF AMD64 `IMAGE_REL_AMD64_*` relocation numbers.
namespace coff_x64 {
constexpr uint32_t kAddr64 = 1;   // IMAGE_REL_AMD64_ADDR64
constexpr uint32_t kAddr32 = 2;   // IMAGE_REL_AMD64_ADDR32
constexpr uint32_t kAddr32Nb = 3; // IMAGE_REL_AMD64_ADDR32NB
constexpr uint32_t kRel32 = 4;    // IMAGE_REL_AMD64_REL32
constexpr uint32_t kRel32_1 = 5;  // IMAGE_REL_AMD64_REL32_1
constexpr uint32_t kRel32_2 = 6;  // IMAGE_REL_AMD64_REL32_2
constexpr uint32_t kRel32_3 = 7;  // IMAGE_REL_AMD64_REL32_3
constexpr uint32_t kRel32_4 = 8;  // IMAGE_REL_AMD64_REL32_4
constexpr uint32_t kRel32_5 = 9;  // IMAGE_REL_AMD64_REL32_5
constexpr uint32_t kSection = 10; // IMAGE_REL_AMD64_SECTION
constexpr uint32_t kSecRel = 11;  // IMAGE_REL_AMD64_SECREL
} // namespace coff_x64

// ── COFF ARM64 Relocation Types (winnt.h) ───────────────────────────────

/// @brief PE/COFF ARM64 `IMAGE_REL_ARM64_*` relocation numbers.
namespace coff_a64 {
constexpr uint32_t kAddr32 = 1;         // IMAGE_REL_ARM64_ADDR32
constexpr uint32_t kAddr32Nb = 2;       // IMAGE_REL_ARM64_ADDR32NB
constexpr uint32_t kBranch26 = 3;       // IMAGE_REL_ARM64_BRANCH26
constexpr uint32_t kPageRel21 = 4;      // IMAGE_REL_ARM64_PAGEBASE_REL21
constexpr uint32_t kPageOff12A = 6;     // IMAGE_REL_ARM64_PAGEOFFSET_12A
constexpr uint32_t kPageOff12L = 7;     // IMAGE_REL_ARM64_PAGEOFFSET_12L
constexpr uint32_t kSecRel = 8;         // IMAGE_REL_ARM64_SECREL
constexpr uint32_t kSecRelLow12A = 9;   // IMAGE_REL_ARM64_SECREL_LOW12A
constexpr uint32_t kSecRelHigh12A = 10; // IMAGE_REL_ARM64_SECREL_HIGH12A
constexpr uint32_t kSecRelLow12L = 11;  // IMAGE_REL_ARM64_SECREL_LOW12L
constexpr uint32_t kSection = 13;       // IMAGE_REL_ARM64_SECTION
constexpr uint32_t kAddr64 = 14;        // IMAGE_REL_ARM64_ADDR64
constexpr uint32_t kBranch19 = 15;      // IMAGE_REL_ARM64_BRANCH19
} // namespace coff_a64

} // namespace zanna::codegen::linker
