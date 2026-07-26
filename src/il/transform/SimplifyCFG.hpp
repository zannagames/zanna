//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/SimplifyCFG.hpp
// Purpose: Control Flow Graph Simplification pass -- canonicalises and
//          simplifies CFG patterns via fixed-point iteration: constant-branch
//          folding, forwarding-block removal, single-predecessor merging,
//          unreachable-block elimination, and block parameter canonicalisation.
//          EH-aware; preserves exception handling semantics.
// Key invariants:
//   - Fixed-point iteration stops when no further changes occur.
//   - EH-sensitive blocks (handlers, cleanup) are never simplified.
//   - Sub-transformations are in SimplifyCFG/ subdirectory modules.
// Ownership/Lifetime: SimplifyCFG is a value type holding an aggressive flag
//          and optional borrowed pointers to Module/AnalysisManager.
// Links: il/core/Function.hpp, il/core/BasicBlock.hpp,
//        il/transform/SimplifyCFG/BlockMerging.hpp,
//        il/transform/SimplifyCFG/ForwardingElimination.hpp,
//        il/transform/SimplifyCFG/ReachabilityCleanup.hpp,
//        il/transform/SimplifyCFG/ParamCanonicalization.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares fixed-point structural simplification of IL control-flow graphs.
 *
 * @details `SimplifyCFG` coordinates branch/switch folding, jump threading,
 *          forwarding-block removal, single-predecessor merging, unreachable
 *          cleanup, and block-parameter canonicalization. Exception-sensitive
 *          shapes remain protected, while optional aggressive mode enables the
 *          transformations with broader CFG reach.
 */

#pragma once

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Module.hpp"

#include <string_view>

namespace il::transform {

class AnalysisManager;

/// @brief Simplify IL control-flow graphs by folding and pruning trivial shapes.
///
/// @details The pass focuses on canonicalising branching and block structure so
/// subsequent optimisations can operate on a predictable CFG. The scaffold keeps
/// statistics for each transformation performed during fixed-point iteration.
struct SimplifyCFG {
    /// @brief Aggregated statistics from a pass invocation.
    struct Stats {
        /// @brief Number of conditional branches simplified.
        size_t cbrToBr = 0;
        /// @brief Count of empty blocks eliminated.
        size_t emptyBlocksRemoved = 0;
        /// @brief Predecessor edge merges performed.
        size_t predsMerged = 0;
        /// @brief Block parameter reductions.
        size_t paramsShrunk = 0;
        /// @brief Adjacent block merges.
        size_t blocksMerged = 0;
        /// @brief Unreachable block removals.
        size_t unreachableRemoved = 0;
        /// @brief Switches rewritten to unconditional branches.
        size_t switchToBr = 0;
    };

    /// @brief Per-run context shared across helper routines.
    struct SimplifyCFGPassContext {
        /// @brief Construct a pass context for simplifying a function.
        /// @param function The function to simplify.
        /// @param module Parent module (may be null if unavailable).
        /// @param stats Statistics accumulator updated during the pass.
        SimplifyCFGPassContext(il::core::Function &function,
                               const il::core::Module *module,
                               Stats &stats);

        /// @brief Function currently being simplified.
        il::core::Function &function;
        /// @brief Borrowed parent module, or nullptr when unavailable.
        const il::core::Module *module;
        /// @brief Mutable statistics accumulator for the run.
        Stats &stats;

        /// @brief Check if debug logging is enabled for this pass context.
        /// @return True if debug messages should be emitted.
        bool isDebugLoggingEnabled() const;

        /// @brief Emit a debug log message if logging is enabled.
        /// @param message The message to log.
        void logDebug(std::string_view message) const;

        /// @brief Check if a block is sensitive to exception handling.
        /// @details EH-sensitive blocks (handlers, cleanup) require special care
        ///          during CFG transformations to preserve exception semantics.
        /// @param block The block to check.
        /// @return True if the block should be treated as EH-sensitive.
        bool isEHSensitive(const il::core::BasicBlock &block) const;

      private:
        /// @brief Cached debug-logging policy for inexpensive repeated checks.
        bool debugLoggingEnabled_ = false;
    };

    /// @brief Create a CFG simplifier.
    /// @param aggressive Enable switch folding and jump threading when true.
    explicit SimplifyCFG(bool aggressive = true) : aggressive(aggressive) {}

    /// @brief Provide the module containing functions processed by this pass.
    /// @param module Borrowed parent module pointer, or null when unavailable.
    void setModule(const il::core::Module *module) {
        module_ = module;
    }

    /// @brief Provide the active analysis manager so the pass can invalidate caches.
    /// @param manager Borrowed analysis manager pointer, or null for standalone use.
    void setAnalysisManager(AnalysisManager *manager) {
        analysisManager_ = manager;
    }

    /// @brief Run the simplification pass on a single function.
    /// @param F Function mutated in place.
    /// @param outStats Optional pointer populated with pass statistics.
    /// @return True if the pass modified the function.
    bool run(il::core::Function &F, Stats *outStats = nullptr);

  private:
    /// @brief Whether switch folding and jump threading are enabled.
    bool aggressive;
    /// @brief Borrowed parent module used by pass-wide context, or nullptr.
    const il::core::Module *module_ = nullptr;
    /// @brief Borrowed analysis manager invalidated after CFG changes, or nullptr.
    AnalysisManager *analysisManager_ = nullptr;
};

} // namespace il::transform
