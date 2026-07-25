//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ConstFolder.cpp
// Purpose: Implement table-driven literal folding and supported AST traversal
//          for the BASIC frontend.
// Ownership/Lifetime: Replacements transfer through unique_ptr slots owned by
//                     the caller's Program.
// Links: src/frontends/basic/ConstFolder.hpp,
//        src/frontends/basic/constfold/Dispatch.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Provides constant folding utilities for BASIC AST nodes.
/// @details The helpers in this translation unit evaluate expression trees built
///          from literal operands.  They promote operands according to BASIC's
///          suffix rules, consult rule tables for arithmetic and comparison
///          folding, and replace AST nodes with canonical literals when
///          evaluation succeeds.  Out-of-line definitions keep the header light
///          while exposing rich documentation for each folding primitive.

#include "frontends/basic/ConstFolder.hpp"
#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/constfold/Dispatch.hpp"

#include <array>
#include <optional>
#include <utility>

namespace il::frontends::basic {
namespace cf = il::frontends::basic::constfold;

namespace {

/// @brief AST visitor that performs in-place constant folding for BASIC.
/// @details Traverses expressions and statements, eagerly rewriting literal
///          subtrees into canonical nodes.  The pass threads context via member
///          pointers so nested visits can replace the current expression or
///          statement without returning large structures.
/// @invariant currentExpr_ and currentStmt_ point to the active owning slots
///            only during their corresponding recursive dispatch.
class ConstFolderPass : public MutExprVisitor, public MutStmtVisitor {
  public:
    /// @brief Fold all procedures and top-level statements in a program.
    /// @details Dispatches each entry in @c prog.procs and then @c prog.main.
    ///          FunctionDecl and SubDecl visitors are currently no-ops, while
    ///          supported declaration and main-statement visitors recurse as
    ///          documented below.
    /// @param prog Program whose AST will be mutated in place.
    void run(Program &prog) {
        for (auto &decl : prog.procs)
            foldStmt(decl);
        for (auto &stmt : prog.main)
            foldStmt(stmt);
    }

  private:
    /// @brief Recursively fold an expression tree and update the current slot.
    /// @param expr Expression pointer reference that may be replaced with a literal.
    void foldExpr(ExprPtr &expr) {
        if (!expr)
            return;
        ExprPtr *prev = currentExpr_;
        currentExpr_ = &expr;
        expr->accept(*this);
        currentExpr_ = prev;
    }

    /// @brief Recursively fold a statement subtree.
    /// @param stmt Statement pointer reference that may be rewritten.
    void foldStmt(StmtPtr &stmt) {
        if (!stmt)
            return;
        StmtPtr *prev = currentStmt_;
        currentStmt_ = &stmt;
        stmt->accept(*this);
        currentStmt_ = prev;
    }

    /// @brief Access the expression slot currently being rewritten.
    /// @return Reference to the pointer tracked by the visitor.
    /// @pre Called only while foldExpr() has installed a non-null currentExpr_.
    ExprPtr &exprSlot() {
        return *currentExpr_;
    }

    /// @brief Replace the active expression with an integer literal node.
    /// @param v Integer value assigned to the replacement literal.
    /// @param loc Source location propagated to the new node.
    void replaceWithInt(long long v, il::support::SourceLoc loc) {
        exprSlot() = makeIntExpr(v, loc);
    }

    /// @brief Replace the active expression with a boolean literal node.
    /// @param v Boolean value assigned to the replacement literal.
    /// @param loc Source location propagated to the new node.
    void replaceWithBool(bool v, il::support::SourceLoc loc) {
        exprSlot() = makeBoolExpr(v, loc);
    }

    /// @brief Replace the active expression with a string literal node.
    /// @param s String payload for the replacement literal.
    /// @param loc Source location propagated to the new node.
    void replaceWithStr(std::string_view s, il::support::SourceLoc loc) {
        exprSlot() = makeStrExpr(s, loc);
    }

