//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the LateCleanup pass for late-pipeline optimization cleanup.
// The pass combines SimplifyCFG and DCE in a bounded fixpoint to efficiently
// remove dead code and simplify control flow created by earlier optimization
// passes.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the LateCleanup pass for the IL optimization pipeline.
/// @details Runs SimplifyCFG and DCE in a bounded fixpoint to clean up
///          unreachable blocks and dead instructions that accumulate late in
///          the pipeline. The pass records optional statistics about size
///          changes across iterations.

#include "il/transform/LateCleanup.hpp"

#include "il/transform/DCE.hpp"
#include "il/transform/SimplifyCFG.hpp"

#include "il/core/Module.hpp"

#include <cstddef>

using namespace il::core;

namespace il::transform {

namespace {
/// @brief Count the total number of instructions in a module.
/// @details Sums instruction counts across all functions and blocks to provide
///          a coarse size metric for fixpoint detection.
/// @param module Module to inspect.
/// @return Total instruction count.
std::size_t countInstructions(const Module &module) {
    std::size_t total = 0;
    for (const auto &fn : module.functions)
        for (const auto &block : fn.blocks)
            total += block.instructions.size();
    return total;
}

/// @brief Count the total number of basic blocks in a module.
/// @details Sums the block counts across all functions to provide a second
///          fixpoint metric alongside instruction count.
/// @param module Module to inspect.
/// @return Total basic block count.
std::size_t countBlocks(const Module &module) {
    std::size_t total = 0;
    for (const auto &fn : module.functions)
        total += fn.blocks.size();
    return total;
}
} // namespace

/// @brief Return the unique identifier for the LateCleanup pass.
/// @details Used by the pass registry and pipeline definitions.
/// @return The canonical pass id string "late-cleanup".
std::string_view LateCleanup::id() const {
    return "late-cleanup";
}

/// @brief Execute the late cleanup pass on a module.
/// @details Iteratively runs SimplifyCFG on each function and then DCE on the
///          whole module until no size changes are observed. Both component
///          passes are reducing transforms, so instruction/block counts provide
///          a finite progress measure. Optional stats record instruction/block counts
///          before and after each iteration.
/// @param module Module to optimize in place.
/// @param analysis Analysis manager for CFG simplification requirements.
/// @return Preserved analysis set; conservative invalidation on change.
PreservedAnalyses LateCleanup::run(Module &module, AnalysisManager &analysis) {
    bool changedAny = false;

    const std::size_t initialInstr = countInstructions(module);
    const std::size_t initialBlocks = countBlocks(module);
    std::size_t currentInstr = initialInstr;
    std::size_t currentBlocks = initialBlocks;

    if (stats_) {
        stats_->instrBefore = initialInstr;
        stats_->blocksBefore = initialBlocks;
    }

    while (true) {
        const std::size_t iterStartInstr = currentInstr;
        const std::size_t iterStartBlocks = currentBlocks;
        bool simplifyChanged = false;

        // Run SimplifyCFG on each function
        for (auto &function : module.functions) {
            SimplifyCFG cfgPass(/*aggressive=*/true);
            cfgPass.setModule(&module);
            cfgPass.setAnalysisManager(&analysis);

            SimplifyCFG::Stats stats;
            simplifyChanged |= cfgPass.run(function, &stats);
        }

        // Run DCE on the entire module
        dce(module);

        currentInstr = countInstructions(module);
        currentBlocks = countBlocks(module);

        const bool sizeChanged = currentInstr != iterStartInstr || currentBlocks != iterStartBlocks;
        const bool iterChanged = simplifyChanged || sizeChanged;

        if (stats_) {
            stats_->instrPerIter.push_back(currentInstr);
            stats_->blocksPerIter.push_back(currentBlocks);
        }

        changedAny |= iterChanged;
        if (!iterChanged)
            break;
    }

    if (stats_) {
        stats_->iterations = static_cast<unsigned>(stats_->instrPerIter.size());
        stats_->instrAfter = currentInstr;
        stats_->blocksAfter = currentBlocks;
    }

    if (!changedAny)
        return PreservedAnalyses::all();

    // Conservative: invalidate everything since we modified code
    return PreservedAnalyses::none();
}

/// @brief Register the LateCleanup pass with the pass registry.
/// @details Associates the "late-cleanup" identifier with a module-level
///          callback that constructs and runs the pass.
/// @param registry Pass registry to update.
void registerLateCleanupPass(PassRegistry &registry) {
    /// Construct and execute late cleanup for a pass-manager module callback.
    registry.registerModulePass("late-cleanup", [](Module &module, AnalysisManager &analysis) {
        LateCleanup pass;
        return pass.run(module, analysis);
    });
}

} // namespace il::transform
