//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// File: src/frontends/basic/lower/detail/LowererDetail.hpp
// Purpose: Declares internal facade classes that group expression, control,
//          OOP, and runtime-statement lowering operations.
// Key invariants: Helpers forward through Lowerer::DetailAccess and do not own
//                 AST or IR storage.
// Ownership/Lifetime: Every helper stores a borrowed access facade whose
//                     underlying Lowerer outlives the helper.
// Links: src/frontends/basic/lower/detail/ExprLoweringHelper.cpp,
//        src/frontends/basic/lower/detail/ControlLoweringHelper.cpp,
//        src/frontends/basic/lower/detail/OopLoweringHelper.cpp,
//        src/frontends/basic/lower/detail/RuntimeLoweringHelper.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/LowererTypes.hpp"
#include "zanna/il/Module.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/// @file
/// @brief Declares implementation-only BASIC lowering helper facades.

namespace il::frontends::basic {

struct OopLoweringContext;

namespace lower::detail {

//===----------------------------------------------------------------------===//
// Expression Lowering Helpers
//===----------------------------------------------------------------------===//

/// @brief Helper class coordinating expression lowering operations.
/// @details Encapsulates expression lowering logic including literals, variables,
///          unary/binary operators, calls, and OOP expressions. Works with the
///          visitor pattern to dispatch to specific lowering routines.
/// @invariant All methods preserve Lowerer state consistency.
/// @ownership Borrows Lowerer reference; does not own AST nodes.
class ExprLoweringHelper {
  public:
    /// @brief Creates an expression facade over a caller-owned Lowerer.
    /// @param access Authorized forwarding facade for the active lowerer.
    explicit ExprLoweringHelper(Lowerer::DetailAccess access) noexcept;

    /// @brief Lower a variable reference expression.
    /// @param expr Variable reference to resolve and load.
    /// @return Lowered value and IL type.
    [[nodiscard]] RVal lowerVarExpr(const VarExpr &expr);

    /// @brief Lower a unary expression (NOT, negation, etc.).
    /// @param expr Unary expression to lower.
    /// @return Lowered value and IL type.
    [[nodiscard]] RVal lowerUnaryExpr(const UnaryExpr &expr);

    /// @brief Lower a binary expression.
    /// @param expr Binary expression to dispatch by operand domain.
    /// @return Lowered value and IL type.
    [[nodiscard]] RVal lowerBinaryExpr(const BinaryExpr &expr);

    /// @brief Lower a builtin function call expression.
    /// @param expr Builtin call dispatched through the callback registry.
    /// @return Lowered builtin result and IL type.
    [[nodiscard]] RVal lowerBuiltinCall(const BuiltinCallExpr &expr);

    /// @brief Lower a UBOUND query expression.
    /// @param expr Array upper-bound query.
    /// @return I64 upper-bound result.
    [[nodiscard]] RVal lowerUBoundExpr(const UBoundExpr &expr);

    /// @brief Lower logical (AND/OR) expressions with short-circuiting.
    /// @param expr Logical binary expression.
    /// @return Lowered logical value and IL type.
    [[nodiscard]] RVal lowerLogicalBinary(const BinaryExpr &expr);

    /// @brief Lower integer division and modulo with divide-by-zero check.
    /// @param expr Integer division or modulo expression.
    /// @return Lowered quotient/remainder and IL type.
    [[nodiscard]] RVal lowerDivOrMod(const BinaryExpr &expr);

    /// @brief Lower string concatenation and equality/inequality comparisons.
    /// @param expr String-domain binary expression.
    /// @param lhs Already-lowered left operand, consumed by the operation.
    /// @param rhs Already-lowered right operand, consumed by the operation.
    /// @return Lowered string or comparison result.
    [[nodiscard]] RVal lowerStringBinary(const BinaryExpr &expr, RVal lhs, RVal rhs);

    /// @brief Lower numeric arithmetic and comparisons.
    /// @param expr Numeric binary expression.
    /// @param lhs Already-lowered left operand, consumed by the operation.
    /// @param rhs Already-lowered right operand, consumed by the operation.
    /// @return Lowered numeric or comparison result.
    [[nodiscard]] RVal lowerNumericBinary(const BinaryExpr &expr, RVal lhs, RVal rhs);

    /// @brief Lower the power operator by invoking the runtime helper.
    /// @param expr Power expression supplying the source location.
    /// @param lhs Already-lowered base operand.
    /// @param rhs Already-lowered exponent operand.
    /// @return F64 power result.
    [[nodiscard]] RVal lowerPowBinary(const BinaryExpr &expr, RVal lhs, RVal rhs);

