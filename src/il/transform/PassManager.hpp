//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/PassManager.hpp
// Purpose: IL pass manager -- orchestrates optimisation and analysis pipelines
//          for IL modules. Maintains pass/analysis registries, schedules pass
//          execution, manages analysis caching/invalidation, and provides
//          instrumentation hooks (verification, IR printing, statistics).
// Key invariants:
//   - Passes execute in pipeline order; analysis caches are invalidated per
//     PreservedAnalyses metadata.
//   - Verification between passes is optional and controlled by the caller.
// Ownership/Lifetime: PassManager owns its registries, pipeline map, and
//          instrumentation flags. The caller owns the Module passed to run().
// Links: il/transform/PassRegistry.hpp, il/transform/AnalysisManager.hpp,
//        il/transform/PipelineExecutor.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include "il/core/fwd.hpp"
#include "il/transform/AnalysisManager.hpp"
#include "il/transform/PassRegistry.hpp"

#include <functional>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace il::transform {

struct SimplifyCFG;

class PipelineExecutor;

/// @brief Orchestrates execution of transformation passes and analyses on IL modules.
/// @details The PassManager maintains registries for passes and analyses,
///          builds optimization pipelines, and provides debugging features like
///          verification and printing between pass executions.
class PassManager {
  public:
    /// @brief Ordered sequence of pass identifiers executed as one pipeline.
    using Pipeline = std::vector<std::string>;

    /// @brief Construct a new PassManager with default settings.
    PassManager();

    /// @brief Get mutable access to the pass registry.
    /// @return Reference to the internal PassRegistry.
    PassRegistry &passes() {
        return passRegistry_;
    }

    /// @brief Get const access to the pass registry.
    /// @return Const reference to the internal PassRegistry.
    const PassRegistry &passes() const {
        return passRegistry_;
    }

    /// @brief Get mutable access to the analysis registry.
    /// @return Reference to the internal AnalysisRegistry.
    AnalysisRegistry &analyses() {
        return analysisRegistry_;
    }

    /// @brief Get const access to the analysis registry.
    /// @return Const reference to the internal AnalysisRegistry.
    const AnalysisRegistry &analyses() const {
        return analysisRegistry_;
    }

    /// @brief Register a module-level analysis.
    /// @tparam Result Type returned by the analysis computation.
    /// @param id Unique identifier for the analysis.
    /// @param fn Function computing the analysis result from a module.
    template <typename Result>
    void registerModuleAnalysis(const std::string &id, std::function<Result(core::Module &)> fn) {
        analysisRegistry_.registerModuleAnalysis<Result>(id, std::move(fn));
    }

    /// @brief Register a function-level analysis.
    /// @tparam Result Type returned by the analysis computation.
    /// @param id Unique identifier for the analysis.
    /// @param fn Function computing the analysis result from a function.
    template <typename Result>
    void registerFunctionAnalysis(const std::string &id,
                                  std::function<Result(core::Module &, core::Function &)> fn) {
        analysisRegistry_.registerFunctionAnalysis<Result>(id, std::move(fn));
    }

    /// @brief Register a module pass using a factory function.
    /// @param id Unique identifier for the pass.
    /// @param factory Function returning a new ModulePass instance.
    /// @param parallelSafe Whether the pass is audited for concurrent execution.
    void registerModulePass(const std::string &id,
                            PassRegistry::ModulePassFactory factory,
                            bool parallelSafe = false) {
        passRegistry_.registerModulePass(id, std::move(factory), parallelSafe);
    }

    /// @brief Register a module pass using a callback with analysis access.
    /// @param id Unique identifier for the pass.
    /// @param callback Function implementing the pass transformation.
    /// @param parallelSafe Whether the callback is audited for concurrent execution.
    void registerModulePass(const std::string &id,
                            PassRegistry::ModulePassCallback callback,
                            bool parallelSafe = false) {
        passRegistry_.registerModulePass(id, std::move(callback), parallelSafe);
    }

    /// @brief Register a simple module pass without analysis access.
    /// @param id Unique identifier for the pass.
    /// @param fn Function transforming the module (no return value).
    /// @param parallelSafe Whether the callback is audited for concurrent execution.
    void registerModulePass(const std::string &id,
                            const std::function<void(core::Module &)> &fn,
                            bool parallelSafe = false) {
        passRegistry_.registerModulePass(id, fn, parallelSafe);
    }

