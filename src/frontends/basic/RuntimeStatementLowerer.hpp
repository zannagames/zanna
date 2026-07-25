//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/RuntimeStatementLowerer.hpp
// Purpose: Declares RuntimeStatementLowerer class that handles lowering of
//          BASIC runtime statements (terminal control, assignments, variable
//          declarations, etc.) to IL.
//
// What BASIC syntax it handles:
//   - Terminal control: BEEP, CLS, COLOR, LOCATE, CURSOR, ALTSCREEN, SLEEP
//   - Variable assignment: LET var = expr, arr(i) = expr, obj.field = expr
//   - Variable declarations: DIM, REDIM, CONST, STATIC
//   - Miscellaneous: RANDOMIZE, SWAP
//
// Invariants expected from Lowerer/LoweringContext:
//   - Active procedure context must have a valid function and current block
//   - Symbol table must be populated with variable/parameter metadata
//   - Runtime feature tracker must be available for helper requests
//
// IL Builder interaction:
//   - Emits runtime helper calls (rt_bell, rt_term_*, rt_arr_*, etc.)
//   - Uses Lowerer's emit* methods for instructions and control flow
//   - Creates auxiliary basic blocks for bounds checking
//
// Key invariants: Operates on Lowerer context; manages runtime feature requests
// Ownership/Lifetime: Borrows Lowerer reference; does not own AST or IR
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file RuntimeStatementLowerer.hpp
/// @brief Declares runtime-backed statement and assignment lowering for BASIC.
/// @details The helper borrows Lowerer state and groups terminal calls,
///          lifetime-aware assignments, array allocation/resizing, declarations,
///          and miscellaneous runtime statements.

#pragma once

#include "frontends/basic/AST.hpp"
#include "frontends/basic/Lowerer.hpp"

namespace il::support {
struct SourceLoc;
}

namespace il::frontends::basic {

/// @brief Handles lowering of BASIC runtime statements to IL runtime calls.
///
/// @details This class encapsulates the lowering logic for BASIC statements that
///          require runtime library support rather than pure IL instruction generation.
///          It handles terminal control statements (BEEP, CLS, COLOR, etc.), variable
///          assignment (including array element and object field assignment), and
///          variable declaration statements (DIM, REDIM, CONST, STATIC).
///
/// The class demonstrates the modular extraction pattern used in the BASIC frontend:
/// complex lowering concerns are factored into dedicated helper classes that
/// coordinate with the main Lowerer through its public/friend interface.
///
/// @invariant All methods operate on the Lowerer's active procedure context
/// @invariant The lowerer's current block is valid and not terminated before calls
/// @invariant The borrowed Lowerer outlives this helper.
class RuntimeStatementLowerer {
  public:
    /// @brief Construct a runtime statement lowerer bound to a Lowerer instance.
    /// @param lowerer Borrowed parent providing context and helper methods.
    /// @pre @p lowerer outlives the helper.
    explicit RuntimeStatementLowerer(Lowerer &lowerer);

    /// @brief Lower BEEP to the terminal bell runtime helper.
    /// @param s BEEP statement providing source location.
    void visit(const BeepStmt &s);
    /// @brief Lower CLS to the terminal clear-screen runtime helper.
    /// @param s CLS statement providing source location.
    void visit(const ClsStmt &s);
    /// @brief Lower COLOR with required foreground and optional background values.
    /// @param s COLOR statement to lower.
    void visit(const ColorStmt &s);
    /// @brief Lower LOCATE with required row and optional column values.
    /// @param s LOCATE statement to lower.
    void visit(const LocateStmt &s);
    /// @brief Lower CURSOR visibility to the terminal runtime helper.
    /// @param s CURSOR statement to lower.
    void visit(const CursorStmt &s);
    /// @brief Lower ALTSCREEN enablement to the terminal runtime helper.
    /// @param s ALTSCREEN statement to lower.
    void visit(const AltScreenStmt &s);
    /// @brief Lower SLEEP after coercing its duration to I64 milliseconds.
    /// @param s SLEEP statement to lower.
    void visit(const SleepStmt &s);

