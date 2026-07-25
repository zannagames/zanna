//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the core instruction verification function used by the IL
// verifier to validate non-control-flow instructions. It provides the main entry
// point for opcode-specific semantic validation after structural checks pass.
//
// The IL verifier performs verification in stages: first structural validation
// (operand counts, result presence, successor structure), then semantic validation
// (type compatibility, reference validity, side effect constraints). This file
// provides the semantic validation layer that dispatches to specialized checkers
// based on opcode category (arithmetic, memory, calls, etc.).
//
// Key Responsibilities:
// - Validate instruction operands are well-typed for the opcode
// - Verify call.extern and call.func reference valid targets
// - Check memory operations (alloca, load, store, gep) for type safety
// - Ensure runtime helpers (trap, cast, bounds checks) have correct structure
// - Record result types in the type environment for downstream uses
//
// Design Notes:
// The verifyInstruction function coordinates opcode metadata lookup with specialized
// checking logic. It first validates the instruction against its opcode signature
// using verifyOpcodeSignature, then dispatches to category-specific validators.
// The boolean return convention (true = success, false = failure with diagnostics
// already emitted) enables caller error handling without exception overhead.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares stream-oriented instruction and opcode-signature verification.
/// @details These compatibility entry points adapt structured instruction
///          checks to boolean results and formatted diagnostic streams while
///          updating caller-owned type inference state.

#pragma once

#include "il/core/fwd.hpp"
#include "il/verify/TypeInference.hpp"
#include <ostream>
#include <unordered_map>

namespace il::core {
struct Extern;
}

namespace il::verify {

/// @brief Validate a single IL instruction that is not control-flow specific.
/// @param fn Enclosing function for diagnostics.
/// @param bb Basic block containing the instruction.
/// @param instr Instruction to validate.
/// @param externs Map of available extern declarations keyed by name.
/// @param funcs Map of known functions keyed by name.
/// @param types Type inference helper for operand queries and result recording.
/// @param err Stream receiving diagnostics on failure.
/// @return True when the instruction satisfies operand and type rules.
bool verifyInstruction(const il::core::Function &fn,
                       const il::core::BasicBlock &bb,
                       const il::core::Instr &instr,
                       const std::unordered_map<std::string, const il::core::Extern *> &externs,
                       const std::unordered_map<std::string, const il::core::Function *> &funcs,
                       TypeInference &types,
                       std::ostream &err);

/// @brief Validate opcode-independent structural properties of @p instr.
/// @param fn Function used for diagnostic context.
/// @param bb Basic block containing the instruction.
/// @param instr Instruction to verify.
/// @param err Stream receiving diagnostics on failure.
/// @return True if the instruction matches the metadata signature.
bool verifyOpcodeSignature(const il::core::Function &fn,
                           const il::core::BasicBlock &bb,
                           const il::core::Instr &instr,
                           std::ostream &err);

} // namespace il::verify
