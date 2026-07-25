//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/AnalysisManager.hpp
// Purpose: Analysis registration, caching, and invalidation infrastructure
//          for the IL transformation pipeline. AnalysisRegistry stores
//          compute functions; AnalysisManager lazily computes, caches, and
//          invalidates analysis results based on PreservedAnalyses metadata.
// Key invariants:
//   - Analyses are computed at most once until explicitly invalidated.
//   - Module and function analyses are tracked and invalidated independently.
//   - Thread-safe: concurrent cache reads via shared_mutex.
// Ownership/Lifetime: AnalysisManager borrows its Module and AnalysisRegistry
//          references; both must outlive the manager. Cached results are owned
//          by the manager through type-erased shared storage.
// Links: il/core/fwd.hpp, il/transform/PassRegistry.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares typed analysis registration, caching, and invalidation.
/// @details Results are type-erased behind shared ownership but guarded by a
///          stable type spelling before retrieval. Cache access is synchronized
///          for concurrent read-heavy pass pipelines.

#pragma once

#include "il/core/fwd.hpp"

#include <cassert>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <utility>

namespace il::transform {

class PreservedAnalyses;
class AnalysisCacheInvalidator;

namespace detail {
/// @brief Type-erased owner for one cached analysis result.
/// @details Analysis results may be registered by one translation unit and
///          consumed by a pass compiled in another. The IL libraries use hidden
///          symbol visibility on macOS, so RTTI identity cannot be used as the
///          cache contract. This wrapper stores the object behind
///          @c shared_ptr<void> and carries a compiler-generated type spelling
///          that can be compared by content before performing a static cast.
struct AnalysisValue {
    std::shared_ptr<void> storage; ///< Heap-owned analysis result object.
    std::string_view typeName{};   ///< Stable compiler spelling of the stored result type.

