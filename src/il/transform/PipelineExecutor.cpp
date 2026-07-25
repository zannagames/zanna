//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/transform/PipelineExecutor.cpp
// Purpose: Define the executor that materialises IL transformation passes and
//          coordinates analysis invalidation between them.
// Links: docs/internals/architecture.md#passes
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the stateful driver that runs pass pipelines.
/// @details Centralising the execution logic ensures registry lookups, analysis
///          invalidation, and optional verification follow a consistent policy
///          across all pipeline invocations.

#include "il/transform/PipelineExecutor.hpp"

#include "il/core/Module.hpp"
#include "zanna/pass/PassManager.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace il::transform {
namespace {
/// @brief Count blocks and instructions across every function in a module.
/// @param module Module measured for instrumentation.
/// @return Aggregate IR size snapshot.
PipelineExecutor::PassMetrics::IRSize computeIRSize(const core::Module &module) {
    PipelineExecutor::PassMetrics::IRSize size{};
    for (const auto &fn : module.functions) {
        size.blocks += fn.blocks.size();
        for (const auto &block : fn.blocks)
            size.instructions += block.instructions.size();
    }
    return size;
}

/// @brief Identify idempotent cleanup passes eligible for redundant-run suppression.
/// @param passId Registered pass identifier.
/// @return `true` for DCE, CFG simplification, and late cleanup.
bool isCleanupPass(std::string_view passId) {
    return passId == "dce" || passId == "simplify-cfg" || passId == "late-cleanup";
}

/// @brief Initial FNV-family state for structural fingerprints.
constexpr std::uint64_t kFingerprintOffset = 1469598103934665603ull;
/// @brief FNV prime used while mixing variable-length byte sequences.
constexpr std::uint64_t kFingerprintPrime = 1099511628211ull;

/// @brief Mix one integer payload into a structural IR fingerprint.
/// @details Uses the FNV-1a recurrence with a final avalanche-style perturbation
///          for multi-byte scalar values. The fingerprint is only used for
///          in-process before/after equality, not for persistence or security.
/// @param hash Running fingerprint state.
/// @param value Payload to incorporate.
void mixFingerprint(std::uint64_t &hash, std::uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    hash *= kFingerprintPrime;
}

/// @brief Mix any non-bool integral payload into a structural IR fingerprint.
/// @param hash Running fingerprint state.
/// @param value Integral payload to incorporate.
template <typename T,
          typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>>
void mixFingerprint(std::uint64_t &hash, T value) {
    mixFingerprint(hash, static_cast<std::uint64_t>(value));
}

/// @brief Mix a boolean payload into a structural IR fingerprint.
/// @param hash Running fingerprint state.
/// @param value Boolean to incorporate.
void mixFingerprint(std::uint64_t &hash, bool value) {
    mixFingerprint(hash, value ? 1ull : 0ull);
}

/// @brief Mix a string payload into a structural IR fingerprint.
/// @param hash Running fingerprint state.
/// @param value Text to incorporate.
void mixFingerprint(std::uint64_t &hash, std::string_view value) {
    mixFingerprint(hash, static_cast<std::uint64_t>(value.size()));
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= kFingerprintPrime;
    }
}

/// @brief Mix an IL type payload into a structural IR fingerprint.
/// @param hash Running fingerprint state.
/// @param type Type to incorporate.
void mixTypeFingerprint(std::uint64_t &hash, const core::Type &type) {
    mixFingerprint(hash, static_cast<std::uint64_t>(type.kind));
}

/// @brief Mix an IL value payload into a structural IR fingerprint.
/// @details Floating constants are mixed by bit representation so values such as
///          negative zero remain distinguishable from positive zero.
/// @param hash Running fingerprint state.
/// @param value Value to incorporate.
void mixValueFingerprint(std::uint64_t &hash, const core::Value &value) {
    mixFingerprint(hash, static_cast<std::uint64_t>(value.kind));
    switch (value.kind) {
        case core::Value::Kind::Temp:
            mixFingerprint(hash, value.id);
            break;
        case core::Value::Kind::ConstInt:
            mixFingerprint(hash, static_cast<std::uint64_t>(value.i64));
            mixFingerprint(hash, value.isBool);
            break;
        case core::Value::Kind::ConstFloat: {
            static_assert(sizeof(double) == sizeof(std::uint64_t));
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value.f64, sizeof(bits));
            mixFingerprint(hash, bits);
            break;
        }
        case core::Value::Kind::ConstStr:
        case core::Value::Kind::GlobalAddr:
            mixFingerprint(hash, value.str);
            break;
        case core::Value::Kind::NullPtr:
            break;
    }
}

