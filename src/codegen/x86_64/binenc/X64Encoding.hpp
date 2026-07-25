//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/binenc/X64Encoding.hpp
// Purpose: Maps PhysReg enum values to hardware register encoding (3-bit number
//          + REX extension bit) and provides condition-code-to-x86-CC-nibble
//          translation for the binary encoder.
// Key invariants:
//   - hwEncode() covers all 32 PhysReg values and rejects invalid enum values
//   - x86CC nibble values match Intel SDM Table B-1 (JCC/SETcc second opcode byte)
//   - SSE mandatory prefixes (F2/66) are emitted BEFORE REX, then 0F + opcode
// Ownership/Lifetime:
//   - Stateless; helpers are constexpr where invalid input cannot occur,
//     otherwise checked inline functions.
// Links: src/codegen/x86_64/TargetX64.hpp,
//        src/codegen/x86_64/binenc/X64BinaryEncoder.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/ICE.hpp"
#include "codegen/x86_64/TargetX64.hpp"

#include <cstdint>
#include <stdexcept>

/// @file
/// @brief Defines checked x86-64 register, prefix, opcode, and addressing encodings.

namespace zanna::codegen::x64::binenc {

/// @brief Hardware register encoding split across an instruction field and REX.
/// @details The low field occupies ModR/M or SIB bits and @c rexBit extends it
///          to address registers 8 through 15.
struct HwReg {
    uint8_t bits3;  ///< Low 3 bits of register number (0-7).
    uint8_t rexBit; ///< 1 if register requires REX.R/B/X extension, 0 otherwise.
};

/// @brief Maps a backend physical register to its x86-64 hardware encoding.
/// @details The @ref PhysReg order does not match the architectural numbering
///          of the general-purpose bank. A complete table bridges that gap and
///          also supplies the shared low-three-bit/extension representation for
///          XMM registers.
/// @param reg Physical register to encode.
/// @return Three-bit instruction field and corresponding REX extension bit.
/// @throws std::out_of_range If @p reg is outside the declared enumeration.
inline HwReg hwEncode(PhysReg reg) {
    // Index: static_cast<int>(PhysReg)
    // Layout: {bits3, rexBit}
    constexpr HwReg kTable[] = {
        {0, 0}, // RAX  = 0
        {3, 0}, // RBX  = 1
        {1, 0}, // RCX  = 2
        {2, 0}, // RDX  = 3
        {6, 0}, // RSI  = 4
        {7, 0}, // RDI  = 5
        {0, 1}, // R8   = 6
        {1, 1}, // R9   = 7
        {2, 1}, // R10  = 8
        {3, 1}, // R11  = 9
        {4, 1}, // R12  = 10
        {5, 1}, // R13  = 11
        {6, 1}, // R14  = 12
        {7, 1}, // R15  = 13
        {5, 0}, // RBP  = 14
        {4, 0}, // RSP  = 15
        {0, 0}, // XMM0  = 16
        {1, 0}, // XMM1  = 17
        {2, 0}, // XMM2  = 18
        {3, 0}, // XMM3  = 19
        {4, 0}, // XMM4  = 20
        {5, 0}, // XMM5  = 21
        {6, 0}, // XMM6  = 22
        {7, 0}, // XMM7  = 23
        {0, 1}, // XMM8  = 24
        {1, 1}, // XMM9  = 25
        {2, 1}, // XMM10 = 26
        {3, 1}, // XMM11 = 27
        {4, 1}, // XMM12 = 28
        {5, 1}, // XMM13 = 29
        {6, 1}, // XMM14 = 30
        {7, 1}, // XMM15 = 31
    };
    const auto idx = static_cast<int>(reg);
    if (idx < 0 || idx >= static_cast<int>(sizeof(kTable) / sizeof(kTable[0])))
        throw std::out_of_range("x86-64 binary encoder: invalid physical register");
    return kTable[idx];
}

/// @brief Maps a Zanna condition code to the architectural x86 condition nibble.
/// @details The result occupies the low nibble of @c JCC and @c SETcc opcode
///          bytes. The mapping follows the Intel condition-code encoding:
///   Zanna 0 (eq)  -> x86 4 (E)     Zanna 7 (ae) -> x86 3 (AE/NB)
///   Zanna 1 (ne)  -> x86 5 (NE)    Zanna 8 (b)  -> x86 2 (B/C)
///   Zanna 2 (lt)  -> x86 C (L)     Zanna 9 (be) -> x86 6 (BE/NA)
///   Zanna 3 (le)  -> x86 E (LE)    Zanna 10 (p) -> x86 A (P/PE)
///   Zanna 4 (gt)  -> x86 F (G)     Zanna 11 (np)-> x86 B (NP/PO)
///   Zanna 5 (ge)  -> x86 D (GE)    Zanna 12 (o) -> x86 0 (O)
///   Zanna 6 (a)   -> x86 7 (A/NBE) Zanna 13 (no)-> x86 1 (NO)
/// @param zannaCC Backend condition code in the inclusive range 0 through 13.
/// @return Architectural four-bit condition selector.
/// @throws std::out_of_range If @p zannaCC is outside the supported range.
inline uint8_t x86CC(int zannaCC) {
    constexpr uint8_t kTable[] = {
        0x4, // 0: eq  -> E
        0x5, // 1: ne  -> NE
        0xC, // 2: lt  -> L
        0xE, // 3: le  -> LE
        0xF, // 4: gt  -> G
        0xD, // 5: ge  -> GE
        0x7, // 6: a   -> A
        0x3, // 7: ae  -> AE
        0x2, // 8: b   -> B
        0x6, // 9: be  -> BE
        0xA, // 10: p  -> P
        0xB, // 11: np -> NP
        0x0, // 12: o  -> O
        0x1, // 13: no -> NO
    };
    if (zannaCC < 0 || zannaCC > 13)
        throw std::out_of_range("x86-64 binary encoder: invalid condition code");
    return kTable[zannaCC];
}

// === ModR/M + SIB construction helpers ===

/// @brief Builds a ModR/M byte from its three logical fields.
/// @param mod Two-bit addressing-mode selector.
/// @param reg3 Register number or opcode extension; only the low three bits are used.
/// @param rm3 Register or addressing selector; only the low three bits are used.
/// @return Packed byte in @c mod:reg:r/m bit order.
/// @pre @p mod is in the inclusive range 0 through 3.
inline constexpr uint8_t makeModRM(uint8_t mod, uint8_t reg3, uint8_t rm3) {
    return static_cast<uint8_t>((mod << 6) | ((reg3 & 7) << 3) | (rm3 & 7));
}

/// @brief Builds a scale-index-base byte from its three logical fields.
/// @param scale Two-bit logarithmic scale encoding.
/// @param index3 Index-register field; only the low three bits are used.
/// @param base3 Base-register field; only the low three bits are used.
/// @return Packed byte in @c scale:index:base bit order.
/// @pre @p scale is in the inclusive range 0 through 3.
inline constexpr uint8_t makeSIB(uint8_t scale, uint8_t index3, uint8_t base3) {
    return static_cast<uint8_t>((scale << 6) | ((index3 & 7) << 3) | (base3 & 7));
}

/// @brief Computes the SIB field for an architectural integer scale factor.
/// @param scale Effective-address multiplier, which must be 1, 2, 4, or 8.
/// @return Logarithmic SIB encoding: 1→0, 2→1, 4→2, or 8→3.
/// @throws std::invalid_argument If @p scale is not architecturally encodable.
inline uint8_t scaleLog2(uint8_t scale) {
    switch (scale) {
        case 1:
            return 0;
        case 2:
            return 1;
        case 4:
            return 2;
        case 8:
            return 3;
        default:
            throw std::invalid_argument("x86-64 binary encoder: invalid SIB scale factor");
    }
}

/// @brief Constructs a REX prefix byte from its W, R, X, and B flags.
/// @param w Whether to select 64-bit operand width.
/// @param r Extension bit for the ModR/M register field.
/// @param x Extension bit for the SIB index field.
/// @param b Extension bit for the ModR/M r/m or SIB base field.
/// @return Byte @c 0x40|(W<<3)|(R<<2)|(X<<1)|B.
/// @note With all flags false this returns the valid bare REX byte @c 0x40;
///       use @ref needsRex to decide whether a prefix is required.
inline constexpr uint8_t computeRex(bool w, bool r, bool x, bool b) {
    uint8_t rex = 0x40;
    if (w)
        rex |= 0x08;
    if (r)
        rex |= 0x04;
    if (x)
        rex |= 0x02;
    if (b)
        rex |= 0x01;
    return rex;
}

/// @brief Tests whether any requested flag requires emission of a REX prefix.
/// @param w Requested REX.W flag.
/// @param r Requested REX.R flag.
/// @param x Requested REX.X flag.
/// @param b Requested REX.B flag.
/// @return @c true when at least one argument is true.
/// @note Byte-register encodings may require a bare REX prefix even when this
///       helper returns false; those call sites handle that rule explicitly.
inline constexpr bool needsRex(bool w, bool r, bool x, bool b) {
    return w || r || x || b;
}

// === RegReg ALU opcode table ===

/// @brief Describes opcode bytes and operand direction for a reg-reg instruction.
struct RegRegOp {
    uint8_t primary;   ///< Primary opcode byte (or 0x0F escape prefix).
    uint8_t secondary; ///< Secondary byte after 0F (0 if single-byte opcode).
    bool regIsDst;     ///< True if ModR/M reg field is the destination.
};

/// @brief Looks up the encoding descriptor for a register-register GPR opcode.
/// @param op Supported register-register MIR opcode.
/// @return Primary byte, optional secondary byte, and ModR/M operand direction.
/// @pre @p op is one of the register-register opcodes handled by this table.
/// @note Violating the precondition reports an internal compiler error and aborts.
inline RegRegOp regRegOpcode(MOpcode op) {
    switch (op) {
        case MOpcode::MOVrr:
            return {0x89, 0, false}; // reg=src, r/m=dst
        case MOpcode::ADDrr:
            return {0x01, 0, false}; // reg=src, r/m=dst
        case MOpcode::SUBrr:
            return {0x29, 0, false};
        case MOpcode::ANDrr:
            return {0x21, 0, false};
        case MOpcode::ORrr:
            return {0x09, 0, false};
        case MOpcode::XORrr:
            return {0x31, 0, false};
        case MOpcode::CMPrr:
            return {0x39, 0, false};
        case MOpcode::TESTrr:
            return {0x85, 0, false};
        case MOpcode::IMULrr:
            return {0x0F, 0xAF, true}; // reg=dst, r/m=src
        case MOpcode::CMOVNErr:
            return {0x0F, 0x45, true}; // reg=dst, r/m=src
        case MOpcode::XORrr32:
            return {0x31, 0, false};
        case MOpcode::MOVZXrr32:
            return {0x89, 0, false};
        case MOpcode::ADDrr32:
            return {0x01, 0, false};
        case MOpcode::SUBrr32:
            return {0x29, 0, false};
        case MOpcode::CMPrr32:
            return {0x39, 0, false};
        case MOpcode::IMULrr32:
            return {0x0F, 0xAF, true}; // reg=dst, r/m=src
        case MOpcode::MOVSXD:
            return {0x63, 0, true}; // movslq: reg=dst, r/m=src (keeps REX.W)
        case MOpcode::ADDrr16:
            return {0x01, 0, false}; // with 0x66 operand-size prefix
        case MOpcode::SUBrr16:
            return {0x29, 0, false}; // with 0x66 operand-size prefix
        case MOpcode::IMULrr16:
            return {0x0F, 0xAF, true}; // with 0x66 operand-size prefix
        case MOpcode::MOVSXrr16:
            return {0x0F, 0xBF, true}; // movswq: reg=dst, r/m=src (keeps REX.W, no 0x66)
        default:
            ZANNA_ICE("not a reg-reg GPR opcode");
    }
}

/// @brief Looks up the ModR/M opcode extension for an immediate ALU form.
/// @param op Supported register-immediate ALU opcode.
/// @return Three-bit extension used with opcode @c 0x81 or @c 0x83.
/// @pre @p op is handled by the register-immediate encoder.
/// @note Violating the precondition reports an internal compiler error and aborts.
inline uint8_t regImmExt(MOpcode op) {
    switch (op) {
        case MOpcode::ADDri:
        case MOpcode::ADDri32:
        case MOpcode::ADDri16:
            return 0; // /0
        case MOpcode::ORri:
            return 1; // /1
        case MOpcode::ANDri:
            return 4; // /4
        case MOpcode::XORri:
            return 6; // /6
        case MOpcode::CMPri:
            return 7; // /7
        default:
            ZANNA_ICE("not a reg-imm ALU opcode");
    }
}

/// @brief Looks up the ModR/M opcode extension for a shift form.
/// @param op Supported immediate-count or @c CL-count shift opcode.
/// @return Three-bit extension used with opcode @c 0xc1 or @c 0xd3.
/// @pre @p op is handled by the shift encoder.
/// @note Violating the precondition reports an internal compiler error and aborts.
inline uint8_t shiftExt(MOpcode op) {
    switch (op) {
        case MOpcode::SHLri:
        case MOpcode::SHLrc:
            return 4; // /4
        case MOpcode::SHRri:
        case MOpcode::SHRrc:
            return 5; // /5
        case MOpcode::SARri:
        case MOpcode::SARrc:
            return 7; // /7
        default:
            ZANNA_ICE("not a shift opcode");
    }
}

/// @brief Describes prefixes, opcode, direction, and width for an SSE operation.
struct SseOp {
    uint8_t prefix; ///< Mandatory prefix: 0xF2, 0x66, or 0 (none for MOVUPS).
    uint8_t opcode; ///< Opcode byte after 0F.
    bool regIsDst;  ///< True if ModR/M reg field is the destination.
    bool needsRexW; ///< True if REX.W is needed (CVTSI2SD, CVTTSD2SI, MOVQrx, MOVQxr).
};

/// @brief Return the SSE2 encoding descriptor for a scalar-double MIR opcode.
/// @details Maps arithmetic, unordered comparison, integer/double conversions,
///          bitwise register moves, and scalar or unaligned memory moves to
///          their prefix, post-@c 0x0f opcode, operand-direction, and REX.W tuple.
/// @param op Supported SSE MIR opcode.
/// @return Complete table descriptor consumed by the SSE encoders.
/// @pre @p op has an SSE encoding in this table.
/// @note Violating the precondition reports an internal compiler error and aborts.
inline SseOp sseOpcode(MOpcode op) {
    switch (op) {
        case MOpcode::FADD:
            return {0xF2, 0x58, true, false};
        case MOpcode::FSUB:
            return {0xF2, 0x5C, true, false};
        case MOpcode::FMUL:
            return {0xF2, 0x59, true, false};
        case MOpcode::FDIV:
            return {0xF2, 0x5E, true, false};
        case MOpcode::UCOMIS:
            return {0x66, 0x2E, true, false};
        case MOpcode::CVTSI2SD:
            return {0xF2, 0x2A, true, true};
        case MOpcode::CVTTSD2SI:
            return {0xF2, 0x2C, true, true};
        case MOpcode::MOVQrx:
            return {0x66, 0x6E, true, true};
        case MOpcode::MOVQxr:
            return {0x66, 0x7E, false, true};
        case MOpcode::MOVSDrr:
            return {0xF2, 0x10, true, false};
        case MOpcode::MOVSDrm:
            return {0xF2, 0x11, false, false}; // store: reg=src
        case MOpcode::MOVSDmr:
            return {0xF2, 0x10, true, false}; // load: reg=dst
        case MOpcode::MOVUPSrm:
            return {0x00, 0x11, false, false}; // store: no prefix
        case MOpcode::MOVUPSmr:
            return {0x00, 0x10, true, false}; // load: no prefix
        default:
            ZANNA_ICE("not an SSE opcode");
    }
}

} // namespace zanna::codegen::x64::binenc
