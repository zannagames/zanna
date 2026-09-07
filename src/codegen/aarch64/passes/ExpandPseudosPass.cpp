//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/ExpandPseudosPass.cpp
// Purpose: Implements explicit expansion of the MIR forms whose immediates or
//          offsets the emitters cannot encode directly.
// Key invariants:
//   - The "needs expansion" test is InstrEffects::emitTimeScratchClobber, the
//     same predicate the effects model and the verifier use, so the three
//     cannot disagree about which instructions are pseudo forms.
//   - Scratch selection avoids the instruction's operands and every reserved
//     scratch register read later in the block before being written.
//   - Every emitted instruction is directly encodable; nested expansions
//     (pair splits, SP stores with wide offsets) recurse through the same
//     rules.
// Ownership/Lifetime:
//   - Stateless; all containers are function-local.
// Links: src/codegen/aarch64/passes/ExpandPseudosPass.hpp,
//        src/codegen/aarch64/InstrEffects.cpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/passes/ExpandPseudosPass.hpp"

#include "codegen/aarch64/A64ImmediateUtils.hpp"
#include "codegen/aarch64/InstrEffects.hpp"
#include "codegen/aarch64/ra/OperandRoles.hpp"
#include "codegen/common/ICE.hpp"

#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

/// @file
/// @brief Implements ExpandPseudosPass and expandPseudoInstructions().

namespace zanna::codegen::aarch64 {

namespace {

/// @brief Preference order for a scratch register: the historical text-emitter order.
using ScratchOrder = std::initializer_list<PhysReg>;

constexpr ScratchOrder kOrderX9First = {kScratchGPR, kScratchGPR2, kScratchGPR3};
constexpr ScratchOrder kOrderX16First = {kScratchGPR2, kScratchGPR, kScratchGPR3};

/// @brief Physical register of a register operand.
[[nodiscard]] PhysReg regOf(const MOperand &op) noexcept {
    return static_cast<PhysReg>(op.reg.idOrPhys);
}

/// @brief Reserved scratch registers read after @p idx before being written.
/// @details A call or jump table redefines every reserved scratch it clobbers,
///          which ends the scan for those registers.
[[nodiscard]] PhysRegSet scratchLiveAfter(const std::vector<MInstr> &instrs, std::size_t idx) {
    const PhysRegSet scratch = emitScratchGPRs();
    PhysRegSet live;
    PhysRegSet defined;
    for (std::size_t j = idx + 1; j < instrs.size(); ++j) {
        const MInstr &mi = instrs[j];
        for (std::size_t k = 0; k < mi.ops.size(); ++k) {
            const auto &op = mi.ops[k];
            if (op.kind != MOperand::Kind::Reg || !op.reg.isPhys)
                continue;
            const PhysReg reg = regOf(op);
            if (!scratch.contains(reg) || defined.contains(reg))
                continue;
            if (ra::operandRoles(mi, k).first)
                live.add(reg);
        }
        for (std::size_t k = 0; k < mi.ops.size(); ++k) {
            const auto &op = mi.ops[k];
            if (op.kind == MOperand::Kind::Reg && op.reg.isPhys && ra::operandRoles(mi, k).second)
                defined.add(regOf(op));
        }
        if (mi.opc == MOpcode::Bl || mi.opc == MOpcode::Blr)
            defined |= scratch;
        if (mi.opc == MOpcode::JumpTable) {
            defined.add(kScratchGPR2);
            defined.add(kScratchGPR3);
        }
        if ((defined.bits & scratch.bits) == scratch.bits)
            break;
    }
    return live;
}

/// @brief Choose the first scratch in @p order that is neither blocked nor live.
[[nodiscard]] PhysReg pickScratch(ScratchOrder order,
                                  PhysRegSet blocked,
                                  PhysRegSet live,
                                  const MInstr &context) {
    for (PhysReg candidate : order) {
        if (!blocked.contains(candidate) && !live.contains(candidate))
            return candidate;
    }
    ZANNA_ICE("AArch64 ExpandPseudos: no free scratch register for '" + toString(context) + "'");
}

/// @brief `MovRI scratch, #imm` carrying @p loc.
[[nodiscard]] MInstr movImm(PhysReg dst, long long imm, const il::support::SourceLoc &loc) {
    MInstr mi{MOpcode::MovRI, {MOperand::regOp(dst), MOperand::immOp(imm)}};
    mi.loc = loc;
    return mi;
}

/// @brief Three-register form of an immediate ALU opcode.
[[nodiscard]] MOpcode registerForm(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::AddRI:
            return MOpcode::AddRRR;
        case MOpcode::SubRI:
            return MOpcode::SubRRR;
        case MOpcode::AddsRI:
            return MOpcode::AddsRRR;
        case MOpcode::SubsRI:
            return MOpcode::SubsRRR;
        case MOpcode::AndRI:
            return MOpcode::AndRRR;
        case MOpcode::OrrRI:
            return MOpcode::OrrRRR;
        case MOpcode::EorRI:
            return MOpcode::EorRRR;
        default:
            return opc;
    }
}

/// @brief Base-register form of a frame-relative access.
[[nodiscard]] MOpcode baseForm(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::LdrRegFpImm:
            return MOpcode::LdrRegBaseImm;
        case MOpcode::Ldr8RegFpImm:
            return MOpcode::Ldr8RegBaseImm;
        case MOpcode::Ldr16RegFpImm:
            return MOpcode::Ldr16RegBaseImm;
        case MOpcode::Ldr32RegFpImm:
            return MOpcode::Ldr32RegBaseImm;
        case MOpcode::LdrFprFpImm:
            return MOpcode::LdrFprBaseImm;
        case MOpcode::StrRegFpImm:
        case MOpcode::PhiStoreGPR:
            return MOpcode::StrRegBaseImm;
        case MOpcode::Str8RegFpImm:
            return MOpcode::Str8RegBaseImm;
        case MOpcode::Str16RegFpImm:
            return MOpcode::Str16RegBaseImm;
        case MOpcode::Str32RegFpImm:
            return MOpcode::Str32RegBaseImm;
        case MOpcode::StrFprFpImm:
        case MOpcode::PhiStoreFPR:
            return MOpcode::StrFprBaseImm;
        case MOpcode::StrRegSpImm:
            return MOpcode::StrRegBaseImm;
        case MOpcode::StrFprSpImm:
            return MOpcode::StrFprBaseImm;
        default:
            return opc;
    }
}

