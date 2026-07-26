//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Lowering.EmitCommon.cpp
// Purpose: Implement the shared lowering helpers declared in
//          Lowering.EmitCommon.hpp. Centralises register materialisation and
//          instruction construction so opcode-specific emitters stay focused.
// Key invariants:
//   - Helper routines respect the register class requested by the caller.
//   - Temporaries are created only when strictly necessary.
// Ownership/Lifetime:
//   - Operates on a borrowed MIRBuilder reference; no IL or MIR objects
//     are owned by this file.
// Links: src/codegen/x86_64/Lowering.EmitCommon.hpp,
//        src/codegen/x86_64/LowerILToMIR.hpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#include "Lowering.EmitCommon.hpp"
#include "Unsupported.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

/**
 * @file
 * @brief Implements shared x86-64 lowering emission and operand legalization.
 *
 * The implementation validates backend-facing IL shapes, selects encodable
 * register/immediate forms, reconstructs safe indexed addresses, emits delayed
 * ABI call plans where needed, and centralizes compare, select, control-flow,
 * memory, cast, and div/rem sequences used by individual rule units.
 */

namespace zanna::codegen::x64 {

namespace {

/// @brief Emit a runtime call that retains @p valueVReg if it holds a managed string.
/// @details Wraps the @c rt_str_retain_maybe runtime entry point. The
///          @c _maybe suffix indicates the helper checks the handle's type tag
///          and no-ops on non-strings, so callers can use it unconditionally
///          when storing a value to a long-lived location.
/// @param builder Active MIR builder.
/// @param valueVReg Virtual register holding the candidate value.
void emitRetainStringVReg(MIRBuilder &builder, const VReg &valueVReg) {
    CallLoweringPlan retainPlan{};
    retainPlan.callee = "rt_str_retain_maybe";
    retainPlan.args.push_back(
        CallArg{.cls = CallArgClass::GPR, .vreg = valueVReg.id, .isImm = false, .imm = 0});
    retainPlan.numNamedArgs = retainPlan.args.size();

    const uint32_t callPlanId = builder.recordCallPlan(std::move(retainPlan));
    MInstr call =
        MInstr::make(MOpcode::CALL, {makeLabelOperand(std::string{"rt_str_retain_maybe"})});
    call.callPlanId = callPlanId;
    builder.append(std::move(call));
}

/// @brief Predicate: is @p value an inline integer-class immediate?
/// @details Combines the builder's @c isImmediate test with a kind check so
///          opcode emitters can take the "imm" path only when both criteria
///          hold (some opcodes refuse pointer immediates even when literal).
/// @param builder Active MIR builder (for the immediate query).
/// @param value IL value to classify.
/// @return True for integer-class immediates (I64/I1/PTR).
[[nodiscard]] bool isIntegerLikeImmediate(const MIRBuilder &builder,
                                          const ILValue &value) noexcept {
    if (!builder.isImmediate(value)) {
        return false;
    }
    return value.kind == ILValue::Kind::I64 || value.kind == ILValue::Kind::I1 ||
           value.kind == ILValue::Kind::PTR;
}

/// @brief Read the canonical immediate payload from an integer-class IL value.
/// @details Same canonicalisation as @ref canonicalIntegerImmediate in
///          LowerILToMIR.cpp; duplicated here to keep the EmitCommon unit
///          self-contained.
/// @param value IL value carrying the payload.
/// @return Signed 64-bit canonical value.
[[nodiscard]] int64_t integerImmediateValue(const ILValue &value) noexcept {
    return value.kind == ILValue::Kind::I1 ? (value.i64 != 0 ? 1 : 0) : value.i64;
}

/// @brief Return true when a backward peephole scan must not cross @p opcode.
/// @details Indexed-address folding is a local straight-line rewrite. Labels
///          and control transfers mark possible alternate paths, so scans stop
///          there instead of inferring definitions from a different path.
/// @param opcode Candidate scan boundary.
/// @return @c true for a label, control transfer, return, or trap.
[[nodiscard]] bool isIndexedFoldScanBarrier(MOpcode opcode) noexcept {
    return opcode == MOpcode::LABEL || opcode == MOpcode::JMP || opcode == MOpcode::JCC ||
           opcode == MOpcode::RET || opcode == MOpcode::UD2;
}

/// @brief Return true if @p instr has a first-operand vreg definition for @p id.
/// @details The address-building patterns this helper recognizes use
///          destination-first MIR instructions. Treating any other definition of
///          the tracked vreg as a blocker keeps the fold conservative.
/// @param instr Instruction to inspect.
/// @param id Virtual-register identifier being traced.
/// @return @c true when operand zero defines the requested virtual register.
[[nodiscard]] bool definesVReg(const MInstr &instr, uint16_t id) noexcept {
    if (instr.operands.empty())
        return false;
    const auto *dst = std::get_if<OpReg>(&instr.operands.front());
    return dst != nullptr && !dst->isPhys && dst->idOrPhys == id;
}

/// @brief True if @p kind is integer-like (I64/I1/PTR) — GPR-eligible.
/// @param kind IL kind to classify.
/// @return @c true for an integer, boolean, or pointer.
[[nodiscard]] bool isIntegerLikeKind(ILValue::Kind kind) noexcept {
    return kind == ILValue::Kind::I64 || kind == ILValue::Kind::I1 || kind == ILValue::Kind::PTR;
}

/// @brief Assert @p value has an integer-like IL kind (I64/I1/PTR).
/// @details Raises phaseAUnsupported(@p context) when the kind is not
///          integer-like, halting lowering of a shape the backend can't emit.
/// @param value Value whose kind is validated.
/// @param context Diagnostic text reported on mismatch.
void requireIntegerLike(const ILValue &value, const char *context) {
    if (!isIntegerLikeKind(value.kind)) {
        phaseAUnsupported(context);
    }
}

/// @brief Assert @p value is an IL label operand.
/// @details Raises phaseAUnsupported(@p context) for any non-LABEL kind;
///          used by control-flow emitters that require a branch target.
/// @param value Candidate branch-target value.
/// @param context Diagnostic text reported on mismatch.
void requireLabel(const ILValue &value, const char *context) {
    if (value.kind != ILValue::Kind::LABEL) {
        phaseAUnsupported(context);
    }
}

/// @brief Assert @p operand is a register of @p cls, or (for GPR) an immediate.
/// @details A register operand must match @p cls; an immediate is accepted only
///          when @p cls is GPR. Anything else raises phaseAUnsupported(@p context).
/// @param operand Operand to validate.
/// @param cls Required register class.
/// @param context Diagnostic text reported on mismatch.
void requireRegOrImmForClass(const Operand &operand, RegClass cls, const char *context) {
    if (const auto *reg = std::get_if<OpReg>(&operand)) {
        if (reg->cls != cls) {
            phaseAUnsupported(context);
        }
        return;
    }
    if (cls == RegClass::GPR && std::holds_alternative<OpImm>(operand)) {
        return;
    }
    phaseAUnsupported(context);
}

/// @brief Assert @p operand is a register of exactly @p cls (no immediate form).
/// @details Stricter than requireRegOrImmForClass(); raises
///          phaseAUnsupported(@p context) for immediates or class mismatch.
/// @param operand Operand to validate.
/// @param cls Required register class.
/// @param context Diagnostic text reported on mismatch.
void requireRegisterForClass(const Operand &operand, RegClass cls, const char *context) {
    const auto *reg = std::get_if<OpReg>(&operand);
    if (!reg || reg->cls != cls) {
        phaseAUnsupported(context);
    }
}

} // namespace

// fitsImm32() is now declared inline in Lowering.EmitCommon.hpp.

/// @brief Construct an @ref EmitCommon helper that appends to @p builder.
/// @details Stores the builder pointer for later reuse so subsequent helper
///          calls share a common emission context without repeatedly passing the
///          builder reference.
/// @param builder Active MIR builder receiving emitted instructions.
EmitCommon::EmitCommon(MIRBuilder &builder) noexcept : builder_(&builder) {}

/// @brief Access the underlying MIR builder.
/// @return Reference to the builder supplied at construction.
MIRBuilder &EmitCommon::builder() const noexcept {
    return *builder_;
}

/// @brief Create a shallow copy of a Machine IR operand.
/// @details Operands are lightweight value types, so copying by value is
///          sufficient when forwarding operands into new instructions.  The
///          helper centralises the intent and improves readability at call
///          sites.
/// @param operand Operand to duplicate.
/// @return Copy of @p operand.
Operand EmitCommon::clone(const Operand &operand) const {
    return operand;
}

/// @brief Materialise an operand into a register of the requested class.
/// @details Values already represented as registers are passed through.
///          Immediates become MOV instructions, labels trigger LEA, and other
///          operands are copied via MOVrr.  A fresh temporary register is
///          created when materialisation is required so callers can subsequently
///          reference the operand in register-only instruction forms.
/// @param operand Operand that may require materialisation.
/// @param cls Target register class for the temporary.
/// @return Operand referencing the original value or the created temporary.
Operand EmitCommon::materialise(Operand operand, RegClass cls) {
    if (std::holds_alternative<OpReg>(operand)) {
        requireRegisterForClass(operand, cls, "operand register class mismatch");
        return operand;
    }

    const VReg tmp = builder().makeTempVReg(cls);
    const Operand tmpOp = makeVRegOperand(tmp.cls, tmp.id);

    if (std::holds_alternative<OpImm>(operand)) {
        if (cls == RegClass::XMM) {
            // XMM registers cannot be loaded directly from immediates.
            // Move the bit-pattern to a GPR first, then transfer to XMM unchanged.
            const VReg gprTmp = builder().makeTempVReg(RegClass::GPR);
            const Operand gprOp = makeVRegOperand(gprTmp.cls, gprTmp.id);
            builder().append(
                MInstr::make(MOpcode::MOVri, std::vector<Operand>{clone(gprOp), clone(operand)}));
            builder().append(
                MInstr::make(MOpcode::MOVQrx, std::vector<Operand>{clone(tmpOp), clone(gprOp)}));
        } else {
            builder().append(
                MInstr::make(MOpcode::MOVri, std::vector<Operand>{clone(tmpOp), clone(operand)}));
        }
    } else if (const auto *label = std::get_if<OpLabel>(&operand)) {
        if (cls != RegClass::GPR) {
            phaseAUnsupported("label operand requested in an XMM context");
        }
        builder().append(MInstr::make(
            MOpcode::LEA, std::vector<Operand>{clone(tmpOp), makeRipLabelOperand(label->name)}));
    } else if (const auto *rip = std::get_if<OpRipLabel>(&operand)) {
        if (cls == RegClass::XMM) {
            builder().append(
                MInstr::make(MOpcode::MOVSDmr, std::vector<Operand>{clone(tmpOp), clone(operand)}));
        } else {
            builder().append(MInstr::make(
                MOpcode::LEA, std::vector<Operand>{clone(tmpOp), makeRipLabelOperand(rip->name)}));
        }
    } else if (std::holds_alternative<OpMem>(operand)) {
        builder().append(MInstr::make(cls == RegClass::XMM ? MOpcode::MOVSDmr : MOpcode::MOVmr,
                                      std::vector<Operand>{clone(tmpOp), clone(operand)}));
    } else {
        phaseAUnsupported("cannot materialise unsupported operand kind");
    }

    return tmpOp;
}

/// @brief Best-effort attempt to fuse `base + (idx << k) + disp` into a memory operand.
/// @details Walks backward through the current block looking for the MIR
///          definition of the address producer's vreg and tries to recover
///          a (base, index, scale, disp) tuple from a recent ADDrr. Returns
///          @c std::nullopt when the analysis cannot prove the pattern
///          (e.g. the def is in a different block or the producer used a
///          physical register). Conservative — never returns a fused
///          operand when there is any doubt.
/// @param addrProducer IL instruction that computes the candidate address.
/// @param displacementOperandIndex Reserved; not yet consulted but kept in
///        the signature so future improvements can plumb explicit
///        displacement operands.
/// @return Fused memory operand or @c std::nullopt.
std::optional<Operand> EmitCommon::tryMakeIndexedMem(const ILInstr &addrProducer,
                                                     std::size_t displacementOperandIndex) {
    // Attempt to reconstruct (base + (idx << k) + disp) from MIR in the current block.
    if (addrProducer.ops.empty()) {
        return std::nullopt;
    }

    const std::optional<Operand> addrOp =
        builder().tryGetOperandForValue(addrProducer.ops[0], RegClass::GPR);
    if (!addrOp) {
        return std::nullopt;
    }

    const auto *addrReg = std::get_if<OpReg>(&*addrOp);
    if (!addrReg || addrReg->isPhys) {
        return std::nullopt;
    }

    const uint16_t addrVReg = addrReg->idOrPhys;
    MBasicBlock &blk = builder().block();

    std::size_t defIdx = static_cast<std::size_t>(-1);
    for (std::size_t i = blk.instructions.size(); i > 0; --i) {
        const auto &mi = blk.instructions[i - 1];
        if (mi.operands.empty()) {
            continue;
        }
        if (const auto *dst = std::get_if<OpReg>(&mi.operands[0]); dst) {
            if (!dst->isPhys && dst->idOrPhys == addrVReg) {
                defIdx = i - 1;
                break;
            }
        }
    }
    if (defIdx == static_cast<std::size_t>(-1)) {
        return std::nullopt;
    }

    const MInstr &addInstr = blk.instructions[defIdx];
    if (addInstr.opcode != MOpcode::ADDrr || addInstr.operands.size() < 2) {
        return std::nullopt;
    }

    const auto *idxReg = std::get_if<OpReg>(&addInstr.operands[1]);
    if (!idxReg || idxReg->isPhys) {
        return std::nullopt;
    }

    OpReg baseReg{};
    bool haveBase = false;
    for (std::size_t i = defIdx; i > 0; --i) {
        const auto &mi = blk.instructions[i - 1];
        if (isIndexedFoldScanBarrier(mi.opcode)) {
            return std::nullopt;
        }
        if (mi.opcode == MOpcode::MOVrr && mi.operands.size() >= 2) {
            const auto *dst = std::get_if<OpReg>(&mi.operands[0]);
            const auto *src = std::get_if<OpReg>(&mi.operands[1]);
            if (dst && src && !dst->isPhys && dst->idOrPhys == addrVReg) {
                baseReg = *src;
                haveBase = true;
                break;
            }
        }
        if (definesVReg(mi, addrVReg)) {
            return std::nullopt;
        }
    }
    if (!haveBase) {
        return std::nullopt;
    }

    uint8_t scale = 1;
    OpReg actualIdx = *idxReg;
    for (std::size_t i = defIdx; i > 0; --i) {
        const auto &mi = blk.instructions[i - 1];
        if (isIndexedFoldScanBarrier(mi.opcode)) {
            break;
        }
        if (definesVReg(mi, idxReg->idOrPhys) && mi.opcode != MOpcode::SHLri) {
            break;
        }
        if (mi.opcode == MOpcode::SHLri && mi.operands.size() >= 2) {
            const auto *dst = std::get_if<OpReg>(&mi.operands[0]);
            const auto *imm = std::get_if<OpImm>(&mi.operands[1]);
            if (dst && imm && !dst->isPhys && dst->idOrPhys == idxReg->idOrPhys) {
                const int sh = static_cast<int>(imm->val);
                if (sh >= 0 && sh <= 3) {
                    scale = static_cast<uint8_t>(1U << sh);
                    // Now trace back to find the original index before the SHL
                    // SHL is destructive, so we need to find the MOV that defined the SHL dest
                    bool foundOriginalIndex = false;
                    for (std::size_t j = i - 1; j > 0; --j) {
                        const auto &mj = blk.instructions[j - 1];
                        if (isIndexedFoldScanBarrier(mj.opcode)) {
                            return std::nullopt;
                        }
                        if (mj.opcode == MOpcode::MOVrr && mj.operands.size() >= 2) {
                            const auto *movDst = std::get_if<OpReg>(&mj.operands[0]);
                            const auto *movSrc = std::get_if<OpReg>(&mj.operands[1]);
                            if (movDst && movSrc && !movDst->isPhys &&
                                movDst->idOrPhys == idxReg->idOrPhys) {
                                actualIdx = *movSrc;
                                foundOriginalIndex = true;
                                break;
                            }
                        }
                        if (definesVReg(mj, idxReg->idOrPhys)) {
                            return std::nullopt;
                        }
                    }
                    if (!foundOriginalIndex) {
                        return std::nullopt;
                    }
                }
                break;
            }
        }
    }

    int32_t disp = 0;
    if (addrProducer.ops.size() > displacementOperandIndex) {
        const ILValue &dispValue = addrProducer.ops[displacementOperandIndex];
        if (!isIntegerLikeImmediate(builder(), dispValue) ||
            !fitsImm32(integerImmediateValue(dispValue))) {
            return std::nullopt;
        }
        disp = static_cast<int32_t>(integerImmediateValue(dispValue));
    }

    return makeMemOperand(baseReg, actualIdx, scale, disp);
}

/// @brief Ensure an operand is materialised in a general-purpose register.
/// @details Convenience wrapper around @ref materialise that hard codes the GPR
///          register class, used when instructions require integer register
///          operands.
/// @param operand Operand to materialise if necessary.
/// @return Operand referencing the resulting GPR.
Operand EmitCommon::materialiseGpr(Operand operand) {
    return materialise(std::move(operand), RegClass::GPR);
}

/// @brief Fold an oversized displacement into a freshly allocated base register.
/// @details See header for the contract. Centralises the imm32-bounds check so
///          load and store lowering stay in lockstep; previously this sequence
///          was copy-pasted at both call sites.
/// @param baseOp Input base-register operand to preserve or materialize.
/// @param[in,out] disp Signed byte displacement; reset to zero when folded into the base.
/// @return Effective base operand for the resulting memory reference.
Operand EmitCommon::materialiseDisplacement(const Operand &baseOp, int64_t &disp) {
    if (fitsImm32(disp))
        return clone(baseOp);

    const auto *baseReg = std::get_if<OpReg>(&baseOp);
    if (!baseReg || baseReg->cls != RegClass::GPR) {
        phaseAUnsupported("large displacement requires a GPR base register");
    }

    const VReg offsetReg = builder().makeTempVReg(RegClass::GPR);
    const Operand offset = makeVRegOperand(offsetReg.cls, offsetReg.id);
    const VReg addrReg = builder().makeTempVReg(RegClass::GPR);
    const Operand addr = makeVRegOperand(addrReg.cls, addrReg.id);
    builder().append(
        MInstr::make(MOpcode::MOVri, std::vector<Operand>{clone(offset), makeImmOperand(disp)}));
    builder().append(
        MInstr::make(MOpcode::MOVrr, std::vector<Operand>{clone(addr), clone(baseOp)}));
    builder().append(MInstr::make(MOpcode::ADDrr, std::vector<Operand>{clone(addr), offset}));
    disp = 0;
    return addr;
}

/// @brief Emit a binary arithmetic instruction with immediate folding support.
/// @details Copies the left-hand operand into the destination register, then
///          attempts to use the immediate form when the right-hand operand is a
///          suitable immediate value.  Otherwise the helper materialises the
///          right-hand operand into a register and emits the register-register
///          encoding.  The routine is shared by many arithmetic lowerings to
///          keep operand-handling logic consistent.
/// @param instr IL instruction currently being lowered.
/// @param opcRR Opcode used when both operands reside in registers.
/// @param opcRI Opcode used when the right operand fits the immediate form.
/// @param cls Register class expected by the instruction.
/// @param requireImm32 When true, immediates must fit in 32 bits to use @p opcRI.
void EmitCommon::emitBinary(
    const ILInstr &instr, MOpcode opcRR, MOpcode opcRI, RegClass cls, bool requireImm32) {
    if (instr.resultId < 0 || instr.ops.size() < 2) {
        phaseAUnsupported("binary op: missing operands");
    }

    const VReg destReg = builder().ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);
    const Operand lhs = builder().makeOperandForValue(instr.ops[0], cls);
    Operand rhs = builder().makeOperandForValue(instr.ops[1], cls);

