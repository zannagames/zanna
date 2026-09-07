//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/InstrEffects.cpp
// Purpose: Implements the shared instruction-effects model for AArch64 MIR.
// Key invariants:
//   - Explicit operand roles are delegated to ra::operandRoles; this file adds
//     only implicit effects (ABI registers, NZCV, memory, emit-time scratch).
//   - The pseudo-form predicate (emitTimeScratchClobber) mirrors the
//     emitters' encodability checks exactly (A64ImmediateUtils / A64Encoding);
//     it is what ExpandPseudosPass rewrites and what the emitters reject.
// Ownership/Lifetime:
//   - Stateless.
// Links: src/codegen/aarch64/InstrEffects.hpp,
//        src/codegen/aarch64/AsmEmitter.cpp (expansion ranges),
//        src/codegen/aarch64/binenc/A64BinaryEncoder.cpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/InstrEffects.hpp"

#include "codegen/aarch64/A64ImmediateUtils.hpp"
#include "codegen/aarch64/Noreturn.hpp"
#include "codegen/aarch64/binenc/A64Encoding.hpp"
#include "codegen/aarch64/ra/OperandRoles.hpp"

#include <cstring>
#include <optional>

/// @file
/// @brief Implements effectsOf() and the opcode predicates it is built from.

namespace zanna::codegen::aarch64 {

namespace {

/// @brief Last immediate operand of @p mi, if any.
[[nodiscard]] std::optional<long long> lastImmediate(const MInstr &mi) noexcept {
    for (std::size_t k = mi.ops.size(); k > 0; --k) {
        if (mi.ops[k - 1].kind == MOperand::Kind::Imm)
            return mi.ops[k - 1].imm;
    }
    return std::nullopt;
}

} // namespace

bool isFrameRelativeOpcode(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::LdrRegFpImm:
        case MOpcode::StrRegFpImm:
        case MOpcode::Ldr8RegFpImm:
        case MOpcode::Str8RegFpImm:
        case MOpcode::Ldr16RegFpImm:
        case MOpcode::Str16RegFpImm:
        case MOpcode::Ldr32RegFpImm:
        case MOpcode::Str32RegFpImm:
        case MOpcode::LdrFprFpImm:
        case MOpcode::StrFprFpImm:
        case MOpcode::PhiStoreGPR:
        case MOpcode::PhiStoreFPR:
        case MOpcode::AddFpImm:
        case MOpcode::LdpRegFpImm:
        case MOpcode::StpRegFpImm:
        case MOpcode::LdpFprFpImm:
        case MOpcode::StpFprFpImm:
            return true;
        default:
            return false;
    }
}

bool isSpRelativeOpcode(MOpcode opc) noexcept {
    return opc == MOpcode::StrRegSpImm || opc == MOpcode::StrFprSpImm;
}

bool isSpAdjustOpcode(MOpcode opc) noexcept {
    return opc == MOpcode::SubSpImm || opc == MOpcode::AddSpImm;
}

unsigned memAccessBytes(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::Ldr8RegFpImm:
        case MOpcode::Str8RegFpImm:
        case MOpcode::Ldr8RegBaseImm:
        case MOpcode::Str8RegBaseImm:
            return 1;
        case MOpcode::Ldr16RegFpImm:
        case MOpcode::Str16RegFpImm:
        case MOpcode::Ldr16RegBaseImm:
        case MOpcode::Str16RegBaseImm:
            return 2;
        case MOpcode::Ldr32RegFpImm:
        case MOpcode::Str32RegFpImm:
        case MOpcode::Ldr32RegBaseImm:
        case MOpcode::Str32RegBaseImm:
        case MOpcode::Ldr32RegBaseRegLsl:
        case MOpcode::Str32RegBaseRegLsl:
            return 4;
        case MOpcode::LdrRegFpImm:
        case MOpcode::StrRegFpImm:
        case MOpcode::LdrFprFpImm:
        case MOpcode::StrFprFpImm:
        case MOpcode::LdrRegBaseImm:
        case MOpcode::StrRegBaseImm:
        case MOpcode::LdrFprBaseImm:
        case MOpcode::StrFprBaseImm:
        case MOpcode::LdrRegBaseRegLsl:
        case MOpcode::StrRegBaseRegLsl:
        case MOpcode::LdrFprBaseRegLsl:
        case MOpcode::StrFprBaseRegLsl:
        case MOpcode::StrRegSpImm:
        case MOpcode::StrFprSpImm:
        case MOpcode::PhiStoreGPR:
        case MOpcode::PhiStoreFPR:
            return 8;
        case MOpcode::LdpRegFpImm:
        case MOpcode::StpRegFpImm:
        case MOpcode::LdpFprFpImm:
        case MOpcode::StpFprFpImm:
            return 16;
        default:
            return 0;
    }
}