    /// @brief Check whether this cache slot contains a computed analysis.
    /// @return True when @ref storage owns a result object.
    bool has_value() const {
        return static_cast<bool>(storage);
    }
};

/// @brief Return a stable spelling for an analysis result type.
/// @details Compares RTTI name text rather than RTTI object identity. Hidden
///          symbol visibility can produce distinct @c std::type_info objects
///          for the same type across translation units on macOS, but the
///          mangled type-name text remains a stable guard before the cache casts
///          its type-erased payload back to @p Result.
/// @tparam Result Concrete analysis result type.
/// @return Implementation-defined name text describing @p Result.
template <typename Result> std::string_view analysisTypeName() {
    return typeid(Result).name();
}

/// @brief Build a type-erased analysis cache value.
/// @details Allocates the result on the heap and keeps its original deleter via
///          @c shared_ptr<Result> conversion to @c shared_ptr<void>. The stored
///          type spelling is checked on retrieval before the pointer is cast
///          back to @p Result.
/// @tparam Result Concrete analysis result type.
/// @param result Freshly computed analysis result.
/// @return Type-erased cache value owning @p result.
template <typename Result> AnalysisValue makeAnalysisValue(Result result) {
    AnalysisValue value;
    value.storage = std::make_shared<Result>(std::move(result));
    value.typeName = analysisTypeName<Result>();
    return value;
}

/// @brief Type-erased factory and expected result type for a module analysis.
struct ModuleAnalysisRecord {
    /// Callable that computes an owning cache value.
    std::function<AnalysisValue(core::Module &)> compute;
    /// Stable spelling of the concrete result type.
    std::string_view typeName{};
};

/// @brief Type-erased factory and expected result type for a function analysis.
struct FunctionAnalysisRecord {
    /// Callable that computes an owning cache value for one function.
    std::function<AnalysisValue(core::Module &, core::Function &)> compute;
    /// Stable spelling of the concrete result type.
    std::string_view typeName{};
};

/// @brief Build a diagnostic for an unregistered analysis lookup.
/// @details The analysis manager is usually queried by pass identifiers. Listing
///          the registered names in release builds keeps pipeline mistakes
///          actionable instead of turning them into null dereferences.
/// @tparam Map Analysis registry map type.
/// @param scope Human-readable analysis scope.
/// @param id Requested analysis identifier.
/// @param analyses Registered analyses for @p scope.
/// @return Error message suitable for std::logic_error.
template <typename Map>
std::string unknownAnalysisMessage(const char *scope, const std::string &id, const Map &analyses) {
    std::string message = "unknown ";
    message += scope;
    message += " analysis '";
    message += id;
    message += "'; registered:";
    for (const auto &entry : analyses) {
        message += ' ';
        message += entry.first;
    }
    return message;
}

/// @brief Build a diagnostic for an analysis result type mismatch.
/// @tparam Result Type requested by the caller.
/// @param id Requested analysis identifier.
/// @param actual Type spelling recorded by the registry or cache.
/// @return Error message suitable for std::logic_error.
template <typename Result>
std::string analysisTypeMismatchMessage(const std::string &id, std::string_view actual) {
    std::string message = "analysis result type mismatch for '";
    message += id;
    message += "': requested ";
    message += analysisTypeName<Result>();
    message += ", registered ";
    message += actual;
    return message;
}

/// @brief Validate that a registered analysis produces the requested result type.
/// @details A mismatched type means the caller's template argument and the
///          registry declaration disagree. The comparison intentionally uses
///          compiler signature text instead of RTTI identity so it remains
///          reliable when the analysis registry and consuming pass are compiled
///          with hidden symbol visibility.
/// @tparam Result Type requested by the caller.
/// @param id Requested analysis identifier.
/// @param actual Type spelling recorded by the registry.
template <typename Result>
void requireAnalysisType(const std::string &id, std::string_view actual) {
    if (actual != analysisTypeName<Result>())
        throw std::logic_error(analysisTypeMismatchMessage<Result>(id, actual));
}

/// @brief Cast a type-erased cached analysis value back to its concrete type.
/// @details The stored type spelling must match @p Result before the
///          @c shared_ptr<void> payload is statically cast. An empty cache slot
///          indicates an internal AnalysisManager bookkeeping error.
/// @tparam Result Type requested by the caller.
/// @param id Requested analysis identifier.
/// @param value Cache value to cast.
/// @return Mutable reference to the cached @p Result object.
template <typename Result> Result &castAnalysisValue(const std::string &id, AnalysisValue &value) {
    requireAnalysisType<Result>(id, value.typeName);
    if (!value.storage)
        throw std::logic_error("empty analysis result cache entry");
    return *static_cast<Result *>(value.storage.get());
}
} // namespace detail

/// @brief Registry map from module-analysis identifiers to factories.
using ModuleAnalysisMap = std::unordered_map<std::string, detail::ModuleAnalysisRecord>;
/// @brief Registry map from function-analysis identifiers to factories.
using FunctionAnalysisMap = std::unordered_map<std::string, detail::FunctionAnalysisRecord>;

/// @brief Diagnostic counters for cache-miss computations.
struct AnalysisCounts {
    /// Number of module analysis results actually computed.
    std::size_t moduleComputations = 0;
    /// Number of function analysis results actually computed.
    std::size_t functionComputations = 0;
};

/// @brief Owns the available typed analysis factories.
/// @details Registration replaces any previous factory with the same identifier.
///          Managers borrow the exposed maps and therefore require this registry
///          to outlive them.
class AnalysisRegistry {
  public:
    /// @brief Register a module-level analysis computation.
    /// @tparam Result Concrete movable analysis result type.
    /// @param id     Unique string identifier used to look up the analysis later.
    /// @param fn     Callable that computes a fresh @c Result from a @c Module.
    template <typename Result>
    void registerModuleAnalysis(const std::string &id, std::function<Result(core::Module &)> fn) {
        /// Adapt the typed factory to the type-erased cache representation.
        moduleAnalyses_[id] = detail::ModuleAnalysisRecord{
            [fn = std::move(fn)](core::Module &module) -> detail::AnalysisValue {
                return detail::makeAnalysisValue<Result>(fn(module));
            },
            detail::analysisTypeName<Result>()};
    }

