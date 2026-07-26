//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: codegen/aarch64/MOpcodeDef.inc
// Purpose: X-macro definition file for AArch64 Machine IR opcodes. This is the
//          single source of truth for the MOpcode enum and name table. Include
//          this file with ZANNA_MIR_OPCODE(name) defined to generate:
//            - The enum body (in MachineIR.hpp)
//            - The opcodeName switch cases (in MachineIR.cpp)
//            - Any future metadata tables
//
// Key invariants:
//   - Order must match the historical enum order for ABI stability.
//   - Every opcode here must have an encoding handler or be a pseudo-op.
//   - New opcodes are added ONLY here; enum and name table are auto-generated.
//
// Ownership/Lifetime: Stateless macro file, included multiple times.
// Links: codegen/aarch64/MachineIR.hpp, plans/audit-02-mir-opcode-generation.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Enumerates AArch64 Machine IR opcodes through a reusable X-macro.
/// @details Consumers define @c ZANNA_MIR_OPCODE before inclusion to generate
///          the enum, mnemonic lookup, or another order-sensitive metadata
///          structure from this single ABI-stable list.
/// @def ZANNA_MIR_OPCODE
/// @param name Machine opcode identifier emitted by the including consumer.

#ifndef ZANNA_MIR_OPCODE
#error "Define ZANNA_MIR_OPCODE(name) before including MOpcodeDef.inc"
#endif