bool isEncodableLdStOffset(long long offset, unsigned accessBytes) noexcept {
    if (offset >= -256 && offset <= 255)
        return true;
    if (accessBytes == 0 || offset < 0)
        return false;
    const auto width = static_cast<long long>(accessBytes);
    return (offset % width) == 0 && (offset / width) <= 4095;
}

bool isEncodablePairOffset(long long offset) noexcept {
    return (offset % 8) == 0 && offset >= -512 && offset <= 504;
}

bool isEncodableSpStoreOffset(long long offset) noexcept {
    return offset >= 0 && (offset % 8) == 0 && (offset / 8) <= 4095;
}

bool setsFlags(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::CmpRR:
        case MOpcode::CmpRI:
        case MOpcode::TstRR:
        case MOpcode::FCmpRR:
        case MOpcode::AddsRRR:
        case MOpcode::SubsRRR:
        case MOpcode::AddsRI:
        case MOpcode::SubsRI:
        case MOpcode::AddOvfRRR:
        case MOpcode::SubOvfRRR:
        case MOpcode::AddOvfRI:
        case MOpcode::SubOvfRI:
        case MOpcode::MulOvfRRR:
        case MOpcode::Bl:
        case MOpcode::Blr:
            return true;
        default:
            return false;
    }
}

bool readsFlags(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::BCond:
        case MOpcode::Cset:
        case MOpcode::Csel:
        case MOpcode::FCsel:
            return true;
        default:
            return false;
    }
}

bool isLoadOpcode(MOpcode opc) noexcept {
    switch (opc) {
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
            return true;
        default:
            return false;
    }
}

bool isStoreOpcode(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::StrRegFpImm:
        case MOpcode::Str8RegFpImm:
        case MOpcode::Str16RegFpImm:
        case MOpcode::Str32RegFpImm:
        case MOpcode::StrFprFpImm:
        case MOpcode::StrRegSpImm:
        case MOpcode::StrFprSpImm:
        case MOpcode::StrRegBaseImm:
        case MOpcode::Str8RegBaseImm:
        case MOpcode::Str16RegBaseImm:
        case MOpcode::Str32RegBaseImm:
        case MOpcode::StrFprBaseImm:
        case MOpcode::StrRegBaseRegLsl:
        case MOpcode::Str32RegBaseRegLsl:
        case MOpcode::StrFprBaseRegLsl:
        case MOpcode::StpRegFpImm:
        case MOpcode::StpFprFpImm:
        case MOpcode::PhiStoreGPR:
        case MOpcode::PhiStoreFPR:
            return true;
        default:
            return false;
    }
}

bool isTerminatorOpcode(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::Br:
        case MOpcode::BCond:
        case MOpcode::Cbz:
        case MOpcode::Cbnz:
        case MOpcode::Tbz:
        case MOpcode::Tbnz:
        case MOpcode::JumpTable:
        case MOpcode::Ret:
            return true;
        default:
            return false;
    }
}

PhysRegSet emitScratchGPRs() noexcept {
    PhysRegSet set;
    set.add(kScratchGPR);
    set.add(kScratchGPR2);
    set.add(kScratchGPR3);
    return set;
}