/// @brief Mix a parameter declaration into a structural IR fingerprint.
/// @param hash Running fingerprint state.
/// @param param Parameter to incorporate.
void mixParamFingerprint(std::uint64_t &hash, const core::Param &param) {
    mixFingerprint(hash, param.name);
    mixTypeFingerprint(hash, param.type);
    mixFingerprint(hash, param.id);
    mixFingerprint(hash, param.Attrs.noalias);
    mixFingerprint(hash, param.Attrs.nocapture);
    mixFingerprint(hash, param.Attrs.nonnull);
}

/// @brief Mix an instruction into a structural IR fingerprint.
/// @param hash Running fingerprint state.
/// @param instr Instruction to incorporate.
void mixInstructionFingerprint(std::uint64_t &hash, const core::Instr &instr) {
    mixFingerprint(hash, instr.result.has_value());
    if (instr.result)
        mixFingerprint(hash, *instr.result);
    mixFingerprint(hash, static_cast<std::uint64_t>(instr.op));
    mixTypeFingerprint(hash, instr.type);
    mixFingerprint(hash, instr.operands.size());
    for (const auto &operand : instr.operands)
        mixValueFingerprint(hash, operand);
    mixFingerprint(hash, instr.callee);
    mixFingerprint(hash, instr.labels.size());
    for (const auto &label : instr.labels)
        mixFingerprint(hash, label);
    mixFingerprint(hash, instr.brArgs.size());
    for (const auto &argList : instr.brArgs) {
        mixFingerprint(hash, argList.size());
        for (const auto &arg : argList)
            mixValueFingerprint(hash, arg);
    }
    mixFingerprint(hash, instr.loc.file_id);
    mixFingerprint(hash, instr.loc.line);
    mixFingerprint(hash, instr.loc.column);
    mixFingerprint(hash, instr.CallAttr.nothrow);
    mixFingerprint(hash, instr.CallAttr.readonly);
    mixFingerprint(hash, instr.CallAttr.pure);
    mixFingerprint(hash, instr.hasIndirectSignature);
    mixTypeFingerprint(hash, instr.indirectRetType);
    mixFingerprint(hash, instr.indirectParamTypes.size());
    for (const auto &type : instr.indirectParamTypes)
        mixTypeFingerprint(hash, type);
    mixFingerprint(hash, instr.indirectIsVarArg);
}

