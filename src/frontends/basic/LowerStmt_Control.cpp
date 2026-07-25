//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Delegation layer for control-flow statement lowering.
/// @details Preserves the `Lowerer` entry points for BASIC control-flow
///          statements while forwarding implementation to
///          @ref ControlStatementLowerer. The wrappers exist for API stability
///          and do not introduce additional logic beyond delegation.
/// @invariant The owning Lowerer initializes ctrlStmtLowerer_ before any wrapper
///            is called.
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/ControlStatementLowerer.hpp"
#include "frontends/basic/Lowerer.hpp"

namespace il::frontends::basic {

/// @brief Forward GOSUB lowering to the control statement lowerer.
/// @details Delegates to @ref ControlStatementLowerer::lowerGosub to emit the
///          runtime call/stack manipulation required by BASIC GOSUB semantics.
/// @param stmt Parsed GOSUB statement.
void Lowerer::lowerGosub(const GosubStmt &stmt) {
    ctrlStmtLowerer_->lowerGosub(stmt);
}

/// @brief Forward GOTO lowering to the control statement lowerer.
/// @details Delegates to @ref ControlStatementLowerer::lowerGoto to resolve the
///          target label and emit the appropriate branch.
/// @param stmt Parsed GOTO statement.
void Lowerer::lowerGoto(const GotoStmt &stmt) {
    ctrlStmtLowerer_->lowerGoto(stmt);
}

/// @brief Forward RETURN (GOSUB) lowering to the control statement lowerer.
/// @details Delegates to @ref ControlStatementLowerer::lowerGosubReturn to
///          unwind the GOSUB return stack and resume execution at the caller.
/// @param stmt Parsed RETURN statement flagged as GOSUB return.
void Lowerer::lowerGosubReturn(const ReturnStmt &stmt) {
    ctrlStmtLowerer_->lowerGosubReturn(stmt);
}

/// @brief Forward END statement lowering to the control statement lowerer.
/// @details Delegates to @ref ControlStatementLowerer::lowerEnd to emit the
///          appropriate termination sequence for BASIC programs.
/// @param stmt Parsed END statement.
void Lowerer::lowerEnd(const EndStmt &stmt) {
    ctrlStmtLowerer_->lowerEnd(stmt);
}

} // namespace il::frontends::basic
