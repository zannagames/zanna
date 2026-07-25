//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/internal/io/OperandParser.hpp
// Purpose: Declares a helper for parsing textual IL operands.
// Key invariants: Requires ParserState to supply SSA mappings and diagnostics.
// Ownership/Lifetime: Operates on instructions owned by the parser caller.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

#pragma once

#include "il/core/fwd.hpp"
#include "il/internal/io/ParserState.hpp"
#include "support/diag_expected.hpp"

#include <string>
#include <vector>

namespace il::io::detail {

/// @brief Parses operand tokens for an instruction currently under construction.
class OperandParser {
  public:
    /// @brief Construct an operand parser bound to the current parser state and instruction.
    /// @param state Parser state supplying temporary mappings and diagnostics.
    /// @param instr Instruction receiving parsed operands.
    OperandParser(ParserState &state, il::core::Instr &instr);

    /// @brief Parse a single value token (temporary, global, literal, etc.).
    /// @param token Token extracted from the instruction text.
    /// @return Parsed IL value or a diagnostic on failure.
    [[nodiscard]] il::support::Expected<il::core::Value> parseValueToken(
        const std::string &token) const;

    /// @brief Parse the call operand syntax and append results to the instruction.
    /// @param text Remainder of the instruction line starting at the callee token.
    /// @return Empty on success; otherwise, a diagnostic describing the malformed call.
    [[nodiscard]] il::support::Expected<void> parseCallOperands(const std::string &text);

    /// @brief Parse call.indirect operands: %fnPtr(%arg1, %arg2, ...).
    /// @param text Operand text beginning with the function-pointer value.
    /// @return Success after populating operands, or a structured diagnostic.
    [[nodiscard]] il::support::Expected<void> parseCallIndirectOperands(const std::string &text);

    /// @brief Parse branch target lists of the form `label(args), label(args)`.
    /// @param text Textual segment containing the branch targets.
    /// @param expectedTargets Number of targets dictated by the opcode metadata.
    /// @return Empty on success; otherwise, a diagnostic describing the malformed targets.
    [[nodiscard]] il::support::Expected<void> parseBranchTargets(const std::string &text,
                                                                 size_t expectedTargets);

    /// @brief Parse switch operand payloads consisting of a default target followed by cases.
    /// @param text Textual segment beginning with the default target.
    /// @return Empty on success; otherwise, a diagnostic describing the malformed switch payload.
    [[nodiscard]] il::support::Expected<void> parseSwitchTargets(const std::string &text);

  private:
    /// @brief Split top-level comma-separated text while respecting nested syntax.
    /// @param text Input segment.
    /// @param context Human-readable construct name for diagnostics.
    /// @return Trimmed segments or a delimiter/syntax diagnostic.
    il::support::Expected<std::vector<std::string>> splitCommaSeparated(const std::string &text,
                                                                        const char *context) const;

    /// @brief Parse one branch label with its optional argument bundle.
    /// @param segment Single target segment.
    /// @param label Receives the target label without its sigil.
    /// @param args Receives parsed block arguments.
    /// @return Success or a structured target diagnostic.
    il::support::Expected<void> parseBranchTarget(const std::string &segment,
                                                  std::string &label,
                                                  std::vector<il::core::Value> &args) const;

    /// @brief Validate a branch argument count against known block metadata.
    /// @param label Target block label.
    /// @param argCount Number of parsed arguments.
    /// @return Success, or an arity diagnostic when the label is known and mismatched.
    il::support::Expected<void> checkBranchArgCount(const std::string &label,
                                                    size_t argCount) const;

    /// @brief Parse and append the switch default target.
    /// @param segment Default-label segment.
    /// @return Success or a structured branch-target diagnostic.
    il::support::Expected<void> parseDefaultTarget(const std::string &segment);

    /// @brief Parse one switch case value and branch target.
    /// @param segment Complete case segment.
    /// @param mnemonic Opcode spelling used in diagnostics.
    /// @return Success or a structured case diagnostic.
    il::support::Expected<void> parseCaseSegment(const std::string &segment, const char *mnemonic);

    /// @brief Validate and append a parsed switch case target.
    /// @param label Target label.
    /// @param args Parsed branch arguments.
    /// @return Success or a block-parameter arity diagnostic.
    il::support::Expected<void> validateCaseArity(std::string label,
                                                  std::vector<il::core::Value> args);

    ParserState &state_;
    il::core::Instr &instr_;
};

} // namespace il::io::detail
