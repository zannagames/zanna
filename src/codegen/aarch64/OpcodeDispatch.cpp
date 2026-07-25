//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/OpcodeDispatch.cpp
// Purpose: Central opcode dispatch switch for IL→MIR lowering. Routes each IL
//          opcode to its InstrLowering handler; returns false for opcodes that
//          the caller (LowerILToMIR) must handle directly.
// Key invariants:
//   - Returns true on handled opcodes, false to fall through to caller.
//   - Terminators (Br/CBr/SwitchI32) return true but emit no MIR here;
//     TerminatorLowering handles them in a later pass.
//   - All block access goes through bbOut() so no block reference is retained
//     across nested lowering helpers.
// Ownership/Lifetime:
//   - Free function; borrows ctx and its contained MFunction for the call.
// Links: src/codegen/aarch64/OpcodeDispatch.hpp,
//        src/codegen/aarch64/InstrLowering.cpp,
//        src/codegen/aarch64/TerminatorLowering.cpp,
//        src/codegen/aarch64/LowerILToMIR.cpp
//
//===----------------------------------------------------------------------===//

#include "OpcodeDispatch.hpp"
#include "InstrLowering.hpp"
#include "Noreturn.hpp"
#include "OpcodeMappings.hpp"
#include "codegen/common/ScalarBits.hpp"

#include "il/core/Opcode.hpp"
#include "il/runtime/RuntimeNameMap.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>

/**
 * @file
 * @brief Implements AArch64 IL instruction dispatch and checked-cast lowering.
 *
 * The dispatcher combines small in-place lowering cases with calls into the
 * specialized instruction-lowering helpers. Floating-point checked casts
 * register deferred shared trap blocks, while ordinary calls, memory
 * operations, arithmetic, error accessors, and constants emit MIR directly
 * into the indexed destination block.
 */