/// @brief Whether @p opc is a base-register load/store (`[base, #imm]`).
[[nodiscard]] bool isBaseAccess(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::LdrRegBaseImm:
        case MOpcode::Ldr8RegBaseImm:
        case MOpcode::Ldr16RegBaseImm:
        case MOpcode::Ldr32RegBaseImm:
        case MOpcode::LdrFprBaseImm:
        case MOpcode::StrRegBaseImm:
        case MOpcode::Str8RegBaseImm:
        case MOpcode::Str16RegBaseImm:
        case MOpcode::Str32RegBaseImm:
        case MOpcode::StrFprBaseImm:
            return true;
        default:
            return false;
    }
}

/// @brief Whether @p opc is a scalar frame-relative access (`[x29, #imm]`).
[[nodiscard]] bool isFrameAccess(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::LdrRegFpImm:
        case MOpcode::Ldr8RegFpImm:
        case MOpcode::Ldr16RegFpImm:
        case MOpcode::Ldr32RegFpImm:
        case MOpcode::LdrFprFpImm:
        case MOpcode::StrRegFpImm:
        case MOpcode::Str8RegFpImm:
        case MOpcode::Str16RegFpImm:
        case MOpcode::Str32RegFpImm:
        case MOpcode::StrFprFpImm:
        case MOpcode::PhiStoreGPR:
        case MOpcode::PhiStoreFPR:
            return true;
        default:
            return false;
    }
}

/// @brief Whether @p opc is a pair access (`[x29, #imm]`, two registers).
[[nodiscard]] bool isPairAccess(MOpcode opc) noexcept {
    return opc == MOpcode::LdpRegFpImm || opc == MOpcode::StpRegFpImm ||
           opc == MOpcode::LdpFprFpImm || opc == MOpcode::StpFprFpImm;
}

/// @brief Scalar frame-relative opcode matching one register of a pair access.
[[nodiscard]] MOpcode pairElementForm(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::LdpRegFpImm:
            return MOpcode::LdrRegFpImm;
        case MOpcode::StpRegFpImm:
            return MOpcode::StrRegFpImm;
        case MOpcode::LdpFprFpImm:
            return MOpcode::LdrFprFpImm;
        case MOpcode::StpFprFpImm:
            return MOpcode::StrFprFpImm;
        default:
            return opc;
    }
}

