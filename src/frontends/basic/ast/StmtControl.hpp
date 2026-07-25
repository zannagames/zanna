//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ast/StmtControl.hpp
// Purpose: Defines BASIC branches, SELECT arms, exception regions, loops,
//          transfers, returns, label nodes, and scoped USING blocks.
// Key invariants:
//   - Each stmtKind override matches its concrete node type.
//   - Nested expressions and statements use unique ownership.
//   - Ordered vectors preserve source evaluation/execution order.
//   - Nullable children are explicitly documented for optional syntax.
// Ownership/Lifetime:
//   - Nodes own all ExprPtr and StmtPtr children.
//   - Visitors borrow a node only during accept dispatch.
// Links: src/frontends/basic/ast/StmtBase.hpp,
//        src/frontends/basic/ast/ExprNodes.hpp,
//        src/frontends/basic/SelectModel.hpp,
//        src/frontends/basic/AST.cpp,
//        src/frontends/basic/Parser_Stmt_Control.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/SelectModel.hpp"
#include "frontends/basic/ast/ExprNodes.hpp"
#include "frontends/basic/ast/StmtBase.hpp"

#include "support/source_location.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/// @file
/// @brief Defines control-flow and structured-lifetime BASIC statement nodes.

namespace il::frontends::basic {

/// @brief Pseudo statement that only carries a line label.
struct LabelStmt : Stmt {
    /// @brief Returns this node's label discriminator.
    /// @return @ref Kind::Label.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Label;
    }

    /// @brief Dispatches this label to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this label to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief IF statement with optional ELSEIF chain and ELSE branch.
struct IfStmt : Stmt {
    /// @brief Returns this node's IF discriminator.
    /// @return @ref Kind::If.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::If;
    }

    /// @brief ELSEIF arm.
    struct ElseIf {
        /// Condition expression controlling this arm; owned and non-null.
        ExprPtr cond;

        /// Executed when @ref cond evaluates to true; owned and non-null.
        StmtPtr then_branch;
    };

    /// Initial IF condition; owned and non-null.
    ExprPtr cond;

    /// THEN branch when @ref cond is true; owned and non-null.
    StmtPtr then_branch;

    /// Zero or more ELSEIF arms evaluated in order.
    std::vector<ElseIf> elseifs;

    /// Optional trailing ELSE branch (may be null) executed when no condition matched.
    StmtPtr else_branch;

    /// @brief Dispatches this IF tree to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this IF tree to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief Arm within a SELECT CASE statement.
struct CaseArm {
    /// @brief Relational guard matched by the arm.
    struct CaseRel {
        /// @brief Relational operation kind.
        enum Op {
            LT, ///< Selector must be less than rhs.
            LE, ///< Selector must be less than or equal to rhs.
            EQ, ///< Selector must equal rhs.
            GE, ///< Selector must be greater than or equal to rhs.
            GT  ///< Selector must be greater than rhs.
        };

        /// @brief Relational operator applied to the selector.
        Op op{EQ};

        /// @brief Right-hand-side integer operand compared against the selector.
        std::int64_t rhs{0};
    };

    /// @brief Literal labels matched by the arm.
    std::vector<std::int64_t> labels;

    /// @brief String literal labels matched by the arm when the selector is a string.
    std::vector<std::string> str_labels;

    /// @brief Inclusive integer ranges matched by the arm.
    std::vector<std::pair<std::int64_t, std::int64_t>> ranges;

    /// @brief Relational comparisons matched by the arm.
    std::vector<CaseRel> rels;

    /// @brief Statements executed when the labels match.
    std::vector<StmtPtr> body;

    /// @brief Source range covering the CASE keyword and its labels.
    il::support::SourceRange range{};

    /// @brief Length of the CASE keyword lexeme for diagnostics.
    uint32_t caseKeywordLength = 0;
};

/// @brief SELECT CASE statement with zero or more CASE arms and optional ELSE body.
struct SelectCaseStmt : Stmt {
    /// @brief Returns this node's SELECT CASE discriminator.
    /// @return @ref Kind::SelectCase.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::SelectCase;
    }

    /// @brief Expression whose value selects a CASE arm; owned and non-null.
    ExprPtr selector;

    /// @brief Ordered CASE arms evaluated sequentially.
    std::vector<CaseArm> arms;

    /// @brief Statements executed when no CASE label matches; empty when absent.
    std::vector<StmtPtr> elseBody;

