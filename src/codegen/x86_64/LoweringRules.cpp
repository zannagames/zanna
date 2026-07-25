//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/LoweringRules.cpp
// Purpose: Assemble the lowering rule registry by bridging declarative table
//          entries to match and emit thunks.
// Key invariants:
//   - Rules are initialised lazily and remain immutable after the first access.
//   - The generated registry mirrors the layout of the declarative table.
// Ownership/Lifetime:
//   - Returned registry is stored as a static vector whose lifetime spans the
//     process.
// Links: src/codegen/x86_64/LoweringRules.hpp,
//        src/codegen/x86_64/LoweringRuleTable.hpp
//
//===----------------------------------------------------------------------===//

#include "LoweringRules.hpp"

#include "LoweringRuleTable.hpp"

#include <cstddef>
#include <utility>
#include <vector>

/**
 * @file
 * @brief Implements thunk generation for the legacy lowering-rule facade.
 *
 * Template-generated match and emit functions retain compile-time RuleSpec
 * indices. A lazily initialized vector packages those thunks with diagnostic
 * names, while rule selection continues to use the declarative table's indexed
 * lookup and maps the result back to the parallel vector.
 */

namespace zanna::codegen::x64 {

namespace {

/// @brief Match thunk that forwards to a declarative lowering rule.
/// @details Wraps the @ref zanna::codegen::x64::lowering::kLoweringRuleTable
///          entry at @p Index in a function pointer compatible signature for the
///          runtime registry.  This enables the declarative data to satisfy the
///          imperative interface expected by the pass manager.
/// @tparam Index Entry within the lowering table to query.
/// @param instr IL instruction presented to the rule.
/// @return True when the instruction satisfies the rule's predicate.
template <std::size_t Index> bool matchRuleThunk(const IL::Instr &instr) {
    return matchesRuleSpec(lowering::kLoweringRuleTable[Index], instr);
}

/// @brief Emit thunk that invokes the declarative lowering rule.
/// @details Calls the `emit` function stored in the declarative table entry at
///          @p Index, adapting it to the runtime function pointer signature used
///          by the registry.
/// @tparam Index Entry within the lowering table to invoke.
/// @param instr IL instruction being lowered.
/// @param builder Machine IR builder receiving the emitted instructions.
template <std::size_t Index> void emitRuleThunk(const IL::Instr &instr, MIRBuilder &builder) {
    lowering::kLoweringRuleTable[Index].emit(instr, builder);
}

/// @brief Build the immutable lowering rule registry from a compile-time index sequence.
/// @details Uses template parameter packs to transform every entry in the
///          declarative rule table into a @ref LoweringRule record containing the
///          match and emit thunks.  The resulting vector is stored in static
///          storage so subsequent calls reuse the cached registry.
/// @tparam Indices Compile-time indices spanning the declarative rule table.
/// @return Reference to the lazily initialised registry.
template <std::size_t... Indices>
const std::vector<LoweringRule> &makeRules(std::index_sequence<Indices...>) {
    static const std::vector<LoweringRule> rules{
        LoweringRule{&matchRuleThunk<Indices>,
                     &emitRuleThunk<Indices>,
                     lowering::kLoweringRuleTable[Indices].name}...};
    return rules;
}

/// @brief Lazily construct or fetch the lowering rule registry.
/// @details Instantiates the registry on first use by calling @ref makeRules
///          with an index sequence spanning the declarative rule table.  Later
///          invocations reuse the cached vector.
/// @return Reference to the static lowering rule registry.
const std::vector<LoweringRule> &buildRules() {
    static const auto &rules =
        makeRules(std::make_index_sequence<lowering::kLoweringRuleTable.size()>{});
    return rules;
}

} // namespace

/// @brief Retrieve the full set of lowering rules available to the backend.
/// @details Forwards to @ref buildRules so callers always receive the cached
///          registry constructed from the declarative rule table.
/// @return Reference to the lowering rule registry.
const std::vector<LoweringRule> &zanna_get_lowering_rules() {
    return buildRules();
}

/// @brief Locate the lowering rule associated with an IL instruction.
/// @details Performs a lookup in the declarative rule table, converts the
///          resulting pointer into a registry index, and returns the
///          corresponding @ref LoweringRule entry when found.  The helper keeps
///          the runtime-facing API independent from the declarative data
///          structures.
/// @param instr IL instruction that needs a lowering rule.
/// @return Pointer to the matching rule or nullptr when no rule applies.
const LoweringRule *zanna_select_rule(const IL::Instr &instr) {
    const auto *spec = lookupRuleSpec(instr);
    if (!spec) {
        return nullptr;
    }

    const auto index = static_cast<std::size_t>(spec - lowering::kLoweringRuleTable.data());
    const auto &rules = buildRules();
    return index < rules.size() ? &rules[index] : nullptr;
}

} // namespace zanna::codegen::x64
