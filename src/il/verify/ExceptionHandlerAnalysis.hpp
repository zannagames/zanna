//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/verify/ExceptionHandlerAnalysis.hpp
// Purpose: Analysis utilities for identifying and validating exception handler
//          basic blocks. Handler blocks must declare exactly two parameters
//          (Error + ResumeTok) with specific naming (%err, %tok). Returns a
//          three-way result: not-a-handler, valid handler, or malformed handler.
// Key invariants:
//   - Handler blocks have exactly two parameters: Error and ResumeTok.
//   - std::nullopt means "not a handler"; error means "malformed handler".
// Ownership/Lifetime: Stateless free function operating on caller-owned IL
//          structures.
// Links: support/diag_expected.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares classification and signature validation for EH entry blocks.
/// @details The analysis distinguishes ordinary blocks from well-formed
///          handlers and malformed handler attempts. It returns parameter IDs
///          only after validating the canonical `eh.entry` placement, arity,
///          types, and names.

#pragma once

#include "support/diag_expected.hpp"

#include <optional>

namespace il::core {
struct BasicBlock;
struct Function;
} // namespace il::core

namespace il::verify {

/// @brief Captures the parameter IDs associated with a handler's %err and %tok values.
struct HandlerSignature {
    /// @brief Temporary identifier of the `%err:Error` block parameter.
    unsigned errorParam = 0;

    /// @brief Temporary identifier of the `%tok:ResumeTok` block parameter.
    unsigned resumeTokenParam = 0;
};

/// @brief Inspect @p bb and determine whether it is a handler block with a valid signature.
/// @details A block beginning with `eh.entry` must declare exactly
///          `(%err:Error, %tok:ResumeTok)`. An `eh.entry` found later in an
///          otherwise ordinary block is also diagnosed. Blocks containing no
///          `eh.entry` are classified as non-handlers.
/// @param fn Function providing diagnostic context.
/// @param bb Basic block to analyse.
/// @return Empty optional when @p bb is not a handler, a signature when valid,
///         or a diagnostic for malformed placement or parameters.
[[nodiscard]] il::support::Expected<std::optional<HandlerSignature>> analyzeHandlerBlock(
    const il::core::Function &fn, const il::core::BasicBlock &bb);

} // namespace il::verify
