//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/DiagFormat.hpp
// Purpose: Centralized diagnostic string builders for VM error paths.
// Key invariants: All helpers are cold-path only; never call in hot loops.
// Ownership/Lifetime: Returns std::string by value; no ownership transfer.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares cold-path diagnostic message builders used by the VM.
/// @details These helpers centralize wording for runtime-kind, helper lookup,
///          call-arity, and branch-arity failures.  Each function returns an
///          owning string and performs formatting only when an error is reported.

#pragma once

#include "il/core/Type.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace il::vm::diag {

/// @brief Format an error for unsupported type kind in marshalling.
/// @param operation Description of the operation (e.g., "argument", "return", "assign").
/// @param kind The unsupported type kind.
/// @return Formatted error message.
/// @note Cold path only - constructs string on each call.
[[nodiscard]] std::string formatUnsupportedKind(std::string_view operation,
                                                il::core::Type::Kind kind);

/// @brief Format an error for unknown runtime helper.
/// @param name The unrecognized helper name.
/// @return Formatted error message.
/// @note Cold path only - constructs string on each call.
[[nodiscard]] std::string formatUnknownRuntimeHelper(std::string_view name);

/// @brief Format an argument count mismatch error for function calls.
/// @param functionName Name of the function being called.
/// @param expected Expected argument count.
/// @param received Actual argument count provided.
/// @return Formatted error message.
/// @note Cold path only - constructs string on each call.
[[nodiscard]] std::string formatArgumentCountMismatch(std::string_view functionName,
                                                      std::size_t expected,
                                                      std::size_t received);

/// @brief Format a branch argument count mismatch error.
/// @param targetLabel Label of the target block.
/// @param sourceLabel Label of the source block (may be empty).
/// @param expected Expected argument count.
/// @param provided Actual argument count provided.
/// @return Formatted error message.
/// @note Cold path only - constructs string on each call.
[[nodiscard]] std::string formatBranchArgMismatch(std::string_view targetLabel,
                                                  std::string_view sourceLabel,
                                                  std::size_t expected,
                                                  std::size_t provided);

} // namespace il::vm::diag
