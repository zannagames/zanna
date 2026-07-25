//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/LoweringRules.hpp
// Purpose: Declare the rule registry that drives IL to MIR lowering.
// Key invariants:
//   - Rules are initialised lazily on first access and remain immutable thereafter.
//   - Selection mirrors the indexed declarative rule-table lookup.
// Ownership/Lifetime:
//   - Registry stored as a process-wide static vector, returned by const reference.
// Links: src/codegen/x86_64/LoweringRules.cpp,
//        src/codegen/x86_64/LowerILToMIR.hpp,
//        src/codegen/x86_64/LoweringRuleTable.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <vector>

/**
 * @file
 * @brief Declares the legacy-compatible facade over declarative lowering rules.
 *
 * Each LoweringRule holds plain match and emit function pointers. The
 * implementation synthesizes one thunk pair per RuleSpec so existing consumers
 * can use the legacy interface while opcode indexing and operand matching remain
 * centralized in LoweringRuleTable.
 */

namespace zanna::codegen::x64 {
/// Backend-facing IL instruction adapted by the registry.
struct ILInstr;
} // namespace zanna::codegen::x64

namespace IL {
/// Compatibility alias used by the historical lowering-rule interface.
using Instr = zanna::codegen::x64::ILInstr;
} // namespace IL

namespace zanna::codegen::x64 {

class MIRBuilder;

/// @brief Legacy callable view of one declarative IL lowering rule.
struct LoweringRule {
    bool (*match)(const IL::Instr &);              ///< Match predicate invoked before emit.
    void (*emit)(const IL::Instr &, MIRBuilder &); ///< Emit routine for matched opcode.
    const char *name; ///< Debug name describing the handled opcode family.
};

/// @brief Retrieve the global registry of lowering rules.
///
/// @details The registry is lazily initialised on the first call and remains
///          immutable for the lifetime of the process. Its indices correspond
///          exactly to entries in @c lowering::kLoweringRuleTable.
///
/// @return Const reference to the process-wide vector of lowering rules.
[[nodiscard]] const std::vector<LoweringRule> &zanna_get_lowering_rules();

/// @brief Select the indexed declarative rule that accepts an instruction.
///
/// @details Uses the declarative exact/prefix lookup, converts the matched
///          RuleSpec pointer to its parallel legacy-registry index, and returns
///          null when no rule matches.
///
/// @param instr The IL instruction to match against registered rules.
/// @return Pointer to the first matching LoweringRule, or nullptr if none match.
[[nodiscard]] const LoweringRule *zanna_select_rule(const IL::Instr &instr);

} // namespace zanna::codegen::x64
