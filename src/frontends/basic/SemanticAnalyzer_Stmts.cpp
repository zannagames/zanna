//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the visitor that dispatches BASIC AST statements to the specialised
// semantic-analysis routines.  Concentrating the forwarding logic in this
// translation unit keeps the public headers slim while documenting how the
// analyser walks statement sequences.
//
//===----------------------------------------------------------------------===//
//
/// @file SemanticAnalyzer_Stmts.cpp
/// @brief Statement dispatch helpers for the BASIC semantic analyser.
/// @details Defines the visitor class that translates generic AST visits into
///          calls on @ref SemanticAnalyzer. Declaration nodes handled by
///          separate program/class passes and semantically empty terminal/OOP
///          nodes use explicit no-op overrides.

#include "frontends/basic/SemanticAnalyzer_Internal.hpp"

namespace il::frontends::basic {

/// @brief Visitor that forwards AST statements to the owning analyser.
/// @details Each override delegates to the corresponding `SemanticAnalyzer::analyze*`
///          routine so the heavy semantic logic remains in the dedicated
///          compilation units.  The visitor itself owns no resources beyond a
///          reference to the active analyser.
/// @invariant @ref analyzer_ outlives this visitor.
class SemanticAnalyzerStmtVisitor final : public MutStmtVisitor {
  public:
    /// @brief Construct the visitor with a reference to the active analyser.
    /// @param analyzer Semantic analyser that will process visited statements.
    /// @post No statement is visited during construction.
    explicit SemanticAnalyzerStmtVisitor(SemanticAnalyzer &analyzer) noexcept
        : analyzer_(analyzer) {}

    /// @brief Ignores a label already handled by label pre-collection.
    void visit(LabelStmt &) override {}

    /// @brief Delegate PRINT statements to @ref SemanticAnalyzer::analyzePrint.
    /// @param stmt Mutable print statement.
    void visit(PrintStmt &stmt) override {
        analyzer_.analyzePrint(stmt);
    }

    /// @brief Delegate PRINT# channel statements to the analyser.
    /// @param stmt Mutable channel-print statement.
    void visit(PrintChStmt &stmt) override {
        analyzer_.analyzePrintCh(stmt);
    }

    /// @brief Ignores a parameterless BEEP statement.
    void visit(BeepStmt &) override {}

    /// @brief Resolve subroutine invocations via @ref SemanticAnalyzer::analyzeCallStmt.
    /// @param stmt Mutable call statement that may receive resolved target data.
    void visit(CallStmt &stmt) override {
        analyzer_.analyzeCallStmt(stmt);
    }

    /// @brief Validate CLS statements through the analyser.
    /// @param stmt Statement passed to its specialized overload.
    void visit(ClsStmt &stmt) override {
        analyzer_.visit(stmt);
    }

    /// @brief Validate CURSOR statements through the analyser.
    /// @param stmt Statement passed to its specialized overload.
    void visit(CursorStmt &stmt) override {
        analyzer_.visit(stmt);
    }

    /// @brief Ignores an ALTSCREEN toggle with no typed operands.
    void visit(AltScreenStmt &) override {}

    /// @brief Forward COLOR statements for palette validation.
    /// @param stmt Statement passed to its specialized overload.
    void visit(ColorStmt &stmt) override {
        analyzer_.visit(stmt);
    }

    /// @brief Forward SLEEP statements for duration validation.
    /// @param stmt Statement passed to its specialized overload.
    void visit(SleepStmt &stmt) override {
        analyzer_.visit(stmt);
    }

    /// @brief Forwards LOCATE for operand validation.
    /// @param stmt Statement passed to its specialized overload.
    void visit(LocateStmt &stmt) override {
        analyzer_.visit(stmt);
    }

    /// @brief Defer LET assignment analysis to the analyser.
    /// @param stmt Mutable assignment.
    void visit(LetStmt &stmt) override {
        analyzer_.analyzeLet(stmt);
    }

    /// @brief Validates and records a CONST declaration.
    /// @param stmt Mutable constant declaration.
    void visit(ConstStmt &stmt) override {
        analyzer_.analyzeConst(stmt);
    }

    /// @brief Validates and records a DIM declaration.
    /// @param stmt Mutable variable/array declaration.
    void visit(DimStmt &stmt) override {
        analyzer_.analyzeDim(stmt);
    }

    /// @brief STATIC statements declare procedure-local persistent variables.
    /// @param stmt Mutable declaration.
    void visit(StaticStmt &stmt) override {
        analyzer_.analyzeStatic(stmt);
    }

