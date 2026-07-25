//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/binenc/A64Encoding.hpp
// Purpose: AArch64 instruction encoding constants, templates, and helpers for
//          the binary encoder. Provides 32-bit instruction templates for each
//          opcode class, register-to-hardware mapping, and condition code encoding.
// Key invariants:
//   - Every AArch64 instruction is exactly 32 bits (4 bytes), little-endian
//   - PhysReg enum values map directly to hardware encoding for GPRs (0-31)
//   - FPR hardware encoding = PhysReg enum value - 32
//   - Condition codes are 4-bit values; inversion flips the LSB
// Ownership/Lifetime:
//   - Helpers are inline and stateless; invalid runtime inputs throw
// Links: src/codegen/aarch64/TargetAArch64.hpp,
//        src/codegen/aarch64/binenc/A64BinaryEncoder.hpp,
//        src/codegen/aarch64/binenc/A64BinaryEncoder.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/TargetAArch64.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <stdexcept>

/**
 * @file
 * @brief Defines AArch64 instruction templates and field-packing helpers.
 *
 * Constants provide fixed opcode bits for the binary emitter. Helper functions
 * map Zanna register/condition representations to architectural fields and
 * encode the nontrivial logical and floating-point immediate formats.
 */

namespace zanna::codegen::aarch64::binenc {

// =============================================================================
// Register Encoding
// =============================================================================

/// @brief Map a GPR `PhysReg` to its 5-bit hardware encoding (0-31).
/// @details GPR PhysReg values are defined to coincide with the architectural
///          register number, so the mapping is a direct cast after the kind check.
/// @param r Physical register expected to be one of `X0..X30`/`SP`/`XZR`.
/// @return 5-bit hardware encoding suitable for the `Rd`/`Rn`/`Rm`/`Rt` field.
/// @throws std::invalid_argument if @p r is not a GPR.
inline uint32_t hwGPR(PhysReg r) {
    if (!isGPR(r))
        throw std::invalid_argument("AArch64 binary encoder: expected a GPR register");
    return static_cast<uint32_t>(r);
}

/// @brief Map an FPR `PhysReg` (V0-V31) to its 5-bit hardware encoding (0-31).
/// @details The PhysReg enum lays GPRs (0-31) before FPRs (V0=32 onward), so the
///          hardware encoding for an FPR is its enum value minus `V0`.
/// @param r Physical register expected to be one of `V0..V31`.
/// @return 5-bit hardware encoding for the FPR.
/// @throws std::invalid_argument if @p r is not an FPR.
inline uint32_t hwFPR(PhysReg r) {
    if (!isFPR(r))
        throw std::invalid_argument("AArch64 binary encoder: expected an FPR register");
    return static_cast<uint32_t>(r) - static_cast<uint32_t>(PhysReg::V0);
}

// =============================================================================
// Condition Code Encoding
// =============================================================================

/// @brief Map a condition code mnemonic to its 4-bit AArch64 encoding.
/// @details Accepts the two-character condition mnemonics defined by the
///          ARM ARM (`eq`, `ne`, `hs`/`cs`, `lo`/`cc`, `mi`, `pl`, `vs`,
///          `vc`, `hi`, `ls`, `ge`, `lt`, `gt`, `le`, `al`, `nv`). The
///          aliases `hs`/`cs` and `lo`/`cc` encode identically.
/// @param cond Two-character condition mnemonic (NUL-terminated).
/// @return 4-bit condition encoding suitable for `B.cond` / `CSEL` / `CSET`.
/// @throws std::invalid_argument if @p cond is null or not a recognised mnemonic.
inline uint32_t condCode(const char *cond) {
    if (!cond)
        throw std::invalid_argument("AArch64 binary encoder: missing condition code");

    // Fast 2-char comparison using memcmp
    struct Entry {
        const char str[3];
        uint32_t code;
    };

    static constexpr Entry table[] = {
        {"eq", 0x0},
        {"ne", 0x1},
        {"hs", 0x2},
        {"cs", 0x2},
        {"lo", 0x3},
        {"cc", 0x3},
        {"mi", 0x4},
        {"pl", 0x5},
        {"vs", 0x6},
        {"vc", 0x7},
        {"hi", 0x8},
        {"ls", 0x9},
        {"ge", 0xA},
        {"lt", 0xB},
        {"gt", 0xC},
        {"le", 0xD},
        {"al", 0xE},
        {"nv", 0xF},
    };
    for (const auto &e : table)
        if (std::strcmp(cond, e.str) == 0)
            return e.code;
    throw std::invalid_argument("AArch64 binary encoder: invalid condition code");
}

/// @brief Invert an AArch64 4-bit condition code.
/// @details ARM defines the inverse of every condition as `cc XOR 1`, e.g.
///          `EQ` (0x0) inverts to `NE` (0x1), `LT` (0xB) inverts to `GE` (0xA).
///          Used by `CSET` (CSINC) which encodes the *inverted* condition.
/// @param cc 4-bit condition code returned by @ref condCode.
/// @return 4-bit inverse condition code.
constexpr uint32_t invertCond(uint32_t cc) {
    return cc ^ 1;
}

// =============================================================================
// Instruction Templates — Data Processing (Three-Register)
// =============================================================================

inline constexpr uint32_t kAddRRR = 0x8B000000; // add  Xd, Xn, Xm
inline constexpr uint32_t kSubRRR = 0xCB000000; // sub  Xd, Xn, Xm
inline constexpr uint32_t kAndRRR = 0x8A000000; // and  Xd, Xn, Xm
inline constexpr uint32_t kOrrRRR = 0xAA000000; // orr  Xd, Xn, Xm
inline constexpr uint32_t kEorRRR = 0xCA000000; // eor  Xd, Xn, Xm
// Logical immediate: AND/ORR/EOR Xd, Xn, #bitmask  (N:immr:imms encoding)
inline constexpr uint32_t kAndImm = 0x92000000;  // and  Xd, Xn, #bitmask
inline constexpr uint32_t kOrrImm = 0xB2000000;  // orr  Xd, Xn, #bitmask
inline constexpr uint32_t kEorImm = 0xD2000000;  // eor  Xd, Xn, #bitmask
inline constexpr uint32_t kAddsRRR = 0xAB000000; // adds Xd, Xn, Xm
inline constexpr uint32_t kSubsRRR = 0xEB000000; // subs Xd, Xn, Xm
inline constexpr uint32_t kAndsRRR = 0xEA000000; // ands Xd, Xn, Xm (for TST alias)

// =============================================================================
// Instruction Templates — Variable Shift
// =============================================================================

inline constexpr uint32_t kLslvRRR = 0x9AC02000; // lslv Xd, Xn, Xm
inline constexpr uint32_t kLsrvRRR = 0x9AC02400; // lsrv Xd, Xn, Xm
inline constexpr uint32_t kAsrvRRR = 0x9AC02800; // asrv Xd, Xn, Xm

// =============================================================================
// Instruction Templates — Multiply / Divide
// =============================================================================

inline constexpr uint32_t kMulRRR = 0x9B007C00;   // madd Xd, Xn, Xm, XZR (mul alias)
inline constexpr uint32_t kSmulhRRR = 0x9B407C00; // smulh Xd, Xn, Xm
inline constexpr uint32_t kUmulhRRR = 0x9BC07C00; // umulh Xd, Xn, Xm
inline constexpr uint32_t kSDivRRR = 0x9AC00C00;  // sdiv Xd, Xn, Xm
inline constexpr uint32_t kUDivRRR = 0x9AC00800;  // udiv Xd, Xn, Xm
inline constexpr uint32_t kMSubRRRR = 0x9B008000; // msub Xd, Xn, Xm, Xa
inline constexpr uint32_t kMAddRRRR = 0x9B000000; // madd Xd, Xn, Xm, Xa

// =============================================================================
// Instruction Templates — Immediate (Add/Sub)
// =============================================================================

inline constexpr uint32_t kAddRI = 0x91000000;  // add  Xd, Xn, #imm12
inline constexpr uint32_t kSubRI = 0xD1000000;  // sub  Xd, Xn, #imm12
inline constexpr uint32_t kAddsRI = 0xB1000000; // adds Xd, Xn, #imm12
inline constexpr uint32_t kSubsRI = 0xF1000000; // subs Xd, Xn, #imm12

// =============================================================================
// Instruction Templates — Move
// =============================================================================

inline constexpr uint32_t kMovRR = 0xAA0003E0;  // orr Xd, XZR, Xm (mov alias)
inline constexpr uint32_t kMovZ = 0xD2800000;   // movz Xd, #imm16          (lsl #0)
inline constexpr uint32_t kMovZ16 = 0xD2A00000; // movz Xd, #imm16, lsl #16
inline constexpr uint32_t kMovZ32 = 0xD2C00000; // movz Xd, #imm16, lsl #32
inline constexpr uint32_t kMovZ48 = 0xD2E00000; // movz Xd, #imm16, lsl #48
inline constexpr uint32_t kMovN = 0x92800000;   // movn Xd, #imm16          (lsl #0)
inline constexpr uint32_t kMovN16 = 0x92A00000; // movn Xd, #imm16, lsl #16
inline constexpr uint32_t kMovN32 = 0x92C00000; // movn Xd, #imm16, lsl #32
inline constexpr uint32_t kMovN48 = 0x92E00000; // movn Xd, #imm16, lsl #48
inline constexpr uint32_t kMovK = 0xF2800000;   // movk Xd, #imm16          (lsl #0)
inline constexpr uint32_t kMovK16 = 0xF2A00000; // movk Xd, #imm16, lsl #16
inline constexpr uint32_t kMovK32 = 0xF2C00000; // movk Xd, #imm16, lsl #32
inline constexpr uint32_t kMovK48 = 0xF2E00000; // movk Xd, #imm16, lsl #48

// =============================================================================
// Instruction Templates — Bitfield (Shift Aliases)
// =============================================================================

inline constexpr uint32_t kUbfm = 0xD3400000; // ubfm Xd, Xn, #immr, #imms (for LSL/LSR)
inline constexpr uint32_t kSbfm = 0x93400000; // sbfm Xd, Xn, #immr, #imms (for ASR)

// =============================================================================
// Instruction Templates — Conditional
// =============================================================================

inline constexpr uint32_t kCset = 0x9A9F07E0; // csinc Xd, XZR, XZR, invert(cond)
inline constexpr uint32_t kCsel = 0x9A800000; // csel Xd, Xn, Xm, cond

// =============================================================================
// Instruction Templates — Load/Store Unsigned Offset
// =============================================================================

inline constexpr uint32_t kLdrGpr = 0xF9400000;   // ldr Xt, [Xn, #imm12*8]
inline constexpr uint32_t kStrGpr = 0xF9000000;   // str Xt, [Xn, #imm12*8]
inline constexpr uint32_t kLdr8Gpr = 0x39400000;  // ldrb Wt, [Xn, #imm12]
inline constexpr uint32_t kStr8Gpr = 0x39000000;  // strb Wt, [Xn, #imm12]
inline constexpr uint32_t kLdr16Gpr = 0x79400000; // ldrh Wt, [Xn, #imm12*2]
inline constexpr uint32_t kStr16Gpr = 0x79000000; // strh Wt, [Xn, #imm12*2]
inline constexpr uint32_t kLdr32Gpr = 0xB9400000; // ldr  Wt, [Xn, #imm12*4]
inline constexpr uint32_t kStr32Gpr = 0xB9000000; // str  Wt, [Xn, #imm12*4]
inline constexpr uint32_t kLdrFpr = 0xFD400000;   // ldr Dt, [Xn, #imm12*8]
inline constexpr uint32_t kStrFpr = 0xFD000000;   // str Dt, [Xn, #imm12*8]

// Unscaled variants (for offsets not divisible by 8 or negative)
inline constexpr uint32_t kLdurGpr = 0xF8400000;   // ldur Xt, [Xn, #simm9]
inline constexpr uint32_t kSturGpr = 0xF8000000;   // stur Xt, [Xn, #simm9]
inline constexpr uint32_t kLdur8Gpr = 0x38400000;  // ldurb Wt, [Xn, #simm9]
inline constexpr uint32_t kStur8Gpr = 0x38000000;  // sturb Wt, [Xn, #simm9]
inline constexpr uint32_t kLdur16Gpr = 0x78400000; // ldurh Wt, [Xn, #simm9]
inline constexpr uint32_t kStur16Gpr = 0x78000000; // sturh Wt, [Xn, #simm9]
inline constexpr uint32_t kLdur32Gpr = 0xB8400000; // ldur  Wt, [Xn, #simm9]
inline constexpr uint32_t kStur32Gpr = 0xB8000000; // stur  Wt, [Xn, #simm9]
inline constexpr uint32_t kLdurFpr = 0xFC400000;   // ldur Dt, [Xn, #simm9]
inline constexpr uint32_t kSturFpr = 0xFC000000;   // stur Dt, [Xn, #simm9]

// =============================================================================
// Instruction Templates — Load/Store Pair
// =============================================================================

inline constexpr uint32_t kLdpGpr = 0xA9400000; // ldp Xt1, Xt2, [Xn, #imm7*8]
inline constexpr uint32_t kStpGpr = 0xA9000000; // stp Xt1, Xt2, [Xn, #imm7*8]
inline constexpr uint32_t kLdpFpr = 0x6D400000; // ldp Dt1, Dt2, [Xn, #imm7*8]
inline constexpr uint32_t kStpFpr = 0x6D000000; // stp Dt1, Dt2, [Xn, #imm7*8]

// Pre-indexed pair (for prologue: stp x29, x30, [sp, #-16]!)
inline constexpr uint32_t kStpGprPre = 0xA9800000;
inline constexpr uint32_t kLdpGprPost = 0xA8C00000; // ldp Xt1, Xt2, [Xn], #imm
inline constexpr uint32_t kStpFprPre = 0x6D800000;
inline constexpr uint32_t kLdpFprPost = 0x6CC00000;

// Single register pre/post-indexed (for odd callee-saved count)
inline constexpr uint32_t kStrGprPre = 0xF8000C00;  // str Xt, [Xn, #simm9]!
inline constexpr uint32_t kLdrGprPost = 0xF8400400; // ldr Xt, [Xn], #simm9
inline constexpr uint32_t kStrFprPre = 0xFC000C00;  // str Dt, [Xn, #simm9]!
inline constexpr uint32_t kLdrFprPost = 0xFC400400; // ldr Dt, [Xn], #simm9

// =============================================================================
// Instruction Templates — Floating Point
// =============================================================================

inline constexpr uint32_t kFAddRRR = 0x1E602800; // fadd Dd, Dn, Dm
inline constexpr uint32_t kFSubRRR = 0x1E603800; // fsub Dd, Dn, Dm
inline constexpr uint32_t kFMulRRR = 0x1E600800; // fmul Dd, Dn, Dm
inline constexpr uint32_t kFDivRRR = 0x1E601800; // fdiv Dd, Dn, Dm
inline constexpr uint32_t kFCmpRR = 0x1E602000;  // fcmp Dn, Dm
inline constexpr uint32_t kFMovRR = 0x1E604000;  // fmov Dd, Dn
inline constexpr uint32_t kFRintN = 0x1E644000;  // frintn Dd, Dn

// =============================================================================
// Instruction Templates — Int↔Float Conversion
// =============================================================================

inline constexpr uint32_t kSCvtF = 0x9E620000;  // scvtf Dd, Xn
inline constexpr uint32_t kFCvtZS = 0x9E780000; // fcvtzs Xd, Dn
inline constexpr uint32_t kUCvtF = 0x9E630000;  // ucvtf Dd, Xn
inline constexpr uint32_t kFCvtZU = 0x9E790000; // fcvtzu Xd, Dn
inline constexpr uint32_t kFMovGR = 0x9E670000; // fmov Dd, Xn (GPR→FPR bit transfer)

// =============================================================================
// Instruction Templates — Branch
// =============================================================================

inline constexpr uint32_t kBr = 0x14000000;    // b   label
inline constexpr uint32_t kBl = 0x94000000;    // bl  label
inline constexpr uint32_t kBCond = 0x54000000; // b.cond label
inline constexpr uint32_t kCbz = 0xB4000000;   // cbz Xt, label
inline constexpr uint32_t kCbnz = 0xB5000000;  // cbnz Xt, label
inline constexpr uint32_t kTbz = 0x36000000;   // tbz Rt, #bit, label (b5 in bit 31)
inline constexpr uint32_t kTbnz = 0x37000000;  // tbnz Rt, #bit, label (b5 in bit 31)
inline constexpr uint32_t kAdr = 0x10000000;   // adr Xd, label (+/-1MB byte offset)
inline constexpr uint32_t kFCsel = 0x1E600C00; // fcsel Dd, Dn, Dm, cond
inline constexpr uint32_t kBrReg = 0xD61F0000; // br Xn (register-indirect branch)
inline constexpr uint32_t kLdrswRegLsl2 = 0xB8A07800; // ldrsw Xt, [Xn, Xm, lsl #2]
inline constexpr uint32_t kAddShifted = 0x8B000000;   // add Xd, Xn, Xm
inline constexpr uint32_t kBlr = 0xD63F0000;   // blr Xn
inline constexpr uint32_t kRet = 0xD65F03C0;   // ret x30

// --- Pointer Authentication & Branch Target Identification ---
// These are HINT instructions that execute as NOPs on hardware without PAC/BTI.
inline constexpr uint32_t kPaciasp = 0xD503233F; // HINT #25 — sign LR with SP
inline constexpr uint32_t kAutiasp = 0xD50323BF; // HINT #29 — verify LR with SP
inline constexpr uint32_t kBtiC = 0xD503245F;    // HINT #34 — valid indirect call target

// =============================================================================
// Mach-O Compact Unwind Encodings (Darwin only)
// =============================================================================
// Layout per Apple's compact_unwind_encoding.h: bits [27:24] carry the mode.
// MODE_FRAME additionally defines saved-register pair FLAGS in bits [8:0] and
// MODE_FRAMELESS a stack size in bits [23:12]; we emit neither because the
// Zanna prologue stores callee-saved pairs below the locals area (not at the
// canonical compact-unwind slots) and frameless functions always have a zero
// local frame.

inline constexpr uint32_t kUnwindArm64ModeFrameless = 0x02000000; // fp-less leaf, stack size 0
inline constexpr uint32_t kUnwindArm64ModeFrame = 0x04000000;     // fp/lr frame chain

// =============================================================================
// Instruction Templates — Address Materialization
// =============================================================================

inline constexpr uint32_t kAdrp = 0x90000000; // adrp Xd, label

// =============================================================================
// Encoding Helpers
// =============================================================================

/// @brief Build a 3-register instruction word.
/// @details Lays out the destination, first source, and second source operand
///          fields into a base template: `tmpl | (Rm << 16) | (Rn << 5) | Rd`.
///          Used for shift-register and data-processing-register encodings.
/// @param tmpl Base instruction template carrying opcode and option bits.
/// @param rd   5-bit destination register field.
/// @param rn   5-bit first source register field.
/// @param rm   5-bit second source register field.
/// @return Fully-encoded 32-bit instruction word.
constexpr uint32_t encode3Reg(uint32_t tmpl, uint32_t rd, uint32_t rn, uint32_t rm) {
    return tmpl | (rm << 16) | (rn << 5) | rd;
}

/// @brief Build a 4-register instruction word (MADD / MSUB / etc.).
/// @details Lays out the destination, two multiplicands, and the accumulator
///          into a base template: `tmpl | (Rm << 16) | (Ra << 10) | (Rn << 5) | Rd`.
/// @param tmpl Base instruction template (e.g. `kMAddRRRR`, `kMSubRRRR`).
/// @param rd   5-bit destination register field.
/// @param rn   5-bit multiplicand register field.
/// @param rm   5-bit multiplier register field.
/// @param ra   5-bit accumulator register field.
/// @return Fully-encoded 32-bit instruction word.
constexpr uint32_t encode4Reg(uint32_t tmpl, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra) {
    return tmpl | (rm << 16) | (ra << 10) | (rn << 5) | rd;
}

/// @brief Build an ADD/SUB-immediate instruction word.
/// @details Lays the 12-bit immediate into bits 10..21 of the template:
///          `tmpl | (imm12 << 10) | (Rn << 5) | Rd`. The template selects ADD
///          vs SUB and the sf bit (32/64).
/// @param tmpl  Base instruction template (e.g. `kAddRI`, `kSubRI`).
/// @param rd    5-bit destination register field.
/// @param rn    5-bit source register field.
/// @param imm12 Unsigned 12-bit immediate; values >0xFFF are rejected.
/// @return Fully-encoded 32-bit instruction word.
/// @throws std::out_of_range if @p imm12 exceeds 12 bits.
inline uint32_t encodeAddSubImm(uint32_t tmpl, uint32_t rd, uint32_t rn, uint32_t imm12) {
    if (imm12 > 0xFFF)
        throw std::out_of_range("encodeAddSubImm: immediate exceeds 12-bit range");
    return tmpl | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd;
}

/// @brief Build an ADD/SUB-immediate instruction word with `LSL #12` applied.
/// @details Sets bit 22 (`sh`) to enable the architectural left-shift-12 of the
///          12-bit immediate; the effective operand is `imm12 << 12`.
/// @param tmpl  Base instruction template (e.g. `kAddRI`, `kSubRI`).
/// @param rd    5-bit destination register field.
/// @param rn    5-bit source register field.
/// @param imm12 Unsigned 12-bit immediate; effective value is `imm12 * 4096`.
/// @return Fully-encoded 32-bit instruction word.
/// @throws std::out_of_range if @p imm12 exceeds 12 bits.
inline uint32_t encodeAddSubImmShift(uint32_t tmpl, uint32_t rd, uint32_t rn, uint32_t imm12) {
    if (imm12 > 0xFFF)
        throw std::out_of_range("encodeAddSubImmShift: immediate exceeds 12-bit range");
    return tmpl | (1U << 22) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd;
}

/// @brief Build a 2-register instruction word.
/// @details Lays the destination and source fields into the template:
///          `tmpl | (Rn << 5) | Rd`. Used by single-source forms such as
///          `MOV (register)`, `NEG`, `MVN`, conversion instructions.
/// @param tmpl Base instruction template carrying opcode and option bits.
/// @param rd   5-bit destination register field.
/// @param rn   5-bit source register field.
/// @return Fully-encoded 32-bit instruction word.
constexpr uint32_t encode2Reg(uint32_t tmpl, uint32_t rd, uint32_t rn) {
    return tmpl | (rn << 5) | rd;
}

// =============================================================================
// Logical Immediate Encoding
// =============================================================================

/**
 * @brief Encodes a 64-bit bitmask as `N:immr:imms`.
 *
 * The architectural parameter space is searched by element width, run length,
 * and rotation. Candidate elements are replicated to 64 bits and compared with
 * the requested value.
 *
 * @param val Logical-immediate bit pattern.
 * @return Packed 13-bit encoding, or `-1` when @p val is not encodable.
 */
inline int32_t encodeLogicalImmediate(uint64_t val) {
    if (val == 0 || val == ~0ULL)
        return -1; // All-zeros and all-ones are not encodable.

    /**
     * @brief Rotates an element right within a selected bit width.
     * @param bits Element bits before masking.
     * @param rot Rotation count.
     * @param width Element width from the architectural set.
     * @return Masked, rotated element.
     */
    auto ror = [](uint64_t bits, unsigned rot, unsigned width) -> uint64_t {
        const uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1);
        bits &= mask;
        rot %= width;
        if (rot == 0)
            return bits;
        return ((bits >> rot) | (bits << (width - rot))) & mask;
    };

