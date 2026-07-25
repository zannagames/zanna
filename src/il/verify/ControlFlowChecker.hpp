//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/verify/ControlFlowChecker.hpp
// Purpose: Core control-flow verification infrastructure -- validates basic
//          block structure, terminator placement, and CFG integrity. Provides
//          the iteration and dispatch framework (VerifyInstrFn callback) that
//          orchestrates per-instruction verification within a function.
// Key invariants:
//   - Every basic block must end with exactly one terminator.
//   - Control-flow transfers must respect block parameter signatures.
//   - Structural checks are separate from opcode-specific semantic validation.
// Ownership/Lifetime: Stateless free functions operating on caller-owned IL
//          structures and a caller-provided ostream for diagnostics.
// Links: il/verify/BlockMap.hpp, il/verify/TypeInference.hpp,
//        il/core/Opcode.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares reusable control-flow checks and streaming compatibility APIs.
/// @details The checker validates block parameters, instruction traversal, and
///          terminator placement. Boolean entry points adapt structured
///          verifier failures to a caller-provided diagnostic stream.

#pragma once

#include "il/core/Opcode.hpp"
#include "il/core/fwd.hpp"
#include "il/verify/BlockMap.hpp"
#include "il/verify/TypeInference.hpp"

#include <ostream>
#include <unordered_map>
#include <vector>

namespace il::core {
struct Extern;
}

namespace il::verify {

/// @brief Boolean instruction-verifier callback used by the streaming adapter.
/// @details The callback receives the enclosing function and block, symbol
///          lookup tables, mutable type inference state, and a stream for
///          diagnostics. It returns `false` after writing a failure diagnostic.
using VerifyInstrFn =
    bool (*)(const il::core::Function &fn,
             const il::core::BasicBlock &bb,
             const il::core::Instr &instr,
             const BlockMap &blockMap,
             const std::unordered_map<std::string, const il::core::Extern *> &externs,
             const std::unordered_map<std::string, const il::core::Function *> &funcs,
             TypeInference &types,
             std::ostream &err);

/// @brief Determine whether an opcode terminates a basic block.
/// @param op Opcode to classify through the central opcode registry.
/// @return `true` when the opcode metadata marks @p op as a terminator.
bool isTerminator(il::core::Opcode op);

/// @brief Validate incoming block parameters and populate the type environment.
/// @details Rejects malformed or duplicate parameter names, duplicate temporary
///          identifiers, and void parameter types. Successfully visited
///          parameters are registered in @p types and appended to @p paramIds.
/// @param fn Function owning @p bb and providing diagnostic context.
/// @param bb Block whose parameter declarations are checked.
/// @param types Type environment updated with valid parameter definitions.
/// @param paramIds Output sequence receiving valid parameter identifiers.
/// @param err Stream receiving the first formatted failure diagnostic.
/// @return `true` when every parameter is valid; otherwise `false`.
bool validateBlockParams(const il::core::Function &fn,
                         const il::core::BasicBlock &bb,
                         TypeInference &types,
                         std::vector<unsigned> &paramIds,
                         std::ostream &err);

/// @brief Iterate instructions in a block and dispatch verification callbacks.
/// @details Ensures each instruction's operands are defined before invoking
///          @p verifyInstrFn and stops after the first terminator. Callback text
///          is normalized into structured warnings and errors before being
///          replayed to @p err.
/// @param verifyInstrFn Instruction-specific boolean verifier to invoke.
/// @param fn Function owning @p bb.
/// @param bb Block traversed in instruction order.
/// @param blockMap Borrowed map used to resolve branch targets.
/// @param externs External declarations visible to instruction checks.
/// @param funcs Function declarations visible to instruction checks.
/// @param types Mutable type-inference state for operand validation.
/// @param err Stream receiving warnings and the first failure diagnostic.
/// @return `true` when operand and callback checks all succeed; otherwise
///         `false`.
bool iterateBlockInstructions(
    VerifyInstrFn verifyInstrFn,
    const il::core::Function &fn,
    const il::core::BasicBlock &bb,
    const BlockMap &blockMap,
    const std::unordered_map<std::string, const il::core::Extern *> &externs,
    const std::unordered_map<std::string, const il::core::Function *> &funcs,
    TypeInference &types,
    std::ostream &err);

/// @brief Validate that terminator placement within the block is well-formed.
/// @details Requires a nonempty block with exactly one terminator as its final
///          instruction; rejects duplicate terminators and instructions after
///          the first terminator.
/// @param fn Function owning @p bb and providing diagnostic context.
/// @param bb Block whose instruction sequence is checked.
/// @param err Stream receiving the first formatted failure diagnostic.
/// @return `true` when @p bb has valid terminator placement; otherwise `false`.
bool checkBlockTerminators(const il::core::Function &fn,
                           const il::core::BasicBlock &bb,
                           std::ostream &err);

/// @brief Verify branch instruction argument structure.
/// @param fn Function containing @p instr.
/// @param bb Block containing @p instr.
/// @param instr Unconditional branch to validate.
/// @param blockMap Borrowed map used to resolve the branch target.
/// @param types Type inference state used for argument compatibility.
/// @param err Stream receiving a formatted diagnostic on failure.
/// @return `true` when the branch is locally valid; otherwise `false`.
bool verifyBr(const il::core::Function &fn,
              const il::core::BasicBlock &bb,
              const il::core::Instr &instr,
              const BlockMap &blockMap,
              TypeInference &types,
              std::ostream &err);

/// @brief Verify conditional branch argument structure.
/// @param fn Function containing @p instr.
/// @param bb Block containing @p instr.
/// @param instr Conditional branch to validate.
/// @param blockMap Borrowed map used to resolve successor targets.
/// @param types Type inference state used for condition and argument types.
/// @param err Stream receiving a formatted diagnostic on failure.
/// @return `true` when the conditional branch is locally valid; otherwise
///         `false`.
bool verifyCBr(const il::core::Function &fn,
               const il::core::BasicBlock &bb,
               const il::core::Instr &instr,
               const BlockMap &blockMap,
               TypeInference &types,
               std::ostream &err);

/// @brief Verify return instruction matches function signature.
/// @param fn Function whose return type constrains @p instr.
/// @param bb Block containing @p instr.
/// @param instr Return instruction to validate.
/// @param types Type inference state used to resolve the returned value.
/// @param err Stream receiving a formatted diagnostic on failure.
/// @return `true` when the return matches @p fn; otherwise `false`.
bool verifyRet(const il::core::Function &fn,
               const il::core::BasicBlock &bb,
               const il::core::Instr &instr,
               TypeInference &types,
               std::ostream &err);

} // namespace il::verify
