//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/AstWalker.hpp
// Purpose: Provides a reusable recursive AST walker for BASIC front-end passes.
// Key invariants: Traversal order matches the legacy visitors for statements and expressions.
// Ownership/Lifetime: Walker borrows AST nodes without owning them.
// Links: src/frontends/basic/AstWalkerUtils.hpp, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file AstWalker.hpp
 * @brief Defines the CRTP-based recursive walker for the complete BASIC AST.
 *
 * Every concrete visitor brackets a node with optional Derived::before() and
 * Derived::after() hooks. Child traversal can be pruned per node and exposes
 * parent/child hooks while retaining the frontend's established visit order.
 */

#pragma once

#include "frontends/basic/AstWalkerUtils.hpp"
#include "frontends/basic/ast/DeclNodes.hpp"
#include <type_traits>

namespace il::frontends::basic {

/// @brief Generic recursive AST walker that forwards traversal hooks to @p Derived.
/// @tparam Derived Concrete walker implementing optional callbacks.
/// @details The walker visits statements and expressions in the same order as the
/// legacy lowering visitors. Derived classes may override `before`,
/// `after`, `shouldVisitChildren`, `beforeChild`, and `afterChild` for any
/// node type; the base implementation provides no-op defaults.
template <typename Derived> class BasicAstWalker : public ExprVisitor, public StmtVisitor {
  public:
    /// Preserve the complete overloaded expression visitor set.
    using ExprVisitor::visit;
    /// Preserve the complete overloaded statement visitor set.
    using StmtVisitor::visit;

    /// @brief Visit an expression subtree.
    /// @param expr Root expression to walk.
    /// @details Dispatches through @p expr using the CRTP derived visitor.
    void walkExpr(const Expr &expr) {
        expr.accept(*static_cast<Derived *>(this));
    }

    /// @brief Visit a statement subtree.
    /// @param stmt Root statement to walk.
    /// @details Dispatches through @p stmt using the CRTP derived visitor.
    void walkStmt(const Stmt &stmt) {
        stmt.accept(*static_cast<Derived *>(this));
    }

  protected:
    /// @brief Construct the stateless walker base for a Derived instance.
    BasicAstWalker() = default;

    template <typename OtherDerived, typename Parent, typename Ptr>
    friend void walker::detail::visitOptionalChild(BasicAstWalker<OtherDerived> &,
                                                   const Parent &,
                                                   const Ptr &);
    template <typename OtherDerived, typename Parent, typename Range>
    friend void walker::detail::visitChildRange(BasicAstWalker<OtherDerived> &,
                                                const Parent &,
                                                const Range &);
    template <typename OtherDerived, typename Parent, typename Child>
    friend void walker::detail::notifyChild(BasicAstWalker<OtherDerived> &,
                                            const Parent &,
                                            const Child &);
    template <typename OtherDerived, typename Parent, typename Range>
    friend void walker::detail::notifyChildRange(BasicAstWalker<OtherDerived> &,
                                                 const Parent &,
                                                 const Range &);
    template <typename OtherDerived>
    friend void walker::detail::visitPrintItem(BasicAstWalker<OtherDerived> &,
                                               const PrintStmt &,
                                               const PrintItem &);
    template <typename OtherDerived>
    friend void walker::detail::visitPrintItems(BasicAstWalker<OtherDerived> &, const PrintStmt &);

    /// @brief Invoke the Derived pre-visit hook when available.
    /// @details Override `Derived::before` to run logic before traversing @p node, such as
    /// updating bookkeeping stacks or allocating temporary state.
    /// @tparam Node Concrete node type being entered.
    /// @param node Node passed to the detected hook.
    template <typename Node> void callBefore(const Node &node) {
        if constexpr (requires(Derived &d, const Node &n) { d.before(n); })
            static_cast<Derived *>(this)->before(node);
    }

    /// @brief Invoke the Derived post-visit hook when available.
    /// @details Override `Derived::after` to clean up state after all children of @p node
    /// were processed or to record synthesized results.
    /// @tparam Node Concrete node type being exited.
    /// @param node Node passed to the detected hook.
    template <typename Node> void callAfter(const Node &node) {
        if constexpr (requires(Derived &d, const Node &n) { d.after(n); })
            static_cast<Derived *>(this)->after(node);
    }

    /// @brief Ask Derived whether to traverse the children of @p node.
    /// @details Override `Derived::shouldVisitChildren` to short-circuit traversal for
    /// pruned subtrees or to skip nodes that were already processed elsewhere.
    /// @tparam Node Concrete node type whose children may be pruned.
    /// @param node Node passed to the optional predicate.
    /// @return The derived predicate's result when available, otherwise true.
    template <typename Node> bool callShouldVisit(const Node &node) {
        if constexpr (requires(Derived &d, const Node &n) { d.shouldVisitChildren(n); })
            return static_cast<Derived *>(this)->shouldVisitChildren(node);
        return true;
    }

    /// @brief Invoke the Derived hook before visiting @p child.
    /// @details Override `Derived::beforeChild` to observe the parent/child relationship
    /// prior to recursively traversing the child node.
    /// @tparam Parent Parent node type.
    /// @tparam Child Child node type.
    /// @param parent Node that owns @p child.
    /// @param child Child about to be announced or traversed.
    template <typename Parent, typename Child>
    void callBeforeChild(const Parent &parent, const Child &child) {
        if constexpr (requires(Derived &d, const Parent &p, const Child &c) {
                          d.beforeChild(p, c);
                      })
            static_cast<Derived *>(this)->beforeChild(parent, child);
    }

    /// @brief Invoke the Derived hook after visiting @p child.
    /// @details Override `Derived::afterChild` to perform post-processing that requires
    /// both the parent context and the just-visited child node.
    /// @tparam Parent Parent node type.
    /// @tparam Child Child node type.
    /// @param parent Node that owns @p child.
    /// @param child Child that was just announced or traversed.
    template <typename Parent, typename Child>
    void callAfterChild(const Parent &parent, const Child &child) {
        if constexpr (requires(Derived &d, const Parent &p, const Child &c) { d.afterChild(p, c); })
            static_cast<Derived *>(this)->afterChild(parent, child);
    }

    // Expression visitors --------------------------------------------------

    /// @brief Visit an integer literal, which has no child nodes.
    /// @param expr Literal bracketed by before/after hooks.
    void visit(const IntExpr &expr) override {
        callBefore(expr);
        callAfter(expr);
    }

    /// @brief Visit a floating-point literal, which has no child nodes.
    /// @param expr Literal bracketed by before/after hooks.
    void visit(const FloatExpr &expr) override {
        callBefore(expr);
        callAfter(expr);
    }

    /// @brief Visit a string literal, which has no child nodes.
    /// @param expr Literal bracketed by before/after hooks.
    void visit(const StringExpr &expr) override {
        callBefore(expr);
        callAfter(expr);
    }

    /// @brief Visit a Boolean literal, which has no child nodes.
    /// @param expr Literal bracketed by before/after hooks.
    void visit(const BoolExpr &expr) override {
        callBefore(expr);
        callAfter(expr);
    }

    /// @brief Visit a variable reference, which has no child nodes.
    /// @param expr Reference bracketed by before/after hooks.
    void visit(const VarExpr &expr) override {
        callBefore(expr);
        callAfter(expr);
    }

    /// @brief Visit an array reference and its index expressions.
    /// @details Traverses the legacy single @c index when present; otherwise
    ///          traverses non-null entries in @c indices in stored order.
    /// @param expr Array reference bracketed by node hooks.
    void visit(const ArrayExpr &expr) override {
        callBefore(expr);
        if (callShouldVisit(expr)) {
            // For backward compatibility: visit 'index' for single-dim, 'indices' for multi-dim
            if (expr.index) {
                // Single-dimensional array: visit deprecated 'index' field
                walker::detail::visitOptionalChild(*this, expr, expr.index);
            } else {
                // Multi-dimensional array: visit all indices in 'indices' vector
                walker::detail::visitChildRange(*this, expr, expr.indices);
            }
        }
        callAfter(expr);
    }

    /// @brief Visit an LBOUND expression and its optional dimension.
    /// @param expr Bound query bracketed by node hooks.
    void visit(const LBoundExpr &expr) override {
        callBefore(expr);
        if (callShouldVisit(expr))
            walker::detail::visitOptionalChild(*this, expr, expr.dimension);
        callAfter(expr);
    }

    /// @brief Visit a UBOUND expression and its optional dimension.
    /// @param expr Bound query bracketed by node hooks.
    void visit(const UBoundExpr &expr) override {
        callBefore(expr);
        if (callShouldVisit(expr))
            walker::detail::visitOptionalChild(*this, expr, expr.dimension);
        callAfter(expr);
    }

    /// @brief Visit a unary expression and then its optional operand.
    /// @param expr Unary node bracketed by node hooks.
    void visit(const UnaryExpr &expr) override {
        callBefore(expr);
        if (callShouldVisit(expr)) {
            walker::detail::visitOptionalChild(*this, expr, expr.expr);
        }
        callAfter(expr);
    }

    /// @brief Visit a binary expression's left operand followed by its right.
    /// @param expr Binary node bracketed by node hooks.
    void visit(const BinaryExpr &expr) override {
        callBefore(expr);
        if (callShouldVisit(expr)) {
            walker::detail::visitOptionalChild(*this, expr, expr.lhs);
            walker::detail::visitOptionalChild(*this, expr, expr.rhs);
        }
        callAfter(expr);
    }

    /// @brief Visit builtin-call arguments in stored order.
    /// @param expr Builtin call bracketed by node hooks.
    void visit(const BuiltinCallExpr &expr) override {
        callBefore(expr);
        if (callShouldVisit(expr)) {
            walker::detail::visitChildRange(*this, expr, expr.args);
        }
        callAfter(expr);
    }

    /// @brief Visit user-call arguments in stored order.
    /// @param expr Call expression bracketed by node hooks.
    void visit(const CallExpr &expr) override {
        callBefore(expr);
        if (callShouldVisit(expr)) {
            walker::detail::visitChildRange(*this, expr, expr.args);
        }
        callAfter(expr);
    }

    /// @brief Visit object-construction arguments in stored order.
    /// @param expr Construction expression bracketed by node hooks.
    void visit(const NewExpr &expr) override {
        callBefore(expr);
        if (callShouldVisit(expr)) {
            walker::detail::visitChildRange(*this, expr, expr.args);
        }
        callAfter(expr);
    }

    /// @brief Visit the current-instance expression, which has no children.
    /// @param expr ME expression bracketed by before/after hooks.
    void visit(const MeExpr &expr) override {
        callBefore(expr);
        callAfter(expr);
    }

    /// @brief Visit a member access and its optional base expression.
    /// @param expr Member access bracketed by node hooks.
    void visit(const MemberAccessExpr &expr) override {
        callBefore(expr);
        if (callShouldVisit(expr)) {
            walker::detail::visitOptionalChild(*this, expr, expr.base);
        }
        callAfter(expr);
    }

    /// @brief Visit a method receiver followed by its arguments.
    /// @param expr Method call bracketed by node hooks.
    void visit(const MethodCallExpr &expr) override {
        callBefore(expr);
        if (callShouldVisit(expr)) {
            walker::detail::visitOptionalChild(*this, expr, expr.base);
            walker::detail::visitChildRange(*this, expr, expr.args);
        }
        callAfter(expr);
    }

    /// @brief Visit an IS type test and its optional value expression.
    /// @param expr Type-test expression bracketed by node hooks.
    void visit(const IsExpr &expr) override {
        callBefore(expr);
        if (callShouldVisit(expr)) {
            walker::detail::visitOptionalChild(*this, expr, expr.value);
        }
        callAfter(expr);
    }

    /// @brief Visit an AS cast and its optional value expression.
    /// @param expr Cast expression bracketed by node hooks.
    void visit(const AsExpr &expr) override {
        callBefore(expr);
        if (callShouldVisit(expr)) {
            walker::detail::visitOptionalChild(*this, expr, expr.value);
        }
        callAfter(expr);
    }

    /// @brief Visit an ADDRESSOF node, whose target is a name rather than a child.
    /// @param expr Procedure-address expression bracketed by node hooks.
    void visit(const AddressOfExpr &expr) override {
        callBefore(expr);
        // ADDRESSOF has no child expressions; the target is just a name string.
        callAfter(expr);
    }

    // Statement visitors ---------------------------------------------------

    /// @brief Visit a label statement, which has no child AST nodes.
    /// @param stmt Label bracketed by before/after hooks.
    void visit(const LabelStmt &stmt) override {
        callBefore(stmt);
        callAfter(stmt);
    }

    /// @brief Visit expression-bearing PRINT items in source order.
    /// @param stmt PRINT statement bracketed by node hooks.
    void visit(const PrintStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitPrintItems(*this, stmt);
        }
        callAfter(stmt);
    }

