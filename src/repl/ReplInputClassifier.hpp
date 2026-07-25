//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares stateless completeness classifiers for Zia and BASIC REPL input.
/// @details Classification is deliberately shallower than parsing: it decides
///          whether to request continuation, dispatch a meta command, ignore
///          empty input, or submit accumulated source to a language adapter.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/repl/ReplInputClassifier.hpp
// Purpose: Classifies REPL input as complete, incomplete (multi-line), a
//          meta-command, or empty. Used by ReplSession to decide whether to
//          prompt for continuation or to evaluate the input.
// Key invariants:
//   - Classification is purely syntactic (bracket depth, block keywords).
//   - No full parse is required for classification.
// Ownership/Lifetime:
//   - Stateless utility; all methods are static or const.
// Links: src/repl/ReplSession.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

namespace zanna::repl {

/// @brief Classification of a REPL input line or accumulated input.
enum class InputKind {
    Complete,    ///< Input is syntactically complete; ready to compile.
    Incomplete,  ///< Input has unclosed brackets/blocks; needs continuation.
    MetaCommand, ///< Input starts with '.' (e.g., .help, .quit).
    Empty,       ///< Input is blank or whitespace-only.
};

/// @brief Classifies accumulated REPL input for Zia and BASIC.
/// @details The Zia path tracks delimiter balance outside strings/comments,
///          while the BASIC path tracks string termination and block keywords.
///          The utility owns no state between calls.
class ReplInputClassifier {
  public:
    /// @brief Classify Zia input (bracket depth tracking).
    /// @details Delimiters inside quoted strings or line comments are ignored.
    /// @param input The accumulated REPL input (may span multiple lines).
    /// @return The classification of the input.
    static InputKind classify(const std::string &input);

    /// @brief Classify BASIC input (block keyword tracking).
    /// @details Tracks openers (IF/THEN, DO, FOR, WHILE, SUB, FUNCTION, etc.)
    ///          against closers (END IF, LOOP, NEXT, WEND, END SUB, etc.).
    ///          Handles single-line IF (IF...THEN <statement>) as complete.
    /// @param input The accumulated REPL input (may span multiple lines).
    /// @return The classification of the input.
    static InputKind classifyBasic(const std::string &input);
};

} // namespace zanna::repl
