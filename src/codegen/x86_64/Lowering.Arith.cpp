//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Lowering.Arith.cpp
// Purpose: Implement arithmetic opcode lowering rules for the x86-64 backend.
//          Arithmetic emitters delegate common mechanics to EmitCommon, keeping
//          each rule focused on opcode selection.
// Key invariants:
//   - All emitters honour the register classes reported by the MIRBuilder.
//   - Malformed or unsupported instruction shapes produce a backend diagnostic.
// Ownership/Lifetime:
//   - Operates on borrowed IL instructions and MIR builders; no persistent state.
// Links: src/codegen/x86_64/LoweringRules.hpp,
//        src/codegen/x86_64/Lowering.EmitCommon.hpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#include "LoweringRuleTable.hpp"

#include "LowerILToMIR.hpp"
#include "Lowering.EmitCommon.hpp"
#include "Unsupported.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

/**
 * @file
 * @brief Implements x86-64 arithmetic, comparison, and numeric-cast lowering rules.
 *
 * Besides basic scalar arithmetic, this unit emits checked narrow arithmetic,
 * div/rem pseudos, NaN-aware comparisons, integer width normalization, and
 * signed/unsigned conversions with explicit range, rounding, and runtime trap
 * paths where required by IL semantics.
 */

namespace zanna::codegen::x64::lowering {
namespace {

/// Runtime error code for a NaN or otherwise invalid numeric cast.
constexpr int64_t kErrInvalidCast = 5;
/// Runtime error code for a numeric range overflow.
constexpr int64_t kErrOverflow = 4;

/// @brief Encode 2^@p exponent as the 64-bit pattern of a positive IEEE-754 double.
/// @details Computes the biased exponent (1023 + @p exponent) and packs it into
///          bits 52–62; mantissa is zero. Branch-free, constexpr-evaluable, and
///          covers all the bounds we need without per-width switch tables.
/// @param exponent Unbiased power-of-two exponent.
/// @return IEEE-754 binary64 bit pattern for the exact power of two.
[[nodiscard]] constexpr uint64_t f64BitsForPowerOfTwo(int exponent) noexcept {
    return static_cast<uint64_t>(1023 + exponent) << 52;
}

/// @brief Return the IEEE-754 bit pattern of @c -2^(widthBits-1) as a double.
/// @details Lower (inclusive) bound for checked fp-to-signed-int. The sign bit
///          is or-ed in to negate the positive power-of-two encoding.
/// @param widthBits Target signed-integer width: 16, 32, or 64.
/// @return Exact binary64 lower-bound bit pattern.
/// @throws std::runtime_error Through @c phaseAUnsupported for another width.
uint64_t signedMinF64Bits(std::uint8_t widthBits) {
    if (widthBits != 16 && widthBits != 32 && widthBits != 64)
        phaseAUnsupported("checked fp-to-si cast: unsupported result width");
    constexpr uint64_t kSignBit = 0x8000000000000000ULL;
    return kSignBit | f64BitsForPowerOfTwo(widthBits - 1);
}

/// @brief Return the IEEE-754 bit pattern of @c 2^(widthBits-1) as a double.
/// @details The "upper exclusive" bound for checked fp-to-signed-int. The
///          input must be strictly less than this value to fit in the
///          signed range.
/// @param widthBits Target signed-integer width: 16, 32, or 64.
/// @return Exact binary64 exclusive upper-bound bit pattern.
/// @throws std::runtime_error Through @c phaseAUnsupported for another width.
uint64_t signedUpperExclusiveF64Bits(std::uint8_t widthBits) {
    if (widthBits != 16 && widthBits != 32 && widthBits != 64)
        phaseAUnsupported("checked fp-to-si cast: unsupported result width");
    return f64BitsForPowerOfTwo(widthBits - 1);
}

/// @brief Return the IEEE-754 bit pattern of @c 2^widthBits as a double.
/// @details Upper exclusive bound for checked fp-to-unsigned-int. For 64-bit
///          unsigned conversion this is 2^64, handled in @ref emitFpToUi via
///          the subtract-and-set-bit-63 trick because CVTTSD2SI is signed-only.
/// @param widthBits Target unsigned-integer width: 16, 32, or 64.
/// @return Exact binary64 exclusive upper-bound bit pattern.
/// @throws std::runtime_error Through @c phaseAUnsupported for another width.
uint64_t unsignedUpperExclusiveF64Bits(std::uint8_t widthBits) {
    if (widthBits != 16 && widthBits != 32 && widthBits != 64)
        phaseAUnsupported("checked fp-to-ui cast: unsupported result width");
    return f64BitsForPowerOfTwo(widthBits);
}

/// @brief True if @p kind is integer-like (I64/I1/PTR) — GPR-eligible.
/// @param kind IL kind to classify.
/// @return @c true for an integer, boolean, or pointer.
[[nodiscard]] bool isIntegerLikeKind(ILValue::Kind kind) noexcept {
    return kind == ILValue::Kind::I64 || kind == ILValue::Kind::I1 || kind == ILValue::Kind::PTR;
}

/// @brief True if @p kind is a scalar integer (I64/I1) — excludes PTR.
/// @param kind IL kind to classify.
/// @return @c true for an integer or boolean scalar.
[[nodiscard]] bool isIntegerScalarKind(ILValue::Kind kind) noexcept {
    return kind == ILValue::Kind::I64 || kind == ILValue::Kind::I1;
}

/// @brief Assert @p instr is a well-formed integer→F64 conversion.
/// @details Requires a result, at least one operand, an F64 result kind, and an
///          integer-scalar source; otherwise raises phaseAUnsupported(@p context).
/// @param instr Conversion instruction to validate.
/// @param context Diagnostic text used on failure.
/// @throws std::runtime_error Through @c phaseAUnsupported on a shape mismatch.
void requireIntToFpShape(const ILInstr &instr, const char *context) {
    if (instr.resultId < 0 || instr.ops.empty()) {
        phaseAUnsupported(context);
    }
    if (instr.resultKind != ILValue::Kind::F64) {
        phaseAUnsupported(context);
    }
    if (!isIntegerScalarKind(instr.ops.front().kind)) {
        phaseAUnsupported(context);
    }
}

/// @brief Assert @p instr is a well-formed F64→integer conversion.
/// @details Mirror of requireIntToFpShape(): requires an F64 source operand and
///          an integer-scalar result; otherwise raises phaseAUnsupported(@p context).
/// @param instr Conversion instruction to validate.
/// @param context Diagnostic text used on failure.
/// @throws std::runtime_error Through @c phaseAUnsupported on a shape mismatch.
void requireFpToIntShape(const ILInstr &instr, const char *context) {
    if (instr.resultId < 0 || instr.ops.empty()) {
        phaseAUnsupported(context);
    }
    if (instr.ops.front().kind != ILValue::Kind::F64) {
        phaseAUnsupported(context);
    }
    if (!isIntegerScalarKind(instr.resultKind)) {
        phaseAUnsupported(context);
    }
}

/// @brief Assert @p instr is a well-formed integer→integer narrowing cast.
/// @details Requires a result, at least one operand, and integer-scalar kinds on
///          both source and result; otherwise raises phaseAUnsupported(@p context).
/// @param instr Narrowing instruction to validate.
/// @param context Diagnostic text used on failure.
/// @throws std::runtime_error Through @c phaseAUnsupported on a shape mismatch.
void requireNarrowCastShape(const ILInstr &instr, const char *context) {
    if (instr.resultId < 0 || instr.ops.empty()) {
        phaseAUnsupported(context);
    }
    if (!isIntegerScalarKind(instr.ops.front().kind) || !isIntegerScalarKind(instr.resultKind)) {
        phaseAUnsupported(context);
    }
}

/// @brief Clamp a declared integer width to a supported value.
/// @details I1 collapses to 1 bit; 0 or >64 normalizes to 64. Used so mask
///          width math has a well-defined bit count for every IL integer.
/// @param bits Declared width.
/// @param kind Value kind that can force boolean width.
/// @return Normalized width in the inclusive range one through 64.
[[nodiscard]] std::uint8_t normalizedIntegerBits(std::uint8_t bits, ILValue::Kind kind) noexcept {
    if (kind == ILValue::Kind::I1) {
        return 1;
    }
    if (bits == 0 || bits > 64) {
        return 64;
    }
    return bits;
}

/// @brief Bitmask with the low @p bits set (all-ones for @p bits >= 64).
/// @param bits Number of low-order one bits.
/// @return Requested mask without shifting by the machine word size.
[[nodiscard]] uint64_t lowBitsMask(std::uint8_t bits) noexcept {
    if (bits >= 64) {
        return ~uint64_t{0};
    }
    return (uint64_t{1} << bits) - uint64_t{1};
}

/// @brief Mask @p dest down to its low @p widthBits bits in place.
/// @details No-op for widths >= 64. Emits an `AND dest, imm` using the imm32
///          form when the mask fits, otherwise materializes the 64-bit mask
///          into a register first. Used to keep narrow integer results
///          canonically zero-extended after wider arithmetic.
/// @param builder   MIR builder receiving the emitted instruction(s).
/// @param emit      EmitCommon façade (operand cloning / materialization).
/// @param dest      Destination operand to mask in place.
/// @param widthBits Target width in bits (<64 to have any effect).
void emitMaskToWidth(MIRBuilder &builder,
                     EmitCommon &emit,
                     const Operand &dest,
                     std::uint8_t widthBits) {
    if (widthBits >= 64) {
        return;
    }

    const uint64_t mask = lowBitsMask(widthBits);
    if (fitsImm32(static_cast<int64_t>(mask))) {
        builder.append(MInstr::make(
            MOpcode::ANDri, {emit.clone(dest), makeImmOperand(static_cast<int64_t>(mask))}));
        return;
    }

    const VReg maskReg = builder.makeTempVReg(RegClass::GPR);
    const Operand maskOp = makeVRegOperand(maskReg.cls, maskReg.id);
    builder.append(MInstr::make(MOpcode::MOVri,
                                {emit.clone(maskOp), makeImmOperand(static_cast<int64_t>(mask))}));
    builder.append(MInstr::make(MOpcode::ANDrr, {emit.clone(dest), maskOp}));
}

/// @brief Build a CALL instruction tagged with the supplied call-plan id.
/// @details The call plan is what frame lowering uses to materialise argument
///          shuffles, so we must stamp the id onto the synthesised call.
/// @param target Label operand naming the callee.
/// @param callPlanId Identifier returned by @c MIRBuilder::recordCallPlan.
/// @return CALL @c MInstr ready to be appended to a block.
MInstr makePlannedCall(Operand target, uint32_t callPlanId) {
    MInstr call = MInstr::make(MOpcode::CALL, std::vector<Operand>{std::move(target)});
    call.callPlanId = callPlanId;
    return call;
}

/// @brief Emit a call to the runtime trap that raises a typed error.
/// @details Builds a single-argument call plan (the @p errCode immediate
///          carried in the SysV first integer-arg register) and follows it
///          with a UD2 so optimisations cannot let control flow fall past
///          the trap.
/// @param builder Active MIR builder.
/// @param errCode One of the @c kErr* constants identifying the trap reason.
void emitTrapRaiseError(MIRBuilder &builder, int64_t errCode) {
    CallLoweringPlan plan{};
    plan.callee = "rt_trap_raise_error";
    plan.numNamedArgs = 1;
    plan.args.push_back(
        CallArg{.cls = CallArgClass::GPR, .vreg = 0, .isImm = true, .imm = errCode});
    const uint32_t callPlanId = builder.recordCallPlan(std::move(plan));
    builder.append(
        makePlannedCall(makeLabelOperand(std::string{"rt_trap_raise_error"}), callPlanId));
    builder.append(MInstr::make(MOpcode::UD2));
}

/// @brief Sign-extend @p src as if it were a @p widthBits-wide signed integer.
/// @details Emits the canonical "SHL k ; SAR k" sequence where @c k = 64 - widthBits.
///          For widths of 64 the value is returned unchanged. The narrowing
///          conversion preserves the sign bit and zero-extends nothing; it is
///          how the checked-cast paths detect lost information by comparing
///          the narrowed-then-widened result against the original.
/// @param builder Active MIR builder.
/// @param emit EmitCommon helper bound to @p builder.
/// @param src Source operand (any GPR-compatible operand kind).
/// @param widthBits Target signed bit width (1..64).
/// @return Virtual-register operand holding the sign-extended value.
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

/// @brief Zero-extend @p src as if it were a @p widthBits-wide unsigned integer.
/// @details Twin of @ref emitSignExtendedToWidth using SHL/SHR (logical
///          right shift) so the high bits are zeroed instead of replicated.
/// @param builder Active MIR builder.
/// @param emit EmitCommon helper bound to @p builder.
/// @param src Source operand (any GPR-compatible operand kind).
/// @param widthBits Target unsigned bit width (1..64).
/// @return Virtual-register operand holding the zero-extended value.
Operand emitZeroExtendedToWidth(MIRBuilder &builder,
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
    builder.append(MInstr::make(MOpcode::SHRri, {emit.clone(narrowed), makeImmOperand(shift)}));
    return narrowed;
}

/// @brief Trap when @p value cannot be represented in @p widthBits as a signed integer.
/// @details Used after a sub-64-bit checked add/sub/mul to validate that the
///          full 64-bit result still equals its sign-extended narrowing. If
///          they differ, the high bits carried information that the target
///          width cannot hold, so we call @c rt_trap_raise_error with the
///          overflow code. The control flow is structured as
///          trap-or-fallthrough so the optimiser cannot reorder it.
/// @param builder Active MIR builder.
/// @param emit EmitCommon helper bound to @p builder.
/// @param value Operand holding the candidate result.
/// @param widthBits Target width (no-op when >= 64).
/// @param prefix String prefix used to derive unique trap/done labels.
void emitSubWidthOverflowCheck(MIRBuilder &builder,
                               EmitCommon &emit,
                               const Operand &value,
                               std::uint8_t widthBits,
                               const std::string &prefix) {
    if (widthBits >= 64) {
        return;
    }

    const uint32_t labelId = builder.lower().nextLocalLabelId();
    const std::string trapLabel = prefix + "_trap_" + std::to_string(labelId);
    const std::string doneLabel = prefix + "_done_" + std::to_string(labelId);
    Operand narrowed = emitSignExtendedToWidth(builder, emit, emit.clone(value), widthBits);
    builder.append(MInstr::make(MOpcode::CMPrr, {emit.clone(narrowed), emit.clone(value)}));
    builder.append(MInstr::make(MOpcode::JCC, {makeImmOperand(1), makeLabelOperand(trapLabel)}));
    builder.append(MInstr::make(MOpcode::JMP, {makeLabelOperand(doneLabel)}));
    builder.append(MInstr::make(MOpcode::LABEL, {makeLabelOperand(trapLabel)}));
    emitTrapRaiseError(builder, kErrOverflow);
    builder.append(MInstr::make(MOpcode::LABEL, {makeLabelOperand(doneLabel)}));
}

/// @brief Lower a sub-width overflow-checked add/sub/mul via native narrow flags.
/// @details A 32-bit or 16-bit ALU op sets OF at the target width directly, so
///          the widen-compute-narrow-compare dance is unnecessary: compute at
///          the narrow width (input high bits are ignored), branch to the
///          overflow trap on OF, and restore the canonical sign-extended
///          64-bit form with the matching MOVSX. When the right operand is an
///          in-range immediate and an immediate opcode is available, it is
///          used directly.
/// @param instr IL checked-arithmetic instruction (resultBits 32 or 16).
/// @param builder Active MIR builder.
/// @param emit EmitCommon helper bound to @p builder.
/// @param rrOpc Narrow reg-reg opcode (ADDrr32/SUBrr32/IMULrr32/ADDrr16/...).
/// @param riOpc Narrow reg-imm opcode, or MOpcode::LABEL as "none" sentinel.
/// @param movsxOpc Sign-extending re-widen opcode (MOVSXD or MOVSXrr16).
/// @param immMin Lowest immediate the reg-imm form encodes.
/// @param immMax Highest immediate the reg-imm form encodes.
/// @param prefix Label prefix for the trap/done labels.
void emitNarrowCheckedOp(const ILInstr &instr,
                         MIRBuilder &builder,
                         EmitCommon &emit,
                         MOpcode rrOpc,
                         MOpcode riOpc,
                         MOpcode movsxOpc,
                         int64_t immMin,
                         int64_t immMax,
                         const char *prefix) {
    // Condition code 12 = "o" (overflow), matching LowerOvf.
    constexpr int64_t kCondOverflow = 12;

    const Operand lhs =
        emit.materialiseGpr(builder.makeOperandForValue(instr.ops[0], RegClass::GPR));
    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);
    builder.append(MInstr::make(MOpcode::MOVrr, {emit.clone(dest), emit.clone(lhs)}));

