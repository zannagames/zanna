//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/fastpaths/FastPaths_Return.cpp
// Purpose: Fast-path pattern matching for return-related patterns.
//          Handles ret %paramN, ret const i64, ret const f64, ret const_str,
//          and ret addr_of — all single-block functions with no computation.
// Key invariants:
//   - Only single-block functions are matched.
//   - Direct parameter returns additionally reject side-effecting blocks.
//   - Return value must be directly available (no computation needed).
// Ownership/Lifetime:
//   - Stateless free functions; FastPathContext is borrowed for the call duration.
// Links: src/codegen/aarch64/fastpaths/FastPathsInternal.hpp
//
//===----------------------------------------------------------------------===//

#include "FastPathsInternal.hpp"

/**
 * @file
 * @brief Implements direct-value AArch64 return fast paths.
 *
 * Recognized values include entry parameters, integer/FP constants, pooled
 * string globals, and raw global addresses. Results are placed in the
 * appropriate ABI return register before emitting `Ret`.
 */

namespace zanna::codegen::aarch64::fastpaths {

using il::core::Opcode;

/**
 * @brief Attempts to lower a directly available return value without generic lowering.
 *
 * Boolean returns stay on the generic path. String constants select the
 * length-aware runtime constructor when literal metadata is available and
 * fall back to the C-string bridge otherwise; raw addresses skip construction.
 *
 * @param[in,out] ctx Fast-path state and output MIR.
 * @return Completed MIR function on a match, otherwise `std::nullopt`.
 */
std::optional<MFunction> tryReturnFastPaths(FastPathContext &ctx) {
    if (ctx.fn.blocks.empty())
        return std::nullopt;
    if (ctx.fn.retType.kind == il::core::Type::Kind::I1)
        return std::nullopt;

    const auto &bb = ctx.fn.blocks.front();
    auto &bbMir = ctx.bbOut(0);

    // =========================================================================
    // ret %paramN fast-path
    // =========================================================================
    // Pattern: Single-block with no side effects, returning a parameter directly.
    // Emits: mov x0/d0, srcReg (if needed); ret
    if (ctx.fn.blocks.size() == 1 && !bb.instructions.empty() && !bb.params.empty() &&
        !hasSideEffects(bb)) {
        const auto &retI = bb.instructions.back();
        if (retI.op == Opcode::Ret && !retI.operands.empty()) {
            const auto &rv = retI.operands[0];
            if (rv.kind == il::core::Value::Kind::Temp) {
                int pIdx = indexOfParam(bb, rv.id);
                if (pIdx >= 0) {
                    if (ctx.fn.retType.kind == il::core::Type::Kind::F64) {
                        const PhysReg src = ctx.ti.f64ArgOrder[static_cast<size_t>(pIdx)];
                        if (src != PhysReg::V0)
                            bbMir.instrs.push_back(
                                MInstr{MOpcode::FMovRR,
                                       {MOperand::regOp(PhysReg::V0), MOperand::regOp(src)}});
                    } else {
                        const PhysReg src = ctx.argOrder[static_cast<size_t>(pIdx)];
                        if (src != PhysReg::X0)
                            bbMir.instrs.push_back(
                                MInstr{MOpcode::MovRR,
                                       {MOperand::regOp(PhysReg::X0), MOperand::regOp(src)}});
                    }
                    bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
                    ctx.fb.finalize();
                    return ctx.mf;
                }
            }
        }
    }

    // =========================================================================
    // ret const_str / ret addr_of fast-path
    // =========================================================================
    // Pattern:
    //   - const_str: materialize the pooled literal address, then call the
    //     length-aware literal helper when the byte length is known
    //   - addr_of: materialize and return the raw symbol address
    if (ctx.fn.blocks.size() == 1 && bb.instructions.size() >= 2) {
        const auto &retI = bb.instructions.back();
        if (retI.op == Opcode::Ret && !retI.operands.empty()) {
            const auto &rv = retI.operands[0];
            if (rv.kind == il::core::Value::Kind::Temp) {
                const unsigned rid = rv.id;
                /// @brief Finds the instruction defining the returned temporary.
                auto prodIt = std::find_if(
                    bb.instructions.begin(), bb.instructions.end(), [&](const il::core::Instr &I) {
                        return I.result && *I.result == rid;
                    });
                if (prodIt != bb.instructions.end()) {
                    const auto &prod = *prodIt;
                    if (!prod.operands.empty() &&
                        prod.operands[0].kind == il::core::Value::Kind::GlobalAddr &&
                        (prod.op == Opcode::ConstStr || prod.op == Opcode::AddrOf)) {
                        const std::string &sym = prod.operands[0].str;
                        bbMir.instrs.push_back(
                            MInstr{MOpcode::AdrPage,
                                   {MOperand::regOp(PhysReg::X0), MOperand::labelOp(sym)}});
                        bbMir.instrs.push_back(MInstr{MOpcode::AddPageOff,
                                                      {MOperand::regOp(PhysReg::X0),
                                                       MOperand::regOp(PhysReg::X0),
                                                       MOperand::labelOp(sym)}});
                        if (prod.op == Opcode::ConstStr) {
                            if (ctx.stringLiteralByteLengths) {
                                const auto it = ctx.stringLiteralByteLengths->find(sym);
                                if (it != ctx.stringLiteralByteLengths->end()) {
                                    bbMir.instrs.push_back(MInstr{
                                        MOpcode::MovRI,
                                        {MOperand::regOp(PhysReg::X1),
                                         MOperand::immOp(static_cast<long long>(it->second))}});
                                    bbMir.instrs.push_back(MInstr{
                                        MOpcode::Bl, {MOperand::labelOp("rt_str_from_lit")}});
                                } else {
                                    bbMir.instrs.push_back(
                                        MInstr{MOpcode::Bl, {MOperand::labelOp("rt_const_cstr")}});
                                }
                            } else {
                                bbMir.instrs.push_back(
                                    MInstr{MOpcode::Bl, {MOperand::labelOp("rt_const_cstr")}});
                            }
                        }
                        bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
                        ctx.fb.finalize();
                        return ctx.mf;
                    }
                }
            }
        }
    }

    // =========================================================================
    // ret const i64 fast-path
    // =========================================================================
    // Pattern: Single-block with exactly one ret-const instruction.
    // Emits: mov x0, #imm; ret
    if (ctx.fn.blocks.size() == 1) {
        const auto &only = ctx.fn.blocks.front();
        if (only.instructions.size() == 1) {
            const auto &term = only.instructions.back();
            if (term.op == Opcode::Ret && !term.operands.empty()) {
                const auto &v = term.operands[0];
                if (v.kind == il::core::Value::Kind::ConstInt) {
                    const long long imm = v.i64;
                    ctx.bbOut(0).instrs.push_back(MInstr{
                        MOpcode::MovRI, {MOperand::regOp(PhysReg::X0), MOperand::immOp(imm)}});
                    ctx.bbOut(0).instrs.push_back(MInstr{MOpcode::Ret, {}});
                    return ctx.mf;
                }
            }
        }
    }

    // =========================================================================
    // ret const f64 fast-path
    // =========================================================================
    // Pattern: Single-block with exactly one ret-const-float instruction.
    // Emits: fmovri v0, <exact binary64 bits>; ret
    if (ctx.fn.blocks.size() == 1) {
        const auto &only = ctx.fn.blocks.front();
        if (only.instructions.size() == 1) {
            const auto &term = only.instructions.back();
            if (term.op == Opcode::Ret && !term.operands.empty()) {
                const auto &v = term.operands[0];
                if (v.kind == il::core::Value::Kind::ConstFloat) {
                    // Materialize float constant via FMovRI (encode as bitcast to i64)
                    long long bits;
                    std::memcpy(&bits, &v.f64, sizeof(double));
                    ctx.bbOut(0).instrs.push_back(MInstr{
                        MOpcode::FMovRI, {MOperand::regOp(PhysReg::V0), MOperand::immOp(bits)}});
                    ctx.bbOut(0).instrs.push_back(MInstr{MOpcode::Ret, {}});
                    ctx.fb.finalize();
                    return ctx.mf;
                }
            }
        }
    }

    return std::nullopt;
}

} // namespace zanna::codegen::aarch64::fastpaths
