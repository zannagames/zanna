//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/internal/io/InstrParser.hpp
// Purpose: Declares helpers for parsing IL instruction statements.
// Key invariants: Operates within a valid ParserState containing an active block.
// Ownership/Lifetime: Writes instructions directly into the ParserState's current block.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the internal parser for one textual IL instruction.
 *
 * @details The parser resolves SSA names and opcode-specific operands through
 *          `ParserState`, appends a successfully formed instruction to the
 *          active block, and reports legacy textual diagnostics to the supplied
 *          stream when parsing fails.
 */

#pragma once

#include "il/internal/io/ParserState.hpp"

#include <ostream>
#include <string>

namespace il::io::detail {

/// @brief Parse a single textual IL instruction and append it to the active block.
/// @param line Instruction text potentially including a result assignment.
/// @param st Parser state supplying context such as current block and SSA map.
/// @param err Stream receiving diagnostic output when parsing fails.
/// @return True when the instruction is parsed successfully.
bool parseInstruction(const std::string &line, ParserState &st, std::ostream &err);

} // namespace il::io::detail
