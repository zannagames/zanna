//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/lower/Lowerer_Stmt.cpp
// Purpose: Provides the BASIC statement visitor wiring that forwards AST nodes
//          into the shared Lowerer helpers.
// Key invariants: Statement visitors honour the active Lowerer context and never
//                 mutate AST ownership.
// Ownership/Lifetime: Operates on a borrowed Lowerer instance while the caller
//                     retains AST ownership.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the statement visitor and top-level statement lowering
///        entry points for the BASIC frontend.

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"
#include "frontends/basic/lower/AstVisitor.hpp"

#include "zanna/il/Module.hpp"

namespace il::frontends::basic {

/// @brief Visitor that lowers BASIC statements through the shared Lowerer.
/// @details Implements the generated @ref StmtVisitor interface and forwards
///          each concrete statement to the appropriate helper on
///          @ref Lowerer. The visitor borrows the lowering context so it can set
///          source locations and reuse shared facilities such as block
///          management and runtime call emission.
class LowererStmtVisitor final : public lower::AstVisitor, public StmtVisitor {
  public:
    /// @brief Construct a visitor that operates on @p lowerer.
    /// @details The visitor caches a reference to the lowering context and does
    ///          not take ownership of any AST nodes.
    /// @param lowerer Lowering context that provides statement helper methods.
    explicit LowererStmtVisitor(Lowerer &lowerer) noexcept : lowerer_(lowerer) {}

    /// @brief Ignore expression nodes encountered via the generic visitor.
    /// @details Statement lowering never processes expressions directly, so this
    ///          override is a no-op required by the @ref lower::AstVisitor base.
    void visitExpr(const Expr &) override {}

    /// @brief Dispatch a statement node to its specialised visitor.
    /// @details Invokes @ref Stmt::accept to trigger double dispatch and update
    ///          the visitor's lowering state accordingly.
    /// @param stmt Statement node to lower into IL.
    void visitStmt(const Stmt &stmt) override {
        stmt.accept(*this);
    }

    /// @brief Observe a label definition without emitting code.
    /// @details Labels are handled elsewhere when control-flow instructions bind
    ///          to them, so the visitor performs no action during the initial
    ///          walk.
    void visit(const LabelStmt &) override {}

    /// @brief Lower a PRINT statement.
    /// @details Forwards to @ref Lowerer::lowerPrint which handles argument
    ///          lowering and runtime dispatch.
    /// @param stmt PRINT statement node.
    void visit(const PrintStmt &stmt) override {
        lowerer_.lowerPrint(stmt);
    }

    /// @brief Lower a PRINT# (channel) statement.
    /// @details Delegates to @ref Lowerer::lowerPrintCh so channel-specific
    ///          semantics remain encapsulated in the helper.
    /// @param stmt PRINT# statement node.
    void visit(const PrintChStmt &stmt) override {
        lowerer_.lowerPrintCh(stmt);
    }

    /// @brief Lower the BEEP statement.
    /// @details Delegates to @ref Lowerer::visit which emits the runtime call.
    /// @param stmt BEEP statement node.
    void visit(const BeepStmt &stmt) override {
        lowerer_.visit(stmt);
    }

    /// @brief Lower a call statement that invokes a procedure.
    /// @details Defers to @ref Lowerer::lowerCallStmt to reuse the call lowering
    ///          logic shared with expression contexts.
    /// @param stmt CALL statement node.
    void visit(const CallStmt &stmt) override {
        lowerer_.lowerCallStmt(stmt);
    }

    /// @brief Lower the CLS statement.
    /// @details Invokes the generic @ref Lowerer::visit helper which dispatches
    ///          to the runtime bridge for screen clearing.
    /// @param stmt CLS statement node.
    void visit(const ClsStmt &stmt) override {
        lowerer_.visit(stmt);
    }