    /// @brief Register a function-level analysis computation.
    /// @tparam Result Concrete movable analysis result type.
    /// @param id     Unique string identifier used to look up the analysis later.
    /// @param fn     Callable that computes a fresh @c Result from a @c Module and @c Function.
    template <typename Result>
    void registerFunctionAnalysis(const std::string &id,
                                  std::function<Result(core::Module &, core::Function &)> fn) {
        /// Adapt the typed factory to the type-erased cache representation.
        functionAnalyses_[id] = detail::FunctionAnalysisRecord{
            [fn = std::move(fn)](core::Module &module,
                                 core::Function &fnRef) -> detail::AnalysisValue {
                return detail::makeAnalysisValue<Result>(fn(module, fnRef));
            },
            detail::analysisTypeName<Result>()};
    }

    /// @brief Access registered module-analysis factories.
    /// @return Borrowed map valid for this registry's lifetime.
    const ModuleAnalysisMap &moduleAnalyses() const {
        return moduleAnalyses_;
    }

    /// @brief Access registered function-analysis factories.
    /// @return Borrowed map valid for this registry's lifetime.
    const FunctionAnalysisMap &functionAnalyses() const {
        return functionAnalyses_;
    }

  private:
    ModuleAnalysisMap moduleAnalyses_;
    FunctionAnalysisMap functionAnalyses_;
};

/// @brief Manages computation and caching of analysis results during pass execution.
/// @details The AnalysisManager lazily computes analyses on demand, caches results,
///          and invalidates stale caches based on PreservedAnalyses information
///          from passes. Module and function analyses are tracked separately.
class AnalysisManager {
  public:
    /// @brief Construct an AnalysisManager for a module.
    /// @param module Module this manager operates on.
    /// @param registry Registry containing registered analysis factories.
    AnalysisManager(core::Module &module, const AnalysisRegistry &registry);

    /// @brief Retrieve or compute a module-level analysis result.
    /// @tparam Result Type of the analysis result.
    /// @param id Identifier of the analysis to run.
    /// @return Reference to the cached or freshly computed result.
    template <typename Result> Result &getModuleResult(const std::string &id) {
        // Fast-path for cache hits under a shared lock to avoid blocking other readers.
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            assert(moduleAnalyses_ && "no module analyses registered");
            if (!moduleAnalyses_)
                throw std::logic_error("no module analyses registered");
            auto it = moduleAnalyses_->find(id);
            assert(it != moduleAnalyses_->end() && "unknown module analysis");
            if (it == moduleAnalyses_->end())
                throw std::logic_error(
                    detail::unknownAnalysisMessage("module", id, *moduleAnalyses_));
            auto cacheIt = moduleCache_.find(id);
            if (cacheIt != moduleCache_.end() && cacheIt->second.has_value()) {
                assert(it->second.typeName == detail::analysisTypeName<Result>() &&
                       "analysis result type mismatch");
                detail::requireAnalysisType<Result>(id, it->second.typeName);
                return detail::castAnalysisValue<Result>(id, cacheIt->second);
            }
        }

        std::unique_lock<std::shared_mutex> lock(mutex_);
        assert(moduleAnalyses_ && "no module analyses registered");
        if (!moduleAnalyses_)
            throw std::logic_error("no module analyses registered");
        auto it = moduleAnalyses_->find(id);
        assert(it != moduleAnalyses_->end() && "unknown module analysis");
        if (it == moduleAnalyses_->end())
            throw std::logic_error(detail::unknownAnalysisMessage("module", id, *moduleAnalyses_));
        detail::AnalysisValue &cache = moduleCache_[id];
        if (!cache.has_value()) {
            cache = it->second.compute(module_);
            ++counts_.moduleComputations;
        }
        assert(it->second.typeName == detail::analysisTypeName<Result>() &&
               "analysis result type mismatch");
        detail::requireAnalysisType<Result>(id, it->second.typeName);
        return detail::castAnalysisValue<Result>(id, cache);
    }

