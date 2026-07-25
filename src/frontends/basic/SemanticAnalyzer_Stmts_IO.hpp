//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SemanticAnalyzer_Stmts_IO.hpp
// Purpose: Declares helpers specific to IO-oriented statement analysis for the
// Key invariants: IO helpers leverage shared utilities without exposing analyzer internals.
// Ownership/Lifetime: Helpers borrow SemanticAnalyzer state.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file SemanticAnalyzer_Stmts_IO.hpp
/// @brief Declares the restricted context used by BASIC I/O checkers.
/// @details The type adds no state to @ref StmtShared; it exposes shared RAII
///          guards and helper access through an I/O-specific facade.

#pragma once

#include "frontends/basic/SemanticAnalyzer_Stmts_Shared.hpp"

namespace il::frontends::basic::semantic_analyzer_detail {

/// @brief I/O-specific facade over common statement-analysis state.
/// @invariant The analyzer borrowed by the base class outlives this context.
class IOStmtContext : public StmtShared {
  public:
    /// @brief RAII guard for one loop/EXIT context.
    using LoopGuard = StmtShared::LoopGuard;

    /// @brief RAII guard for a FOR variable and loop context.
    using ForLoopGuard = StmtShared::ForLoopGuard;

    /// @brief Binds I/O helpers to an existing analyzer.
    /// @param analyzer Analyzer borrowed by @ref StmtShared.
    explicit IOStmtContext(SemanticAnalyzer &analyzer) noexcept;
};

} // namespace il::frontends::basic::semantic_analyzer_detail
