//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the transform pass registry responsible for constructing pass
// instances on demand and tracking preservation summaries.  The registry
// decouples pass registration from pipeline execution so tools can lazily look
// up factories by identifier without hard-coding dependencies.
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements pass preservation summaries, factories, and built-in registration.
 *
 * @details The registry adapts concrete factories, analysis-aware callbacks,
 *          and legacy void transforms to one polymorphic pipeline interface.
 *          Legacy module callbacks use semantic fingerprints to distinguish
 *          true no-ops, while built-in registrations record structural
 *          preservation and parallel-safety policy for each optimizer.
 */

#include "il/transform/PassRegistry.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Extern.hpp"
#include "il/core/Function.hpp"
#include "il/core/Global.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Param.hpp"
#include "il/core/Value.hpp"
#include "il/transform/AnalysisIDs.hpp"
#include "il/transform/AnalysisManager.hpp"
#include "il/transform/ConstFold.hpp"
#include "il/transform/DCE.hpp"
#include "il/transform/DSE.hpp"
#include "il/transform/EHOpt.hpp"
#include "il/transform/EarlyCSE.hpp"
#include "il/transform/GVN.hpp"
#include "il/transform/IndVarSimplify.hpp"
#include "il/transform/Inline.hpp"
#include "il/transform/LICM.hpp"
#include "il/transform/LoopRotate.hpp"
#include "il/transform/LoopSimplify.hpp"
#include "il/transform/Mem2Reg.hpp"
#include "il/transform/Peephole.hpp"
#include "il/transform/Reassociate.hpp"
#include "il/transform/SCCP.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