    /// @brief Lower the COLOR statement.
    /// @details Delegates to @ref Lowerer::visit so colour changes go through the
    ///          shared runtime helper path.
    /// @param stmt COLOR statement node.
    void visit(const ColorStmt &stmt) override {
        lowerer_.visit(stmt);
    }

    /// @brief Lower the SLEEP statement.
    /// @details Delegates to @ref Lowerer::visit to emit the runtime call.
    /// @param stmt SLEEP statement node.
    void visit(const SleepStmt &stmt) override {
        lowerer_.visit(stmt);
    }

    /// @brief Lower the LOCATE statement.
    /// @details Uses @ref Lowerer::visit to emit runtime calls for cursor
    ///          positioning.
    /// @param stmt LOCATE statement node.
    void visit(const LocateStmt &stmt) override {
        lowerer_.visit(stmt);
    }

    /// @brief Lower a CURSOR statement to control cursor visibility.
    /// @details Delegates to @ref Lowerer::visit to emit the runtime helper call.
    /// @param stmt CURSOR statement node.
    void visit(const CursorStmt &stmt) override {
        lowerer_.visit(stmt);
    }

    /// @brief Lower an ALTSCREEN statement to control alternate screen buffer.
    /// @details Delegates to @ref Lowerer::visit to emit the runtime helper call.
    /// @param stmt ALTSCREEN statement node.
    void visit(const AltScreenStmt &stmt) override {
        lowerer_.visit(stmt);
    }

    /// @brief Lower a LET assignment statement.
    /// @details Delegates to @ref Lowerer::lowerLet which manages target storage
    ///          resolution and expression coercion.
    /// @param stmt LET statement node.
    void visit(const LetStmt &stmt) override {
        lowerer_.lowerLet(stmt);
    }

    /// @brief Lower a CONST statement.
    /// @details Delegates to @ref Lowerer::lowerConst which evaluates the
    ///          initializer and stores it like a LET assignment.
    /// @param stmt CONST statement node.
    void visit(const ConstStmt &stmt) override {
        lowerer_.lowerConst(stmt);
    }

    /// @brief Lower a DIM statement.
    /// @details Only array declarations require code generation; scalar DIM
    ///          statements are compile-time declarations and therefore skipped.
    /// @param stmt DIM statement node.
    void visit(const DimStmt &stmt) override {
        if (stmt.isArray)
            lowerer_.lowerDim(stmt);
    }

    /// @brief Lower a STATIC statement.
    /// @details STATIC variables are allocated in module-level storage, not on the stack.
    /// @param stmt STATIC statement node.
    void visit(const StaticStmt &stmt) override {
        lowerer_.lowerStatic(stmt);
    }

    /// @brief Lower a SHARED statement (no-op; semantics handled by analyzer).
    /// @param stmt SHARED statement node.
    void visit(const SharedStmt &stmt) override {
        (void)stmt;
        // No code emission required; SHARED affects name resolution only.
    }

    /// @brief Lower a REDIM statement.
    /// @details Delegates to @ref Lowerer::lowerReDim to emit runtime reallocation logic.
    /// @param stmt REDIM statement node.
    void visit(const ReDimStmt &stmt) override {
        lowerer_.lowerReDim(stmt);
    }

    /// @brief Lower a RANDOMIZE statement.
    /// @details Forwards to @ref Lowerer::lowerRandomize so seeding semantics
    ///          remain centralised.
    /// @param stmt RANDOMIZE statement node.
    void visit(const RandomizeStmt &stmt) override {
        lowerer_.lowerRandomize(stmt);
    }

    /// @brief Lower a SWAP statement.
    /// @details Delegates to @ref Lowerer::lowerSwap which emits temporary
    ///          storage and exchange logic.
    /// @param stmt SWAP statement node.
    void visit(const SwapStmt &stmt) override {
        lowerer_.lowerSwap(stmt);
    }

