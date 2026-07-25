//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/LowerOvf.cpp
// Purpose: Expand overflow-checked arithmetic pseudos (ADDOvfrr, SUBOvfrr,
//          IMULOvfrr) into real instructions with a conditional trap branch.
// Key invariants:
//   - Each overflow-checked op emits: <arith> dest, lhs, rhs; JO .Ltrap_ovf.
//   - A single trap block per function is reused to minimise code growth.
//   - The pass executes between IL→MIR lowering and register allocation.
// Ownership/Lifetime:
//   - Mutates the MFunction in-place; no persistent auxiliary structures.
// Links: src/codegen/x86_64/LowerILToMIR.hpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#include "MachineIR.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * @file
 * @brief Implements expansion of checked integer arithmetic MIR pseudos.
 *
 * Checked add, subtract, and multiply operations become their flag-setting
 * x86-64 two-operand forms followed by an overflow-condition branch. Every
 * function reuses one canonical, non-returning runtime overflow trap block.
 */

namespace zanna::codegen::x64 {

namespace {

/// @brief Locate a basic block index using its label, if present.
/// @param fn Function whose blocks are searched in layout order.
/// @param label Exact block label to match.
/// @return Matching block index, or @c std::nullopt.
[[nodiscard]] std::optional<std::size_t> findBlock(const MFunction &fn, const std::string &label) {
    for (std::size_t idx = 0; idx < fn.blocks.size(); ++idx) {
        if (fn.blocks[idx].label == label) {
            return idx;
        }
    }
    return std::nullopt;
}

/// @brief Produce a shallow copy of a Machine IR operand.
/// @param operand Value-semantic operand to duplicate.
/// @return Independent copy suitable for a replacement instruction.
[[nodiscard]] Operand cloneOp(const Operand &operand) {
    return operand;
}

} // namespace

/// @brief Rewrite overflow-checked arithmetic pseudos into guarded sequences.
///
/// @details Walks each machine basic block looking for ADDOvfrr, SUBOvfrr, and
///          IMULOvfrr pseudo-ops. Each is replaced with the real arithmetic
///          instruction (ADDrr, SUBrr, IMULrr) followed by a JCC with overflow
///          condition to a shared trap block. The trap block calls
///          rt_trap_ovf, which matches the no-argument lowering pattern.
///
/// @param fn Machine IR function being rewritten in place.
void lowerOverflowOps(MFunction &fn) {
    const std::string trapLabel = ".Ltrap_ovf_" + fn.name;
    std::optional<std::size_t> trapIndex{};

    /**
     * @brief Find, repair, or create the function's canonical overflow trap.
     * @return Stable block index cached for subsequent calls.
     */
    auto ensureTrapBlock = [&]() -> std::size_t {
        if (trapIndex) {
            return *trapIndex;
        }

        if (auto existing = findBlock(fn, trapLabel)) {
            trapIndex = *existing;
            auto &trapBlock = fn.blocks[*trapIndex];
            /// Recognize an existing direct call to the overflow runtime trap.
            const bool hasRuntimeCall =
                std::any_of(trapBlock.instructions.begin(),
                            trapBlock.instructions.end(),
                            [](const MInstr &instr) {
                                if (instr.opcode != MOpcode::CALL || instr.operands.empty()) {
                                    return false;
                                }
                                const auto *label = std::get_if<OpLabel>(&instr.operands.front());
                                return label && label->name == "rt_trap_ovf";
                            });
            /// Locate the non-returning terminator so a missing call precedes it.
            auto ud2It =
                std::find_if(trapBlock.instructions.begin(),
                             trapBlock.instructions.end(),
                             [](const MInstr &instr) { return instr.opcode == MOpcode::UD2; });
            if (!hasRuntimeCall) {
                MInstr call = MInstr::make(MOpcode::CALL,
                                           std::vector<Operand>{makeLabelOperand("rt_trap_ovf")});
                if (ud2It != trapBlock.instructions.end()) {
                    ud2It = trapBlock.instructions.insert(ud2It, std::move(call));
                    ++ud2It;
                } else {
                    trapBlock.append(std::move(call));
                    ud2It = trapBlock.instructions.end();
                }
            }
            if (ud2It == trapBlock.instructions.end()) {
                trapBlock.append(MInstr::make(MOpcode::UD2));
            }
            return *trapIndex;
        }

        MBasicBlock trapBlock{};
        trapBlock.label = trapLabel;
        trapBlock.append(
            MInstr::make(MOpcode::CALL, std::vector<Operand>{makeLabelOperand("rt_trap_ovf")}));
        trapBlock.append(MInstr::make(MOpcode::UD2));
        fn.blocks.push_back(std::move(trapBlock));
        trapIndex = fn.blocks.size() - 1U;
        return *trapIndex;
    };

    // Condition code 12 = "o" (overflow)
    constexpr int64_t kCondOverflow = 12;

    // Pre-scan: if any overflow pseudo exists, create the trap block up front
    // so that fn.blocks is not reallocated while we hold references into it.
    for (const auto &block : fn.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.opcode == MOpcode::ADDOvfrr || instr.opcode == MOpcode::SUBOvfrr ||
                instr.opcode == MOpcode::IMULOvfrr) {
                ensureTrapBlock();
                goto rewrite; // trap block created, proceed to rewriting
            }
        }
    }
    return; // no overflow pseudos found

