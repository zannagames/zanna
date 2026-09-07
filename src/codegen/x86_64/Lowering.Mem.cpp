//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Lowering.Mem.cpp
// Purpose: Implement memory-oriented opcode lowering rules for the x86-64
//          backend, including loads, stores, and call sequencing.
// Key invariants:
//   - Emitters rely on EmitCommon for operand preparation.
//   - ABI-mandated register classes are preserved.
//   - Malformed operand shapes produce an explicit backend diagnostic.
// Ownership/Lifetime:
//   - Operates on borrowed MIRBuilder state; call metadata is recorded for
//     later passes without taking ownership of IR nodes.
// Links: src/codegen/x86_64/LoweringRules.hpp,
//        src/codegen/x86_64/Lowering.EmitCommon.hpp,
//        src/codegen/x86_64/CallLowering.hpp
//
//===----------------------------------------------------------------------===//

#include "LoweringRuleTable.hpp"

#include "CallLowering.hpp"
#include "LowerILToMIR.hpp"
#include "Lowering.EmitCommon.hpp"
#include "Noreturn.hpp"
#include "OperandUtils.hpp"
#include "Unsupported.hpp"

#include "codegen/common/StringRetainPolicy.hpp"
#include "il/runtime/RuntimeNameMap.hpp"
#include "il/runtime/RuntimeSignatures.hpp"

#include <bit>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @file
 * @brief Implements memory, call, literal, stack-allocation, and address rules.
 *
 * Direct and indirect calls are translated into delayed ABI call plans and
 * result-register captures, including string-retain policy. Memory operations
 * reuse EmitCommon addressing, while allocas, GEPs, globals, nulls, strings,
 * and floating constants are materialized into the appropriate MIR forms.
 */