/// @brief Capture a deterministic structural fingerprint of a module's semantic IR.
/// @details The pass executor uses this to distinguish actual IR mutation from
///          analysis invalidation metadata. It intentionally ignores interned
///          symbol sidecars because they mirror string identifiers and are
///          refreshed by the executor after each pass.
/// @param module Module to fingerprint before or after a pass.
/// @return Stable in-process fingerprint of functions, globals, externs, and bodies.
std::uint64_t moduleStateFingerprint(const core::Module &module) {
    std::uint64_t hash = kFingerprintOffset;
    mixFingerprint(hash, module.version);
    mixFingerprint(hash, module.target.has_value());
    if (module.target)
        mixFingerprint(hash, *module.target);

    mixFingerprint(hash, module.externs.size());
    for (const auto &ext : module.externs) {
        mixFingerprint(hash, ext.name);
        mixTypeFingerprint(hash, ext.retType);
        mixFingerprint(hash, ext.params.size());
        for (const auto &type : ext.params)
            mixTypeFingerprint(hash, type);
        mixFingerprint(hash, ext.Attrs.nothrow);
        mixFingerprint(hash, ext.Attrs.readonly);
        mixFingerprint(hash, ext.Attrs.pure);
    }

    mixFingerprint(hash, module.globals.size());
    for (const auto &global : module.globals) {
        mixFingerprint(hash, global.name);
        mixTypeFingerprint(hash, global.type);
        mixFingerprint(hash, global.init);
        mixFingerprint(hash, static_cast<std::uint64_t>(global.linkage));
        mixFingerprint(hash, global.isConst);
        mixFingerprint(hash, global.hasInitializer);
    }

    mixFingerprint(hash, module.functions.size());
    for (const auto &fn : module.functions) {
        mixFingerprint(hash, fn.name);
        mixTypeFingerprint(hash, fn.retType);
        mixFingerprint(hash, fn.params.size());
        for (const auto &param : fn.params)
            mixParamFingerprint(hash, param);
        mixFingerprint(hash, fn.isVarArg);
        mixFingerprint(hash, static_cast<std::uint64_t>(fn.callingConv));
        mixFingerprint(hash, static_cast<std::uint64_t>(fn.linkage));
        mixFingerprint(hash, fn.Attrs.nothrow);
        mixFingerprint(hash, fn.Attrs.readonly);
        mixFingerprint(hash, fn.Attrs.pure);
        mixFingerprint(hash, fn.valueNames.size());
        for (const auto &name : fn.valueNames)
            mixFingerprint(hash, name);
        mixFingerprint(hash, fn.blocks.size());
        for (const auto &block : fn.blocks) {
            mixFingerprint(hash, block.label);
            mixFingerprint(hash, block.params.size());
            for (const auto &param : block.params)
                mixParamFingerprint(hash, param);
            mixFingerprint(hash, block.terminated);
            mixFingerprint(hash, block.instructions.size());
            for (const auto &instr : block.instructions)
                mixInstructionFingerprint(hash, instr);
        }
    }
    return hash;
}

/// @brief Determine whether expensive pass-change auditing is enabled.
/// @details Normal optimizer execution trusts each pass's PreservedAnalyses
///          result and therefore avoids hashing the complete module around every
///          pass. Setting ZANNA_VERIFY_PASS_CHANGE_REPORTS retains the historical
///          structural fingerprint check for optimizer development and converts
///          any under-reported mutation into a conservative changed result.
/// @return True when structural change reports should be independently audited.
bool verifyPassChangeReports() {
    static const bool enabled = std::getenv("ZANNA_VERIFY_PASS_CHANGE_REPORTS") != nullptr;
    return enabled;
}

/// @brief Select a bounded worker count for function-parallel IL passes.
/// @details ZANNA_OPT_THREADS provides build orchestration with an explicit
///          per-process CPU budget. Invalid or absent values fall back to host
///          hardware concurrency, and non-empty workloads always receive at
///          least one worker.
/// @param functionCount Number of independent functions in the current module.
/// @return Worker count in the inclusive range [1, functionCount] for non-empty input.
std::size_t optimizerWorkerCount(std::size_t functionCount) {
    if (functionCount == 0)
        return 0;
    std::size_t cap =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::thread::hardware_concurrency()));
    if (const char *raw = std::getenv("ZANNA_OPT_THREADS")) {
        std::size_t parsed = 0;
        const char *end = raw + std::strlen(raw);
        const auto result = std::from_chars(raw, end, parsed);
        if (result.ec == std::errc{} && result.ptr == end && parsed > 0)
            cap = parsed;
    }
    return std::min(functionCount, cap);
}
} // namespace

/// @brief Construct an executor bound to specific pass and analysis registries.
/// @details Stores references to the pass and analysis registries plus a flag
///          controlling debug verification.  The executor itself remains
///          lightweight so pass managers can instantiate it per pipeline
///          invocation without sharing mutable state.
/// @param registry Pass registry supplying factories for module/function passes.
/// @param analysisRegistry Registry describing available analyses.
/// @param instrumentation Optional print, verification, and metrics hooks.
/// @param parallelFunctionPasses Permit audited function passes to use worker threads.
PipelineExecutor::PipelineExecutor(const PassRegistry &registry,
                                   const AnalysisRegistry &analysisRegistry,
                                   Instrumentation instrumentation,
                                   bool parallelFunctionPasses)
    : registry_(registry), analysisRegistry_(analysisRegistry),
      instrumentation_(std::move(instrumentation)),
      parallelFunctionPasses_(parallelFunctionPasses) {}