rewrite:
    // The trap block index is now stable — fn.blocks will not reallocate.
    // Do not assume the trap block is last: division lowering can create the
    // same overflow trap before appending continuation blocks.
    const std::size_t trapBlockIndex = *trapIndex;
    for (std::size_t blockIdx = 0; blockIdx < fn.blocks.size(); ++blockIdx) {
        if (blockIdx == trapBlockIndex) {
            continue;
        }
        auto &block = fn.blocks[blockIdx];
        for (std::size_t i = 0; i < block.instructions.size(); ++i) {
            const MInstr instr = block.instructions[i];
            MOpcode realOpc;
            switch (instr.opcode) {
                case MOpcode::ADDOvfrr:
                    realOpc = MOpcode::ADDrr;
                    break;
                case MOpcode::SUBOvfrr:
                    realOpc = MOpcode::SUBrr;
                    break;
                case MOpcode::IMULOvfrr:
                    realOpc = MOpcode::IMULrr;
                    break;
                default:
                    continue;
            }

            if (instr.operands.size() < 2U) {
                continue;
            }

            std::vector<MInstr> replacement{};
            replacement.reserve(3);

            if (instr.operands.size() >= 3U) {
                const Operand dest = cloneOp(instr.operands[0]);
                const Operand lhs = cloneOp(instr.operands[1]);
                const Operand rhs = cloneOp(instr.operands[2]);
                if (std::holds_alternative<OpImm>(lhs)) {
                    replacement.push_back(MInstr::make(
                        MOpcode::MOVri, std::vector<Operand>{cloneOp(dest), cloneOp(lhs)}));
                } else if (std::holds_alternative<OpReg>(lhs)) {
                    replacement.push_back(MInstr::make(
                        MOpcode::MOVrr, std::vector<Operand>{cloneOp(dest), cloneOp(lhs)}));
                } else {
                    continue;
                }
                replacement.push_back(
                    MInstr::make(realOpc, std::vector<Operand>{cloneOp(dest), cloneOp(rhs)}));
            } else {
                replacement.push_back(MInstr::make(
                    realOpc,
                    std::vector<Operand>{cloneOp(instr.operands[0]), cloneOp(instr.operands[1])}));
            }

            for (auto &replacementInstr : replacement) {
                replacementInstr.loc = instr.loc;
            }

            auto joInstr = MInstr::make(
                MOpcode::JCC,
                std::vector<Operand>{makeImmOperand(kCondOverflow), makeLabelOperand(trapLabel)});
            joInstr.loc = instr.loc;
            replacement.push_back(std::move(joInstr));

            const auto beginIt = block.instructions.begin() + static_cast<std::ptrdiff_t>(i);
            block.instructions.erase(beginIt);
            block.instructions.insert(block.instructions.begin() + static_cast<std::ptrdiff_t>(i),
                                      replacement.begin(),
                                      replacement.end());

            i += replacement.size() - 1U;
        }
    }
}

} // namespace zanna::codegen::x64
