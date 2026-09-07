//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/PeepholeCommon.cpp
// Purpose: Out-of-line implementations of the AArch64 peephole dataflow
//          classifiers (definesReg, usesReg, classifyOperand, getDefinedReg,
//          hasSideEffects, updateKnownConsts).
// Key invariants:
//   - Explicit operand roles are derived from ra::operandRoles, the backend's
//     single use/def table. This file never re-lists operand positions per
//     opcode; adding an opcode means classifying it once in
//     ra/OperandRoles.cpp.
//   - Implicit effects (call clobbers, emit-time scratch writes) come from
//     InstrEffects so constant tracking and DCE agree with the scheduler and
//     the CFG-aware DCE.
//   - hasSideEffects is a DCE *policy* (what local DCE must keep), deliberately
//     broader than architectural side effects.
//
// Ownership/Lifetime:
//   - Free functions; no state.
//
// Links: codegen/aarch64/peephole/PeepholeCommon.hpp,
//        codegen/aarch64/ra/OperandRoles.hpp,
//        codegen/aarch64/InstrEffects.hpp
//
//===----------------------------------------------------------------------===//

#include "PeepholeCommon.hpp"

#include "codegen/aarch64/InstrEffects.hpp"
#include "codegen/aarch64/ra/OperandRoles.hpp"

#include <exception>

/// @file
/// @brief Implements AArch64 peephole dataflow classifiers on the shared role table.