bool emitTimeScratchClobber(const MInstr &mi) noexcept {
    switch (mi.opc) {
        case MOpcode::AddFpImm: {
            const auto imm = lastImmediate(mi);
            return imm.has_value() && (*imm > 4095 || *imm < -4095);
        }
        // Wide-immediate ALU forms: `mov xS, #imm; op dst, lhs, xS`.
        case MOpcode::AddRI:
        case MOpcode::SubRI:
        case MOpcode::AddsRI:
        case MOpcode::SubsRI: {
            const auto imm = lastImmediate(mi);
            return imm.has_value() && !classifyAddSubImmEncoding(absImmUnsigned(*imm)).has_value();
        }
        case MOpcode::AndRI:
        case MOpcode::OrrRI:
        case MOpcode::EorRI: {
            const auto imm = lastImmediate(mi);
            return imm.has_value() &&
                   binenc::encodeLogicalImmediate(static_cast<uint64_t>(*imm)) < 0;
        }
        case MOpcode::CmpRI: {
            const auto imm = lastImmediate(mi);
            return imm.has_value() && (*imm > 4095 || *imm < -4095);
        }
        case MOpcode::FMovRI: {
            if (mi.ops.size() < 2 || mi.ops[1].kind != MOperand::Kind::Imm)
                return false;
            double value = 0.0;
            static_assert(sizeof(value) == sizeof(mi.ops[1].imm), "unexpected f64 size");
            std::memcpy(&value, &mi.ops[1].imm, sizeof(value));
            return binenc::encodeFP8Immediate(value) < 0;
        }
        case MOpcode::LdpRegFpImm:
        case MOpcode::StpRegFpImm:
        case MOpcode::LdpFprFpImm:
        case MOpcode::StpFprFpImm: {
            const auto imm = lastImmediate(mi);
            return imm.has_value() && !isEncodablePairOffset(*imm);
        }
        case MOpcode::StrRegSpImm:
        case MOpcode::StrFprSpImm: {
            const auto imm = lastImmediate(mi);
            return imm.has_value() && !isEncodableSpStoreOffset(*imm);
        }
        // Register-offset forms carry no displacement.
        case MOpcode::LdrRegBaseRegLsl:
        case MOpcode::Ldr32RegBaseRegLsl:
        case MOpcode::LdrFprBaseRegLsl:
        case MOpcode::StrRegBaseRegLsl:
        case MOpcode::Str32RegBaseRegLsl:
        case MOpcode::StrFprBaseRegLsl:
            return false;
        default:
            break;
    }
    const unsigned width = memAccessBytes(mi.opc);
    if (width == 0)
        return false;
    const auto imm = lastImmediate(mi);
    return imm.has_value() && !isEncodableLdStOffset(*imm, width);
}

PhysRegSet callClobberSet(const TargetInfo &target) noexcept {
    PhysRegSet set;
    for (PhysReg reg : target.callerSavedGPR)
        set.add(reg);
    for (PhysReg reg : target.callerSavedFPR)
        set.add(reg);
    set.add(PhysReg::X30); // LR is written by the branch-and-link itself.
    return set;
}

InstrEffects effectsOf(const MInstr &instr, const TargetInfo &target) {
    InstrEffects fx;
    for (std::size_t idx = 0; idx < instr.ops.size(); ++idx) {
        const auto &op = instr.ops[idx];
        if (op.kind != MOperand::Kind::Reg || !op.reg.isPhys)
            continue;
        const auto [isUse, isDef] = ra::operandRoles(instr, idx);
        const auto phys = static_cast<PhysReg>(op.reg.idOrPhys);
        if (isUse)
            fx.uses.add(phys);
        if (isDef)
            fx.defs.add(phys);
    }

    fx.readsFlags = readsFlags(instr.opc);
    fx.writesFlags = setsFlags(instr.opc);
    fx.isTerminator = isTerminatorOpcode(instr.opc);

    if (isLoadOpcode(instr.opc))
        fx.mem = InstrEffects::Mem::Load;
    else if (isStoreOpcode(instr.opc))
        fx.mem = InstrEffects::Mem::Store;

    if (isFrameRelativeOpcode(instr.opc))
        fx.uses.add(PhysReg::X29);
    if (isSpRelativeOpcode(instr.opc))
        fx.uses.add(PhysReg::SP);
    if (isSpAdjustOpcode(instr.opc)) {
        fx.uses.add(PhysReg::SP);
        fx.defs.add(PhysReg::SP);
        fx.mem = InstrEffects::Mem::Barrier;
    }

    switch (instr.opc) {
        case MOpcode::Bl:
        case MOpcode::Blr: {
            fx.isCall = true;
            fx.isNoReturn = isNoReturnCall(instr);
            fx.mem = InstrEffects::Mem::Barrier;
            for (PhysReg reg : target.intArgOrder)
                fx.uses.add(reg);
            for (PhysReg reg : target.f64ArgOrder)
                fx.uses.add(reg);
            fx.uses.add(PhysReg::SP);
            fx.defs |= callClobberSet(target);
            break;
        }
        case MOpcode::Ret:
            fx.uses.add(target.intReturnReg);
            fx.uses.add(target.f64ReturnReg);
            fx.uses.add(PhysReg::SP);
            break;
        case MOpcode::JumpTable:
            // The dispatch sequence materializes the table address and the
            // scaled target through the reserved x16/x17 scratch registers.
            fx.defs.add(kScratchGPR2);
            fx.defs.add(kScratchGPR3);
            break;
        default:
            break;
    }

    if (emitTimeScratchClobber(instr))
        fx.defs |= emitScratchGPRs();

    return fx;
}

} // namespace zanna::codegen::aarch64
