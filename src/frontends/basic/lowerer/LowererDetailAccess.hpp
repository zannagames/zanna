//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lowerer/LowererDetailAccess.hpp
// Purpose: Defines Lowerer's narrow DetailAccess facade for modular expression,
//          control, OOP, and runtime lowering helpers.
// Key invariants: Every operation forwards to the same backing Lowerer and
//                 exposes no independent mutable state.
// Ownership/Lifetime: DetailAccess borrows a Lowerer that must outlive it.
// Integration: This fragment is included inside the Lowerer class body and
//              therefore must not add include guards, namespaces, or includes.
// Links: src/frontends/basic/Lowerer.hpp,
//        src/frontends/basic/lower/detail/LowererDetail.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines the Lowerer-internal facade used by modular BASIC lowering helpers.
/// @details This class-body fragment forwards expression, control-flow, OOP,
///          declaration, and statement lowering to one borrowed Lowerer while
///          exposing no independent state or ownership.

/// @brief Narrow public facade providing controlled access to Lowerer internals.
/// @details Modular lowering helpers receive a DetailAccess handle instead of
///          friendship to the entire Lowerer class. This limits the surface area
///          exposed to helper modules while still allowing them to delegate back
///          to core lowering routines.
class DetailAccess {
  public:
    /// @brief Construct a detail-access handle bound to the given Lowerer.
    /// @param lowerer The owning Lowerer instance to delegate through.
    explicit DetailAccess(Lowerer &lowerer) noexcept : lowerer_(&lowerer) {}

    /// @brief Retrieve the underlying Lowerer reference.
    /// @return The Lowerer instance backing this handle.
    [[nodiscard]] Lowerer &lowerer() const noexcept {
        return *lowerer_;
    }

    /// @brief Lower a variable reference expression.
    /// @param expr Variable expression AST node.
    /// @return Loaded value and its type.
    [[nodiscard]] RVal lowerVarExpr(const VarExpr &expr) {
        return lowerer_->lowerVarExpr(expr);
    }

    /// @brief Lower a unary expression (e.g. NOT, negation).
    /// @param expr Unary expression AST node.
    /// @return Resulting value and type.
    [[nodiscard]] RVal lowerUnaryExpr(const UnaryExpr &expr) {
        return lowerer_->lowerUnaryExpr(expr);
    }

    /// @brief Lower a binary expression (arithmetic, comparison, etc.).
    /// @param expr Binary expression AST node.
    /// @return Resulting value and type.
    [[nodiscard]] RVal lowerBinaryExpr(const BinaryExpr &expr) {
        return lowerer_->lowerBinaryExpr(expr);
    }

    /// @brief Lower a UBOUND query expression.
    /// @param expr UBOUND expression AST node naming the array.
    /// @return Resulting value and type.
    [[nodiscard]] RVal lowerUBoundExpr(const UBoundExpr &expr) {
        return lowerer_->lowerUBoundExpr(expr);
    }

    /// @brief Lower an IF/ELSEIF/ELSE statement.
    /// @param stmt IF statement AST node.
    void lowerIf(const IfStmt &stmt) {
        lowerer_->lowerIf(stmt);
    }

    /// @brief Lower a WHILE/WEND loop statement.
    /// @param stmt WHILE statement AST node.
    void lowerWhile(const WhileStmt &stmt) {
        lowerer_->lowerWhile(stmt);
    }

    /// @brief Lower a DO/LOOP statement.
    /// @param stmt DO statement AST node.
    void lowerDo(const DoStmt &stmt) {
        lowerer_->lowerDo(stmt);
    }

    /// @brief Lower a FOR/NEXT loop statement.
    /// @param stmt FOR statement AST node.
    void lowerFor(const ForStmt &stmt) {
        lowerer_->lowerFor(stmt);
    }

    /// @brief Lower a FOR EACH iteration statement.
    /// @param stmt FOR EACH statement AST node.
    void lowerForEach(const ForEachStmt &stmt) {
        lowerer_->lowerForEach(stmt);
    }

