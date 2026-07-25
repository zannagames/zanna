//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares runtime-oriented statement lowering entry points.
/// @details This header declares the Lowerer-facing helpers that implement
///          BASIC statements requiring runtime support: assignments, variable
///          declarations, terminal control, and miscellaneous runtime services
///          such as RANDOMIZE and SWAP. The definitions in
///          `LowerStmt_Runtime.cpp` forward to @ref RuntimeStatementLowerer,
///          keeping the Lowerer interface stable while delegating the heavy
///          lifting to the modular runtime statement lowering class.
/// @note This is a class-body declaration fragment rather than a standalone
///       namespace-level interface.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string_view>

/// @brief Lower a LET assignment statement.
/// @details Resolves the target l-value (scalar, array element, or field),
///          lowers the right-hand side expression, and performs BASIC-compatible
///          coercions and lifetime handling before emitting stores. Implemented
///          as a delegating wrapper to @ref RuntimeStatementLowerer.
/// @param stmt Parsed LET statement.
void lowerLet(const LetStmt &stmt);

/// @brief Lower a CONST declaration.
/// @details Evaluates the initializer expression and stores it in the constant's
///          storage, using the same coercion and lifetime rules as assignments.
///          Implemented as a delegating wrapper to @ref RuntimeStatementLowerer.
/// @param stmt Parsed CONST statement.
void lowerConst(const ConstStmt &stmt);

/// @brief Lower a STATIC declaration.
/// @details Emits no instructions: variable collection has already arranged
///          module-level storage, and later references resolve to it. Implemented
///          as a delegating wrapper to @ref RuntimeStatementLowerer.
/// @param stmt Parsed STATIC statement.
void lowerStatic(const StaticStmt &stmt);

/// @brief Assign an r-value to a scalar storage slot.
/// @details Applies BASIC coercion rules (integer/float/boolean), manages
///          string and object retain/release semantics, and emits the final
///          store into the provided slot. Implemented as a delegating wrapper
///          to @ref RuntimeStatementLowerer.
/// @param slotInfo Metadata describing the target slot.
/// @param slot Address/value designating the storage location.
/// @param value Lowered right-hand side value and its type.
/// @param loc Source location for diagnostics and emitted instructions.
void assignScalarSlot(const SlotType &slotInfo, Value slot, RVal value, il::support::SourceLoc loc);

/// @brief Assign an r-value to an array element.
/// @details Lowers index expressions, performs bounds checks, selects the
///          correct runtime store helper for the element type, and manages
///          lifetime rules for string/object arrays. Implemented as a delegating
///          wrapper to @ref RuntimeStatementLowerer.
/// @param target Array expression describing the destination element.
/// @param value Lowered right-hand side value and its type.
/// @param loc Source location for diagnostics and emitted instructions.
void assignArrayElement(const ArrayExpr &target, RVal value, il::support::SourceLoc loc);

/// @brief Lower a DIM declaration to runtime array allocation.
/// @details Evaluates dimension expressions, normalizes bounds to BASIC's
///          inclusive size semantics, performs overflow checks, and emits runtime
///          allocation calls. Implemented as a delegating wrapper to
///          @ref RuntimeStatementLowerer.
/// @param stmt Parsed DIM statement.
void lowerDim(const DimStmt &stmt);

/// @brief Lower a REDIM declaration to resize an existing array.
/// @details Evaluates and flattens the new inclusive bounds, selects the object,
///          floating, or integer-array resize helper, replaces the stored handle,
///          and updates the optional bounds-check length slot. Implemented as a
///          delegating wrapper to @ref RuntimeStatementLowerer.
/// @param stmt Parsed REDIM statement.
void lowerReDim(const ReDimStmt &stmt);

/// @brief Lower a RANDOMIZE statement to runtime seeding.
/// @details Evaluates the optional seed expression and emits the corresponding
///          runtime call to reseed the PRNG. Implemented as a delegating wrapper
///          to @ref RuntimeStatementLowerer.
/// @param stmt Parsed RANDOMIZE statement.
void lowerRandomize(const RandomizeStmt &stmt);

/// @brief Lower a SWAP statement.
/// @details Evaluates both operands, saves the left value in an eight-byte
///          temporary, and assigns in reverse order when each destination is a
///          variable or array element. Implemented as a delegating wrapper to
///          @ref RuntimeStatementLowerer.
/// @param stmt Parsed SWAP statement.
void lowerSwap(const SwapStmt &stmt);

/// @brief Lower a BEEP statement to terminal control runtime calls.
/// @details Emits the runtime helper that triggers the audible beep. Implemented
///          as a delegating wrapper to @ref RuntimeStatementLowerer.
/// @param stmt Parsed BEEP statement.
void visit(const BeepStmt &stmt);

/// @brief Lower a CLS statement to clear the terminal.
/// @details Emits runtime calls to clear the screen and reset terminal state.
///          Implemented as a delegating wrapper to @ref RuntimeStatementLowerer.
/// @param stmt Parsed CLS statement.
void visit(const ClsStmt &stmt);

/// @brief Lower a COLOR statement to update terminal colors.
/// @details Evaluates foreground/background expressions and emits the runtime
///          helper that updates the terminal palette. Implemented as a delegating
///          wrapper to @ref RuntimeStatementLowerer.
/// @param stmt Parsed COLOR statement.
void visit(const ColorStmt &stmt);

/// @brief Lower a SLEEP statement to runtime delay.
/// @details Evaluates the delay expression and emits the runtime sleep helper.
///          Implemented as a delegating wrapper to @ref RuntimeStatementLowerer.
/// @param stmt Parsed SLEEP statement.
void visit(const SleepStmt &stmt);

/// @brief Lower a LOCATE statement to move the cursor.
/// @details Evaluates row/column arguments and emits the runtime helper that
///          positions the cursor. Implemented as a delegating wrapper to
///          @ref RuntimeStatementLowerer.
/// @param stmt Parsed LOCATE statement.
void visit(const LocateStmt &stmt);

/// @brief Lower a CURSOR statement to toggle cursor visibility.
/// @details Emits the runtime helper that shows or hides the cursor based on
///          the statement operands. Implemented as a delegating wrapper to
///          @ref RuntimeStatementLowerer.
/// @param stmt Parsed CURSOR statement.
void visit(const CursorStmt &stmt);

/// @brief Lower an ALTSCREEN statement to switch display mode.
/// @details Emits runtime calls to enable or disable the alternate screen
///          buffer. Implemented as a delegating wrapper to
///          @ref RuntimeStatementLowerer.
/// @param stmt Parsed ALTSCREEN statement.
void visit(const AltScreenStmt &stmt);

/// @brief Emit a runtime-checked array length value.
/// @details Adjusts the requested bound to BASIC's inclusive length semantics,
///          emits overflow-aware addition, and, when a function and current
///          block exist, traps on a negative result before passing the length
///          into a continuation block parameter. Without active control flow it
///          returns the checked addition directly. The @p labelBase derives
///          deterministic block names. Implemented as a delegating wrapper to
///          @ref RuntimeStatementLowerer.
/// @param bound Upper-bound value provided by the user.
/// @param loc Source location for diagnostics and emitted instructions.
/// @param labelBase Prefix used when naming helper blocks.
/// @return Validated length value suitable for runtime allocation calls.
Value emitArrayLengthCheck(Value bound, il::support::SourceLoc loc, std::string_view labelBase);
