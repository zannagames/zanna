//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/internal/io/FunctionParser.hpp
// Purpose: Declares helpers for parsing IL function definitions.
// Key invariants: Requires ParserState to track current function and block context.
// Ownership/Lifetime: Populates the module held by ParserState with parsed functions.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares internal parsers for IL functions and basic-block headers.
 *
 * @details These helpers consume textual function structure into the module
 *          owned by `ParserState`, update the active function/block context,
 *          and return structured diagnostics for syntax or resource failures.
 *          Whole-function parsing is transactional and rolls back incomplete
 *          state when the body cannot be completed.
 */

#pragma once

#include "il/internal/io/ParserState.hpp"
#include "support/diag_expected.hpp"

#include <istream>
#include <string>

namespace il::io::detail {

/// @brief Parse a function header introducing parameters and return type.
/// @param header Complete function declaration line.
/// @param st Mutable parser state receiving the new active function.
/// @return Success or a structured syntax/resource diagnostic.
[[nodiscard]] il::support::Expected<void> parseFunctionHeader(const std::string &header,
                                                              ParserState &st);

/// @brief Parse a basic block label and its optional parameter list.
/// @param header Block header text ending before the colon.
/// @param st Mutable state with an active function.
/// @return Success or a structured label/parameter diagnostic.
[[nodiscard]] il::support::Expected<void> parseBlockHeader(const std::string &header,
                                                           ParserState &st);

/// @brief Parse an entire function body following its header line.
/// @param is Input stream positioned after @p header.
/// @param header Mutable header-line buffer used to begin parsing.
/// @param st Parser state and destination module.
/// @return Success after the closing brace, or a diagnostic with state rolled back.
[[nodiscard]] il::support::Expected<void> parseFunction(std::istream &is,
                                                        std::string &header,
                                                        ParserState &st);

} // namespace il::io::detail