    Operand rhsRaw = builder.makeOperandForValue(instr.ops[1], RegClass::GPR);
    const auto *imm = std::get_if<OpImm>(&rhsRaw);
    if (riOpc != MOpcode::LABEL && imm != nullptr && imm->val >= immMin && imm->val <= immMax) {
        builder.append(MInstr::make(riOpc, {emit.clone(dest), makeImmOperand(imm->val)}));
    } else {
        const Operand rhs = emit.materialiseGpr(std::move(rhsRaw));
        builder.append(MInstr::make(rrOpc, {emit.clone(dest), emit.clone(rhs)}));
    }

    const uint32_t labelId = builder.lower().nextLocalLabelId();
    const std::string trapLabel = std::string(prefix) + "_trap_" + std::to_string(labelId);
    const std::string doneLabel = std::string(prefix) + "_done_" + std::to_string(labelId);
    builder.append(
        MInstr::make(MOpcode::JCC, {makeImmOperand(kCondOverflow), makeLabelOperand(trapLabel)}));
    builder.append(MInstr::make(MOpcode::JMP, {makeLabelOperand(doneLabel)}));
    builder.append(MInstr::make(MOpcode::LABEL, {makeLabelOperand(trapLabel)}));
    emitTrapRaiseError(builder, kErrOverflow);
    builder.append(MInstr::make(MOpcode::LABEL, {makeLabelOperand(doneLabel)}));
    builder.append(MInstr::make(movsxOpc, {emit.clone(dest), emit.clone(dest)}));
}

/// @brief Materialise an XMM operand holding @p bits as a 64-bit IEEE-754 value.
/// @details Builds the constant by loading the bit pattern into a GPR and then
///          using @c MOVQrx to transfer it into an XMM register. Avoids a
///          rodata reference for small one-off constants.
/// @param builder Active MIR builder.
/// @param bits 64-bit IEEE-754 representation to load.
/// @return XMM virtual-register operand carrying the materialised value.
Operand emitF64BitsConstant(MIRBuilder &builder, uint64_t bits) {
    const VReg bitsReg = builder.makeTempVReg(RegClass::GPR);
    const Operand bitsOp = makeVRegOperand(bitsReg.cls, bitsReg.id);
    const VReg xmmReg = builder.makeTempVReg(RegClass::XMM);
    const Operand xmmOp = makeVRegOperand(xmmReg.cls, xmmReg.id);
    builder.append(
        MInstr::make(MOpcode::MOVri, {bitsOp, makeImmOperand(static_cast<int64_t>(bits))}));
    builder.append(MInstr::make(MOpcode::MOVQrx, {xmmOp, bitsOp}));
    return xmmOp;
}

/// @brief Call the runtime banker's-rounding helper for @p src.
/// @details Checked fp-to-int conversions must perform IEEE-754 round-to-nearest
///          ties-to-even rounding before truncation; the SSE @c CVTTSD2SI
///          opcode truncates toward zero, so we delegate to @c rt_round_even
///          and then operate on its result. The call uses the f64 return
///          register defined by the target.
/// @param builder Active MIR builder.
/// @param src Source XMM operand to round.
/// @return XMM virtual-register operand holding the rounded value.
Operand emitRoundEvenCall(MIRBuilder &builder, const Operand &src) {
    CallLoweringPlan plan{};
    plan.callee = "rt_round_even";
    plan.numNamedArgs = 2;
    const auto *srcReg = std::get_if<OpReg>(&src);
    if (!srcReg) {
        phaseAUnsupported("rt_round_even source did not materialize to an XMM register");
    }
    plan.args.push_back(
        CallArg{.cls = CallArgClass::FPR, .vreg = srcReg->idOrPhys, .isImm = false, .imm = 0});
    plan.args.push_back(CallArg{.cls = CallArgClass::GPR, .vreg = 0, .isImm = true, .imm = 0});

    const uint32_t callPlanId = builder.recordCallPlan(std::move(plan));
    builder.append(makePlannedCall(makeLabelOperand(std::string{"rt_round_even"}), callPlanId));

    const VReg roundedReg = builder.makeTempVReg(RegClass::XMM);
    const Operand roundedOp = makeVRegOperand(roundedReg.cls, roundedReg.id);
    builder.append(MInstr::make(
        MOpcode::MOVSDrr,
        {roundedOp,
         makePhysRegOperand(RegClass::XMM, static_cast<uint16_t>(builder.target().f64ReturnReg))}));
    return roundedOp;
}

} // namespace

