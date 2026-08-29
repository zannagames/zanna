//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/transform/PassManager.cpp
// Purpose: Implement the orchestration layer that registers passes, manages
//          pipelines, and invokes the executor to run them.
// Links: docs/internals/architecture.md#passes
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines the pass manager responsible for constructing and executing transformation
/// pipelines.
/// @details Provides registration facilities for analyses, passes, and pipelines
///          while delegating execution to @ref PipelineExecutor.  Keeping this
///          coordination logic isolated simplifies testing and maintenance of
///          the transformation stack.

#include "il/transform/PassManager.hpp"
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include "il/analysis/BasicAA.hpp"
#include "il/analysis/CFG.hpp"
#include "il/analysis/Dominators.hpp"
#include "il/analysis/IntRangeAnalysis.hpp"
#include "il/analysis/MemorySSA.hpp"
#include "il/io/Serializer.hpp"
#include "il/transform/AnalysisIDs.hpp"
#include "il/transform/ArrayFastPathOpt.hpp"
#include "il/transform/CheckOpt.hpp"
#include "il/transform/Devirtualize.hpp"
#include "il/transform/IfConvert.hpp"
#include "il/transform/LICM.hpp"
#include "il/transform/LateCleanup.hpp"
#include "il/transform/LoopUnroll.hpp"
#include "il/transform/OwnershipOpt.hpp"
#include "il/transform/PipelineExecutor.hpp"
#include "il/transform/RuntimeFastPathOpt.hpp"
#include "il/transform/SiblingRecursion.hpp"
#include "il/transform/SimplifyCFG.hpp"
#include "il/transform/analysis/Liveness.hpp"
#include "il/transform/analysis/LoopInfo.hpp"
#include "il/verify/Verifier.hpp"
#include "support/diag_expected.hpp"
#include "zanna/pass/PassManager.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <utility>

using namespace il::core;

