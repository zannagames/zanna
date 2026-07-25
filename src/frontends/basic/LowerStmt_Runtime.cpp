//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Delegation layer for runtime statement lowering.
/// @details Preserves the legacy `Lowerer` entry points for runtime-oriented
///          statements while forwarding the implementation to
///          @ref RuntimeStatementLowerer. The wrappers do not add extra logic;
///          they exist so callers can keep using the `Lowerer` interface while
///          the lowering code remains modular.
/// @invariant The owning Lowerer initializes runtimeStmtLowerer_ before any
///            forwarding member is invoked.
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/RuntimeStatementLowerer.hpp"

namespace il::frontends::basic {

/// @brief Forward BEEP lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::visit to emit the runtime
///          helper that triggers a terminal beep.
/// @param s Parsed BEEP statement.
void Lowerer::visit(const BeepStmt &s) {
    runtimeStmtLowerer_->visit(s);
}

/// @brief Forward CLS lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::visit to emit the runtime
///          helper that clears the terminal.
/// @param s Parsed CLS statement.
void Lowerer::visit(const ClsStmt &s) {
    runtimeStmtLowerer_->visit(s);
}

/// @brief Forward COLOR lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::visit to emit runtime
///          calls that update the terminal colors.
/// @param s Parsed COLOR statement.
void Lowerer::visit(const ColorStmt &s) {
    runtimeStmtLowerer_->visit(s);
}

/// @brief Forward LOCATE lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::visit to emit runtime
///          cursor-positioning calls.
/// @param s Parsed LOCATE statement.
void Lowerer::visit(const LocateStmt &s) {
    runtimeStmtLowerer_->visit(s);
}

/// @brief Forward CURSOR lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::visit to emit runtime
///          cursor visibility toggles.
/// @param s Parsed CURSOR statement.
void Lowerer::visit(const CursorStmt &s) {
    runtimeStmtLowerer_->visit(s);
}

/// @brief Forward ALTSCREEN lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::visit to emit runtime
///          helpers that toggle the alternate screen buffer.
/// @param s Parsed ALTSCREEN statement.
void Lowerer::visit(const AltScreenStmt &s) {
    runtimeStmtLowerer_->visit(s);
}

/// @brief Forward SLEEP lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::visit to emit runtime
///          sleep/delay helpers.
/// @param s Parsed SLEEP statement.
void Lowerer::visit(const SleepStmt &s) {
    runtimeStmtLowerer_->visit(s);
}

/// @brief Forward scalar assignment lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::assignScalarSlot to
///          perform BASIC coercions and lifetime management before storing.
/// @param slotInfo Metadata describing the target slot.
/// @param slot Storage location for the assignment.
/// @param value Lowered r-value to assign.
/// @param loc Source location for diagnostics and emitted instructions.
void Lowerer::assignScalarSlot(const SlotType &slotInfo,
                               Value slot,
                               RVal value,
                               il::support::SourceLoc loc) {
    runtimeStmtLowerer_->assignScalarSlot(slotInfo, slot, value, loc);
}

/// @brief Forward array element assignment lowering to the runtime lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::assignArrayElement to
///          compute indices, emit bounds checks, and store the element.
/// @param target Array expression describing the destination.
/// @param value Lowered r-value to assign.
/// @param loc Source location for diagnostics and emitted instructions.
void Lowerer::assignArrayElement(const ArrayExpr &target, RVal value, il::support::SourceLoc loc) {
    runtimeStmtLowerer_->assignArrayElement(target, value, loc);
}

/// @brief Forward LET statement lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::lowerLet to resolve the
///          l-value and emit the appropriate assignment logic.
/// @param stmt Parsed LET statement.
void Lowerer::lowerLet(const LetStmt &stmt) {
    runtimeStmtLowerer_->lowerLet(stmt);
}

/// @brief Forward CONST statement lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::lowerConst to evaluate
///          the initializer and store the constant value.
/// @param stmt Parsed CONST statement.
void Lowerer::lowerConst(const ConstStmt &stmt) {
    runtimeStmtLowerer_->lowerConst(stmt);
}

/// @brief Forward STATIC statement lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::lowerStatic, which
///          intentionally emits no instructions because static storage was
///          materialized during variable collection.
/// @param stmt Parsed STATIC statement.
void Lowerer::lowerStatic(const StaticStmt &stmt) {
    runtimeStmtLowerer_->lowerStatic(stmt);
}

/// @brief Forward DIM statement lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::lowerDim to evaluate
///          array bounds and emit allocation helpers.
/// @param stmt Parsed DIM statement.
void Lowerer::lowerDim(const DimStmt &stmt) {
    runtimeStmtLowerer_->lowerDim(stmt);
}

/// @brief Forward REDIM statement lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::lowerReDim to resize
///          arrays and preserve BASIC semantics.
/// @param stmt Parsed REDIM statement.
void Lowerer::lowerReDim(const ReDimStmt &stmt) {
    runtimeStmtLowerer_->lowerReDim(stmt);
}

/// @brief Forward RANDOMIZE statement lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::lowerRandomize to seed
///          the runtime RNG.
/// @param stmt Parsed RANDOMIZE statement.
void Lowerer::lowerRandomize(const RandomizeStmt &stmt) {
    runtimeStmtLowerer_->lowerRandomize(stmt);
}

/// @brief Forward SWAP statement lowering to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::lowerSwap, which
///          exchanges variable or array-element l-values through a temporary.
/// @param stmt Parsed SWAP statement.
void Lowerer::lowerSwap(const SwapStmt &stmt) {
    runtimeStmtLowerer_->lowerSwap(stmt);
}

/// @brief Forward array length checks to the runtime statement lowerer.
/// @details Delegates to @ref RuntimeStatementLowerer::emitArrayLengthCheck to
///          add one to the inclusive upper bound and, in an active function,
///          trap if the resulting length is negative.
/// @param bound Raw bound value provided by the user.
/// @param loc Source location for diagnostics and emitted instructions.
/// @param labelBase Prefix for generated block labels.
/// @return Validated length value suitable for allocation helpers.
Lowerer::Value Lowerer::emitArrayLengthCheck(Value bound,
                                             il::support::SourceLoc loc,
                                             std::string_view labelBase) {
    return runtimeStmtLowerer_->emitArrayLengthCheck(bound, loc, labelBase);
}

} // namespace il::frontends::basic