/// @brief Lower an integer or floating-point add IL instruction.
/// @details Selects MOV/ADD forms based on the destination register class and
///          delegates operand handling to @ref EmitCommon::emitBinary so immediates
///          can be folded when possible.
/// @param instr IL add instruction to lower.
/// @param builder Machine IR builder receiving the emitted instructions.
void emitAdd(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon emit(builder);
    const RegClass cls = builder.regClassFor(instr.resultKind);
    const MOpcode opRR = cls == RegClass::GPR ? MOpcode::ADDrr : MOpcode::FADD;
    const MOpcode opRI = cls == RegClass::GPR ? MOpcode::ADDri : opRR;
    emit.emitBinary(instr, opRR, opRI, cls, cls == RegClass::GPR);
}

/// @brief Lower a subtraction IL instruction.
/// @details Chooses between integer and floating-point subtraction opcodes, then
///          forwards to @ref EmitCommon::emitBinary to handle operand
///          normalisation.
/// @param instr IL subtract instruction to lower.
/// @param builder Machine IR builder receiving the emitted instructions.
void emitSub(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon emit(builder);
    const RegClass cls = builder.regClassFor(instr.resultKind);
    const MOpcode opRR = cls == RegClass::GPR ? MOpcode::SUBrr : MOpcode::FSUB;
    emit.emitBinary(instr, opRR, opRR, cls, false);
}