    /// @brief Source range spanning the SELECT CASE header.
    il::support::SourceRange range{};

    /// @brief Normalised model describing CASE labels and ranges.
    SelectModel model{};

    /// @brief Dispatches this SELECT CASE tree to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this SELECT CASE tree to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief TRY/CATCH/FINALLY statement with optional catch variable and finally block.
///
/// Syntax:
///   TRY
///       <try-body>
///   [CATCH [errVar]
///       <catch-body>]
///   [FINALLY
///       <finally-body>]
///   END TRY
///
/// At least one of CATCH or FINALLY must be present.
/// The finally block always executes after try/catch regardless of whether
/// an exception occurred, providing guaranteed cleanup semantics.
struct TryCatchStmt : Stmt {
    /// @brief Returns this node's TRY/CATCH discriminator.
    /// @return @ref Kind::TryCatch.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::TryCatch;
    }

    /// @brief Statements executed under the TRY region.
    std::vector<StmtPtr> tryBody;

    /// @brief Optional catch variable name (binds error code as i64 when present).
    std::optional<std::string> catchVar;

    /// @brief Statements executed when an error is caught. Empty if no CATCH clause.
    std::vector<StmtPtr> catchBody;

    /// @brief Statements executed unconditionally after try/catch. Empty if no FINALLY clause.
    std::vector<StmtPtr> finallyBody;

    /// @brief Source range covering the TRY…CATCH header for diagnostics.
    il::support::SourceRange header{};

    /// @brief Dispatches this exception region to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this exception region to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief WHILE ... WEND loop statement.
struct WhileStmt : Stmt {
    /// @brief Returns this node's WHILE discriminator.
    /// @return @ref Kind::While.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::While;
    }

    /// Loop continuation condition; owned and non-null.
    ExprPtr cond;

    /// Body statements executed while @ref cond is true.
    std::vector<StmtPtr> body;

    /// @brief Dispatches this loop to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this loop to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief DO ... LOOP statement supporting WHILE and UNTIL tests.
struct DoStmt : Stmt {
    /// @brief Returns this node's DO discriminator.
    /// @return @ref Kind::Do.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Do;
    }
    /// Condition kind controlling loop continuation.
    enum class CondKind {
        None,  ///< No explicit condition; loop runs until EXIT.
        While, ///< Continue while condition evaluates to true.
        Until, ///< Continue until condition evaluates to true.
    } condKind{CondKind::None};

    /// Whether condition is evaluated before or after executing the body.
    enum class TestPos {
        Pre,  ///< Evaluate condition before each iteration.
        Post, ///< Evaluate condition after executing the body.
    } testPos{TestPos::Pre};

    /// Continuation condition; null when @ref condKind == CondKind::None.
    ExprPtr cond;

    /// Ordered statements forming the loop body.
    std::vector<StmtPtr> body;

    /// @brief Dispatches this loop to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this loop to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief FOR ... NEXT loop statement.
struct ForStmt : Stmt {
    /// @brief Returns this node's FOR discriminator.
    /// @return @ref Kind::For.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::For;
    }

    /// Loop variable expression (lvalue) controlling the iteration.
    /// Can be a VarExpr (simple variable), MemberAccessExpr (object member),
    /// or ArrayExpr (array element). Owned and non-null.
    /// BUG-081 fix: Changed from std::string to ExprPtr to support object members.
    ExprPtr varExpr;

    /// Initial value assigned to loop variable; owned and non-null.
    ExprPtr start;

    /// Loop end value; owned and non-null.
    ExprPtr end;

    /// Optional step expression; null means 1.
    ExprPtr step;

    /// Body statements executed each iteration.
    std::vector<StmtPtr> body;

    /// @brief Dispatches this loop to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this loop to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief FOR EACH ... IN ... NEXT loop statement for array iteration.
/// @details Iterates over all elements of an array, assigning each element
///          to the loop variable in sequence. The loop runs from the first
///          to the last element of the array.
struct ForEachStmt : Stmt {
    /// @brief Returns this node's FOR EACH discriminator.
    /// @return @ref Kind::ForEach.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::ForEach;
    }

    /// Name of the element variable receiving each array element.
    std::string elementVar;

    /// Name of the array being iterated.
    std::string arrayName;

    /// Body statements executed for each element.
    std::vector<StmtPtr> body;

    /// @brief Dispatches this loop to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this loop to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief NEXT statement closing a FOR.
struct NextStmt : Stmt {
    /// @brief Returns this node's NEXT discriminator.
    /// @return @ref Kind::Next.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Next;
    }