    if (destReg.cls != cls) {
        phaseAUnsupported("binary op result register class mismatch");
    }
    requireRegOrImmForClass(lhs, cls, "binary op lhs register class mismatch");
    requireRegOrImmForClass(rhs, cls, "binary op rhs register class mismatch");

    if (std::holds_alternative<OpImm>(lhs)) {
        if (cls == RegClass::XMM) {
            // XMM registers cannot be loaded directly from immediates.
            // Move the bit-pattern to a GPR first, then transfer to XMM unchanged.
            const VReg gprTmp = builder().makeTempVReg(RegClass::GPR);
            const Operand gprOp = makeVRegOperand(gprTmp.cls, gprTmp.id);
            builder().append(MInstr::make(MOpcode::MOVri, std::vector<Operand>{clone(gprOp), lhs}));
            builder().append(
                MInstr::make(MOpcode::MOVQrx, std::vector<Operand>{clone(dest), clone(gprOp)}));
        } else {
            builder().append(MInstr::make(MOpcode::MOVri, std::vector<Operand>{clone(dest), lhs}));
        }
    } else {
        if (cls == RegClass::XMM) {
            builder().append(
                MInstr::make(MOpcode::MOVSDrr, std::vector<Operand>{clone(dest), lhs}));
        } else {
            builder().append(MInstr::make(MOpcode::MOVrr, std::vector<Operand>{clone(dest), lhs}));
        }
    }