/// @brief Lower a multiply IL instruction.
/// @details Selects integer or floating-point multiply opcodes and leverages
///          @ref EmitCommon::emitBinary to move operands into their canonical
///          locations.
/// @param instr IL multiply instruction to lower.
/// @param builder Machine IR builder receiving the emitted instructions.
void emitMul(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon emit(builder);
    const RegClass cls = builder.regClassFor(instr.resultKind);
    const MOpcode opRR = cls == RegClass::GPR ? MOpcode::IMULrr : MOpcode::FMUL;
    emit.emitBinary(instr, opRR, opRR, cls, false);
}

/// @brief Lower a floating-point division IL instruction.
/// @details Division always occurs in XMM registers, so the helper directly
///          invokes @ref EmitCommon::emitBinary with FDIV opcodes and floating
///          register classes.
/// @param instr IL floating-point divide instruction.
/// @param builder Machine IR builder receiving the emitted instructions.
void emitFDiv(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon(builder).emitBinary(instr, MOpcode::FDIV, MOpcode::FDIV, RegClass::XMM, false);
}

/// @brief Lower an integer compare IL instruction.
/// @details Uses @ref EmitCommon::icmpConditionCode to resolve the condition
///          code and @ref EmitCommon::emitCmp to produce the Machine IR compare
///          sequence.
/// @param instr IL integer compare instruction.
/// @param builder Machine IR builder receiving the emitted instructions.
void emitICmp(const ILInstr &instr, MIRBuilder &builder) {
    if (const auto cond = EmitCommon::icmpConditionCode(instr.opcode)) {
        EmitCommon(builder).emitCmp(instr, RegClass::GPR, *cond);
        return;
    }
    phaseAUnsupported(("unknown integer compare opcode: " + instr.opcode).c_str());
}

/// @brief Lower a floating-point compare IL instruction.
/// @details Routes NaN-sensitive comparisons (eq, ne, lt, le) through the
///          multi-instruction @ref EmitCommon::emitFCmpNanSafe helper that
///          correctly handles the UCOMISD flag state when either operand is NaN.
///          The remaining comparisons (gt, ge, ord, uno) use simple SETcc via
///          the generic @ref EmitCommon::emitCmp path.
/// @param instr IL floating-point compare instruction.
/// @param builder Machine IR builder receiving the emitted instructions.
void emitFCmp(const ILInstr &instr, MIRBuilder &builder) {
    if (!instr.opcode.starts_with("fcmp_")) {
        return;
    }

    const std::string_view suffix(instr.opcode.data() + 5, instr.opcode.size() - 5);

    // eq/ne/lt/le require NaN-safe multi-instruction sequences.
    if (suffix == "eq" || suffix == "ne" || suffix == "lt" || suffix == "le") {
        EmitCommon(builder).emitFCmpNanSafe(instr, suffix);
        return;
    }

    // gt/ge/ord/uno are already NaN-correct with a single SETcc.
    if (const auto cond = EmitCommon::fcmpConditionCode(instr.opcode)) {
        EmitCommon(builder).emitCmp(instr, RegClass::XMM, *cond);
        return;
    }
    phaseAUnsupported(("unknown floating-point compare opcode: " + instr.opcode).c_str());
}

/// @brief Lower an explicit compare IL instruction that encodes the result type.
/// @details Determines the register class using either the result or first
///          operand kind, then emits a compare that materialises the condition
///          into the destination virtual register.
/// @param instr IL compare instruction with explicit operands.
/// @param builder Machine IR builder receiving the emitted instructions.
void emitCmpExplicit(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon emit(builder);
    const RegClass cls =
        builder.regClassFor(instr.ops.empty() ? instr.resultKind : instr.ops.front().kind);
    emit.emitCmp(instr, cls, 1);
}