    /// @brief Lower an IF statement.
    /// @details Delegates to @ref Lowerer::lowerIf which constructs the control
    ///          flow graph for conditional execution.
    /// @param stmt IF statement node.
    void visit(const IfStmt &stmt) override {
        lowerer_.lowerIf(stmt);
    }

    /// @brief Lower a SELECT CASE statement.
    /// @details Uses @ref Lowerer::lowerSelectCase to build the dispatch blocks
    ///          for each case branch.
    /// @param stmt SELECT CASE statement node.
    void visit(const SelectCaseStmt &stmt) override {
        lowerer_.lowerSelectCase(stmt);
    }

    /// @brief Lower a TRY/CATCH statement.
    /// @details Delegates to Lowerer helper that maps to eh.push/eh.pop and a handler with
    /// resume.label.
    /// @param stmt TRY/CATCH/FINALLY statement node.
    void visit(const TryCatchStmt &stmt) override {
        lowerer_.lowerTryCatch(stmt);
    }

    /// @brief Lower a WHILE loop.
    /// @details Delegates to @ref Lowerer::lowerWhile which wires up loop entry
    ///          and exit blocks.
    /// @param stmt WHILE statement node.
    void visit(const WhileStmt &stmt) override {
        lowerer_.lowerWhile(stmt);
    }

    /// @brief Lower a DO loop.
    /// @details Forwards to @ref Lowerer::lowerDo to cover DO WHILE and DO UNTIL
    ///          semantics.
    /// @param stmt DO statement node.
    void visit(const DoStmt &stmt) override {
        lowerer_.lowerDo(stmt);
    }

    /// @brief Lower a FOR loop.
    /// @details Delegates to @ref Lowerer::lowerFor which emits induction setup
    ///          and loop body blocks.
    /// @param stmt FOR statement node.
    void visit(const ForStmt &stmt) override {
        lowerer_.lowerFor(stmt);
    }

    /// @brief Lower a FOR EACH array iteration loop.
    /// @details Delegates to @ref Lowerer::lowerForEach which emits array bounds
    ///          computation and element access.
    /// @param stmt FOR EACH statement node.
    void visit(const ForEachStmt &stmt) override {
        lowerer_.lowerForEach(stmt);
    }

    /// @brief Lower a NEXT statement.
    /// @details Uses @ref Lowerer::lowerNext to advance the active FOR loop's
    ///          induction variable and evaluate continuation conditions.
    /// @param stmt NEXT statement node.
    void visit(const NextStmt &stmt) override {
        lowerer_.lowerNext(stmt);
    }

    /// @brief Lower an EXIT statement.
    /// @details Delegates to @ref Lowerer::lowerExit so the correct exit target
    ///          (loop or procedure) is selected.
    /// @param stmt EXIT statement node.
    void visit(const ExitStmt &stmt) override {
        lowerer_.lowerExit(stmt);
    }

    /// @brief Lower a GOTO statement.
    /// @details Forwards to @ref Lowerer::lowerGoto which resolves labels and
    ///          emits branch instructions.
    /// @param stmt GOTO statement node.
    void visit(const GotoStmt &stmt) override {
        lowerer_.lowerGoto(stmt);
    }

    /// @brief Lower a GOSUB statement.
    /// @details Uses @ref Lowerer::lowerGosub to emit call/return bookkeeping for
    ///          subroutine invocations.
    /// @param stmt GOSUB statement node.
    void visit(const GosubStmt &stmt) override {
        lowerer_.lowerGosub(stmt);
    }

    /// @brief Lower an OPEN statement for file handles.
    /// @details Delegates to @ref Lowerer::lowerOpen to generate runtime calls
    ///          for channel creation.
    /// @param stmt OPEN statement node.
    void visit(const OpenStmt &stmt) override {
        lowerer_.lowerOpen(stmt);
    }