    /**
     * @brief Replicates an element until it fills a 64-bit value.
     * @param elem Low-width element bits.
     * @param width Element width dividing 64.
     * @return Replicated 64-bit pattern.
     */
    auto replicate = [](uint64_t elem, unsigned width) -> uint64_t {
        if (width == 64)
            return elem;
        uint64_t out = 0;
        for (unsigned shift = 0; shift < 64; shift += width)
            out |= elem << shift;
        return out;
    };

    // Brute-force the architectural DecodeBitMasks parameter space. This is
    // compact, avoids all 64-bit shift edge cases, and guarantees the returned
    // N:immr:imms encoding decodes back to the requested value.
    for (unsigned width = 2; width <= 64; width <<= 1) {
        const unsigned levels = width - 1;
        const uint32_t N = (width == 64) ? 1u : 0u;
        const uint32_t immsPrefix = (width == 64) ? 0u : ((~levels) & 0x3Fu);

        for (unsigned runLen = 1; runLen < width; ++runLen) {
            const uint64_t ones = (runLen == 64) ? ~0ULL : ((1ULL << runLen) - 1);
            const uint32_t imms = immsPrefix | static_cast<uint32_t>(runLen - 1);
            for (unsigned immr = 0; immr < width; ++immr) {
                const uint64_t elem = ror(ones, immr, width);
                if (replicate(elem, width) == val)
                    return static_cast<int32_t>((N << 12) | (immr << 6) | imms);
            }
        }
    }
    return -1;
}