namespace il::transform {
namespace {

/// @brief Combine one hash value into an accumulated fingerprint.
/// @details Uses the standard boost-style hash-combine formula to build compact
///          semantic fingerprints for legacy void-returning module passes.
/// @param seed Accumulator updated in place.
/// @param value Hash value to fold into @p seed.
void hashCombine(std::size_t &seed, std::size_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

/// @brief Add a boolean field to a semantic fingerprint.
/// @param seed Accumulator updated in place.
/// @param value Boolean value to fold into @p seed.
void hashBool(std::size_t &seed, bool value) {
    hashCombine(seed, std::hash<bool>{}(value));
}

/// @brief Add an IL type discriminator to a semantic fingerprint.
/// @param seed Accumulator updated in place.
/// @param type IL type whose kind is folded into @p seed.
void hashType(std::size_t &seed, const core::Type &type) {
    hashCombine(seed, static_cast<std::size_t>(type.kind));
}

/// @brief Add callable effect attributes to a semantic fingerprint.
/// @param seed Accumulator updated in place.
/// @param attrs Effect attributes to fold into @p seed.
void hashEffectAttrs(std::size_t &seed, const core::EffectAttrs &attrs) {
    hashBool(seed, attrs.nothrow);
    hashBool(seed, attrs.readonly);
    hashBool(seed, attrs.pure);
}

/// @brief Add parameter metadata to a semantic fingerprint.
/// @param seed Accumulator updated in place.
/// @param param Function or block parameter to fold into @p seed.
void hashParam(std::size_t &seed, const core::Param &param) {
    hashCombine(seed, std::hash<std::string>{}(param.name));
    hashType(seed, param.type);
    hashCombine(seed, std::hash<unsigned>{}(param.id));
    hashBool(seed, param.Attrs.noalias);
    hashBool(seed, param.Attrs.nocapture);
    hashBool(seed, param.Attrs.nonnull);
}

/// @brief Add instruction metadata to a semantic fingerprint.
/// @details Includes opcode, result temp, operands, branch labels and arguments,
///          direct/indirect call signature data, and instruction-local effect
///          attributes.  Source locations are deliberately excluded.
/// @param seed Accumulator updated in place.
/// @param instr Instruction to fold into @p seed.
void hashInstr(std::size_t &seed, const core::Instr &instr) {
    hashCombine(seed, std::hash<bool>{}(instr.result.has_value()));
    if (instr.result)
        hashCombine(seed, std::hash<unsigned>{}(*instr.result));
    hashCombine(seed, static_cast<std::size_t>(instr.op));
    hashType(seed, instr.type);
    for (const auto &operand : instr.operands)
        hashCombine(seed, core::valueHash(operand));
    hashCombine(seed, std::hash<std::string>{}(instr.callee));
    for (const auto &label : instr.labels)
        hashCombine(seed, std::hash<std::string>{}(label));
    for (const auto &argList : instr.brArgs) {
        hashCombine(seed, argList.size());
        for (const auto &arg : argList)
            hashCombine(seed, core::valueHash(arg));
    }
    hashBool(seed, instr.CallAttr.nothrow);
    hashBool(seed, instr.CallAttr.readonly);
    hashBool(seed, instr.CallAttr.pure);
    hashBool(seed, instr.hasIndirectSignature);
    hashType(seed, instr.indirectRetType);
    for (const auto &type : instr.indirectParamTypes)
        hashType(seed, type);
    hashBool(seed, instr.indirectIsVarArg);
}

/// @brief Produce a semantic fingerprint for a module.
/// @details Used only to detect whether legacy void-returning module passes were
///          no-ops. Source locations are intentionally ignored because these
///          passes should not be classified as changing IR semantics by metadata
///          churn alone.
/// @param module Module whose semantic IR state is fingerprinted.
/// @return Hash value suitable for before/after equality comparisons.
std::size_t moduleFingerprint(const core::Module &module) {
    std::size_t seed = 0;
    hashCombine(seed, std::hash<std::string>{}(module.version));
    hashBool(seed, module.target.has_value());
    if (module.target)
        hashCombine(seed, std::hash<std::string>{}(*module.target));

    for (const auto &ext : module.externs) {
        hashCombine(seed, std::hash<std::string>{}(ext.name));
        hashType(seed, ext.retType);
        for (const auto &type : ext.params)
            hashType(seed, type);
        hashEffectAttrs(seed, ext.attrs());
    }
    for (const auto &global : module.globals) {
        hashCombine(seed, std::hash<std::string>{}(global.name));
        hashType(seed, global.type);
        hashCombine(seed, std::hash<std::string>{}(global.init));
        hashCombine(seed, static_cast<std::size_t>(global.linkage));
        hashBool(seed, global.isConst);
        hashBool(seed, global.hasInitializer);
    }
    for (const auto &fn : module.functions) {
        hashCombine(seed, std::hash<std::string>{}(fn.name));
        hashType(seed, fn.retType);
        hashBool(seed, fn.isVarArg);
        hashCombine(seed, static_cast<std::size_t>(fn.linkage));
        hashEffectAttrs(seed, fn.attrs());
        for (const auto &param : fn.params)
            hashParam(seed, param);
        for (const auto &name : fn.valueNames)
            hashCombine(seed, std::hash<std::string>{}(name));
        for (const auto &block : fn.blocks) {
            hashCombine(seed, std::hash<std::string>{}(block.label));
            hashBool(seed, block.terminated);
            for (const auto &param : block.params)
                hashParam(seed, param);
            for (const auto &instr : block.instructions)
                hashInstr(seed, instr);
        }
    }
    return seed;
}

/// @brief Run a void-returning module pass and infer whether it changed IR.
/// @details Several legacy passes mutate in place but do not report a change
///          bit.  This wrapper fingerprints the module before and after the
///          pass so the pass manager can preserve analyses for true no-ops.
/// @param module Module transformed by @p pass.
/// @param pass Callable taking @p module by mutable reference.
/// @return Conservative preservation summary based on observed fingerprint change.
template <typename Fn> PreservedAnalyses runVoidModulePass(core::Module &module, Fn &&pass) {
    const std::size_t before = moduleFingerprint(module);
    pass(module);
    if (moduleFingerprint(module) == before)
        return PreservedAnalyses::all();
    return PreservedAnalyses::none();
}

} // namespace

/// @brief Describe a summary where every registered analysis remains valid.
/// @details Returns an instance that marks both module and function analyses as
///          fully preserved, allowing the pipeline executor to skip invalidation
///          entirely because no cached data has to be recomputed.
/// @return Preservation summary retaining all analyses.
PreservedAnalyses PreservedAnalyses::all() {
    PreservedAnalyses p;
    p.preserveAllModules_ = true;
    p.preserveAllFunctions_ = true;
    return p;
}

/// @brief Produce a summary indicating that no analyses remain valid.
/// @details Leaves the module/function preservation flags unset and the
///          preserved sets empty so the executor will purge all cached analyses
///          on the next invalidation pass.
/// @return Preservation summary invalidating every analysis.
PreservedAnalyses PreservedAnalyses::none() {
    return PreservedAnalyses{};
}

/// @brief Record that a specific module analysis was preserved by a pass.
/// @details Inserts the identifier into the preserved set so the invalidator can
///          recognise that the cached result remains valid after the pass
///          finishes executing.
/// @param id Identifier of the module analysis to keep.
/// @return Reference to @c *this for fluent chaining.
PreservedAnalyses &PreservedAnalyses::preserveModule(const std::string &id) {
    moduleAnalyses_.insert(id);
    return *this;
}

/// @brief Record that a specific function analysis was preserved by a pass.
/// @details Mirrors @ref preserveModule by marking a function analysis as
///          retained, enabling selective invalidation when only certain analyses
///          become stale.
/// @param id Identifier of the function analysis to keep.
/// @return Reference to @c *this for fluent chaining.
PreservedAnalyses &PreservedAnalyses::preserveFunction(const std::string &id) {
    functionAnalyses_.insert(id);
    return *this;
}

/// @brief Mark every registered module analysis as preserved.
/// @details Sets the fast-path flag that prevents the invalidator from scanning
///          individual module analysis entries, providing an efficient escape
///          hatch for passes that leave the entire module analysis cache intact.
/// @return Reference to @c *this for fluent chaining.
PreservedAnalyses &PreservedAnalyses::preserveAllModules() {
    preserveAllModules_ = true;
    return *this;
}

/// @brief Mark every registered function analysis as preserved.
/// @details Enables a shortcut similar to @ref preserveAllModules so the
///          invalidator can skip per-analysis checks for function-scoped
///          results.
/// @return Reference to @c *this for fluent chaining.
PreservedAnalyses &PreservedAnalyses::preserveAllFunctions() {
    preserveAllFunctions_ = true;
    return *this;
}

/// @brief Check whether all module analyses were preserved.
/// @details Reports whether the fast-path flag set by
///          @ref preserveAllModules() is active, allowing callers to avoid set
///          lookups when the entire cache remains valid.
/// @return @c true when @ref PreservedAnalyses::preserveAllModules was invoked.
bool PreservedAnalyses::preservesAllModuleAnalyses() const {
    return preserveAllModules_;
}

/// @brief Check whether all function analyses were preserved.
/// @details Reports whether the fast-path flag set by
///          @ref preserveAllFunctions() is active.
/// @return @c true when @ref PreservedAnalyses::preserveAllFunctions was invoked.
bool PreservedAnalyses::preservesAllFunctionAnalyses() const {
    return preserveAllFunctions_;
}

/// @copydoc PreservedAnalyses::preservesAllAnalyses()
bool PreservedAnalyses::preservesAllAnalyses() const {
    return preserveAllModules_ && preserveAllFunctions_ && changedFunctions_.empty();
}

/// @brief Determine whether a specific module analysis is preserved.
/// @details Checks the fast-path flag and falls back to the preserved identifier
///          set, enabling selective retention of cached results.
/// @param id Analysis identifier to query.
/// @return @c true when the analysis was explicitly preserved or the summary
///         preserves all module analyses.
bool PreservedAnalyses::isModulePreserved(const std::string &id) const {
    return preserveAllModules_ || moduleAnalyses_.contains(id);
}

/// @brief Determine whether a specific function analysis is preserved.
/// @details Mirrors @ref isModulePreserved by consulting the function
///          preservation data.
/// @param id Analysis identifier to query.
/// @return @c true when the analysis was explicitly preserved or the summary
///         preserves all function analyses.
bool PreservedAnalyses::isFunctionPreserved(const std::string &id) const {
    return preserveAllFunctions_ || functionAnalyses_.contains(id);
}

/// @brief Check whether any module analyses were explicitly preserved.
/// @details Allows callers to distinguish between "preserve everything" and
///          "preserve only these identifiers" cases when invalidating caches.
/// @return @c true when the module preservation set is non-empty.
bool PreservedAnalyses::hasModulePreservations() const {
    return !moduleAnalyses_.empty();
}

/// @brief Check whether any function analyses were explicitly preserved.
/// @details Companion to @ref hasModulePreservations for the function-level
///          cache.
/// @return @c true when the function preservation set is non-empty.
bool PreservedAnalyses::hasFunctionPreservations() const {
    return !functionAnalyses_.empty();
}

/// @copydoc PreservedAnalyses::preserveCFG()
PreservedAnalyses &PreservedAnalyses::preserveCFG() {
    return preserveFunction(kAnalysisCFG);
}

/// @copydoc PreservedAnalyses::preserveDominators()
PreservedAnalyses &PreservedAnalyses::preserveDominators() {
    return preserveFunction(kAnalysisDominators);
}

/// @copydoc PreservedAnalyses::preserveLoopInfo()
PreservedAnalyses &PreservedAnalyses::preserveLoopInfo() {
    return preserveFunction(kAnalysisLoopInfo);
}

/// @copydoc PreservedAnalyses::preserveLiveness()
PreservedAnalyses &PreservedAnalyses::preserveLiveness() {
    return preserveFunction(kAnalysisLiveness);
}

/// @copydoc PreservedAnalyses::preserveBasicAA()
PreservedAnalyses &PreservedAnalyses::preserveBasicAA() {
    return preserveFunction(kAnalysisBasicAA);
}

/// @copydoc PreservedAnalyses::markChangedFunction()
PreservedAnalyses &PreservedAnalyses::markChangedFunction(const std::string &name) {
    changedFunctions_.insert(name);
    return *this;
}

/// @copydoc PreservedAnalyses::hasChangedFunctions()
bool PreservedAnalyses::hasChangedFunctions() const {
    return !changedFunctions_.empty();
}

/// @copydoc PreservedAnalyses::isChangedFunction()
bool PreservedAnalyses::isChangedFunction(const std::string &name) const {
    return changedFunctions_.contains(name);
}

namespace {
/// @brief Polymorphic module pass backed by a registered callback.
class LambdaModulePass : public ModulePass {
  public:
    /// @brief Wrap a module-pass callback with the @ref ModulePass interface.
    /// @details Stores the identifier for reporting and the callback used to
    ///          implement the pass.  Instances are created on demand by the
    ///          registry when a pass is requested so pipelines can remain
    ///          decoupled from concrete types.
    /// @param id Identifier supplied during registration.
    /// @param cb Callback invoked when the pass executes.
    LambdaModulePass(std::string id, PassRegistry::ModulePassCallback cb)
        : id_(std::move(id)), callback_(std::move(cb)) {}