    /// @brief Lower a CLOSE statement.
    /// @details Invokes @ref Lowerer::lowerClose so channel shutdown logic is
    ///          reused across the compiler.
    /// @param stmt CLOSE statement node.
    void visit(const CloseStmt &stmt) override {
        lowerer_.lowerClose(stmt);
    }

    /// @brief Lower a SEEK statement.
    /// @details Delegates to @ref Lowerer::lowerSeek which emits the runtime
    ///          reposition call.
    /// @param stmt SEEK statement node.
    void visit(const SeekStmt &stmt) override {
        lowerer_.lowerSeek(stmt);
    }

    /// @brief Lower an ON ERROR GOTO handler.
    /// @details Forwards to @ref Lowerer::lowerOnErrorGoto to wire the error
    ///          handling metadata.
    /// @param stmt ON ERROR GOTO statement node.
    void visit(const OnErrorGoto &stmt) override {
        lowerer_.lowerOnErrorGoto(stmt);
    }

    /// @brief Lower a RESUME statement.
    /// @details Delegates to @ref Lowerer::lowerResume so the runtime resumes the
    ///          saved error state appropriately.
    /// @param stmt RESUME statement node.
    void visit(const Resume &stmt) override {
        lowerer_.lowerResume(stmt);
    }

    /// @brief Lower an END statement.
    /// @details Forwards to @ref Lowerer::lowerEnd which emits program
    ///          termination code.
    /// @param stmt END statement node.
    void visit(const EndStmt &stmt) override {
        lowerer_.lowerEnd(stmt);
    }

    /// @brief Lower an INPUT statement.
    /// @details Delegates to @ref Lowerer::lowerInput to emit runtime prompts and
    ///          assignments.
    /// @param stmt INPUT statement node.
    void visit(const InputStmt &stmt) override {
        lowerer_.lowerInput(stmt);
    }

    /// @brief Lower an INPUT# statement.
    /// @details Uses @ref Lowerer::lowerInputCh to process channel-based input.
    /// @param stmt INPUT# statement node.
    void visit(const InputChStmt &stmt) override {
        lowerer_.lowerInputCh(stmt);
    }

    /// @brief Lower a LINE INPUT# statement.
    /// @details Delegates to @ref Lowerer::lowerLineInputCh for buffered channel
    ///          reads.
    /// @param stmt LINE INPUT# statement node.
    void visit(const LineInputChStmt &stmt) override {
        lowerer_.lowerLineInputCh(stmt);
    }

    /// @brief Lower a RETURN statement.
    /// @details Forwards to @ref Lowerer::lowerReturn which distinguishes normal
    ///          procedure returns from GOSUB returns.
    /// @param stmt RETURN statement node.
    void visit(const ReturnStmt &stmt) override {
        lowerer_.lowerReturn(stmt);
    }

    /// @brief Ignore function declarations encountered during statement lowering.
    /// @details Declarations are handled by dedicated compilation phases so the
    ///          visitor performs no action.
    void visit(const FunctionDecl &) override {}

    /// @brief Ignore subroutine declarations.
    /// @details Lowering of procedure bodies occurs elsewhere; the visitor keeps
    ///          traversal stable by doing nothing.
    void visit(const SubDecl &) override {}

    /// @brief Lower a nested statement list.
    /// @details Delegates to @ref Lowerer::lowerStmtList so child statements are
    ///          processed sequentially.
    /// @param stmt Statement list node.
    void visit(const StmtList &stmt) override {
        lowerer_.lowerStmtList(stmt);
    }

    /// @brief Lower a DELETE statement.
    /// @details Uses @ref Lowerer::lowerDelete to emit object destruction and
    ///          release behavior.
    /// @param stmt DELETE statement node.
    void visit(const DeleteStmt &stmt) override {
        lowerer_.lowerDelete(stmt);
    }

    /// @brief Ignore constructor declarations encountered during lowering.
    /// @details Constructors are compiled via dedicated passes, so this visitor
    ///          leaves them untouched.
    void visit(const ConstructorDecl &) override {}