namespace zanna::codegen::x64::lowering {
namespace {

/// @brief Predicate: does @p kind live in a GPR on x86-64?
/// @details Duplicate of helpers in other lowering TUs; keeping a local copy
///          avoids cross-file include churn while these emitters remain
///          stand-alone.
/// @param kind IL kind to classify.
/// @return @c true for an integer, boolean, or pointer.
[[nodiscard]] bool isIntegerLikeKind(ILValue::Kind kind) noexcept {
    return kind == ILValue::Kind::I64 || kind == ILValue::Kind::I1 || kind == ILValue::Kind::PTR;
}

/// @brief Predicate: is @p value an immediate of integer-class kind?
/// @param builder Builder used for immediate classification.
/// @param value Candidate IL value.
/// @return @c true for an inline integer, boolean, or pointer payload.
[[nodiscard]] bool isIntegerLikeImmediate(const MIRBuilder &builder,
                                          const ILValue &value) noexcept {
    return builder.isImmediate(value) && isIntegerLikeKind(value.kind);
}

/// @brief Canonicalise an integer-class IL immediate to its signed 64-bit value.
/// @details I1 booleans are clamped to {0, 1} before being treated as i64.
/// @param value Integer-like immediate.
/// @return Canonical payload for MIR immediate emission.
[[nodiscard]] int64_t integerImmediateValue(const ILValue &value) noexcept {
    return value.kind == ILValue::Kind::I1 ? (value.i64 != 0 ? 1 : 0) : value.i64;
}

/// @brief Translate an IL call argument into the @ref CallArg descriptor.
/// @details The descriptor records whether the argument is an immediate or
///          lives in a register and tags it with the calling-convention class
///          (GPR vs FPR) so the eventual argument shuffle in FrameLowering can
///          route it to the right slot. F64 immediates have their bit pattern
///          captured so they can be materialised through a GPR scratch.
/// @param argVal Source IL value (positional argument).
/// @param builder Active MIR builder (provides immediate detection and class lookup).
/// @return Populated @ref CallArg.
CallArg makeCallArg(const ILValue &argVal, MIRBuilder &builder) {
    CallArg arg{};
    arg.cls =
        builder.regClassFor(argVal.kind) == RegClass::GPR ? CallArgClass::GPR : CallArgClass::FPR;

    if (argVal.kind != ILValue::Kind::LABEL && argVal.kind != ILValue::Kind::STR &&
        builder.isImmediate(argVal)) {
        arg.isImm = true;
        arg.imm = (argVal.kind == ILValue::Kind::F64)
                      ? static_cast<int64_t>(std::bit_cast<std::uint64_t>(argVal.f64))
                      : (argVal.kind == ILValue::Kind::I1 ? (argVal.i64 != 0 ? 1 : 0) : argVal.i64);
        return arg;
    }

    Operand operand = builder.makeOperandForValue(argVal, builder.regClassFor(argVal.kind));
    if (!std::holds_alternative<OpReg>(operand) && !std::holds_alternative<OpImm>(operand)) {
        operand =
            EmitCommon(builder).materialise(std::move(operand), builder.regClassFor(argVal.kind));
    }
    if (const auto *reg = std::get_if<OpReg>(&operand)) {
        arg.vreg = reg->idOrPhys;
    } else if (const auto *imm = std::get_if<OpImm>(&operand)) {
        arg.isImm = true;
        arg.imm = imm->val;
    }
    return arg;
}

/// @brief Populate the variadic-call metadata on a call plan.
/// @details Combines static tables (@c isVarArgCallee, @c findRuntimeSignature)
///          with the lowerer's user-supplied set of known vararg targets. If
///          the callee is a vararg function the plan records the named-arg
///          count so the SysV "AL holds XMM count" prologue can be emitted
///          correctly; for non-variadic callees the named count equals the
///          actual argument count.
/// @param plan Call plan being populated (modified in place).
/// @param callee Symbol name as it appears in IL.
/// @param builder Active MIR builder (used to consult the user vararg set).
void applyKnownVarArgMetadata(CallLoweringPlan &plan,
                              std::string_view callee,
                              const MIRBuilder &builder) {
    if (callee.empty())
        return;

    std::string_view mappedCallee = callee;
    if (const auto mapped = il::runtime::mapCanonicalRuntimeName(callee))
        mappedCallee = *mapped;

    plan.isVarArg = il::runtime::isVarArgCallee(mappedCallee) ||
                    il::runtime::isVarArgCallee(callee) ||
                    builder.lower().isKnownVarArgCallee(mappedCallee) ||
                    builder.lower().isKnownVarArgCallee(callee);
    if (!plan.isVarArg) {
        plan.numNamedArgs = plan.args.size();
        return;
    }

    if (const auto *mappedSig = il::runtime::findRuntimeSignature(mappedCallee))
        plan.numNamedArgs = mappedSig->paramTypes.size();
    else if (const auto *rawSig = il::runtime::findRuntimeSignature(callee))
        plan.numNamedArgs = rawSig->paramTypes.size();
    else
        plan.numNamedArgs = 0;
}

/// @brief Build a CALL instruction tagged with the supplied call-plan id.
/// @details Tagged CALLs let the FrameLowering pass find their plan when
///          emitting argument shuffles and stack adjustment.
/// @param target Direct label or indirect call target operand.
/// @param callPlanId Stable plan index recorded by MIRBuilder.
/// @return CALL instruction carrying @p callPlanId.
MInstr makePlannedCall(Operand target, uint32_t callPlanId) {
    MInstr call = MInstr::make(MOpcode::CALL, std::vector<Operand>{std::move(target)});
    call.callPlanId = callPlanId;
    return call;
}

/// @brief Emit @c rt_str_retain_maybe(resultVReg) for a freshly returned value.
/// @details The runtime helper is no-op when the value is not a managed string,
///          so it can be applied unconditionally to any call whose result kind
///          is @c STR. Without this retain the caller's reference count would
///          remain at whatever the callee returned (typically 1) and an
///          additional decrement during cleanup would prematurely free the
///          string.
/// @param resultVReg Virtual register holding the captured return value.
/// @param builder Active MIR builder.
void emitRetainStringResultCall(const VReg &resultVReg, MIRBuilder &builder) {
    CallLoweringPlan retainPlan{};
    retainPlan.callee = "rt_str_retain_maybe";
    retainPlan.args.push_back(
        CallArg{.cls = CallArgClass::GPR, .vreg = resultVReg.id, .isImm = false, .imm = 0});
    retainPlan.numNamedArgs = retainPlan.args.size();

    const uint32_t callPlanId = builder.recordCallPlan(std::move(retainPlan));
    builder.append(
        makePlannedCall(makeLabelOperand(std::string{"rt_str_retain_maybe"}), callPlanId));
}

/// @brief Move a call's ABI return register into the caller's result vreg.
/// @details Dispatches on the IL result kind:
///          - F64 returns travel via the FP return register (XMM0 on SysV /
///            Win64) and are copied with @c MOVSDrr.
///          - I1 returns occupy the integer return register (RAX) but only
///            the low byte is meaningful; @c MOVZXrr8 zero-extends.
///          - All other GPR kinds use @c MOVrr.
///          When the result kind is @c STR, the captured value is also
///          retained via @ref emitRetainStringResultCall — unless the callee
///          is known to transfer an owned reference, in which case the
///          defensive retain is elided (see StringRetainPolicy.hpp).
/// @param instr Original IL call instruction (used to consult result kind).
/// @param resultVReg Virtual register that will hold the captured value.
/// @param builder Active MIR builder.
/// @param callee Direct callee name, or empty for indirect calls.
void emitCapturedCallResult(const ILInstr &instr,
                            const VReg &resultVReg,
                            MIRBuilder &builder,
                            std::string_view callee = {}) {
    const Operand resultOp = makeVRegOperand(resultVReg.cls, resultVReg.id);
    if (instr.resultKind == ILValue::Kind::F64) {
        const Operand retReg =
            makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(builder.target().f64ReturnReg));
        builder.append(MInstr::make(MOpcode::MOVSDrr, std::vector<Operand>{resultOp, retReg}));
    } else if (instr.resultKind == ILValue::Kind::I1) {
        const Operand retReg =
            makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(builder.target().intReturnReg));
        builder.append(MInstr::make(MOpcode::MOVZXrr8, std::vector<Operand>{resultOp, retReg}));
    } else {
        const Operand retReg =
            makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(builder.target().intReturnReg));
        builder.append(MInstr::make(MOpcode::MOVrr, std::vector<Operand>{resultOp, retReg}));
    }

    (void)callee;
    if (instr.resultKind == ILValue::Kind::STR &&
        !builder.lower().isStrCallRetainElidable(instr.resultId))
        emitRetainStringResultCall(resultVReg, builder);
}

} // namespace

