//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/LateCleanup.hpp
// Purpose: Late-pipeline cleanup module pass -- bounded fixpoint of
//          SimplifyCFG + DCE designed for the tail of the O2 pipeline.
//          Records IL size before/after for diagnostics via LateCleanupStats.
// Key invariants:
//   - Iteration count is capped to avoid unbounded work.
//   - Stops early when no further reduction is observed.
// Ownership/Lifetime: ModulePass instantiated by the registry. Optional
//          LateCleanupStats pointer is borrowed; must outlive run().
// Links: il/transform/PassRegistry.hpp, il/transform/SimplifyCFG.hpp,
//        il/transform/DCE.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <vector>

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Aggregate size information collected while running LateCleanup.
/// @details Captures instruction and block counts before the first iteration,
///          after the final iteration, and at each intermediate step.  This
///          allows callers to evaluate the effectiveness of the late-cleanup
///          fixpoint and to produce diagnostics or pass-pipeline statistics.
struct LateCleanupStats {
    unsigned iterations = 0;                ///< Number of SimplifyCFG+DCE iterations that executed.
    std::size_t instrBefore = 0;            ///< Total instruction count before the first iteration.
    std::size_t blocksBefore = 0;           ///< Total basic-block count before the first iteration.
    std::size_t instrAfter = 0;             ///< Total instruction count after the final iteration.
    std::size_t blocksAfter = 0;            ///< Total basic-block count after the final iteration.
    std::vector<std::size_t> instrPerIter;  ///< Instruction count snapshot after each iteration.
    std::vector<std::size_t> blocksPerIter; ///< Basic-block count snapshot after each iteration.
};

/// @brief Late-pipeline cleanup pass combining CFG simplification and DCE.
/// @details Runs a bounded fixpoint of SimplifyCFG then DCE, tracking IL size
///          per iteration. This is a cheap cleanup pass designed for the tail
///          of optimization pipelines; iteration count is capped to avoid
///          unbounded work while still converging on a small fixpoint.
class LateCleanup : public ModulePass {
  public:
    /// @brief Optionally attach a stats sink to observe size deltas.
    /// @param stats Borrowed struct populated during @ref run, or null to disable collection.
    /// @note A non-null sink must remain alive until the pass finishes running.
    void setStats(LateCleanupStats *stats) {
        stats_ = stats;
    }

    /// @brief Get the unique identifier for this pass.
    /// @return String view "late-cleanup".
    std::string_view id() const override;

    /// @brief Execute the late cleanup pass on the module.
    /// @param module Module to transform.
    /// @param analysis Analysis manager for querying cached results.
    /// @return PreservedAnalyses indicating which analyses remain valid.
    PreservedAnalyses run(core::Module &module, AnalysisManager &analysis) override;

  private:
    LateCleanupStats *stats_ = nullptr;
};

/// @brief Register the late cleanup pass with the registry.
/// @param registry PassRegistry to register the pass into.
void registerLateCleanupPass(PassRegistry &registry);

} // namespace il::transform