    /// @brief Tests whether the right operand can use the immediate opcode form.
    /// @return `true` when an immediate form exists and its value is encodable.
    const bool canUseImm = [&]() {
        if (opcRI == opcRR) {
            return false;
        }
        const auto *imm = std::get_if<OpImm>(&rhs);
        if (!imm) {
            return false;
        }
        if (!requireImm32) {
            return true;
        }
        return fitsImm32(imm->val);
    }();

    if (canUseImm) {
        builder().append(MInstr::make(opcRI, std::vector<Operand>{clone(dest), rhs}));
        return;
    }

    const Operand rhsReg = materialise(std::move(rhs), cls);
    builder().append(MInstr::make(opcRR, std::vector<Operand>{clone(dest), clone(rhsReg)}));
}

/// @brief Emit a shift instruction choosing between immediate and register forms.
/// @details Moves the left operand into the destination register, then inspects
///          the shift amount.  Literal counts are masked to the architectural
///          width and use the immediate opcode, while variable counts are
///          materialised into RCX (the x86 shift-count register) before emitting
///          the register form.  The helper therefore encapsulates the subtle
///          register conventions of x86 shifts.
/// @param instr IL instruction being lowered.
/// @param opcImm Opcode for the immediate shift form.
/// @param opcReg Opcode for the register-controlled shift form.
void EmitCommon::emitShift(const ILInstr &instr, MOpcode opcImm, MOpcode opcReg) {
    if (instr.resultId < 0 || instr.ops.size() < 2) {
        phaseAUnsupported("shift: missing operands");
    }
    if (!isIntegerLikeKind(instr.resultKind) || !isIntegerLikeKind(instr.ops[0].kind) ||
        !isIntegerLikeKind(instr.ops[1].kind)) {
        phaseAUnsupported("shift: expected integer-like operands");
    }

    const VReg destReg = builder().ensureVReg(instr.resultId, instr.resultKind);
    if (destReg.cls != RegClass::GPR) {
        phaseAUnsupported("shift: destination must be a GPR");
    }
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);
    const Operand lhs = builder().makeOperandForValue(instr.ops[0], RegClass::GPR);
    requireRegOrImmForClass(lhs, RegClass::GPR, "shift lhs register class mismatch");

    if (std::holds_alternative<OpImm>(lhs)) {
        builder().append(MInstr::make(MOpcode::MOVri, std::vector<Operand>{clone(dest), lhs}));
    } else {
        builder().append(MInstr::make(MOpcode::MOVrr, std::vector<Operand>{clone(dest), lhs}));
    }

    Operand rhs = builder().makeOperandForValue(instr.ops[1], RegClass::GPR);
    requireRegOrImmForClass(rhs, RegClass::GPR, "shift rhs register class mismatch");
    if (auto *imm = std::get_if<OpImm>(&rhs)) {
        const auto masked = static_cast<int64_t>(static_cast<std::uint64_t>(imm->val) & 63ULL);
        builder().append(
            MInstr::make(opcImm, std::vector<Operand>{clone(dest), makeImmOperand(masked)}));
        return;
    }

    const Operand clOperand =
        makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RCX));

    bool alreadyCl = false;
    if (const auto *reg = std::get_if<OpReg>(&rhs); reg) {
        alreadyCl = reg->isPhys && reg->cls == RegClass::GPR &&
                    reg->idOrPhys == static_cast<uint16_t>(PhysReg::RCX);
    }

    if (!alreadyCl) {
        builder().append(
            MInstr::make(MOpcode::MOVrr, std::vector<Operand>{clone(clOperand), clone(rhs)}));
    }

    builder().append(MInstr::make(opcReg, std::vector<Operand>{clone(dest), clone(clOperand)}));
}

