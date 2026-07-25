//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file ProcedureSymbolTracker.cpp
/// @brief Implements BASIC procedure symbol usage tracking.
/// @details Provides the out-of-line definitions for
///          @ref ProcedureSymbolTracker, which records symbol usage during
///          lowering through the Lowerer's symbol-table mutation APIs and
///          optionally records module symbols that require cross-procedure
///          storage.
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/ProcedureSymbolTracker.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"

#include <utility>

namespace il::frontends::basic {

/// @brief Construct a symbol tracker bound to a lowerer.
/// @details Stores the lowerer reference and the flag controlling whether
///          cross-procedure global tracking is enabled.
/// @param lowerer Lowering engine used to record symbol references.
/// @param trackCrossProc Whether to track cross-procedure globals.
/// @pre @p lowerer outlives the tracker.
ProcedureSymbolTracker::ProcedureSymbolTracker(Lowerer &lowerer, bool trackCrossProc) noexcept
    : lowerer_(lowerer), trackCrossProc_(trackCrossProc) {}

/// @brief Determine whether a symbol should be skipped by the tracker.
/// @details Skips empty names and implicit field references to avoid polluting
///          symbol usage sets with non-variable identifiers.
/// @param name Symbol name to inspect.
/// @return True when tracking should be skipped for @p name.
bool ProcedureSymbolTracker::shouldSkip(std::string_view name) const {
    if (name.empty())
        return true;
    if (lowerer_.isFieldInScope(name))
        return true;
    return false;
}

/// @brief Record usage of a scalar symbol.
/// @details Marks the symbol as referenced and, when enabled, updates the
///          cross-procedure global set.
/// @param name Symbol name to record.
void ProcedureSymbolTracker::trackScalar(std::string_view name) {
    if (shouldSkip(name))
        return;
    lowerer_.markSymbolReferenced(name);
    trackCrossProcGlobalIfNeeded(name);
}

/// @brief Record usage of an array symbol.
/// @details Marks the symbol as referenced, flags it as an array, and optionally
///          updates cross-procedure global tracking. If module metadata records
///          an object element class, that class is copied into symbol metadata.
/// @param name Array symbol name to record.
void ProcedureSymbolTracker::trackArray(std::string_view name) {
    if (shouldSkip(name))
        return;
    lowerer_.markSymbolReferenced(name);
    lowerer_.markArray(name);
    if (std::string elemClass = lowerer_.lookupModuleArrayElemClass(name); !elemClass.empty())
        lowerer_.setSymbolObjectType(name, std::move(elemClass));
    trackCrossProcGlobalIfNeeded(name);
}

/// @brief Record usage of a symbol with explicit array classification.
/// @details Dispatches to @ref trackArray or @ref trackScalar based on the
///          @p isArray flag.
/// @param name Symbol name to record.
/// @param isArray Whether the symbol should be treated as an array.
void ProcedureSymbolTracker::track(std::string_view name, bool isArray) {
    if (isArray)
        trackArray(name);
    else
        trackScalar(name);
}

/// @brief Record cross-procedure global usage when enabled.
/// @details When cross-procedure tracking is active, this helper consults the
///          semantic analyzer to determine whether the symbol is module-level
///          and marks it as a cross-procedure global if referenced from a
///          non-main procedure (or during early scans).
/// @param name Symbol name to inspect.
void ProcedureSymbolTracker::trackCrossProcGlobalIfNeeded(std::string_view name) {
    if (!trackCrossProc_)
        return;

    if (name.empty())
        return;

    auto sema = lowerer_.semanticAnalyzer();
    if (!sema)
        return;

    // Track cross-proc globals when either:
    // 1. fn == nullptr (early scan phase before function context is set)
    // 2. fn->name != "main" (inside a procedure other than @main)
    // This ensures module-level symbols used in procedures get runtime-backed storage.
    const auto *fn = lowerer_.context().function();
    if (fn == nullptr || fn->name != "main") {
        // Construct the string once and reuse for both checks
        std::string nameStr(name);
        if (sema->isModuleLevelSymbol(nameStr))
            lowerer_.markCrossProcGlobal(std::move(nameStr));
    }
}

/// @brief Check whether the current lowering context is inside @c main.
/// @details Looks at the active function in the lowering context and returns
///          true only when it is present and named "main".
/// @return True when lowering is currently within the main procedure.
bool ProcedureSymbolTracker::isInMain() const {
    const auto *fn = lowerer_.context().function();
    return fn != nullptr && fn->name == "main";
}

} // namespace il::frontends::basic
