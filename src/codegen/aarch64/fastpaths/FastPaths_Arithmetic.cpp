//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/fastpaths/FastPaths_Arithmetic.cpp
// Purpose: Fast-path pattern matching for arithmetic operations.
//          Handles integer RR ops, integer RI ops, comparisons, division/modulo,
//          negation, and two-op chains where operands are entry parameters or
//          constants and results flow directly to a ret.
// Key invariants:
//   - Operands must be entry parameters or constant immediates.
//   - Result must flow directly to a ret instruction.
//   - Parameters must fit within the ABI register argument limit.
// Ownership/Lifetime:
//   - Stateless free functions; FastPathContext is borrowed for the call duration.
// Links: src/codegen/aarch64/fastpaths/FastPathsInternal.hpp,
//        src/codegen/aarch64/A64ImmediateUtils.hpp
//
//===----------------------------------------------------------------------===//

#include "FastPathsInternal.hpp"
#include "codegen/aarch64/A64ImmediateUtils.hpp"

/**
 * @file
 * @brief Implements narrow AArch64 fast paths for arithmetic-and-return IL shapes.
 *
 * Matching is intentionally conservative: operands must resolve directly to
 * entry ABI registers or accepted constants, and the computed value must feed
 * the return. Checked sub-width arithmetic is rejected for generic lowering.
 */