    /// @brief Lower a SELECT CASE statement.
    /// @param stmt SELECT CASE statement AST node.
    void lowerSelectCase(const SelectCaseStmt &stmt) {
        lowerer_->lowerSelectCase(stmt);
    }

    /// @brief Lower a NEXT statement (FOR loop increment/termination).
    /// @param stmt NEXT statement AST node.
    void lowerNext(const NextStmt &stmt) {
        lowerer_->lowerNext(stmt);
    }

    /// @brief Lower an EXIT statement (loop early termination).
    /// @param stmt EXIT statement AST node.
    void lowerExit(const ExitStmt &stmt) {
        lowerer_->lowerExit(stmt);
    }

    /// @brief Lower a GOTO statement (unconditional branch).
    /// @param stmt GOTO statement AST node.
    void lowerGoto(const GotoStmt &stmt) {
        lowerer_->lowerGoto(stmt);
    }

    /// @brief Lower a GOSUB statement (subroutine call with return address).
    /// @param stmt GOSUB statement AST node.
    void lowerGosub(const GosubStmt &stmt) {
        lowerer_->lowerGosub(stmt);
    }

    /// @brief Lower a GOSUB RETURN statement.
    /// @param stmt RETURN statement AST node.
    void lowerGosubReturn(const ReturnStmt &stmt) {
        lowerer_->lowerGosubReturn(stmt);
    }

    /// @brief Lower an ON ERROR GOTO statement (error handler registration).
    /// @param stmt ON ERROR GOTO statement AST node.
    void lowerOnErrorGoto(const OnErrorGoto &stmt) {
        lowerer_->lowerOnErrorGoto(stmt);
    }

    /// @brief Lower a RESUME statement (error handler continuation).
    /// @param stmt RESUME statement AST node.
    void lowerResume(const Resume &stmt) {
        lowerer_->lowerResume(stmt);
    }

    /// @brief Lower an END statement (program termination).
    /// @param stmt END statement AST node.
    void lowerEnd(const EndStmt &stmt) {
        lowerer_->lowerEnd(stmt);
    }

    /// @brief Lower a TRY/CATCH statement.
    /// @param stmt TRY/CATCH statement AST node.
    void lowerTryCatch(const TryCatchStmt &stmt) {
        lowerer_->lowerTryCatch(stmt);
    }

    /// @brief Lower a NEW expression allocating a BASIC object instance.
    /// @param expr NEW expression AST node.
    /// @return Resulting object pointer value and type.
    [[nodiscard]] RVal lowerNewExpr(const NewExpr &expr) {
        return lowerer_->lowerNewExpr(expr);
    }

    /// @brief Lower a NEW expression with an explicit OOP context.
    /// @param expr NEW expression AST node.
    /// @param ctx OOP lowering context providing cached metadata.
    /// @return Resulting object pointer value and type.
    [[nodiscard]] RVal lowerNewExpr(const NewExpr &expr, OopLoweringContext &ctx) {
        return lowerer_->lowerNewExpr(expr, ctx);
    }

    /// @brief Lower a ME expression referencing the implicit instance slot.
    /// @param expr ME expression AST node.
    /// @return Resulting self-pointer value and type.
    [[nodiscard]] RVal lowerMeExpr(const MeExpr &expr) {
        return lowerer_->lowerMeExpr(expr);
    }

    /// @brief Lower a ME expression with an explicit OOP context.
    /// @param expr ME expression AST node.
    /// @param ctx OOP lowering context providing cached metadata.
    /// @return Resulting self-pointer value and type.
    [[nodiscard]] RVal lowerMeExpr(const MeExpr &expr, OopLoweringContext &ctx) {
        return lowerer_->lowerMeExpr(expr, ctx);
    }

    /// @brief Lower a member access expression (field read).
    /// @param expr Member access expression AST node.
    /// @return Loaded field value and its type.
    [[nodiscard]] RVal lowerMemberAccessExpr(const MemberAccessExpr &expr) {
        return lowerer_->lowerMemberAccessExpr(expr);
    }