    /// @brief Register a function pass using a factory function.
    /// @param id Unique identifier for the pass.
    /// @param factory Function returning a new FunctionPass instance.
    /// @param parallelSafe Permit execution across functions when parallel mode is enabled.
    void registerFunctionPass(const std::string &id,
                              PassRegistry::FunctionPassFactory factory,
                              bool parallelSafe = false) {
        passRegistry_.registerFunctionPass(id, std::move(factory), parallelSafe);
    }

    /// @brief Register a function pass using a callback with analysis access.
    /// @param id Unique identifier for the pass.
    /// @param callback Function implementing the pass transformation.
    /// @param parallelSafe Permit execution across functions when parallel mode is enabled.
    void registerFunctionPass(const std::string &id,
                              PassRegistry::FunctionPassCallback callback,
                              bool parallelSafe = false) {
        passRegistry_.registerFunctionPass(id, std::move(callback), parallelSafe);
    }

    /// @brief Register a simple function pass without analysis access.
    /// @param id Unique identifier for the pass.
    /// @param fn Function transforming a function (no return value).
    /// @param parallelSafe Permit execution across functions when parallel mode is enabled.
    void registerFunctionPass(const std::string &id,
                              const std::function<void(core::Function &)> &fn,
                              bool parallelSafe = false) {
        passRegistry_.registerFunctionPass(id, fn, parallelSafe);
    }

    /// @brief Add the SimplifyCFG pass to the pass registry.
    /// @param aggressive Enable aggressive control flow simplification.
    void addSimplifyCFG(bool aggressive = true);

    /// @brief Register a named pipeline of pass identifiers.
    /// @param id Unique identifier for the pipeline.
    /// @param pipeline Ordered list of pass names to execute.
    void registerPipeline(const std::string &id, Pipeline pipeline);

    /// @brief Look up a registered pipeline by name.
    /// @param id Pipeline identifier to query.
    /// @return Pointer to pipeline if found, nullptr otherwise.
    const Pipeline *getPipeline(const std::string &id) const;

    /// @brief Enable or disable module verification between passes.
    /// @param enable True to verify IR consistency after each pass.
    void setVerifyBetweenPasses(bool enable);

    /// @brief Enable or disable printing IR before each pass.
    /// @param enable True to print the module before pass execution.
    void setPrintBeforeEach(bool enable);

    /// @brief Enable or disable printing IR after each pass.
    /// @param enable True to print the module after pass execution.
    void setPrintAfterEach(bool enable);

    /// @brief Set the output stream for instrumentation output.
    /// @param os Stream for printing IR and diagnostics.
    void setInstrumentationStream(std::ostream &os);

    /// @brief Enable or disable per-pass statistics (IR size, analysis counts).
    /// @param enable When true, emit pass metrics to the instrumentation stream.
    void setReportPassStatistics(bool enable);

    /// @brief Enable parallel execution of audited function passes.
    /// @details When enabled the executor may run function-local passes across multiple
    ///          functions concurrently. This mode is off by default to preserve determinism.
    /// @param enable True to allow parallel function pass execution.
    void enableParallelFunctionPasses(bool enable);

    /// @brief Execute a pipeline of passes on a module.
    /// @param module Module to transform.
    /// @param pipeline Ordered list of pass identifiers to run.
    /// @return @c true when every pass in the pipeline was materialized and
    ///         executed successfully; otherwise @c false.
    bool run(core::Module &module, const Pipeline &pipeline) const;

    /// @brief Execute a named registered pipeline on a module.
    /// @param module Module to transform.
    /// @param pipelineId Identifier of the registered pipeline.
    /// @return @c true when the pipeline existed and every pass in it ran
    ///         successfully; otherwise @c false.
    bool runPipeline(core::Module &module, const std::string &pipelineId) const;

  private:
    AnalysisRegistry analysisRegistry_;
    PassRegistry passRegistry_;
    std::unordered_map<std::string, Pipeline> pipelines_;
    bool verifyBetweenPasses_ = false;
    bool printBeforeEach_ = false;
    bool printAfterEach_ = false;
    std::ostream *instrumentationStream_ = nullptr;
    bool reportPassStatistics_ = false;
    bool parallelFunctionPasses_ = false;
};

} // namespace il::transform