/// @brief Rewrites one block's pseudo forms; shared state for nested expansions.
class Expander {
  public:
    explicit Expander(std::vector<MInstr> &out) : out_(out) {}

    /// @brief Append @p mi to the output, expanding it when required.
    /// @param mi Instruction (possibly a pseudo form).
    /// @param live Reserved scratch registers live after @p mi in the block.
    /// @return Number of pseudo forms rewritten (0 or 1, plus nested ones).
    std::size_t emit(const MInstr &mi, PhysRegSet live) {
        if (!emitTimeScratchClobber(mi)) {
            out_.push_back(mi);
            return 0;
        }
        return expand(mi, live) + 1;
    }

  private:
    std::vector<MInstr> &out_;

    /// @brief Rewrite a pseudo form into explicit MIR.
    /// @return Number of nested pseudo forms that were rewritten in turn.
    std::size_t expand(const MInstr &mi, PhysRegSet live) {
        switch (mi.opc) {
            case MOpcode::AddRI:
            case MOpcode::SubRI:
            case MOpcode::AndRI:
            case MOpcode::OrrRI:
            case MOpcode::EorRI:
                return expandAluImm(mi, live, kOrderX9First);
            case MOpcode::AddsRI:
            case MOpcode::SubsRI:
                return expandAluImm(mi, live, kOrderX16First);
            case MOpcode::CmpRI: {
                const PhysReg lhs = regOf(mi.ops[0]);
                PhysRegSet blocked;
                blocked.add(lhs);
                const PhysReg scratch = pickScratch(kOrderX16First, blocked, live, mi);
                out_.push_back(movImm(scratch, mi.ops[1].imm, mi.loc));
                MInstr cmp{MOpcode::CmpRR, {MOperand::regOp(lhs), MOperand::regOp(scratch)}};
                cmp.loc = mi.loc;
                out_.push_back(std::move(cmp));
                return 0;
            }
            case MOpcode::FMovRI: {
                const PhysReg scratch = pickScratch(kOrderX16First, {}, live, mi);
                long long bits = 0;
                static_assert(sizeof(bits) == sizeof(mi.ops[1].imm), "unexpected f64 size");
                std::memcpy(&bits, &mi.ops[1].imm, sizeof(bits));
                out_.push_back(movImm(scratch, bits, mi.loc));
                MInstr fmov{MOpcode::FMovGR, {mi.ops[0], MOperand::regOp(scratch)}};
                fmov.loc = mi.loc;
                out_.push_back(std::move(fmov));
                return 0;
            }
            case MOpcode::AddFpImm: {
                const PhysReg scratch = pickScratch(kOrderX9First, {}, live, mi);
                out_.push_back(movImm(scratch, mi.ops[1].imm, mi.loc));
                MInstr add{MOpcode::AddRRR,
                           {mi.ops[0], MOperand::regOp(PhysReg::X29), MOperand::regOp(scratch)}};
                add.loc = mi.loc;
                out_.push_back(std::move(add));
                return 0;
            }
            default:
                break;
        }

        if (isPairAccess(mi.opc))
            return expandPair(mi, live);
        if (isFrameAccess(mi.opc)) {
            expandAccess(mi, PhysReg::X29, mi.ops[1].imm, live);
            return 0;
        }
        if (isBaseAccess(mi.opc)) {
            expandAccess(mi, regOf(mi.ops[1]), mi.ops[2].imm, live);
            return 0;
        }
        if (isSpRelativeOpcode(mi.opc)) {
            expandAccess(mi, PhysReg::SP, mi.ops[1].imm, live);
            return 0;
        }

        ZANNA_ICE("AArch64 ExpandPseudos: no expansion for '" + toString(mi) + "'");
    }

    /// @brief `op dst, lhs, #imm` with a non-encodable immediate.
    std::size_t expandAluImm(const MInstr &mi, PhysRegSet live, ScratchOrder order) {
        PhysRegSet blocked;
        blocked.add(regOf(mi.ops[0]));
        blocked.add(regOf(mi.ops[1]));
        const PhysReg scratch = pickScratch(order, blocked, live, mi);
        out_.push_back(movImm(scratch, mi.ops[2].imm, mi.loc));
        MInstr op{registerForm(mi.opc), {mi.ops[0], mi.ops[1], MOperand::regOp(scratch)}};
        op.loc = mi.loc;
        out_.push_back(std::move(op));
        return 0;
    }

