//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/BlockLayoutPass.cpp
// Purpose: Greedy trace block layout for the AArch64 MIR pipeline. For each
//          function, builds a name→index map and then extends a trace from the
//          entry block by following unconditional branches. After reordering,
//          PeepholePass eliminates the resulting fall-through branches.
// Key invariants:
//   - Entry block always remains at index 0 after reordering.
//   - Only block order changes; no instructions are added or removed.
//   - If the computed order matches the original, the reorder is skipped.
// Ownership/Lifetime:
//   - Mutates MFunction::blocks in place; borrows module only during run().
// Links: src/codegen/aarch64/passes/BlockLayoutPass.hpp,
//        src/codegen/aarch64/passes/PeepholePass.cpp (consumer of fall-through layout)
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/passes/BlockLayoutPass.hpp"

#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/common/Diagnostics.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @file
 * @brief Implements entry-rooted greedy trace ordering for AArch64 MIR.
 *
 * The algorithm computes a permutation of existing block indices, preserving
 * each block object and its instructions. It avoids rebuilding the vector when
 * the computed permutation is already the identity.
 */

namespace zanna::codegen::aarch64::passes {

namespace {

/**
 * @brief Tests whether an opcode is a conditional branch family.
 * @param opc MIR opcode to classify.
 * @return `true` for `BCond`, `Cbz`, `Cbnz`, `Tbz`, or `Tbnz`.
 */
static bool isConditionalBranch(MOpcode opc) {
    switch (opc) {
        case MOpcode::BCond:
        case MOpcode::Cbz:
        case MOpcode::Cbnz:
        case MOpcode::Tbz:
        case MOpcode::Tbnz:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Resolves an instruction's final label operand to an unplaced block.
 * @param instr Branch-like instruction whose last operand is inspected.
 * @param nameToIdx Block-name to original-index map.
 * @param placed Placement bitmap parallel to the original block vector.
 * @return Target index when known and unplaced, otherwise `std::nullopt`.
 */
static std::optional<std::size_t> lookupUnplacedTarget(
    const MInstr &instr,
    const std::unordered_map<std::string, std::size_t> &nameToIdx,
    const std::vector<bool> &placed) {
    if (instr.ops.empty() || instr.ops.back().kind != MOperand::Kind::Label)
        return std::nullopt;
    auto it = nameToIdx.find(instr.ops.back().label);
    if (it == nameToIdx.end() || placed[it->second])
        return std::nullopt;
    return it->second;
}

/**
 * @brief Computes and applies greedy trace layout to one function.
 * @param[in,out] fn Function whose block vector may be permuted.
 * @post The original entry block remains first and every original block appears once.
 */
static void layoutFunction(MFunction &fn) {
    const std::size_t n = fn.blocks.size();
    if (n <= 1)
        return;

    // Build name → original-index map.
    std::unordered_map<std::string, std::size_t> nameToIdx;
    nameToIdx.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        nameToIdx[fn.blocks[i].name] = i;

    std::vector<bool> placed(n, false);
    std::vector<std::size_t> order;
    order.reserve(n);

    // Greedy trace: always start at the entry block (index 0).
    std::size_t cur = 0;
    while (order.size() < n) {
        if (!placed[cur]) {
            placed[cur] = true;
            order.push_back(cur);

            // Look at the last instruction of this block to find the preferred
            // fall-through successor.
            const MBasicBlock &bb = fn.blocks[cur];
            if (!bb.instrs.empty()) {
                const MInstr &last = bb.instrs.back();
                if (last.opc == MOpcode::Br) {
                    if (auto target = lookupUnplacedTarget(last, nameToIdx, placed)) {
                        cur = *target;
                        continue; // extend the trace
                    }
                }

                if (bb.instrs.size() >= 2 &&
                    isConditionalBranch(bb.instrs[bb.instrs.size() - 2].opc) &&
                    last.opc == MOpcode::Br) {
                    // Prefer the explicit false-edge target so the trailing
                    // unconditional branch can collapse into fallthrough.
                    if (auto fallthrough = lookupUnplacedTarget(last, nameToIdx, placed)) {
                        cur = *fallthrough;
                        continue;
                    }
                    if (auto taken = lookupUnplacedTarget(
                            bb.instrs[bb.instrs.size() - 2], nameToIdx, placed)) {
                        cur = *taken;
                        continue;
                    }
                }

                if (isConditionalBranch(last.opc)) {
                    if (auto target = lookupUnplacedTarget(last, nameToIdx, placed)) {
                        cur = *target;
                        continue;
                    }
                }
            }
        }

        // Advance to the next unplaced block in original order.
        bool found = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (!placed[i]) {
                cur = i;
                found = true;
                break;
            }
        }
        if (!found)
            break;
    }

    // If the trace didn't differ from original order, skip the reorder.
    bool changed = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (order[i] != i) {
            changed = true;
            break;
        }
    }
    if (!changed)
        return;

    // Reorder fn.blocks according to the computed trace.
    std::vector<MBasicBlock> reordered;
    reordered.reserve(n);
    for (std::size_t idx : order)
        reordered.push_back(std::move(fn.blocks[idx]));
    fn.blocks = std::move(reordered);
}

} // namespace

/**
 * @brief Applies greedy block layout independently to every MIR function.
 * @param[in,out] module Module containing functions to reorder.
 * @param diags Unused diagnostic sink.
 * @return Always `true`.
 */
bool BlockLayoutPass::run(AArch64Module &module, Diagnostics & /*diags*/) {
    for (MFunction &fn : module.mir)
        layoutFunction(fn);
    return true;
}

} // namespace zanna::codegen::aarch64::passes