/// @brief Lower an overflow-checked integer add.
/// @details Emits the ADDOvfrr pseudo-op which the post-lowering pass will
///          expand into ADD + JO (trap on overflow).
/// @param instr Checked-add IL instruction.
/// @param builder MIR destination and lowering context.
void emitAddOvf(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon emit(builder);
    if (instr.resultBits < 64) {
        if (instr.resultId < 0 || instr.ops.size() < 2) {
            phaseAUnsupported("iadd.ovf: missing operands");
        }
        if (instr.resultBits == 32) {
            emitNarrowCheckedOp(instr,
                                builder,
                                emit,
                                MOpcode::ADDrr32,
                                MOpcode::ADDri32,
                                MOpcode::MOVSXD,
                                std::numeric_limits<int32_t>::min(),
                                std::numeric_limits<int32_t>::max(),
                                ".Liadd_ovf32");
            return;
        }
        if (instr.resultBits == 16) {
            emitNarrowCheckedOp(instr,
                                builder,
                                emit,
                                MOpcode::ADDrr16,
                                MOpcode::ADDri16,
                                MOpcode::MOVSXrr16,
                                std::numeric_limits<int16_t>::min(),
                                std::numeric_limits<int16_t>::max(),
                                ".Liadd_ovf16");
            return;
        }
        const Operand lhs =
            emitSignExtendedToWidth(builder,
                                    emit,
                                    builder.makeOperandForValue(instr.ops[0], RegClass::GPR),
                                    instr.resultBits);
        const Operand rhs =
            emitSignExtendedToWidth(builder,
                                    emit,
                                    builder.makeOperandForValue(instr.ops[1], RegClass::GPR),
                                    instr.resultBits);
        const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
        const Operand dest = makeVRegOperand(destReg.cls, destReg.id);
        builder.append(MInstr::make(MOpcode::MOVrr, {emit.clone(dest), emit.clone(lhs)}));
        builder.append(MInstr::make(MOpcode::ADDrr, {emit.clone(dest), emit.clone(rhs)}));
        emitSubWidthOverflowCheck(builder, emit, dest, instr.resultBits, ".Liadd_ovf");
        return;
    }
    emit.emitBinary(instr, MOpcode::ADDOvfrr, MOpcode::ADDOvfrr, RegClass::GPR, false);
}

/// @brief Lower an overflow-checked integer subtract.
/// @details Emits the SUBOvfrr pseudo-op.
/// @param instr Checked-subtract IL instruction.
/// @param builder MIR destination and lowering context.
void emitSubOvf(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon emit(builder);
    if (instr.resultBits < 64) {
        if (instr.resultId < 0 || instr.ops.size() < 2) {
            phaseAUnsupported("isub.ovf: missing operands");
        }
        if (instr.resultBits == 32) {
            emitNarrowCheckedOp(instr,
                                builder,
                                emit,
                                MOpcode::SUBrr32,
                                MOpcode::LABEL,
                                MOpcode::MOVSXD,
                                0,
                                0,
                                ".Lisub_ovf32");
            return;
        }
        if (instr.resultBits == 16) {
            emitNarrowCheckedOp(instr,
                                builder,
                                emit,
                                MOpcode::SUBrr16,
                                MOpcode::LABEL,
                                MOpcode::MOVSXrr16,
                                0,
                                0,
                                ".Lisub_ovf16");
            return;
        }
        const Operand lhs =
            emitSignExtendedToWidth(builder,
                                    emit,
                                    builder.makeOperandForValue(instr.ops[0], RegClass::GPR),
                                    instr.resultBits);
        const Operand rhs =
            emitSignExtendedToWidth(builder,
                                    emit,
                                    builder.makeOperandForValue(instr.ops[1], RegClass::GPR),
                                    instr.resultBits);
        const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
        const Operand dest = makeVRegOperand(destReg.cls, destReg.id);
        builder.append(MInstr::make(MOpcode::MOVrr, {emit.clone(dest), emit.clone(lhs)}));
        builder.append(MInstr::make(MOpcode::SUBrr, {emit.clone(dest), emit.clone(rhs)}));
        emitSubWidthOverflowCheck(builder, emit, dest, instr.resultBits, ".Lisub_ovf");
        return;
    }
    emit.emitBinary(instr, MOpcode::SUBOvfrr, MOpcode::SUBOvfrr, RegClass::GPR, false);
}

/// @brief Lower an overflow-checked integer multiply.
/// @details Emits the IMULOvfrr pseudo-op.
/// @param instr Checked-multiply IL instruction.
/// @param builder MIR destination and lowering context.
void emitMulOvf(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon emit(builder);
    if (instr.resultBits < 64) {
        if (instr.resultId < 0 || instr.ops.size() < 2) {
            phaseAUnsupported("imul.ovf: missing operands");
        }
        if (instr.resultBits == 32) {
            emitNarrowCheckedOp(instr,
                                builder,
                                emit,
                                MOpcode::IMULrr32,
                                MOpcode::LABEL,
                                MOpcode::MOVSXD,
                                0,
                                0,
                                ".Limul_ovf32");
            return;
        }
        if (instr.resultBits == 16) {
            emitNarrowCheckedOp(instr,
                                builder,
                                emit,
                                MOpcode::IMULrr16,
                                MOpcode::LABEL,
                                MOpcode::MOVSXrr16,
                                0,
                                0,
                                ".Limul_ovf16");
            return;
        }
        const Operand lhs =
            emitSignExtendedToWidth(builder,
                                    emit,
                                    builder.makeOperandForValue(instr.ops[0], RegClass::GPR),
                                    instr.resultBits);
        const Operand rhs =
            emitSignExtendedToWidth(builder,
                                    emit,
                                    builder.makeOperandForValue(instr.ops[1], RegClass::GPR),
                                    instr.resultBits);
        const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
        const Operand dest = makeVRegOperand(destReg.cls, destReg.id);
        builder.append(MInstr::make(MOpcode::MOVrr, {emit.clone(dest), emit.clone(lhs)}));
        builder.append(MInstr::make(MOpcode::IMULrr, {emit.clone(dest), emit.clone(rhs)}));
        emitSubWidthOverflowCheck(builder, emit, dest, instr.resultBits, ".Limul_ovf");
        return;
    }
    emit.emitBinary(instr, MOpcode::IMULOvfrr, MOpcode::IMULOvfrr, RegClass::GPR, false);
}

/// @brief Lower any of the signed/unsigned div or rem IL opcodes.
/// @details Delegates to @ref EmitCommon::emitDivRem, which selects the
///          checked or unchecked DIV/REM pseudo based on the IL opcode string.
///          The actual CQO/IDIV expansion happens later in @ref LowerDiv.
/// @param instr IL div/rem instruction.
/// @param builder Active MIR builder.
void emitDivFamily(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon(builder).emitDivRem(instr, instr.opcode);
}

/// @brief Lower zero-/sign-extension and truncation IL instructions.
/// @details Emits a copy followed by the canonical mask or shift sequence
///          required by the source/result bit-width metadata. This keeps
///          narrow values from preserving stale high bits after widening or
///          truncating, including boolean results.
/// @param instr IL zext/sext/trunc instruction.
/// @param builder Active MIR builder.
void emitZSTrunc(const ILInstr &instr, MIRBuilder &builder) {
    EmitCommon emit(builder);
    if (instr.resultId < 0 || instr.ops.empty()) {
        phaseAUnsupported("cast: missing operands");
    }
    if (!isIntegerLikeKind(instr.resultKind) || !isIntegerLikeKind(instr.ops.front().kind)) {
        phaseAUnsupported("cast: integer zext/sext/trunc expected integer-like operands");
    }

    const ILValue &sourceValue = instr.ops.front();
    const std::uint8_t sourceBits = normalizedIntegerBits(sourceValue.bits, sourceValue.kind);
    const std::uint8_t resultBits = normalizedIntegerBits(instr.resultBits, instr.resultKind);

    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    if (destReg.cls != RegClass::GPR) {
        phaseAUnsupported("cast: integer zext/sext/trunc destination must be a GPR");
    }
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);
    const Operand src =
        emit.materialiseGpr(builder.makeOperandForValue(sourceValue, RegClass::GPR));

    builder.append(MInstr::make(MOpcode::MOVrr, {emit.clone(dest), emit.clone(src)}));

    if (instr.opcode == "trunc") {
        emitMaskToWidth(builder, emit, dest, resultBits);
        return;
    }

    const std::uint8_t valueBits = std::min(sourceBits, resultBits);
    if (instr.opcode == "zext") {
        emitMaskToWidth(builder, emit, dest, valueBits);
        return;
    }

    if (instr.opcode == "sext") {
        if (valueBits < 64) {
            const int shift = 64 - static_cast<int>(valueBits);
            builder.append(MInstr::make(MOpcode::SHLri, {emit.clone(dest), makeImmOperand(shift)}));
            builder.append(MInstr::make(MOpcode::SARri, {emit.clone(dest), makeImmOperand(shift)}));
        }
        return;
    }

    phaseAUnsupported("cast: unsupported integer cast opcode");
}

