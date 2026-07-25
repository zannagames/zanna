//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Lowering.CF.cpp
// Purpose: Implement control-flow lowering rules for the x86-64 backend,
//          covering branches, selects, and returns.
// Key invariants:
//   - Emitters rely on EmitCommon for operand preparation.
//   - Register classes are dictated by MIRBuilder.
// Ownership/Lifetime:
//   - Works with borrowed MIRBuilder and IL instruction data; no persistent state.
// Links: src/codegen/x86_64/LoweringRules.hpp,
//        src/codegen/x86_64/Lowering.EmitCommon.hpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#include "LoweringRuleTable.hpp"

#include "LowerILToMIR.hpp"
#include "Lowering.EmitCommon.hpp"
#include "MachineIR.hpp"
#include "Unsupported.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>

/**
 * @file
 * @brief Implements branches, returns, bounds checks, selects, and i32 switches.
 *
 * Small switches become linear compare chains, larger sparse switches use a
 * balanced comparison tree, and sufficiently dense switches use an inline jump
 * table. Bounds checks preserve signed lower-bound behavior while sharing the
 * runtime error-call planning convention used by other checked operations.
 */

namespace zanna::codegen::x64::lowering {
namespace {

/// Runtime error code used for an out-of-range index.
constexpr int64_t kErrBounds = 7;

/// @brief True if @p kind is integer-like (I64/I1/PTR) — GPR-eligible.
/// @param kind IL kind to classify.
/// @return @c true for a GPR-representable integer, boolean, or pointer.
[[nodiscard]] bool isIntegerLikeKind(ILValue::Kind kind) noexcept {
    return kind == ILValue::Kind::I64 || kind == ILValue::Kind::I1 || kind == ILValue::Kind::PTR;
}

/// @brief Extract the integer value of an immediate ILValue.
/// @details Normalizes I1 to a canonical 0/1; all other integer kinds pass
///          their raw @c i64 through. Used to build switch-case keys.
/// @param value Immediate integer-like IL value.
/// @return Canonical signed representation used as a switch key.
[[nodiscard]] int64_t integerImmediateValue(const ILValue &value) noexcept {
    return value.kind == ILValue::Kind::I1 ? (value.i64 != 0 ? 1 : 0) : value.i64;
}

/// @brief Validate and normalize a switch_i32 case immediate.
/// @details x86 lowering eventually emits CMPri, whose encoding is a signed
///          32-bit immediate. Rejecting wider immediates here keeps malformed
///          IL from failing later in the binary encoder or comparing with the
///          wrong switch_i32 semantics.
/// @param value IL case value operand.
/// @return The value narrowed to an i32 switch key and widened for MIR immediates.
/// @throws std::runtime_error Through @c phaseAUnsupported when outside i32.
[[nodiscard]] int64_t checkedSwitchI32Immediate(const ILValue &value) {
    const int64_t raw = integerImmediateValue(value);
    if (raw < std::numeric_limits<int32_t>::min() ||
        raw > std::numeric_limits<int32_t>::max()) {
        phaseAUnsupported("switch: case value out of i32 range");
    }
    return static_cast<int64_t>(static_cast<int32_t>(raw));
}

/// @brief Build a CALL instruction tagged with the supplied call-plan id.
/// @details Duplicate of the helper in Lowering.Arith.cpp; each translation
///          unit owns its copy so the helpers can be in anonymous namespaces.
/// @param target Label operand naming the callee.
/// @param callPlanId Identifier returned by @c MIRBuilder::recordCallPlan.
/// @return CALL @c MInstr ready to be appended.
MInstr makePlannedCall(Operand target, uint32_t callPlanId) {
    MInstr call = MInstr::make(MOpcode::CALL, std::vector<Operand>{std::move(target)});
    call.callPlanId = callPlanId;
    return call;
}

/// @brief Emit the runtime bounds-trap call followed by UD2.
/// @details Used by both @ref emitIdxChk paths when an index falls outside
///          its declared range. UD2 ensures the optimiser cannot let
///          execution fall through past the trap.
/// @param builder Active MIR builder.
void emitBoundsTrap(MIRBuilder &builder) {
    CallLoweringPlan plan{};
    plan.callee = "rt_trap_raise_error";
    plan.numNamedArgs = 1;
    plan.args.push_back(
        CallArg{.cls = CallArgClass::GPR, .vreg = 0, .isImm = true, .imm = kErrBounds});
    const uint32_t callPlanId = builder.recordCallPlan(std::move(plan));
    builder.append(
        makePlannedCall(makeLabelOperand(std::string{"rt_trap_raise_error"}), callPlanId));
    builder.append(MInstr::make(MOpcode::UD2));
}

/// @brief Sign-extend @p src to @p widthBits in a fresh GPR.
/// @details Local copy of the helper used in Lowering.Arith.cpp; lives in
///          this TU's anonymous namespace so the bounds-check lowering can
///          truncate-then-sign-extend indices before comparison without
///          polluting the public lowering surface.
/// @param builder Active MIR builder.
/// @param emit EmitCommon helper bound to @p builder.
/// @param src Source operand.
/// @param widthBits Target signed width (returns unchanged when >= 64).
/// @return Sign-extended operand in a virtual register.
Operand emitSignExtendedToWidth(MIRBuilder &builder,
                                EmitCommon &emit,
                                Operand src,
                                std::uint8_t widthBits) {
    Operand value = emit.materialiseGpr(std::move(src));
    if (widthBits >= 64) {
        return value;
    }

    const int shift = 64 - static_cast<int>(widthBits);
    const VReg narrowedReg = builder.makeTempVReg(RegClass::GPR);
    const Operand narrowed = makeVRegOperand(narrowedReg.cls, narrowedReg.id);
    builder.append(MInstr::make(MOpcode::MOVrr, {emit.clone(narrowed), emit.clone(value)}));
    builder.append(MInstr::make(MOpcode::SHLri, {emit.clone(narrowed), makeImmOperand(shift)}));
    builder.append(MInstr::make(MOpcode::SARri, {emit.clone(narrowed), makeImmOperand(shift)}));
    return narrowed;
}

/// @brief One arm of a switch statement during lowering.
struct SwitchCase {
    int64_t value{0}; ///< Constant value matched by this case.
    Operand label{};  ///< Successor label when @c value matches the scrutinee.
};

/// @brief Recursively emit a balanced compare-tree for a sorted case list.
/// @details For @c N cases this generates @c O(log N) compare/branch steps
///          in the worst case. Each recursive call picks the middle case
///          as the pivot, branches to its label on equality, descends left
///          on less-than, and descends right or falls through to the
///          default otherwise. Used for switches with more than three
///          cases — small switches use a linear cascade in
///          @ref emitSwitchI32.
/// @param cases Sorted (ascending) list of cases.
/// @param begin Inclusive start index of the active range.
/// @param end Exclusive end index of the active range.
/// @param scrutinee Operand holding the value being switched on.
/// @param defaultLabel Label to branch to when no case matches.
/// @param builder Active MIR builder.
void emitSwitchTree(const std::vector<SwitchCase> &cases,
                    std::size_t begin,
                    std::size_t end,
                    const Operand &scrutinee,
                    const Operand &defaultLabel,
                    MIRBuilder &builder) {
    if (begin >= end) {
        builder.append(MInstr::make(MOpcode::JMP, std::vector<Operand>{defaultLabel}));
        return;
    }

    const std::size_t mid = begin + (end - begin) / 2;
    const SwitchCase &pivot = cases[mid];
    builder.append(
        MInstr::make(MOpcode::CMPri, std::vector<Operand>{scrutinee, makeImmOperand(pivot.value)}));
    builder.append(
        MInstr::make(MOpcode::JCC, std::vector<Operand>{makeImmOperand(0), pivot.label}));

    const bool hasLeft = begin < mid;
    const bool hasRight = mid + 1 < end;
    const uint32_t labelSeed = builder.lower().nextLocalLabelId();
    const Operand leftLabel = makeLabelOperand(".Lswitch_left_" + std::to_string(labelSeed));
    const Operand rightLabel = makeLabelOperand(".Lswitch_right_" + std::to_string(labelSeed));

    if (hasLeft) {
        builder.append(
            MInstr::make(MOpcode::JCC, std::vector<Operand>{makeImmOperand(2), leftLabel}));
    }
    builder.append(
        MInstr::make(MOpcode::JMP, std::vector<Operand>{hasRight ? rightLabel : defaultLabel}));

    if (hasLeft) {
        builder.append(MInstr::make(MOpcode::LABEL, std::vector<Operand>{leftLabel}));
        emitSwitchTree(cases, begin, mid, scrutinee, defaultLabel, builder);
    }
    if (hasRight) {
        builder.append(MInstr::make(MOpcode::LABEL, std::vector<Operand>{rightLabel}));
        emitSwitchTree(cases, mid + 1, end, scrutinee, defaultLabel, builder);
    }
}

} // namespace

/// @brief Lower a SELECT IL instruction into Machine IR.
/// @details Delegates to @ref EmitCommon::emitSelect so the helper can implement
///          conditional move sequencing for both integer and floating-point
///          values.
/// @param instr IL select instruction.
/// @param builder Machine IR builder receiving the emitted code.
void emitSelect(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon(builder).emitSelect(instr);
}

/// @brief Lower an unconditional branch IL instruction.
/// @details Calls @ref EmitCommon::emitBranch to append a JMP to the target
///          label extracted from the IL operand list.
/// @param instr IL branch instruction.
/// @param builder Machine IR builder receiving the emitted code.
void emitBranch(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon(builder).emitBranch(instr);
}

/// @brief Lower a conditional branch IL instruction.
/// @details Uses @ref EmitCommon::emitCondBranch to build the TEST/JCC/JMP
///          sequence that mirrors IL conditional control flow.
/// @param instr IL conditional branch instruction.
/// @param builder Machine IR builder receiving the emitted code.
void emitCondBranch(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon(builder).emitCondBranch(instr);
}

/// @brief Lower a RETURN IL instruction.
/// @details Forwards to @ref EmitCommon::emitReturn so ABI-specific register
///          conventions and optional return values are handled uniformly.
/// @param instr IL return instruction.
/// @param builder Machine IR builder receiving the emitted code.
void emitReturn(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon(builder).emitReturn(instr);
}

/// @brief Lower an idx_chk instruction (bounds check with trap on out-of-bounds).
/// @details Emits inline CMP + JCC + UD2 sequences using in-block LABEL definitions
///          to conditionally trap when the index is outside [lower, upper).
///          A zero lower bound uses an unsigned upper comparison, which also
///          rejects negative indices; nonzero bounds use signed comparisons.
///          The result is the index value passed through if the check succeeds.
/// @param instr IL idx_chk instruction: ops[0]=index, ops[1]=lower, ops[2]=upper.
/// @param builder Machine IR builder receiving the emitted code.
void emitIdxChk(const ILInstr &instr, MIRBuilder &builder) {
    if (instr.resultId < 0 || instr.ops.size() < 3) {
        phaseAUnsupported("idx_chk: missing operands");
    }

    EmitCommon emit(builder);
    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    // Materialise the index into a GPR, truncating to the IL result width before comparing.
    Operand index = emitSignExtendedToWidth(
        builder, emit, builder.makeOperandForValue(instr.ops[0], RegClass::GPR), instr.resultBits);

    // Generate unique labels for the skip points
    const uint32_t labelId = builder.lower().nextLocalLabelId();
    const std::string passUpperLabel = ".Lidxchk_u_" + std::to_string(labelId);
    const std::string passLowerLabel = ".Lidxchk_l_" + std::to_string(labelId);

    const bool loIsZero =
        instr.ops[1].id < 0 && instr.ops[1].kind == ILValue::Kind::I64 && instr.ops[1].i64 == 0;

    // Check upper bound.
    Operand upper = builder.makeOperandForValue(instr.ops[2], RegClass::GPR);
    if (instr.resultBits < 64) {
        upper = emitSignExtendedToWidth(builder, emit, std::move(upper), instr.resultBits);
    }
    if (const auto *imm = std::get_if<OpImm>(&upper); imm && fitsImm32(imm->val)) {
        builder.append(MInstr::make(MOpcode::CMPri, std::vector<Operand>{index, upper}));
    } else {
        upper = emit.materialiseGpr(std::move(upper));
        builder.append(MInstr::make(MOpcode::CMPrr, std::vector<Operand>{index, upper}));
    }

    // With lo == 0, unsigned compare also rejects negative indices. Otherwise,
    // use signed compares so negative lower bounds behave like the VM.
    const int passUpperCond = loIsZero ? 8 : 2; // JB / JL
    builder.append(MInstr::make(
        MOpcode::JCC,
        std::vector<Operand>{makeImmOperand(passUpperCond), makeLabelOperand(passUpperLabel)}));
    emitBoundsTrap(builder);
    builder.append(
        MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(passUpperLabel)}));

    Operand lower{};
    if (!loIsZero) {
        lower = builder.makeOperandForValue(instr.ops[1], RegClass::GPR);
        if (instr.resultBits < 64) {
            lower = emitSignExtendedToWidth(builder, emit, std::move(lower), instr.resultBits);
        }
        if (const auto *imm = std::get_if<OpImm>(&lower); imm && fitsImm32(imm->val)) {
            builder.append(MInstr::make(MOpcode::CMPri, std::vector<Operand>{index, lower}));
        } else {
            lower = emit.materialiseGpr(std::move(lower));
            builder.append(MInstr::make(MOpcode::CMPrr, std::vector<Operand>{index, lower}));
        }
        builder.append(MInstr::make(
            MOpcode::JCC,
            std::vector<Operand>{makeImmOperand(5), makeLabelOperand(passLowerLabel)})); // JGE
        emitBoundsTrap(builder);
        builder.append(
            MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(passLowerLabel)}));
    }

    // Result is the normalized zero-based index (idx - lo).
    builder.append(MInstr::make(MOpcode::MOVrr, std::vector<Operand>{dest, index}));
    if (loIsZero) {
        return;
    }

    if (const auto *imm = std::get_if<OpImm>(&lower);
        imm && fitsImm32(imm->val) && imm->val != std::numeric_limits<int64_t>::min()) {
        builder.append(
            MInstr::make(MOpcode::ADDri, std::vector<Operand>{dest, makeImmOperand(-imm->val)}));
        return;
    }

    if (std::holds_alternative<OpImm>(lower)) {
        lower = emit.materialiseGpr(std::move(lower));
    }
    builder.append(MInstr::make(MOpcode::SUBrr, std::vector<Operand>{dest, lower}));
}

