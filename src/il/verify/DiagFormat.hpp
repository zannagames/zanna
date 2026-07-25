//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/verify/DiagFormat.hpp
// Purpose: Diagnostic formatting utilities for the IL verifier -- generates
//          human-readable error messages with hierarchical context (function,
//          block label, instruction snippet). Consistent format:
//          "function:block: instruction-snippet: message".
// Key invariants:
//   - All formatters are stateless pure functions; never modify IL or take
//     ownership.
// Ownership/Lifetime: Stateless free functions returning std::string values.
// Links: il/core/fwd.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares deterministic textual context formatting for IL diagnostics.
/// @details The formatters return single-line strings and append optional
///          caller text only when it is nonempty. They neither emit diagnostics
///          nor retain references to the supplied IL objects.

#pragma once

#include "il/core/fwd.hpp"

#include <string>
#include <string_view>

namespace il::verify {

/// @brief Format a diagnostic string describing a basic block.
/// @param fn Function owning the block used to provide context.
/// @param bb Basic block referenced by the diagnostic.
/// @param message Optional trailing text appended to the diagnostic.
/// @return Single-line diagnostic including the function and block label.
std::string formatBlockDiag(const il::core::Function &fn,
                            const il::core::BasicBlock &bb,
                            std::string_view message = {});

/// @brief Format a diagnostic string describing an instruction.
/// @param fn Function owning the instruction.
/// @param bb Basic block containing the instruction.
/// @param instr Instruction rendered via makeSnippet for context.
/// @param message Optional trailing text appended to the diagnostic.
/// @return Single-line diagnostic with function, block, instruction snippet and message.
std::string formatInstrDiag(const il::core::Function &fn,
                            const il::core::BasicBlock &bb,
                            const il::core::Instr &instr,
                            std::string_view message = {});

} // namespace il::verify