/// @brief Lower signed-integer-to-double conversion via CVTSI2SD.
/// @details Single-instruction lowering — CVTSI2SD handles full 64-bit
///          signed inputs directly. For the unsigned counterpart see
///          @ref emitUiToFp which must split the high-bit case.
/// @param instr IL sitofp instruction.
/// @param builder Active MIR builder.
void emitSIToFP(const ILInstr &instr, MIRBuilder &builder) {
    requireIntToFpShape(instr, "sitofp: expected integer source and f64 result");
    EmitCommon(builder).emitCast(instr, MOpcode::CVTSI2SD, RegClass::XMM, RegClass::GPR);
}

/// @brief Lower unchecked floating-point-to-signed-integer conversion.
/// @details Performs NaN and range checks against the precomputed F64 bound
///          patterns and traps when the input would produce a value outside
///          the signed result type. The truncated value is produced via
///          @c CVTTSD2SI. A unique label triple is allocated per call site
///          to keep branch destinations deterministic across the function.
/// @param instr IL fptosi instruction.
/// @param builder Active MIR builder.
void emitFPToSI(const ILInstr &instr, MIRBuilder &builder) {
    requireFpToIntShape(instr, "fptosi: expected f64 source and integer result");

    EmitCommon emit(builder);
    const Operand src =
        emit.materialise(builder.makeOperandForValue(instr.ops[0], RegClass::XMM), RegClass::XMM);
    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    if (destReg.cls != RegClass::GPR) {
        phaseAUnsupported("fptosi: destination must be a GPR");
    }
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    const uint32_t labelId = builder.lower().nextLocalLabelId();
    const std::string invalidLabel = ".Lfptosi_invalid_" + std::to_string(labelId);
    const std::string overflowLabel = ".Lfptosi_ovf_" + std::to_string(labelId);
    const std::string doneLabel = ".Lfptosi_done_" + std::to_string(labelId);

    builder.append(
        MInstr::make(MOpcode::UCOMIS, std::vector<Operand>{emit.clone(src), emit.clone(src)}));
    builder.append(MInstr::make(
        MOpcode::JCC, std::vector<Operand>{makeImmOperand(10), makeLabelOperand(invalidLabel)}));

    const Operand minOp = emitF64BitsConstant(builder, signedMinF64Bits(instr.resultBits));
    builder.append(
        MInstr::make(MOpcode::UCOMIS, std::vector<Operand>{emit.clone(src), emit.clone(minOp)}));
    builder.append(MInstr::make(
        MOpcode::JCC, std::vector<Operand>{makeImmOperand(8), makeLabelOperand(overflowLabel)}));

    const Operand maxExclusiveOp =
        emitF64BitsConstant(builder, signedUpperExclusiveF64Bits(instr.resultBits));
    builder.append(MInstr::make(MOpcode::UCOMIS,
                                std::vector<Operand>{emit.clone(src), emit.clone(maxExclusiveOp)}));
    builder.append(MInstr::make(
        MOpcode::JCC, std::vector<Operand>{makeImmOperand(7), makeLabelOperand(overflowLabel)}));

    builder.append(
        MInstr::make(MOpcode::CVTTSD2SI, std::vector<Operand>{emit.clone(dest), emit.clone(src)}));
    builder.append(MInstr::make(MOpcode::JMP, std::vector<Operand>{makeLabelOperand(doneLabel)}));
    builder.append(
        MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(invalidLabel)}));
    emitTrapRaiseError(builder, kErrInvalidCast);
    builder.append(
        MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(overflowLabel)}));
    emitTrapRaiseError(builder, kErrOverflow);
    builder.append(MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(doneLabel)}));
}

/// @brief Lower the checked fp-to-signed-int conversion (with explicit rounding).
/// @details Differs from @ref emitFPToSI in that the input is first rounded
///          to nearest-even via @ref emitRoundEvenCall, matching the IL
///          contract for `fptosi.chk`. The rest of the bound checks and
///          trap structure mirror the unchecked path.
/// @param instr IL fptosi.chk instruction.
/// @param builder Active MIR builder.
void emitFPToSIChecked(const ILInstr &instr, MIRBuilder &builder) {
    requireFpToIntShape(instr, "fptosi_chk: expected f64 source and integer result");

    EmitCommon emit(builder);
    const Operand src =
        emit.materialise(builder.makeOperandForValue(instr.ops[0], RegClass::XMM), RegClass::XMM);
    const Operand rounded = emitRoundEvenCall(builder, src);
    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    if (destReg.cls != RegClass::GPR) {
        phaseAUnsupported("fptosi_chk: destination must be a GPR");
    }
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    const uint32_t labelId = builder.lower().nextLocalLabelId();
    const std::string invalidLabel = ".Lfptosi_chk_invalid_" + std::to_string(labelId);
    const std::string overflowLabel = ".Lfptosi_chk_ovf_" + std::to_string(labelId);
    const std::string doneLabel = ".Lfptosi_chk_done_" + std::to_string(labelId);

    builder.append(MInstr::make(MOpcode::UCOMIS,
                                std::vector<Operand>{emit.clone(rounded), emit.clone(rounded)}));
    builder.append(MInstr::make(
        MOpcode::JCC, std::vector<Operand>{makeImmOperand(10), makeLabelOperand(invalidLabel)}));

    const Operand minOp = emitF64BitsConstant(builder, signedMinF64Bits(instr.resultBits));
    builder.append(MInstr::make(MOpcode::UCOMIS,
                                std::vector<Operand>{emit.clone(rounded), emit.clone(minOp)}));
    builder.append(MInstr::make(
        MOpcode::JCC, std::vector<Operand>{makeImmOperand(8), makeLabelOperand(overflowLabel)}));

    const Operand maxExclusiveOp =
        emitF64BitsConstant(builder, signedUpperExclusiveF64Bits(instr.resultBits));
    builder.append(MInstr::make(
        MOpcode::UCOMIS, std::vector<Operand>{emit.clone(rounded), emit.clone(maxExclusiveOp)}));
    builder.append(MInstr::make(
        MOpcode::JCC, std::vector<Operand>{makeImmOperand(7), makeLabelOperand(overflowLabel)}));

    builder.append(MInstr::make(MOpcode::CVTTSD2SI,
                                std::vector<Operand>{emit.clone(dest), emit.clone(rounded)}));
    builder.append(MInstr::make(MOpcode::JMP, std::vector<Operand>{makeLabelOperand(doneLabel)}));
    builder.append(
        MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(invalidLabel)}));
    emitTrapRaiseError(builder, kErrInvalidCast);
    builder.append(
        MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(overflowLabel)}));
    emitTrapRaiseError(builder, kErrOverflow);
    builder.append(MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(doneLabel)}));
}