    /// @brief SHARED statements list names that should refer to module-level bindings.
    /// @param stmt Mutable declaration.
    void visit(SharedStmt &stmt) override {
        analyzer_.analyzeShared(stmt);
    }

    /// @brief Re-DIM statements re-use the analyser's array checks.
    /// @param stmt Mutable resize statement.
    void visit(ReDimStmt &stmt) override {
        analyzer_.analyzeReDim(stmt);
    }

    /// @brief SWAP statements validate that both operands are valid lvalues.
    /// @param stmt Mutable swap statement.
    void visit(SwapStmt &stmt) override {
        analyzer_.analyzeSwap(stmt);
    }

    /// @brief RANDOMIZE statements evaluate seed expressions via the analyser.
    /// @param stmt Mutable statement.
    void visit(RandomizeStmt &stmt) override {
        analyzer_.analyzeRandomize(stmt);
    }

    /// @brief Forward IF statements for branch analysis.
    /// @param stmt Mutable conditional statement.
    void visit(IfStmt &stmt) override {
        analyzer_.analyzeIf(stmt);
    }

    /// @brief Delegate SELECT CASE semantics.
    /// @param stmt Mutable SELECT statement that receives its model.
    void visit(SelectCaseStmt &stmt) override {
        analyzer_.analyzeSelectCase(stmt);
    }

    /// @brief Loop constructs are analysed by dedicated helpers.
    /// @param stmt Mutable WHILE statement.
    void visit(WhileStmt &stmt) override {
        analyzer_.analyzeWhile(stmt);
    }

    /// @brief Defer DO loop validation to the analyser.
    /// @param stmt Mutable DO statement.
    void visit(DoStmt &stmt) override {
        analyzer_.analyzeDo(stmt);
    }

    /// @brief Ensure FOR loops are type consistent.
    /// @param stmt Mutable FOR statement.
    void visit(ForStmt &stmt) override {
        analyzer_.analyzeFor(stmt);
    }

    /// @brief Forwards FOR EACH for iterator/container validation.
    /// @param stmt Mutable FOR EACH statement.
    void visit(ForEachStmt &stmt) override {
        analyzer_.analyzeForEach(stmt);
    }

    /// @brief Route NEXT statements to the matching FOR-loop checker.
    /// @param stmt Mutable NEXT statement.
    void visit(NextStmt &stmt) override {
        analyzer_.analyzeNext(stmt);
    }

    /// @brief EXIT statements are validated for legal context.
    /// @param stmt Mutable EXIT statement.
    void visit(ExitStmt &stmt) override {
        analyzer_.analyzeExit(stmt);
    }

    /// @brief Forward GOTO statements for label resolution.
    /// @param stmt Mutable GOTO statement.
    void visit(GotoStmt &stmt) override {
        analyzer_.analyzeGoto(stmt);
    }

    /// @brief Defer GOSUB statements for continuation tracking.
    /// @param stmt Mutable GOSUB statement.
    void visit(GosubStmt &stmt) override {
        analyzer_.analyzeGosub(stmt);
    }

    /// @brief OPEN statements perform channel validation.
    /// @param stmt Mutable OPEN statement.
    void visit(OpenStmt &stmt) override {
        analyzer_.analyzeOpen(stmt);
    }

    /// @brief CLOSE statements ensure matching handles.
    /// @param stmt Mutable CLOSE statement.
    void visit(CloseStmt &stmt) override {
        analyzer_.analyzeClose(stmt);
    }

    /// @brief SEEK statements validate position operands.
    /// @param stmt Mutable SEEK statement.
    void visit(SeekStmt &stmt) override {
        analyzer_.analyzeSeek(stmt);
    }

    /// @brief ON ERROR GOTO statements delegate to exception analysis.
    /// @param stmt Mutable handler statement.
    void visit(OnErrorGoto &stmt) override {
        analyzer_.analyzeOnErrorGoto(stmt);
    }

    /// @brief END statements simply check program termination rules.
    /// @param stmt Mutable END statement.
    void visit(EndStmt &stmt) override {
        analyzer_.analyzeEnd(stmt);
    }

    /// @brief Standard INPUT statements check prompt and target expressions.
    /// @param stmt Mutable INPUT statement.
    void visit(InputStmt &stmt) override {
        analyzer_.analyzeInput(stmt);
    }

