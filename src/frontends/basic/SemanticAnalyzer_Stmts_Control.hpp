//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SemanticAnalyzer_Stmts_Control.hpp
// Purpose: Declares helpers specific to control-flow statement analysis for the
// Key invariants: Helpers reuse shared RAII guards to keep loop tracking balanced.
// Ownership/Lifetime: Non-owning views over SemanticAnalyzer state.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file SemanticAnalyzer_Stmts_Control.hpp
/// @brief Declares the restricted context used by control statement checkers.
/// @details The context adds no state to @ref StmtShared; it exposes the shared
///          loop and FOR-variable RAII guard types under a control-specific name.

#pragma once

#include "frontends/basic/SemanticAnalyzer_Stmts_Shared.hpp"

namespace il::frontends::basic::semantic_analyzer_detail {

/// @brief Control-flow-specific facade over shared statement analyzer access.
/// @invariant The analyzer borrowed by @ref StmtShared outlives this context.
class ControlStmtContext : public StmtShared {
  public:
    /// @brief RAII guard for one active loop/EXIT construct.
    using LoopGuard = StmtShared::LoopGuard;

    /// @brief RAII guard for one active FOR variable plus loop construct.
    using ForLoopGuard = StmtShared::ForLoopGuard;

    /// @brief Binds control helpers to an existing semantic analyzer.
    /// @param analyzer Analyzer borrowed by the base context.
    /// @post No stack is modified until a guard/helper operation is requested.
    explicit ControlStmtContext(SemanticAnalyzer &analyzer) noexcept : StmtShared(analyzer) {}
};

} // namespace il::frontends::basic::semantic_analyzer_detail
