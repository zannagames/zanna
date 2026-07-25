//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/fastpaths/FastPaths_Cast.cpp
// Purpose: Fast-path pattern matching for type conversion operations.
//          Handles zext1/trunc1 (boolean masking), cast.si_narrow.chk (signed
//          narrowing with a trap on overflow).
// Key invariants:
//   - Result must flow directly to a ret instruction.
//   - Checked signed narrowing preserves the original value until the
//     sign-extended truncated result has been compared against it.
// Ownership/Lifetime:
//   - Stateless free functions; FastPathContext is borrowed for the call duration.
// Links: src/codegen/aarch64/fastpaths/FastPathsInternal.hpp
//
//===----------------------------------------------------------------------===//

#include "FastPathsInternal.hpp"

/**
 * @file
 * @brief Implements boolean and checked signed-narrowing AArch64 fast paths.
 *
 * Boolean conversions mask an entry parameter to one bit. Checked narrowing
 * performs a shift-based sign extension, compares it with the preserved input,
 * and branches to a uniquely named no-return trap block on mismatch.
 */

namespace zanna::codegen::aarch64::fastpaths {

using il::core::Opcode;

/**
 * @brief Per-thread suffix source for checked-cast trap block labels.
 *
 * Thread-local storage prevents collisions caused by concurrent lowering
 * threads sharing a process-wide counter.
 */
thread_local unsigned trapLabelCounter = 0;

/**
 * @brief Attempts a supported conversion whose result is returned immediately.
 *
 * `Zext1`/`Trunc1` require a parameter source and return its low bit.
 * `CastSiNarrowChk` emits signed truncation validation and appends a trap block.
 *
 * @param[in,out] ctx Fast-path state and output MIR.
 * @return Completed MIR function on a match, otherwise `std::nullopt`.
 */
std::optional<MFunction> tryCastFastPaths(FastPathContext &ctx) {
    if (ctx.fn.blocks.empty())
        return std::nullopt;

    const auto &bb = ctx.fn.blocks.front();
    auto &bbMir = ctx.bbOut(0);

    // Need at least 2 instructions and 1 parameter
    if (ctx.fn.blocks.size() != 1 || bb.instructions.size() < 2 || bb.params.empty())
        return std::nullopt;

    const auto &binI = bb.instructions[bb.instructions.size() - 2];
    const auto &retI = bb.instructions.back();

    // Must be a cast instruction feeding ret
    if (retI.op != Opcode::Ret || !binI.result || retI.operands.empty() ||
        retI.operands[0].kind != il::core::Value::Kind::Temp || retI.operands[0].id != *binI.result)
        return std::nullopt;

    // =========================================================================
    // zext1/trunc1: Boolean extension/truncation
    // =========================================================================
    // Pattern: zext1/trunc1 %param -> %r; ret %r
    // Both operations mask to lowest bit: and x0, x0, #1
    if (binI.op == Opcode::Zext1 || binI.op == Opcode::Trunc1) {
        const auto &o0 = binI.operands[0];
        if (o0.kind == il::core::Value::Kind::Temp) {
            int pIdx = indexOfParam(bb, o0.id);
            if (pIdx >= 0) {
                PhysReg src = ctx.argOrder[static_cast<std::size_t>(pIdx)];
                if (src != PhysReg::X0)
                    bbMir.instrs.push_back(MInstr{
                        MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(src)}});
                // and x0, x0, #1 via scratch register
                bbMir.instrs.push_back(
                    MInstr{MOpcode::MovRI, {MOperand::regOp(kScratchGPR), MOperand::immOp(1)}});
                bbMir.instrs.push_back(MInstr{MOpcode::AndRRR,
                                              {MOperand::regOp(PhysReg::X0),
                                               MOperand::regOp(PhysReg::X0),
                                               MOperand::regOp(kScratchGPR)}});
                bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
                ctx.fb.finalize();
                return ctx.mf;
            }
        }
        // Fall through to generic lowering if operand is not a param
    }

    // =========================================================================
    // cast.si_narrow.chk: Signed narrowing with range check
    // =========================================================================
    // Pattern: cast.si_narrow.chk %param -> %r; ret %r
    // Emits: sign-extend truncation, compare, trap on mismatch
    if (binI.op == Opcode::CastSiNarrowChk) {
        // Determine target width from binI.type
        int bits = 64;
        if (binI.type.kind == il::core::Type::Kind::I16)
            bits = 16;
        else if (binI.type.kind == il::core::Type::Kind::I32)
            bits = 32;
        else if (binI.type.kind == il::core::Type::Kind::I64)
            bits = 64;
        const int sh = 64 - bits;
        const auto &o0 = binI.operands[0];
        PhysReg src = PhysReg::X0;
        if (o0.kind == il::core::Value::Kind::Temp) {
            int pIdx = indexOfParam(bb, o0.id);
            if (pIdx >= 0)
                src = ctx.argOrder[static_cast<std::size_t>(pIdx)];
        }
        // Save the original source value into scratch BEFORE modifying X0.
        // When src == X0, the LSL+ASR below would clobber it, making the
        // subsequent comparison always equal (never trapping on overflow).
        bbMir.instrs.push_back(
            MInstr{MOpcode::MovRR, {MOperand::regOp(kScratchGPR), MOperand::regOp(src)}});

        if (src != PhysReg::X0)
            bbMir.instrs.push_back(
                MInstr{MOpcode::MovRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(src)}});

        // tmp = (x0 << sh) >> sh  (sign-extended truncation)
        if (sh > 0) {
            bbMir.instrs.push_back(MInstr{
                MOpcode::LslRI,
                {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X0), MOperand::immOp(sh)}});
            bbMir.instrs.push_back(MInstr{
                MOpcode::AsrRI,
                {MOperand::regOp(PhysReg::X0), MOperand::regOp(PhysReg::X0), MOperand::immOp(sh)}});
        }

        // Compare sign-extended value against the saved original
        bbMir.instrs.push_back(
            MInstr{MOpcode::CmpRR, {MOperand::regOp(PhysReg::X0), MOperand::regOp(kScratchGPR)}});

        // If not equal, branch to a trap block
        const std::string trapLabel = ".Ltrap_cast_" + std::to_string(trapLabelCounter++);
        bbMir.instrs.push_back(
            MInstr{MOpcode::BCond, {MOperand::condOp("ne"), MOperand::labelOp(trapLabel)}});

        // Fall-through: range is OK, return
        bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});

        // Append trap block to function with a call to rt_trap
        ctx.mf.blocks.emplace_back();
        ctx.mf.blocks.back().name = trapLabel;
        ctx.mf.blocks.back().instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp("rt_trap")}});
        ctx.fb.finalize();
        return ctx.mf;
    }

    return std::nullopt;
}

} // namespace zanna::codegen::aarch64::fastpaths
