//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/LowerOvf.cpp
// Purpose: Expand overflow-checked arithmetic pseudo-opcodes into guarded
//          AArch64 sequences. Executes between IL→MIR lowering and register
//          allocation; all operands remain virtual registers throughout.
// Key invariants:
//   - A single shared trap block (Ltrap_ovf_<fn>) is reused per function.
//   - When needed, the trap block is established before rewriting blocks so
//     fn.blocks is not reallocated while block references are held.
//   - add/sub overflow uses ADDS/SUBS + b.vs; mul overflow uses smulh+cmp+b.ne.
// Ownership/Lifetime:
//   - Rewrites MFunction in place; borrows fn only during the call.
// Links: src/codegen/aarch64/LowerOvf.hpp,
//        src/codegen/aarch64/passes/LoweringPass.cpp (caller),
//        docs/zannalib/game/core.md (overflow semantics)
//
//===----------------------------------------------------------------------===//

#include "LowerOvf.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * @file
 * @brief Implements AArch64 checked-arithmetic pseudo-instruction expansion.
 *
 * Addition and subtraction use AArch64's signed-overflow flag. Multiplication
 * compares the signed high half of the full product with the sign extension
 * implied by its retained low half. The pass creates temporaries only for the
 * multiplication sequence and allocates their IDs above the function's
 * existing virtual-register range.
 */

namespace zanna::codegen::aarch64 {

namespace {

/**
 * @brief Tests whether an opcode requires checked-arithmetic expansion.
 *
 * @param opc Machine opcode to classify.
 * @return `true` for the five signed-overflow pseudo-opcodes handled by this
 *         pass; otherwise `false`.
 */
[[nodiscard]] bool isOverflowPseudo(MOpcode opc) {
    switch (opc) {
        case MOpcode::AddOvfRRR:
        case MOpcode::SubOvfRRR:
        case MOpcode::AddOvfRI:
        case MOpcode::SubOvfRI:
        case MOpcode::MulOvfRRR:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Finds a basic block by its exact MIR label.
 *
 * @param fn Function whose block list is searched without modification.
 * @param name Exact block name to match.
 * @return The block's zero-based index, or `std::nullopt` when no block has
 *         the requested name.
 */
[[nodiscard]] std::optional<std::size_t> findBlock(const MFunction &fn, const std::string &name) {
    for (std::size_t i = 0; i < fn.blocks.size(); ++i) {
        if (fn.blocks[i].name == name)
            return i;
    }
    return std::nullopt;
}

} // namespace

/**
 * @brief Expands all signed-overflow arithmetic pseudos in a MIR function.
 *
 * @param[in,out] fn Function whose original blocks are rewritten in place.
 * @post Checked add/subtract operations set flags and branch on `vs`; checked
 *       multiplies compare their signed high half and branch on `ne`.
 * @post All inserted guards share a block that invokes `rt_trap_ovf`.
 */
void lowerOverflowOps(MFunction &fn) {
    // Reuse the per-function shared overflow trap block created during IL->MIR
    // lowering when one exists (a single `bl rt_trap_ovf` body — matched
    // semantically because LoweringPass sanitizes block labels after
    // lowering), so a function pays for at most one overflow trap block.
    // Otherwise fall back to a fresh assembler-local label: `L...` stays out
    // of the symbol table on Mach-O (.subsections_via_symbols) and resolves
    // as a normal non-exported label on ELF.
    /**
     * @brief Locates an already-lowered overflow trap block by its contents.
     *
     * @return The index of the first single-instruction block that calls
     *         `rt_trap_ovf`, or `std::nullopt` if none exists.
     */
    auto findExistingOvfTrap = [&fn]() -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < fn.blocks.size(); ++i) {
            const auto &candidate = fn.blocks[i];
            if (candidate.instrs.size() == 1 && candidate.instrs[0].opc == MOpcode::Bl &&
                !candidate.instrs[0].ops.empty() &&
                candidate.instrs[0].ops[0].kind == MOperand::Kind::Label &&
                candidate.instrs[0].ops[0].label == "rt_trap_ovf")
                return i;
        }
        return std::nullopt;
    };

    const auto existingOvfTrap = findExistingOvfTrap();
    const std::string trapLabel =
        existingOvfTrap ? fn.blocks[*existingOvfTrap].name : "Ltrap_ovf_" + fn.name;
    std::optional<std::size_t> trapIndex{};