    /// @brief Retrieve or compute a function-level analysis result.
    /// @tparam Result Type of the analysis result.
    /// @param id Identifier of the analysis to run.
    /// @param fn Function to analyze.
    /// @return Reference to the cached or freshly computed result.
    template <typename Result>
    Result &getFunctionResult(const std::string &id, core::Function &fn) {
        // Shared lock allows concurrent cache hits across functions.
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            assert(functionAnalyses_ && "no function analyses registered");
            if (!functionAnalyses_)
                throw std::logic_error("no function analyses registered");
            auto it = functionAnalyses_->find(id);
#ifndef NDEBUG
            if (it == functionAnalyses_->end()) {
                std::cerr << "Unknown function analysis '" << id << "'; registered:";
                for (const auto &entry : *functionAnalyses_)
                    std::cerr << " " << entry.first;
                std::cerr << std::endl;
            }
#endif
            assert(it != functionAnalyses_->end() && "unknown function analysis");
            if (it == functionAnalyses_->end())
                throw std::logic_error(
                    detail::unknownAnalysisMessage("function", id, *functionAnalyses_));
            auto cacheIt = functionCache_.find(id);
            if (cacheIt != functionCache_.end()) {
                auto fnIt = cacheIt->second.find(&fn);
                if (fnIt != cacheIt->second.end() && fnIt->second.has_value()) {
                    assert(it->second.typeName == detail::analysisTypeName<Result>() &&
                           "analysis result type mismatch");
                    detail::requireAnalysisType<Result>(id, it->second.typeName);
                    return detail::castAnalysisValue<Result>(id, fnIt->second);
                }
            }
        }

        std::unique_lock<std::shared_mutex> lock(mutex_);
        assert(functionAnalyses_ && "no function analyses registered");
        if (!functionAnalyses_)
            throw std::logic_error("no function analyses registered");
        auto it = functionAnalyses_->find(id);
#ifndef NDEBUG
        if (it == functionAnalyses_->end()) {
            std::cerr << "Unknown function analysis '" << id << "'; registered:";
            for (const auto &entry : *functionAnalyses_)
                std::cerr << " " << entry.first;
            std::cerr << std::endl;
        }
#endif
        assert(it != functionAnalyses_->end() && "unknown function analysis");
        if (it == functionAnalyses_->end())
            throw std::logic_error(
                detail::unknownAnalysisMessage("function", id, *functionAnalyses_));
        detail::AnalysisValue &cache = functionCache_[id][&fn];
        if (!cache.has_value()) {
            cache = it->second.compute(module_, fn);
            ++counts_.functionComputations;
        }
        assert(it->second.typeName == detail::analysisTypeName<Result>() &&
               "analysis result type mismatch");
        detail::requireAnalysisType<Result>(id, it->second.typeName);
        return detail::castAnalysisValue<Result>(id, cache);
    }

    /// @brief Invalidate analyses not preserved by a module pass.
    /// @param preserved Preservation info returned by the module pass.
    void invalidateAfterModulePass(const PreservedAnalyses &preserved);

    /// @brief Invalidate analyses not preserved by a function pass.
    /// @param preserved Preservation info returned by the function pass.
    /// @param fn Function that was transformed.
    void invalidateAfterFunctionPass(const PreservedAnalyses &preserved, core::Function &fn);

    /// @brief Invalidate one cached function-analysis result for a function.
    /// @details Provides a narrow escape hatch for multi-phase passes that need
    ///          to recompute a specific analysis after an internal mutation while
    ///          leaving unrelated cached analyses intact.
    /// @param id Identifier of the function analysis to drop.
    /// @param fn Function whose cached analysis entry should be erased.
    void invalidateFunctionResult(const std::string &id, core::Function &fn);

    /// @brief Get mutable access to the module.
    /// @return Reference to the managed module.
    core::Module &module() {
        return module_;
    }

    /// @brief Get const access to the module.
    /// @return Const reference to the managed module.
    const core::Module &module() const {
        return module_;
    }

    /// @brief Snapshot analysis computation counts for diagnostics.
    /// @return Number of module and function analyses computed so far.
    AnalysisCounts counts() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return counts_;
    }

  private:
    core::Module &module_;
    const ModuleAnalysisMap *moduleAnalyses_ = nullptr;
    const FunctionAnalysisMap *functionAnalyses_ = nullptr;
    std::unordered_map<std::string, detail::AnalysisValue> moduleCache_;
    std::unordered_map<std::string,
                       std::unordered_map<const core::Function *, detail::AnalysisValue>>
        functionCache_;
    AnalysisCounts counts_{};
    mutable std::shared_mutex mutex_;

    friend class AnalysisCacheInvalidator;
};

} // namespace il::transform