    /// @brief Store a value into one scalar slot with coercion and lifetime handling.
    /// @details Retains/releases string or object values as needed and emits
    ///          object destruction/free control flow for an old final reference.
    /// @param slotInfo Target type, boolean, array, and object metadata.
    /// @param slot Pointer to target storage.
    /// @param value Already-lowered right-hand value.
    /// @param loc Source location for conversions and emitted calls.
    void assignScalarSlot(const Lowerer::SlotType &slotInfo,
                          Lowerer::Value slot,
                          Lowerer::RVal value,
                          il::support::SourceLoc loc);
    /// @brief Store a value into an array element through resolved runtime metadata.
    /// @param target Array expression containing name and indices.
    /// @param value Already-lowered right-hand value.
    /// @param loc Source location for access checks and helper calls.
    void assignArrayElement(const ArrayExpr &target,
                            Lowerer::RVal value,
                            il::support::SourceLoc loc);
    /// @brief Lower one LET statement by dispatching on its target expression kind.
    /// @param stmt LET statement containing non-null target and expression after valid parsing.
    void lowerLet(const LetStmt &stmt);
    /// @brief Assign an already-lowered value to a scalar or whole-array variable.
    /// @param stmt Parent LET statement.
    /// @param var Target variable reference.
    /// @param value Already-lowered right-hand value.
    void lowerLetToVar(const LetStmt &stmt, const VarExpr &var, Lowerer::RVal value);
    /// @brief Assign through method-call-shaped object field-array syntax.
    /// @param stmt Parent LET statement.
    /// @param mc Target carrying receiver, field name, and indices.
    /// @param value Already-lowered right-hand value.
    void lowerLetToMethodCall(const LetStmt &stmt, const MethodCallExpr &mc, Lowerer::RVal value);
    /// @brief Assign through call-shaped implicit field-array syntax.
    /// @param stmt Parent LET statement.
    /// @param call Target carrying implicit field name and indices.
    /// @param value Already-lowered right-hand value.
    void lowerLetToCall(const LetStmt &stmt, const CallExpr &call, Lowerer::RVal value);
    /// @brief Assign to an ordinary array element.
    /// @param stmt Parent LET statement.
    /// @param arr Array-element target.
    /// @param value Already-lowered right-hand value.
    void lowerLetToArray(const LetStmt &stmt, const ArrayExpr &arr, Lowerer::RVal value);
    /// @brief Assign to a field, runtime/user property, or static class member.
    /// @param stmt Parent LET statement.
    /// @param member Member-access target with a non-null base expression.
    /// @param value Already-lowered right-hand value.
    void lowerLetToMember(const LetStmt &stmt, const MemberAccessExpr &member, Lowerer::RVal value);

    /// @brief Evaluate and store a CONST initializer in its allocated slot.
    /// @param stmt CONST declaration to lower.
    void lowerConst(const ConstStmt &stmt);
    /// @brief Treat a STATIC declaration as an emission-time no-op.
    /// @details Collection records persistence metadata; later variable uses
    ///          resolve procedure-qualified runtime modvar storage.
    /// @param stmt STATIC declaration already processed during collection.
    void lowerStatic(const StaticStmt &stmt);
    /// @brief Allocate and store a DIM array using inclusive BASIC bounds.
    /// @param stmt DIM declaration to lower.
    void lowerDim(const DimStmt &stmt);
    /// @brief Resize an existing array and replace its stored handle.
    /// @param stmt REDIM declaration to lower.
    void lowerReDim(const ReDimStmt &stmt);

    /// @brief Lower RANDOMIZE using its optional seed or zero.
    /// @param stmt RANDOMIZE statement to lower.
    void lowerRandomize(const RandomizeStmt &stmt);
    /// @brief Exchange two lvalue slots through a temporary.
    /// @param stmt SWAP statement to lower.
    void lowerSwap(const SwapStmt &stmt);

    /// @brief Convert an inclusive BASIC upper bound to a validated array length.
    /// @details Adds one with overflow checking and, in an active function/block,
    ///          emits negative-length trap and SSA continuation blocks.
    /// @param bound Lowered inclusive upper bound.
    /// @param loc Source location for checked arithmetic and trap emission.
    /// @param labelBase Prefix for deterministic generated block names.
    /// @return Continuation block parameter when control flow is emitted,
    ///         otherwise the checked `bound + 1` value.
    Lowerer::Value emitArrayLengthCheck(Lowerer::Value bound,
                                        il::support::SourceLoc loc,
                                        std::string_view labelBase);

    /// @brief Coerce a lowered value to a BASIC scalar type expected by a setter.
    /// @param value Lowered value to coerce.
    /// @param target Declared BASIC scalar type.
    /// @param loc Source location for conversion diagnostics.
    /// @return Coerced value when a supported conversion exists.
    Lowerer::RVal coerceToAstScalar(Lowerer::RVal value, Type target, il::support::SourceLoc loc);

    /// Borrowed parent lowerer providing all context and emission helpers.
    Lowerer &lowerer_; ///< Parent lowerer providing context and helpers
};

} // namespace il::frontends::basic
