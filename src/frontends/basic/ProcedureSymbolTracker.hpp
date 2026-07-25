//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ProcedureSymbolTracker.hpp
// Purpose: Centralizes symbol usage tracking for procedure-level lowering.
// Key invariants: All symbol tracking goes through this helper to avoid
//                 duplicated logic in VarCollectWalker and RuntimeNeedsScanner.
// Ownership/Lifetime: Operates on borrowed Lowerer state; does not own data.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
/// @file ProcedureSymbolTracker.hpp
/// @brief Declares shared scalar, array, and cross-procedure usage tracking.
/// @details ProcedureSymbolTracker borrows a Lowerer and centralizes the symbol
///          mutations performed by variable discovery and runtime-needs scans.

#pragma once

#include <string_view>

namespace il::frontends::basic {

class Lowerer;
class SemanticAnalyzer;

/// @brief Centralizes symbol usage tracking during procedure lowering.
/// @details Provides a unified API for recording symbol references, array usage,
///          and cross-procedure global tracking. This avoids duplicated logic
///          between VarCollectWalker (variable discovery) and RuntimeNeedsScanner
///          (runtime helper tracking).
///
/// Key responsibilities:
/// - Recording symbol usage (scalar vs array)
/// - Marking cross-procedure global usage for runtime-backed storage
/// - Checking field scope to skip class members
/// - Enforcing module-level symbol sharing rules
/// @invariant The borrowed Lowerer outlives this tracker.
class ProcedureSymbolTracker {
  public:
    /// @brief Construct a tracker bound to the lowering context.
    /// @param lowerer Borrowed lowering driver whose symbol tables are updated.
    /// @param trackCrossProc If true, marks module-level symbols used outside
    ///        @main or before a function is active as cross-procedure globals.
    ///        Should be true for variable collection and false for runtime-needs scanning.
    /// @pre @p lowerer outlives the tracker.
    explicit ProcedureSymbolTracker(Lowerer &lowerer, bool trackCrossProc = true) noexcept;

    /// @brief Check if a symbol name should be skipped (empty or field in scope).
    /// @param name Symbol name to check.
    /// @return True if the symbol should be skipped from tracking.
    [[nodiscard]] bool shouldSkip(std::string_view name) const;

    /// @brief Record usage of a scalar variable.
    /// @details Marks the symbol as referenced and optionally checks for
    ///          cross-procedure global usage when outside @main.
    /// @param name Variable name to track.
    void trackScalar(std::string_view name);

    /// @brief Record usage of an array variable.
    /// @details Marks the symbol as both referenced and an array, and optionally
    ///          checks cross-procedure global usage. Module object-element
    ///          metadata, when present, is copied to the symbol record.
    /// @param name Array name to track.
    void trackArray(std::string_view name);

    /// @brief Record usage of a variable that may be scalar or array.
    /// @details Unified entry point that marks referenced and optionally array.
    /// @param name Variable name to track.
    /// @param isArray True if the variable is used with array semantics.
    void track(std::string_view name, bool isArray);

    /// @brief Check and mark cross-procedure global usage if applicable.
    /// @details When enabled and semantic analysis classifies the symbol as
    ///          module-level, records it if the active function is non-main or
    ///          no function is active yet.
    /// @param name Symbol name to check.
    void trackCrossProcGlobalIfNeeded(std::string_view name);

  private:
    /// @brief Check if currently lowering the @main function.
    /// @return `true` only for a non-null active function named exactly `main`.
    [[nodiscard]] bool isInMain() const;

    /// Borrowed lowering driver receiving all tracking mutations.
    Lowerer &lowerer_;
    /// Whether module-level cross-procedure classification is active.
    bool trackCrossProc_;
};

} // namespace il::frontends::basic