    /// @brief Lower a member access expression with an explicit OOP context.
    /// @param expr Member access expression AST node.
    /// @param ctx OOP lowering context providing cached metadata.
    /// @return Loaded field value and its type.
    [[nodiscard]] RVal lowerMemberAccessExpr(const MemberAccessExpr &expr,
                                             OopLoweringContext &ctx) {
        return lowerer_->lowerMemberAccessExpr(expr, ctx);
    }

    /// @brief Lower a method call expression.
    /// @param expr Method call expression AST node.
    /// @return Resulting return value and its type.
    [[nodiscard]] RVal lowerMethodCallExpr(const MethodCallExpr &expr) {
        return lowerer_->lowerMethodCallExpr(expr);
    }

    /// @brief Lower a method call expression with an explicit OOP context.
    /// @param expr Method call expression AST node.
    /// @param ctx OOP lowering context providing cached metadata.
    /// @return Resulting return value and its type.
    [[nodiscard]] RVal lowerMethodCallExpr(const MethodCallExpr &expr, OopLoweringContext &ctx) {
        return lowerer_->lowerMethodCallExpr(expr, ctx);
    }

    /// @brief Lower a DELETE statement releasing an object reference.
    /// @param stmt DELETE statement AST node.
    void lowerDelete(const DeleteStmt &stmt) {
        lowerer_->lowerDelete(stmt);
    }

    /// @brief Lower a DELETE statement with an explicit OOP context.
    /// @param stmt DELETE statement AST node.
    /// @param ctx OOP lowering context providing cached metadata.
    void lowerDelete(const DeleteStmt &stmt, OopLoweringContext &ctx) {
        lowerer_->lowerDelete(stmt, ctx);
    }

    /// @brief Scan program OOP constructs to populate class layouts.
    /// @param prog Program AST root node.
    void scanOOP(const Program &prog) {
        lowerer_->scanOOP(prog);
    }

    /// @brief Emit constructor, destructor, and method bodies for CLASS declarations.
    /// @param prog Program AST root node.
    void emitOopDeclsAndBodies(const Program &prog) {
        lowerer_->emitOopDeclsAndBodies(prog);
    }

    /// @brief Lower a LET assignment statement.
    /// @param stmt LET statement AST node.
    void lowerLet(const LetStmt &stmt) {
        lowerer_->lowerLet(stmt);
    }

    /// @brief Lower a CONST declaration statement.
    /// @param stmt CONST statement AST node.
    void lowerConst(const ConstStmt &stmt) {
        lowerer_->lowerConst(stmt);
    }

    /// @brief Lower a STATIC variable declaration statement.
    /// @param stmt STATIC statement AST node.
    void lowerStatic(const StaticStmt &stmt) {
        lowerer_->lowerStatic(stmt);
    }

    /// @brief Lower a DIM array/variable declaration statement.
    /// @param stmt DIM statement AST node.
    void lowerDim(const DimStmt &stmt) {
        lowerer_->lowerDim(stmt);
    }

    /// @brief Lower a REDIM array resizing statement.
    /// @param stmt REDIM statement AST node.
    void lowerReDim(const ReDimStmt &stmt) {
        lowerer_->lowerReDim(stmt);
    }

    /// @brief Lower a RANDOMIZE statement (seed random number generator).
    /// @param stmt RANDOMIZE statement AST node.
    void lowerRandomize(const RandomizeStmt &stmt) {
        lowerer_->lowerRandomize(stmt);
    }

    /// @brief Lower a SWAP statement (exchange two variables).
    /// @param stmt SWAP statement AST node.
    void lowerSwap(const SwapStmt &stmt) {
        lowerer_->lowerSwap(stmt);
    }

  private:
    Lowerer *lowerer_; ///< Non-owning pointer to the backing Lowerer.
};

/// @brief Create a detail-access handle for modular lowering helpers.
/// @return A lightweight facade exposing a controlled subset of Lowerer internals.
[[nodiscard]] DetailAccess detailAccess() noexcept {
    return DetailAccess(*this);
}