namespace zanna::codegen::aarch64::fastpaths {

using il::core::Opcode;

/**
 * @brief Tests whether an IL opcode requests checked integer overflow.
 * @param op Opcode to classify.
 * @return `true` for `IAddOvf`, `ISubOvf`, or `IMulOvf`.
 */
static bool isCheckedOverflowOp(Opcode op) {
    return op == Opcode::IAddOvf || op == Opcode::ISubOvf || op == Opcode::IMulOvf;
}

/**
 * @brief Tests whether checked arithmetic requires sub-64-bit overflow semantics.
 * @param instr Candidate arithmetic instruction.
 * @return `true` for a checked operation whose result type is not `I64`.
 */
static bool isSubWidthCheckedOverflow(const il::core::Instr &instr) {
    return isCheckedOverflowOp(instr.op) && instr.type.kind != il::core::Type::Kind::I64;
}

/**
 * @brief Attempts a binary register/register operation returned immediately.
 *
 * Accepted integer arithmetic, bitwise, and comparison operands must be entry
 * parameters in the GPR argument bank. Inputs are normalized through `x0/x1`
 * before the result is produced in `x0`.
 *
 * @param[in,out] ctx Fast-path state and output MIR.
 * @return Completed MIR function on a match, otherwise `std::nullopt`.
 */
static std::optional<MFunction> tryRegRegRetFastPath(FastPathContext &ctx) {
    const auto &bb = ctx.fn.blocks.front();
    auto &bbMir = ctx.bbOut(0);

    if (ctx.fn.blocks.size() == 1 && bb.instructions.size() >= 2 && bb.params.size() >= 2) {
        const auto &opI = bb.instructions[bb.instructions.size() - 2];
        const auto &retI = bb.instructions.back();
        if (!isSubWidthCheckedOverflow(opI) &&
            (opI.op == Opcode::Add || opI.op == Opcode::IAddOvf || opI.op == Opcode::Sub ||
             opI.op == Opcode::ISubOvf || opI.op == Opcode::Mul || opI.op == Opcode::IMulOvf ||
             opI.op == Opcode::And || opI.op == Opcode::Or || opI.op == Opcode::Xor ||
             opI.op == Opcode::ICmpEq || opI.op == Opcode::ICmpNe || opI.op == Opcode::SCmpLT ||
             opI.op == Opcode::SCmpLE || opI.op == Opcode::SCmpGT || opI.op == Opcode::SCmpGE ||
             opI.op == Opcode::UCmpLT || opI.op == Opcode::UCmpLE || opI.op == Opcode::UCmpGT ||
             opI.op == Opcode::UCmpGE) &&
            retI.op == Opcode::Ret && opI.result && !retI.operands.empty()) {
            const auto &retV = retI.operands[0];
            if (retV.kind == il::core::Value::Kind::Temp && retV.id == *opI.result &&
                opI.operands.size() == 2 && opI.operands[0].kind == il::core::Value::Kind::Temp &&
                opI.operands[1].kind == il::core::Value::Kind::Temp) {
                const int idx0 = indexOfParam(bb, opI.operands[0].id);
                const int idx1 = indexOfParam(bb, opI.operands[1].id);
                if (idx0 >= 0 && idx1 >= 0 && static_cast<std::size_t>(idx0) < kMaxGPRArgs &&
                    static_cast<std::size_t>(idx1) < kMaxGPRArgs) {
                    const PhysReg src0 = ctx.argOrder[static_cast<size_t>(idx0)];
                    const PhysReg src1 = ctx.argOrder[static_cast<size_t>(idx1)];
                    // Normalize to x0,x1 — skip if already in place
                    if (src0 != PhysReg::X0 || src1 != PhysReg::X1) {
                        bbMir.instrs.push_back(MInstr{
                            MOpcode::MovRR, {MOperand::regOp(kScratchGPR), MOperand::regOp(src1)}});
                        bbMir.instrs.push_back(MInstr{
                            MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(src0)}});
                        bbMir.instrs.push_back(
                            MInstr{MOpcode::MovRR,
                                   {MOperand::regOp(PhysReg::X1), MOperand::regOp(kScratchGPR)}});
                    }
                    switch (opI.op) {
                        case Opcode::Add:
                            bbMir.instrs.push_back(MInstr{MOpcode::AddRRR,
                                                          {MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X1)}});
                            break;
                        case Opcode::IAddOvf:
                            bbMir.instrs.push_back(MInstr{MOpcode::AddOvfRRR,
                                                          {MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X1)}});
                            break;
                        case Opcode::Sub:
                            bbMir.instrs.push_back(MInstr{MOpcode::SubRRR,
                                                          {MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X1)}});
                            break;
                        case Opcode::ISubOvf:
                            bbMir.instrs.push_back(MInstr{MOpcode::SubOvfRRR,
                                                          {MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X1)}});
                            break;
                        case Opcode::Mul:
                            bbMir.instrs.push_back(MInstr{MOpcode::MulRRR,
                                                          {MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X1)}});
                            break;
                        case Opcode::IMulOvf:
                            bbMir.instrs.push_back(MInstr{MOpcode::MulOvfRRR,
                                                          {MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X1)}});
                            break;
                        case Opcode::And:
                            bbMir.instrs.push_back(MInstr{MOpcode::AndRRR,
                                                          {MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X1)}});
                            break;
                        case Opcode::Or:
                            bbMir.instrs.push_back(MInstr{MOpcode::OrrRRR,
                                                          {MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X1)}});
                            break;
                        case Opcode::Xor:
                            bbMir.instrs.push_back(MInstr{MOpcode::EorRRR,
                                                          {MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X1)}});
                            break;
                        case Opcode::ICmpEq:
                        case Opcode::ICmpNe:
                        case Opcode::SCmpLT:
                        case Opcode::SCmpLE:
                        case Opcode::SCmpGT:
                        case Opcode::SCmpGE:
                        case Opcode::UCmpLT:
                        case Opcode::UCmpLE:
                        case Opcode::UCmpGT:
                        case Opcode::UCmpGE:
                            bbMir.instrs.push_back(MInstr{
                                MOpcode::CmpRR,
                                {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X1)}});
                            bbMir.instrs.push_back(
                                MInstr{MOpcode::Cset,
                                       {MOperand::regOp(PhysReg::X0),
                                        MOperand::condOp(lookupCondition(opI.op))}});
                            break;
                        default:
                            break;
                    }
                    bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
                    return ctx.mf;
                }
            }
        }
    }
    return std::nullopt;
}