    /// @brief Expose the identifier under which the pass was registered.
    /// @details Allows the executor to report which pass is currently running
    ///          during diagnostics or verification steps.
    /// @return String view referencing the stored identifier.
    std::string_view id() const override {
        return id_;
    }

    /// @brief Execute the wrapped module pass callback.
    /// @details Simply forwards to the stored callback and returns the
    ///          preservation summary so the executor can invalidate analyses.
    /// @param module Module being transformed.
    /// @param analysis Analysis manager providing cached queries.
    /// @return Preservation summary reported by the callback.
    PreservedAnalyses run(core::Module &module, AnalysisManager &analysis) override {
        return callback_(module, analysis);
    }

  private:
    /// @brief Stable identifier reported to the pipeline executor.
    std::string id_;
    /// @brief Owned transformation callback invoked by run().
    PassRegistry::ModulePassCallback callback_;
};

/// @brief Polymorphic function pass backed by a registered callback.
class LambdaFunctionPass : public FunctionPass {
  public:
    /// @brief Wrap a function-pass callback with the @ref FunctionPass interface.
    /// @details Stores the identifier and callback so lookups yield full
    ///          @ref FunctionPass instances without exposing concrete pass types.
    /// @param id Identifier supplied during registration.
    /// @param cb Callback invoked when the pass executes.
    LambdaFunctionPass(std::string id, PassRegistry::FunctionPassCallback cb)
        : id_(std::move(id)), callback_(std::move(cb)) {}