/// @brief Lower the checked fp-to-unsigned-int conversion.
/// @details CVTTSD2SI is signed-only; for unsigned conversion of values in
///          [2^63, 2^64) we subtract 2^63 from the rounded source, convert
///          the resulting signed value, then set bit 63 of the result. The
///          "small" path (input < 2^63) uses CVTTSD2SI directly. NaN and
///          negative inputs trap as invalid casts; inputs >= 2^64 trap as
///          overflow.
/// @param instr IL fptoui.chk instruction.
/// @param builder Active MIR builder.
void emitFpToUi(const ILInstr &instr, MIRBuilder &builder) {
    requireFpToIntShape(instr, "fptoui: expected f64 source and integer result");

    EmitCommon emit(builder);
    const Operand src =
        emit.materialise(builder.makeOperandForValue(instr.ops[0], RegClass::XMM), RegClass::XMM);
    const Operand rounded = emitRoundEvenCall(builder, src);
    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    if (destReg.cls != RegClass::GPR) {
        phaseAUnsupported("fptoui: destination must be a GPR");
    }
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    const uint32_t labelId = builder.lower().nextLocalLabelId();
    const std::string invalidLabel = ".Lfptoui_invalid_" + std::to_string(labelId);
    const std::string overflowLabel = ".Lfptoui_ovf_" + std::to_string(labelId);
    const std::string smallLabel = ".Lfptoui_sm_" + std::to_string(labelId);
    const std::string doneLabel = ".Lfptoui_done_" + std::to_string(labelId);

    builder.append(MInstr::make(MOpcode::UCOMIS,
                                std::vector<Operand>{emit.clone(rounded), emit.clone(rounded)}));
    builder.append(MInstr::make(
        MOpcode::JCC, std::vector<Operand>{makeImmOperand(10), makeLabelOperand(invalidLabel)}));

    const Operand zeroOp = emitF64BitsConstant(builder, 0x0000000000000000ULL);
    builder.append(MInstr::make(MOpcode::UCOMIS,
                                std::vector<Operand>{emit.clone(rounded), emit.clone(zeroOp)}));
    builder.append(MInstr::make(
        MOpcode::JCC, std::vector<Operand>{makeImmOperand(8), makeLabelOperand(invalidLabel)}));

    const Operand upperExclusiveOp =
        emitF64BitsConstant(builder, unsignedUpperExclusiveF64Bits(instr.resultBits));
    builder.append(MInstr::make(
        MOpcode::UCOMIS, std::vector<Operand>{emit.clone(rounded), emit.clone(upperExclusiveOp)}));
    builder.append(MInstr::make(
        MOpcode::JCC, std::vector<Operand>{makeImmOperand(7), makeLabelOperand(overflowLabel)}));

    const Operand limitOp = emitF64BitsConstant(builder, 0x43E0000000000000ULL);
    builder.append(MInstr::make(MOpcode::UCOMIS,
                                std::vector<Operand>{emit.clone(rounded), emit.clone(limitOp)}));
    builder.append(MInstr::make(
        MOpcode::JCC, std::vector<Operand>{makeImmOperand(8), makeLabelOperand(smallLabel)}));

    const VReg adjReg = builder.makeTempVReg(RegClass::XMM);
    const Operand adjOp = makeVRegOperand(adjReg.cls, adjReg.id);
    builder.append(MInstr::make(MOpcode::MOVSDrr,
                                std::vector<Operand>{emit.clone(adjOp), emit.clone(rounded)}));
    builder.append(MInstr::make(MOpcode::FSUB, std::vector<Operand>{adjOp, emit.clone(limitOp)}));
    builder.append(MInstr::make(MOpcode::CVTTSD2SI,
                                std::vector<Operand>{emit.clone(dest), emit.clone(adjOp)}));
    const VReg highBitReg = builder.makeTempVReg(RegClass::GPR);
    const Operand highBitOp = makeVRegOperand(highBitReg.cls, highBitReg.id);
    builder.append(
        MInstr::make(MOpcode::MOVri,
                     std::vector<Operand>{
                         highBitOp, makeImmOperand(static_cast<int64_t>(0x8000000000000000ULL))}));
    builder.append(MInstr::make(MOpcode::ORrr, std::vector<Operand>{dest, highBitOp}));
    builder.append(MInstr::make(MOpcode::JMP, std::vector<Operand>{makeLabelOperand(doneLabel)}));

    builder.append(
        MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(invalidLabel)}));
    emitTrapRaiseError(builder, kErrInvalidCast);
    builder.append(
        MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(overflowLabel)}));
    emitTrapRaiseError(builder, kErrOverflow);

    builder.append(
        MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(smallLabel)}));
    builder.append(
        MInstr::make(MOpcode::CVTTSD2SI, std::vector<Operand>{dest, emit.clone(rounded)}));

    builder.append(MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(doneLabel)}));
}

/// @brief Common implementation for checked signed/unsigned integer narrowing.
/// @details Sign- or zero-extends the source to the target width, compares
///          against the original 64-bit value, and traps with the overflow
///          error when the two differ. The result lives in the destination
///          virtual register only on the fallthrough (success) path.
/// @param instr IL narrow-cast instruction.
/// @param builder Active MIR builder.
/// @param isSigned True for `s.narrow.chk`, false for `u.narrow.chk`.
void emitNarrowCastChecked(const ILInstr &instr, MIRBuilder &builder, bool isSigned) {
    requireNarrowCastShape(instr,
                           isSigned ? "si_narrow_chk: expected integer operands"
                                    : "ui_narrow_chk: expected integer operands");

    EmitCommon emit(builder);
    const Operand src =
        emit.materialiseGpr(builder.makeOperandForValue(instr.ops[0], RegClass::GPR));
    const Operand narrowed =
        isSigned ? emitSignExtendedToWidth(builder, emit, emit.clone(src), instr.resultBits)
                 : emitZeroExtendedToWidth(builder, emit, emit.clone(src), instr.resultBits);
    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    if (destReg.cls != RegClass::GPR) {
        phaseAUnsupported("narrow_chk: destination must be a GPR");
    }
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    const uint32_t labelId = builder.lower().nextLocalLabelId();
    const std::string trapLabel = ".Lnarrow_chk_trap_" + std::to_string(labelId);
    const std::string doneLabel = ".Lnarrow_chk_done_" + std::to_string(labelId);

    builder.append(MInstr::make(MOpcode::CMPrr, {emit.clone(narrowed), emit.clone(src)}));
    builder.append(MInstr::make(MOpcode::JCC, {makeImmOperand(1), makeLabelOperand(trapLabel)}));
    builder.append(MInstr::make(MOpcode::MOVrr, {emit.clone(dest), emit.clone(narrowed)}));
    builder.append(MInstr::make(MOpcode::JMP, {makeLabelOperand(doneLabel)}));
    builder.append(MInstr::make(MOpcode::LABEL, {makeLabelOperand(trapLabel)}));
    emitTrapRaiseError(builder, kErrOverflow);
    builder.append(MInstr::make(MOpcode::LABEL, {makeLabelOperand(doneLabel)}));
}