/**
 * @brief Attempts supported integer arithmetic, comparison, division, negation, and chain shapes.
 *
 * @param[in,out] ctx Fast-path state and output MIR.
 * @return Completed MIR function on the first matching shape, otherwise
 *         `std::nullopt`.
 */
std::optional<MFunction> tryIntArithmeticFastPaths(FastPathContext &ctx) {
    if (ctx.fn.blocks.empty())
        return std::nullopt;

    if (auto result = tryRegRegRetFastPath(ctx))
        return result;

    const auto &bb = ctx.fn.blocks.front();
    auto &bbMir = ctx.bbOut(0);

    // =========================================================================
    // RI ops: add/sub/shl/lshr/ashr with immediate
    // =========================================================================
    // Pattern: binop %param, #imm -> %r; ret %r
    if (ctx.fn.blocks.size() == 1 && bb.instructions.size() >= 2 && !bb.params.empty()) {
        const auto &binI = bb.instructions[bb.instructions.size() - 2];
        const auto &retI = bb.instructions.back();
        const bool isAdd = (binI.op == Opcode::Add);
        const bool isAddOvf = (binI.op == Opcode::IAddOvf);
        const bool isSub = (binI.op == Opcode::Sub);
        const bool isSubOvf = (binI.op == Opcode::ISubOvf);
        const bool isShl = (binI.op == Opcode::Shl);
        const bool isLShr = (binI.op == Opcode::LShr);
        const bool isAShr = (binI.op == Opcode::AShr);
        const bool isICmpImm = (lookupCondition(binI.op) != nullptr);

        if (!isSubWidthCheckedOverflow(binI) &&
            (isAdd || isAddOvf || isSub || isSubOvf || isShl || isLShr || isAShr) &&
            retI.op == Opcode::Ret && binI.result && !retI.operands.empty() &&
            binI.operands.size() == 2) {
            const auto &retV = retI.operands[0];
            if (retV.kind == il::core::Value::Kind::Temp && retV.id == *binI.result) {
                const auto &o0 = binI.operands[0];
                const auto &o1 = binI.operands[1];
                /**
                 * @brief Emits the selected parameter/immediate arithmetic and return sequence.
                 * @param paramIndex Integer argument-bank index.
                 * @param imm Signed immediate operand.
                 */
                auto emitImm = [&](unsigned paramIndex, long long imm) {
                    const PhysReg src = ctx.argOrder[paramIndex];
                    bbMir.instrs.push_back(MInstr{
                        MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(src)}});
                    if (isAdd || isAddOvf || isSub || isSubOvf) {
                        emitLegalizedSignedImmArith(
                            bbMir,
                            MOperand::regOp(PhysReg::X0),
                            MOperand::regOp(PhysReg::X0),
                            imm,
                            (isAdd || isAddOvf) ? SignedImmArithKind::Add : SignedImmArithKind::Sub,
                            (isAddOvf || isSubOvf) ? MOpcode::AddOvfRI : MOpcode::AddRI,
                            (isAddOvf || isSubOvf) ? MOpcode::SubOvfRI : MOpcode::SubRI,
                            (isAddOvf || isSubOvf) ? MOpcode::AddOvfRRR : MOpcode::AddRRR,
                            (isAddOvf || isSubOvf) ? MOpcode::SubOvfRRR : MOpcode::SubRRR,
                            /// @brief Materializes a non-encodable arithmetic immediate in `x16`.
                            [&](long long materializedImm) {
                                const MOperand scratch = MOperand::regOp(PhysReg::X16);
                                bbMir.instrs.push_back(MInstr{
                                    MOpcode::MovRI, {scratch, MOperand::immOp(materializedImm)}});
                                return scratch;
                            });
                    } else if (isShl)
                        bbMir.instrs.push_back(MInstr{MOpcode::LslRI,
                                                      {MOperand::regOp(PhysReg::X0),
                                                       MOperand::regOp(PhysReg::X0),
                                                       MOperand::immOp(imm)}});
                    else if (isLShr)
                        bbMir.instrs.push_back(MInstr{MOpcode::LsrRI,
                                                      {MOperand::regOp(PhysReg::X0),
                                                       MOperand::regOp(PhysReg::X0),
                                                       MOperand::immOp(imm)}});
                    else if (isAShr)
                        bbMir.instrs.push_back(MInstr{MOpcode::AsrRI,
                                                      {MOperand::regOp(PhysReg::X0),
                                                       MOperand::regOp(PhysReg::X0),
                                                       MOperand::immOp(imm)}});
                    bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
                };
                if (o0.kind == il::core::Value::Kind::Temp &&
                    o1.kind == il::core::Value::Kind::ConstInt) {
                    for (size_t i = 0; i < bb.params.size(); ++i)
                        if (i < kMaxGPRArgs && bb.params[i].id == o0.id) {
                            emitImm(static_cast<unsigned>(i), o1.i64);
                            return ctx.mf;
                        }
                }
                if (o1.kind == il::core::Value::Kind::Temp &&
                    o0.kind == il::core::Value::Kind::ConstInt) {
                    // Only commutative operations can swap operands.
                    // Shifts (shl, lshr, ashr) are NOT commutative: `5 << x` != `x << 5`.
                    if (isAdd || isAddOvf) {
                        for (size_t i = 0; i < bb.params.size(); ++i)
                            if (i < kMaxGPRArgs && bb.params[i].id == o1.id) {
                                emitImm(static_cast<unsigned>(i), o0.i64);
                                return ctx.mf;
                            }
                    }
                }
            }
        }

        // Immediate comparisons
        if (isICmpImm && retI.op == Opcode::Ret && binI.result && !retI.operands.empty() &&
            binI.operands.size() == 2) {
            const auto &retV = retI.operands[0];
            if (retV.kind == il::core::Value::Kind::Temp && retV.id == *binI.result) {
                const auto &o0 = binI.operands[0];
                const auto &o1 = binI.operands[1];
                /**
                 * @brief Emits an immediate comparison whose boolean result is returned.
                 * @param paramIndex Integer argument-bank index.
                 * @param imm Comparison immediate.
                 */
                auto emitCmpImm = [&](unsigned paramIndex, long long imm) {
                    const PhysReg src = ctx.argOrder[paramIndex];
                    if (src != PhysReg::X0)
                        bbMir.instrs.push_back(MInstr{
                            MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(src)}});
                    bbMir.instrs.push_back(MInstr{
                        MOpcode::CmpRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(imm)}});
                    bbMir.instrs.push_back(MInstr{MOpcode::Cset,
                                                  {MOperand::regOp(PhysReg::X0),
                                                   MOperand::condOp(lookupCondition(binI.op))}});
                    bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
                };
                if (o0.kind == il::core::Value::Kind::Temp &&
                    o1.kind == il::core::Value::Kind::ConstInt) {
                    for (size_t i = 0; i < bb.params.size(); ++i)
                        if (i < kMaxGPRArgs && bb.params[i].id == o0.id) {
                            emitCmpImm(static_cast<unsigned>(i), o1.i64);
                            return ctx.mf;
                        }
                }
            }
        }
    }

    // =========================================================================
    // Division/Remainder RR ops
    // =========================================================================
    // Pattern: divop %p0, %p1 -> %r; ret %r
    // Handles: sdiv, udiv (srem/urem require msub which is more complex)
    if (ctx.fn.blocks.size() == 1 && bb.instructions.size() >= 2 && bb.params.size() >= 2) {
        const auto &opI = bb.instructions[bb.instructions.size() - 2];
        const auto &retI = bb.instructions.back();
        const bool isSDiv = (opI.op == Opcode::SDiv);
        const bool isUDiv = (opI.op == Opcode::UDiv);
        if ((isSDiv || isUDiv) && retI.op == Opcode::Ret && opI.result && !retI.operands.empty() &&
            opI.operands.size() == 2) {
            const auto &retV = retI.operands[0];
            if (retV.kind == il::core::Value::Kind::Temp && retV.id == *opI.result &&
                opI.operands[0].kind == il::core::Value::Kind::Temp &&
                opI.operands[1].kind == il::core::Value::Kind::Temp) {
                const int idx0 = indexOfParam(bb, opI.operands[0].id);
                const int idx1 = indexOfParam(bb, opI.operands[1].id);
                if (idx0 >= 0 && idx1 >= 0 && static_cast<std::size_t>(idx0) < kMaxGPRArgs &&
                    static_cast<std::size_t>(idx1) < kMaxGPRArgs) {
                    const PhysReg src0 = ctx.argOrder[static_cast<std::size_t>(idx0)];
                    const PhysReg src1 = ctx.argOrder[static_cast<std::size_t>(idx1)];
                    // Normalize to x0,x1 using scratch
                    bbMir.instrs.push_back(MInstr{
                        MOpcode::MovRR, {MOperand::regOp(kScratchGPR), MOperand::regOp(src1)}});
                    bbMir.instrs.push_back(MInstr{
                        MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(src0)}});
                    bbMir.instrs.push_back(
                        MInstr{MOpcode::MovRR,
                               {MOperand::regOp(PhysReg::X1), MOperand::regOp(kScratchGPR)}});
                    if (isSDiv)
                        bbMir.instrs.push_back(MInstr{MOpcode::SDivRRR,
                                                      {MOperand::regOp(PhysReg::X0),
                                                       MOperand::regOp(PhysReg::X0),
                                                       MOperand::regOp(PhysReg::X1)}});
                    else
                        bbMir.instrs.push_back(MInstr{MOpcode::UDivRRR,
                                                      {MOperand::regOp(PhysReg::X0),
                                                       MOperand::regOp(PhysReg::X0),
                                                       MOperand::regOp(PhysReg::X1)}});
                    bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
                    ctx.fb.finalize();
                    return ctx.mf;
                }
            }
        }
    }

    // =========================================================================
    // Negation: sub 0, %param
    // =========================================================================
    // Pattern: sub 0, %p0 -> %r; ret %r (integer negation)
    // Emits: neg x0, srcReg; ret
    if (ctx.fn.blocks.size() == 1 && bb.instructions.size() >= 2 && !bb.params.empty()) {
        const auto &subI = bb.instructions[bb.instructions.size() - 2];
        const auto &retI = bb.instructions.back();
        if (subI.op == Opcode::Sub && retI.op == Opcode::Ret && subI.result &&
            !retI.operands.empty() && subI.operands.size() == 2) {
            const auto &retV = retI.operands[0];
            const auto &o0 = subI.operands[0];
            const auto &o1 = subI.operands[1];
            // Check for sub 0, %param pattern
            if (retV.kind == il::core::Value::Kind::Temp && retV.id == *subI.result &&
                o0.kind == il::core::Value::Kind::ConstInt && o0.i64 == 0 &&
                o1.kind == il::core::Value::Kind::Temp) {
                int pIdx = indexOfParam(bb, o1.id);
                if (pIdx >= 0 && static_cast<std::size_t>(pIdx) < kMaxGPRArgs) {
                    const PhysReg src = ctx.argOrder[static_cast<std::size_t>(pIdx)];
                    // neg x0, src  via: mov x0, #0; sub x0, x0, src
                    bbMir.instrs.push_back(
                        MInstr{MOpcode::MovRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(0)}});
                    bbMir.instrs.push_back(MInstr{MOpcode::SubRRR,
                                                  {MOperand::regOp(PhysReg::X0),
                                                   MOperand::regOp(PhysReg::X0),
                                                   MOperand::regOp(src)}});
                    bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
                    ctx.fb.finalize();
                    return ctx.mf;
                }
            }
        }
    }

    // =========================================================================
    // Two-op arithmetic chain
    // =========================================================================
    // Pattern: %t1 = op %p0, %p1; %t2 = op %t1, %p2; ret %t2
    // Common in expressions like (a + b) * c
    if (ctx.fn.blocks.size() == 1 && bb.instructions.size() == 3 && bb.params.size() >= 3) {
        const auto &op1I = bb.instructions[0];
        const auto &op2I = bb.instructions[1];
        const auto &retI = bb.instructions[2];

        // Check basic structure
        if (retI.op == Opcode::Ret && !retI.operands.empty() && op1I.result && op2I.result &&
            retI.operands[0].kind == il::core::Value::Kind::Temp &&
            retI.operands[0].id == *op2I.result) {
            // Check that op2 uses op1 result as first operand and a param as second
            if (op2I.operands.size() == 2 && op2I.operands[0].kind == il::core::Value::Kind::Temp &&
                op2I.operands[0].id == *op1I.result &&
                op2I.operands[1].kind == il::core::Value::Kind::Temp) {
                // Check that op1 uses two params
                if (op1I.operands.size() == 2 &&
                    op1I.operands[0].kind == il::core::Value::Kind::Temp &&
                    op1I.operands[1].kind == il::core::Value::Kind::Temp) {
                    int p0 = indexOfParam(bb, op1I.operands[0].id);
                    int p1 = indexOfParam(bb, op1I.operands[1].id);
                    int p2 = indexOfParam(bb, op2I.operands[1].id);

                    if (p0 >= 0 && p1 >= 0 && p2 >= 0 &&
                        static_cast<std::size_t>(p0) < kMaxGPRArgs &&
                        static_cast<std::size_t>(p1) < kMaxGPRArgs &&
                        static_cast<std::size_t>(p2) < kMaxGPRArgs) {
                        // Only handle simple ops for the chain
                        /**
                         * @brief Maps an unchecked chain operation to register/register MIR.
                         * @param op IL opcode to map.
                         * @return MIR opcode, or `std::nullopt` when unsupported.
                         */
                        auto mapOp = [](Opcode op) -> std::optional<MOpcode> {
                            switch (op) {
                                case Opcode::Add:
                                case Opcode::IAddOvf:
                                    return MOpcode::AddRRR;
                                case Opcode::Sub:
                                case Opcode::ISubOvf:
                                    return MOpcode::SubRRR;
                                case Opcode::Mul:
                                case Opcode::IMulOvf:
                                    return MOpcode::MulRRR;
                                case Opcode::And:
                                    return MOpcode::AndRRR;
                                case Opcode::Or:
                                    return MOpcode::OrrRRR;
                                case Opcode::Xor:
                                    return MOpcode::EorRRR;
                                default:
                                    return std::nullopt;
                            }
                        };

                        auto mop1 = isCheckedOverflowOp(op1I.op) ? std::nullopt : mapOp(op1I.op);
                        auto mop2 = isCheckedOverflowOp(op2I.op) ? std::nullopt : mapOp(op2I.op);
                        if (mop1 && mop2) {
                            const PhysReg r0 = ctx.argOrder[static_cast<std::size_t>(p0)];
                            const PhysReg r1 = ctx.argOrder[static_cast<std::size_t>(p1)];
                            const PhysReg r2 = ctx.argOrder[static_cast<std::size_t>(p2)];

                            // First op: x0 = op1(r0, r1)
                            bbMir.instrs.push_back(MInstr{*mop1,
                                                          {MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(r0),
                                                           MOperand::regOp(r1)}});
                            // Second op: x0 = op2(x0, r2)
                            bbMir.instrs.push_back(MInstr{*mop2,
                                                          {MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(PhysReg::X0),
                                                           MOperand::regOp(r2)}});
                            bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
                            ctx.fb.finalize();
                            return ctx.mf;
                        }
                    }
                }
            }
        }
    }

    return std::nullopt;
}

