//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ControlStatementLowerer.hpp
// Purpose: Declares ControlStatementLowerer, which handles lowering of
//          BASIC control flow statements (GOSUB, GOTO, RETURN, END) to IL.
//          Extracted from Lowerer to demonstrate modular extraction pattern.
// Key invariants: Operates on Lowerer context; maintains deterministic block graphs
// Ownership/Lifetime: Borrows Lowerer reference; does not own AST or IR
// Links: src/frontends/basic/ControlStatementLowerer.cpp,
//        src/frontends/basic/Lowerer.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ControlStatementLowerer.hpp
 * @brief Declares IL lowering for BASIC jump and termination statements.
 *
 * The helper delegates all state, block creation, instruction emission, runtime
 * requirements, and naming to one borrowed Lowerer.
 */

#pragma once

#include "frontends/basic/AST.hpp"

namespace il::frontends::basic {

class Lowerer;

/// @brief Handles lowering of BASIC control flow statements to IL branches.
/// @invariant All methods operate on lowerer_'s active procedure context.
/// @ownership Borrows lowerer_ for the object's full lifetime.
class ControlStatementLowerer {
  public:
    /// @brief Construct a control statement lowerer bound to a Lowerer instance.
    /// @param lowerer Parent lowerer providing context and helper methods.
    /// @warning @p lowerer must outlive this helper.
    explicit ControlStatementLowerer(Lowerer &lowerer);

    /// @brief Lower GOSUB statement for subroutine calls.
    /// @details Ensures the fixed-depth continuation stack, emits overflow
    ///          trapping, stores a continuation index, increments the stack
    ///          pointer, and branches to the resolved target-line block.
    /// @param stmt GOSUB statement supplying source location and target line.
    /// @note Returns without emission when no function/current block is active.
    void lowerGosub(const GosubStmt &stmt);

    /// @brief Lower GOTO statement for unconditional jumps.
    /// @param stmt GOTO statement whose target label is resolved through the
    ///             active procedure's line-block map.
    /// @note An unresolved target emits no branch.
    /// @pre A resolved target requires an active function.
    void lowerGoto(const GotoStmt &stmt);

    /// @brief Lower RETURN statement from GOSUB.
    /// @details Emits empty-stack trapping, pops a 32-bit continuation index,
    ///          and switches to the registered continuation block, with an
    ///          invalid-index trap as the default destination.
    /// @param stmt RETURN statement supplying the emitted switch location.
    /// @note Returns without emission when no function/current block is active.
    void lowerGosubReturn(const ReturnStmt &stmt);

    /// @brief Lower END statement to terminate program.
    /// @details Emits `ret 0` when the active function returns i64; otherwise
    ///          emits a trap.
    /// @param stmt END statement supplying the active source location.
    void lowerEnd(const EndStmt &stmt);

    ///< Borrowed parent lowering context used by every operation.
    Lowerer &lowerer_; ///< Parent lowerer providing context and helpers
};

} // namespace il::frontends::basic