/// @brief Lower an IL call instruction into the backend call plan.
/// @details Builds a @ref CallLoweringPlan by classifying the callee operand,
///          materialising argument descriptors, and reserving result vregs when
///          present.  The completed plan is recorded on the @p builder so that
///          later lowering phases can emit ABI-conforming prologues and
///          epilogues.  Finally, a placeholder CALL is appended to the Machine
///          IR so scheduling and register allocation see the pending call.
/// @param instr High-level IL instruction describing the call.
/// @param builder MIR construction context that owns register state.
void emitCall(const ILInstr &instr, MIRBuilder &builder) {
    if (instr.ops.empty()) {
        phaseAUnsupported("call: missing callee");
    }

    CallLoweringPlan plan{};
    if (instr.ops.front().kind != ILValue::Kind::LABEL)
        phaseAUnsupported("call target is not a label");
    plan.callee = instr.ops.front().label;

    for (std::size_t idx = 1; idx < instr.ops.size(); ++idx) {
        plan.args.push_back(makeCallArg(instr.ops[idx], builder));
    }
    applyKnownVarArgMetadata(plan, plan.callee, builder);

    VReg resultVReg{};
    bool hasResult = (instr.resultId >= 0);
    if (hasResult) {
        resultVReg = builder.ensureVReg(instr.resultId, instr.resultKind);
        if (instr.resultKind == ILValue::Kind::F64) {
            plan.returnsF64 = true;
        }
    }

    const uint32_t callPlanId = builder.recordCallPlan(std::move(plan));
    builder.append(makePlannedCall(builder.makeLabelOperand(instr.ops[0]), callPlanId));

    // A runtime helper that never returns (trap.from_err lowers to a plain
    // call of rt_trap_raise_error) ends the block: the defensive UD2 keeps the
    // MIR block terminated like the inline trap emitters do, so the CFG and
    // the verifier never see control fall off a no-return call.
    if (common::isNoReturnRuntimeCallee(instr.ops.front().label)) {
        builder.append(MInstr::make(MOpcode::UD2));
        return;
    }

    if (hasResult)
        emitCapturedCallResult(instr, resultVReg, builder, instr.ops.front().label);
}

