//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the bookkeeping utilities responsible for caching IL analyses and
// invalidating them after transform passes execute.  AnalysisManager maintains
// per-module and per-function caches keyed by the identifiers registered in the
// pass registry.  This file centralises the invalidation logic so preservation
// summaries only need to describe which analyses remain valid.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements analysis cache invalidation and manager construction.
/// @details Preservation summaries are translated into precise module and
///          per-function cache erasure while retaining still-live results.

#include "il/transform/AnalysisManager.hpp"

#include "il/core/Function.hpp"
#include "il/core/Module.hpp"
#include "il/transform/PassRegistry.hpp"

#include <cassert>
#include <unordered_set>
#include <utility>

namespace il::transform {
class AnalysisCacheInvalidator {
  public:
    /// @brief Prepare an invalidator for the given manager/preservation pair.
    /// @details Stores references to the owning AnalysisManager and the
    ///          preservation summary returned by a pass.  Subsequent calls use
    ///          these references to decide which cached analyses to evict.
    /// @param manager Manager whose caches will be pruned.
    /// @param preserved Summary describing which analyses remain valid.
    AnalysisCacheInvalidator(AnalysisManager &manager, const PreservedAnalyses &preserved)
        : manager_(manager), preserved_(preserved) {}

    /// @brief Invalidate module-scoped analyses according to preservation data.
    /// @details Skips work when no module analyses were registered or when the
    ///          summary preserved everything.  Otherwise the routine either
    ///          clears the entire cache or erases only those entries not listed in
    ///          the summary, balancing correctness with minimal churn.
    void afterModulePass() {
        assertWellFormed();
        invalidateModuleCache();
        invalidateFunctionCacheForModulePass();
    }

    /// @brief Invalidate function-scoped analyses for a specific function.
    /// @details Mirrors @ref AnalysisCacheInvalidator::afterModulePass() but operates on the
    ///          per-function caches.  When only a subset of analyses are preserved, the routine
    ///          walks each cache entry and removes the stale data for @p fn.  Empty analysis maps
    ///          are pruned to keep the cache compact.
    /// @param fn Function whose cached analyses should be reviewed.
    void afterFunctionPass(core::Function &fn) {
        assertWellFormed();
        if (!manager_.functionAnalyses_)
            return;
        if (manager_.functionCache_.empty())
            return;
        if (preserved_.preservesAllFunctionAnalyses())
            return;
        if (!preserved_.hasFunctionPreservations()) {
            for (auto it = manager_.functionCache_.begin(); it != manager_.functionCache_.end();) {
                it->second.erase(&fn);
                if (it->second.empty())
                    it = manager_.functionCache_.erase(it);
                else
                    ++it;
            }
#ifndef NDEBUG
            for (const auto &entry : manager_.functionCache_)
                assert(entry.second.count(&fn) == 0 && "stale function analysis entry");
#endif
            return;
        }

        for (auto it = manager_.functionCache_.begin(); it != manager_.functionCache_.end();) {
            if (preserved_.isFunctionPreserved(it->first)) {
                ++it;
                continue;
            }
            it->second.erase(&fn);
            if (it->second.empty())
                it = manager_.functionCache_.erase(it);
            else
                ++it;
        }
    }

  private:
    /// @brief Evict module-level cached analyses that were not preserved.
    /// @details Short-circuits when no module analyses are registered, when the
    ///          pass preserves everything, or when nothing is preserved (full
    ///          clear).  Otherwise removes only the stale entries.
    void invalidateModuleCache() {
        if (!manager_.moduleAnalyses_)
            return;
        if (manager_.moduleCache_.empty())
            return;
        if (preserved_.preservesAllModuleAnalyses())
            return;
        if (!preserved_.hasModulePreservations()) {
            manager_.moduleCache_.clear();
            return;
        }

        for (auto it = manager_.moduleCache_.begin(); it != manager_.moduleCache_.end();) {
            if (preserved_.isModulePreserved(it->first)) {
                ++it;
                continue;
            }
            it = manager_.moduleCache_.erase(it);
        }
    }

