//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/internal/io/ModuleParser.hpp
// Purpose: Declares helpers for parsing module-level IL directives.
// Key invariants: Operates with ParserState positioned at module scope.
// Ownership/Lifetime: Updates module metadata and dispatches to function parsing.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares dispatch for module-scope IL directives.
 *
 * @details The module parser recognizes declarations and metadata at top level,
 *          updates the destination module through `ParserState`, and delegates
 *          multi-line function definitions to the function parser while
 *          preserving the shared input stream and diagnostic channel.
 */

#pragma once

#include "il/internal/io/ParserState.hpp"

#include <istream>
#include <ostream>
#include <string>

namespace il::io::detail {

/// @brief Parse a single top-level directive such as extern, global, or func.
/// @param is Module input stream, used when a function directive owns following lines.
/// @param line Mutable current top-level line.
/// @param st Parser state and destination module.
/// @param err Stream receiving legacy textual diagnostics.
/// @return True when the directive was accepted.
bool parseModuleHeader(std::istream &is, std::string &line, ParserState &st, std::ostream &err);

} // namespace il::io::detail
