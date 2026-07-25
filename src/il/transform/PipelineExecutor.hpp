//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/PipelineExecutor.hpp
// Purpose: Coordinates execution of optimisation pass pipelines on IL modules.
//          Resolves pass names to registered factories, manages analysis
//          caching/invalidation via AnalysisManager, and invokes
//          instrumentation hooks (IR printing, verification, metrics).
// Key invariants:
//   - Pass and analysis registries are borrowed by const reference and must
//     outlive the executor.
//   - A single AnalysisManager is created per pipeline run.
// Ownership/Lifetime: PipelineExecutor borrows registries and instrumentation
//          callbacks; does not own them. Caller owns the Module.
// Links: il/transform/PassRegistry.hpp, il/transform/AnalysisManager.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "il/core/fwd.hpp"
#include "il/transform/AnalysisManager.hpp"
#include "il/transform/PassRegistry.hpp"
#include "zanna/pass/PassManager.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace il::transform {

class PipelineExecutor {
  public:
    /// @brief Measurements captured around one pass invocation.
    struct PassMetrics {
        /// @brief Aggregate basic-block and instruction counts for a module.
        struct IRSize {
            /// Number of basic blocks across all functions.
            std::size_t blocks = 0;
            /// Number of instructions across all blocks.
            std::size_t instructions = 0;
        };

        /// IR size immediately before the pass.
        IRSize before;
        /// IR size immediately after the pass and optional verification.
        IRSize after;
        /// New analysis computations attributable to the pass.
        AnalysisCounts analysesComputed{};
        /// Wall-clock duration of pass execution, excluding verification.
        std::chrono::nanoseconds duration{};
        /// Wall-clock duration of the optional verification hook.
        std::chrono::nanoseconds verifyDuration{};
        /// Whether a verification hook was invoked.
        bool verifyRan = false;
    };

    /// @brief Configuration for instrumentation hooks around pass execution.
    struct Instrumentation {
        /// Optional hook invoked before each materialized pass.
        zanna::pass::PassManager::PrintHook printBefore;
        /// Optional hook invoked after each successful pass.
        zanna::pass::PassManager::PrintHook printAfter;
        /// Optional hook returning whether post-pass IR verification succeeded.
        zanna::pass::PassManager::VerifyHook verifyEach;
        /// Optional metrics sink receiving one record per executed pass.
        std::function<void(std::string_view id, const PassMetrics &metrics)> passMetrics;
    };

    /// @brief Bind an executor to registries and instrumentation policy.
    /// @param registry Borrowed pass registry that must outlive the executor.
    /// @param analysisRegistry Borrowed analysis registry that must outlive the executor.
    /// @param instrumentation Hooks copied into the executor.
    /// @param parallelFunctionPasses Permit registered parallel-safe function passes
    ///        to run concurrently across module functions.
    PipelineExecutor(const PassRegistry &registry,
                     const AnalysisRegistry &analysisRegistry,
                     Instrumentation instrumentation,
                     bool parallelFunctionPasses = false);

    /// @brief Execute the pass pipeline against @p module.
    /// @details Resolves each pass name via the registry, constructs an
    ///          @ref AnalysisManager, and drives module or function passes in
    ///          order.  Instrumentation hooks are called around each pass.
    /// @param module   Module to transform in place.
    /// @param pipeline Ordered list of pass identifiers to run.
    /// @return @c true when every pass was materialized and executed
    ///         successfully; otherwise @c false.
    bool run(core::Module &module, const std::vector<std::string> &pipeline) const;

  private:
    const PassRegistry &registry_;
    const AnalysisRegistry &analysisRegistry_;
    Instrumentation instrumentation_;
    bool parallelFunctionPasses_ = false;
};

} // namespace il::transform