/// @brief Emit a compare instruction and optional SETcc materialisation.
/// @details Evaluates optional condition-code operands, performs the register or
///          floating-point comparison, and when the IL instruction produces a
///          result emits a SETcc pseudo with the resolved condition code.  The
///          helper shields callers from the boilerplate associated with IL
///          comparisons that may or may not capture their results.
/// @param instr IL compare instruction.
/// @param cls Register class required for the compare operands.
/// @param defaultCond Condition code used when the IL instruction omits an override.
void EmitCommon::emitCmp(const ILInstr &instr, RegClass cls, int defaultCond) {
    if (instr.ops.size() < 2) {
        phaseAUnsupported("compare: missing operands");
    }

    int condCode = defaultCond;
    Operand condOperand{};
    if (instr.ops.size() > 2) {
        condOperand = builder().makeOperandForValue(instr.ops[2], RegClass::GPR);
        if (const auto *imm = std::get_if<OpImm>(&condOperand)) {
            condCode = static_cast<int>(imm->val);
        }
    }

    Operand lhs = builder().makeOperandForValue(instr.ops[0], cls);
    Operand rhs = builder().makeOperandForValue(instr.ops[1], cls);
    requireRegOrImmForClass(lhs, cls, "compare lhs register class mismatch");
    requireRegOrImmForClass(rhs, cls, "compare rhs register class mismatch");

    // x86 CMP requires the first operand to be a register, not an immediate.
    // If LHS is an immediate, materialize it to a temporary register first.
    if (std::holds_alternative<OpImm>(lhs)) {
        const VReg tmp = builder().makeTempVReg(RegClass::GPR);
        const Operand tmpOp = makeVRegOperand(tmp.cls, tmp.id);
        builder().append(MInstr::make(MOpcode::MOVri, std::vector<Operand>{tmpOp, lhs}));
        lhs = tmpOp;
    }

    // If RHS is an immediate that doesn't fit in the sign-extended imm32
    // encoding, materialise it into a register so the assembler doesn't reject
    // the instruction.
    if (cls == RegClass::GPR) {
        if (const auto *imm = std::get_if<OpImm>(&rhs); imm && !fitsImm32(imm->val)) {
            rhs = materialiseGpr(std::move(rhs));
        }
    }

    MOpcode cmpOpc;
    if (cls == RegClass::XMM) {
        cmpOpc = MOpcode::UCOMIS;
    } else if (std::holds_alternative<OpImm>(rhs)) {
        cmpOpc = MOpcode::CMPri;
    } else {
        cmpOpc = MOpcode::CMPrr;
    }
    builder().append(MInstr::make(cmpOpc, std::vector<Operand>{clone(lhs), rhs}));

    if (instr.resultId < 0) {
        return;
    }

    const VReg destReg = builder().ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);
    builder().append(
        MInstr::make(MOpcode::SETcc, std::vector<Operand>{makeImmOperand(condCode), dest}));
    builder().append(
        MInstr::make(MOpcode::MOVZXrr8, std::vector<Operand>{clone(dest), clone(dest)}));
}

