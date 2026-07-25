//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/verify/BranchVerifier.hpp
// Purpose: Verification helpers for branch and return terminators (br, cbr,
//          switch.i32, ret). Enforces that branch labels resolve to valid
//          blocks, argument counts/types match target signatures, and return
//          types match the enclosing function's declared return type.
// Key invariants:
//   - Helpers are stateless; Expected-based diagnostics only.
//   - Missing labels are silently skipped (reported by other components).
// Ownership/Lifetime: Stateless free functions operating on caller-owned IL
//          structures. No internal caches.
// Links: il/verify/BlockMap.hpp, il/verify/TypeInference.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares structural and type checks for IL branch-family terminators.
/// @details Each verifier returns the first local diagnostic through
///          `Expected<void>`. Target-resolution failures are intentionally left
///          to the verifier component that owns block-label existence checks.

#pragma once

#include "il/verify/BlockMap.hpp"
#include "support/diag_expected.hpp"

#include <vector>

namespace il::core {
struct BasicBlock;
struct Function;
struct Instr;
} // namespace il::core

namespace il::verify {
class TypeInference;

/// @brief Verify an unconditional branch terminator against the target signature.
/// @details Confirms the `br` instruction has no operands and exactly one label.
///          When the label resolves in @p blockMap, validates that the branch
///          argument list matches the target block's parameter count and types
///          using @p types. Missing labels are ignored here so other verifier
///          components can report unresolved-target diagnostics.
/// @param fn Function currently being verified.
/// @param bb Block containing the branch terminator.
/// @param instr Branch instruction to validate.
/// @param blockMap Mapping from labels to target blocks.
/// @param types Type inference context used to compare operand types.
/// @return Expected success or a diagnostic describing the mismatch.
[[nodiscard]] il::support::Expected<void> verifyBr_E(const il::core::Function &fn,
                                                     const il::core::BasicBlock &bb,
                                                     const il::core::Instr &instr,
                                                     const BlockMap &blockMap,
                                                     TypeInference &types);

/// @brief Verify a conditional branch terminator and its edge payloads.
/// @details Requires exactly one i1 condition operand and two successor labels.
///          For each label that resolves in @p blockMap, checks that the branch
///          argument list aligns with the target block's parameter count and
///          types. Any structural mismatch (operand/label count or condition
///          type) yields a diagnostic tied to @p instr.
/// @param fn Function currently being verified.
/// @param bb Block containing the conditional branch.
/// @param instr Conditional branch instruction under validation.
/// @param blockMap Mapping from labels to target blocks.
/// @param types Type inference context used to compare operand types.
/// @return Expected success or a diagnostic describing the mismatch.
[[nodiscard]] il::support::Expected<void> verifyCBr_E(const il::core::Function &fn,
                                                      const il::core::BasicBlock &bb,
                                                      const il::core::Instr &instr,
                                                      const BlockMap &blockMap,
                                                      TypeInference &types);

/// @brief Verify a `switch.i32` terminator and all of its case/default edges.
/// @details Ensures a scrutinee operand exists and is i32, a default label is
///          present, branch argument bundles match the number of labels, and
///          the operand list aligns with the number of cases. Each case value
///          must be a unique 32-bit integer constant. For every resolved target
///          label, the branch arguments are checked against the block's
///          parameter signature using @p types.
/// @param fn Function currently being verified.
/// @param bb Block containing the switch terminator.
/// @param instr switch.i32 instruction whose structure is examined.
/// @param blockMap Mapping from labels to target blocks.
/// @param types Type inference context used to compare operand types.
/// @return Expected success or a diagnostic describing the mismatch.
[[nodiscard]] il::support::Expected<void> verifySwitchI32_E(const il::core::Function &fn,
                                                            const il::core::BasicBlock &bb,
                                                            const il::core::Instr &instr,
                                                            const BlockMap &blockMap,
                                                            TypeInference &types);

/// @brief Verify a `ret` terminator against the function signature.
/// @details For void functions, ensures the return instruction carries no
///          operands. For non-void functions, requires exactly one operand and
///          checks that its inferred type matches the function's declared
///          return type. Any mismatch returns a diagnostic tied to @p instr.
/// @param fn Function currently being verified.
/// @param bb Block containing the return instruction.
/// @param instr Return instruction to validate.
/// @param types Type inference context used to resolve operand types.
/// @return Expected success or a diagnostic describing the mismatch.
[[nodiscard]] il::support::Expected<void> verifyRet_E(const il::core::Function &fn,
                                                      const il::core::BasicBlock &bb,
                                                      const il::core::Instr &instr,
                                                      TypeInference &types);

} // namespace il::verify
