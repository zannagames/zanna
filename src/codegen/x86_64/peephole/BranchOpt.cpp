//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/peephole/BranchOpt.cpp
// Purpose: Branch optimization peephole sub-passes for the x86-64 backend.
//          Implements greedy trace block layout, cold block reordering,
//          branch chain elimination, conditional branch inversion, and
//          fallthrough jump removal.
// Key invariants:
//   - Entry block always stays first in the layout.
//   - Branch chain resolution detects cycles and leaves cyclic chains unchanged.
//   - All control-flow rewrites preserve semantic equivalence.
// Ownership/Lifetime:
//   - Stateless; all state is owned by the caller.
// Links: src/codegen/x86_64/peephole/BranchOpt.hpp,
//        src/codegen/x86_64/peephole/PeepholeCommon.hpp
//
//===----------------------------------------------------------------------===//

#include "BranchOpt.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/// @file
/// @brief Implements cycle-safe branch retargeting and block-layout rewrites.

namespace zanna::codegen::x64::peephole {

namespace {

/// @brief Find the first not-yet-placed successor referenced by @p instr.
/// @details Scans operands in reverse so the JCC's taken edge (typically
///          first in the operand list) is preferred only when no earlier
///          unplaced label exists. Returns @c std::nullopt when no operand
///          points at a candidate block.
/// @param instr Terminator instruction whose successors are inspected.
/// @param nameToIdx Mapping from block label to block index.
/// @param placed Per-block flag marking already-laid-out blocks.
/// @return Index of the first unplaced successor, or @c std::nullopt.
std::optional<std::size_t> lookupUnplacedTarget(
    const MInstr &instr,
    const std::unordered_map<std::string, std::size_t> &nameToIdx,
    const std::vector<bool> &placed) {
    for (auto it = instr.operands.rbegin(); it != instr.operands.rend(); ++it) {
        const auto *lbl = std::get_if<OpLabel>(&*it);
        if (!lbl)
            continue;
        auto target = nameToIdx.find(lbl->name);
        if (target == nameToIdx.end() || placed[target->second])
            continue;
        return target->second;
    }
    return std::nullopt;
}

/// @brief Predicate: does @p block fall through to its sibling at runtime?
/// @details A block falls through unless its last instruction is an
///          unconditional terminator (JMP/RET/UD2). JCC counts as
///          fall-through because it has an implicit unconditional path.
/// @param block Machine block whose terminal instruction is inspected.
/// @return @c true when execution may continue into the physically next block.
bool fallsThroughToNext(const MBasicBlock &block) {
    if (block.instructions.empty()) {
        return true;
    }

    const MOpcode last = block.instructions.back().opcode;
    return last != MOpcode::JMP && last != MOpcode::RET && last != MOpcode::UD2 &&
           last != MOpcode::JUMPTABLE;
}

/// @brief Finds the unique label operand in a mutable instruction.
/// @param instr Instruction whose operand variants are scanned.
/// @return Pointer to the sole label, or @c nullptr for zero or multiple labels.
OpLabel *singleLabelOperand(MInstr &instr) noexcept {
    OpLabel *label = nullptr;
    for (auto &operand : instr.operands) {
        auto *candidate = std::get_if<OpLabel>(&operand);
        if (!candidate)
            continue;
        if (label)
            return nullptr;
        label = candidate;
    }
    return label;
}

/// @brief Finds the unique label operand in an immutable instruction.
/// @param instr Instruction whose operand variants are scanned.
/// @return Pointer to the sole label, or @c nullptr for zero or multiple labels.
const OpLabel *singleLabelOperand(const MInstr &instr) noexcept {
    const OpLabel *label = nullptr;
    for (const auto &operand : instr.operands) {
        const auto *candidate = std::get_if<OpLabel>(&operand);
        if (!candidate)
            continue;
        if (label)
            return nullptr;
        label = candidate;
    }
    return label;
}

/// @brief Finds the unique immediate condition operand in an instruction.
/// @param instr Conditional instruction whose operand variants are scanned.
/// @return Pointer to the sole immediate, or @c nullptr for zero or multiple immediates.
OpImm *singleConditionOperand(MInstr &instr) noexcept {
    OpImm *condition = nullptr;
    for (auto &operand : instr.operands) {
        auto *candidate = std::get_if<OpImm>(&operand);
        if (!candidate)
            continue;
        if (condition)
            return nullptr;
        condition = candidate;
    }
    return condition;
}

/// @brief Apply forwarding to a JMP/JCC's target label.
/// @details The function follows the @p forwarding table once (single hop),
///          updating the sole label operand in place when a new target is
///          recorded for the current label. Scanning by operand kind keeps
///          hand-built MIR that uses `{label, cc}` for JCC from becoming a
///          silent peephole miss.
/// @param instr Branch instruction to rewrite.
/// @param forwarding Map of @c old_label -> @c new_label.
/// @return @c true when the operand was rewritten.
bool retargetBranchLabel(MInstr &instr,
                         const std::unordered_map<std::string, std::string> &forwarding) {
    if (instr.opcode != MOpcode::JMP && instr.opcode != MOpcode::JCC)
        return false;

    OpLabel *label = singleLabelOperand(instr);
    if (!label)
        return false;

    const auto it = forwarding.find(label->name);
    if (it == forwarding.end() || it->second == label->name) {
        return false;
    }

    label->name = it->second;
    return true;
}

} // namespace

/// @brief Greedy trace-based block layout for fall-through maximisation.
/// @details Starts at the entry block and follows the preferred fall-through
///          edge of each terminator, threading blocks together so the most
///          likely path through the function uses physical adjacency
///          (eliminating the need for a JMP). Explicit trap blocks come last
///          courtesy of the subsequent @ref moveColdBlocks pass. Bails out
///          early when the function has 2 or fewer blocks because no useful
///          rearrangement is possible. Functions containing any implicit
///          fall-through edge are also left unchanged.
/// @param fn Machine function whose blocks may be moved.
/// @param stats Statistics incremented by the function's block count on change.
void traceBlockLayout(MFunction &fn, PeepholeStats &stats) {
    if (fn.blocks.size() <= 2)
        return;

    const auto n = fn.blocks.size();

    // Reordering a block with an implicit fall-through successor changes its
    // control flow unless the successor stays physically adjacent. Keep trace
    // layout to fully explicit CFG shapes; moveColdBlocks has its own local
    // protection for fall-through pairs.
    for (std::size_t bi = 0; bi + 1 < n; ++bi) {
        if (fallsThroughToNext(fn.blocks[bi]))
            return;
    }

    // Build label -> index map.
    std::unordered_map<std::string, std::size_t> nameToIdx;
    nameToIdx.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        nameToIdx[fn.blocks[i].label] = i;

    std::vector<bool> placed(n, false);
    std::vector<std::size_t> order;
    order.reserve(n);

    // Seed with entry block.
    std::size_t cur = 0;
    while (order.size() < n) {
        if (!placed[cur]) {
            placed[cur] = true;
            order.push_back(cur);

            // Follow a preferred fallthrough edge to extend the trace.
            const auto &instrs = fn.blocks[cur].instructions;
            if (!instrs.empty()) {
                const auto &last = instrs.back();
                if (instrs.size() >= 2 && instrs[instrs.size() - 2].opcode == MOpcode::JCC &&
                    last.opcode == MOpcode::JMP) {
                    if (auto fallthrough = lookupUnplacedTarget(last, nameToIdx, placed)) {
                        cur = *fallthrough;
                        continue;
                    }
                    if (auto taken =
                            lookupUnplacedTarget(instrs[instrs.size() - 2], nameToIdx, placed)) {
                        cur = *taken;
                        continue;
                    }
                }

                // A lone JCC has an implicit fallthrough successor in the next
                // physical block. Following the taken edge here can move that
                // target into the fallthrough slot on a later layout iteration
                // after removeFallthroughJumps has deleted the explicit false
                // JMP, changing branch semantics.
                if (last.opcode == MOpcode::JMP) {
                    if (auto target = lookupUnplacedTarget(last, nameToIdx, placed)) {
                        cur = *target;
                        continue;
                    }
                }
            }
        }

        // Find next unplaced block.
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

    // Check if reordering changed anything.
    bool changed = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (order[i] != i) {
            changed = true;
            break;
        }
    }

    if (changed) {
        std::vector<MBasicBlock> reordered;
        reordered.reserve(n);
        for (std::size_t idx : order)
            reordered.push_back(std::move(fn.blocks[idx]));
        fn.blocks = std::move(reordered);
        stats.blocksReordered += n;
    }
}

/// @brief Move explicit trap blocks to the tail of the function.
/// @details Classifies a non-entry block as cold when it contains @c UD2 and
///          does not participate in an implicit fall-through pair. Reorders
///          hot blocks before cold blocks while preserving order within each
///          partition and keeping the entry first.
/// @param fn Machine function whose blocks may be moved.
/// @param stats Statistics incremented once for each cold block appended.
void moveColdBlocks(MFunction &fn, PeepholeStats &stats) {
    if (fn.blocks.size() <= 2)
        return;

    std::vector<std::size_t> coldIndices;
    std::vector<std::size_t> hotIndices;
    std::vector<bool> fallthroughProtected(fn.blocks.size(), false);

    for (std::size_t bi = 0; bi + 1 < fn.blocks.size(); ++bi) {
        if (!fallsThroughToNext(fn.blocks[bi]))
            continue;
        fallthroughProtected[bi] = true;
        fallthroughProtected[bi + 1] = true;
    }

    // First block (entry) is always hot and stays first
    hotIndices.push_back(0);

    // Classify remaining blocks
    for (std::size_t bi = 1; bi < fn.blocks.size(); ++bi) {
        const auto &block = fn.blocks[bi];
        bool isCold = false;
        if (!fallthroughProtected[bi]) {
            for (const auto &instr : block.instructions) {
                if (instr.opcode == MOpcode::UD2) {
                    isCold = true;
                    break;
                }
            }
        }

        if (isCold)
            coldIndices.push_back(bi);
        else
            hotIndices.push_back(bi);
    }

    // Only reorder if we found cold blocks
    if (!coldIndices.empty() && hotIndices.size() > 1) {
        std::vector<MBasicBlock> newBlocks;
        newBlocks.reserve(fn.blocks.size());

        // Add hot blocks first
        for (std::size_t idx : hotIndices)
            newBlocks.push_back(std::move(fn.blocks[idx]));

        // Add cold blocks at the end
        for (std::size_t idx : coldIndices) {
            newBlocks.push_back(std::move(fn.blocks[idx]));
            ++stats.coldBlocksMoved;
        }

        fn.blocks = std::move(newBlocks);
    }
}

/// @brief Collapse multi-hop branch chains into single branches.
/// @details Detects blocks whose sole instruction is an unconditional jump
///          and rewrites all branches that target such blocks to point at
///          the eventual destination instead. Explicit visited-label sets
///          identify cycles; forwarding entries rooted in a cycle are removed
///          so their existing targets remain unchanged.
/// @param fn Machine function whose @c JMP and @c JCC labels may be rewritten.
/// @param stats Statistics incremented once per rewritten branch instruction.
void eliminateBranchChains(MFunction &fn, PeepholeStats &stats) {
    // Build forwarding map: label -> ultimate JMP target for single-JMP blocks.
    std::unordered_map<std::string, std::string> forwarding;
    for (const auto &block : fn.blocks) {
        if (block.instructions.size() == 1 && block.instructions[0].opcode == MOpcode::JMP) {
            const auto *lbl = singleLabelOperand(block.instructions[0]);
            if (lbl)
                forwarding[block.label] = lbl->name;
        }
    }

    // Resolve chains with explicit cycle detection. Cyclic forwarding entries
    // are erased so existing branch targets remain unchanged.
    std::vector<std::string> cyclicLabels;
    for (auto &[label, target] : forwarding) {
        std::unordered_set<std::string> seen;
        seen.insert(label);
        bool cycle = false;
        for (;;) {
            if (!seen.insert(target).second) {
                cycle = true;
                break;
            }
            auto it = forwarding.find(target);
            if (it == forwarding.end())
                break;
            target = it->second;
        }
        if (cycle)
            cyclicLabels.push_back(label);
    }
    for (const auto &label : cyclicLabels)
        forwarding.erase(label);

    // Retarget branches.
    if (!forwarding.empty()) {
        for (auto &block : fn.blocks) {
            for (auto &instr : block.instructions) {
                if (retargetBranchLabel(instr, forwarding)) {
                    ++stats.branchChainsEliminated;
                }
            }
        }
    }
}

/// @brief Invert conditional branches whose taken edge is already adjacent.
/// @details Pattern: a JCC followed by a JMP where the JMP target is the
///          next block. After inversion the JCC condition flips and the
///          JMP can be removed because the fall-through replaces it. This
///          saves an instruction on the hot path.
/// @param fn Machine function rewritten in place.
/// @param stats Statistics incremented once per successful inversion.
void invertConditionalBranches(MFunction &fn, PeepholeStats &stats) {
    // Pattern:  JCC(cc, label_skip) / JMP(label_exit) where label_skip is the next block
    // Rewrite:  JCC(invert(cc), label_exit) — saves 5 bytes (eliminates JMP)
    for (std::size_t bi = 0; bi + 1 < fn.blocks.size(); ++bi) {
        auto &block = fn.blocks[bi];
        const auto &nextBlock = fn.blocks[bi + 1];

        if (block.instructions.size() < 2)
            continue;

        auto &secondToLast = block.instructions[block.instructions.size() - 2];
        auto &last = block.instructions[block.instructions.size() - 1];

        // Check: second-to-last is JCC, last is JMP.
        if (secondToLast.opcode != MOpcode::JCC || last.opcode != MOpcode::JMP)
            continue;

        // JCC operands are usually {OpImm(cc), OpLabel(target)}, but older
        // hand-built tests used {OpLabel(target), OpImm(cc)}.
        if (secondToLast.operands.size() < 2 || last.operands.size() < 1)
            continue;

        // Check: JCC target is the next block's label (i.e., it skips over the JMP).
        auto *jccLabel = singleLabelOperand(secondToLast);
        if (!jccLabel || jccLabel->name != nextBlock.label)
            continue;

        // Get the JMP target — this becomes the inverted JCC target.
        const auto *jmpLabel = singleLabelOperand(last);
        if (!jmpLabel)
            continue;

        // Invert the condition code.
        auto *ccImm = singleConditionOperand(secondToLast);
        if (!ccImm)
            continue;

        // Zanna CC inversion table:
        //  0 (eq) <-> 1 (ne), 2 (lt) <-> 5 (ge), 3 (le) <-> 4 (gt)
        //  6 (a)  <-> 9 (be), 7 (ae) <-> 8 (b),  10 (p) <-> 11 (np)
        //  12 (o) <-> 13 (no)
        constexpr int kInvertTable[] = {1, 0, 5, 4, 3, 2, 9, 8, 7, 6, 11, 10, 13, 12};
        int cc = static_cast<int>(ccImm->val);
        if (cc < 0 || cc > 13)
            continue;

        // Rewrite: JCC(invCC, jmpTarget)
        ccImm->val = kInvertTable[cc];
        *jccLabel = *jmpLabel;
        block.instructions.pop_back(); // Remove JMP.
        ++stats.branchesInverted;
    }
}

/// @brief Drop @c JMP instructions that target the immediately following block.
/// @details After layout passes have run, many JMPs become redundant
///          because their target sits physically next. This pass scans
///          every terminator and removes such jumps, replacing them with
///          implicit fall-through. A jump immediately following @c JCC is
///          retained because it represents the explicit alternative edge.
/// @param fn Machine function rewritten in place.
/// @param stats Statistics incremented once per removed jump.
void removeFallthroughJumps(MFunction &fn, PeepholeStats &stats) {
    for (std::size_t bi = 0; bi + 1 < fn.blocks.size(); ++bi) {
        auto &block = fn.blocks[bi];
        const auto &nextBlock = fn.blocks[bi + 1];

        if (block.instructions.empty())
            continue;

        // Check if the last instruction is an unconditional jump to the next block
        auto &lastInstr = block.instructions.back();
        if (block.instructions.size() >= 2 &&
            block.instructions[block.instructions.size() - 2].opcode == MOpcode::JCC) {
            continue;
        }
        if (isJumpTo(lastInstr, nextBlock.label)) {
            block.instructions.pop_back();
            ++stats.branchesToNextRemoved;
        }
    }
}

} // namespace zanna::codegen::x64::peephole