/// @brief Emit the Machine IR sequence that implements an IL select.
/// @details Handles both integer and floating-point selects by materialising the
///          false branch, conditionally overwriting it with the true branch, and
///          emitting an explicit select pseudo for ISel to expand. For integer
///          selects immediates are first moved into temporaries so conditional
///          moves have register operands.
/// @param instr IL select instruction containing condition, true, and false operands.
void EmitCommon::emitSelect(const ILInstr &instr) {
    if (instr.resultId < 0 || instr.ops.size() < 3) {
        phaseAUnsupported("select: missing operands");
    }

    const VReg destReg = builder().ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);
    requireIntegerLike(instr.ops[0], "select: condition must be integer-like");
    // TESTrr requires register operands — materialise the condition if it is
    // an immediate (constant-folded select condition).
    const Operand cond = materialiseGpr(builder().makeOperandForValue(instr.ops[0], RegClass::GPR));
    const Operand trueVal = builder().makeOperandForValue(instr.ops[1], destReg.cls);
    const Operand falseVal = builder().makeOperandForValue(instr.ops[2], destReg.cls);

    if (destReg.cls == RegClass::GPR) {
        /// @brief Preserves legal register/immediate arms and materializes other GPR values.
        /// @param operand Select arm to legalize.
        /// @return Register or immediate operand suitable for integer selection.
        auto materialiseGprSelectValue = [&](Operand operand) -> Operand {
            if (std::holds_alternative<OpReg>(operand) || std::holds_alternative<OpImm>(operand)) {
                return operand;
            }
            return materialise(std::move(operand), RegClass::GPR);
        };

        const Operand falseArm = materialiseGprSelectValue(clone(falseVal));
        Operand cmovSource = materialiseGprSelectValue(clone(trueVal));
        if (std::holds_alternative<OpImm>(cmovSource)) {
            const VReg tmpVReg = builder().makeTempVReg(destReg.cls);
            cmovSource = makeVRegOperand(tmpVReg.cls, tmpVReg.id);
            builder().append(MInstr::make(MOpcode::MOVri,
                                          std::vector<Operand>{clone(cmovSource), clone(trueVal)}));
        }

        builder().append(MInstr::make(
            MOpcode::SELECT_GPR,
            std::vector<Operand>{clone(dest), clone(cond), clone(falseArm), clone(cmovSource)}));
        return;
    }

    // Materialise FP operands into XMM vregs — MOVSDrr requires registers,
    // and the ISel lowerXmmSelect pattern requires OpReg for both values.
    const Operand matFalse = materialise(clone(falseVal), RegClass::XMM);
    const Operand matTrue = materialise(clone(trueVal), RegClass::XMM);

    builder().append(MInstr::make(
        MOpcode::SELECT_XMM,
        std::vector<Operand>{clone(dest), clone(cond), clone(matFalse), clone(matTrue)}));
}

/// @brief Emit an unconditional branch to the target label.
/// @details Extracts the label operand from the IL instruction and appends a
///          Machine IR JMP instruction pointing to the resolved block label.
/// @param instr IL branch instruction providing the label operand.
void EmitCommon::emitBranch(const ILInstr &instr) {
    if (instr.ops.empty()) {
        phaseAUnsupported("branch: missing target label");
    }
    requireLabel(instr.ops[0], "branch: target must be a label");
    builder().append(
        MInstr::make(MOpcode::JMP, std::vector<Operand>{builder().makeLabelOperand(instr.ops[0])}));
}

/// @brief Emit a conditional branch that tests the provided operand.
/// @details Lowers to a TEST/JCC/JMP sequence that mirrors the IL conditional
///          branch semantics.  The helper prepares both the taken and fallthrough
///          labels so control flow mirrors the IL structure.
/// @param instr IL conditional branch instruction (cond, true, false operands).
void EmitCommon::emitCondBranch(const ILInstr &instr) {
    if (instr.ops.size() < 3) {
        phaseAUnsupported("cond branch: missing operands");
    }

    requireIntegerLike(instr.ops[0], "cond branch: condition must be integer-like");
    requireLabel(instr.ops[1], "cond branch: true target must be a label");
    requireLabel(instr.ops[2], "cond branch: false target must be a label");

    // TESTrr requires register operands — materialise the condition if it is
    // an immediate (constant-folded branch condition).
    const Operand cond = materialiseGpr(builder().makeOperandForValue(instr.ops[0], RegClass::GPR));
    const Operand trueLabel = builder().makeLabelOperand(instr.ops[1]);
    const Operand falseLabel = builder().makeLabelOperand(instr.ops[2]);

    builder().append(MInstr::make(MOpcode::TESTrr, std::vector<Operand>{clone(cond), cond}));
    builder().append(
        MInstr::make(MOpcode::JCC, std::vector<Operand>{makeImmOperand(1), trueLabel}));
    builder().append(MInstr::make(MOpcode::JMP, std::vector<Operand>{falseLabel}));
}