    /// Loop variable after NEXT.
    std::string var;

    /// @brief Dispatches this NEXT statement to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this NEXT statement to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief EXIT statement leaving the innermost enclosing loop.
struct ExitStmt : Stmt {
    /// @brief Returns this node's EXIT discriminator.
    /// @return @ref Kind::Exit.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Exit;
    }
    /// Loop type targeted by this EXIT.
    enum class LoopKind {
        For,      ///< EXIT FOR
        While,    ///< EXIT WHILE
        Do,       ///< EXIT DO
        Sub,      ///< EXIT SUB
        Function, ///< EXIT FUNCTION
    } kind{LoopKind::While};

    /// @brief Dispatches this EXIT statement to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this EXIT statement to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief GOTO statement transferring control to a line number.
struct GotoStmt : Stmt {
    /// @brief Returns this node's GOTO discriminator.
    /// @return @ref Kind::Goto.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Goto;
    }

    /// Target line number to jump to.
    int target{0};

    /// @brief Dispatches this transfer to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this transfer to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief GOSUB statement invoking a line label as a subroutine.
struct GosubStmt : Stmt {
    /// @brief Returns this node's GOSUB discriminator.
    /// @return @ref Kind::Gosub.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Gosub;
    }

    /// Target line number to branch to.
    int targetLine{0};

    /// @brief Dispatches this subroutine transfer to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this subroutine transfer to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief ON ERROR GOTO statement configuring error handler target.
struct OnErrorGoto : Stmt {
    /// @brief Returns this node's ON ERROR GOTO discriminator.
    /// @return @ref Kind::OnErrorGoto.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::OnErrorGoto;
    }

    /// Destination line for error handler when @ref toZero is false.
    int target{0};

    /// True when the statement uses "GOTO 0" to disable the handler.
    bool toZero{false};

    /// @brief Dispatches this handler configuration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this handler configuration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief RESUME statement controlling error-handler resumption.
struct Resume : Stmt {
    /// @brief Returns this node's RESUME discriminator.
    /// @return @ref Kind::Resume.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Resume;
    }
    /// Resumption strategy following an error handler.
    enum class Mode {
        Same,  ///< Resume execution at the failing statement.
        Next,  ///< Resume at the statement following the failure site.
        Label, ///< Resume at a labeled line.
    } mode{Mode::Same};

    /// Target line label when @ref mode == Mode::Label.
    int target{0};

    /// @brief Dispatches this resumption to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this resumption to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief END statement terminating program execution.
struct EndStmt : Stmt {
    /// @brief Returns this node's END discriminator.
    /// @return @ref Kind::End.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::End;
    }

    /// @brief Dispatches this termination to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this termination to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief RETURN statement optionally yielding a value.
struct ReturnStmt : Stmt {
    /// @brief Returns this node's RETURN discriminator.
    /// @return @ref Kind::Return.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Return;
    }

    /// Expression whose value is returned; null when no expression is provided.
    ExprPtr value;

    /// True when this RETURN exits a GOSUB (top-level RETURN without a value).
    bool isGosubReturn{false};

    /// @brief Dispatches this return to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this return to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief USING statement for automatic resource disposal.
///
/// Syntax:
///   USING varName AS TypeName = NEW TypeName(args)
///       <body>
///   END USING
///
/// Desugars to:
///   DIM varName AS TypeName
///   varName = NEW TypeName(args)
///   TRY
///       <body>
///   FINALLY
///       DISPOSE varName
///   END TRY
///
/// Guarantees cleanup of the resource when the USING block exits.
struct UsingStmt : Stmt {
    /// @brief Returns this node's scoped USING discriminator.
    /// @return @ref Kind::UsingStmt.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::UsingStmt;
    }

    /// Variable name for the managed resource.
    std::string varName;

    /// Qualified type name parts, e.g. ["Zanna", "IO", "File"].
    std::vector<std::string> typeQualified;

    /// Initializer expression (typically a NEW expression); may be null for existing var.
    ExprPtr initExpr;

    /// Body statements executed within the USING block.
    std::vector<StmtPtr> body;

    /// @brief Dispatches this scoped resource block to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override {
        visitor.visit(*this);
    }

    /// @brief Dispatches this scoped resource block to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override {
        visitor.visit(*this);
    }
};

} // namespace il::frontends::basic