    /// @brief Visit a PRINT# channel expression followed by its arguments.
    /// @param stmt Channel-print statement bracketed by node hooks.
    void visit(const PrintChStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.channelExpr);
            walker::detail::visitChildRange(*this, stmt, stmt.args);
        }
        callAfter(stmt);
    }

    /// @brief Visit a BEEP statement, which has no child AST nodes.
    /// @param stmt BEEP statement bracketed by before/after hooks.
    void visit(const BeepStmt &stmt) override {
        callBefore(stmt);
        // BEEP has no child expressions.
        callAfter(stmt);
    }

    /// @brief Visit a CALL statement's optional call expression.
    /// @param stmt CALL statement bracketed by node hooks.
    void visit(const CallStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.call);
        }
        callAfter(stmt);
    }

    /// @brief Visit a CLS statement, which has no child AST nodes.
    /// @param stmt CLS statement bracketed by node hooks.
    void visit(const ClsStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            // CLS has no child expressions.
        }
        callAfter(stmt);
    }

    /// @brief Visit optional COLOR foreground and background expressions.
    /// @param stmt COLOR statement bracketed by node hooks.
    void visit(const ColorStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.fg);
            walker::detail::visitOptionalChild(*this, stmt, stmt.bg);
        }
        callAfter(stmt);
    }

    /// @brief Visit a SLEEP statement's optional duration expression.
    /// @param stmt SLEEP statement bracketed by node hooks.
    void visit(const SleepStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.ms);
        }
        callAfter(stmt);
    }

    /// @brief Visit LOCATE row followed by column when present.
    /// @param stmt LOCATE statement bracketed by node hooks.
    void visit(const LocateStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.row);
            walker::detail::visitOptionalChild(*this, stmt, stmt.col);
        }
        callAfter(stmt);
    }

    /// @brief Visit a CURSOR statement, which has no child AST nodes.
    /// @param stmt CURSOR statement bracketed by before/after hooks.
    void visit(const CursorStmt &stmt) override {
        callBefore(stmt);
        callAfter(stmt);
    }

    /// @brief Visit an ALTSCREEN statement, which has no child AST nodes.
    /// @param stmt ALTSCREEN statement bracketed by before/after hooks.
    void visit(const AltScreenStmt &stmt) override {
        callBefore(stmt);
        callAfter(stmt);
    }

    /// @brief Visit LET target followed by assigned expression.
    /// @param stmt Assignment statement bracketed by node hooks.
    void visit(const LetStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.target);
            walker::detail::visitOptionalChild(*this, stmt, stmt.expr);
        }
        callAfter(stmt);
    }

    /// @brief Visit a CONST declaration's optional initializer.
    /// @param stmt Constant declaration bracketed by node hooks.
    void visit(const ConstStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.initializer);
        }
        callAfter(stmt);
    }

    /// @brief Visit DIM's legacy size followed by all dimension expressions.
    /// @param stmt Variable declaration bracketed by node hooks.
    void visit(const DimStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.size);
            walker::detail::visitChildRange(*this, stmt, stmt.dimensions);
        }
        callAfter(stmt);
    }

    /// @brief Visit a STATIC declaration, which has no child AST nodes.
    /// @param stmt Static declaration bracketed by before/after hooks.
    void visit(const StaticStmt &stmt) override {
        callBefore(stmt);
        callAfter(stmt);
    }

    /// @brief Visit a SHARED declaration, which stores names rather than nodes.
    /// @param stmt Shared declaration bracketed by before/after hooks.
    void visit(const SharedStmt &stmt) override {
        callBefore(stmt);
        callAfter(stmt);
    }

    /// @brief Visit a REDIM statement's optional size expression.
    /// @param stmt Array-resize statement bracketed by node hooks.
    void visit(const ReDimStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.size);
        }
        callAfter(stmt);
    }

    /// @brief Visit a RANDOMIZE statement's optional seed.
    /// @param stmt RANDOMIZE statement bracketed by node hooks.
    void visit(const RandomizeStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.seed);
        }
        callAfter(stmt);
    }

    /// @brief Visit SWAP's left operand followed by its right operand.
    /// @param stmt SWAP statement bracketed by node hooks.
    void visit(const SwapStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.lhs);
            walker::detail::visitOptionalChild(*this, stmt, stmt.rhs);
        }
        callAfter(stmt);
    }

    /// @brief Visit an IF condition and branches in source control-flow order.
    /// @details Traverses the main condition and then branch, each ELSEIF
    ///          condition and branch in stored order, and finally the optional
    ///          ELSE branch.
    /// @param stmt Conditional statement bracketed by node hooks.
    void visit(const IfStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.cond);
            walker::detail::visitOptionalChild(*this, stmt, stmt.then_branch);
            for (const auto &elseif : stmt.elseifs) {
                walker::detail::visitOptionalChild(*this, stmt, elseif.cond);
                walker::detail::visitOptionalChild(*this, stmt, elseif.then_branch);
            }
            walker::detail::visitOptionalChild(*this, stmt, stmt.else_branch);
        }
        callAfter(stmt);
    }

    /// @brief Visit SELECT's selector, arm bodies, and ELSE body.
    /// @details Arm labels, ranges, and relational guards are scalar metadata,
    ///          so only each arm body contributes child AST nodes.
    /// @param stmt SELECT CASE statement bracketed by node hooks.
    void visit(const SelectCaseStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.selector);
            for (const auto &arm : stmt.arms) {
                walker::detail::visitChildRange(*this, stmt, arm.body);
            }
            walker::detail::visitChildRange(*this, stmt, stmt.elseBody);
        }
        callAfter(stmt);
    }

    /// @brief Visit TRY body statements followed by CATCH body statements.
    /// @details This implementation does not traverse @c finallyBody.
    /// @param stmt Exception-handling statement bracketed by node hooks.
    void visit(const TryCatchStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitChildRange(*this, stmt, stmt.tryBody);
            walker::detail::visitChildRange(*this, stmt, stmt.catchBody);
        }
        callAfter(stmt);
    }

    /// @brief Visit a USING initializer followed by its statement body.
    /// @param stmt Resource-scope statement bracketed by node hooks.
    void visit(const UsingStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.initExpr);
            walker::detail::visitChildRange(*this, stmt, stmt.body);
        }
        callAfter(stmt);
    }

    /// @brief Visit a WHILE condition followed by its body statements.
    /// @param stmt WHILE loop bracketed by node hooks.
    void visit(const WhileStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.cond);
            walker::detail::visitChildRange(*this, stmt, stmt.body);
        }
        callAfter(stmt);
    }

    /// @brief Visit a DO condition followed by its body statements.
    /// @param stmt DO loop bracketed by node hooks.
    void visit(const DoStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.cond);
            walker::detail::visitChildRange(*this, stmt, stmt.body);
        }
        callAfter(stmt);
    }

    /// @brief Visit FOR start, end, step, and body in that order.
    /// @param stmt FOR loop bracketed by node hooks.
    void visit(const ForStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.start);
            walker::detail::visitOptionalChild(*this, stmt, stmt.end);
            walker::detail::visitOptionalChild(*this, stmt, stmt.step);
            walker::detail::visitChildRange(*this, stmt, stmt.body);
        }
        callAfter(stmt);
    }

    /// @brief Visit a FOR EACH body; element and collection are stored as names.
    /// @param stmt FOR EACH loop bracketed by node hooks.
    void visit(const ForEachStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitChildRange(*this, stmt, stmt.body);
        }
        callAfter(stmt);
    }

    /// @brief Visit a NEXT statement, which has no child AST nodes.
    /// @param stmt NEXT statement bracketed by before/after hooks.
    void visit(const NextStmt &stmt) override {
        callBefore(stmt);
        callAfter(stmt);
    }

    /// @brief Visit an EXIT statement, which has no child AST nodes.
    /// @param stmt EXIT statement bracketed by before/after hooks.
    void visit(const ExitStmt &stmt) override {
        callBefore(stmt);
        callAfter(stmt);
    }

    /// @brief Visit a GOTO statement, whose destination is stored as a label.
    /// @param stmt GOTO statement bracketed by before/after hooks.
    void visit(const GotoStmt &stmt) override {
        callBefore(stmt);
        callAfter(stmt);
    }

    /// @brief Visit a GOSUB statement, whose destination is stored as a label.
    /// @param stmt GOSUB statement bracketed by before/after hooks.
    void visit(const GosubStmt &stmt) override {
        callBefore(stmt);
        callAfter(stmt);
    }

    /// @brief Visit an OPEN path followed by its channel expression.
    /// @param stmt File-open statement bracketed by node hooks.
    void visit(const OpenStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.pathExpr);
            walker::detail::visitOptionalChild(*this, stmt, stmt.channelExpr);
        }
        callAfter(stmt);
    }

    /// @brief Visit a CLOSE statement's optional channel expression.
    /// @param stmt File-close statement bracketed by node hooks.
    void visit(const CloseStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.channelExpr);
        }
        callAfter(stmt);
    }

    /// @brief Visit SEEK's channel followed by position expression.
    /// @param stmt File-position statement bracketed by node hooks.
    void visit(const SeekStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.channelExpr);
            walker::detail::visitOptionalChild(*this, stmt, stmt.positionExpr);
        }
        callAfter(stmt);
    }

    /// @brief Visit ON ERROR GOTO, whose target is stored as a label.
    /// @param stmt Error-handler statement bracketed by before/after hooks.
    void visit(const OnErrorGoto &stmt) override {
        callBefore(stmt);
        callAfter(stmt);
    }

    /// @brief Visit RESUME, whose mode and target are non-node metadata.
    /// @param stmt Resume statement bracketed by before/after hooks.
    void visit(const Resume &stmt) override {
        callBefore(stmt);
        callAfter(stmt);
    }

    /// @brief Visit END, which has no child AST nodes.
    /// @param stmt END statement bracketed by before/after hooks.
    void visit(const EndStmt &stmt) override {
        callBefore(stmt);
        callAfter(stmt);
    }

    /// @brief Visit an INPUT statement's optional prompt expression.
    /// @param stmt Console-input statement bracketed by node hooks.
    void visit(const InputStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.prompt);
        }
        callAfter(stmt);
    }

    /// @brief Visit INPUT#, which stores channel and destinations as metadata.
    /// @param stmt Channel-input statement bracketed by before/after hooks.
    void visit(const InputChStmt &stmt) override {
        callBefore(stmt);
        callAfter(stmt);
    }

    /// @brief Visit LINE INPUT# channel followed by target expression.
    /// @param stmt Line-input statement bracketed by node hooks.
    void visit(const LineInputChStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.channelExpr);
            walker::detail::visitOptionalChild(*this, stmt, stmt.targetVar);
        }
        callAfter(stmt);
    }

    /// @brief Visit a RETURN statement's optional value expression.
    /// @param stmt RETURN statement bracketed by node hooks.
    void visit(const ReturnStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.value);
        }
        callAfter(stmt);
    }

    /// @brief Visit a function declaration's body statements in order.
    /// @details Parameter declarations are not announced by this overload.
    /// @param stmt Function declaration bracketed by node hooks.
    void visit(const FunctionDecl &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitChildRange(*this, stmt, stmt.body);
        }
        callAfter(stmt);
    }

    /// @brief Visit a subroutine declaration's body statements in order.
    /// @details Parameter declarations are not announced by this overload.
    /// @param stmt Subroutine declaration bracketed by node hooks.
    void visit(const SubDecl &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitChildRange(*this, stmt, stmt.body);
        }
        callAfter(stmt);
    }

    /// @brief Visit a DELETE statement's optional target expression.
    /// @param stmt DELETE statement bracketed by node hooks.
    void visit(const DeleteStmt &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitOptionalChild(*this, stmt, stmt.target);
        }
        callAfter(stmt);
    }

    /// @brief Announce constructor parameters, then visit body statements.
    /// @details Parameters receive beforeChild/afterChild notifications without
    ///          recursive accept calls.
    /// @param stmt Constructor declaration bracketed by node hooks.
    void visit(const ConstructorDecl &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::notifyChildRange(*this, stmt, stmt.params);
            walker::detail::visitChildRange(*this, stmt, stmt.body);
        }
        callAfter(stmt);
    }

    /// @brief Visit a destructor declaration's body statements.
    /// @param stmt Destructor declaration bracketed by node hooks.
    void visit(const DestructorDecl &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitChildRange(*this, stmt, stmt.body);
        }
        callAfter(stmt);
    }

    /// @brief Announce method parameters, then visit body statements.
    /// @details Parameters receive beforeChild/afterChild notifications without
    ///          recursive accept calls.
    /// @param stmt Method declaration bracketed by node hooks.
    void visit(const MethodDecl &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::notifyChildRange(*this, stmt, stmt.params);
            walker::detail::visitChildRange(*this, stmt, stmt.body);
        }
        callAfter(stmt);
    }

    /// @brief Visit class members in their stored declaration order.
    /// @param stmt Class declaration bracketed by node hooks.
    void visit(const ClassDecl &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitChildRange(*this, stmt, stmt.members);
        }
        callAfter(stmt);
    }

    /// @brief Visit a TYPE declaration without recursing into simple fields.
    /// @param stmt Type declaration bracketed by node hooks.
    void visit(const TypeDecl &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            // TYPE fields are simple declarations without nested AST nodes.
        }
        callAfter(stmt);
    }

    /// @brief Visit an ENUM declaration without recursing into constant members.
    /// @param stmt Enumeration declaration bracketed by node hooks.
    void visit(const EnumDecl &stmt) override {
        callBefore(stmt);
        // ENUM members are compile-time constants without nested AST nodes.
        callAfter(stmt);
    }

    /// @brief Visit interface members in their stored declaration order.
    /// @param stmt Interface declaration bracketed by node hooks.
    void visit(const InterfaceDecl &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitChildRange(*this, stmt, stmt.members);
        }
        callAfter(stmt);
    }

    /// @brief Visit a USING declaration, whose namespace path is metadata.
    /// @param stmt Namespace-import declaration bracketed by node hooks.
    void visit(const UsingDecl &stmt) override {
        callBefore(stmt);
        // USING has no child statements or expressions to visit.
        callAfter(stmt);
    }

    /// @brief Visit every non-null statement in a sequence.
    /// @param stmt Statement list bracketed by node hooks.
    void visit(const StmtList &stmt) override {
        callBefore(stmt);
        if (callShouldVisit(stmt)) {
            walker::detail::visitChildRange(*this, stmt, stmt.stmts);
        }
        callAfter(stmt);
    }
};

} // namespace il::frontends::basic
