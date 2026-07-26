//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/PassRegistry.hpp
// Purpose: Pass registration infrastructure and preservation tracking for
//          the IL transformation pipeline. Declares PreservedAnalyses (tracks
//          which analyses remain valid after a pass), ModulePass/FunctionPass
//          base classes, and PassRegistry (maps pass names to factories).
// Key invariants:
//   - Pass identifiers must be unique within the registry.
//   - PreservedAnalyses tracks module and function analyses independently.
//   - Factory functions transfer ownership of pass objects to the caller.
// Ownership/Lifetime: PassRegistry owns its factory map. PreservedAnalyses is
//          a lightweight value type. ModulePass/FunctionPass are polymorphic
//          bases owned via unique_ptr by the pipeline executor.
// Links: il/core/fwd.hpp, il/transform/AnalysisManager.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares IL pass abstractions, preservation summaries, and registration.
 *
 * @details Transformation passes report module- and function-analysis validity
 *          through `PreservedAnalyses`. `PassRegistry` type-erases factories or
 *          callbacks behind stable identifiers, records execution scope and
 *          parallel-safety metadata, and transfers newly constructed pass
 *          ownership to the pipeline executor.
 */

#pragma once

#include "il/core/fwd.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace il::transform {

class AnalysisManager;

/// @brief Tracks which analyses are preserved by a pass execution.
/// @details Used by passes to signal which analysis results remain valid
///          after transformation, enabling the pass manager to avoid
///          unnecessary recomputation. Module and function analyses are tracked
///          separately so module passes can opt-in to preserving per-function
///          results (e.g. CFG, dominators) when they leave those structures
///          untouched.
class PreservedAnalyses {
  public:
    /// @brief Create a PreservedAnalyses indicating all analyses are preserved.
    /// @return PreservedAnalyses object preserving all module and function analyses.
    static PreservedAnalyses all();

    /// @brief Create a PreservedAnalyses indicating no analyses are preserved.
    /// @return PreservedAnalyses object invalidating all analyses.
    static PreservedAnalyses none();

    /// @brief Mark a specific module-level analysis as preserved.
    /// @param id Analysis identifier to preserve.
    /// @return Reference to this object for method chaining.
    PreservedAnalyses &preserveModule(const std::string &id);

    /// @brief Mark a specific function-level analysis as preserved.
    /// @param id Analysis identifier to preserve.
    /// @return Reference to this object for method chaining.
    PreservedAnalyses &preserveFunction(const std::string &id);

    /// @brief Mark all module-level analyses as preserved.
    /// @return Reference to this object for method chaining.
    PreservedAnalyses &preserveAllModules();

    /// @brief Mark all function-level analyses as preserved.
    /// @return Reference to this object for method chaining.
    PreservedAnalyses &preserveAllFunctions();

    /// @brief Check if all module-level analyses are preserved.
    /// @return True if all module analyses remain valid.
    bool preservesAllModuleAnalyses() const;

    /// @brief Check if all function-level analyses are preserved.
    /// @return True if all function analyses remain valid.
    bool preservesAllFunctionAnalyses() const;

    /// @brief Check if both module and function analyses are fully preserved.
    /// @return True when the pass reported no IR mutation requiring invalidation.
    bool preservesAllAnalyses() const;

    /// @brief Check if a specific module analysis is preserved.
    /// @param id Analysis identifier to query.
    /// @return True if the module analysis remains valid.
    bool isModulePreserved(const std::string &id) const;

    /// @brief Check if a specific function analysis is preserved.
    /// @param id Analysis identifier to query.
    /// @return True if the function analysis remains valid.
    bool isFunctionPreserved(const std::string &id) const;

    /// @brief Check if any module analyses are explicitly preserved.
    /// @return True if specific module analyses or all module analyses are preserved.
    bool hasModulePreservations() const;

    /// @brief Check if any function analyses are explicitly preserved.
    /// @return True if specific function analyses or all function analyses are preserved.
    bool hasFunctionPreservations() const;

    /// @brief Convenience helper to preserve the CFG analysis.
    /// @return Reference to this object for method chaining.
    PreservedAnalyses &preserveCFG();

    /// @brief Convenience helper to preserve the dominator tree analysis.
    /// @return Reference to this object for method chaining.
    PreservedAnalyses &preserveDominators();

    /// @brief Convenience helper to preserve loop information analysis.
    /// @return Reference to this object for method chaining.
    PreservedAnalyses &preserveLoopInfo();

    /// @brief Convenience helper to preserve liveness analysis.
    /// @return Reference to this object for method chaining.
    PreservedAnalyses &preserveLiveness();

    /// @brief Convenience helper to preserve the basic alias analysis.
    /// @return Reference to this object for method chaining.
    PreservedAnalyses &preserveBasicAA();

