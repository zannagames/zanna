//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/peephole/PeepholeCommon.cpp
// Purpose: Out-of-line implementations for the switch-heavy AArch64 peephole
//          classifiers (definesReg, usesReg, classifyOperand, hasSideEffects,
//          getDefinedReg, updateKnownConsts). Keeping the per-opcode switches
//          here keeps PeepholeCommon.hpp small and lets sub-pass translation
//          units skip the ~500-LOC compile cost they previously paid through
//          header inlining.
// Key invariants:
//   - Behaviour must match the historical inline definitions byte-for-byte;
//     this is a pure refactor.
//   - Tables stay in sync with MachineIR opcode additions (same constraint as
//     when these lived in the header).
//
// Ownership/Lifetime:
//   - Free functions; no state.
//
// Links: codegen/aarch64/peephole/PeepholeCommon.hpp
//
//===----------------------------------------------------------------------===//

#include "PeepholeCommon.hpp"

/// @file
/// @brief Implements opcode-specific AArch64 peephole dataflow classifiers.

namespace zanna::codegen::aarch64::peephole {

/// @copydoc definesReg
bool definesReg(const MInstr &instr, const MOperand &reg) noexcept {
    if (!isPhysReg(reg))
        return false;

    switch (instr.opc) {
        // --- Moves and conversions (dest = ops[0]) ---
        case MOpcode::MovRR:
        case MOpcode::MovRI:
        case MOpcode::FMovRR:
        case MOpcode::FMovRI:
        case MOpcode::FMovGR:
        // --- Integer arithmetic (dest = ops[0]) ---
        case MOpcode::AddRRRLsl:
        case MOpcode::SubRRRLsl:
        case MOpcode::AndRRRLsl:
        case MOpcode::OrrRRRLsl:
        case MOpcode::EorRRRLsl:
        case MOpcode::LdrRegBaseRegLsl:
        case MOpcode::Ldr32RegBaseRegLsl:
        case MOpcode::LdrFprBaseRegLsl:
        case MOpcode::AddRRR:
        case MOpcode::SubRRR:
        case MOpcode::MulRRR:
        case MOpcode::SmulhRRR:
        case MOpcode::UmulhRRR:
        case MOpcode::SDivRRR:
        case MOpcode::UDivRRR:
        case MOpcode::AddRI:
        case MOpcode::SubRI:
        case MOpcode::MSubRRRR:
        case MOpcode::MAddRRRR:
        // --- Bitwise (dest = ops[0]) ---
        case MOpcode::AndRRR:
        case MOpcode::OrrRRR:
        case MOpcode::EorRRR:
        case MOpcode::AndRI:
        case MOpcode::OrrRI:
        case MOpcode::EorRI:
        // --- Shifts (dest = ops[0]) ---
        case MOpcode::LslRI:
        case MOpcode::LsrRI:
        case MOpcode::AsrRI:
        case MOpcode::LslvRRR:
        case MOpcode::LsrvRRR:
        case MOpcode::AsrvRRR:
        // --- Loads (dest = ops[0]) ---
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
        // --- Address computation (dest = ops[0]) ---
        case MOpcode::AddFpImm:
        case MOpcode::AdrPage:
        case MOpcode::AddPageOff:
        // --- Floating-point arithmetic (dest = ops[0]) ---
        case MOpcode::FAddRRR:
        case MOpcode::FSubRRR:
        case MOpcode::FMulRRR:
        case MOpcode::FDivRRR:
        // --- FP/int conversions (dest = ops[0]) ---
        case MOpcode::SCvtF:
        case MOpcode::FCvtZS:
        case MOpcode::UCvtF:
        case MOpcode::FCvtZU:
        case MOpcode::FRintN:
        // --- Conditional select/set (dest = ops[0]) ---
        case MOpcode::Cset:
        case MOpcode::Csel:
        case MOpcode::FCsel:
        // --- Flag-setting arithmetic (dest = ops[0]) ---
        case MOpcode::AddsRRR:
        case MOpcode::SubsRRR:
        case MOpcode::AddsRI:
        case MOpcode::SubsRI:
        case MOpcode::AddOvfRRR:
        case MOpcode::SubOvfRRR:
        case MOpcode::AddOvfRI:
        case MOpcode::SubOvfRI:
        case MOpcode::MulOvfRRR:
            if (!instr.ops.empty() && samePhysReg(instr.ops[0], reg))
                return true;
            break;

        // LDP defines two registers (ops[0] and ops[1])
        case MOpcode::LdpRegFpImm:
        case MOpcode::LdpFprFpImm:
            if (instr.ops.size() >= 1 && samePhysReg(instr.ops[0], reg))
                return true;
            if (instr.ops.size() >= 2 && samePhysReg(instr.ops[1], reg))
                return true;
            break;

        // JumpTable clobbers only the reserved X16/X17 scratch registers,
        // which never carry values across instructions.
        case MOpcode::JumpTable:
            break;

        // --- Instructions that don't define registers ---
        case MOpcode::PhiStoreGPR:
        case MOpcode::PhiStoreFPR:
        case MOpcode::CmpRR:
        case MOpcode::CmpRI:
        case MOpcode::TstRR:
        case MOpcode::FCmpRR:
        case MOpcode::Br:
        case MOpcode::BCond:
        case MOpcode::Bl:
        case MOpcode::Blr:
        case MOpcode::Ret:
        case MOpcode::Cbz:
        case MOpcode::Cbnz:
        case MOpcode::Tbz:
        case MOpcode::Tbnz:
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
        case MOpcode::StrRegBaseRegLsl:
        case MOpcode::Str32RegBaseRegLsl:
        case MOpcode::StrFprBaseRegLsl:
        case MOpcode::StrRegSpImm:
        case MOpcode::StrFprSpImm:
        case MOpcode::StpRegFpImm:
        case MOpcode::StpFprFpImm:
        case MOpcode::SubSpImm:
        case MOpcode::AddSpImm:
            break;
    }
    return false;
}

/// @copydoc usesReg
bool usesReg(const MInstr &instr, const MOperand &reg) noexcept {
    if (!isPhysReg(reg))
        return false;

    switch (instr.opc) {
        case MOpcode::MovRR:
        case MOpcode::FMovRR:
        case MOpcode::FMovGR:
            if (instr.ops.size() >= 2 && samePhysReg(instr.ops[1], reg))
                return true;
            break;

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
        case MOpcode::FAddRRR:
        case MOpcode::FSubRRR:
        case MOpcode::FMulRRR:
        case MOpcode::FDivRRR:
        case MOpcode::LslvRRR:
        case MOpcode::LsrvRRR:
        case MOpcode::AsrvRRR:
        case MOpcode::AddsRRR:
        case MOpcode::SubsRRR:
        case MOpcode::AddOvfRRR:
        case MOpcode::SubOvfRRR:
        case MOpcode::MulOvfRRR:
            if (instr.ops.size() >= 2 && samePhysReg(instr.ops[1], reg))
                return true;
            if (instr.ops.size() >= 3 && samePhysReg(instr.ops[2], reg))
                return true;
            break;

        case MOpcode::AndRI:
        case MOpcode::OrrRI:
        case MOpcode::EorRI:
        case MOpcode::AddRI:
        case MOpcode::SubRI:
        case MOpcode::LslRI:
        case MOpcode::LsrRI:
        case MOpcode::AsrRI:
        case MOpcode::AddsRI:
        case MOpcode::SubsRI:
        case MOpcode::AddOvfRI:
        case MOpcode::SubOvfRI:
            if (instr.ops.size() >= 2 && samePhysReg(instr.ops[1], reg))
                return true;
            break;

        case MOpcode::CmpRR:
        case MOpcode::TstRR:
        case MOpcode::FCmpRR:
            if (instr.ops.size() >= 1 && samePhysReg(instr.ops[0], reg))
                return true;
            if (instr.ops.size() >= 2 && samePhysReg(instr.ops[1], reg))
                return true;
            break;

        case MOpcode::CmpRI:
            if (instr.ops.size() >= 1 && samePhysReg(instr.ops[0], reg))
                return true;
            break;

        case MOpcode::StrRegFpImm:
        case MOpcode::Str8RegFpImm:
        case MOpcode::Str16RegFpImm:
        case MOpcode::Str32RegFpImm:
        case MOpcode::StrFprFpImm:
            if (instr.ops.size() >= 1 && samePhysReg(instr.ops[0], reg))
                return true;
            break;

        case MOpcode::StrRegBaseImm:
        case MOpcode::Str8RegBaseImm:
        case MOpcode::Str16RegBaseImm:
        case MOpcode::Str32RegBaseImm:
        case MOpcode::StrFprBaseImm:
            if (instr.ops.size() >= 1 && samePhysReg(instr.ops[0], reg))
                return true;
            if (instr.ops.size() >= 2 && samePhysReg(instr.ops[1], reg))
                return true;
            break;

        case MOpcode::StrRegBaseRegLsl:
        case MOpcode::Str32RegBaseRegLsl:
        case MOpcode::StrFprBaseRegLsl:
            for (std::size_t i = 0; i < instr.ops.size() && i <= 2; ++i) {
                if (samePhysReg(instr.ops[i], reg))
                    return true;
            }
            break;

        case MOpcode::LdrRegBaseRegLsl:
        case MOpcode::Ldr32RegBaseRegLsl:
        case MOpcode::LdrFprBaseRegLsl:
        case MOpcode::AddRRRLsl:
        case MOpcode::SubRRRLsl:
        case MOpcode::AndRRRLsl:
        case MOpcode::OrrRRRLsl:
        case MOpcode::EorRRRLsl:
            if (instr.ops.size() >= 2 && samePhysReg(instr.ops[1], reg))
                return true;
            if (instr.ops.size() >= 3 && samePhysReg(instr.ops[2], reg))
                return true;
            break;

        case MOpcode::LdrRegBaseImm:
        case MOpcode::Ldr8RegBaseImm:
        case MOpcode::Ldr16RegBaseImm:
        case MOpcode::Ldr32RegBaseImm:
        case MOpcode::LdrFprBaseImm:
            if (instr.ops.size() >= 2 && samePhysReg(instr.ops[1], reg))
                return true;
            break;

        case MOpcode::Cbz:
        case MOpcode::Tbz:
        case MOpcode::Tbnz:
        case MOpcode::JumpTable:
            if (instr.ops.size() >= 1 && samePhysReg(instr.ops[0], reg))
                return true;
            break;

        case MOpcode::Blr:
            if (instr.ops.size() >= 1 && samePhysReg(instr.ops[0], reg))
                return true;
            break;

        case MOpcode::SCvtF:
        case MOpcode::FCvtZS:
        case MOpcode::UCvtF:
        case MOpcode::FCvtZU:
            if (instr.ops.size() >= 2 && samePhysReg(instr.ops[1], reg))
                return true;
            break;

        case MOpcode::MSubRRRR:
        case MOpcode::MAddRRRR:
            for (std::size_t i = 1; i < instr.ops.size() && i <= 3; ++i) {
                if (samePhysReg(instr.ops[i], reg))
                    return true;
            }
            break;

        case MOpcode::AddPageOff:
            if (instr.ops.size() >= 2 && samePhysReg(instr.ops[1], reg))
                return true;
            break;

        case MOpcode::Cbnz:
            if (instr.ops.size() >= 1 && samePhysReg(instr.ops[0], reg))
                return true;
            break;

        case MOpcode::Csel:
        case MOpcode::FCsel:
            if (instr.ops.size() >= 2 && samePhysReg(instr.ops[1], reg))
                return true;
            if (instr.ops.size() >= 3 && samePhysReg(instr.ops[2], reg))
                return true;
            break;

        case MOpcode::StpRegFpImm:
        case MOpcode::StpFprFpImm:
            if (instr.ops.size() >= 1 && samePhysReg(instr.ops[0], reg))
                return true;
            if (instr.ops.size() >= 2 && samePhysReg(instr.ops[1], reg))
                return true;
            break;

        case MOpcode::LdpRegFpImm:
        case MOpcode::LdpFprFpImm:
            break;

        default:
            break;
    }
    return false;
}

/// @copydoc classifyOperand
std::pair<bool, bool> classifyOperand(const MInstr &instr, std::size_t idx) noexcept {
    switch (instr.opc) {
        case MOpcode::LdrRegBaseRegLsl:
        case MOpcode::Ldr32RegBaseRegLsl:
        case MOpcode::LdrFprBaseRegLsl:
            if (idx == 0)
                return {false, true};
            if (idx == 1 || idx == 2)
                return {true, false};
            return {false, false};

        case MOpcode::StrRegBaseRegLsl:
        case MOpcode::Str32RegBaseRegLsl:
        case MOpcode::StrFprBaseRegLsl:
            if (idx <= 2)
                return {true, false};
            return {false, false};

        case MOpcode::AddRRRLsl:
        case MOpcode::SubRRRLsl:
        case MOpcode::AndRRRLsl:
        case MOpcode::OrrRRRLsl:
        case MOpcode::EorRRRLsl:
            if (idx == 0)
                return {false, true};
            if (idx == 1 || idx == 2)
                return {true, false};
            return {false, false};

        case MOpcode::MovRR:
        case MOpcode::MovRI:
        case MOpcode::FMovRR:
        case MOpcode::FMovRI:
        case MOpcode::FMovGR:
            return idx == 0 ? std::make_pair(false, true) : std::make_pair(true, false);

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
        case MOpcode::FAddRRR:
        case MOpcode::FSubRRR:
        case MOpcode::FMulRRR:
        case MOpcode::FDivRRR:
        case MOpcode::LslvRRR:
        case MOpcode::LsrvRRR:
        case MOpcode::AsrvRRR:
            return idx == 0 ? std::make_pair(false, true) : std::make_pair(true, false);

        case MOpcode::AndRI:
        case MOpcode::OrrRI:
        case MOpcode::EorRI:
        case MOpcode::AddRI:
        case MOpcode::SubRI:
        case MOpcode::LslRI:
        case MOpcode::LsrRI:
        case MOpcode::AsrRI:
            if (idx == 0)
                return {false, true};
            if (idx == 1)
                return {true, false};
            return {false, false};

        case MOpcode::CmpRR:
        case MOpcode::TstRR:
        case MOpcode::FCmpRR:
            return {true, false};

        case MOpcode::CmpRI:
            return idx == 0 ? std::make_pair(true, false) : std::make_pair(false, false);

        case MOpcode::Cset:
            return idx == 0 ? std::make_pair(false, true) : std::make_pair(false, false);

        case MOpcode::LdrRegFpImm:
        case MOpcode::Ldr8RegFpImm:
        case MOpcode::Ldr16RegFpImm:
        case MOpcode::Ldr32RegFpImm:
        case MOpcode::LdrFprFpImm:
            return idx == 0 ? std::make_pair(false, true) : std::make_pair(false, false);

        case MOpcode::LdrRegBaseImm:
        case MOpcode::Ldr8RegBaseImm:
        case MOpcode::Ldr16RegBaseImm:
        case MOpcode::Ldr32RegBaseImm:
        case MOpcode::LdrFprBaseImm:
            if (idx == 0)
                return {false, true};
            if (idx == 1)
                return {true, false};
            return {false, false};

        case MOpcode::StrRegFpImm:
        case MOpcode::Str8RegFpImm:
        case MOpcode::Str16RegFpImm:
        case MOpcode::Str32RegFpImm:
        case MOpcode::StrFprFpImm:
        case MOpcode::StrRegSpImm:
        case MOpcode::StrFprSpImm:
            return idx == 0 ? std::make_pair(true, false) : std::make_pair(false, false);

        case MOpcode::StrRegBaseImm:
        case MOpcode::Str8RegBaseImm:
        case MOpcode::Str16RegBaseImm:
        case MOpcode::Str32RegBaseImm:
        case MOpcode::StrFprBaseImm:
            if (idx == 0)
                return {true, false};
            if (idx == 1)
                return {true, false};
            return {false, false};

        case MOpcode::SCvtF:
        case MOpcode::FCvtZS:
        case MOpcode::UCvtF:
        case MOpcode::FCvtZU:
        case MOpcode::FRintN:
            return idx == 0 ? std::make_pair(false, true) : std::make_pair(true, false);

        case MOpcode::MSubRRRR:
        case MOpcode::MAddRRRR:
            return idx == 0 ? std::make_pair(false, true) : std::make_pair(true, false);

        case MOpcode::Csel:
        case MOpcode::FCsel:
            if (idx == 0)
                return {false, true};
            if (idx == 1 || idx == 2)
                return {true, false};
            return {false, false};

        case MOpcode::LdpRegFpImm:
        case MOpcode::LdpFprFpImm:
            if (idx == 0 || idx == 1)
                return {false, true};
            return {false, false};

        case MOpcode::StpRegFpImm:
        case MOpcode::StpFprFpImm:
            if (idx == 0 || idx == 1)
                return {true, false};
            return {false, false};

        case MOpcode::Cbnz:
            return idx == 0 ? std::make_pair(true, false) : std::make_pair(false, false);

        case MOpcode::AdrPage:
            return idx == 0 ? std::make_pair(false, true) : std::make_pair(false, false);

        case MOpcode::AddPageOff:
            if (idx == 0)
                return {false, true};
            if (idx == 1)
                return {true, false};
            return {false, false};

        case MOpcode::Cbz:
        case MOpcode::Tbz:
        case MOpcode::Tbnz:
            return idx == 0 ? std::make_pair(true, false) : std::make_pair(false, false);

        case MOpcode::JumpTable:
            return idx == 0 ? std::make_pair(true, false) : std::make_pair(false, false);

        case MOpcode::Blr:
            return idx == 0 ? std::make_pair(true, false) : std::make_pair(false, false);

        case MOpcode::AddFpImm:
            return idx == 0 ? std::make_pair(false, true) : std::make_pair(false, false);

        case MOpcode::SubSpImm:
        case MOpcode::AddSpImm:
            return {false, false};

        default:
            return idx == 0 ? std::make_pair(false, true) : std::make_pair(true, false);
    }
}

/// @copydoc updateKnownConsts
void updateKnownConsts(const MInstr &instr, RegConstMap &knownConsts) {
    if (instr.opc == MOpcode::MovRI && instr.ops.size() == 2 && isPhysReg(instr.ops[0]) &&
        instr.ops[1].kind == MOperand::Kind::Imm) {
        knownConsts[instr.ops[0].reg.idOrPhys] = instr.ops[1].imm;
        return;
    }

    switch (instr.opc) {
        case MOpcode::MovRR:
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
        case MOpcode::AndRI:
        case MOpcode::OrrRI:
        case MOpcode::EorRI:
        case MOpcode::AddRI:
        case MOpcode::SubRI:
        case MOpcode::LslRI:
        case MOpcode::LsrRI:
        case MOpcode::AsrRI:
        case MOpcode::LslvRRR:
        case MOpcode::LsrvRRR:
        case MOpcode::AsrvRRR:
        case MOpcode::Cset:
        case MOpcode::LdrRegFpImm:
        case MOpcode::Ldr8RegFpImm:
        case MOpcode::Ldr16RegFpImm:
        case MOpcode::Ldr32RegFpImm:
        case MOpcode::LdrRegBaseImm:
        case MOpcode::Ldr8RegBaseImm:
        case MOpcode::Ldr16RegBaseImm:
        case MOpcode::Ldr32RegBaseImm:
        case MOpcode::AddFpImm:
        case MOpcode::AdrPage:
        case MOpcode::AddPageOff:
        case MOpcode::MSubRRRR:
        case MOpcode::MAddRRRR:
        case MOpcode::Csel:
        case MOpcode::FCsel:
            if (!instr.ops.empty() && isPhysReg(instr.ops[0]))
                knownConsts.erase(instr.ops[0].reg.idOrPhys);
            break;
        case MOpcode::LdpRegFpImm:
        case MOpcode::LdpFprFpImm:
            if (instr.ops.size() >= 1 && isPhysReg(instr.ops[0]))
                knownConsts.erase(instr.ops[0].reg.idOrPhys);
            if (instr.ops.size() >= 2 && isPhysReg(instr.ops[1]))
                knownConsts.erase(instr.ops[1].reg.idOrPhys);
            break;
        case MOpcode::JumpTable:
            // Clobbers the reserved X16/X17 scratch registers.
            knownConsts.erase(16);
            knownConsts.erase(17);
            break;
        default:
            break;
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
            return false;
    }
}

/// @copydoc getDefinedReg
std::optional<MOperand> getDefinedReg(const MInstr &instr) noexcept {
    switch (instr.opc) {
        case MOpcode::MovRR:
        case MOpcode::MovRI:
        case MOpcode::FMovRR:
        case MOpcode::FMovRI:
        case MOpcode::FMovGR:
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
        case MOpcode::AndRI:
        case MOpcode::OrrRI:
        case MOpcode::EorRI:
        case MOpcode::AddRI:
        case MOpcode::SubRI:
        case MOpcode::LslRI:
        case MOpcode::LsrRI:
        case MOpcode::AsrRI:
        case MOpcode::Cset:
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
        case MOpcode::AddFpImm:
        case MOpcode::AdrPage:
        case MOpcode::AddPageOff:
        case MOpcode::FAddRRR:
        case MOpcode::FSubRRR:
        case MOpcode::FMulRRR:
        case MOpcode::FDivRRR:
        case MOpcode::SCvtF:
        case MOpcode::FCvtZS:
        case MOpcode::UCvtF:
        case MOpcode::FCvtZU:
        case MOpcode::FRintN:
        case MOpcode::MSubRRRR:
        case MOpcode::MAddRRRR:
        case MOpcode::Csel:
        case MOpcode::FCsel:
        case MOpcode::LdpRegFpImm:
        case MOpcode::LdpFprFpImm:
            if (!instr.ops.empty() && isPhysReg(instr.ops[0]))
                return instr.ops[0];
            break;

        default:
            break;
    }
    return std::nullopt;
}

} // namespace zanna::codegen::aarch64::peephole