/// @brief Execute the supplied pipeline against the module.
/// @details Creates an @ref AnalysisManager, materialises each pass via the
///          registry, and invokes it with the module or function as appropriate.
///          After each run the helper invalidates analyses based on the
///          preserved set reported by the pass.  In debug builds it can also run
///          the configured verification hook between passes.
/// @param module Module undergoing transformation.
/// @param pipeline Ordered list of pass identifiers.
/// @return `true` when every pass was found, constructed, executed, and verified.
bool PipelineExecutor::run(core::Module &module, const std::vector<std::string> &pipeline) const {
    AnalysisManager analysis(module, analysisRegistry_);
    const bool collectMetrics = static_cast<bool>(instrumentation_.passMetrics);

    zanna::pass::PassManager driver;
    if (instrumentation_.printBefore)
        driver.setPrintBeforeHook(instrumentation_.printBefore);
    if (instrumentation_.printAfter)
        driver.setPrintAfterHook(instrumentation_.printAfter);

    bool changedSinceLastCleanup = true;
    bool hasRunAnyPass = false;
    std::unordered_set<std::string> cleanupPassesRunSinceChange;
    for (const auto &passId : pipeline) {
        /// Materialize and execute one registered pass when the generic driver reaches its id.
        driver.registerPass(
            passId,
            [this,
             &module,
             &analysis,
             &changedSinceLastCleanup,
             &hasRunAnyPass,
             &cleanupPassesRunSinceChange,
             passId,
             collectMetrics]() -> bool {
                if (hasRunAnyPass && isCleanupPass(passId) && !changedSinceLastCleanup &&
                    cleanupPassesRunSinceChange.contains(passId))
                    return true;

                PassMetrics metrics{};
                AnalysisCounts countsBefore{};
                std::chrono::steady_clock::time_point startTime{};
                std::chrono::steady_clock::time_point passEndTime{};
                if (collectMetrics) {
                    metrics.before = computeIRSize(module);
                    countsBefore = analysis.counts();
                    startTime = std::chrono::steady_clock::now();
                }

                const detail::PassFactory *factory = registry_.lookup(passId);
                if (!factory)
                    return false;

                const bool auditChanges = verifyPassChangeReports();
                const std::uint64_t beforeState =
                    auditChanges ? moduleStateFingerprint(module) : std::uint64_t{0};
                bool executed = false;
                bool passChanged = false;
                AnalysisCounts parallelAnalysisCounts{};
                switch (factory->kind) {
                    case detail::PassKind::Module: {
                        if (!factory->makeModule)
                            return false;
                        auto pass = factory->makeModule();
                        if (!pass)
                            return false;
                        PreservedAnalyses preserved = pass->run(module, analysis);
                        passChanged = !preserved.preservesAllAnalyses();
                        analysis.invalidateAfterModulePass(preserved);
                        executed = true;
                        break;
                    }
                    case detail::PassKind::Function: {
                        if (!factory->makeFunction)
                            return false;

                        /// Construct and run one fresh function-pass instance with invalidation.
                        auto runFunctionPass = [&](core::Function &fn,
                                                   AnalysisManager &functionAnalysis,
                                                   bool &functionChanged) -> bool {
                            auto pass = factory->makeFunction();
                            if (!pass)
                                return false;
                            PreservedAnalyses preserved = pass->run(fn, functionAnalysis);
                            functionChanged = !preserved.preservesAllAnalyses();
                            functionAnalysis.invalidateAfterFunctionPass(preserved, fn);
                            return true;
                        };

                        bool executedAll = true;
                        // Parallel execution is reserved for function passes
                        // that were explicitly audited and registered as safe.
                        if (parallelFunctionPasses_ && factory->parallelSafe &&
                            module.functions.size() > 1) {
                            const std::size_t workerCount =
                                optimizerWorkerCount(module.functions.size());
                            std::atomic_size_t nextIndex{0};
                            std::atomic_bool allOk{true};
                            std::atomic_bool anyChanged{false};
                            std::vector<AnalysisCounts> workerCounts(workerCount);
                            std::vector<std::thread> workers;
                            workers.reserve(workerCount);
                            for (std::size_t w = 0; w < workerCount; ++w) {
                                /// Consume function indices and collect analysis counts for one worker.
                                workers.emplace_back([&, w]() {
                                    AnalysisManager workerAnalysis(module, analysisRegistry_);
                                    for (;;) {
                                        std::size_t idx = nextIndex.fetch_add(1);
                                        if (idx >= module.functions.size())
                                            break;
                                        bool functionChanged = false;
                                        if (!runFunctionPass(module.functions[idx],
                                                             workerAnalysis,
                                                             functionChanged))
                                            allOk.store(false, std::memory_order_relaxed);
                                        if (functionChanged)
                                            anyChanged.store(true, std::memory_order_relaxed);
                                    }
                                    workerCounts[w] = workerAnalysis.counts();
                                });
                            }
                            for (auto &worker : workers)
                                worker.join();
                            for (const AnalysisCounts &counts : workerCounts) {
                                parallelAnalysisCounts.moduleComputations +=
                                    counts.moduleComputations;
                                parallelAnalysisCounts.functionComputations +=
                                    counts.functionComputations;
                            }
                            executedAll = allOk.load(std::memory_order_relaxed);
                            passChanged = anyChanged.load(std::memory_order_relaxed);
                            // Workers invalidated only their own throwaway
                            // AnalysisManagers; the persistent one still holds
                            // pre-pass function analyses (dominator trees, CFG
                            // info) whose BasicBlock pointers may now dangle
                            // into reallocated block vectors. The per-function
                            // preservation summaries died with the workers, so
                            // when the pass mutated the module conservatively
                            // drop every cached function analysis. (GVN crashed
                            // intermittently on a stale DomTree without this.)
                            if (executedAll && passChanged) {
                                const PreservedAnalyses nothingPreserved{};
                                for (auto &fn : module.functions)
                                    analysis.invalidateAfterFunctionPass(nothingPreserved, fn);
                            }
                        } else {
                            for (auto &fn : module.functions) {
                                bool functionChanged = false;
                                executedAll &= runFunctionPass(fn, analysis, functionChanged);
                                passChanged = passChanged || functionChanged;
                            }
                        }

                        executed = executedAll;
                        break;
                    }
                }

                if (!executed)
                    return false;
                if (auditChanges)
                    passChanged = passChanged || moduleStateFingerprint(module) != beforeState;
                if (passChanged)
                    module.internOwnedIdentifiers();

                if (collectMetrics)
                    passEndTime = std::chrono::steady_clock::now();

                if (instrumentation_.verifyEach) {
                    const auto verifyStart = std::chrono::steady_clock::now();
                    const bool verified = instrumentation_.verifyEach(passId);
                    metrics.verifyRan = true;
                    if (collectMetrics)
                        metrics.verifyDuration = std::chrono::steady_clock::now() - verifyStart;
                    if (!verified)
                        return false;
                }

                if (collectMetrics && instrumentation_.passMetrics) {
                    metrics.after = computeIRSize(module);
                    AnalysisCounts countsAfter = analysis.counts();
                    metrics.analysesComputed.moduleComputations =
                        countsAfter.moduleComputations - countsBefore.moduleComputations +
                        parallelAnalysisCounts.moduleComputations;
                    metrics.analysesComputed.functionComputations =
                        countsAfter.functionComputations - countsBefore.functionComputations +
                        parallelAnalysisCounts.functionComputations;
                    metrics.duration = passEndTime - startTime;
                    instrumentation_.passMetrics(passId, metrics);
                }

                hasRunAnyPass = true;
                if (passChanged)
                    cleanupPassesRunSinceChange.clear();
                if (isCleanupPass(passId)) {
                    changedSinceLastCleanup = passChanged;
                    cleanupPassesRunSinceChange.insert(passId);
                } else if (passChanged) {
                    changedSinceLastCleanup = true;
                }

                return true;
            });
    }

    bool ok = driver.runPipeline(pipeline);
    if (!ok) {
        std::cerr << "warning: pass pipeline execution failed\n";
    }
    return ok;
}

} // namespace il::transform
