//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements reachability analysis and cleanup utilities used by the SimplifyCFG
// pass to prune unreachable blocks.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Reachability-based cleanup helpers for SimplifyCFG.
/// @details Provides graph traversal routines and block-pruning helpers that
///          remove unreachable blocks while respecting exception-handling
///          structure.

#include "il/transform/SimplifyCFG/ReachabilityCleanup.hpp"

#include "il/transform/SimplifyCFG/Utils.hpp"

#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"

#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace il::transform::simplify_cfg {
namespace {

/// @brief Compute the set of blocks reachable from the entry block.
/// @details Performs a breadth-first traversal following branch labels while
///          respecting exception-handling terminators.  Returns a bit vector
///          marking every block visited.
/// @param function Function whose blocks should be analysed.
/// @return Bit vector with bits set for reachable block indices.
BitVector markReachable(il::core::Function &function) {
    BitVector reachable(function.blocks.size(), false);
    if (function.blocks.empty())
        return reachable;

    std::unordered_map<std::string, size_t> labelToIndex;
    labelToIndex.reserve(function.blocks.size());
    for (size_t idx = 0; idx < function.blocks.size(); ++idx)
        labelToIndex.emplace(function.blocks[idx].label, idx);

    std::deque<size_t> worklist;
    reachable.set(0);
    worklist.push_back(0);

    while (!worklist.empty()) {
        size_t index = worklist.front();
        worklist.pop_front();

        const il::core::BasicBlock &block = function.blocks[index];
        const il::core::Instr *terminator = findTerminator(block);
        if (!terminator)
            continue;

        /// Resolve and enqueue one successor label if it has not been visited.
        auto addLabel = [&](const std::string &label) {
            enqueueSuccessor(reachable, worklist, lookupBlockIndex(labelToIndex, label));
        };

        for (const auto &instr : block.instructions) {
            if (instr.op != il::core::Opcode::EhPush)
                continue;
            for (const auto &label : instr.labels)
                addLabel(label);
        }

        switch (terminator->op) {
            case il::core::Opcode::Br:
                if (!terminator->labels.empty())
                    addLabel(terminator->labels.front());
                break;
            case il::core::Opcode::CBr:
            case il::core::Opcode::SwitchI32:
                for (const auto &label : terminator->labels)
                    addLabel(label);
                break;
            case il::core::Opcode::ResumeLabel:
                if (!terminator->labels.empty())
                    addLabel(terminator->labels.front());
                break;
            default:
                break;
        }
    }

    return reachable;
}

} // namespace

/// @brief Remove blocks that are not reachable according to @ref markReachable.
/// @details Iterates unreachable blocks in reverse order, skipping those marked
///          as EH-sensitive or referenced by a retained EH-sensitive region,
///          erases the remaining candidates, and updates statistics/logging hooks.
/// @param ctx Pass context providing function, EH sensitivity checks, and stats.
/// @return True when any block was removed.
bool removeUnreachableBlocks(SimplifyCFG::SimplifyCFGPassContext &ctx) {
    il::core::Function &F = ctx.function;
    BitVector reachable = markReachable(F);

    std::vector<size_t> unreachableBlocks;
    unreachableBlocks.reserve(F.blocks.size());
    for (size_t index = 1; index < F.blocks.size(); ++index) {
        if (!reachable.test(index))
            unreachableBlocks.push_back(index);
    }

    size_t removedBlocks = 0;
    std::unordered_set<std::string> labelsReferencedByRetainedUnreachable;
    for (size_t blockIndex : unreachableBlocks) {
        if (blockIndex >= F.blocks.size())
            continue;
        const il::core::BasicBlock &candidate = F.blocks[blockIndex];
        if (!ctx.isEHSensitive(candidate))
            continue;
        for (const auto &instr : candidate.instructions)
            for (const auto &label : instr.labels)
                labelsReferencedByRetainedUnreachable.insert(label);
    }

    for (auto it = unreachableBlocks.rbegin(); it != unreachableBlocks.rend(); ++it) {
        const size_t blockIndex = *it;
        if (blockIndex >= F.blocks.size())
            continue;

        il::core::BasicBlock &candidate = F.blocks[blockIndex];
        if (ctx.isEHSensitive(candidate))
            continue;

        const std::string label = candidate.label;
        if (labelsReferencedByRetainedUnreachable.contains(label))
            continue;

        F.blocks.erase(F.blocks.begin() + static_cast<std::ptrdiff_t>(blockIndex));
        ++removedBlocks;
    }

    if (removedBlocks > 0) {
        ctx.stats.unreachableRemoved += removedBlocks;
        if (ctx.isDebugLoggingEnabled()) {
            std::string message = "erased " + std::to_string(removedBlocks) + " unreachable block" +
                                  (removedBlocks == 1 ? "" : "s");
            ctx.logDebug(message);
        }
        return true;
    }

    return false;
}

} // namespace il::transform::simplify_cfg