/// @brief Lower an IL call.indirect instruction into the backend call plan.
/// @details Similar to emitCall but treats the first operand as a value holding
///          the callee pointer (in a register or memory). Records the call plan
///          for argument setup and appends a CALL with an indirect target.
/// @param instr High-level IL instruction describing the indirect call.
/// @param builder MIR construction context that owns register state.
void emitCallIndirect(const ILInstr &instr, MIRBuilder &builder) {
    if (instr.ops.empty()) {
        phaseAUnsupported("call.indirect: missing target");
    }
    if (instr.ops[0].kind != ILValue::Kind::LABEL && !isIntegerLikeKind(instr.ops[0].kind)) {
        phaseAUnsupported("call.indirect: target must be a pointer or label");
    }

    CallLoweringPlan plan{};
    if (instr.ops[0].kind == ILValue::Kind::LABEL)
        plan.callee = instr.ops[0].label;

    for (std::size_t idx = 1; idx < instr.ops.size(); ++idx) {
        plan.args.push_back(makeCallArg(instr.ops[idx], builder));
    }
    applyKnownVarArgMetadata(plan, plan.callee, builder);

    VReg resultVReg{};
    bool hasResult = (instr.resultId >= 0);
    if (hasResult) {
        resultVReg = builder.ensureVReg(instr.resultId, instr.resultKind);
        if (instr.resultKind == ILValue::Kind::F64) {
            plan.returnsF64 = true;
        }
    }

    const uint32_t callPlanId = builder.recordCallPlan(std::move(plan));
    // Use GPR as preferred class when materialising the callee pointer.
    Operand calleeOp = builder.makeOperandForValue(instr.ops[0], RegClass::GPR);
    if (const auto *reg = std::get_if<OpReg>(&calleeOp); reg && reg->cls != RegClass::GPR) {
        phaseAUnsupported("call.indirect: target register must be GPR");
    }
    if (std::holds_alternative<OpLabel>(calleeOp)) {
        calleeOp = EmitCommon(builder).materialiseGpr(std::move(calleeOp));
    }
    // The CALL instruction requires a register or memory operand here; labels
    // name function addresses and are first materialized with LEA.
    // If the callee materialised as an immediate (e.g. null function pointer),
    // load it into a register so the encoder can emit an indirect call.
    if (std::holds_alternative<OpImm>(calleeOp)) {
        const VReg tmp = builder.makeTempVReg(RegClass::GPR);
        const Operand tmpOp = makeVRegOperand(tmp.cls, tmp.id);
        builder.append(MInstr::make(MOpcode::MOVri, std::vector<Operand>{tmpOp, calleeOp}));
        calleeOp = makeVRegOperand(tmp.cls, tmp.id);
    }
    builder.append(makePlannedCall(std::move(calleeOp), callPlanId));

    if (hasResult)
        emitCapturedCallResult(instr, resultVReg, builder);
}

/// @brief Lower an automatic storage load instruction.
/// @details Delegates to @ref EmitCommon::emitLoad so that addressing modes and
///          register class selection stay consistent with the rest of the
///          backend.  The helper ensures the result vreg is allocated in the
///          correct class for the instruction's result kind.
/// @param instr IL instruction representing the load.
/// @param builder MIR construction context.
void emitLoadAuto(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon(builder).emitLoad(instr, builder.regClassFor(instr.resultKind));
}

/// @brief Lower a store instruction targeting automatic storage.
/// @details Invokes @ref EmitCommon::emitStore to synthesise the necessary
///          Machine IR operations.  Using the shared helper keeps store
///          semantics aligned with other lowering paths and guarantees
///          consistent operand validation.
/// @param instr IL store instruction.
/// @param builder MIR construction context.
void emitStore(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon(builder).emitStore(instr);
}

/// @brief Lower a const_str instruction to produce a runtime string handle.
/// @details Emits a call to rt_str_from_lit with the string literal data,
///          storing the result in the destination vreg.
/// @param instr IL const_str instruction with string operand.
/// @param builder MIR construction context.
void emitConstStr(const ILInstr &instr, MIRBuilder &builder) {
    if (instr.ops.empty() || instr.resultId < 0) {
        phaseAUnsupported("const_str: missing operands");
    }
    if (instr.resultKind != ILValue::Kind::STR) {
        phaseAUnsupported("const_str: result must be a string");
    }

    // The operand contains the string literal data
    const auto &strVal = instr.ops.front();
    if (strVal.kind != ILValue::Kind::STR) {
        phaseAUnsupported("const_str: operand must be a string literal");
    }

    // Reserve the result vreg
    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    // Materialize the string using the MIRBuilder's STR handling
    // This emits LEA + CALL rt_str_from_lit and returns the result
    const Operand strOp = builder.makeOperandForValue(strVal, RegClass::GPR);

    // Copy the materialized result to the destination vreg
    builder.append(MInstr::make(MOpcode::MOVrr, std::vector<Operand>{dest, strOp}));
}

