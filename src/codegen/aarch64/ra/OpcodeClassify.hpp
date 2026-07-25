//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/ra/OpcodeClassify.hpp
// Purpose: MOpcode classification predicates used by the AArch64 register
//          allocator to determine operand roles and instruction categories.
//
// Key invariants:
//   - Each predicate is a pure function of MOpcode with no side effects.
//   - Classification must stay in sync with MachineIR.hpp opcode definitions.
//
// Ownership/Lifetime:
//   - Stateless free functions; no ownership.
//
// Links: codegen/aarch64/ra/Allocator.cpp,
//        codegen/aarch64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/MachineIR.hpp"

/// @file
/// @brief Defines lightweight opcode-family predicates for register allocation.
///
/// These helpers describe operand shapes used by the allocator; they are not a
/// complete semantic taxonomy of every AArch64 machine instruction.

namespace zanna::codegen::aarch64::ra {

/// @brief Test for an integer three-register operation shaped `dst = src1 op src2`.
/// @param opc Machine opcode to classify.
/// @return `true` for the listed operations whose operand zero is def-only and
///         operands one and two are uses.
inline bool isThreeAddrRRR(MOpcode opc) {
    switch (opc) {
        case MOpcode::AddRRR:
        case MOpcode::SubRRR:
        case MOpcode::MulRRR:
        case MOpcode::SmulhRRR:
        case MOpcode::UmulhRRR:
        case MOpcode::SDivRRR:
        case MOpcode::UDivRRR:
        case MOpcode::AndRRR:
        case MOpcode::OrrRRR:
        case MOpcode::EorRRR:
        case MOpcode::LslvRRR:
        case MOpcode::LsrvRRR:
        case MOpcode::AsrvRRR:
        case MOpcode::AddsRRR:
        case MOpcode::SubsRRR:
        case MOpcode::AddOvfRRR:
        case MOpcode::SubOvfRRR:
        case MOpcode::MulOvfRRR:
            return true;
        default:
            return false;
    }
}

/// @brief Test for an integer register/immediate operation shaped `dst = src op imm`.
/// @param opc Machine opcode to classify.
/// @return `true` when operand zero is a definition, operand one is a use, and
///         operand two is an immediate.
inline bool isUseDefImmLike(MOpcode opc) {
    switch (opc) {
        case MOpcode::AddRI:
        case MOpcode::SubRI:
        case MOpcode::LslRI:
        case MOpcode::LsrRI:
        case MOpcode::AsrRI:
        case MOpcode::AndRI:
        case MOpcode::OrrRI:
        case MOpcode::EorRI:
        case MOpcode::AddsRI:
        case MOpcode::SubsRI:
        case MOpcode::AddOvfRI:
        case MOpcode::SubOvfRI:
            return true;
        default:
            return false;
    }
}

/// @brief Test whether an opcode adjusts the implicit stack pointer.
/// @param opc Machine opcode to classify.
/// @return `true` for `SubSpImm` or `AddSpImm`.
inline bool isSpAdj(MOpcode opc) {
    return opc == MOpcode::SubSpImm || opc == MOpcode::AddSpImm;
}

/// @brief Test for the operand-free direct branch forms handled as a unit.
/// @param opc Machine opcode to classify.
/// @return `true` for `Br` or `BCond`.
/// @note Register-consuming conditional branches are excluded so their operands
///       still pass through allocation.
inline bool isBranch(MOpcode opc) {
    return opc == MOpcode::Br || opc == MOpcode::BCond;
}

/// @brief Test whether an opcode is a direct or indirect function call.
/// @param opc Machine opcode to classify.
/// @return `true` for `Bl` or `Blr`.
inline bool isCall(MOpcode opc) {
    return opc == MOpcode::Bl || opc == MOpcode::Blr;
}

/// @brief Test for a register-register integer comparison.
/// @param opc Machine opcode to classify.
/// @return `true` for `CmpRR`.
inline bool isCmpRR(MOpcode opc) {
    return opc == MOpcode::CmpRR;
}

/// @brief Test for a register-immediate integer comparison.
/// @param opc Machine opcode to classify.
/// @return `true` for `CmpRI`.
inline bool isCmpRI(MOpcode opc) {
    return opc == MOpcode::CmpRI;
}

/// @brief Test for condition-code materialization into a register.
/// @param opc Machine opcode to classify.
/// @return `true` for `Cset`.
inline bool isCset(MOpcode opc) {
    return opc == MOpcode::Cset;
}

/// @brief Test for a function return.
/// @param opc Machine opcode to classify.
/// @return `true` for `Ret`.
inline bool isRet(MOpcode opc) {
    return opc == MOpcode::Ret;
}

/// @brief Test whether an opcode terminates AArch64 MIR control flow.
/// @param opc Machine opcode to classify.
/// @return `true` for direct and register-test branches, jump tables, or returns.
inline bool isTerminator(MOpcode opc) {
    return isBranch(opc) || opc == MOpcode::Cbz || opc == MOpcode::Cbnz || opc == MOpcode::Tbz ||
           opc == MOpcode::Tbnz || opc == MOpcode::JumpTable || isRet(opc);
}

/// @brief Test for a scalar memory-load form whose registers need allocation.
/// @param opc Machine opcode to classify.
/// @return `true` for the listed GPR/FPR FP-relative, base-immediate, and
///         base-plus-scaled-index loads.
/// @note Paired loads are classified separately by allocator operand-role logic.
inline bool isMemLd(MOpcode opc) {
    return opc == MOpcode::LdrRegFpImm || opc == MOpcode::LdrRegBaseImm ||
           opc == MOpcode::Ldr8RegFpImm || opc == MOpcode::Ldr8RegBaseImm ||
           opc == MOpcode::Ldr16RegFpImm || opc == MOpcode::Ldr16RegBaseImm ||
           opc == MOpcode::Ldr32RegFpImm || opc == MOpcode::Ldr32RegBaseImm ||
           opc == MOpcode::LdrFprFpImm || opc == MOpcode::LdrFprBaseImm ||
           opc == MOpcode::LdrRegBaseRegLsl || opc == MOpcode::Ldr32RegBaseRegLsl ||
           opc == MOpcode::LdrFprBaseRegLsl;
}

/// @brief Test for a scalar memory-store form whose registers need allocation.
/// @param opc Machine opcode to classify.
/// @return `true` for the listed GPR/FPR FP-relative, SP-relative,
///         base-immediate, and base-plus-scaled-index stores.
/// @note Paired stores are classified separately by allocator operand-role logic.
inline bool isMemSt(MOpcode opc) {
    return opc == MOpcode::StrRegFpImm || opc == MOpcode::StrRegBaseImm ||
           opc == MOpcode::Str8RegFpImm || opc == MOpcode::Str8RegBaseImm ||
           opc == MOpcode::Str16RegFpImm || opc == MOpcode::Str16RegBaseImm ||
           opc == MOpcode::Str32RegFpImm || opc == MOpcode::Str32RegBaseImm ||
           opc == MOpcode::StrRegSpImm || opc == MOpcode::StrFprFpImm ||
           opc == MOpcode::StrFprBaseImm || opc == MOpcode::StrFprSpImm ||
           opc == MOpcode::StrRegBaseRegLsl || opc == MOpcode::Str32RegBaseRegLsl ||
           opc == MOpcode::StrFprBaseRegLsl;
}

} // namespace zanna::codegen::aarch64::ra
