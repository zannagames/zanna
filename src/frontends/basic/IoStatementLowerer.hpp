//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/IoStatementLowerer.hpp
// Purpose: Declares IoStatementLowerer, which handles lowering of BASIC
//          I/O statements (PRINT, INPUT, OPEN, CLOSE, SEEK, etc.) to IL.
//          Extracted from Lowerer to demonstrate modular extraction pattern.
// Key invariants: Operates on Lowerer context; delegates to runtime I/O functions
// Ownership/Lifetime: Borrows Lowerer reference; does not own AST or IR
// Links: src/frontends/basic/IoStatementLowerer.cpp,
//        src/frontends/basic/Lowerer.hpp,
//        src/frontends/basic/RuntimeCallHelpers.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file IoStatementLowerer.hpp
 * @brief Declares terminal and channel I/O lowering for BASIC statements.
 *
 * The helper lowers AST operands through one borrowed Lowerer and emits runtime
 * calls, conversions, string ownership operations, and error-to-trap paths.
 */

#pragma once

#include "frontends/basic/AST.hpp"

namespace il::frontends::basic {

class Lowerer;

/// @brief Handles lowering of BASIC I/O statements to IL runtime calls.
/// @invariant All methods operate on lowerer_'s active procedure context.
/// @ownership Borrows lowerer_ for the object's full lifetime.
class IoStatementLowerer {
  public:
    /// @brief Construct an I/O statement lowerer bound to a Lowerer instance.
    /// @param lowerer Parent lowerer providing context and helper methods.
    /// @warning @p lowerer must outlive this helper.
    explicit IoStatementLowerer(Lowerer &lowerer);

    /// @brief Lower OPEN statement for file I/O.
    /// @details Lowers path and channel, passes the encoded mode, calls
    ///          `rt_open_err_vstr`, and traps through the shared error path.
    /// @param stmt OPEN statement AST node containing filename, mode, and channel.
    /// @note Missing path or channel expressions produce no IL.
    void lowerOpen(const OpenStmt &stmt);

    /// @brief Lower CLOSE statement to close file channels.
    /// @details Normalizes the channel to i32 and calls `rt_close_err` with
    ///          runtime error checking.
    /// @param stmt CLOSE statement AST node containing the channel to close.
    /// @note A missing channel expression produces no IL.
    void lowerClose(const CloseStmt &stmt);

    /// @brief Lower SEEK statement for file positioning.
    /// @details Normalizes the channel, coerces the position to i64, and calls
    ///          `rt_seek_ch_err` with runtime error checking.
    /// @param stmt SEEK statement AST node containing channel and byte offset.
    /// @note Missing channel or position expressions produce no IL.
    void lowerSeek(const SeekStmt &stmt);

    /// @brief Lower PRINT statement for console output.
    /// @details Selects terminal helpers by lowered type, manages temporary
    ///          string ownership, emits 14-column comma zones, and appends a
    ///          newline unless the last item is a semicolon.
    /// @param stmt PRINT statement AST node containing the expression list to print.
    void lowerPrint(const PrintStmt &stmt);

    /// @brief Lower PRINT# statement for file output.
    /// @details WRITE mode quotes strings and builds one comma-delimited record.
    ///          PRINT mode streams non-null arguments and applies the statement's
    ///          trailing-newline flag. Every runtime status is checked.
    /// @param stmt PRINT# statement AST node containing channel and expression list.
    /// @note A missing channel expression produces no IL.
    void lowerPrintCh(const PrintChStmt &stmt);

    /// @brief Lower INPUT statement for console input.
    /// @details Prints only a literal-string prompt, reads one line, optionally
    ///          splits it to the target count, converts fields by slot type, and
    ///          transfers or releases string values appropriately.
    /// @param stmt INPUT statement AST node containing variable targets and optional prompt.
    /// @note With no target variables, prompt emission may occur but no input is read.
    void lowerInput(const InputStmt &stmt);

    /// @brief Lower INPUT# statement for file input.
    /// @details Reads one channel line, splits to the number of targets (or one
    ///          field when none), parses numeric/Boolean fields, and stores
    ///          strings directly.
    /// @param stmt INPUT# statement AST node containing channel and variable targets.
    void lowerInputCh(const InputChStmt &stmt);

    /// @brief Lower LINE INPUT# statement for line-based file input.
    /// @details Reads one complete line and stores it only when the target is a
    ///          resolvable VarExpr.
    /// @param stmt LINE INPUT# statement AST node containing channel and target variable.
    /// @note Missing operands produce no IL.
    void lowerLineInputCh(const LineInputChStmt &stmt);

    /// Borrowed parent lowerer; public so translation-unit helper functions can
    /// share the same emission context.
    Lowerer
        &lowerer_; ///< Parent lowerer providing context and helpers (public for file-local helpers)
};

} // namespace il::frontends::basic
