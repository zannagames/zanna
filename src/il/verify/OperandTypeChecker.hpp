//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the OperandTypeChecker helper class, which validates that
// instruction operands have types compatible with the requirements specified by
// opcode metadata. This is the type-checking layer of structural verification.
//
// IL opcodes declare type category constraints for their operands: some require
// specific concrete types (i32, ptr, etc.), others require categories that match
// the instruction's declared type, and some accept any type. The OperandTypeChecker
// enforces these constraints using the InstructionSpec metadata and the type
// environment maintained during verification.
//
// Key Responsibilities:
// - Resolve actual operand types from literals, temporaries, and block parameters
// - Match operand types against metadata type category requirements
// - Handle InstrType category (operand type must match instruction's result type)
// - Validate literal values fit within their declared type constraints
// - Generate precise type mismatch diagnostics
//
// Design Notes:
// OperandTypeChecker is the most complex of the structural verification helpers
// because it must coordinate between the type environment (TypeInference), opcode
// metadata (InstructionSpec), and actual operand values. It follows the same
// construct-and-execute pattern as other detail checkers, with the run() method
// orchestrating per-operand type validation. The checker is internal to the
// table-driven verification system.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares table-driven operand type and literal-range validation.
/// @details The checker combines generated operand categories with inferred
///          value types and instruction annotations, returning a contextual
///          diagnostic for the first mismatch.

#pragma once

#include "il/verify/SpecTables.hpp"
#include "il/verify/VerifyCtx.hpp"

#include "support/diag_expected.hpp"

#include <string_view>

namespace il::verify::detail {

/// @brief Ensures an instruction's operands satisfy the metadata type requirements.
class OperandTypeChecker {
  public:
    /// @brief Bind a checker to one instruction and its generated specification.
    /// @param ctx Verification context that must outlive the checker.
    /// @param spec Opcode specification that must outlive the checker.
    OperandTypeChecker(const VerifyCtx &ctx, const InstructionSpec &spec);

    /// @brief Validates operand types described by opcode metadata.
    /// @return Empty on success or a populated diagnostic on failure.
    [[nodiscard]] il::support::Expected<void> run() const;

  private:
    /// @brief Format a type failure in the current instruction context.
    /// @param message Specific mismatch text.
    /// @return Structured failure anchored to the instruction.
    il::support::Expected<void> report(std::string_view message) const;

    /// @brief Borrowed verification and type-inference context.
    const VerifyCtx &ctx_;

    /// @brief Borrowed generated operand type specification.
    const InstructionSpec &spec_;
};

} // namespace il::verify::detail