namespace zanna::codegen::aarch64::peephole {

namespace {

/// @brief Role of operand @p idx of @p instr, never throwing.
/// @details ra::operandRoles throws for a register operand it cannot classify
///          (a backend bug the MIR verifier reports). The peephole helpers are
///          `noexcept` policy queries, so an unclassified operand is treated as
///          neither use nor def here; non-register operands are never roles.
[[nodiscard]] std::pair<bool, bool> safeRoles(const MInstr &instr, std::size_t idx) noexcept {
    if (idx >= instr.ops.size() || instr.ops[idx].kind != MOperand::Kind::Reg)
        return {false, false};
    try {
        return ra::operandRoles(instr, idx);
    } catch (const std::exception &) {
        return {false, false};
    }
}

} // namespace

/// @copydoc definesReg
bool definesReg(const MInstr &instr, const MOperand &reg) noexcept {
    if (!isPhysReg(reg))
        return false;
    for (std::size_t idx = 0; idx < instr.ops.size(); ++idx) {
        if (safeRoles(instr, idx).second && samePhysReg(instr.ops[idx], reg))
            return true;
    }
    return false;
}

/// @copydoc usesReg
bool usesReg(const MInstr &instr, const MOperand &reg) noexcept {
    if (!isPhysReg(reg))
        return false;
    for (std::size_t idx = 0; idx < instr.ops.size(); ++idx) {
        if (safeRoles(instr, idx).first && samePhysReg(instr.ops[idx], reg))
            return true;
    }
    return false;
}

/// @copydoc classifyOperand
std::pair<bool, bool> classifyOperand(const MInstr &instr, std::size_t idx) noexcept {
    return safeRoles(instr, idx);
}

/// @copydoc updateKnownConsts
void updateKnownConsts(const MInstr &instr, RegConstMap &knownConsts) {
    if (instr.opc == MOpcode::MovRI && instr.ops.size() == 2 && isPhysReg(instr.ops[0]) &&
        instr.ops[1].kind == MOperand::Kind::Imm) {
        knownConsts[instr.ops[0].reg.idOrPhys] = instr.ops[1].imm;
        return;
    }

    if (knownConsts.empty())
        return;

    // ZB-29: every register the instruction defines loses its constant fact.
    // The invalidation is derived from the canonical definesReg classification
    // instead of a parallel opcode allowlist — the allowlist had drifted and
    // omitted the flag-setting/overflow forms (adds/subs/…ovf), so a
    // `mov x5, #2 … adds x5, x28, #1 … sdiv x, y, x5` chain was strength-
    // reduced to a divide by 2 on the native -O1 build.
    for (auto it = knownConsts.begin(); it != knownConsts.end();) {
        const MOperand reg = MOperand::regOp(static_cast<PhysReg>(it->first));
        if (definesReg(instr, reg))
            it = knownConsts.erase(it);
        else
            ++it;
    }

    switch (instr.opc) {
        case MOpcode::JumpTable:
            // Clobbers the reserved X16/X17 scratch registers.
            knownConsts.erase(static_cast<uint16_t>(kScratchGPR2));
            knownConsts.erase(static_cast<uint16_t>(kScratchGPR3));
            break;
        default:
            break;
    }

    // A wide immediate or large offset is materialised through a reserved
    // scratch GPR at emit time; a constant tracked there is stale afterwards.
    if (emitTimeScratchClobber(instr)) {
        knownConsts.erase(static_cast<uint16_t>(kScratchGPR));
        knownConsts.erase(static_cast<uint16_t>(kScratchGPR2));
        knownConsts.erase(static_cast<uint16_t>(kScratchGPR3));
    }

    if (instr.opc == MOpcode::Bl || instr.opc == MOpcode::Blr) {
        for (uint16_t i = 0; i <= 18; ++i)
            knownConsts.erase(i);
    }
}

/// @copydoc hasSideEffects
bool hasSideEffects(const MInstr &instr) noexcept {
    switch (instr.opc) {
        case MOpcode::StrRegBaseRegLsl:
        case MOpcode::Str32RegBaseRegLsl:
        case MOpcode::StrFprBaseRegLsl:
        case MOpcode::StrRegFpImm:
        case MOpcode::Str8RegFpImm:
        case MOpcode::Str16RegFpImm:
        case MOpcode::Str32RegFpImm:
        case MOpcode::StrFprFpImm:
        case MOpcode::StrRegBaseImm:
        case MOpcode::Str8RegBaseImm:
        case MOpcode::Str16RegBaseImm:
        case MOpcode::Str32RegBaseImm:
        case MOpcode::StrFprBaseImm:
        case MOpcode::StrRegSpImm:
        case MOpcode::StrFprSpImm:
        case MOpcode::StpRegFpImm:
        case MOpcode::StpFprFpImm:
        case MOpcode::PhiStoreGPR:
        case MOpcode::PhiStoreFPR:
        case MOpcode::Bl:
        case MOpcode::Blr:
        case MOpcode::Br:
        case MOpcode::BCond:
        case MOpcode::Ret:
        case MOpcode::Cbz:
        case MOpcode::Cbnz:
        case MOpcode::Tbz:
        case MOpcode::Tbnz:
        case MOpcode::JumpTable:
        case MOpcode::SubSpImm:
        case MOpcode::AddSpImm:
        case MOpcode::CmpRR:
        case MOpcode::CmpRI:
        case MOpcode::TstRR:
        case MOpcode::FCmpRR:
        case MOpcode::LdrRegFpImm:
        case MOpcode::Ldr8RegFpImm:
        case MOpcode::Ldr16RegFpImm:
        case MOpcode::Ldr32RegFpImm:
        case MOpcode::LdrFprFpImm:
        case MOpcode::LdrRegBaseImm:
        case MOpcode::Ldr8RegBaseImm:
        case MOpcode::Ldr16RegBaseImm:
        case MOpcode::Ldr32RegBaseImm:
        case MOpcode::LdrFprBaseImm:
        case MOpcode::LdrRegBaseRegLsl:
        case MOpcode::Ldr32RegBaseRegLsl:
        case MOpcode::LdrFprBaseRegLsl:
        case MOpcode::LdpRegFpImm:
        case MOpcode::LdpFprFpImm:
        case MOpcode::AdrPage:
        case MOpcode::AddPageOff:
        case MOpcode::AddFpImm:
            return true;
        case MOpcode::MovRR:
        case MOpcode::MovRI:
        case MOpcode::FMovRR:
        case MOpcode::FMovRI:
        case MOpcode::FMovGR: {
            // Moves into ABI argument/return registers feed calls and returns
            // whose implicit reads local DCE does not model; keep them.
            if (instr.ops.empty())
                return false;
            const auto &dst = instr.ops[0];
            if (dst.kind != MOperand::Kind::Reg || !dst.reg.isPhys)
                return false;
            auto pr = static_cast<PhysReg>(dst.reg.idOrPhys);
            if (dst.reg.cls == RegClass::GPR && pr <= PhysReg::X7)
                return true;
            if (dst.reg.cls == RegClass::FPR && pr >= PhysReg::V0 && pr <= PhysReg::V7)
                return true;
            return false;
        }
        default:
            // Flag-setting arithmetic feeds a later conditional branch or
            // select through NZCV, which local DCE does not track.
            return setsFlags(instr.opc);
    }
}

/// @copydoc getDefinedReg
std::optional<MOperand> getDefinedReg(const MInstr &instr) noexcept {
    // Flag-setting forms are never candidates for local dead-result removal:
    // their NZCV result is consumed by a later branch or select that local DCE
    // does not track (the CFG-aware DCE guards the same way).
    if (setsFlags(instr.opc))
        return std::nullopt;
    for (std::size_t idx = 0; idx < instr.ops.size(); ++idx) {
        if (safeRoles(instr, idx).second && isPhysReg(instr.ops[idx]))
            return instr.ops[idx];
    }
    return std::nullopt;
}

} // namespace zanna::codegen::aarch64::peephole