    /// @brief Pair access outside the imm7 range: two scalar accesses.
    std::size_t expandPair(const MInstr &mi, PhysRegSet live) {
        const MOpcode scalar = pairElementForm(mi.opc);
        const long long offset = mi.ops[2].imm;
        if (offset > std::numeric_limits<long long>::max() - 8)
            ZANNA_ICE("AArch64 ExpandPseudos: pair offset overflows in '" + toString(mi) + "'");
        MInstr first{scalar, {mi.ops[0], MOperand::immOp(offset)}};
        first.loc = mi.loc;
        MInstr second{scalar, {mi.ops[1], MOperand::immOp(offset + 8)}};
        second.loc = mi.loc;
        // For a load the first element's destination may itself be a scratch
        // the second element must not reuse; the explicit operand check in
        // expandAccess covers the operands, and liveness covers the rest.
        std::size_t nested = 0;
        nested += emit(first, live);
        nested += emit(second, live);
        return nested;
    }

    /// @brief Scalar access whose offset is outside the encodable range.
    /// @details Emits `mov xS,#off; add xS,base,xS; op rt,[xS,#0]`; an SP base
    ///          cannot be an operand of a register `add`, so it is copied with
    ///          `add xS,sp,#0` and the offset added by immediate.
    void expandAccess(const MInstr &mi, PhysReg base, long long offset, PhysRegSet live) {
        const MOpcode opc = baseForm(mi.opc);
        const bool isStore = isStoreOpcode(mi.opc);
        PhysRegSet blocked;
        blocked.add(base);
        if (isStore)
            blocked.add(regOf(mi.ops[0]));
        const PhysReg scratch = pickScratch(kOrderX9First, blocked, live, mi);

        if (base == PhysReg::SP) {
            MInstr copy{
                MOpcode::AddRI,
                {MOperand::regOp(scratch), MOperand::regOp(PhysReg::SP), MOperand::immOp(0)}};
            copy.loc = mi.loc;
            out_.push_back(std::move(copy));
            if (classifyAddSubImmEncoding(absImmUnsigned(offset)).has_value()) {
                MInstr add{
                    MOpcode::AddRI,
                    {MOperand::regOp(scratch), MOperand::regOp(scratch), MOperand::immOp(offset)}};
                add.loc = mi.loc;
                out_.push_back(std::move(add));
            } else {
                PhysRegSet blockedInner = blocked;
                blockedInner.add(scratch);
                const PhysReg inner = pickScratch(kOrderX9First, blockedInner, live, mi);
                out_.push_back(movImm(inner, offset, mi.loc));
                MInstr add{
                    MOpcode::AddRRR,
                    {MOperand::regOp(scratch), MOperand::regOp(scratch), MOperand::regOp(inner)}};
                add.loc = mi.loc;
                out_.push_back(std::move(add));
            }
        } else {
            out_.push_back(movImm(scratch, offset, mi.loc));
            MInstr add{MOpcode::AddRRR,
                       {MOperand::regOp(scratch), MOperand::regOp(base), MOperand::regOp(scratch)}};
            add.loc = mi.loc;
            out_.push_back(std::move(add));
        }

        MInstr access{opc, {mi.ops[0], MOperand::regOp(scratch), MOperand::immOp(0)}};
        access.loc = mi.loc;
        out_.push_back(std::move(access));
    }
};

} // namespace

std::size_t expandPseudoInstructions(MFunction &fn) {
    std::size_t rewritten = 0;
    for (auto &block : fn.blocks) {
        bool needed = false;
        for (const auto &mi : block.instrs) {
            if (emitTimeScratchClobber(mi)) {
                needed = true;
                break;
            }
        }
        if (!needed)
            continue;

        std::vector<MInstr> out;
        out.reserve(block.instrs.size() + 8);
        Expander expander(out);
        for (std::size_t idx = 0; idx < block.instrs.size(); ++idx) {
            const MInstr &mi = block.instrs[idx];
            if (!emitTimeScratchClobber(mi)) {
                out.push_back(mi);
                continue;
            }
            rewritten += expander.emit(mi, scratchLiveAfter(block.instrs, idx));
        }
        block.instrs = std::move(out);
    }
    return rewritten;
}

namespace passes {

bool ExpandPseudosPass::run(AArch64Module &module, Diagnostics &diags) {
    (void)diags;
    for (auto &fn : module.mir)
        (void)expandPseudoInstructions(fn);
    return true;
}

} // namespace passes
} // namespace zanna::codegen::aarch64