/// @brief Build an AND/ORR/EOR-immediate instruction word.
/// @details Lays the encoded `N:immr:imms` 13-bit bitmask immediate into bits
///          10..22 of the template: `tmpl | (nimms << 10) | (Rn << 5) | Rd`.
///          The caller must pre-compute @p nimms via @ref encodeLogicalImmediate.
/// @param tmpl  Base instruction template (`kAndImm`, `kOrrImm`, `kEorImm`).
/// @param rd    5-bit destination register field.
/// @param rn    5-bit source register field.
/// @param nimms 13-bit packed `N:immr:imms` immediate.
/// @return Fully-encoded 32-bit instruction word.
inline uint32_t encodeLogImm(uint32_t tmpl, uint32_t rd, uint32_t rn, int32_t nimms) {
    // nimms = N:immr:imms (13 bits)
    return tmpl | (static_cast<uint32_t>(nimms) << 10) | (rn << 5) | rd;
}

// =============================================================================
// FP8 Immediate Encoding
// =============================================================================

/// FMOV Dd, #imm8 instruction template.
inline constexpr uint32_t kFMovDImm = 0x1E601000;

/**
 * @brief Encodes a binary64 value in AArch64's 8-bit FP-immediate format.
 *
 * Encodable values have the form `±(1 + n/16) * 2^r`, where `n` is in
 * `[0, 15]` and `r` is in `[-3, 4]`.
 *
 * @param val Floating-point value to encode exactly.
 * @return Packed 8-bit immediate, or `-1` if the exponent or mantissa cannot
 *         be represented.
 */