// =========================================================================
// Floating-point RR ops
// =========================================================================
// Pattern: fop %p0, %p1 -> %r; ret %r
// Handles: fadd, fsub, fmul, fdiv

/**
 * @brief Attempts a binary FP operation on two entry parameters returned immediately.
 *
 * Inputs must fit the FP argument bank. They are normalized through `v0/v1`,
 * and the result remains in the ABI FP return register `v0`.
 *
 * @param[in,out] ctx Fast-path state and output MIR.
 * @return Completed MIR function on a match, otherwise `std::nullopt`.
 */
std::optional<MFunction> tryFPArithmeticFastPaths(FastPathContext &ctx) {
    if (ctx.fn.blocks.empty())
        return std::nullopt;

    const auto &bb = ctx.fn.blocks.front();
    auto &bbMir = ctx.bbOut(0);

    if (ctx.fn.blocks.size() == 1 && bb.instructions.size() >= 2 && bb.params.size() >= 2) {
        const auto &opI = bb.instructions[bb.instructions.size() - 2];
        const auto &retI = bb.instructions.back();
        const bool isFAdd = (opI.op == Opcode::FAdd);
        const bool isFSub = (opI.op == Opcode::FSub);
        const bool isFMul = (opI.op == Opcode::FMul);
        const bool isFDiv = (opI.op == Opcode::FDiv);
        if ((isFAdd || isFSub || isFMul || isFDiv) && retI.op == Opcode::Ret && opI.result &&
            !retI.operands.empty()) {
            const auto &retV = retI.operands[0];
            if (retV.kind == il::core::Value::Kind::Temp && retV.id == *opI.result &&
                opI.operands.size() == 2 && opI.operands[0].kind == il::core::Value::Kind::Temp &&
                opI.operands[1].kind == il::core::Value::Kind::Temp) {
                const int idx0 = indexOfParam(bb, opI.operands[0].id);
                const int idx1 = indexOfParam(bb, opI.operands[1].id);
                if (idx0 >= 0 && idx1 >= 0 && static_cast<std::size_t>(idx0) < kMaxFPRArgs &&
                    static_cast<std::size_t>(idx1) < kMaxFPRArgs) {
                    const PhysReg src0 = ctx.ti.f64ArgOrder[static_cast<std::size_t>(idx0)];
                    const PhysReg src1 = ctx.ti.f64ArgOrder[static_cast<std::size_t>(idx1)];
                    // Normalize to d0,d1 using FPR scratch register
                    bbMir.instrs.push_back(MInstr{
                        MOpcode::FMovRR, {MOperand::regOp(kScratchFPR), MOperand::regOp(src1)}});
                    bbMir.instrs.push_back(MInstr{
                        MOpcode::FMovRR, {MOperand::regOp(PhysReg::V0), MOperand::regOp(src0)}});
                    bbMir.instrs.push_back(
                        MInstr{MOpcode::FMovRR,
                               {MOperand::regOp(PhysReg::V1), MOperand::regOp(kScratchFPR)}});
                    if (isFAdd)
                        bbMir.instrs.push_back(MInstr{MOpcode::FAddRRR,
                                                      {MOperand::regOp(PhysReg::V0),
                                                       MOperand::regOp(PhysReg::V0),
                                                       MOperand::regOp(PhysReg::V1)}});
                    else if (isFSub)
                        bbMir.instrs.push_back(MInstr{MOpcode::FSubRRR,
                                                      {MOperand::regOp(PhysReg::V0),
                                                       MOperand::regOp(PhysReg::V0),
                                                       MOperand::regOp(PhysReg::V1)}});
                    else if (isFMul)
                        bbMir.instrs.push_back(MInstr{MOpcode::FMulRRR,
                                                      {MOperand::regOp(PhysReg::V0),
                                                       MOperand::regOp(PhysReg::V0),
                                                       MOperand::regOp(PhysReg::V1)}});
                    else if (isFDiv)
                        bbMir.instrs.push_back(MInstr{MOpcode::FDivRRR,
                                                      {MOperand::regOp(PhysReg::V0),
                                                       MOperand::regOp(PhysReg::V0),
                                                       MOperand::regOp(PhysReg::V1)}});
                    bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
                    ctx.fb.finalize();
                    return ctx.mf;
                }
            }
        }
    }

    return std::nullopt;
}

} // namespace zanna::codegen::aarch64::fastpaths