namespace zanna::codegen::aarch64 {

using il::core::Opcode;

/**
 * @brief Materializes a scalar value and copies it into a physical argument register.
 *
 * @param value IL value to materialize.
 * @param bbIn Source block used when resolving value producers.
 * @param[in,out] ctx Lowering state and virtual-register allocator.
 * @param dstReg Physical GPR that receives the value.
 * @param[in,out] out Destination block for materialization and move instructions.
 * @param what Non-null human-readable operand name used in failures.
 * @throws std::runtime_error If materialization fails or produces a non-GPR value.
 */
static void moveValueToArg(const il::core::Value &value,
                           const il::core::BasicBlock &bbIn,
                           LoweringContext &ctx,
                           PhysReg dstReg,
                           MBasicBlock &out,
                           const char *what) {
    uint16_t src = 0;
    RegClass cls = RegClass::GPR;
    if (!materializeValueToVReg(value, bbIn, ctx, out, src, cls))
        throw std::runtime_error(std::string("AArch64 lowering: failed to materialize ") + what);
    if (cls != RegClass::GPR)
        throw std::runtime_error(std::string("AArch64 lowering: expected GPR for ") + what);
    out.instrs.push_back(
        MInstr{MOpcode::MovRR, {MOperand::regOp(dstReg), MOperand::vregOp(RegClass::GPR, src)}});
}

/**
 * @brief Captures an optional GPR call result from the AArch64 return register.
 *
 * Instructions without a result are left untouched. Otherwise a fresh GPR
 * virtual register is mapped to the IL result and initialized from `x0`.
 *
 * @param ins Call-like IL instruction whose result is being captured.
 * @param[in,out] ctx Lowering maps and virtual-register allocator.
 * @param[in,out] out Block that receives the result-copy instruction.
 * @throws std::runtime_error If the ordinary virtual-register range is exhausted.
 */
static void captureGprCallResult(const il::core::Instr &ins,
                                 LoweringContext &ctx,
                                 MBasicBlock &out) {
    if (!ins.result)
        return;
    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
    ctx.tempVReg[*ins.result] = dst;
    ctx.tempRegClass[*ins.result] = RegClass::GPR;
    out.instrs.push_back(MInstr{
        MOpcode::MovRR, {MOperand::vregOp(RegClass::GPR, dst), MOperand::regOp(PhysReg::X0)}});
}

/**
 * @brief Resolves a canonical runtime name while preserving ordinary user symbols.
 *
 * @param name Symbol spelling to resolve.
 * @return Owned mapped runtime linker name when known, otherwise an owned copy
 *         of @p name.
 */
static std::string mapExternalSymbol(std::string_view name) {
    if (auto mapped = il::runtime::mapCanonicalRuntimeName(name))
        return std::string(*mapped);
    return std::string(name);
}

/**
 * @brief Materializes a binary64 constant in a fresh FPR virtual register.
 *
 * The exact IEEE-754 bits are first loaded into a temporary GPR and then moved
 * bitwise to the destination FPR.
 *
 * @param value Floating-point value whose representation is required.
 * @param[in,out] ctx Virtual-register allocator and lowering state.
 * @param[in,out] out Block that receives the `MovRI` and `FMovGR` sequence.
 * @return Newly allocated FPR virtual-register ID.
 * @throws std::runtime_error If either required virtual register cannot be allocated.
 */
static uint16_t materializeF64Constant(double value, LoweringContext &ctx, MBasicBlock &out) {
    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
    const uint16_t bitsGpr = allocateNextVReg(ctx.nextVRegId);
    out.instrs.push_back(
        MInstr{MOpcode::MovRI,
               {MOperand::vregOp(RegClass::GPR, bitsGpr),
                MOperand::immOp(static_cast<long long>(zanna::codegen::common::f64Bits(value)))}});
    out.instrs.push_back(
        MInstr{MOpcode::FMovGR,
               {MOperand::vregOp(RegClass::FPR, dst), MOperand::vregOp(RegClass::GPR, bitsGpr)}});
    return dst;
}

/**
 * @brief Maps a supported IL integer type to its checked-cast bit width.
 *
 * @param kind IL result-type kind.
 * @return `1`, `16`, `32`, or `64` for the corresponding integer kind.
 * @throws std::runtime_error If @p kind is not a supported integer type.
 */
static int integerTypeBits(il::core::Type::Kind kind) {
    switch (kind) {
        case il::core::Type::Kind::I1:
            return 1;
        case il::core::Type::Kind::I16:
            return 16;
        case il::core::Type::Kind::I32:
            return 32;
        case il::core::Type::Kind::I64:
            return 64;
        default:
            throw std::runtime_error("AArch64 lowering: checked cast requires integer result");
    }
}

/**
 * @brief Computes the inclusive floating-point lower bound for a signed width.
 *
 * @param bits Supported integer width (`1`, `16`, `32`, or `64`).
 * @return `-2^(bits-1)` represented as a `double`.
 * @throws std::runtime_error If @p bits is unsupported.
 */
static double signedLowerBoundForBits(int bits) {
    switch (bits) {
        case 1:
            return -1.0;
        case 16:
            return -32768.0;
        case 32:
            return -2147483648.0;
        case 64:
            return -9223372036854775808.0;
        default:
            throw std::runtime_error("AArch64 lowering: unsupported signed fp cast width");
    }
}

/**
 * @brief Computes the exclusive floating-point upper bound for a signed width.
 *
 * @param bits Supported integer width (`1`, `16`, `32`, or `64`).
 * @return `2^(bits-1)` represented as a `double`.
 * @throws std::runtime_error If @p bits is unsupported.
 */
static double signedUpperExclusiveForBits(int bits) {
    switch (bits) {
        case 1:
            return 1.0;
        case 16:
            return 32768.0;
        case 32:
            return 2147483648.0;
        case 64:
            return 9223372036854775808.0;
        default:
            throw std::runtime_error("AArch64 lowering: unsupported signed fp cast width");
    }
}

/**
 * @brief Computes the exclusive floating-point upper bound for an unsigned width.
 *
 * @param bits Supported integer width (`1`, `16`, `32`, or `64`).
 * @return `2^bits` represented as a `double`.
 * @throws std::runtime_error If @p bits is unsupported.
 */
static double unsignedUpperExclusiveForBits(int bits) {
    switch (bits) {
        case 1:
            return 2.0;
        case 16:
            return 65536.0;
        case 32:
            return 4294967296.0;
        case 64:
            return 18446744073709551616.0;
        default:
            throw std::runtime_error("AArch64 lowering: unsupported unsigned fp cast width");
    }
}

/**
 * @brief Lowers integer/float conversion and checked-narrowing opcodes.
 *
 * Checked conversions emit range or round-trip guards that branch to deferred
 * per-function trap blocks. Plain conversions emit the corresponding AArch64
 * conversion MIR. Opcodes outside this helper's set return `false`.
 *
 * @param ins Candidate conversion instruction.
 * @param bbIn Source block used to materialize the input operand.
 * @param[in,out] ctx Function-wide lowering state.
 * @param bbOutIdx Index of the destination block in `ctx.mf.blocks`.
 * @return `true` when the conversion was successfully lowered; `false` for an
 *         unowned opcode or a missing/incompatible required operand.
 * @pre `bbOutIdx < ctx.mf.blocks.size()`.
 * @throws std::runtime_error For unsupported checked-cast widths or exhausted
 *         virtual-register space.
 */
static bool lowerCastOpcodes(const il::core::Instr &ins,
                             const il::core::BasicBlock &bbIn,
                             LoweringContext &ctx,
                             std::size_t bbOutIdx) {
    /// @brief Reacquires the indexed destination block after nested helper calls.
    auto bbOut = [&]() -> MBasicBlock & { return ctx.mf.blocks[bbOutIdx]; };

    switch (ins.op) {
        case Opcode::Zext1:
        case Opcode::Trunc1: {
            if (!ins.result || ins.operands.empty())
                return false;
            uint16_t sv = 0;
            RegClass scls = RegClass::GPR;
            if (!materializeValueToVReg(ins.operands[0],
                                        bbIn,
                                        ctx.ti,
                                        ctx.fb,
                                        bbOut(),
                                        ctx.tempVReg,
                                        ctx.tempRegClass,
                                        ctx.nextVRegId,
                                        sv,
                                        scls))
                return false;
            const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
            ctx.tempVReg[*ins.result] = dst;
            const uint16_t one = allocateNextVReg(ctx.nextVRegId);
            bbOut().instrs.push_back(
                MInstr{MOpcode::MovRI, {MOperand::vregOp(RegClass::GPR, one), MOperand::immOp(1)}});
            bbOut().instrs.push_back(MInstr{MOpcode::AndRRR,
                                            {MOperand::vregOp(RegClass::GPR, dst),
                                             MOperand::vregOp(RegClass::GPR, sv),
                                             MOperand::vregOp(RegClass::GPR, one)}});
            return true;
        }
        case Opcode::CastSiNarrowChk:
        case Opcode::CastUiNarrowChk: {
            if (!ins.result || ins.operands.empty())
                return false;
            const int bits = integerTypeBits(ins.type.kind);
            const int sh = 64 - bits;
            uint16_t sv = 0;
            RegClass scls = RegClass::GPR;
            if (!materializeValueToVReg(ins.operands[0],
                                        bbIn,
                                        ctx.ti,
                                        ctx.fb,
                                        bbOut(),
                                        ctx.tempVReg,
                                        ctx.tempRegClass,
                                        ctx.nextVRegId,
                                        sv,
                                        scls))
                return false;
            const uint16_t vt = allocateNextVReg(ctx.nextVRegId);
            if (sh > 0) {
                bbOut().instrs.push_back(MInstr{
                    MOpcode::MovRR,
                    {MOperand::vregOp(RegClass::GPR, vt), MOperand::vregOp(RegClass::GPR, sv)}});
                if (ins.op == Opcode::CastSiNarrowChk) {
                    bbOut().instrs.push_back(MInstr{MOpcode::LslRI,
                                                    {MOperand::vregOp(RegClass::GPR, vt),
                                                     MOperand::vregOp(RegClass::GPR, vt),
                                                     MOperand::immOp(sh)}});
                    bbOut().instrs.push_back(MInstr{MOpcode::AsrRI,
                                                    {MOperand::vregOp(RegClass::GPR, vt),
                                                     MOperand::vregOp(RegClass::GPR, vt),
                                                     MOperand::immOp(sh)}});
                } else {
                    bbOut().instrs.push_back(MInstr{MOpcode::LslRI,
                                                    {MOperand::vregOp(RegClass::GPR, vt),
                                                     MOperand::vregOp(RegClass::GPR, vt),
                                                     MOperand::immOp(sh)}});
                    bbOut().instrs.push_back(MInstr{MOpcode::LsrRI,
                                                    {MOperand::vregOp(RegClass::GPR, vt),
                                                     MOperand::vregOp(RegClass::GPR, vt),
                                                     MOperand::immOp(sh)}});
                }
            } else {
                bbOut().instrs.push_back(MInstr{
                    MOpcode::MovRR,
                    {MOperand::vregOp(RegClass::GPR, vt), MOperand::vregOp(RegClass::GPR, sv)}});
            }
            bbOut().instrs.push_back(
                MInstr{MOpcode::CmpRR,
                       {MOperand::vregOp(RegClass::GPR, vt), MOperand::vregOp(RegClass::GPR, sv)}});
            const std::string trapLabel = requestSharedTrapBlock(ctx, "ovf", "rt_trap_ovf");
            bbOut().instrs.push_back(
                MInstr{MOpcode::BCond, {MOperand::condOp("ne"), MOperand::labelOp(trapLabel)}});
            // The narrowed value in vt is the result — record it directly instead of
            // copying into a fresh vreg. The extra `MovRR dst, vt` was a copy-forwarding
            // hazard: pre-RA single-use copy forwarding could rewrite one use of a
            // multi-use result (e.g. a switch scrutinee compared across several cases)
            // and drop the copy, leaving the other uses pointing at an undefined vreg.
            ctx.tempVReg[*ins.result] = vt;
            ctx.tempRegClass[*ins.result] = RegClass::GPR;
            return true;
        }
        case Opcode::CastFpToSiRteChk:
        case Opcode::CastFpToUiRteChk: {
            if (!ins.result || ins.operands.empty())
                return false;
            uint16_t fv = 0;
            RegClass fcls = RegClass::FPR;
            if (!materializeValueToVReg(ins.operands[0],
                                        bbIn,
                                        ctx.ti,
                                        ctx.fb,
                                        bbOut(),
                                        ctx.tempVReg,
                                        ctx.tempRegClass,
                                        ctx.nextVRegId,
                                        fv,
                                        fcls))
                return false;
            if (fcls != RegClass::FPR)
                return false;

            const uint16_t rounded = allocateNextVReg(ctx.nextVRegId);
            bbOut().instrs.push_back(MInstr{
                MOpcode::FRintN,
                {MOperand::vregOp(RegClass::FPR, rounded), MOperand::vregOp(RegClass::FPR, fv)}});

            const std::string trapLabel = requestSharedTrapBlock(ctx, "fp_invalid", nullptr, 5);
            const std::string overflowLabel = requestSharedTrapBlock(ctx, "fp_ovf", nullptr, 4);

            // NaN becomes unordered; trap before any range comparisons.
            bbOut().instrs.push_back(MInstr{MOpcode::FCmpRR,
                                            {MOperand::vregOp(RegClass::FPR, rounded),
                                             MOperand::vregOp(RegClass::FPR, rounded)}});
            bbOut().instrs.push_back(
                MInstr{MOpcode::BCond, {MOperand::condOp("vs"), MOperand::labelOp(trapLabel)}});

            const int resultBits = integerTypeBits(ins.type.kind);
            if (ins.op == Opcode::CastFpToSiRteChk) {
                const uint16_t lowerBound =
                    materializeF64Constant(signedLowerBoundForBits(resultBits), ctx, bbOut());
                const uint16_t upperBound =
                    materializeF64Constant(signedUpperExclusiveForBits(resultBits), ctx, bbOut());
                bbOut().instrs.push_back(MInstr{MOpcode::FCmpRR,
                                                {MOperand::vregOp(RegClass::FPR, rounded),
                                                 MOperand::vregOp(RegClass::FPR, lowerBound)}});
                bbOut().instrs.push_back(MInstr{
                    MOpcode::BCond, {MOperand::condOp("lt"), MOperand::labelOp(overflowLabel)}});
                bbOut().instrs.push_back(MInstr{MOpcode::FCmpRR,
                                                {MOperand::vregOp(RegClass::FPR, rounded),
                                                 MOperand::vregOp(RegClass::FPR, upperBound)}});
                bbOut().instrs.push_back(MInstr{
                    MOpcode::BCond, {MOperand::condOp("ge"), MOperand::labelOp(overflowLabel)}});
            } else {
                const uint16_t zero = materializeF64Constant(0.0, ctx, bbOut());
                const uint16_t upperBound =
                    materializeF64Constant(unsignedUpperExclusiveForBits(resultBits), ctx, bbOut());
                bbOut().instrs.push_back(MInstr{MOpcode::FCmpRR,
                                                {MOperand::vregOp(RegClass::FPR, rounded),
                                                 MOperand::vregOp(RegClass::FPR, zero)}});
                bbOut().instrs.push_back(
                    MInstr{MOpcode::BCond, {MOperand::condOp("lt"), MOperand::labelOp(trapLabel)}});
                bbOut().instrs.push_back(MInstr{MOpcode::FCmpRR,
                                                {MOperand::vregOp(RegClass::FPR, rounded),
                                                 MOperand::vregOp(RegClass::FPR, upperBound)}});
                bbOut().instrs.push_back(MInstr{
                    MOpcode::BCond, {MOperand::condOp("ge"), MOperand::labelOp(overflowLabel)}});
            }

            const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
            ctx.tempVReg[*ins.result] = dst;
            ctx.tempRegClass[*ins.result] = RegClass::GPR;
            if (ins.op == Opcode::CastFpToSiRteChk) {
                bbOut().instrs.push_back(MInstr{MOpcode::FCvtZS,
                                                {MOperand::vregOp(RegClass::GPR, dst),
                                                 MOperand::vregOp(RegClass::FPR, rounded)}});
            } else {
                bbOut().instrs.push_back(MInstr{MOpcode::FCvtZU,
                                                {MOperand::vregOp(RegClass::GPR, dst),
                                                 MOperand::vregOp(RegClass::FPR, rounded)}});
            }
            return true;
        }
        case Opcode::CastSiToFp:
        case Opcode::CastUiToFp: {
            if (!ins.result || ins.operands.empty())
                return false;
            uint16_t sv = 0;
            RegClass scls = RegClass::GPR;
            if (!materializeValueToVReg(ins.operands[0],
                                        bbIn,
                                        ctx.ti,
                                        ctx.fb,
                                        bbOut(),
                                        ctx.tempVReg,
                                        ctx.tempRegClass,
                                        ctx.nextVRegId,
                                        sv,
                                        scls))
                return false;
            const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
            ctx.tempVReg[*ins.result] = dst;
            ctx.tempRegClass[*ins.result] = RegClass::FPR;
            if (ins.op == Opcode::CastSiToFp)
                bbOut().instrs.push_back(MInstr{
                    MOpcode::SCvtF,
                    {MOperand::vregOp(RegClass::FPR, dst), MOperand::vregOp(RegClass::GPR, sv)}});
            else
                bbOut().instrs.push_back(MInstr{
                    MOpcode::UCvtF,
                    {MOperand::vregOp(RegClass::FPR, dst), MOperand::vregOp(RegClass::GPR, sv)}});
            return true;
        }
        default:
            return false;
    }
}

/**
 * @brief Dispatches one IL instruction to AArch64 MIR lowering.
 *
 * @param ins Instruction to lower or recognize for a later pass.
 * @param bbIn Containing IL block used for operand materialization.
 * @param[in,out] ctx Shared lowering state and output function.
 * @param bbOutIdx Destination block index in `ctx.mf.blocks`.
 * @return `true` when the instruction is handled or intentionally deferred;
 *         `false` when the outer lowering loop retains responsibility.
 * @throws std::runtime_error For invalid handled operands, unlowered structured
 *         resume opcodes, or exhausted virtual-register space.
 */
bool lowerInstruction(const il::core::Instr &ins,
                      const il::core::BasicBlock &bbIn,
                      LoweringContext &ctx,
                      std::size_t bbOutIdx) {
    /**
     * @brief Reacquires the indexed output block without retaining a reference.
     *
     * @return Mutable destination block for the current instruction.
     */
    auto bbOut = [&]() -> MBasicBlock & { return ctx.mf.blocks[bbOutIdx]; };

    // Integer/float conversion opcodes are handled by a dedicated helper.
    if (lowerCastOpcodes(ins, bbIn, ctx, bbOutIdx))
        return true;

    switch (ins.op) {
        case Opcode::SRemChk0:
            return lowerSRemChk0(ins, bbIn, ctx, bbOut());
        case Opcode::SDivChk0:
            return lowerSDivChk0(ins, bbIn, ctx, bbOut());
        case Opcode::UDivChk0:
            return lowerUDivChk0(ins, bbIn, ctx, bbOut());
        case Opcode::URemChk0:
            return lowerURemChk0(ins, bbIn, ctx, bbOut());
        case Opcode::IdxChk:
            return lowerIdxChk(ins, bbIn, ctx, bbOut());
        case Opcode::Select:
            return lowerSelect(ins, bbIn, ctx, bbOut());
        case Opcode::SRem:
            return lowerSRem(ins, bbIn, ctx, bbOut());
        case Opcode::URem:
            return lowerURem(ins, bbIn, ctx, bbOut());
        case Opcode::FAdd:
        case Opcode::FSub:
        case Opcode::FMul:
        case Opcode::FDiv:
            return lowerFpArithmetic(ins, bbIn, ctx, bbOut());
        case Opcode::FCmpEQ:
        case Opcode::FCmpNE:
        case Opcode::FCmpLT:
        case Opcode::FCmpLE:
        case Opcode::FCmpGT:
        case Opcode::FCmpGE:
        case Opcode::FCmpOrd:
        case Opcode::FCmpUno:
            return lowerFpCompare(ins, bbIn, ctx, bbOut());
        case Opcode::Sitofp:
            return lowerSitofp(ins, bbIn, ctx, bbOut());
        case Opcode::Fptosi:
            return lowerFptosi(ins, bbIn, ctx, bbOut());
        case Opcode::ConstF64: {
            // Lower const.f64 by materializing a floating-point constant.
            // We load the 64-bit IEEE-754 representation into a GPR and then
            // use fmov to transfer to FPR.  Operands may arrive as ConstFloat
            // (when parsed from a float literal like 1.0) or ConstInt (when
            // parsed from the integer bit-pattern encoding used by the IL
            // serializer for programmatically-constructed modules).
            if (!ins.result || ins.operands.empty())
                return false;

            const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
            ctx.tempVReg[*ins.result] = dst;
            ctx.tempRegClass[*ins.result] = RegClass::FPR;

            uint64_t bits = 0;
            if (ins.operands[0].kind == il::core::Value::Kind::ConstFloat) {
                bits = zanna::codegen::common::f64Bits(ins.operands[0].f64);
            } else if (ins.operands[0].kind == il::core::Value::Kind::ConstInt) {
                // Integer operand holds IEEE-754 bit pattern directly.
                bits = static_cast<uint64_t>(ins.operands[0].i64);
            } else {
                return false;
            }

            const uint16_t tmpGpr = allocateNextVReg(ctx.nextVRegId);
            bbOut().instrs.push_back(MInstr{MOpcode::MovRI,
                                            {MOperand::vregOp(RegClass::GPR, tmpGpr),
                                             MOperand::immOp(static_cast<long long>(bits))}});
            bbOut().instrs.push_back(MInstr{
                MOpcode::FMovGR,
                {MOperand::vregOp(RegClass::FPR, dst), MOperand::vregOp(RegClass::GPR, tmpGpr)}});
            return true;
        }
        case Opcode::ConstNull: {
            // const_null produces a null pointer (0)
            if (!ins.result)
                return false;
            const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
            ctx.tempVReg[*ins.result] = dst;
            ctx.tempRegClass[*ins.result] = RegClass::GPR;
            bbOut().instrs.push_back(
                MInstr{MOpcode::MovRI, {MOperand::vregOp(RegClass::GPR, dst), MOperand::immOp(0)}});
            return true;
        }
        case Opcode::GAddr: {
            // gaddr @symbol produces the address of a global variable
            if (!ins.result || ins.operands.empty())
                return false;
            if (ins.operands[0].kind != il::core::Value::Kind::GlobalAddr)
                return false;
            const std::string sym = mapExternalSymbol(ins.operands[0].str);
            // Materialize address of global symbol using adrp+add
            const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
            ctx.tempVReg[*ins.result] = dst;
            ctx.tempRegClass[*ins.result] = RegClass::GPR;
            bbOut().instrs.push_back(MInstr{
                MOpcode::AdrPage, {MOperand::vregOp(RegClass::GPR, dst), MOperand::labelOp(sym)}});
            bbOut().instrs.push_back(MInstr{MOpcode::AddPageOff,
                                            {MOperand::vregOp(RegClass::GPR, dst),
                                             MOperand::vregOp(RegClass::GPR, dst),
                                             MOperand::labelOp(sym)}});
            return true;
        }
        case Opcode::ConstStr: {
            // Lower const_str to produce a runtime string handle from a pooled literal.
            // This must be lowered proactively (not demand-lowered) when the result
            // is a cross-block temp that will be spilled.
            if (!ins.result || ins.operands.empty())
                return false;
            if (ins.operands[0].kind != il::core::Value::Kind::GlobalAddr)
                return false;
            const std::string &sym = ins.operands[0].str;
            const uint16_t dst = emitConstStrGlobalToVReg(
                sym, ctx.stringLiteralByteLengths, bbOut(), ctx.nextVRegId);
            ctx.tempVReg[*ins.result] = dst;
            ctx.tempRegClass[*ins.result] = RegClass::GPR;
            return true;
        }
        case Opcode::AddrOf: {
            if (!ins.result || ins.operands.empty())
                return false;

            if (ins.operands[0].kind == il::core::Value::Kind::GlobalAddr) {
                const std::string sym = mapExternalSymbol(ins.operands[0].str);
                const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
                ctx.tempVReg[*ins.result] = dst;
                ctx.tempRegClass[*ins.result] = RegClass::GPR;
                bbOut().instrs.push_back(
                    MInstr{MOpcode::AdrPage,
                           {MOperand::vregOp(RegClass::GPR, dst), MOperand::labelOp(sym)}});
                bbOut().instrs.push_back(MInstr{MOpcode::AddPageOff,
                                                {MOperand::vregOp(RegClass::GPR, dst),
                                                 MOperand::vregOp(RegClass::GPR, dst),
                                                 MOperand::labelOp(sym)}});
                return true;
            }

            if (ins.operands[0].kind == il::core::Value::Kind::Temp) {
                const int offset = ctx.fb.localOffset(ins.operands[0].id);
                if (offset != 0) {
                    const uint16_t dst = allocateNextVReg(ctx.nextVRegId);
                    ctx.tempVReg[*ins.result] = dst;
                    ctx.tempRegClass[*ins.result] = RegClass::GPR;
                    bbOut().instrs.push_back(
                        MInstr{MOpcode::AddFpImm,
                               {MOperand::vregOp(RegClass::GPR, dst), MOperand::immOp(offset)}});
                    return true;
                }
            }

            return false;
        }
        case Opcode::Store:
            return lowerStore(ins, bbIn, ctx, bbOut());
        case Opcode::GEP:
            return lowerGEP(ins, bbIn, ctx, bbOut());
        case Opcode::Load:
            return lowerLoad(ins, bbIn, ctx, bbOut());
        case Opcode::Call:
            lowerCall(ins, bbIn, ctx, bbOut());
            return true;
        case Opcode::CallIndirect:
            return lowerCallIndirect(ins, bbIn, ctx, bbOut());
        case Opcode::Ret:
            lowerRet(ins, bbIn, ctx, bbOut());
            return true;
        case Opcode::Alloca:
            // Alloca is handled during frame building, no MIR needed here
            return true;
        case Opcode::Br:
        case Opcode::CBr:
        case Opcode::SwitchI32:
            // Terminators are lowered in a separate pass after all instructions
            return true;

        // === Structured Error Handling ===
        // NativeEHLowering rewrites structured EH into helper calls and plain
        // control flow before backend lowering. If raw EH markers still reach
        // this stage, treat them as inert markers rather than generating new
        // machine semantics.
        case Opcode::EhPush:
        case Opcode::EhPop:
        case Opcode::EhEntry:
            return true;

        case Opcode::Trap:
            if (std::any_of(bbOut().instrs.begin(), bbOut().instrs.end(), isNoReturnCall))
                return true;
            if (ins.operands.empty()) {
                bbOut().instrs.push_back(
                    MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(0)}});
            } else {
                moveValueToArg(ins.operands[0], bbIn, ctx, PhysReg::X0, bbOut(), "trap message");
            }
            bbOut().instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp("rt_trap_string")}});
            return true;

        case Opcode::TrapKind:
            bbOut().instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp("rt_trap_get_kind")}});
            captureGprCallResult(ins, ctx, bbOut());
            return true;

        // trap.err constructs the current error payload and returns an opaque token.
        case Opcode::TrapErr: {
            if (ins.operands.size() < 2)
                throw std::runtime_error(
                    "AArch64 lowering: trap.err expects code and text operands");
            moveValueToArg(ins.operands[0], bbIn, ctx, PhysReg::X0, bbOut(), "trap.err code");
            moveValueToArg(ins.operands[1], bbIn, ctx, PhysReg::X1, bbOut(), "trap.err message");
            bbOut().instrs.push_back(
                MInstr{MOpcode::Bl, {MOperand::labelOp("rt_trap_error_make")}});
            captureGprCallResult(ins, ctx, bbOut());
            return true;
        }

        // Error field accessors call runtime TLS bridge functions to retrieve
        // trap classification stored by rt_trap_fields_set() before the trap.
        case Opcode::ErrGetKind:
        case Opcode::ErrGetCode:
        case Opcode::ErrGetLine: {
            const char *rtFunc = nullptr;
            switch (ins.op) {
                case Opcode::ErrGetKind:
                    rtFunc = "rt_trap_get_kind";
                    break;
                case Opcode::ErrGetCode:
                    rtFunc = "rt_trap_get_code";
                    break;
                case Opcode::ErrGetLine:
                    rtFunc = "rt_trap_get_line";
                    break;
                default:
                    rtFunc = "rt_trap_get_kind";
                    break;
            }
            bbOut().instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp(rtFunc)}});
            captureGprCallResult(ins, ctx, bbOut());
            return true;
        }
        case Opcode::ErrGetIp: {
            bbOut().instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp("rt_trap_get_ip")}});
            captureGprCallResult(ins, ctx, bbOut());
            return true;
        }
        case Opcode::ErrGetMsg: {
            bbOut().instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp("rt_throw_msg_get")}});
            captureGprCallResult(ins, ctx, bbOut());
            return true;
        }

        // resume.label is a branch to an explicit target label.
        // The resume token operand is ignored in native codegen.
        // Handled as a terminator in TerminatorLowering.cpp.
        case Opcode::TrapFromErr: {
            if (ins.operands.empty()) {
                bbOut().instrs.push_back(
                    MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(0)}});
            } else {
                moveValueToArg(
                    ins.operands[0], bbIn, ctx, PhysReg::X0, bbOut(), "trap.from_err code");
            }
            bbOut().instrs.push_back(
                MInstr{MOpcode::Bl, {MOperand::labelOp("rt_trap_raise_error")}});
            return true;
        }
        case Opcode::ResumeLabel:
            return true;

        // resume.same / resume.next must have been lowered away by the shared
        // native EH rewrite before they reach backend instruction selection.
        case Opcode::ResumeSame:
        case Opcode::ResumeNext:
            throw std::runtime_error(
                std::string("AArch64 lowering received raw ") + il::core::toString(ins.op) +
                " after NativeEHLowering; structured EH rewrite is incomplete.");

        default:
            // Opcode not handled - caller should process
            return false;
    }
}

} // namespace zanna::codegen::aarch64