    /// @brief Record that a module pass changed a specific function body.
    /// @details Allows module passes that only rewrite a subset of functions
    ///          (for example the inliner) to invalidate cached function
    ///          analyses selectively instead of clearing all function-analysis
    ///          results in the module.
    /// @param name Function name whose cached analyses must be dropped.
    /// @return Reference to this object for method chaining.
    PreservedAnalyses &markChangedFunction(const std::string &name);

    /// @brief Check whether a module pass reported any changed functions.
    /// @return True when one or more functions were marked changed.
    bool hasChangedFunctions() const;

    /// @brief Check whether a function name was marked changed.
    /// @param name Function name to query.
    /// @return True when the function was explicitly marked changed.
    bool isChangedFunction(const std::string &name) const;

  private:
    /// @brief Whether every module-level analysis remains valid.
    bool preserveAllModules_ = false;
    /// @brief Whether every function-level analysis remains valid.
    bool preserveAllFunctions_ = false;
    /// @brief Individually preserved module-analysis identifiers.
    std::unordered_set<std::string> moduleAnalyses_;
    /// @brief Individually preserved function-analysis identifiers.
    std::unordered_set<std::string> functionAnalyses_;
    /// @brief Functions explicitly changed by a selective module pass.
    std::unordered_set<std::string> changedFunctions_;
};

/// @brief Base class for transformation passes operating on entire modules.
/// @details Module passes can modify any function, global, or extern declaration
///          within the module. They receive the full AnalysisManager for querying
///          cached analysis results.
class ModulePass {
  public:
    /// @brief Destroy a module pass through its polymorphic base.
    virtual ~ModulePass() = default;

    /// @brief Get the unique identifier for this pass.
    /// @return String view representing the pass name.
    virtual std::string_view id() const = 0;

    /// @brief Execute the transformation on the module.
    /// @param module Module to transform.
    /// @param analysis Analysis manager for querying cached results.
    /// @return PreservedAnalyses indicating which analyses remain valid.
    virtual PreservedAnalyses run(core::Module &module, AnalysisManager &analysis) = 0;
};

/// @brief Base class for transformation passes operating on individual functions.
/// @details Function passes transform one function at a time and can query
///          analyses at both function and module scope.
class FunctionPass {
  public:
    /// @brief Destroy a function pass through its polymorphic base.
    virtual ~FunctionPass() = default;

    /// @brief Get the unique identifier for this pass.
    /// @return String view representing the pass name.
    virtual std::string_view id() const = 0;

    /// @brief Execute the transformation on a single function.
    /// @param function Function to transform.
    /// @param analysis Analysis manager for querying cached results.
    /// @return PreservedAnalyses indicating which analyses remain valid.
    virtual PreservedAnalyses run(core::Function &function, AnalysisManager &analysis) = 0;
};

namespace detail {
/// @brief Scope on which a registered pass executes.
/// @details Values distinguish whole-module passes from independently invoked
///          function passes.
enum class PassKind { Module, Function };

/// @brief Type-erased construction metadata stored for one pass id.
struct PassFactory {
    /// @brief Scope selecting which factory member is valid.
    PassKind kind{PassKind::Function};
    /// @brief Constructor for a module pass when @ref kind is module scope.
    std::function<std::unique_ptr<ModulePass>()> makeModule;
    /// @brief Constructor for a function pass when @ref kind is function scope.
    std::function<std::unique_ptr<FunctionPass>()> makeFunction;
    /// @brief Whether execution has been audited for the executor's parallel mode.
    bool parallelSafe = false;
};
} // namespace detail

/// @brief Registry of available transformation passes for the IL optimizer.
/// @details Stores factories and callbacks for module and function passes,
///          enabling dynamic pass lookup and instantiation at pipeline
///          construction time.
class PassRegistry {
  public:
    /// @brief Callable that transfers ownership of a new module pass.
    using ModulePassFactory = std::function<std::unique_ptr<ModulePass>()>;
    /// @brief Callable that transfers ownership of a new function pass.
    using FunctionPassFactory = std::function<std::unique_ptr<FunctionPass>()>;
    /// @brief Analysis-aware module transformation callback.
    using ModulePassCallback = std::function<PreservedAnalyses(core::Module &, AnalysisManager &)>;
    /// @brief Analysis-aware function transformation callback.
    using FunctionPassCallback =
        std::function<PreservedAnalyses(core::Function &, AnalysisManager &)>;

    /// @brief Register a module pass using a factory function.
    /// @param id Unique identifier for the pass.
    /// @param factory Function returning a new ModulePass instance.
    /// @param parallelSafe Whether execution is audited for parallel mode.
    void registerModulePass(const std::string &id,
                            ModulePassFactory factory,
                            bool parallelSafe = false);

