//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares BASIC I/O statement lowering entry points.
/// @details These declarations describe the Lowerer-facing helpers that lower
///          BASIC I/O statements (OPEN/CLOSE/SEEK/PRINT/INPUT and channel
///          variants). The implementations live in `LowerStmt_IO.cpp` as thin
///          forwarding wrappers to @ref IoStatementLowerer, preserving the
///          Lowerer interface while keeping the I/O lowering logic modular.
///          Each helper evaluates statement operands, performs required
///          coercions (such as channel normalization), and emits the runtime
///          calls or control-flow needed to implement BASIC I/O semantics.
/// @note This is a class-body declaration fragment rather than a standalone
///       namespace-level interface.
//
//===----------------------------------------------------------------------===//

#pragma once

/// @brief Lower an OPEN statement to runtime I/O setup.
/// @details Evaluates the channel, filename, and mode operands, normalizes the
///          channel to the runtime representation, and emits the appropriate
///          runtime helper calls and error checks. Implemented as a delegating
///          wrapper to @ref IoStatementLowerer.
/// @param stmt Parsed OPEN statement.
void lowerOpen(const OpenStmt &stmt);

/// @brief Lower a CLOSE statement to runtime channel teardown.
/// @details Emits runtime calls to close one or more file channels and performs
///          any necessary runtime error checks. Implemented as a delegating
///          wrapper to @ref IoStatementLowerer.
/// @param stmt Parsed CLOSE statement.
void lowerClose(const CloseStmt &stmt);

/// @brief Lower a SEEK statement to reposition a file channel.
/// @details Evaluates the target channel and position expression, normalizes
///          channel types, and emits the runtime seek helper with error
///          checking. Implemented as a delegating wrapper to
///          @ref IoStatementLowerer.
/// @param stmt Parsed SEEK statement.
void lowerSeek(const SeekStmt &stmt);

/// @brief Lower a PRINT statement that targets the console.
/// @details Emits runtime output calls for each print item, preserving BASIC
///          separator semantics (comma/semicolon/newline) and string formatting
///          rules. Implemented as a delegating wrapper to @ref IoStatementLowerer.
/// @param stmt Parsed PRINT statement.
void lowerPrint(const PrintStmt &stmt);

/// @brief Lower a PRINT# statement that targets a file channel.
/// @details Evaluates the channel and print items, normalizes the channel type,
///          and emits runtime write calls that mirror BASIC file output
///          semantics. Implemented as a delegating wrapper to
///          @ref IoStatementLowerer.
/// @param stmt Parsed PRINT# statement.
void lowerPrintCh(const PrintChStmt &stmt);

/// @brief Lower an INPUT statement that reads from the console.
/// @details Emits runtime input calls and stores the parsed values into the
///          target variables according to BASIC input parsing rules. Implemented
///          as a delegating wrapper to @ref IoStatementLowerer.
/// @param stmt Parsed INPUT statement.
void lowerInput(const InputStmt &stmt);

/// @brief Lower an INPUT# statement that reads from a file channel.
/// @details Evaluates the channel and reads one line for each target, then
///          parses or assigns that line according to the target's scalar type.
///          Implemented as a delegating wrapper to @ref IoStatementLowerer.
/// @param stmt Parsed INPUT# statement.
void lowerInputCh(const InputChStmt &stmt);

/// @brief Lower a LINE INPUT# statement for line-based file input.
/// @details Reads a full line from the specified channel using runtime helpers
///          and assigns the resulting string to the target variable. Implemented
///          as a delegating wrapper to @ref IoStatementLowerer.
/// @param stmt Parsed LINE INPUT# statement.
void lowerLineInputCh(const LineInputChStmt &stmt);