/// @brief Lower an alloca instruction to allocate stack space.
/// @details Allocates a stack slot and produces the address in the result vreg.
///          The actual frame offset is assigned during FrameLowering pass.
/// @param instr IL alloca instruction with size operand.
/// @param builder MIR construction context.
void emitAlloca(const ILInstr &instr, MIRBuilder &builder) {
    if (instr.resultId < 0) {
        return;
    }
    if (instr.resultKind != ILValue::Kind::PTR) {
        phaseAUnsupported("alloca: result must be a pointer");
    }
    if (instr.ops.empty() || !isIntegerLikeImmediate(builder, instr.ops[0])) {
        phaseAUnsupported("alloca: size must be a positive integer immediate");
    }
    const int64_t sizeImm = integerImmediateValue(instr.ops[0]);
    if (sizeImm <= 0 || sizeImm > std::numeric_limits<int32_t>::max()) {
        phaseAUnsupported("alloca: size is out of range");
    }

    // Reserve the result vreg for the pointer
    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);
    // A stack address is provably mapped; loads/stores through it skip the
    // null-page guard (see EmitCommon::emitNullAddressGuard).
    builder.lower().noteAllocaResult(instr.resultId);

    const int sizeBytes = static_cast<int>(sizeImm);
    const int32_t placeholderOffset =
        builder.reserveStackLocalPlaceholder(sizeBytes, kSlotSizeBytes);

    // LEA dest, [rbp + offset]
    const OpReg rbpBase = makePhysBase(PhysReg::RBP);
    const Operand mem = makeMemOperand(rbpBase, placeholderOffset);
    builder.append(MInstr::make(MOpcode::LEA, std::vector<Operand>{dest, mem}));
}

/// @brief Lower a GEP (get element pointer) instruction.
/// @details Computes base + offset and stores the result pointer.
/// @param instr IL GEP instruction with base and offset operands.
/// @param builder MIR construction context.
void emitGEP(const ILInstr &instr, MIRBuilder &builder) {
    if (instr.resultId < 0 || instr.ops.size() < 2) {
        phaseAUnsupported("gep: missing operands");
    }
    if (instr.resultKind != ILValue::Kind::PTR) {
        phaseAUnsupported("gep: result must be a pointer");
    }
    if (instr.ops[0].kind != ILValue::Kind::PTR) {
        phaseAUnsupported("gep: base must be a pointer");
    }

    // Reserve the result vreg for the pointer
    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    EmitCommon emit(builder);

    // Get the base pointer. It can be a pointer literal such as null, so force
    // it into a GPR before building LEA-style addressing.
    const Operand baseOp =
        emit.materialiseGpr(builder.makeOperandForValue(instr.ops[0], RegClass::GPR));
    const auto *baseReg = std::get_if<OpReg>(&baseOp);
    if (!baseReg) {
        phaseAUnsupported("gep: base did not materialize to a register");
    }

    // Get the offset
    const auto &offsetVal = instr.ops[1];
    if (!isIntegerLikeKind(offsetVal.kind)) {
        phaseAUnsupported("gep: offset must be an integer value");
    }

    if (builder.isImmediate(offsetVal)) {
        const int64_t offsetImm = integerImmediateValue(offsetVal);
        if (fitsImm32(offsetImm)) {
            // Base is a register, offset is immediate -> use LEA [base + imm]
            const int32_t offset = static_cast<int32_t>(offsetImm);
            const Operand mem = makeMemOperand(*baseReg, offset);
            builder.append(MInstr::make(MOpcode::LEA, std::vector<Operand>{dest, mem}));
        } else {
            const VReg offsetReg = builder.makeTempVReg(RegClass::GPR);
            const Operand offset = makeVRegOperand(offsetReg.cls, offsetReg.id);
            builder.append(MInstr::make(MOpcode::MOVri,
                                        std::vector<Operand>{offset, makeImmOperand(offsetImm)}));
            builder.append(MInstr::make(MOpcode::MOVrr, std::vector<Operand>{dest, baseOp}));
            builder.append(MInstr::make(MOpcode::ADDrr, std::vector<Operand>{dest, offset}));
        }
    } else {
        // Both base and offset are registers -> use LEA [base + index*1]
        Operand offsetOp = builder.makeOperandForValue(offsetVal, RegClass::GPR);
        const auto *offsetReg = std::get_if<OpReg>(&offsetOp);
        if (!offsetReg) {
            offsetOp = emit.materialiseGpr(std::move(offsetOp));
            offsetReg = std::get_if<OpReg>(&offsetOp);
        }
        if (offsetReg) {
            const Operand mem = makeMemOperand(*baseReg, *offsetReg, 1, 0);
            builder.append(MInstr::make(MOpcode::LEA, std::vector<Operand>{dest, mem}));
        } else {
            // Fallback: copy base to dest, then add offset
            builder.append(MInstr::make(MOpcode::MOVrr, std::vector<Operand>{dest, baseOp}));
            builder.append(MInstr::make(MOpcode::ADDrr, std::vector<Operand>{dest, offsetOp}));
        }
    }
}