inline int32_t encodeFP8Immediate(double val) {
    uint64_t bits;
    std::memcpy(&bits, &val, sizeof(bits));

    uint32_t sign = static_cast<uint32_t>(bits >> 63) & 1;
    int32_t exp = static_cast<int32_t>((bits >> 52) & 0x7FF) - 1023;
    uint64_t mantissa = bits & 0x000FFFFFFFFFFFFFULL;

    // Exponent must be in [-3, 4].
    if (exp < -3 || exp > 4)
        return -1;

    // Low 48 mantissa bits must be zero (only 4 fractional bits allowed).
    if (mantissa & 0x0000FFFFFFFFFFFFULL)
        return -1;

    uint32_t frac4 = static_cast<uint32_t>((mantissa >> 48) & 0xF);

    // Encode exponent: value = NOT(b)*4 + c*2 + d - 3, where bcd are the 3 exponent bits.
    int32_t e = exp + 3; // Maps [-3,4] → [0,7].
    uint32_t b, cd;
    if (e >= 4) {
        b = 0;
        cd = static_cast<uint32_t>(e - 4);
    } else {
        b = 1;
        cd = static_cast<uint32_t>(e);
    }

    return static_cast<int32_t>((sign << 7) | (b << 6) | (cd << 4) | frac4);
}

/// @brief Build a load/store-pair instruction word.
/// @details Lays the two transfer registers, base register, and scaled signed
///          7-bit immediate into the template:
///          `tmpl | ((imm7 & 0x7F) << 15) | (Rt2 << 10) | (Rn << 5) | Rt`.
///          The immediate is the architectural scaled value (already divided
///          by the access size, e.g. 8 bytes for GPR-pair forms).
/// @param tmpl Base template (`kStpGpr`, `kLdpGpr`, pre/post-index variants...).
/// @param rt   5-bit first transfer register field.
/// @param rt2  5-bit second transfer register field.
/// @param rn   5-bit base register field.
/// @param imm7 Signed 7-bit scaled offset (only low 7 bits are encoded).
/// @return Fully-encoded 32-bit instruction word.
constexpr uint32_t encodePair(uint32_t tmpl, uint32_t rt, uint32_t rt2, uint32_t rn, int32_t imm7) {
    return tmpl | ((static_cast<uint32_t>(imm7) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt;
}

} // namespace zanna::codegen::aarch64::binenc