  private:
    /// Borrowed forwarding facade for the active lowerer.
    Lowerer::DetailAccess access_;
};

//===----------------------------------------------------------------------===//
// Control Flow Lowering Helpers
//===----------------------------------------------------------------------===//

/// @brief Helper class coordinating control flow statement lowering.
/// @details Handles IF, WHILE, DO, FOR, SELECT CASE, GOTO, GOSUB, and related
///          control flow constructs. Manages block creation and branching.
/// @invariant Preserves CFG validity (single terminator per block).
/// @ownership Borrows Lowerer reference; does not own AST nodes.
class ControlLoweringHelper {
  public:
    /// @brief Creates a control-flow facade over a caller-owned Lowerer.
    /// @param access Authorized forwarding facade for the active lowerer.
    explicit ControlLoweringHelper(Lowerer::DetailAccess access) noexcept;

    /// @brief Lower an IF statement with optional ELSEIF/ELSE branches.
    /// @param stmt Conditional statement to lower.
    void lowerIf(const IfStmt &stmt);

    /// @brief Lower a WHILE loop.
    /// @param stmt Pre-test loop to lower.
    void lowerWhile(const WhileStmt &stmt);

    /// @brief Lower a DO loop (DO WHILE/DO UNTIL variants).
    /// @param stmt DO loop to lower.
    void lowerDo(const DoStmt &stmt);

    /// @brief Lower a FOR loop with bounds and step.
    /// @param stmt Counting loop to lower.
    void lowerFor(const ForStmt &stmt);

    /// @brief Lower a FOR EACH array iteration loop.
    /// @param stmt Array iteration loop to lower.
    void lowerForEach(const ForEachStmt &stmt);

    /// @brief Lower a SELECT CASE statement.
    /// @param stmt Multi-arm selection statement to lower.
    void lowerSelectCase(const SelectCaseStmt &stmt);

    /// @brief Lower a NEXT statement (FOR loop increment).
    /// @param stmt Loop continuation statement to lower.
    void lowerNext(const NextStmt &stmt);

    /// @brief Lower an EXIT statement (loop/procedure exit).
    /// @param stmt Exit statement selecting the target construct.
    void lowerExit(const ExitStmt &stmt);

    /// @brief Lower a GOTO statement.
    /// @param stmt Direct branch statement to lower.
    void lowerGoto(const GotoStmt &stmt);

    /// @brief Lower a GOSUB statement.
    /// @param stmt Legacy subroutine branch to lower.
    void lowerGosub(const GosubStmt &stmt);

    /// @brief Lower a GOSUB RETURN statement.
    /// @param stmt Return node marked as a GOSUB return.
    void lowerGosubReturn(const ReturnStmt &stmt);

    /// @brief Lower an ON ERROR GOTO handler.
    /// @param stmt Error-handler installation or clearing directive.
    void lowerOnErrorGoto(const OnErrorGoto &stmt);

    /// @brief Lower a RESUME statement.
    /// @param stmt Resume mode and optional target line.
    void lowerResume(const Resume &stmt);

    /// @brief Lower an END statement.
    /// @param stmt Program termination statement.
    void lowerEnd(const EndStmt &stmt);

    /// @brief Lower a TRY/CATCH statement.
    /// @param stmt TRY/CATCH/FINALLY construct to lower.
    void lowerTryCatch(const TryCatchStmt &stmt);

  private:
    /// Borrowed forwarding facade for the active lowerer.
    Lowerer::DetailAccess access_;
};

//===----------------------------------------------------------------------===//
// OOP Lowering Helpers
//===----------------------------------------------------------------------===//

/// @brief Helper class coordinating OOP construct lowering.
/// @details Handles NEW expressions, ME references, member access, method calls,
///          DELETE statements, and class/constructor/method emission.
/// @invariant Maintains class layout consistency during lowering.
/// @ownership Borrows Lowerer reference and OOP context; does not own AST nodes.
class OopLoweringHelper {
  public:
    /// @brief Creates an OOP facade over a caller-owned Lowerer.
    /// @param access Authorized forwarding facade for the active lowerer.
    explicit OopLoweringHelper(Lowerer::DetailAccess access) noexcept;

    /// @brief Lower a NEW expression allocating a BASIC object instance.
    /// @param expr Object allocation and constructor arguments.
    /// @return Pointer-valued allocation result.
    [[nodiscard]] RVal lowerNewExpr(const NewExpr &expr);
    /// @brief Lowers NEW using an explicit OOP context.
    /// @param expr Object allocation and constructor arguments.
    /// @param ctx OOP state used for class resolution and emission.
    /// @return Pointer-valued allocation result.
    [[nodiscard]] RVal lowerNewExpr(const NewExpr &expr, OopLoweringContext &ctx);