/// @brief Emit the return sequence for an IL return instruction.
/// @details Handles empty returns by materialising a zero process/function
///          return in the integer ABI register.  When a value is returned, the
///          helper materialises the operand, performs sign or zero extension for
///          boolean returns, moves the value into the appropriate ABI register,
///          and finally emits RET.  This centralises ABI-specific logic so
///          callers remain simple.
/// @param instr IL return instruction.
void EmitCommon::emitReturn(const ILInstr &instr) {
    if (instr.ops.empty()) {
        const Operand retReg = makePhysRegOperand(
            RegClass::GPR, static_cast<uint16_t>(builder().target().intReturnReg));
        builder().append(
            MInstr::make(MOpcode::XORrr32, std::vector<Operand>{clone(retReg), clone(retReg)}));
        builder().append(MInstr::make(MOpcode::RET, {}));
        return;
    }

    const ILValue &retVal = instr.ops.front();
    const RegClass cls = builder().regClassFor(retVal.kind);

    Operand src = builder().makeOperandForValue(retVal, cls);

    if (retVal.kind == ILValue::Kind::I1) {
        if (const auto *imm = std::get_if<OpImm>(&src)) {
            src = makeImmOperand(imm->val != 0 ? 1 : 0);
        }
    }

    Operand srcReg = materialise(std::move(src), cls);

    if (retVal.kind == ILValue::Kind::I1 && std::holds_alternative<OpReg>(srcReg)) {
        const auto &reg = std::get<OpReg>(srcReg);
        if (!reg.isPhys) {
            const VReg zx = builder().makeTempVReg(RegClass::GPR);
            const Operand zxOp = makeVRegOperand(zx.cls, zx.id);
            builder().append(
                MInstr::make(MOpcode::MOVZXrr8, std::vector<Operand>{clone(zxOp), clone(srcReg)}));
            srcReg = zxOp;
        }
    }

    if (cls == RegClass::XMM) {
        const Operand retReg = makePhysRegOperand(
            RegClass::XMM, static_cast<uint16_t>(builder().target().f64ReturnReg));
        builder().append(
            MInstr::make(MOpcode::MOVSDrr, std::vector<Operand>{retReg, clone(srcReg)}));
    } else {
        const Operand retReg = makePhysRegOperand(
            RegClass::GPR, static_cast<uint16_t>(builder().target().intReturnReg));
        builder().append(MInstr::make(MOpcode::MOVrr, std::vector<Operand>{retReg, clone(srcReg)}));
    }

    builder().append(MInstr::make(MOpcode::RET, {}));
}

/// @brief Emit a load from memory into a virtual register.
/// @details Accepts a base register and optional displacement, verifies that the
///          base is addressable, and then emits either a MOV or MOVSD load based
///          on the requested register class.  The helper ensures the destination
///          virtual register exists before appending the instruction.
/// @param instr IL load instruction describing the address and displacement.
/// @param cls Register class expected for the loaded value.
void EmitCommon::emitLoad(const ILInstr &instr, RegClass cls) {
    if (instr.resultId < 0 || instr.ops.empty()) {
        phaseAUnsupported("load: missing operands");
    }

    Operand baseOp = materialiseGpr(builder().makeOperandForValue(instr.ops[0], RegClass::GPR));
    const auto *baseReg = std::get_if<OpReg>(&baseOp);
    if (!baseReg) {
        phaseAUnsupported("load: address base did not materialize to a register");
    }

    int64_t disp64 = 0;
    if (instr.ops.size() > 1) {
        if (!isIntegerLikeImmediate(builder(), instr.ops[1])) {
            phaseAUnsupported("load: displacement must be an integer immediate");
        }
        disp64 = integerImmediateValue(instr.ops[1]);
    }
    const VReg destReg = builder().ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    Operand effectiveBase = materialiseDisplacement(baseOp, disp64);

    Operand mem = makeMemOperand(std::get<OpReg>(effectiveBase), static_cast<int32_t>(disp64));
    if (fitsImm32(disp64)) {
        if (const auto indexed = tryMakeIndexedMem(instr, 1)) {
            mem = *indexed;
        }
    }

    if (cls == RegClass::GPR) {
        builder().append(MInstr::make(MOpcode::MOVmr, std::vector<Operand>{clone(dest), mem}));
    } else {
        builder().append(MInstr::make(MOpcode::MOVSDmr, std::vector<Operand>{clone(dest), mem}));
    }

    if (instr.resultKind == ILValue::Kind::STR &&
        !builder().lower().isStrLoadRetainElidable(instr.resultId))
        emitRetainStringVReg(builder(), destReg);
}

