//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Delegation layer for BASIC I/O statement lowering.
/// @details This file keeps the historical `Lowerer` entry points for I/O
///          statements while routing all work to @ref IoStatementLowerer. The
///          wrappers provide a stable interface for the rest of the frontend,
///          but the actual lowering logic (argument coercions, runtime helper
///          selection, and error handling) lives in the dedicated I/O lowerer
///          implementation.
/// @invariant The owning Lowerer initializes ioStmtLowerer_ before any forwarding
///            member is invoked.
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/IoStatementLowerer.hpp"
#include "frontends/basic/Lowerer.hpp"

namespace il::frontends::basic {

/// @brief Lower a PRINT# argument into its runtime string representation.
/// @details Implemented in `IoStatementLowerer.cpp`. The helper converts either
///          string or numeric values to a runtime string, optionally quoting
///          string values for CSV-like emission, and returns any runtime feature
///          requirement alongside the lowered string.
/// @param self I/O lowering helper that provides emission context.
/// @param expr Expression being lowered.
/// @param value Lowered r-value for the expression.
/// @param quoteStrings Whether to quote string values for CSV emission.
/// @return Lowered string plus an optional runtime feature dependency.
Lowerer::PrintChArgString lowerPrintChArgToString(IoStatementLowerer &self,
                                                  const Expr &expr,
                                                  Lowerer::RVal value,
                                                  bool quoteStrings);

/// @brief Build a comma-delimited PRINT# record string.
/// @details Implemented in `IoStatementLowerer.cpp`. The helper lowers each
///          PRINT# argument to a string, concatenates them with separators, and
///          returns a runtime string handle ready for file output.
/// @param self I/O lowering helper that provides emission context.
/// @param stmt Parsed PRINT# statement.
/// @return Runtime string value representing the formatted record.
Value buildPrintChWriteRecord(IoStatementLowerer &self, const PrintChStmt &stmt);

/// @brief Forward OPEN statement lowering to the I/O lowerer.
/// @details Delegates to @ref IoStatementLowerer::lowerOpen to perform channel
///          normalization, argument coercions, and runtime helper emission.
/// @param stmt Parsed OPEN statement.
void Lowerer::lowerOpen(const OpenStmt &stmt) {
    ioStmtLowerer_->lowerOpen(stmt);
}

/// @brief Forward CLOSE statement lowering to the I/O lowerer.
/// @details Delegates to @ref IoStatementLowerer::lowerClose to emit runtime
///          calls that close file channels and handle errors.
/// @param stmt Parsed CLOSE statement.
void Lowerer::lowerClose(const CloseStmt &stmt) {
    ioStmtLowerer_->lowerClose(stmt);
}

/// @brief Forward SEEK statement lowering to the I/O lowerer.
/// @details Delegates to @ref IoStatementLowerer::lowerSeek to normalize the
///          channel and emit the runtime seek helper.
/// @param stmt Parsed SEEK statement.
void Lowerer::lowerSeek(const SeekStmt &stmt) {
    ioStmtLowerer_->lowerSeek(stmt);
}

/// @brief Forward PRINT statement lowering to the I/O lowerer.
/// @details Delegates to @ref IoStatementLowerer::lowerPrint to emit console
///          output with BASIC's formatting and separator rules.
/// @param stmt Parsed PRINT statement.
void Lowerer::lowerPrint(const PrintStmt &stmt) {
    ioStmtLowerer_->lowerPrint(stmt);
}

/// @brief Forward PRINT# statement lowering to the I/O lowerer.
/// @details Delegates to @ref IoStatementLowerer::lowerPrintCh to format output
///          records and emit file-channel runtime calls.
/// @param stmt Parsed PRINT# statement.
void Lowerer::lowerPrintCh(const PrintChStmt &stmt) {
    ioStmtLowerer_->lowerPrintCh(stmt);
}

/// @brief Forward INPUT statement lowering to the I/O lowerer.
/// @details Delegates to @ref IoStatementLowerer::lowerInput to read from the
///          console and assign parsed values to variables.
/// @param stmt Parsed INPUT statement.
void Lowerer::lowerInput(const InputStmt &stmt) {
    ioStmtLowerer_->lowerInput(stmt);
}

/// @brief Forward INPUT# statement lowering to the I/O lowerer.
/// @details Delegates to @ref IoStatementLowerer::lowerInputCh to read values
///          from a file channel and assign them to variables.
/// @param stmt Parsed INPUT# statement.
void Lowerer::lowerInputCh(const InputChStmt &stmt) {
    ioStmtLowerer_->lowerInputCh(stmt);
}

/// @brief Forward LINE INPUT# statement lowering to the I/O lowerer.
/// @details Delegates to @ref IoStatementLowerer::lowerLineInputCh to read a
///          full line from a file channel into a string variable.
/// @param stmt Parsed LINE INPUT# statement.
void Lowerer::lowerLineInputCh(const LineInputChStmt &stmt) {
    ioStmtLowerer_->lowerLineInputCh(stmt);
}

/// @brief Lower a PRINT# argument using the shared I/O helper.
/// @details Forwards to the free helper in `IoStatementLowerer.cpp` to convert
///          the expression into a runtime string representation, preserving the
///          quoting behavior required by the caller.
/// @param expr Expression being lowered.
/// @param value Lowered r-value for the expression.
/// @param quoteStrings Whether to quote string values for CSV emission.
/// @return Lowered string plus an optional runtime feature dependency.
Lowerer::PrintChArgString Lowerer::lowerPrintChArgToString(const Expr &expr,
                                                           RVal value,
                                                           bool quoteStrings) {
    return ::il::frontends::basic::lowerPrintChArgToString(
        *ioStmtLowerer_, expr, value, quoteStrings);
}

/// @brief Build a PRINT# record string using the shared I/O helper.
/// @details Forwards to the free helper in `IoStatementLowerer.cpp` to lower
///          each argument, concatenate the resulting strings, and return the
///          record value used by file output routines.
/// @param stmt Parsed PRINT# statement.
/// @return Runtime string value representing the formatted record.
Value Lowerer::buildPrintChWriteRecord(const PrintChStmt &stmt) {
    return ::il::frontends::basic::buildPrintChWriteRecord(*ioStmtLowerer_, stmt);
}

} // namespace il::frontends::basic