/// @brief Lower a switch_i32 instruction (multi-way branch).
/// @details Emits a chain of CMP + JCC pairs, one per case, followed by a JMP to the
///          default label. The operands are: ops[0]=scrutinee, then (value, label) pairs,
///          then an optional default label as the final operand.
/// @param instr IL switch_i32 instruction with variable-length operands.
/// @param builder Machine IR builder receiving the emitted code.
void emitSwitchI32(const ILInstr &instr, MIRBuilder &builder) {
    if (instr.ops.size() < 2) {
        phaseAUnsupported("switch: missing default label");
    }
    if (!isIntegerLikeKind(instr.ops[0].kind)) {
        phaseAUnsupported("switch: scrutinee must be integer-like");
    }
    if (instr.ops.back().kind != ILValue::Kind::LABEL) {
        phaseAUnsupported("switch: default target must be a label");
    }

    const std::size_t caseOperandCount = instr.ops.size() - 2;
    if ((caseOperandCount % 2) != 0) {
        phaseAUnsupported("switch: case value missing target label");
    }

    EmitCommon emit(builder);
    Operand scrutinee =
        emit.materialiseGpr(builder.makeOperandForValue(instr.ops[0], RegClass::GPR));
    std::vector<SwitchCase> cases{};
    for (std::size_t idx = 1; idx + 2 < instr.ops.size(); idx += 2) {
        const ILValue &caseValue = instr.ops[idx];
        const ILValue &caseLabel = instr.ops[idx + 1];
        if (!builder.isImmediate(caseValue) || !isIntegerLikeKind(caseValue.kind)) {
            phaseAUnsupported("switch: case value must be an integer immediate");
        }
        if (caseLabel.kind != ILValue::Kind::LABEL) {
            phaseAUnsupported("switch: case target must be a label");
        }
        cases.push_back(
            SwitchCase{checkedSwitchI32Immediate(caseValue), builder.makeLabelOperand(caseLabel)});
    }

    Operand defLabel = builder.makeLabelOperand(instr.ops.back());
    /// Sort cases by key so duplicate detection, density checks, and trees are deterministic.
    std::sort(cases.begin(), cases.end(), [](const SwitchCase &lhs, const SwitchCase &rhs) {
        return lhs.value < rhs.value;
    });
    for (std::size_t idx = 1; idx < cases.size(); ++idx) {
        if (cases[idx - 1].value == cases[idx].value) {
            phaseAUnsupported("switch: duplicate case value");
        }
    }

    if (cases.size() <= 3) {
        for (const auto &caseEntry : cases) {
            builder.append(MInstr::make(
                MOpcode::CMPri, std::vector<Operand>{scrutinee, makeImmOperand(caseEntry.value)}));
            builder.append(MInstr::make(MOpcode::JCC,
                                        std::vector<Operand>{makeImmOperand(0), caseEntry.label}));
        }
        builder.append(MInstr::make(MOpcode::JMP, std::vector<Operand>{defLabel}));
        return;
    }

    // Dense case sets dispatch through an inline jump table: one unsigned
    // bounds check plus an indirect jump replaces the O(log N) compare tree.
    constexpr std::size_t kMinJumpTableCases = 6;
    constexpr int64_t kMaxJumpTableSpan = 4096;
    constexpr double kMinJumpTableDensity = 0.5;
    if (cases.size() >= kMinJumpTableCases && std::getenv("ZANNA_NO_JUMP_TABLES") == nullptr) {
        const int64_t lo = cases.front().value;
        const int64_t hi = cases.back().value;
        const int64_t span = hi - lo + 1;
        if (span <= kMaxJumpTableSpan &&
            static_cast<double>(cases.size()) >=
                kMinJumpTableDensity * static_cast<double>(span)) {
            EmitCommon emitJt(builder);
            // Values below lo wrap to huge unsigned values, so a single
            // unsigned compare covers both bounds. With lo == 0 the scrutinee
            // IS the table index; a bare copy here would tempt copy
            // forwarding into splitting the compare and the dispatch onto
            // different registers.
            Operand idx = emitJt.clone(scrutinee);
            if (lo != 0) {
                const VReg idxReg = builder.makeTempVReg(RegClass::GPR);
                idx = makeVRegOperand(idxReg.cls, idxReg.id);
                builder.append(
                    MInstr::make(MOpcode::MOVrr, {emitJt.clone(idx), emitJt.clone(scrutinee)}));
                builder.append(
                    MInstr::make(MOpcode::ADDri, {emitJt.clone(idx), makeImmOperand(-lo)}));
            }
            builder.append(
                MInstr::make(MOpcode::CMPri, {emitJt.clone(idx), makeImmOperand(span)}));
            builder.append(MInstr::make(
                MOpcode::JCC, {makeImmOperand(7 /* ae */), emitJt.clone(defLabel)}));

            // The dispatch tail uses the reserved R10/R11 scratch registers,
            // so only the index needs allocation.
            const std::string tblName =
                ".Ljt_" + std::to_string(builder.lower().nextLocalLabelId());
            std::vector<Operand> jtOps{emitJt.clone(idx), makeLabelOperand(tblName)};
            std::size_t ci = 0;
            for (int64_t v = lo; v <= hi; ++v) {
                if (ci < cases.size() && cases[ci].value == v) {
                    jtOps.push_back(emitJt.clone(cases[ci].label));
                    ++ci;
                } else {
                    jtOps.push_back(emitJt.clone(defLabel));
                }
            }
            builder.append(MInstr::make(MOpcode::JUMPTABLE, std::move(jtOps)));
            return;
        }
    }

    emitSwitchTree(cases, 0, cases.size(), scrutinee, defLabel, builder);
}

} // namespace zanna::codegen::x64::lowering