namespace il::transform {

/// @brief Initialise the pass manager with default analyses and settings.
/// @details Registers
///          core analyses (CFG, dominator tree, and liveness) used by canonical
///          pipelines.  The registrations install factory callbacks that lazily
///          compute results when passes request them.
PassManager::PassManager() {
    instrumentationStream_ = &std::cerr;

    /// Build a control-flow graph for a requested function.
    analysisRegistry_.registerFunctionAnalysis<CFGInfo>(
        kAnalysisCFG,
        [](core::Module &module, core::Function &fn) { return buildCFG(module, fn); });
    /// Build dominance information from the module-aware CFG context.
    analysisRegistry_.registerFunctionAnalysis<zanna::analysis::DomTree>(
        kAnalysisDominators, [](core::Module &module, core::Function &fn) {
            zanna::analysis::CFGContext ctx(module, fn);
            return zanna::analysis::computeDominatorTree(ctx, fn);
        });
    /// Discover natural loops for a requested function.
    analysisRegistry_.registerFunctionAnalysis<LoopInfo>(
        kAnalysisLoopInfo,
        [](core::Module &module, core::Function &fn) { return computeLoopInfo(module, fn); });
    /// Compute block and value liveness for a requested function.
    analysisRegistry_.registerFunctionAnalysis<LivenessInfo>(
        kAnalysisLiveness,
        [](core::Module &module, core::Function &fn) { return computeLiveness(module, fn); });
    // Basic alias analysis for memory disambiguation (available to DSE/LICM etc.)
    /// Construct basic alias-analysis state for a requested function.
    analysisRegistry_.registerFunctionAnalysis<zanna::analysis::BasicAA>(
        kAnalysisBasicAA, [](core::Module &module, core::Function &fn) {
            return zanna::analysis::BasicAA(module, fn);
        });
    // MemorySSA: precise def-use chains for memory operations; used by DSE for
    // cross-block dead-store elimination without false read-barriers on calls.
    /// Compute MemorySSA using a function-local basic alias analysis instance.
    analysisRegistry_.registerFunctionAnalysis<zanna::analysis::MemorySSA>(
        kAnalysisMemorySSA, [](core::Module &module, core::Function &fn) {
            zanna::analysis::BasicAA aa(module, fn);
            return zanna::analysis::computeMemorySSA(fn, aa);
        });
    // Integer value ranges: whole-function forward dataflow used by CheckOpt
    // to prove overflow, bounds, and divide-by-zero checks redundant.
    /// Compute whole-function integer ranges; module state is not required.
    analysisRegistry_.registerFunctionAnalysis<zanna::analysis::IntRangeInfo>(
        kAnalysisIntRanges,
        [](core::Module &, core::Function &fn) { return zanna::analysis::computeIntRanges(fn); });

    addSimplifyCFG(false); // Register simplify-cfg pass (non-aggressive by default)
    registerLoopSimplifyPass(passRegistry_);
    registerLICMPass(passRegistry_);
    registerLICMSafePass(passRegistry_);
    registerSCCPPass(passRegistry_);
    registerConstFoldPass(passRegistry_);
    registerPeepholePass(passRegistry_);
    registerDCEPass(passRegistry_);
    registerMem2RegPass(passRegistry_);
    registerDSEPass(passRegistry_);
    registerEarlyCSEPass(passRegistry_);
    registerGVNPass(passRegistry_);
    registerIndVarSimplifyPass(passRegistry_);
    registerInlinePass(passRegistry_);
    registerCheckOptPass(passRegistry_);
    registerLateCleanupPass(passRegistry_);
    registerLoopUnrollPass(passRegistry_);
    registerSiblingRecursionPass(passRegistry_);
    registerReassociatePass(passRegistry_);
    registerEHOptPass(passRegistry_);
    registerLoopRotatePass(passRegistry_);
    registerOwnershipOptPass(passRegistry_);
    registerArrayFastPathOptPass(passRegistry_);
    registerRuntimeFastPathOptPass(passRegistry_);
    registerDevirtualizePass(passRegistry_);
    registerIfConvertPass(passRegistry_);

    // Pre-register common pipelines
    registerPipeline("O0", {"simplify-cfg", "dce"});
    // Targeted validation pipelines. These remain useful even after the passes
    // are promoted to canonical presets because differential, randomized, and
    // native-vs-VM validation can exercise one pass in relative isolation
    // without rewriting the production pipelines.
    registerPipeline("rehab-mem2reg", {"simplify-cfg", "mem2reg", "dce"});
    registerPipeline("rehab-peephole", {"peephole", "dce"});
    registerPipeline("rehab-licm", {"loop-simplify", "licm", "simplify-cfg", "dce"});
    // mem2reg is enabled after dominance, edge-repair, non-entry alloca, and
    // loop-reentered dynamic-allocation guards made promotion verifier-clean.
    // inline is re-enabled in O1 with conservative eligibility checks:
    // block insertion now preserves textual def-before-use ordering, and
    // callees with allocas / non-scalar signatures remain non-inlineable.
    // Full IL peephole is enabled in O1/O2 after the local-use, use-count, and
    // signed-zero fixes made its rule set verifier-clean across the suite.
    // Full LICM is enabled in O2 after load-safety and BasicAA mod/ref guards
    // made memory hoisting verifier-clean and alias-conservative.
    registerPipeline("O1",
                     {"simplify-cfg",
                      "mem2reg",
                      "simplify-cfg",
                      "sccp",
                      "constfold",
                      "peephole",
                      "dce",
                      "simplify-cfg",
                      "sccp",
                      "inline",
                      "peephole",
                      "dce",
                      "simplify-cfg"});
    // O2 pipeline with interprocedural constant propagation:
    // Run SCCP both before (to simplify callees) and after inline
    // (to propagate constants through inlined code from call sites).
    // mem2reg runs before loop optimizers so stack-promoted induction variables
    // and scalar temporaries are visible to SCCP/LICM/loop cleanup.
    // Ordering notes:
    //   - reassociate runs BEFORE earlycse/gvn so canonicalized expression
    //     trees feed CSE (running it after leaves redundancies unmatched).
    //   - check-opt is followed by loop-simplify+licm: deleted/hoisted checks
    //     unlock loop-invariant hoisting that was blocked by trapping ops.
    //   - a bounded second scalar group (sccp..simplify-cfg) catches
    //     second-order opportunities exposed by inlining and loop opts.
    registerPipeline("O2",
                     {"simplify-cfg",
                      "mem2reg",
                      "simplify-cfg",
                      "loop-simplify",
                      "licm",
                      "loop-rotate",
                      "indvars",
                      "loop-unroll",
                      "simplify-cfg",
                      "reassociate", // Canonicalize before CSE and inlining
                      "earlycse",
                      "sccp", // Pre-inline SCCP: simplify callees
                      "check-opt",
                      "loop-simplify",
                      "licm", // Re-hoist after check-opt removed trapping blockers
                      "eh-opt",
                      "dce",
                      "simplify-cfg",
                      "sibling-recursion",
                      "devirt",
                      "inline-o2",
                      "simplify-cfg",
                      "sccp",      // Post-inline SCCP: propagate call-site constants
                      "constfold", // Fold runtime math calls exposed by SCCP
                      "loop-simplify",
                      "licm",
                      "loop-rotate",
                      "indvars",
                      "loop-unroll",
                      "reassociate", // Canonicalize loop-opt output for GVN/CSE
                      "gvn",
                      "earlycse",
                      "check-opt",
                      "loop-simplify",
                      "licm",
                      "runtime-fastpath",
                      "array-fastpath",
                      "ownership-opt",
                      "peephole",
                      "check-opt",
                      "dce",
                      "simplify-cfg",
                      // Bounded second scalar iteration: inlining + loop opts
                      // expose constants and redundancies the first pass missed.
                      "sccp",
                      "constfold",
                      "peephole",
                      "gvn",
                      "earlycse",
                      // If-conversion runs after every check-opt so branch-
                      // derived range proofs are already consumed, and before
                      // the final cleanup so emptied blocks fold away.
                      "if-conv",
                      "dce",
                      "simplify-cfg",
                      "dse",
                      "dce",
                      "late-cleanup"});
}

/// @brief Register the SimplifyCFG transform in the function pass registry.
/// @details Installs a factory that constructs @ref SimplifyCFG with the
///          requested aggressiveness.  When the pass reports no changes the
///          helper returns a fully preserved analysis set; otherwise analyses are
///          invalidated so downstream passes recompute what they need.
/// @param aggressive Whether to enable aggressive simplifications.
void PassManager::addSimplifyCFG(bool aggressive) {
    /// Run configured CFG simplification and report conservative preservation.
    passRegistry_.registerFunctionPass(
        "simplify-cfg",
        [aggressive](core::Function &function, AnalysisManager &analysis) {
            SimplifyCFG pass(aggressive);
            pass.setModule(&analysis.module());
            pass.setAnalysisManager(&analysis);
            bool changed = pass.run(function, nullptr);
            if (!changed)
                return PreservedAnalyses::all();

            PreservedAnalyses preserved;
            preserved.preserveAllModules();
            return preserved;
        },
        true);
}

/// @brief Associate a pipeline identifier with a sequence of pass identifiers.
/// @details Stores the pipeline in an internal map keyed by @p id so later calls
///          can retrieve it.  Pipelines are copied into the map to keep the API
///          independent of the caller's container lifetimes.
/// @param id Stable identifier used by tools or clients to request execution.
/// @param pipeline Ordered list of pass identifiers.
void PassManager::registerPipeline(const std::string &id, Pipeline pipeline) {
    pipelines_[id] = std::move(pipeline);
}

/// @brief Look up a previously registered pipeline definition.
/// @details Performs a map lookup and returns a pointer to the stored pipeline
///          when found.  Unknown identifiers yield @c nullptr so callers can
///          report missing configurations gracefully.
/// @param id Identifier used during registration.
/// @return Pointer to the pipeline or @c nullptr when the identifier is unknown.
const PassManager::Pipeline *PassManager::getPipeline(const std::string &id) const {
    auto it = pipelines_.find(id);
    return it == pipelines_.end() ? nullptr : &it->second;
}

/// @brief Enable or disable verifier checks between pipeline passes.
/// @details Toggles the flag forwarded to @ref PipelineExecutor so debug builds
///          can optionally verify module integrity between passes.
/// @param enable When @c true, the executor will run the IL verifier after each
///               pass in debug builds.
void PassManager::setVerifyBetweenPasses(bool enable) {
    verifyBetweenPasses_ = enable;
}

/// @copydoc PassManager::setPrintBeforeEach()
void PassManager::setPrintBeforeEach(bool enable) {
    printBeforeEach_ = enable;
}

/// @copydoc PassManager::setPrintAfterEach()
void PassManager::setPrintAfterEach(bool enable) {
    printAfterEach_ = enable;
}

/// @copydoc PassManager::setInstrumentationStream()
void PassManager::setInstrumentationStream(std::ostream &os) {
    instrumentationStream_ = &os;
}

/// @copydoc PassManager::setReportPassStatistics()
void PassManager::setReportPassStatistics(bool enable) {
    reportPassStatistics_ = enable;
}

/// @copydoc PassManager::enableParallelFunctionPasses()
void PassManager::enableParallelFunctionPasses(bool enable) {
    parallelFunctionPasses_ = enable;
}

/// @brief Execute a specific pipeline against a module.
/// @details Constructs a @ref PipelineExecutor using the current pass and
///          analysis registries, then invokes it with the provided pipeline.
///          Ownership of passes remains with the executor, keeping the manager
///          itself stateless.
/// @param module Module undergoing transformation.
/// @param pipeline Ordered list of pass identifiers to execute.
bool PassManager::run(core::Module &module, const Pipeline &pipeline) const {
    PipelineExecutor::Instrumentation instrumentation{};

    if (printBeforeEach_ && instrumentationStream_) {
        /// Serialize the complete module immediately before a pass runs.
        instrumentation.printBefore = [this, &module](std::string_view passId) {
            *instrumentationStream_ << "*** IR before pass '" << passId << "' ***\n";
            il::io::Serializer::write(module, *instrumentationStream_);
            *instrumentationStream_ << "\n";
        };
    }

    if (printAfterEach_ && instrumentationStream_) {
        /// Serialize the complete module immediately after a pass runs.
        instrumentation.printAfter = [this, &module](std::string_view passId) {
            *instrumentationStream_ << "*** IR after pass '" << passId << "' ***\n";
            il::io::Serializer::write(module, *instrumentationStream_);
            *instrumentationStream_ << "\n";
        };
    }

    if (verifyBetweenPasses_) {
        /// Verify the module and report the first diagnostic after each pass.
        instrumentation.verifyEach = [this, &module](std::string_view passId) {
            auto result = il::verify::Verifier::verify(module);
            if (!result) {
                std::ostream &out = instrumentationStream_ ? *instrumentationStream_ : std::cerr;
                out << "verification failed after pass '" << passId << "'\n";
                il::support::printDiag(result.error(), out);
                out << "\n";
                return false;
            }
            return true;
        };
    }

    if (reportPassStatistics_ && instrumentationStream_) {
        /// Emit compact size, analysis-computation, duration, and verification metrics.
        instrumentation.passMetrics = [this](std::string_view passId,
                                             const PipelineExecutor::PassMetrics &metrics) {
            if (!instrumentationStream_)
                return;
            const auto micros =
                std::chrono::duration_cast<std::chrono::microseconds>(metrics.duration).count();
            *instrumentationStream_
                << "[pass " << passId << "] bb " << metrics.before.blocks << " -> "
                << metrics.after.blocks << ", inst " << metrics.before.instructions << " -> "
                << metrics.after.instructions
                << ", analyses M:" << metrics.analysesComputed.moduleComputations
                << " F:" << metrics.analysesComputed.functionComputations << ", time " << micros
                << "us";
            if (metrics.verifyRan) {
                const auto verifyMicros =
                    std::chrono::duration_cast<std::chrono::microseconds>(metrics.verifyDuration)
                        .count();
                *instrumentationStream_ << ", verify " << verifyMicros << "us";
            }
            *instrumentationStream_ << "\n";
        };
    }

    PipelineExecutor executor(
        passRegistry_, analysisRegistry_, std::move(instrumentation), parallelFunctionPasses_);
    return executor.run(module, pipeline);
}

/// @brief Execute a named pipeline if it exists.
/// @details Uses @ref getPipeline to retrieve the configuration and delegates to
///          @ref run when found.  Returns false when the pipeline identifier is
///          unknown so callers can fall back to alternative behaviours.
/// @param module Module undergoing transformation.
/// @param pipelineId Identifier of the desired pipeline.
/// @return @c true when the pipeline was found and executed; otherwise @c false.
/// @brief Function-level triage for the IL optimizer (`ZANNA_IL_OPT_KEEP_FUNCS=<file>`).
/// @details When set, the file lists one IL function name per line; after a
///          named pipeline runs, every function NOT listed is restored to its
///          pre-pipeline body, and functions, externs, and globals the pipeline
///          removed are re-added, so a program-level oracle (VM output vs
///          native output) can bisect "which optimized function changes the
///          result" without a compiler rebuild. Plan 80 E0b found the AArch64
///          divergence this way. Bisection aid only; the variable is read once
///          per pipeline run.
/// @param optimized Module after the pipeline (mutated in place).
/// @param original Snapshot taken before the pipeline ran.
static void revertUnlistedFunctions(core::Module &optimized, const core::Module &original) {
    const char *listPath = std::getenv("ZANNA_IL_OPT_KEEP_FUNCS");
    if (!listPath)
        return;
    std::unordered_set<std::string> keep;
    std::ifstream in(listPath);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty())
            keep.insert(line);
    }
    std::unordered_map<std::string, const core::Function *> originalByName;
    for (const auto &fn : original.functions)
        originalByName[fn.name] = &fn;
    std::unordered_set<std::string> present;
    for (auto &fn : optimized.functions) {
        present.insert(fn.name);
        if (keep.count(fn.name))
            continue;
        auto it = originalByName.find(fn.name);
        if (it != originalByName.end())
            fn = *it->second;
    }
    for (const auto &fn : original.functions) {
        if (!present.count(fn.name))
            optimized.functions.push_back(fn);
    }
    std::unordered_set<std::string> externNames;
    for (const auto &e : optimized.externs)
        externNames.insert(e.name);
    for (const auto &e : original.externs) {
        if (!externNames.count(e.name))
            optimized.externs.push_back(e);
    }
    std::unordered_set<std::string> globalNames;
    for (const auto &g : optimized.globals)
        globalNames.insert(g.name);
    for (const auto &g : original.globals) {
        if (!globalNames.count(g.name))
            optimized.globals.push_back(g);
    }
}

bool PassManager::runPipeline(core::Module &module, const std::string &pipelineId) const {
    const Pipeline *pipeline = getPipeline(pipelineId);
    if (!pipeline)
        return false;
    const bool triage = std::getenv("ZANNA_IL_OPT_KEEP_FUNCS") != nullptr;
    core::Module snapshot;
    if (triage)
        snapshot = module;
    const bool ok = run(module, *pipeline);
    if (ok && triage)
        revertUnlistedFunctions(module, snapshot);
    return ok;
}

} // namespace il::transform