    /// @brief Evict per-function cached analyses that were not preserved by a module pass.
    /// @details A module pass potentially affects all functions, so the entire
    ///          function cache is cleared when nothing is preserved.  If some
    ///          analyses are preserved, only the stale per-analysis entries are removed.
    void invalidateFunctionCacheForModulePass() {
        if (!manager_.functionAnalyses_)
            return;
        if (manager_.functionCache_.empty())
            return;
        if (preserved_.preservesAllFunctionAnalyses())
            return;
        std::unordered_set<const core::Function *> liveFunctions;
        liveFunctions.reserve(manager_.module_.functions.size());
        std::unordered_set<const core::Function *> changedFunctions;
        changedFunctions.reserve(manager_.module_.functions.size());
        for (const auto &fn : manager_.module_.functions) {
            liveFunctions.insert(&fn);
            if (preserved_.isChangedFunction(fn.name))
                changedFunctions.insert(&fn);
        }

        /// Erase dead or explicitly changed functions from one analysis cache.
        auto eraseChangedFunctions =
            [&](std::unordered_map<const core::Function *, detail::AnalysisValue>
                    &cacheForAnalysis) {
                for (auto it = cacheForAnalysis.begin(); it != cacheForAnalysis.end();) {
                    if (!it->first || !liveFunctions.contains(it->first) ||
                        changedFunctions.contains(it->first))
                        it = cacheForAnalysis.erase(it);
                    else
                        ++it;
                }
            };
        if (!preserved_.hasFunctionPreservations() && !preserved_.hasChangedFunctions()) {
            manager_.functionCache_.clear();
            return;
        }

        for (auto it = manager_.functionCache_.begin(); it != manager_.functionCache_.end();) {
            if (preserved_.isFunctionPreserved(it->first)) {
                ++it;
                continue;
            }
            if (preserved_.hasChangedFunctions()) {
                eraseChangedFunctions(it->second);
                if (it->second.empty())
                    it = manager_.functionCache_.erase(it);
                else
                    ++it;
                continue;
            }
            it = manager_.functionCache_.erase(it);
        }
    }

    /// @brief Assert that every cached analysis has a corresponding registration entry.
    /// @details Debug-mode guard only; verifies the module and function caches are
    ///          consistent with the registry so stale or orphaned cache entries are
    ///          caught early.
    void assertWellFormed() const {
#ifndef NDEBUG
        if (manager_.moduleAnalyses_) {
            for (const auto &entry : manager_.moduleCache_) {
                assert(manager_.moduleAnalyses_->count(entry.first) &&
                       "module cache entry without registration");
            }
        }
        if (manager_.functionAnalyses_) {
            for (const auto &entry : manager_.functionCache_) {
                assert(manager_.functionAnalyses_->count(entry.first) &&
                       "function cache entry without registration");
            }
        }
#endif
    }

    AnalysisManager &manager_;
    const PreservedAnalyses &preserved_;
};

/// @brief Construct an analysis manager tied to a module and registry.
/// @details Captures the module reference and caches pointers to the module and
///          function analysis registries so that lookups avoid repeated
///          indirection during pipeline execution.
/// @param module Module undergoing transformation.
/// @param registry Registry describing the available analyses.
AnalysisManager::AnalysisManager(core::Module &module, const AnalysisRegistry &registry)
    : module_(module) {
    moduleAnalyses_ = &registry.moduleAnalyses();
    functionAnalyses_ = &registry.functionAnalyses();
}

/// @brief Apply invalidation logic after a module pass has completed.
/// @details Wraps the helper class so callers can simply forward the
///          preservation summary returned by the pass.
/// @param preserved Summary of analyses that remain valid.
void AnalysisManager::invalidateAfterModulePass(const PreservedAnalyses &preserved) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    AnalysisCacheInvalidator(*this, preserved).afterModulePass();
}

/// @brief Apply invalidation logic after a function pass has completed.
/// @details Delegates to the helper so preservation summaries remain the sole
///          source of truth for invalidation behaviour.
/// @param preserved Summary of analyses that remain valid.
/// @param fn Function whose cached analyses should be pruned.
void AnalysisManager::invalidateAfterFunctionPass(const PreservedAnalyses &preserved,
                                                  core::Function &fn) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    AnalysisCacheInvalidator(*this, preserved).afterFunctionPass(fn);
}

/// @brief Drop a single cached function analysis for one function.
/// @details Used by multi-phase passes that intentionally mutate the function
///          between phases and then need one analysis recomputed without
///          perturbing the rest of the cache.
/// @param id Function-analysis identifier to remove.
/// @param fn Function whose cached entry is invalidated.
void AnalysisManager::invalidateFunctionResult(const std::string &id, core::Function &fn) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto cacheIt = functionCache_.find(id);
    if (cacheIt == functionCache_.end())
        return;
    cacheIt->second.erase(&fn);
    if (cacheIt->second.empty())
        functionCache_.erase(cacheIt);
}

} // namespace il::transform