/// @brief Emit a store from a value operand into memory.
/// @details Resolves the address operands, ensures the base is a register, and
///          emits the appropriate MOV variant depending on whether the value is
///          an integer, floating-point, register, or immediate.  The helper
///          therefore abstracts the heterogeneity of IL store operands.
/// @param instr IL store instruction.
/// @note IL store format is: store type, addr, value
///       So ops[0] is the address and ops[1] is the value to store.
void EmitCommon::emitStore(const ILInstr &instr) {
    if (instr.ops.size() < 2) {
        phaseAUnsupported("store: missing operands");
    }

    // IL store format: store type, addr, value
    // ops[0] = address (Ptr type)
    // ops[1] = value to store (InstrType)
    Operand baseOp = materialiseGpr(builder().makeOperandForValue(instr.ops[0], RegClass::GPR));
    const RegClass valueCls = builder().regClassFor(instr.ops[1].kind);
    Operand value = builder().makeOperandForValue(instr.ops[1], valueCls);
    if (!std::holds_alternative<OpReg>(value) && !std::holds_alternative<OpImm>(value)) {
        value = materialise(std::move(value), valueCls);
    }
    const auto *baseReg = std::get_if<OpReg>(&baseOp);
    if (!baseReg) {
        phaseAUnsupported("store: address base did not materialize to a register");
    }
    int64_t disp64 = 0;
    if (instr.ops.size() > 2) {
        if (!isIntegerLikeImmediate(builder(), instr.ops[2])) {
            phaseAUnsupported("store: displacement must be an integer immediate");
        }
        disp64 = integerImmediateValue(instr.ops[2]);
    }

    Operand effectiveBase = materialiseDisplacement(baseOp, disp64);

    Operand mem = makeMemOperand(std::get<OpReg>(effectiveBase), static_cast<int32_t>(disp64));
    if (fitsImm32(disp64)) {
        if (const auto indexed = tryMakeIndexedMem(instr, 2)) {
            mem = *indexed;
        }
    }

    if (std::holds_alternative<OpReg>(value)) {
        const auto cls = std::get<OpReg>(value).cls;
        if (cls == RegClass::XMM) {
            builder().append(MInstr::make(MOpcode::MOVSDrm, std::vector<Operand>{mem, value}));
        } else {
            builder().append(MInstr::make(MOpcode::MOVrm, std::vector<Operand>{mem, value}));
        }
    } else {
        // For immediate-to-memory stores, we must go through a temp register
        // because x86-64 can't move a 64-bit immediate directly to memory.
        const VReg tmp = builder().makeTempVReg(RegClass::GPR);
        const Operand tmpOp = makeVRegOperand(tmp.cls, tmp.id);
        builder().append(MInstr::make(MOpcode::MOVri, std::vector<Operand>{tmpOp, value}));
        builder().append(MInstr::make(MOpcode::MOVrm, std::vector<Operand>{mem, tmpOp}));
    }
}

/// @brief Emit a cast or move between register classes.
/// @details Creates the destination virtual register and either forwards MOV
///          when the operation is a pure copy, or appends the supplied opcode to
///          perform the conversion.  Immediates are copied using MOV to ensure a
///          register destination is produced.
/// @param instr IL cast instruction.
/// @param opc Machine opcode implementing the conversion.
/// @param dstCls Destination register class (unused but recorded for clarity).
/// @param srcCls Source register class for operand materialisation.
void EmitCommon::emitCast(const ILInstr &instr, MOpcode opc, RegClass dstCls, RegClass srcCls) {
    if (instr.resultId < 0 || instr.ops.empty()) {
        phaseAUnsupported("cast: missing operands");
    }

    const Operand src = builder().makeOperandForValue(instr.ops[0], srcCls);
    const VReg destReg = builder().ensureVReg(instr.resultId, instr.resultKind);
    if (destReg.cls != dstCls) {
        phaseAUnsupported("cast: destination register class mismatch");
    }
    requireRegOrImmForClass(src, srcCls, "cast source register class mismatch");
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    if (std::holds_alternative<OpImm>(src)) {
        if (destReg.cls == RegClass::XMM) {
            // For XMM destination with immediate source, we need to go through a GPR.
            // First move the immediate to a GPR temp, then apply the conversion opcode.
            const VReg gprTmp = builder().makeTempVReg(RegClass::GPR);
            const Operand gprOp = makeVRegOperand(gprTmp.cls, gprTmp.id);
            builder().append(MInstr::make(MOpcode::MOVri, std::vector<Operand>{clone(gprOp), src}));
            builder().append(MInstr::make(opc, std::vector<Operand>{clone(dest), clone(gprOp)}));
        } else {
            builder().append(MInstr::make(MOpcode::MOVri, std::vector<Operand>{clone(dest), src}));
        }
    } else if (opc == MOpcode::MOVrr) {
        builder().append(MInstr::make(MOpcode::MOVrr, std::vector<Operand>{clone(dest), src}));
    } else {
        builder().append(MInstr::make(opc, std::vector<Operand>{clone(dest), src}));
    }
}

/// @brief Emit a division or remainder pseudo instruction.
/// @details Materialises both dividend and divisor into GPRs, selecting the
///          appropriate pseudo opcode based on the mnemonic string.  The helper
///          emits a three-operand instruction capturing destination, dividend,
///          and divisor, leaving it to later passes to expand into concrete
///          machine instructions.
/// @param instr IL div/rem instruction with dividend and divisor operands.
/// @param opcode Textual opcode (e.g. "div", "srem") used to select the pseudo.
void EmitCommon::emitDivRem(const ILInstr &instr, std::string_view opcode) {
    if (instr.resultId < 0 || instr.ops.size() < 2) {
        phaseAUnsupported("div/rem: missing operands");
    }

    const VReg destReg = builder().ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    Operand dividend = builder().makeOperandForValue(instr.ops[0], RegClass::GPR);
    Operand divisor = builder().makeOperandForValue(instr.ops[1], RegClass::GPR);

    if (!std::holds_alternative<OpReg>(dividend) && !std::holds_alternative<OpImm>(dividend)) {
        dividend = materialiseGpr(dividend);
    }

    divisor = materialiseGpr(divisor);

    /// @brief Selects the machine pseudo corresponding to the requested div/rem opcode.
    /// @return Checked or unchecked signed/unsigned division or remainder pseudo.
    const MOpcode pseudo = [&]() {
        if (opcode == "sdiv") {
            return MOpcode::DIVS64rr;
        }
        if (opcode == "div" || opcode == "sdiv.chk0") {
            return MOpcode::DIVS64Chk0rr;
        }
        if (opcode == "srem") {
            return MOpcode::REMS64rr;
        }
        if (opcode == "rem" || opcode == "srem.chk0") {
            return MOpcode::REMS64Chk0rr;
        }
        if (opcode == "udiv") {
            return MOpcode::DIVU64rr;
        }
        if (opcode == "udiv.chk0") {
            return MOpcode::DIVU64Chk0rr;
        }
        if (opcode == "urem.chk0") {
            return MOpcode::REMU64Chk0rr;
        }
        return MOpcode::REMU64rr;
    }();

    builder().append(
        MInstr::make(pseudo, std::vector<Operand>{clone(dest), clone(dividend), clone(divisor)}));
}