    /// @brief Expose the identifier under which the pass was registered.
    /// @details Mirrors the module variant for diagnostic reporting.
    /// @return String view referencing the stored identifier.
    std::string_view id() const override {
        return id_;
    }

    /// @brief Execute the wrapped function pass callback.
    /// @details Forwards to the stored callback and returns the resulting
    ///          preservation summary for downstream invalidation.
    /// @param function Function being transformed.
    /// @param analysis Analysis manager providing cached queries.
    /// @return Preservation summary reported by the callback.
    PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) override {
        return callback_(function, analysis);
    }

  private:
    /// @brief Stable identifier reported to the pipeline executor.
    std::string id_;
    /// @brief Owned transformation callback invoked by run().
    PassRegistry::FunctionPassCallback callback_;
};
} // namespace

/// @brief Register a module pass factory under a stable identifier.
/// @details Stores the factory inside the registry so future lookups can
///          synthesize fresh pass instances on demand.  Ownership of the factory
///          is transferred to the registry, ensuring it remains valid for the
///          program lifetime.
/// @param id Identifier used to reference the pass from pipelines.
/// @param factory Callable producing unique @ref ModulePass instances.
/// @param parallelSafe Whether execution is audited for parallel mode.
void PassRegistry::registerModulePass(const std::string &id,
                                      ModulePassFactory factory,
                                      bool parallelSafe) {
    registry_[id] =
        detail::PassFactory{detail::PassKind::Module, std::move(factory), {}, parallelSafe};
}

/// @brief Register a module pass implemented via a simple callback.
/// @details Wraps the callback in a lambda-backed @ref ModulePass so the
///          registry can supply polymorphic instances to the executor while
///          keeping registration sites terse.
/// @param id Identifier used to reference the pass from pipelines.
/// @param callback Callback implementing the pass behaviour.
/// @param parallelSafe Whether execution is audited for parallel mode.
void PassRegistry::registerModulePass(const std::string &id,
                                      ModulePassCallback callback,
                                      bool parallelSafe) {
    auto cb = ModulePassCallback(callback);
    /// @brief Construct a fresh polymorphic wrapper while retaining the registered id.
    /// @return Newly owned module-pass wrapper.
    registry_[id] = detail::PassFactory{
        detail::PassKind::Module,
        [passId = std::string(id), cb]() { return std::make_unique<LambdaModulePass>(passId, cb); },
        {},
        parallelSafe};
}

/// @brief Register a void callback as a module pass.
/// @details Convenience overload that upgrades a basic callback into a pass
///          returning @ref PreservedAnalyses::none(), allowing quick-and-dirty
///          passes to participate in the framework.
/// @param id Identifier used to reference the pass from pipelines.
/// @param fn Callback executed when the pass runs.
/// @param parallelSafe Whether execution is audited for parallel mode.
void PassRegistry::registerModulePass(const std::string &id,
                                      const std::function<void(core::Module &)> &fn,
                                      bool parallelSafe) {
    registerModulePass(
        id,
        /// @brief Adapt a void callback to the preservation-reporting interface.
        /// @param module Module forwarded to the registered callback.
        /// @return Summary invalidating all analyses after callback execution.
        [fn](core::Module &module, AnalysisManager &) {
            fn(module);
            return PreservedAnalyses::none();
        },
        parallelSafe);
}

/// @brief Register a function pass factory under a stable identifier.
/// @details Transfers ownership of the factory callable so the registry can
///          instantiate fresh function passes whenever a pipeline references the
///          identifier.
/// @param id Identifier used to reference the pass from pipelines.
/// @param factory Callable producing unique @ref FunctionPass instances.
/// @param parallelSafe Permit concurrent invocations across functions.
void PassRegistry::registerFunctionPass(const std::string &id,
                                        FunctionPassFactory factory,
                                        bool parallelSafe) {
    registry_[id] =
        detail::PassFactory{detail::PassKind::Function, {}, std::move(factory), parallelSafe};
}

/// @brief Register a function pass implemented via a simple callback.
/// @details Wraps the callback in a lambda-backed @ref FunctionPass similar to
///          the module overload so pipelines can work with opaque polymorphic
///          objects.
/// @param id Identifier used to reference the pass from pipelines.
/// @param callback Callback implementing the pass behaviour.
/// @param parallelSafe Permit concurrent invocations across functions.
void PassRegistry::registerFunctionPass(const std::string &id,
                                        FunctionPassCallback callback,
                                        bool parallelSafe) {
    auto cb = FunctionPassCallback(callback);
    /// @brief Construct a fresh polymorphic wrapper while retaining the registered id.
    /// @return Newly owned function-pass wrapper.
    registry_[id] = detail::PassFactory{detail::PassKind::Function,
                                        {},
                                        [passId = std::string(id), cb]() {
                                            return std::make_unique<LambdaFunctionPass>(passId, cb);
                                        },
                                        parallelSafe};
}

/// @brief Register a void callback as a function pass.
/// @details Wraps the callback in a preserving adaptor returning
///          @ref PreservedAnalyses::none() so simple lambdas can participate in
///          the pipeline infrastructure.
/// @param id Identifier used to reference the pass from pipelines.
/// @param fn Callback executed when the pass runs.
/// @param parallelSafe Permit concurrent invocations across functions.
void PassRegistry::registerFunctionPass(const std::string &id,
                                        const std::function<void(core::Function &)> &fn,
                                        bool parallelSafe) {
    registerFunctionPass(
        id,
        /// @brief Adapt a void callback to the preservation-reporting interface.
        /// @param function Function forwarded to the registered callback.
        /// @return Summary invalidating all analyses after callback execution.
        [fn](core::Function &function, AnalysisManager &) {
            fn(function);
            return PreservedAnalyses::none();
        },
        parallelSafe);
}

/// @brief Retrieve the factory metadata associated with an identifier.
/// @details Performs a lookup inside the registry and returns a pointer to the
///          stored factory record when found so executors can instantiate the
///          pass.
/// @param id Identifier previously supplied to a registration call.
/// @return Pointer to the stored factory or @c nullptr when the identifier is
///         unknown.
const detail::PassFactory *PassRegistry::lookup(std::string_view id) const {
    auto it = registry_.find(std::string(id));
    if (it == registry_.end())
        return nullptr;
    return &it->second;
}

/// @copydoc registerLoopSimplifyPass()
void registerLoopSimplifyPass(PassRegistry &registry) {
    // Sequential: recomputes whole-module CFG/loop info while mutating block edges.
    /// @brief Construct a stateless loop simplifier for sequential execution.
    /// @return Newly owned `LoopSimplify` pass.
    registry.registerFunctionPass(
        "loop-simplify", []() { return std::make_unique<LoopSimplify>(); }, false);
}

/// @copydoc registerLICMPass()
void registerLICMPass(PassRegistry &registry) {
    // Sequential: analysis dependencies scan the whole module while this pass moves instructions.
    /// @brief Construct LICM with memory hoisting enabled.
    /// @return Newly owned default `LICM` pass.
    registry.registerFunctionPass("licm", []() { return std::make_unique<LICM>(); }, false);
}

/// @copydoc registerLICMSafePass()
void registerLICMSafePass(PassRegistry &registry) {
    // Sequential for the same whole-module analysis reason as the default LICM pass.
    /// @brief Construct LICM with all memory hoisting disabled.
    /// @return Newly owned computation-only `LICM` pass.
    registry.registerFunctionPass(
        "licm-safe", []() { return std::make_unique<LICM>(false); }, false);
}

/// @copydoc registerSCCPPass()
void registerSCCPPass(PassRegistry &registry) {
    /// @brief Run SCCP per function and report exactly which caches became stale.
    /// @param module Module whose functions are optimized.
    /// @return Selective preservation summary for changed functions.
    registry.registerModulePass("sccp", [](core::Module &module, AnalysisManager &) {
        PreservedAnalyses preserved;
        const std::size_t functionCount = module.functions.size();
        std::vector<unsigned char> changedFunctions(functionCount, 0);
        for (std::size_t i = 0; i < functionCount; ++i)
            changedFunctions[i] = sccp(module.functions[i]) ? 1 : 0;

        bool changed = false;
        for (std::size_t i = 0; i < functionCount; ++i) {
            if (!changedFunctions[i])
                continue;
            changed = true;
            preserved.markChangedFunction(module.functions[i].name);
        }
        if (!changed)
            return PreservedAnalyses::all();
        preserved.preserveAllModules();
        return preserved;
    });
}

/// @copydoc registerConstFoldPass()
void registerConstFoldPass(PassRegistry &registry) {
    /// @brief Adapt legacy module constant folding through semantic change detection.
    /// @param module Module to optimize.
    /// @return Preservation inferred from the before/after semantic fingerprint.
    registry.registerModulePass("constfold", [](core::Module &module, AnalysisManager &) {
        /// @brief Invoke the legacy void-returning constant folder.
        /// @param m Module forwarded by the semantic-change adapter.
        return runVoidModulePass(module, [](core::Module &m) { constFold(m); });
    });
}

/// @copydoc registerPeepholePass()
void registerPeepholePass(PassRegistry &registry) {
    /// @brief Adapt legacy peephole optimization through semantic change detection.
    /// @param module Module to optimize.
    /// @return Preservation inferred from the before/after semantic fingerprint.
    registry.registerModulePass("peephole", [](core::Module &module, AnalysisManager &) {
        /// @brief Invoke the legacy void-returning peephole pass.
        /// @param m Module forwarded by the semantic-change adapter.
        return runVoidModulePass(module, [](core::Module &m) { peephole(m); });
    });
}

/// @copydoc registerDCEPass()
void registerDCEPass(PassRegistry &registry) {
    /// @brief Adapt legacy module DCE through semantic change detection.
    /// @param module Module to optimize.
    /// @return Preservation inferred from the before/after semantic fingerprint.
    registry.registerModulePass("dce", [](core::Module &module, AnalysisManager &) {
        /// @brief Invoke the legacy void-returning DCE pass.
        /// @param m Module forwarded by the semantic-change adapter.
        return runVoidModulePass(module, [](core::Module &m) { dce(m); });
    });
}

/// @copydoc registerMem2RegPass()
void registerMem2RegPass(PassRegistry &registry) {
    /// @brief Adapt deterministic module mem2reg through semantic change detection.
    /// @param module Module to optimize.
    /// @return Preservation inferred from the before/after semantic fingerprint.
    registry.registerModulePass("mem2reg", [](core::Module &module, AnalysisManager &) {
        /// @brief Invoke mem2reg without statistics or internal parallelism.
        /// @param m Module forwarded by the semantic-change adapter.
        return runVoidModulePass(module,
                                 [](core::Module &m) { zanna::passes::mem2reg(m, nullptr); });
    });
}

/// @copydoc registerDSEPass()
void registerDSEPass(PassRegistry &registry) {
    /// @brief Combine local and MemorySSA DSE while preserving structural analyses.
    /// @param fn Function to optimize.
    /// @param am Analysis manager providing and invalidating MemorySSA.
    /// @return Preservation summary reflecting whether instructions were erased.
    registry.registerFunctionPass(
        "dse",
        [](core::Function &fn, AnalysisManager &am) {
            bool changed = runDSE(fn, am);
            if (changed)
                am.invalidateFunctionResult(kAnalysisMemorySSA, fn);
            // MemorySSA-based cross-block DSE: catches stores that
            // runDSE's conservative call-barrier logic would miss for
            // non-escaping allocas.
            changed |= runMemorySSADSE(fn, am);
            if (!changed)
                return PreservedAnalyses::all();
            PreservedAnalyses p;
            p.preserveAllModules();
            p.preserveCFG();
            p.preserveDominators();
            p.preserveLoopInfo();
            return p;
        },
        true);
}

/// @copydoc registerEarlyCSEPass()
void registerEarlyCSEPass(PassRegistry &registry) {
    // Each invocation indexes and mutates only its assigned function. Identifier
    // sidecars are synchronized by PipelineExecutor before parallel dispatch.
    /// @brief Run EarlyCSE on one assigned function and report structural preservation.
    /// @param fn Function to optimize.
    /// @param am Analysis manager owning the containing module.
    /// @return Preservation summary reflecting whether instructions were changed.
    registry.registerFunctionPass(
        "earlycse",
        [](core::Function &fn, AnalysisManager &am) {
            bool changed = runEarlyCSE(am.module(), fn);
            if (!changed)
                return PreservedAnalyses::all();
            PreservedAnalyses p;
            p.preserveAllModules();
            p.preserveCFG();
            p.preserveDominators();
            p.preserveLoopInfo();
            return p;
        },
        true);
}

/// @copydoc registerReassociatePass()
void registerReassociatePass(PassRegistry &registry) {
    /// @brief Adapt legacy reassociation through semantic change detection.
    /// @param module Module to optimize.
    /// @return Preservation inferred from the before/after semantic fingerprint.
    registry.registerModulePass("reassociate", [](core::Module &module, AnalysisManager &) {
        /// @brief Invoke the legacy void-returning reassociation pass.
        /// @param m Module forwarded by the semantic-change adapter.
        return runVoidModulePass(module, [](core::Module &m) { reassociate(m); });
    });
}

/// @copydoc registerEHOptPass()
void registerEHOptPass(PassRegistry &registry) {
    /// @brief Adapt legacy exception cleanup through semantic change detection.
    /// @param module Module to optimize.
    /// @return Preservation inferred from the before/after semantic fingerprint.
    registry.registerModulePass("eh-opt", [](core::Module &module, AnalysisManager &) {
        /// @brief Invoke the legacy void-returning exception optimization pass.
        /// @param m Module forwarded by the semantic-change adapter.
        return runVoidModulePass(module, [](core::Module &m) { ehOpt(m); });
    });
}

/// @copydoc registerLoopRotatePass()
void registerLoopRotatePass(PassRegistry &registry) {
    // Sequential: rewrites loop edges and recomputes whole-module loop info.
    /// @brief Construct a stateless loop-rotation pass for sequential execution.
    /// @return Newly owned `LoopRotate` pass.
    registry.registerFunctionPass(
        "loop-rotate", []() { return std::make_unique<LoopRotate>(); }, false);
}

} // namespace il::transform
