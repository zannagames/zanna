//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SemanticAnalyzer_Stmts_Shared.hpp
// Purpose: Declares reusable BASIC statement-analysis helpers and movable RAII
//          guards for loop-kind and FOR-control-variable tracking.
// Key invariants:
//   - Each live guard owns exactly one pending stack pop.
//   - Moving a guard transfers that responsibility and leaves the source inert.
//   - Helpers never own or extend the lifetime of the bound analyzer.
// Ownership/Lifetime:
//   - StmtShared stores a non-owning reference to a SemanticAnalyzer.
//   - Guard analyzer pointers remain valid only while that analyzer is alive.
// Links: src/frontends/basic/SemanticAnalyzer_Stmts_Shared.cpp,
//        src/frontends/basic/SemanticAnalyzer.hpp,
//        src/frontends/basic/SemanticAnalyzer_Stmts_Control.hpp,
//        src/frontends/basic/SemanticAnalyzer_Stmts_IO.hpp,
//        src/frontends/basic/SemanticAnalyzer_Stmts_Runtime.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/SemanticAnalyzer.hpp"

#include <string>
#include <string_view>

/// @file
/// @brief Declares shared state helpers for BASIC statement semantic analysis.

namespace il::frontends::basic::semantic_analyzer_detail {

/// @brief Shared utilities reused by themed statement analyzers.
/// @details Provides access to loop-variable queries and standardized
///          diagnostics while keeping the owning analyzer private to the
///          implementation classes that derive from or contain this helper.
/// @invariant @ref analyzer_ refers to a live analyzer.
class StmtShared {
  public:
    /// @brief Creates a helper bound to an existing semantic analyzer.
    /// @param analyzer Analyzer borrowed for the full lifetime of this object.
    explicit StmtShared(SemanticAnalyzer &analyzer) noexcept;

    /// @brief Prevents copying a helper with a bound analyzer reference.
    StmtShared(const StmtShared &) = delete;

    /// @brief Prevents rebinding the helper through copy assignment.
    StmtShared &operator=(const StmtShared &) = delete;

    /// @brief Guard that pushes a loop kind on construction and pops it on
    ///        destruction.
    /// @details The guard is movable but not copyable. A moved-from guard is
    ///          inert, ensuring exactly one destructor balances each push.
    class LoopGuard {
      public:
        /// @brief Pushes @p kind onto @p analyzer's active loop stack.
        /// @param analyzer Analyzer whose stack is changed and which must
        ///                 outlive the guard.
        /// @param kind Loop or procedure context to make active.
        LoopGuard(SemanticAnalyzer &analyzer, SemanticAnalyzer::LoopKind kind) noexcept;

        /// @brief Disables copying to preserve single ownership of the stack pop.
        LoopGuard(const LoopGuard &) = delete;

        /// @brief Disables copy assignment to preserve balanced stack ownership.
        LoopGuard &operator=(const LoopGuard &) = delete;

        /// @brief Transfers the pending stack pop from @p other.
        /// @param other Guard left inert after the move.
        LoopGuard(LoopGuard &&other) noexcept;

        /// @brief Releases this guard's entry and adopts @p other's entry.
        /// @param other Guard left inert after a non-self move.
        /// @return Reference to this guard.
        LoopGuard &operator=(LoopGuard &&other) noexcept;

        /// @brief Pops the owned loop entry unless this guard was moved from.
        ~LoopGuard() noexcept;

      private:
        /// Analyzer whose stack this guard must pop, or null after a move.
        SemanticAnalyzer *analyzer_{nullptr};
    };

    /// @brief Guard that records an active FOR loop variable for the current
    ///        statement body.
    /// @details The variable is stored in the analyzer's nesting stack until
    ///          the guard is destroyed or its responsibility is transferred.
    class ForLoopGuard {
      public:
        /// @brief Registers a FOR control variable with @p analyzer.
        /// @param analyzer Analyzer that must outlive the guard.
        /// @param variable Canonical variable name transferred into analyzer
        ///                 storage.
        ForLoopGuard(SemanticAnalyzer &analyzer, std::string variable);

        /// @brief Disables copying to preserve single ownership of deregistration.
        ForLoopGuard(const ForLoopGuard &) = delete;

        /// @brief Disables copy assignment to preserve balanced stack ownership.
        ForLoopGuard &operator=(const ForLoopGuard &) = delete;

        /// @brief Transfers pending variable deregistration from @p other.
        /// @param other Guard left inert after the move.
        ForLoopGuard(ForLoopGuard &&other) noexcept;

        /// @brief Deregisters this guard's variable and adopts @p other's.
        /// @param other Guard left inert after a non-self move.
        /// @return Reference to this guard.
        ForLoopGuard &operator=(ForLoopGuard &&other) noexcept;

        /// @brief Deregisters the owned variable unless moved from.
        ~ForLoopGuard() noexcept;

      private:
        /// Analyzer whose FOR-variable stack this guard must pop, or null.
        SemanticAnalyzer *analyzer_{nullptr};
    };

    /// @brief Determine whether @p name is currently an active FOR loop variable.
    /// @param name Canonical identifier to search in the analyzer's active stack.
    /// @return @c true when any active FOR guard tracks @p name.
    [[nodiscard]] bool isLoopVariable(std::string_view name) const noexcept;

    /// @brief Emit the standard diagnostic for mutating a loop variable.
    /// @param name Canonical variable name inserted into diagnostic B1010.
    /// @param loc Source location at which the mutation begins.
    /// @param width Number of source columns to highlight.
    void reportLoopVariableMutation(const std::string &name,
                                    const il::support::SourceLoc &loc,
                                    uint32_t width);

  protected:
    /// @brief Returns mutable access to the bound analyzer.
    /// @return Borrowed reference valid for this helper's lifetime.
    SemanticAnalyzer &analyzer() noexcept {
        return analyzer_;
    }

    /// @brief Returns immutable access to the bound analyzer.
    /// @return Borrowed reference valid for this helper's lifetime.
    const SemanticAnalyzer &analyzer() const noexcept {
        return analyzer_;
    }

  private:
    /// Non-owning analyzer reference used by all helper operations.
    SemanticAnalyzer &analyzer_;
};

} // namespace il::frontends::basic::semantic_analyzer_detail