    /**
     * @brief Resolves or creates the shared overflow trap block.
     *
     * The resolved index is cached so repeated calls do not search or append
     * another block.
     *
     * @return Stable index of the shared block in `fn.blocks`.
     * @post The returned block exists and has the label held in `trapLabel`.
     */
    auto ensureTrapBlock = [&]() -> std::size_t {
        if (trapIndex)
            return *trapIndex;

        if (auto existing = findBlock(fn, trapLabel)) {
            trapIndex = *existing;
            return *trapIndex;
        }

        MBasicBlock trapBlock{};
        trapBlock.name = trapLabel;
        // bl rt_trap_ovf
        trapBlock.instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp("rt_trap_ovf")}});
        fn.blocks.push_back(std::move(trapBlock));
        trapIndex = fn.blocks.size() - 1U;
        return *trapIndex;
    };

    // Pre-scan: if any overflow pseudo exists, create the trap block up front
    // so that fn.blocks is not reallocated while we hold references into it.
    bool hasOverflow = false;
    for (const auto &block : fn.blocks) {
        for (const auto &instr : block.instrs) {
            if (isOverflowPseudo(instr.opc)) {
                hasOverflow = true;
                break;
            }
        }
        if (hasOverflow)
            break;
    }

    if (!hasOverflow)
        return;

    ensureTrapBlock();

    // The trap block index is now stable — fn.blocks will not reallocate.
    // Only iterate blocks that existed before we added the trap block.

    // Compute the maximum virtual register ID across the entire function so
    // that new temporaries introduced by the mul overflow expansion do not
    // collide with existing virtual registers.
    uint16_t maxVReg = 0;
    for (const auto &block : fn.blocks)
        for (const auto &instr : block.instrs)
            for (const auto &op : instr.ops)
                if (!op.reg.isPhys && op.kind == MOperand::Kind::Reg)
                    maxVReg = std::max(maxVReg, op.reg.idOrPhys);

    const std::size_t blockCount = fn.blocks.size() - 1U;
    for (std::size_t blockIdx = 0; blockIdx < blockCount; ++blockIdx) {
        auto &block = fn.blocks[blockIdx];
        for (std::size_t i = 0; i < block.instrs.size(); ++i) {
            const MInstr &instr = block.instrs[i];
            if (!isOverflowPseudo(instr.opc))
                continue;

            if (instr.opc == MOpcode::MulOvfRRR) {
                // Multiply overflow detection using smulh:
                //   mul    Xd, Xn, Xm          // low 64 bits
                //   smulh  Xtmp1, Xn, Xm       // high 64 bits (signed)
                //   asr    Xtmp2, Xd, #63       // sign extension of bit 63
                //   cmp    Xtmp1, Xtmp2         // compare high bits with expected sign
                //   b.ne   Ltrap_ovf            // overflow if they don't match
                auto dst = instr.ops[0];
                auto lhs = instr.ops[1];
                auto rhs = instr.ops[2];

                // 1. mul Xd, Xn, Xm
                block.instrs[i] = MInstr{MOpcode::MulRRR, {dst, lhs, rhs}};

                // 2. smulh Xtmp1, Xn, Xm
                auto smulhDst = MOperand::vregOp(RegClass::GPR, ++maxVReg);
                MInstr smulh{MOpcode::SmulhRRR, {smulhDst, lhs, rhs}};

                // 3. asr Xtmp2, Xd, #63
                auto asrDst = MOperand::vregOp(RegClass::GPR, ++maxVReg);
                MInstr asr{MOpcode::AsrRI, {asrDst, dst, MOperand::immOp(63)}};

                // 4. cmp Xtmp1, Xtmp2
                MInstr cmp{MOpcode::CmpRR, {smulhDst, asrDst}};

                // 5. b.ne Ltrap_ovf
                MInstr bne{MOpcode::BCond, {MOperand::condOp("ne"), MOperand::labelOp(trapLabel)}};

                // Insert instructions after the mul
                auto insertPos = block.instrs.begin() + static_cast<std::ptrdiff_t>(i + 1);
                insertPos = block.instrs.insert(insertPos, std::move(smulh));
                insertPos = block.instrs.insert(insertPos + 1, std::move(asr));
                insertPos = block.instrs.insert(insertPos + 1, std::move(cmp));
                block.instrs.insert(insertPos + 1, std::move(bne));

                // Skip past the 4 instructions we just inserted
                i += 4;
                continue;
            }

            // Determine the real flag-setting opcode.
            MOpcode realOpc;
            switch (instr.opc) {
                case MOpcode::AddOvfRRR:
                    realOpc = MOpcode::AddsRRR;
                    break;
                case MOpcode::SubOvfRRR:
                    realOpc = MOpcode::SubsRRR;
                    break;
                case MOpcode::AddOvfRI:
                    realOpc = MOpcode::AddsRI;
                    break;
                case MOpcode::SubOvfRI:
                    realOpc = MOpcode::SubsRI;
                    break;
                default:
                    continue; // unreachable
            }

            // Replace pseudo with real flag-setting instruction.
            block.instrs[i] = MInstr{realOpc, instr.ops};

            // Insert b.vs to trap block after the flag-setting instruction.
            MInstr bvs{MOpcode::BCond, {MOperand::condOp("vs"), MOperand::labelOp(trapLabel)}};
            block.instrs.insert(block.instrs.begin() + static_cast<std::ptrdiff_t>(i + 1U),
                                std::move(bvs));

            // Skip past the b.vs we just inserted.
            ++i;
        }
    }

    // The shared trap block contains `bl rt_trap_ovf`. LegalizePass therefore
    // classifies the function as non-leaf even though the call is on a
    // no-returning error path.
}

} // namespace zanna::codegen::aarch64
