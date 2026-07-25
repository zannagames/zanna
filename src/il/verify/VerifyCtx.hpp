//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the VerifyCtx structure, which bundles all contextual state
// needed during instruction verification into a single parameter. This reduces
// parameter passing overhead and provides a consistent interface for verification
// helpers.
//
// Instruction verification requires access to multiple pieces of context: the
// enclosing function and basic block (for diagnostic formatting), the instruction
// being verified, type environment (for operand type resolution), extern/function
// symbol tables (for call validation), and diagnostic sink (for error reporting).
// Rather than passing these as individual parameters to every verification function,
// VerifyCtx aggregates them into a single context object.
//
// Key Responsibilities:
// - Bundle verification state into a single copyable structure
// - Provide const access to function, block, and instruction being verified
// - Reference type environment for operand type queries
// - Reference symbol tables for extern and function lookup during call validation
// - Reference diagnostic sink for error and warning reporting
//
// Design Notes:
// VerifyCtx is a simple aggregate struct holding only references - it owns no data.
// This makes it trivial to construct and copy, enabling easy context passing through
// the verification call stack. The struct is designed to be constructed once per
// instruction verification and passed by const reference to all helpers. The
// references remain valid only for the duration of instruction verification.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the borrowed context passed to instruction checkers.
/// @details The aggregate groups diagnostics, type state, module symbol maps,
///          and the exact function/block/instruction location. It owns no data
///          and is valid only while every referenced object remains alive.

#pragma once

#include "il/core/fwd.hpp"

#include <string>
#include <unordered_map>

namespace il::core {
struct Extern;
struct Global;
} // namespace il::core

namespace il::verify {

class DiagSink;
class TypeInference;

/// @brief Bundles shared verifier state when validating a single instruction.
struct VerifyCtx {
    /// @brief Construct a verifier context for one instruction.
    /// @details The context borrows all maps and IR nodes. Callers must ensure
    ///          the referenced verifier state and IR objects outlive the
    ///          instruction verification call.
    /// @param diagSink Diagnostic sink for warnings and errors.
    /// @param typeState Type inference table for the current function.
    /// @param externMap Known extern declarations by name.
    /// @param functionMap Known function declarations by name.
    /// @param globalMap Known globals by name.
    /// @param function Function that owns @p instruction.
    /// @param basicBlock Block that contains @p instruction.
    /// @param instruction Instruction being verified.
    VerifyCtx(DiagSink &diagSink,
              TypeInference &typeState,
              const std::unordered_map<std::string, const il::core::Extern *> &externMap,
              const std::unordered_map<std::string, const il::core::Function *> &functionMap,
              const std::unordered_map<std::string, const il::core::Global *> &globalMap,
              const il::core::Function &function,
              const il::core::BasicBlock &basicBlock,
              const il::core::Instr &instruction)
        : diags(diagSink), types(typeState), externs(externMap), functions(functionMap),
          globals(globalMap), fn(function), block(basicBlock), instr(instruction) {}

    DiagSink &diags;      ///< Diagnostic sink used for warnings and errors.
    TypeInference &types; ///< Type inference table tracking temporaries.
    const std::unordered_map<std::string, const il::core::Extern *>
        &externs; ///< Known extern signatures.
    const std::unordered_map<std::string, const il::core::Function *>
        &functions; ///< Known function definitions.
    const std::unordered_map<std::string, const il::core::Global *>
        &globals;                      ///< Known global definitions.
    const il::core::Function &fn;      ///< Function that owns the instruction.
    const il::core::BasicBlock &block; ///< Basic block containing the instruction.
    const il::core::Instr &instr;      ///< Instruction under validation.
};

} // namespace il::verify
