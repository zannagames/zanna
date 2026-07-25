//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SemanticAnalyzer_Stmts_Runtime.hpp
// Purpose: Declares helpers specific to runtime/data-manipulation statement
// Key invariants: Runtime helpers mirror analyzer state through restricted access.
// Ownership/Lifetime: Helpers borrow SemanticAnalyzer state.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file SemanticAnalyzer_Stmts_Runtime.hpp
/// @brief Declares the restricted context used by runtime-state checkers.
/// @details The facade adds no state to @ref StmtShared and re-exports its
///          balanced loop/FOR guard types for specialized helpers.

#pragma once

#include "frontends/basic/SemanticAnalyzer_Stmts_Shared.hpp"

namespace il::frontends::basic::semantic_analyzer_detail {

/// @brief Runtime-statement-specific facade over shared analyzer access.
/// @invariant The analyzer borrowed by the base class outlives this context.
class RuntimeStmtContext : public StmtShared {
  public:
    /// @brief RAII guard for one active loop/EXIT construct.
    using LoopGuard = StmtShared::LoopGuard;

    /// @brief RAII guard for one FOR variable and loop construct.
    using ForLoopGuard = StmtShared::ForLoopGuard;

    /// @brief Binds runtime statement helpers to an analyzer.
    /// @param analyzer Analyzer borrowed by @ref StmtShared.
    explicit RuntimeStmtContext(SemanticAnalyzer &analyzer) noexcept;
};

} // namespace il::frontends::basic::semantic_analyzer_detail