/// @brief Map an integer compare opcode string to a condition code.
/// @details Recognises the canonical `icmp_*` prefixes and converts the suffix
///          into the SETcc encoding expected by Machine IR.  Unrecognised
///          strings yield @c std::nullopt so callers can detect missing support.
/// @param opcode IL opcode string to translate.
/// @return Condition code index or empty optional when unsupported.
std::optional<int> EmitCommon::icmpConditionCode(std::string_view opcode) noexcept {
    if (!opcode.starts_with("icmp_")) {
        return std::nullopt;
    }

    /// Static lookup table mapping icmp suffix strings to SETcc condition codes.
    /// Order: eq=0, ne=1, slt=2, sle=3, sgt=4, sge=5, ugt=6, uge=7, ult=8, ule=9.
    struct Entry {
        std::string_view suffix{}; ///< Opcode suffix without the `icmp_` prefix.
        int code{0};               ///< SETcc condition-code index for the suffix.
    };

    static constexpr Entry kTable[] = {
        {"eq", 0},
        {"ne", 1},
        {"slt", 2},
        {"sle", 3},
        {"sgt", 4},
        {"sge", 5},
        {"ugt", 6},
        {"uge", 7},
        {"ult", 8},
        {"ule", 9},
    };

    const std::string_view suffix = opcode.substr(5);
    for (const auto &entry : kTable) {
        if (entry.suffix == suffix) {
            return entry.code;
        }
    }
    return std::nullopt;
}

/// @brief Emit a NaN-safe floating-point comparison sequence.
/// @details After UCOMISD with NaN operands, x86 sets ZF=1, PF=1, CF=1.
///          This makes simple SETcc incorrect for eq/ne/lt/le:
///          - eq (SETE): NaN sets ZF=1 → wrongly true.  Fix: SETE ∧ SETNP.
///          - ne (SETNE): NaN sets ZF=1 → wrongly false.  Fix: SETNE ∨ SETP.
///          - lt (SETB): NaN sets CF=1 → wrongly true.  Fix: swap operands + SETA.
///          - le (SETBE): NaN sets CF=1|ZF=1 → wrongly true.  Fix: swap + SETAE.
///
///          For lt/le the fix swaps UCOMISD operands so a<b becomes UCOMISD(b,a)
///          which yields CF=0,ZF=0 → SETA=true; NaN still yields CF=1,ZF=1 →
///          SETA=false. For eq/ne a two-SETcc compound with AND/OR is required.
/// @param instr IL fcmp instruction with at least 2 F64 operands.
/// @param suffix The comparison suffix ("eq", "ne", "lt", or "le").
void EmitCommon::emitFCmpNanSafe(const ILInstr &instr, std::string_view suffix) {
    if (instr.ops.size() < 2 || instr.resultId < 0) {
        phaseAUnsupported("fcmp: missing operands");
    }

    Operand lhs = builder().makeOperandForValue(instr.ops[0], RegClass::XMM);
    Operand rhs = builder().makeOperandForValue(instr.ops[1], RegClass::XMM);
    requireRegisterForClass(lhs, RegClass::XMM, "fcmp lhs register class mismatch");
    requireRegisterForClass(rhs, RegClass::XMM, "fcmp rhs register class mismatch");

    const VReg destReg = builder().ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);
    /// @brief Emits SETcc followed by zero-extension to a canonical integer boolean.
    /// @param code Machine condition-code encoding.
    /// @param target Boolean destination operand.
    auto emitSetccBool = [&](int code, const Operand &target) {
        builder().append(MInstr::make(MOpcode::SETcc,
                                      std::vector<Operand>{makeImmOperand(code), clone(target)}));
        builder().append(
            MInstr::make(MOpcode::MOVZXrr8, std::vector<Operand>{clone(target), clone(target)}));
    };

    if (suffix == "lt" || suffix == "le") {
        // Swap operands: UCOMISD(rhs, lhs) so that a<b → SETA, a<=b → SETAE.
        // NaN gives CF=1,ZF=1 regardless of order → SETA/SETAE → false.
        builder().append(MInstr::make(MOpcode::UCOMIS, std::vector<Operand>{clone(rhs), lhs}));
        const int code = (suffix == "lt") ? 6 : 7; // SETA / SETAE
        emitSetccBool(code, dest);
        return;
    }

    // eq / ne: emit UCOMISD(lhs, rhs) then two SETcc + logical combine.
    builder().append(MInstr::make(MOpcode::UCOMIS, std::vector<Operand>{clone(lhs), rhs}));

    if (suffix == "eq") {
        // Ordered equal: (ZF=1) AND (PF=0) → SETE ∧ SETNP.
        const VReg tmp = builder().makeTempVReg(RegClass::GPR);
        const Operand tmpOp = makeVRegOperand(tmp.cls, tmp.id);
        emitSetccBool(11, tmpOp); // SETNP
        emitSetccBool(0, dest);   // SETE
        builder().append(MInstr::make(MOpcode::ANDrr, std::vector<Operand>{dest, tmpOp}));
    } else // "ne"
    {
        // Unordered not-equal: (ZF=0) OR (PF=1) → SETNE ∨ SETP.
        const VReg tmp = builder().makeTempVReg(RegClass::GPR);
        const Operand tmpOp = makeVRegOperand(tmp.cls, tmp.id);
        emitSetccBool(10, tmpOp); // SETP
        emitSetccBool(1, dest);   // SETNE
        builder().append(MInstr::make(MOpcode::ORrr, std::vector<Operand>{dest, tmpOp}));
    }
}

/// @brief Map a floating-point compare opcode string to a condition code.
/// @details Similar to @ref icmpConditionCode but handles the `fcmp_*` family of
///          opcodes, returning the encoding index consumed by SETcc expansion.
/// @param opcode IL opcode string to translate.
/// @return Condition code index or empty optional when unsupported.
std::optional<int> EmitCommon::fcmpConditionCode(std::string_view opcode) noexcept {
    if (!opcode.starts_with("fcmp_")) {
        return std::nullopt;
    }

    /// Static lookup table mapping fcmp suffix strings to SETcc condition codes.
    /// Uses unsigned condition codes because UCOMISD clears SF/OF:
    /// lt→8 ("b"/below), le→9 ("be"/below-or-equal), gt→6 ("a"/above), ge→7 ("ae"/above-or-equal).
    struct Entry {
        std::string_view suffix{}; ///< Opcode suffix without the `fcmp_` prefix.
        int code{0};               ///< SETcc condition-code index for the suffix.
    };

    static constexpr Entry kTable[] = {
        {"eq", 0},
        {"ne", 1},
        {"lt", 8},
        {"le", 9},
        {"gt", 6},
        {"ge", 7},
        {"ord", 11}, // NP — no parity (neither operand is NaN)
        {"uno", 10}, // P  — parity (at least one operand is NaN)
    };

    const std::string_view suffix = opcode.substr(5);
    for (const auto &entry : kTable) {
        if (entry.suffix == suffix) {
            return entry.code;
        }
    }
    return std::nullopt;
}

} // namespace zanna::codegen::x64