    /// @brief Register a module pass using a callback with analysis access.
    /// @param id Unique identifier for the pass.
    /// @param callback Function implementing the pass transformation.
    /// @param parallelSafe Whether execution is audited for parallel mode.
    void registerModulePass(const std::string &id,
                            ModulePassCallback callback,
                            bool parallelSafe = false);

    /// @brief Register a simple module pass without analysis access.
    /// @param id Unique identifier for the pass.
    /// @param fn Function transforming the module (no return value).
    /// @param parallelSafe Whether execution is audited for parallel mode.
    void registerModulePass(const std::string &id,
                            const std::function<void(core::Module &)> &fn,
                            bool parallelSafe = false);

    /// @brief Register a function pass using a factory function.
    /// @param id Unique identifier for the pass.
    /// @param factory Function returning a new FunctionPass instance.
    /// @param parallelSafe Permit concurrent invocations across functions.
    void registerFunctionPass(const std::string &id,
                              FunctionPassFactory factory,
                              bool parallelSafe = false);

    /// @brief Register a function pass using a callback with analysis access.
    /// @param id Unique identifier for the pass.
    /// @param callback Function implementing the pass transformation.
    /// @param parallelSafe Permit concurrent invocations across functions.
    void registerFunctionPass(const std::string &id,
                              FunctionPassCallback callback,
                              bool parallelSafe = false);

    /// @brief Register a simple function pass without analysis access.
    /// @param id Unique identifier for the pass.
    /// @param fn Function transforming a function (no return value).
    /// @param parallelSafe Permit concurrent invocations across functions.
    void registerFunctionPass(const std::string &id,
                              const std::function<void(core::Function &)> &fn,
                              bool parallelSafe = false);

    /// @brief Look up a registered pass by identifier.
    /// @param id Pass identifier to query.
    /// @return Pointer to PassFactory if found, nullptr otherwise.
    const detail::PassFactory *lookup(std::string_view id) const;

  private:
    /// @brief Type-erased pass construction metadata indexed by stable identifier.
    std::unordered_map<std::string, detail::PassFactory> registry_;
};

/// @brief Register the loop simplification pass with the registry.
/// @param registry PassRegistry to register the pass into.
void registerLoopSimplifyPass(PassRegistry &registry);

/// @brief Register the sparse conditional constant propagation pass.
/// @param registry PassRegistry to register the pass into.
void registerSCCPPass(PassRegistry &registry);

/// @brief Register the constant folding pass.
/// @param registry Registry receiving the module callback.
void registerConstFoldPass(PassRegistry &registry);

/// @brief Register the peephole/inst-combine style pass.
/// @param registry Registry receiving the module callback.
void registerPeepholePass(PassRegistry &registry);

/// @brief Register the trivial dead-code elimination pass.
/// @param registry Registry receiving the module callback.
void registerDCEPass(PassRegistry &registry);

/// @brief Register the mem2reg promotion pass.
/// @param registry Registry receiving the module callback.
void registerMem2RegPass(PassRegistry &registry);

/// @brief Register dead-store elimination, including MemorySSA-backed cleanup.
/// @param registry Registry receiving the function callback.
void registerDSEPass(PassRegistry &registry);

/// @brief Register the dominator-tree-scoped EarlyCSE/GVN-lite pass.
/// @param registry Registry receiving the function callback.
void registerEarlyCSEPass(PassRegistry &registry);

/// @brief Register the GVN + redundant load elimination pass.
/// @param registry Registry receiving the pass factory.
void registerGVNPass(PassRegistry &registry);

/// @brief Register the IndVarSimplify pass.
/// @param registry Registry receiving the pass factory.
void registerIndVarSimplifyPass(PassRegistry &registry);

/// @brief Register the loop unrolling pass.
/// @param registry Registry receiving the pass factory.
void registerLoopUnrollPass(PassRegistry &registry);

/// @brief Register the simple function inliner module pass.
/// @param registry Registry receiving the module callback.
void registerInlinePass(PassRegistry &registry);

/// @brief Register the check optimization pass.
/// @param registry Registry receiving the pass factory.
void registerCheckOptPass(PassRegistry &registry);

/// @brief Register the late cleanup pass.
/// @param registry Registry receiving the module callback.
void registerLateCleanupPass(PassRegistry &registry);

/// @brief Register the sibling recursion pass.
/// @param registry Registry receiving the pass factory.
void registerSiblingRecursionPass(PassRegistry &registry);

/// @brief Register the reassociation pass.
/// @param registry Registry receiving the module callback.
void registerReassociatePass(PassRegistry &registry);

/// @brief Register the exception handling optimization pass.
/// @param registry Registry receiving the module callback.
void registerEHOptPass(PassRegistry &registry);

/// @brief Register the loop rotation pass.
/// @param registry Registry receiving the pass factory.
void registerLoopRotatePass(PassRegistry &registry);

} // namespace il::transform