    /// @brief Ignore destructor declarations.
    /// @details Destructor lowering is handled elsewhere, so the visitor takes
    ///          no action.
    void visit(const DestructorDecl &) override {}

    /// @brief Ignore method declarations not currently being lowered.
    /// @details Method bodies are lowered via separate entry points, so the
    ///          visitor skips declaration nodes.
    void visit(const MethodDecl &) override {}

    /// @brief Ignore class declarations.
    /// @details Structural declarations do not produce IL during statement
    ///          lowering and are therefore skipped.
    void visit(const ClassDecl &) override {}

    /// @brief Ignore type declarations.
    /// @details The type system compiler processes these nodes separately, so no
    ///          action is required here.
    void visit(const TypeDecl &) override {}

    /// @brief Ignore interface declarations during statement lowering.
    /// @details Interface declarations produce no immediate IL.
    void visit(const InterfaceDecl &) override {}

    /// @brief Ignore USING directives during statement lowering.
    /// @details USING is compile-time only and produces no IL.
    void visit(const UsingDecl &) override {}

    /// @brief Lower a USING resource statement with automatic cleanup.
    /// @details Implements TRY/FINALLY semantics to guarantee destructor invocation.
    /// @param stmt Resource-lifetime statement to lower.
    void visit(const UsingStmt &stmt) override {
        lowerer_.lowerUsingStmt(stmt);
    }

    /// @brief Handle NAMESPACE blocks by adjusting the qualification stack.
    /// @details Pushes the namespace segments, lowers the nested body, then pops
    ///          the same number of segments to restore the previous scope.
    /// @param stmt Namespace declaration containing the qualified path and body.
    void visit(const NamespaceDecl &stmt) override {
        lowerer_.pushNamespace(stmt.path);
        for (const auto &child : stmt.body) {
            if (child)
                lowerer_.lowerStmt(*child);
        }
        lowerer_.popNamespace(stmt.path.size());
    }