/// @brief Lower a const_null instruction (null pointer constant).
/// @details Produces a zero-valued pointer by moving immediate 0 into the result register.
/// @param instr IL instruction providing the pointer result identifier and kind.
/// @param builder MIR builder receiving the constant materialization.
void emitConstNull(const ILInstr &instr, MIRBuilder &builder) {
    if (instr.resultId < 0) {
        phaseAUnsupported("const_null: missing result");
    }
    if (instr.resultKind != ILValue::Kind::PTR) {
        phaseAUnsupported("const_null: result must be a pointer");
    }

    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);
    builder.append(MInstr::make(MOpcode::MOVri, std::vector<Operand>{dest, makeImmOperand(0)}));
}

/// @brief Lower a const_f64 instruction (double-precision floating-point constant).
/// @details Materialises the 64-bit constant by transferring the bit pattern through
///          a GPR temporary into the XMM destination register.
/// @param instr IL instruction providing the floating result and literal operand.
/// @param builder MIR builder receiving the materialization sequence.
void emitConstF64(const ILInstr &instr, MIRBuilder &builder) {
    if (instr.resultId < 0 || instr.ops.empty()) {
        return;
    }
    if (instr.resultKind != ILValue::Kind::F64 || instr.ops.front().kind != ILValue::Kind::F64) {
        phaseAUnsupported("const_f64: expected f64 result and operand");
    }

    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    EmitCommon emit(builder);
    Operand src = builder.makeOperandForValue(instr.ops[0], RegClass::XMM);
    src = emit.materialise(std::move(src), RegClass::XMM);
    builder.append(MInstr::make(MOpcode::MOVSDrr, std::vector<Operand>{dest, src}));
}

/// @brief Lower a gaddr instruction (global address).
/// @details Loads the address of a global symbol into the result register using LEA.
/// @param instr IL instruction providing the pointer result and global label operand.
/// @param builder MIR builder receiving the address materialization.
void emitGAddr(const ILInstr &instr, MIRBuilder &builder) {
    if (instr.resultId < 0 || instr.ops.empty()) {
        return;
    }
    if (instr.resultKind != ILValue::Kind::PTR || instr.ops.front().kind != ILValue::Kind::LABEL) {
        phaseAUnsupported("gaddr: expected pointer result and label operand");
    }

    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    EmitCommon emit(builder);
    const Operand src =
        emit.materialiseGpr(builder.makeOperandForValue(instr.ops[0], RegClass::GPR));
    builder.append(MInstr::make(MOpcode::MOVrr, std::vector<Operand>{dest, src}));
}

/// @brief Lower an addr_of instruction (address of a local alloca).
/// @details The alloca instruction already produces a pointer to the stack slot.
///          AddrOf simply forwards that pointer to the result register.
/// @param instr IL instruction providing the pointer result and source alloca value.
/// @param builder MIR builder receiving the pointer copy.
void emitAddrOf(const ILInstr &instr, MIRBuilder &builder) {
    if (instr.resultId < 0 || instr.ops.empty()) {
        return;
    }
    if (instr.resultKind != ILValue::Kind::PTR || instr.ops.front().kind != ILValue::Kind::PTR) {
        phaseAUnsupported("addr_of: expected pointer result and operand");
    }

    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    EmitCommon emit(builder);
    const Operand src =
        emit.materialiseGpr(builder.makeOperandForValue(instr.ops[0], RegClass::GPR));
    builder.append(MInstr::make(MOpcode::MOVrr, std::vector<Operand>{dest, src}));
}

} // namespace zanna::codegen::x64::lowering