    /// @brief Lower a ME expression referencing the implicit instance slot.
    /// @param expr ME reference and source location.
    /// @return Pointer-valued current-instance result.
    [[nodiscard]] RVal lowerMeExpr(const MeExpr &expr);
    /// @brief Lowers ME using an explicit OOP context.
    /// @param expr ME reference and source location.
    /// @param ctx OOP state used to resolve the current instance.
    /// @return Pointer-valued current-instance result.
    [[nodiscard]] RVal lowerMeExpr(const MeExpr &expr, OopLoweringContext &ctx);

    /// @brief Lower a member access reading a field from an object instance.
    /// @param expr Member access to resolve.
    /// @return Loaded field/property value and IL type.
    [[nodiscard]] RVal lowerMemberAccessExpr(const MemberAccessExpr &expr);
    /// @brief Lowers member access using an explicit OOP context.
    /// @param expr Member access to resolve.
    /// @param ctx OOP state used for layout and property resolution.
    /// @return Loaded field/property value and IL type.
    [[nodiscard]] RVal lowerMemberAccessExpr(const MemberAccessExpr &expr, OopLoweringContext &ctx);

    /// @brief Lower an object method invocation expression.
    /// @param expr Method call to resolve and emit.
    /// @return Method result and IL type.
    [[nodiscard]] RVal lowerMethodCallExpr(const MethodCallExpr &expr);
    /// @brief Lowers a method call using an explicit OOP context.
    /// @param expr Method call to resolve and emit.
    /// @param ctx OOP state used for dispatch resolution.
    /// @return Method result and IL type.
    [[nodiscard]] RVal lowerMethodCallExpr(const MethodCallExpr &expr, OopLoweringContext &ctx);

    /// @brief Lower a DELETE statement releasing an object reference.
    /// @param stmt Object release statement.
    void lowerDelete(const DeleteStmt &stmt);
    /// @brief Lowers DELETE using an explicit OOP context.
    /// @param stmt Object release statement.
    /// @param ctx OOP state used for destructor and release emission.
    void lowerDelete(const DeleteStmt &stmt, OopLoweringContext &ctx);

    /// @brief Scan program OOP constructs to populate class layouts and runtime requests.
    /// @param prog Parsed program whose OOP declarations are scanned.
    void scanOOP(const Program &prog);

    /// @brief Emit constructor, destructor, and method bodies for CLASS declarations.
    /// @param prog Parsed program whose OOP bodies are emitted.
    void emitOopDeclsAndBodies(const Program &prog);

  private:
    /// Borrowed forwarding facade for the active lowerer.
    Lowerer::DetailAccess access_;
};

//===----------------------------------------------------------------------===//
// Runtime Helpers
//===----------------------------------------------------------------------===//

/// @brief Helper class coordinating runtime statement lowering.
/// @details Handles DIM, REDIM, LET, CONST, STATIC, SWAP, RANDOMIZE, and other
///          runtime-related statements that interact with memory and state.
/// @invariant Preserves symbol table and slot consistency.
/// @ownership Borrows Lowerer reference; does not own AST nodes.
class RuntimeLoweringHelper {
  public:
    /// @brief Creates a runtime-statement facade over a caller-owned Lowerer.
    /// @param access Authorized forwarding facade for the active lowerer.
    explicit RuntimeLoweringHelper(Lowerer::DetailAccess access) noexcept;

    /// @brief Lower a LET assignment statement.
    /// @param stmt Assignment to lower.
    void lowerLet(const LetStmt &stmt);

    /// @brief Lower a CONST statement.
    /// @param stmt Constant declaration to materialize.
    void lowerConst(const ConstStmt &stmt);

    /// @brief Lower a STATIC statement.
    /// @param stmt Persistent-local declaration to lower.
    void lowerStatic(const StaticStmt &stmt);

    /// @brief Lower a DIM statement.
    /// @param stmt Variable or array declaration to lower.
    void lowerDim(const DimStmt &stmt);

    /// @brief Lower a REDIM statement.
    /// @param stmt Array resize operation to lower.
    void lowerReDim(const ReDimStmt &stmt);

    /// @brief Lower a RANDOMIZE statement.
    /// @param stmt Random-generator seeding statement.
    void lowerRandomize(const RandomizeStmt &stmt);

    /// @brief Lower a SWAP statement.
    /// @param stmt Pair of lvalues to exchange.
    void lowerSwap(const SwapStmt &stmt);

  private:
    /// Borrowed forwarding facade for the active lowerer.
    Lowerer::DetailAccess access_;
};

} // namespace lower::detail
} // namespace il::frontends::basic