    /// @brief Replace the active expression with a floating-point literal node.
    /// @param v Floating-point value assigned to the replacement literal.
    /// @param loc Source location propagated to the new node.
    void replaceWithFloat(double v, il::support::SourceLoc loc) {
        exprSlot() = makeFloatExpr(v, loc);
    }

    /// @brief Replace the active expression with an arbitrary expression node.
    /// @param replacement Newly constructed expression that becomes current.
    void replaceWithExpr(ExprPtr replacement) {
        exprSlot() = std::move(replacement);
    }

    /// @brief Fold LEN builtin calls when the argument is a literal.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a constant.
    bool tryFoldLen(BuiltinCallExpr &expr) {
        if (expr.args.size() != 1 || !expr.args[0])
            return false;
        if (auto folded = cf::foldLenLiteral(*expr.args[0])) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    /// @brief Fold MID builtin calls when all operands are literal.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a constant.
    bool tryFoldMid(BuiltinCallExpr &expr) {
        if (expr.args.size() != 3 || !expr.args[0] || !expr.args[1] || !expr.args[2])
            return false;
        if (auto folded = cf::foldMidLiteral(*expr.args[0], *expr.args[1], *expr.args[2])) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    /// @brief Fold LEFT builtin calls when both operands are literal.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a constant.
    bool tryFoldLeft(BuiltinCallExpr &expr) {
        if (expr.args.size() != 2 || !expr.args[0] || !expr.args[1])
            return false;
        if (auto folded = cf::foldLeftLiteral(*expr.args[0], *expr.args[1])) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    /// @brief Fold RIGHT builtin calls when both operands are literal.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a constant.
    bool tryFoldRight(BuiltinCallExpr &expr) {
        if (expr.args.size() != 2 || !expr.args[0] || !expr.args[1])
            return false;
        if (auto folded = cf::foldRightLiteral(*expr.args[0], *expr.args[1])) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    /// @brief Fold VAL builtin calls when the argument is a literal string.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a numeric constant.
    bool tryFoldVal(BuiltinCallExpr &expr) {
        if (expr.args.size() != 1 || !expr.args[0])
            return false;
        if (auto folded = cf::foldValLiteral(*expr.args[0])) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    /// @brief Fold CINT builtin calls for literal numeric arguments.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a constant.
    bool tryFoldCInt(BuiltinCallExpr &expr) {
        if (expr.args.size() != 1 || !expr.args[0])
            return false;
        if (auto folded = cf::foldCIntLiteral(*expr.args[0])) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    /// @brief Fold CLNG builtin calls for literal numeric arguments.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a constant.
    bool tryFoldCLng(BuiltinCallExpr &expr) {
        if (expr.args.size() != 1 || !expr.args[0])
            return false;
        if (auto folded = cf::foldCLngLiteral(*expr.args[0])) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    /// @brief Fold CSNG builtin calls for literal numeric arguments.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a constant.
    bool tryFoldCSng(BuiltinCallExpr &expr) {
        if (expr.args.size() != 1 || !expr.args[0])
            return false;
        if (auto folded = cf::foldCSngLiteral(*expr.args[0])) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    /// @brief Fold CDBL builtin calls for literal numeric arguments.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a constant.
    bool tryFoldCDbl(BuiltinCallExpr &expr) {
        if (expr.args.size() != 1 || !expr.args[0])
            return false;
        if (auto folded = cf::foldCDblLiteral(*expr.args[0])) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    /// @brief Fold INT builtin calls for literal numeric arguments.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a constant.
    bool tryFoldInt(BuiltinCallExpr &expr) {
        if (expr.args.size() != 1 || !expr.args[0])
            return false;
        if (auto folded = cf::foldIntLiteral(*expr.args[0])) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    /// @brief Fold FIX builtin calls for literal numeric arguments.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a constant.
    bool tryFoldFix(BuiltinCallExpr &expr) {
        if (expr.args.size() != 1 || !expr.args[0])
            return false;
        if (auto folded = cf::foldFixLiteral(*expr.args[0])) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    /// @brief Fold ROUND builtin calls when arguments are literal.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a rounded constant.
    bool tryFoldRound(BuiltinCallExpr &expr) {
        if (expr.args.empty() || !expr.args[0])
            return false;
        const Expr *digits = (expr.args.size() >= 2 && expr.args[1]) ? expr.args[1].get() : nullptr;
        if (auto folded = cf::foldRoundLiteral(*expr.args[0], digits)) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    /// @brief Fold STR builtin calls when the argument is literal numeric.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a string literal.
    bool tryFoldStr(BuiltinCallExpr &expr) {
        if (expr.args.size() != 1 || !expr.args[0])
            return false;
        if (auto folded = cf::foldStrLiteral(*expr.args[0])) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    /// @brief Fold CHR$ builtin calls when the argument is a literal integer.
    /// @param expr Builtin call expression being visited.
    /// @return @c true when the expression was replaced with a string literal.
    bool tryFoldChr(BuiltinCallExpr &expr) {
        if (expr.args.size() != 1 || !expr.args[0])
            return false;
        if (auto folded = cf::foldChrLiteral(*expr.args[0])) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return true;
        }
        return false;
    }

    struct BuiltinDispatchEntry {
        ///< Builtin enumerator matched by this dispatch row.
        BuiltinCallExpr::Builtin builtin;
        ///< Member handler invoked for a matching call.
        bool (ConstFolderPass::*folder)(BuiltinCallExpr &);
    };

    /// Supported pure-builtin folders searched in declaration order.
    static constexpr std::array<BuiltinDispatchEntry, 14> kBuiltinDispatch{{
        {BuiltinCallExpr::Builtin::Len, &ConstFolderPass::tryFoldLen},
        {BuiltinCallExpr::Builtin::Mid, &ConstFolderPass::tryFoldMid},
        {BuiltinCallExpr::Builtin::Left, &ConstFolderPass::tryFoldLeft},
        {BuiltinCallExpr::Builtin::Right, &ConstFolderPass::tryFoldRight},
        {BuiltinCallExpr::Builtin::Val, &ConstFolderPass::tryFoldVal},
        {BuiltinCallExpr::Builtin::Cint, &ConstFolderPass::tryFoldCInt},
        {BuiltinCallExpr::Builtin::Clng, &ConstFolderPass::tryFoldCLng},
        {BuiltinCallExpr::Builtin::Csng, &ConstFolderPass::tryFoldCSng},
        {BuiltinCallExpr::Builtin::Cdbl, &ConstFolderPass::tryFoldCDbl},
        {BuiltinCallExpr::Builtin::Int, &ConstFolderPass::tryFoldInt},
        {BuiltinCallExpr::Builtin::Fix, &ConstFolderPass::tryFoldFix},
        {BuiltinCallExpr::Builtin::Round, &ConstFolderPass::tryFoldRound},
        {BuiltinCallExpr::Builtin::Str, &ConstFolderPass::tryFoldStr},
        {BuiltinCallExpr::Builtin::Chr, &ConstFolderPass::tryFoldChr},
    }};

    // MutExprVisitor overrides ----------------------------------------------
    /// @brief Literals are already canonical, so integer nodes are left untouched.
    void visit(IntExpr &) override {}

    /// @brief Floating literals require no rewriting beyond their existing value.
    void visit(FloatExpr &) override {}

    /// @brief String literals are already canonical and therefore skipped.
    void visit(StringExpr &) override {}

    /// @brief Boolean literals are already canonical and therefore skipped.
    void visit(BoolExpr &) override {}

    /// @brief Variable references cannot be folded directly.
    void visit(VarExpr &) override {}

    /// @brief Fold the legacy single-dimensional array index when present.
    /// @details The @c indices vector used by multidimensional references is
    ///          not traversed by this handler.
    /// @param expr Array access whose legacy index slot may be rewritten.
    void visit(ArrayExpr &expr) override {
        foldExpr(expr.index);
    }

    /// @brief Lower bound queries are left untouched because they resolve at runtime.
    /// @param expr Query whose optional dimension expression is folded.
    void visit(LBoundExpr &expr) override {
        foldExpr(expr.dimension);
    }

    /// @brief Upper bound queries are left untouched because they resolve at runtime.
    /// @param expr Query whose optional dimension expression is folded.
    void visit(UBoundExpr &expr) override {
        foldExpr(expr.dimension);
    }

    /// @brief Fold unary operations when the operand collapses to a literal.
    /// @param expr Unary node whose operand and operation may be replaced.
    void visit(UnaryExpr &expr) override {
        foldExpr(expr.expr);
        switch (expr.op) {
            case UnaryExpr::Op::LogicalNot:
                if (auto replacement = cf::fold_logical_not(*expr.expr)) {
                    replacement->loc = expr.loc;
                    replaceWithExpr(std::move(replacement));
                }
                break;
            case UnaryExpr::Op::Plus:
            case UnaryExpr::Op::Negate:
                if (auto replacement = cf::fold_unary_arith(expr.op, *expr.expr)) {
                    replacement->loc = expr.loc;
                    replaceWithExpr(std::move(replacement));
                }
                break;
        }
    }

    /// @brief Fold binary operations by evaluating literal operands and applying shortcuts.
    /// @param expr Binary node folded left-first with short-circuit handling.
    void visit(BinaryExpr &expr) override {
        foldExpr(expr.lhs);

        if (auto *lhsBool = as<BoolExpr>(*expr.lhs)) {
            if (auto shortCircuit = cf::try_short_circuit(expr.op, *lhsBool)) {
                replaceWithBool(*shortCircuit, expr.loc);
                return;
            }

            if (cf::is_short_circuit(expr.op)) {
                ExprPtr rhs = std::move(expr.rhs);
                foldExpr(rhs);
                if (auto folded = cf::fold_boolean_binary(*lhsBool, expr.op, *rhs)) {
                    folded->loc = expr.loc;
                    replaceWithExpr(std::move(folded));
                } else {
                    replaceWithExpr(std::move(rhs));
                }
                return;
            }
        }

        foldExpr(expr.rhs);

        if (auto folded = cf::fold_boolean_binary(*expr.lhs, expr.op, *expr.rhs)) {
            folded->loc = expr.loc;
            replaceWithExpr(std::move(folded));
            return;
        }

        if (auto folded = cf::fold_expr(expr)) {
            (*folded)->loc = expr.loc;
            replaceWithExpr(std::move(*folded));
        }
    }

    /// @brief Fold builtin function calls whose arguments are literal values.
    /// @param expr Builtin call whose arguments are folded before dispatch.
    void visit(BuiltinCallExpr &expr) override {
        for (auto &arg : expr.args)
            foldExpr(arg);

        for (const auto &entry : kBuiltinDispatch) {
            if (entry.builtin == expr.builtin) {
                if ((this->*entry.folder)(expr))
                    return;
                break;
            }
        }
    }

    /// @brief User-defined calls are not folded because they may have side effects.
    void visit(CallExpr &) override {}

    /// @brief Fold constructor arguments to expose more literal values downstream.
    /// @param expr Construction expression whose arguments are visited in order.
    void visit(NewExpr &expr) override {
        for (auto &arg : expr.args)
            foldExpr(arg);
    }

    /// @brief The ME pseudo-variable cannot be folded.
    void visit(MeExpr &) override {}

    /// @brief Fold the receiver of member accesses before further lowering.
    /// @param expr Member access whose optional base slot is folded.
    void visit(MemberAccessExpr &expr) override {
        foldExpr(expr.base);
    }

    /// @brief Fold the receiver and arguments of method invocations.
    /// @param expr Method call traversed receiver-first, then argument order.
    void visit(MethodCallExpr &expr) override {
        foldExpr(expr.base);
        for (auto &arg : expr.args)
            foldExpr(arg);
    }

    /// @brief Fold inside IS expression (value only; type is metadata).
    /// @param expr Type test whose value expression is folded.
    void visit(IsExpr &expr) override {
        foldExpr(expr.value);
    }

    /// @brief Fold inside AS expression (value only; type is metadata).
    /// @param expr Cast whose value expression is folded.
    void visit(AsExpr &expr) override {
        foldExpr(expr.value);
    }

    /// @brief ADDRESSOF has no sub-expressions to fold.
    void visit(AddressOfExpr &) override {}

    // MutStmtVisitor overrides ----------------------------------------------
    /// @brief Labels carry no expressions to fold.
    void visit(LabelStmt &) override {}

    /// @brief Fold expressions embedded in PRINT statement items.
    /// @param stmt PRINT statement whose expression-kind items are visited.
    void visit(PrintStmt &stmt) override {
        for (auto &item : stmt.items) {
            if (item.kind == PrintItem::Kind::Expr)
                foldExpr(item.expr);
        }
    }

    /// @brief Fold channel and argument expressions for PRINT # statements.
    /// @param stmt PRINT# statement traversed channel-first, then arguments.
    void visit(PrintChStmt &stmt) override {
        foldExpr(stmt.channelExpr);
        for (auto &arg : stmt.args)
            foldExpr(arg);
    }

    /// @brief BEEP has no foldable expressions.
    void visit(BeepStmt &) override {}

    /// @brief Fold arguments within CALL statements while leaving target intact.
    /// @param stmt CALL whose CallExpr or MethodCallExpr operands may be folded.
    void visit(CallStmt &stmt) override {
        if (!stmt.call)
            return;

        if (auto *ce = as<CallExpr>(*stmt.call)) {
            for (auto &arg : ce->args)
                foldExpr(arg);
            return;
        }

        if (auto *me = as<MethodCallExpr>(*stmt.call)) {
            if (me->base)
                foldExpr(me->base);
            for (auto &arg : me->args)
                foldExpr(arg);
            return;
        }
    }

    /// @brief CLS has no foldable expressions.
    void visit(ClsStmt &) override {}

    /// @brief CURSOR has no foldable expressions.
    void visit(CursorStmt &) override {}

    /// @brief ALTSCREEN has no foldable expressions.
    void visit(AltScreenStmt &) override {}

    /// @brief Fold the foreground/background expressions for COLOR statements.
    /// @param stmt COLOR statement traversed foreground-first, then background.
    void visit(ColorStmt &stmt) override {
        foldExpr(stmt.fg);
        foldExpr(stmt.bg);
    }

    /// @brief Fold the millisecond duration in SLEEP statements.
    /// @details The runtime clamps negatives; folding only simplifies literal
    ///          arithmetic in the duration expression, leaving semantics to lowering/runtime.
    /// @param stmt SLEEP statement whose optional duration is folded.
    void visit(SleepStmt &stmt) override {
        foldExpr(stmt.ms);
    }

    /// @brief Fold cursor position expressions for LOCATE statements.
    /// @param stmt LOCATE statement traversed row-first, then column.
    void visit(LocateStmt &stmt) override {
        foldExpr(stmt.row);
        foldExpr(stmt.col);
    }

    /// @brief Fold both the target and assigned expression in LET statements.
    /// @param stmt Assignment traversed target-first, then value.
    void visit(LetStmt &stmt) override {
        foldExpr(stmt.target);
        foldExpr(stmt.expr);
    }

    /// @brief Fold initializer expressions in CONST statements.
    /// @param stmt Constant declaration whose optional initializer is folded.
    void visit(ConstStmt &stmt) override {
        if (stmt.initializer)
            foldExpr(stmt.initializer);
    }

    /// @brief Fold array size expressions in DIM statements when present.
    /// @details Only the legacy @c size slot is traversed; @c dimensions is not.
    /// @param stmt DIM declaration inspected for an array size.
    void visit(DimStmt &stmt) override {
        if (stmt.isArray && stmt.size)
            foldExpr(stmt.size);
    }

    /// @brief STATIC variables have no expressions to fold.
    void visit(StaticStmt &) override {}

    /// @brief SHARED is a declaration-only compatibility statement; no folding.
    void visit(SharedStmt &) override {}

    /// @brief Fold new bounds in REDIM statements when present.
    /// @param stmt REDIM traversed legacy size-first, then all dimensions.
    void visit(ReDimStmt &stmt) override {
        if (stmt.size)
            foldExpr(stmt.size);
        for (auto &dim : stmt.dimensions)
            foldExpr(dim);
    }

    /// @brief Fold expressions in SWAP statements when present.
    /// @param stmt SWAP statement traversed left-first, then right.
    void visit(SwapStmt &stmt) override {
        if (stmt.lhs)
            foldExpr(stmt.lhs);
        if (stmt.rhs)
            foldExpr(stmt.rhs);
    }

    /// @brief Leave RANDOMIZE unchanged, including any stored seed expression.
    void visit(RandomizeStmt &) override {}

    /// @brief Fold predicates and branch bodies within IF statements.
    /// @param stmt IF traversed condition, THEN, ELSEIF pairs, then ELSE.
    void visit(IfStmt &stmt) override {
        foldExpr(stmt.cond);
        foldStmt(stmt.then_branch);
        for (auto &elseif : stmt.elseifs) {
            foldExpr(elseif.cond);
            foldStmt(elseif.then_branch);
        }
        foldStmt(stmt.else_branch);
    }

    /// @brief Fold selectors and arms inside SELECT CASE statements.
    /// @param stmt SELECT traversed selector, arm bodies, then ELSE body.
    void visit(SelectCaseStmt &stmt) override {
        foldExpr(stmt.selector);
        for (auto &arm : stmt.arms)
            for (auto &bodyStmt : arm.body)
                foldStmt(bodyStmt);
        for (auto &bodyStmt : stmt.elseBody)
            foldStmt(bodyStmt);
    }

    /// @brief Fold loop predicates and bodies for WHILE statements.
    /// @param stmt WHILE traversed condition-first, then body order.
    void visit(WhileStmt &stmt) override {
        foldExpr(stmt.cond);
        for (auto &bodyStmt : stmt.body)
            foldStmt(bodyStmt);
    }

    /// @brief Fold DO loop conditions (when present) and bodies.
    /// @param stmt DO traversed optional condition-first, then body order.
    void visit(DoStmt &stmt) override {
        if (stmt.cond)
            foldExpr(stmt.cond);
        for (auto &bodyStmt : stmt.body)
            foldStmt(bodyStmt);
    }

    /// @brief Fold range and body expressions for FOR loops.
    /// @param stmt FOR traversed start, end, optional step, then body.
    void visit(ForStmt &stmt) override {
        foldExpr(stmt.start);
        foldExpr(stmt.end);
        if (stmt.step)
            foldExpr(stmt.step);
        for (auto &bodyStmt : stmt.body)
            foldStmt(bodyStmt);
    }

    /// @brief Fold body expressions for FOR EACH loops.
    /// @param stmt FOR EACH whose body statements are traversed in order.
    void visit(ForEachStmt &stmt) override {
        for (auto &bodyStmt : stmt.body)
            foldStmt(bodyStmt);
    }

    /// @brief NEXT statements have no expressions to fold.
    void visit(NextStmt &) override {}

    /// @brief EXIT statements have no expressions to fold.
    void visit(ExitStmt &) override {}

    /// @brief GOTO statements have no expressions to fold.
    void visit(GotoStmt &) override {}

    /// @brief GOSUB statements have no expressions to fold.
    void visit(GosubStmt &) override {}

    /// @brief Fold OPEN statement operands such as path and channel.
    /// @param stmt OPEN traversed optional path-first, then channel.
    void visit(OpenStmt &stmt) override {
        if (stmt.pathExpr)
            foldExpr(stmt.pathExpr);
        if (stmt.channelExpr)
            foldExpr(stmt.channelExpr);
    }

    /// @brief Fold channel expressions in CLOSE statements.
    /// @param stmt CLOSE statement whose optional channel is folded.
    void visit(CloseStmt &stmt) override {
        if (stmt.channelExpr)
            foldExpr(stmt.channelExpr);
    }

    /// @brief Fold channel and offset expressions in SEEK statements.
    /// @param stmt SEEK traversed optional channel-first, then position.
    void visit(SeekStmt &stmt) override {
        if (stmt.channelExpr)
            foldExpr(stmt.channelExpr);
        if (stmt.positionExpr)
            foldExpr(stmt.positionExpr);
    }

    /// @brief ON ERROR GOTO contains no literal operands to fold.
    void visit(OnErrorGoto &) override {}

    /// @brief RESUME statements rely on runtime state and are left untouched.
    void visit(Resume &) override {}

    /// @brief END statements have no expressions to fold.
    void visit(EndStmt &) override {}

    /// @brief Fold prompts within INPUT statements when literal.
    /// @param stmt INPUT statement whose optional prompt is folded.
    void visit(InputStmt &stmt) override {
        if (stmt.prompt)
            foldExpr(stmt.prompt);
    }

    /// @brief INPUT # statements have no additional foldable expressions.
    void visit(InputChStmt &) override {}

    /// @brief Fold channel and destination expressions in LINE INPUT #.
    /// @param stmt LINE INPUT# traversed channel-first, then target.
    void visit(LineInputChStmt &stmt) override {
        foldExpr(stmt.channelExpr);
        foldExpr(stmt.targetVar);
    }

    /// @brief Leave RETURN unchanged, including any optional return value.
    void visit(ReturnStmt &) override {}

    /// @brief Leave FUNCTION declarations and their bodies unchanged.
    void visit(FunctionDecl &) override {}

    /// @brief Leave SUB declarations and their bodies unchanged.
    void visit(SubDecl &) override {}

    /// @brief Recursively fold every statement within a statement list.
    /// @param stmt Sequence whose children are traversed in stored order.
    void visit(StmtList &stmt) override {
        for (auto &child : stmt.stmts)
            foldStmt(child);
    }

    /// @brief Fold the target expression of DELETE statements.
    /// @param stmt DELETE statement whose target is folded.
    void visit(DeleteStmt &stmt) override {
        foldExpr(stmt.target);
    }

    /// @brief Fold the body statements of constructors.
    /// @param stmt Constructor whose body is traversed in order.
    void visit(ConstructorDecl &stmt) override {
        for (auto &bodyStmt : stmt.body)
            foldStmt(bodyStmt);
    }

    /// @brief Fold the body statements of destructors.
    /// @param stmt Destructor whose body is traversed in order.
    void visit(DestructorDecl &stmt) override {
        for (auto &bodyStmt : stmt.body)
            foldStmt(bodyStmt);
    }

    /// @brief Fold the body statements of method declarations.
    /// @param stmt Method whose body is traversed in order.
    void visit(MethodDecl &stmt) override {
        for (auto &bodyStmt : stmt.body)
            foldStmt(bodyStmt);
    }

    /// @brief Fold every member statement within class declarations.
    /// @param stmt Class whose members are traversed in order.
    void visit(ClassDecl &stmt) override {
        for (auto &member : stmt.members)
            foldStmt(member);
    }

    /// @brief TYPE declarations define shapes only and do not fold expressions.
    void visit(TypeDecl &) override {}

    /// @brief Fold members inside INTERFACE declarations.
    /// @param stmt Interface whose members are traversed in order.
    void visit(InterfaceDecl &stmt) override {
        for (auto &member : stmt.members)
            foldStmt(member);
    }

    /// @brief Visit USING directive (no-op for constant folding).
    /// @details USING is compile-time only and has no expressions to fold.
    void visit(UsingDecl &) override {}

    ///< Active owning expression slot, or null outside expression dispatch.
    ExprPtr *currentExpr_ = nullptr;
    ///< Active owning statement slot, or null outside statement dispatch.
    StmtPtr *currentStmt_ = nullptr;
};

} // namespace

/// @copydoc foldConstants()
void foldConstants(Program &prog) {
    ConstFolderPass pass;
    pass.run(prog);
}

} // namespace il::frontends::basic