    /// @brief Channel-based INPUT statements share analyser logic.
    /// @param stmt Mutable channel-input statement.
    void visit(InputChStmt &stmt) override {
        analyzer_.analyzeInputCh(stmt);
    }

    /// @brief LINE INPUT# statements reuse string-specific validation.
    /// @param stmt Mutable line-input statement.
    void visit(LineInputChStmt &stmt) override {
        analyzer_.analyzeLineInputCh(stmt);
    }

    /// @brief RESUME statements are validated for legal EH state.
    /// @param stmt Mutable RESUME statement.
    void visit(Resume &stmt) override {
        analyzer_.analyzeResume(stmt);
    }

    /// @brief RETURN statements are handed to the analyser for stack checks.
    /// @param stmt Mutable RETURN statement.
    void visit(ReturnStmt &stmt) override {
        analyzer_.analyzeReturn(stmt);
    }

    /// @brief Ignores FUNCTION declarations analyzed by the procedure pass.
    void visit(FunctionDecl &) override {}

    /// @brief Ignores SUB declarations analyzed by the procedure pass.
    void visit(SubDecl &) override {}

    /// @brief Nested statement lists are analysed recursively.
    /// @param stmt Mutable list passed to ordered traversal.
    void visit(StmtList &stmt) override {
        analyzer_.analyzeStmtList(stmt);
    }

    /// @brief Ignores DELETE here; object lifetime checks occur in other stages.
    void visit(DeleteStmt &) override {}

    /// @brief Ignores a constructor declaration whose body is driven by the
    ///        enclosing class analysis.
    void visit(ConstructorDecl &) override {}

    /// @brief Ignores a destructor declaration whose body is driven by the
    ///        enclosing class analysis.
    void visit(DestructorDecl &) override {}

    /// @brief Ignores a method declaration whose body is driven by the
    ///        enclosing class analysis.
    void visit(MethodDecl &) override {}

    /// @brief Enters namespace-aware two-pass semantic traversal.
    /// @param decl Mutable namespace declaration.
    void visit(NamespaceDecl &decl) override {
        analyzer_.analyzeNamespaceDecl(decl);
    }

    /// @brief Forwards TRY/CATCH/FINALLY validation.
    /// @param stmt Mutable structured error-handling statement.
    void visit(TryCatchStmt &stmt) override {
        analyzer_.visit(stmt);
    }

    /// @brief Forwards runtime-resource USING statement validation.
    /// @param stmt Mutable runtime-resource statement.
    void visit(UsingStmt &stmt) override {
        analyzer_.visit(stmt);
    }

    /// @brief Resolves class type references and analyzes member bodies.
    /// @param decl Mutable class declaration.
    void visit(ClassDecl &decl) override {
        analyzer_.analyzeClassDecl(decl);
    }

    /// @brief Ignores legacy TYPE declarations in this semantic dispatcher.
    void visit(TypeDecl &) override {}

    /// @brief Interface declarations are analysed in dedicated OOP passes.
    /// @param decl Mutable interface declaration used for placement state.
    void visit(InterfaceDecl &decl) override {
        analyzer_.analyzeInterfaceDecl(decl);
    }

    /// @brief USING directives will be processed by namespace semantic pass.
    /// @param decl Mutable namespace import/alias declaration.
    void visit(UsingDecl &decl) override {
        analyzer_.analyzeUsingDecl(decl);
    }

  private:
    /// @brief Borrowed analyzer receiving every delegated statement.
    SemanticAnalyzer &analyzer_;
};

/// @brief Dispatch a statement node through the semantic analyser visitor.
/// @details Instantiates a fresh @ref SemanticAnalyzerStmtVisitor for each call
///          so that recursive analysis reuses the most up-to-date analyser
///          state.  The node is then asked to accept the visitor, triggering the
///          appropriate override above.
/// @param s Mutable statement node to dispatch.
void SemanticAnalyzer::visitStmt(Stmt &s) {
    SemanticAnalyzerStmtVisitor visitor(*this);
    s.accept(visitor);
}

/// @brief Visit every statement contained within a list.
/// @details Iterates through the list in program order and forwards each entry
///          to @ref visitStmt.  Null entries are ignored, preserving any
///          optional AST placeholders produced during earlier stages.
/// @param lst Sequence of statements to analyse.
/// @post Analyzer state reflects each non-null child in list order.
void SemanticAnalyzer::analyzeStmtList(const StmtList &lst) {
    for (const auto &st : lst.stmts)
        if (st)
            visitStmt(*st);
}

} // namespace il::frontends::basic