/// @brief Lower signed-integer narrowing with overflow trap.
/// @details Thin wrapper around @ref emitNarrowCastChecked with
///          @c isSigned == true.
/// @param instr Checked signed-narrowing instruction.
/// @param builder MIR destination and lowering context.
void emitSiNarrowChecked(const ILInstr &instr, MIRBuilder &builder) {
    emitNarrowCastChecked(instr, builder, true);
}

/// @brief Lower unsigned-integer narrowing with overflow trap.
/// @details Thin wrapper around @ref emitNarrowCastChecked with
///          @c isSigned == false.
/// @param instr Checked unsigned-narrowing instruction.
/// @param builder MIR destination and lowering context.
void emitUiNarrowChecked(const ILInstr &instr, MIRBuilder &builder) {
    emitNarrowCastChecked(instr, builder, false);
}

/// @brief Lower an IL `uitofp` instruction (unsigned int to floating-point).
/// @details Handles the full unsigned 64-bit input range [0, 2^64).
///
///          x86-64 CVTSI2SD interprets its source as signed.  Values in [0, 2^63)
///          convert correctly.  For values with bit 63 set (>= 2^63), we shift
///          right by 1, preserving the LSB for rounding, convert the halved value,
///          then double the result with ADDSD.
///
///          Generated sequence:
///            testq  %src, %src
///            js     .Lhigh           ; sign bit set → large path
///            cvtsi2sd %src, %dst     ; [0, 2^63) direct
///            jmp    .Ldone
///          .Lhigh:
///            movq   %src, %tmp
///            andq   $1, %lsb        ; save LSB
///            shrq   $1, %tmp        ; halve (now positive)
///            orq    %lsb, %tmp      ; restore rounding bit
///            cvtsi2sd %tmp, %dst
///            addsd  %dst, %dst      ; double back
///          .Ldone:
/// @param instr IL unsigned-integer-to-f64 conversion.
/// @param builder MIR destination and lowering context.
void emitUiToFp(const ILInstr &instr, MIRBuilder &builder) {
    requireIntToFpShape(instr, "uitofp: expected integer source and f64 result");

    EmitCommon emit(builder);
    const Operand src =
        emit.materialiseGpr(builder.makeOperandForValue(instr.ops[0], RegClass::GPR));
    const VReg destReg = builder.ensureVReg(instr.resultId, instr.resultKind);
    if (destReg.cls != RegClass::XMM) {
        phaseAUnsupported("uitofp: destination must be an XMM register");
    }
    const Operand dest = makeVRegOperand(destReg.cls, destReg.id);

    const uint32_t labelId = builder.lower().nextLocalLabelId();
    const std::string highLabel = ".Luitofp_hi_" + std::to_string(labelId);
    const std::string doneLabel = ".Luitofp_done_" + std::to_string(labelId);

    // TEST src, src — sets SF if bit 63 is set.
    builder.append(
        MInstr::make(MOpcode::TESTrr, std::vector<Operand>{emit.clone(src), emit.clone(src)}));
    // JS → high path (condition code: "s" is not in our table, but we can use
    // the sign flag via JCC with condition code for signed-less-than after TEST).
    // After TESTrr: SF = bit 63 of src.  "JS" = jump if SF=1.
    // Looking at conditionSuffix: code 2 = "l" (signed less), code 8 = "b" (unsigned below).
    // After TEST %r,%r: ZF and SF are set, CF=OF=0.
    // "l" (SF!=OF) → SF=1,OF=0 → true when bit 63 set. This is equivalent to JS.
    builder.append(MInstr::make(
        MOpcode::JCC, std::vector<Operand>{makeImmOperand(2), makeLabelOperand(highLabel)}));

    // Small path: src in [0, 2^63), direct CVTSI2SD.
    builder.append(
        MInstr::make(MOpcode::CVTSI2SD, std::vector<Operand>{emit.clone(dest), emit.clone(src)}));
    builder.append(MInstr::make(MOpcode::JMP, std::vector<Operand>{makeLabelOperand(doneLabel)}));

    // High path: src >= 2^63 (bit 63 set).
    builder.append(MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(highLabel)}));

    // tmp = src >> 1 (logical shift right, halves the value)
    const VReg tmpReg = builder.makeTempVReg(RegClass::GPR);
    const Operand tmpOp = makeVRegOperand(tmpReg.cls, tmpReg.id);
    builder.append(
        MInstr::make(MOpcode::MOVrr, std::vector<Operand>{emit.clone(tmpOp), emit.clone(src)}));
    builder.append(MInstr::make(MOpcode::SHRri, std::vector<Operand>{tmpOp, makeImmOperand(1)}));

    // lsb = src & 1 (preserve LSB for correct rounding)
    const VReg lsbReg = builder.makeTempVReg(RegClass::GPR);
    const Operand lsbOp = makeVRegOperand(lsbReg.cls, lsbReg.id);
    builder.append(
        MInstr::make(MOpcode::MOVrr, std::vector<Operand>{emit.clone(lsbOp), emit.clone(src)}));
    builder.append(MInstr::make(MOpcode::ANDri, std::vector<Operand>{lsbOp, makeImmOperand(1)}));

    // tmp |= lsb (OR back the rounding bit)
    builder.append(MInstr::make(MOpcode::ORrr, std::vector<Operand>{tmpOp, emit.clone(lsbOp)}));

    // Convert halved value to double.
    builder.append(
        MInstr::make(MOpcode::CVTSI2SD, std::vector<Operand>{emit.clone(dest), emit.clone(tmpOp)}));

    // Double the result: dst += dst  (2-operand FADD: dest += src).
    builder.append(MInstr::make(MOpcode::FADD, std::vector<Operand>{dest, emit.clone(dest)}));

    // Done.
    builder.append(MInstr::make(MOpcode::LABEL, std::vector<Operand>{makeLabelOperand(doneLabel)}));
}

} // namespace zanna::codegen::x64::lowering