// --- GPR move ---
ZANNA_MIR_OPCODE(MovRR)
ZANNA_MIR_OPCODE(MovRI)
// --- Floating-point (64-bit, scalar) ---
ZANNA_MIR_OPCODE(FMovRR)
ZANNA_MIR_OPCODE(FMovRI)
ZANNA_MIR_OPCODE(FMovGR)
ZANNA_MIR_OPCODE(FAddRRR)
ZANNA_MIR_OPCODE(FSubRRR)
ZANNA_MIR_OPCODE(FMulRRR)
ZANNA_MIR_OPCODE(FDivRRR)
ZANNA_MIR_OPCODE(FCmpRR)
// --- Integer<->Float conversions (64-bit) ---
ZANNA_MIR_OPCODE(SCvtF)
ZANNA_MIR_OPCODE(FCvtZS)
ZANNA_MIR_OPCODE(UCvtF)
ZANNA_MIR_OPCODE(FCvtZU)
// --- Floating-point rounding ---
ZANNA_MIR_OPCODE(FRintN)
// --- Stack pointer adjust ---
ZANNA_MIR_OPCODE(SubSpImm)
ZANNA_MIR_OPCODE(AddSpImm)
// --- Store to outgoing arg area ---
ZANNA_MIR_OPCODE(StrRegSpImm)
ZANNA_MIR_OPCODE(StrFprSpImm)
// --- Load/store from frame pointer ---
ZANNA_MIR_OPCODE(LdrRegFpImm)
ZANNA_MIR_OPCODE(StrRegFpImm)
ZANNA_MIR_OPCODE(Ldr8RegFpImm)
ZANNA_MIR_OPCODE(Str8RegFpImm)
ZANNA_MIR_OPCODE(Ldr16RegFpImm)
ZANNA_MIR_OPCODE(Str16RegFpImm)
ZANNA_MIR_OPCODE(Ldr32RegFpImm)
ZANNA_MIR_OPCODE(Str32RegFpImm)
ZANNA_MIR_OPCODE(LdrFprFpImm)
ZANNA_MIR_OPCODE(StrFprFpImm)
// --- Phi-edge copies ---
ZANNA_MIR_OPCODE(PhiStoreGPR)
ZANNA_MIR_OPCODE(PhiStoreFPR)
ZANNA_MIR_OPCODE(AddFpImm)
// --- Load/store from arbitrary base register ---
ZANNA_MIR_OPCODE(LdrRegBaseImm)
ZANNA_MIR_OPCODE(StrRegBaseImm)
ZANNA_MIR_OPCODE(Ldr8RegBaseImm)
ZANNA_MIR_OPCODE(Str8RegBaseImm)
ZANNA_MIR_OPCODE(Ldr16RegBaseImm)
ZANNA_MIR_OPCODE(Str16RegBaseImm)
ZANNA_MIR_OPCODE(Ldr32RegBaseImm)
ZANNA_MIR_OPCODE(Str32RegBaseImm)
ZANNA_MIR_OPCODE(LdrFprBaseImm)
ZANNA_MIR_OPCODE(StrFprBaseImm)
// --- Load/store with scaled register offset: [base, index, lsl #k] ---
ZANNA_MIR_OPCODE(LdrRegBaseRegLsl)
ZANNA_MIR_OPCODE(StrRegBaseRegLsl)
ZANNA_MIR_OPCODE(Ldr32RegBaseRegLsl)
ZANNA_MIR_OPCODE(Str32RegBaseRegLsl)
ZANNA_MIR_OPCODE(LdrFprBaseRegLsl)
ZANNA_MIR_OPCODE(StrFprBaseRegLsl)
// --- Integer arithmetic ---
ZANNA_MIR_OPCODE(AddRRR)
ZANNA_MIR_OPCODE(SubRRR)
ZANNA_MIR_OPCODE(MulRRR)
ZANNA_MIR_OPCODE(SmulhRRR)
ZANNA_MIR_OPCODE(UmulhRRR)
ZANNA_MIR_OPCODE(SDivRRR)
ZANNA_MIR_OPCODE(UDivRRR)
ZANNA_MIR_OPCODE(MSubRRRR)
// --- Compare and branch ---
ZANNA_MIR_OPCODE(Cbz)
// --- Bitwise ---
ZANNA_MIR_OPCODE(AndRRR)
ZANNA_MIR_OPCODE(OrrRRR)
ZANNA_MIR_OPCODE(EorRRR)
// --- Shifted-operand ALU: dst = a op (b << k) ---
ZANNA_MIR_OPCODE(AddRRRLsl)
ZANNA_MIR_OPCODE(SubRRRLsl)
ZANNA_MIR_OPCODE(AndRRRLsl)
ZANNA_MIR_OPCODE(OrrRRRLsl)
ZANNA_MIR_OPCODE(EorRRRLsl)
ZANNA_MIR_OPCODE(AndRI)
ZANNA_MIR_OPCODE(OrrRI)
ZANNA_MIR_OPCODE(EorRI)
// --- Immediate arithmetic ---
ZANNA_MIR_OPCODE(AddRI)
ZANNA_MIR_OPCODE(SubRI)
// --- Shifts ---
ZANNA_MIR_OPCODE(LslRI)
ZANNA_MIR_OPCODE(LsrRI)
ZANNA_MIR_OPCODE(AsrRI)
ZANNA_MIR_OPCODE(LslvRRR)
ZANNA_MIR_OPCODE(LsrvRRR)
ZANNA_MIR_OPCODE(AsrvRRR)
// --- Compare ---
ZANNA_MIR_OPCODE(CmpRR)
ZANNA_MIR_OPCODE(CmpRI)
ZANNA_MIR_OPCODE(TstRR)
ZANNA_MIR_OPCODE(Cset)
// --- Control flow ---
ZANNA_MIR_OPCODE(Br)
ZANNA_MIR_OPCODE(BCond)
ZANNA_MIR_OPCODE(Bl)
ZANNA_MIR_OPCODE(Blr)
ZANNA_MIR_OPCODE(Ret)
// --- Address materialisation ---
ZANNA_MIR_OPCODE(AdrPage)
ZANNA_MIR_OPCODE(AddPageOff)
// --- Peephole optimization opcodes ---
ZANNA_MIR_OPCODE(Cbnz)
ZANNA_MIR_OPCODE(MAddRRRR)
ZANNA_MIR_OPCODE(Csel)
ZANNA_MIR_OPCODE(FCsel)
ZANNA_MIR_OPCODE(LdpRegFpImm)
ZANNA_MIR_OPCODE(StpRegFpImm)
ZANNA_MIR_OPCODE(LdpFprFpImm)
ZANNA_MIR_OPCODE(StpFprFpImm)
// --- Flag-setting arithmetic ---
ZANNA_MIR_OPCODE(AddsRRR)
ZANNA_MIR_OPCODE(SubsRRR)
ZANNA_MIR_OPCODE(AddsRI)
ZANNA_MIR_OPCODE(SubsRI)
// --- Overflow-checked pseudo-opcodes ---
ZANNA_MIR_OPCODE(AddOvfRRR)
ZANNA_MIR_OPCODE(SubOvfRRR)
ZANNA_MIR_OPCODE(AddOvfRI)
ZANNA_MIR_OPCODE(SubOvfRI)
ZANNA_MIR_OPCODE(MulOvfRRR)
// --- Test-bit-and-branch (peephole-created): tbz/tbnz reg, #bit, label ---
// Operands: [0]=reg (use), [1]=label, [2]=bit immediate — label kept at
// index 1 so every Cbz/Cbnz label-position assumption applies unchanged.
ZANNA_MIR_OPCODE(Tbz)
ZANNA_MIR_OPCODE(Tbnz)
// --- Jump-table dispatch terminator: adr/ldrsw/add/br + inline table ---
// Operands: [0]=index (use), [1]=address temp (def), [2]=entry temp (def),
// [3]=table name label (text-asm only), [4..]=case labels.
ZANNA_MIR_OPCODE(JumpTable)

#undef ZANNA_MIR_OPCODE