  private:
    /// Borrowed lowering context receiving all statement side effects.
    Lowerer &lowerer_;
};

/// @brief Lower a single BASIC statement into IL.
/// @details Updates the active source location for diagnostics, instantiates a
///          @ref LowererStmtVisitor, and lets it process the statement node.
/// @param stmt Statement to lower.
void Lowerer::lowerStmt(const Stmt &stmt) {
    if (++stmtLowerDepth_ > kMaxLowerDepth) {
        --stmtLowerDepth_;
        return;
    }

    struct DepthGuard {
        unsigned &d;

        ~DepthGuard() {
            --d;
        }
    } stmtGuard_{stmtLowerDepth_};

    curLoc = stmt.loc;
    LowererStmtVisitor visitor(*this);
    visitor.visitStmt(stmt);
    // BUG-085 fix: Release deferred temporaries after each statement rather than
    // at function exit. This prevents use-before-def errors when temporaries from
    // array access (rt_arr_obj_get) are created inside loops - the temporaries are
    // loop-local and don't exist at function entry, so releasing them at function
    // exit would reference undefined values.
    releaseDeferredTemps();
}

/// @brief Lower a sequence of BASIC statements.
/// @details Iterates over child statements while respecting block termination
///          so unreachable statements are skipped. Each child is lowered via
///          @ref lowerStmt.
/// @param stmt Statement list to lower.
void Lowerer::lowerStmtList(const StmtList &stmt) {
    for (const auto &child : stmt.stmts) {
        if (!child)
            continue;
        BasicBlock *current = context().current();
        if (current && current->terminated)
            break;
        lowerStmt(*child);
    }
}

/// @brief Lower a statement-level procedure invocation.
/// @details Ensures the call expression exists, updates the source location, and
///          defers to @ref lowerExpr so argument coercions are reused.
/// @param stmt CALL statement node containing the procedure invocation.
void Lowerer::lowerCallStmt(const CallStmt &stmt) {
    if (!stmt.call)
        return;
    curLoc = stmt.loc;
    // If this is a method call, reject FUNCTION-as-statement and emit void call for SUB.
    if (auto *me = as<const MethodCallExpr>(*stmt.call)) {
        // Identify class and check return type
        // BUG-OOP-015 fix: Allow method FUNCTION calls as statements by discarding result
        std::string cls = me->base ? resolveObjectClass(*me->base) : std::string{};
        if (auto ret = findMethodReturnType(cls, me->method)) {
            // Lower the method call as an expression but discard the result
            lowerExpr(*me);
            return;
        }
        // SUB: just lower, which emits a void call under the hood
        lowerMethodCallExpr(*me);
        return;
    }

    // For plain procedures, reject FUNCTION-as-statement
    if (auto *ce = as<const CallExpr>(*stmt.call)) {
        // Resolve callee name as in expression lowering
        std::string calleeResolved;
        if (!ce->calleeQualified.empty())
            calleeResolved = CanonicalizeQualified(ce->calleeQualified);
        else
            calleeResolved = CanonicalizeIdent(ce->callee);
        const std::string &calleeKey = calleeResolved.empty() ? ce->callee : calleeResolved;
        if (const ProcedureSignature *sig = findProcSignature(calleeKey)) {
            // BUG-OOP-015 fix: Allow FUNCTION calls as statements by discarding result
            // This matches common BASIC behavior where return values can be ignored
            if (sig->retType.kind != Type::Kind::Void) {
                // Lower the call as an expression but discard the result
                lowerExpr(*ce);
                return;
            }
        }

        // Attempt direct lowering for runtime builtin externs by descriptor name.
        // Handle unqualified calls via USING imports by probing imported namespaces.
        const il::runtime::RuntimeSignature *rtSig = il::runtime::findRuntimeSignature(calleeKey);
        if (!rtSig && calleeKey.find('.') == std::string::npos && !ce->callee.empty()) {
            // Helper to convert canonical namespace to title-case for runtime lookup
            /// @brief Converts canonical namespace segments to runtime title-case spelling.
            /// @param ns Canonical dotted namespace.
            /// @return Dotted namespace with each segment's first character uppercased.
            auto titleCaseNs = [](const std::string &ns) -> std::string {
                std::string out;
                out.reserve(ns.size());
                bool start = true;
                for (char ch : ns) {
                    if (ch == '.') {
                        out.push_back('.');
                        start = true;
                    } else if (start) {
                        out.push_back(
                            static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
                        start = false;
                    } else {
                        out.push_back(ch);
                    }
                }
                return out;
            };

            // Try USING imports from semantic analyzer
            const SemanticAnalyzer *sema = semanticAnalyzer();
            if (sema) {
                std::vector<std::string> imports = sema->getUsingImports();
                for (const auto &ns : imports) {
                    std::string qualifiedNs = titleCaseNs(ns);
                    std::string candidate = qualifiedNs + "." + ce->callee;
                    if (const auto *sig = il::runtime::findRuntimeSignature(candidate)) {
                        rtSig = sig;
                        calleeResolved = std::move(candidate);
                        break;
                    }
                }
            }
            // Fallback: try common Zanna.* namespaces even without explicit USING
            if (!rtSig) {
                static const char *defaultNamespaces[] = {
                    "Zanna.Console", "Zanna.Terminal", "Zanna.Time"};
                for (const char *ns : defaultNamespaces) {
                    std::string candidate = std::string(ns) + "." + ce->callee;
                    if (const auto *sig = il::runtime::findRuntimeSignature(candidate)) {
                        rtSig = sig;
                        calleeResolved = std::move(candidate);
                        break;
                    }
                }
            }
        }
        if (rtSig) {
            std::vector<il::core::Value> args;
            args.reserve(ce->args.size());
            for (size_t i = 0; i < ce->args.size(); ++i) {
                if (!ce->args[i])
                    continue;
                auto rv = lowerExpr(*ce->args[i]);
                if (i < rtSig->paramTypes.size()) {
                    auto pt = rtSig->paramTypes[i];
                    if (pt.kind == il::core::Type::Kind::F64)
                        rv = coerceToF64(std::move(rv), ce->loc);
                    else if (pt.kind == il::core::Type::Kind::I64)
                        rv = coerceToI64(std::move(rv), ce->loc);
                    else if (pt.kind == il::core::Type::Kind::I1)
                        rv = coerceToBool(std::move(rv), ce->loc);
                    else if (pt.kind == il::core::Type::Kind::I32) {
                        // Narrow to i32 for runtime helpers that use 32-bit ABI
                        rv = ensureI64(std::move(rv), ce->loc);
                        rv.value = emitCommon(ce->loc).narrow_to(rv.value, 64, 32);
                    }
                }
                args.push_back(rv.value);
            }
            curLoc = ce->loc;
            const std::string &target = calleeResolved.empty() ? calleeKey : calleeResolved;
            if (rtSig->retType.kind == il::core::Type::Kind::Void)
                emitCall(target, args);
            else
                (void)emitCallRet(rtSig->retType, target, args);
            return;
        }
    }

    // Default: lower as expression (SUB path yields a void call; result ignored)
    lowerExpr(*stmt.call);
}

/// @brief Lower a RETURN statement.
/// @details Distinguishes between GOSUB returns—which route through
///          @ref lowerGosubReturn—and normal procedure returns. When the
///          statement includes a value it is lowered and emitted via @ref emitRet;
///          otherwise a void return is generated.
/// @param stmt RETURN statement node.
void Lowerer::lowerReturn(const ReturnStmt &stmt) {
    if (stmt.isGosubReturn) {
        lowerGosubReturn(stmt);
        return;
    }

    if (stmt.value) {
        // If the enclosing FUNCTION returns an object (ptr), be strict about
        // returning a pointer-typed value. This avoids accidental scalar
        // coercions and ensures RETURN of an object variable emits a ptr load.
        il::core::Function *fn = context().function();
        if (fn && fn->retType.kind == Type::Kind::Ptr) {
            if (auto *var = as<const VarExpr>(*stmt.value)) {
                // Variable names are resolved by semantic analysis, so we can
                // directly look up the properly-typed symbol.
                if (auto storage = resolveVariableStorage(var->name, var->loc)) {
                    if (storage->slotInfo.isObject ||
                        storage->slotInfo.type.kind == Type::Kind::Ptr) {
                        Value val = emitLoad(Type(Type::Kind::Ptr), storage->pointer);
                        emitRet(val);
                        return;
                    }
                }
            }
        }

        RVal v = lowerExpr(*stmt.value);
        // BUG-057: If the enclosing FUNCTION returns BOOLEAN (i1), ensure the
        // returned value is coerced to i1 to satisfy the IL verifier.
        if (il::core::Function *fn2 = context().function()) {
            if (fn2->retType.kind == il::core::Type::Kind::I1) {
                v = coerceToBool(std::move(v), stmt.loc);
            }
        }
        emitRet(v.value);
    } else {
        // BUG-107: In FUNCTIONs, a bare RETURN should jump to the unified
        // epilogue so we return the implicit result (assignment to the
        // function name) rather than emitting a void return which violates
        // the IL verifier's return type.
        if (il::core::Function *fn = context().function();
            fn && fn->retType.kind != Type::Kind::Void) {
            ProcedureContext &ctx = context();
            Function *func = ctx.function();
            if (func) {
                emitBr(&func->blocks[ctx.exitIndex()]);
                return;
            }
        }
        // SUB or unavailable context: fall back to void return.
        emitRetVoid();
    }
}

} // namespace il::frontends::basic
