//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the SimplifyCFG block merging routines.  These helpers detect
// blocks with a single predecessor and merge their contents into the predecessor
// once branch arguments are substituted.  The transformation reduces the number
// of basic blocks while keeping control flow and SSA operands consistent.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Block merging utilities for SimplifyCFG.
/// @details Provides the worker that performs the actual merge and the public
///          entry point that walks the function, records statistics, and emits
///          debug diagnostics.

#include "il/transform/SimplifyCFG/BlockMerging.hpp"

#include "il/transform/SimplifyCFG/Utils.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"

#include <algorithm>
#include <cassert>
#include <string>
#include <unordered_map>
#include <vector>

namespace il::transform::simplify_cfg {
namespace {

/// @brief Predecessor edge information precomputed for O(1) lookup.
struct PredInfo {
    size_t edgeCount = 0;                 ///< Total predecessor edge count (including self-loops).
    il::core::BasicBlock *pred = nullptr; ///< First non-self predecessor block (if any).
    il::core::Instr *predTerm = nullptr;  ///< Terminator of that predecessor.
};

/// @brief Build predecessor edge map for all blocks in O(B * T).
/// @details Scans every terminator once and records, for each target label,
///          the total edge count and first non-self predecessor.  Self-loop
///          edges are counted so that loop headers (which have a self-loop)
///          correctly show edgeCount > 1 and are never merged.
/// @param F Function whose successor edges are indexed.
/// @return Predecessor summary keyed by target block label.
std::unordered_map<std::string, PredInfo> buildPredMap(il::core::Function &F) {
    std::unordered_map<std::string, PredInfo> predMap;
    predMap.reserve(F.blocks.size());

    for (auto &candidate : F.blocks) {
        il::core::Instr *term = findTerminator(candidate);
        if (!term)
            continue;

        for (const auto &label : term->labels) {
            auto &info = predMap[label];
            ++info.edgeCount;
            // Record first non-self predecessor as the merge candidate.
            if (!info.pred && label != candidate.label) {
                info.pred = &candidate;
                info.predTerm = term;
            }
        }
    }

    return predMap;
}

/// @brief Merge a block into its sole predecessor when safe.
///
/// @details Uses a precomputed predecessor map for O(1) edge-count lookup,
///          verifies that the predecessor terminator is a simple branch, and
///          rewrites all SSA uses in @p block to reference the incoming
///          arguments.  The routine then splices @p block's non-terminator
///          instructions into the predecessor, rewrites the successor labels
///          in the merged terminator, and finally erases the now-redundant
///          block from the function.
///
/// @param ctx     SimplifyCFG context providing EH-sensitivity checks and
///                diagnostic hooks.
/// @param block   Candidate block to merge.
/// @param predMap Precomputed predecessor edge map for the function.
/// @returns True when the block was merged into its predecessor.
bool mergeSinglePred(SimplifyCFG::SimplifyCFGPassContext &ctx,
                     il::core::BasicBlock &block,
                     const std::unordered_map<std::string, PredInfo> &predMap) {
    il::core::Function &F = ctx.function;

    /// Relocate the candidate block in current vector storage before mutation.
    auto blockIt =
        std::find_if(F.blocks.begin(), F.blocks.end(), [&](il::core::BasicBlock &candidate) {
            return &candidate == &block;
        });
    if (blockIt == F.blocks.end())
        return false;

    if (ctx.isEHSensitive(block))
        return false;

    // O(1) predecessor lookup from precomputed map.
    auto predIt = predMap.find(block.label);
    if (predIt == predMap.end())
        return false;

    const auto &info = predIt->second;
    if (info.edgeCount != 1)
        return false;

    il::core::BasicBlock *predBlock = info.pred;
    il::core::Instr *predTerm = info.predTerm;

    if (!predBlock || !predTerm)
        return false;

    if (predTerm->op != il::core::Opcode::Br)
        return false;

    if (predTerm->labels.size() != 1)
        return false;

    if (predTerm->labels.front() != block.label)
        return false;

    il::core::Instr *blockTerm = findTerminator(block);
    if (!blockTerm)
        return false;

    std::vector<il::core::Value> incomingArgs;
    if (!predTerm->brArgs.empty()) {
        if (predTerm->brArgs.size() != 1)
            return false;
        incomingArgs = predTerm->brArgs.front();
    }

    if (block.params.size() != incomingArgs.size())
        return false;

    if (!block.params.empty() && blockParamsUsedOutside(F, block))
        return false;

    std::unordered_map<unsigned, il::core::Value> substitution;
    substitution.reserve(block.params.size());

    for (size_t idx = 0; idx < block.params.size(); ++idx)
        substitution.emplace(block.params[idx].id, incomingArgs[idx]);

    if (!substitution.empty()) {
        for (auto &bb : F.blocks) {
            for (auto &instr : bb.instructions) {
                for (auto &operand : instr.operands)
                    operand = substituteValue(operand, substitution);

                for (auto &argList : instr.brArgs) {
                    for (auto &value : argList)
                        value = substituteValue(value, substitution);
                }
            }
        }
    }

    auto &predInstrs = predBlock->instructions;
    /// Relocate the borrowed predecessor terminator in its instruction vector.
    auto predTermIt = std::find_if(predInstrs.begin(),
                                   predInstrs.end(),
                                   [&](il::core::Instr &instr) { return &instr == predTerm; });
    if (predTermIt == predInstrs.end())
        return false;

    auto &blockInstrs = block.instructions;
    /// Relocate the candidate block's borrowed terminator before moving instructions.
    auto blockTermIt = std::find_if(blockInstrs.begin(),
                                    blockInstrs.end(),
                                    [&](il::core::Instr &instr) { return &instr == blockTerm; });
    if (blockTermIt == blockInstrs.end())
        return false;

    std::vector<il::core::Instr> movedInstrs;
    movedInstrs.reserve(blockInstrs.size() > 0 ? blockInstrs.size() - 1 : 0);
    for (auto it = blockInstrs.begin(); it != blockInstrs.end(); ++it) {
        if (it == blockTermIt)
            continue;
        movedInstrs.push_back(std::move(*it));
    }

    il::core::Instr newTerm = std::move(*blockTermIt);
    for (auto &label : newTerm.labels) {
        if (label == block.label)
            label = predBlock->label;
    }

    predInstrs.erase(predTermIt);

    for (auto &instr : movedInstrs)
        predInstrs.push_back(std::move(instr));

    predInstrs.push_back(std::move(newTerm));
    predBlock->terminated = true;

    size_t blockIndex = static_cast<size_t>(std::distance(F.blocks.begin(), blockIt));
    F.blocks.erase(F.blocks.begin() + static_cast<std::ptrdiff_t>(blockIndex));

    return true;
}

} // namespace

/// @brief Merge every eligible single-predecessor block in a function.
///
/// @details Iterates blocks in order, attempting to merge each into its
///          predecessor via @ref mergeSinglePred.  The walk repeats for the next
///          block only when no merge occurred, ensuring indices remain valid as
///          blocks are erased.  When merges succeed the helper updates
///          statistics and emits optional debug output so callers can understand
///          the transformations performed.
///
/// @param ctx SimplifyCFG context owning the function under transformation.
/// @returns True if any block was merged.
bool mergeSinglePredBlocks(SimplifyCFG::SimplifyCFGPassContext &ctx) {
    il::core::Function &F = ctx.function;
    bool changed = false;

    // Build the predecessor map once — O(B * T) where B = blocks, T = avg
    // terminators.  Each mergeSinglePred call then does O(1) edge-count lookup
    // instead of the previous O(B) full scan.  We rebuild after each successful
    // merge since block erasure invalidates the map.
    auto predMap = buildPredMap(F);

    size_t blockIndex = 0;
    while (blockIndex < F.blocks.size()) {
        const bool debugEnabled = ctx.isDebugLoggingEnabled();
        std::string mergedLabel;
        if (debugEnabled)
            mergedLabel = F.blocks[blockIndex].label;

        if (mergeSinglePred(ctx, F.blocks[blockIndex], predMap)) {
            changed = true;
            ++ctx.stats.blocksMerged;
            if (debugEnabled) {
                std::string message = "merged block '" + mergedLabel + "' into its predecessor";
                ctx.logDebug(message);
            }
            // Rebuild pred map after structural change.
            predMap = buildPredMap(F);
            continue;
        }
        ++blockIndex;
    }

    return changed;
}

} // namespace il::transform::simplify_cfg
