//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares shared utilities and specialized checkers used by table-driven
// instruction verification. It provides the common infrastructure that implements
// the verification strategies referenced by the SpecTables metadata.
//
// The table-driven verification system separates opcode metadata (operand counts,
// type categories, verification strategies) from the implementation of those
// strategies. This file contains the strategy implementations - specialized checking
// functions for memory operations, arithmetic instructions, casts, bounds checks,
// and runtime calls - referenced by the VerifyStrategy enum in SpecTables.
//
// Key Responsibilities:
// - Provide diagnostic formatting helpers for consistent error messages
// - Implement specialized checkers for memory operations (alloca, load, store, gep)
// - Validate runtime call instructions (call.extern, call.func)
// - Check runtime error handling operations (trap, trap.err, trap.from.err)
// - Verify arithmetic and cast operations with proper type constraints
// - Implement the default checker for simple instructions
//
// Design Rationale:
// The il::verify::checker namespace groups strategy implementations separately from
// the dispatch logic. Each checker accepts a VerifyCtx containing all verification
// state (function, block, instruction, type environment, diagnostics) and returns
// Expected<void> for uniform error propagation. Helper functions like formatDiag()
// and fail() reduce boilerplate in checker implementations.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares shared table-driven instruction checker strategies.
/// @details Checkers consume a complete `VerifyCtx`, return structured
///          diagnostics, and share formatting and failure helpers. Specialized
///          implementations are split across arithmetic, memory, and runtime
///          translation units.

#pragma once

#include "il/core/Instr.hpp"
#include "il/core/Type.hpp"
#include "il/verify/DiagFormat.hpp"
#include "il/verify/VerifierTable.hpp"
#include "il/verify/VerifyCtx.hpp"
#include "support/diag_expected.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace il::verify::checker {

using il::core::Type;
using il::support::Expected;
using il::support::makeError;

/// @brief Format a diagnostic message for the current instruction context.
/// @param ctx Verification context providing function, block, and instruction.
/// @param message Human-readable text describing the issue.
/// @return Fully formatted diagnostic string.
inline std::string formatDiag(const VerifyCtx &ctx, std::string_view message) {
    return formatInstrDiag(ctx.fn, ctx.block, ctx.instr, message);
}

/// @brief Construct an error Expected value with a formatted diagnostic.
/// @param ctx Verification context that supplies diagnostic metadata.
/// @param message Human-readable text describing the issue.
/// @return Expected holding a diagnostic error populated with @p message.
inline Expected<void> fail(const VerifyCtx &ctx, std::string_view message) {
    return Expected<void>(makeError(ctx.instr.loc, formatDiag(ctx, message)));
}

/// @brief Helper to build an error Expected<T> with a formatted diagnostic.
/// @tparam T Desired Expected value type.
/// @param ctx Verification context that supplies diagnostic metadata.
/// @param message Human-readable text describing the issue.
/// @return Expected<T> holding a diagnostic error populated with @p message.
template <typename T> inline Expected<T> failWith(const VerifyCtx &ctx, std::string_view message) {
    return Expected<T>(makeError(ctx.instr.loc, formatDiag(ctx, message)));
}

/// @brief Translate a verifier table type class into a concrete kind when available.
/// @param typeClass Classification to translate.
/// @return Matching kind or std::nullopt when the class is dynamic.
std::optional<Type::Kind> kindFromClass(TypeClass typeClass);

/// @brief Translate a verifier table type class into a concrete type when available.
/// @param typeClass Classification to translate.
/// @return Matching type or std::nullopt when the class depends on instruction metadata.
std::optional<Type> typeFromClass(TypeClass typeClass);

// --- Arithmetic verification helpers ---

/// @brief Verify that all operands of the current instruction match a given type kind.
/// @param ctx Verification context with the instruction to check.
/// @param kind Expected type kind for every operand.
/// @return Success when all operands conform; diagnostic error otherwise.
[[nodiscard]] Expected<void> expectAllOperandType(const VerifyCtx &ctx, Type::Kind kind);

/// @brief Verify a binary arithmetic instruction (e.g., add, sub, mul, div).
/// @details Checks that both operands match @p operandKind and the result type
///          matches @p resultType. Produces a diagnostic on type mismatch.
/// @param ctx Verification context containing the binary instruction.
/// @param operandKind Required type kind for both left and right operands.
/// @param resultType Expected result type of the computation.
/// @return Success when types are valid; diagnostic error otherwise.
[[nodiscard]] Expected<void> checkBinary(const VerifyCtx &ctx,
                                         Type::Kind operandKind,
                                         Type resultType);

/// @brief Verify a unary arithmetic instruction (e.g., neg, not, abs).
/// @details Checks that the single operand matches @p operandKind and the
///          result type matches @p resultType.
/// @param ctx Verification context containing the unary instruction.
/// @param operandKind Required type kind for the operand.
/// @param resultType Expected result type of the computation.
/// @return Success when types are valid; diagnostic error otherwise.
[[nodiscard]] Expected<void> checkUnary(const VerifyCtx &ctx,
                                        Type::Kind operandKind,
                                        Type resultType);

/// @brief Verify an index-bounds-check instruction (idx.chk).
/// @details Validates that the index and bound operands are integers and
///          that the instruction metadata is consistent.
/// @param ctx Verification context containing the idx.chk instruction.
/// @return Success when operand types and metadata are valid; diagnostic error otherwise.
[[nodiscard]] Expected<void> checkIdxChk(const VerifyCtx &ctx);

// --- Memory operation verification helpers ---

/// @brief Verify an alloca instruction allocates a valid local variable.
/// @param ctx Verification context containing the alloca instruction.
/// @return Success when type and metadata are valid; diagnostic error otherwise.
[[nodiscard]] Expected<void> checkAlloca(const VerifyCtx &ctx);

/// @brief Verify a GEP (get-element-pointer) instruction.
/// @details Checks that the base operand is a pointer type and that the byte
///          offset is i64, producing a pointer result. Constant offsets rooted
///          in a statically sized alloca are range checked.
/// @param ctx Verification context containing the GEP instruction.
/// @return Success when base and index types are valid; diagnostic error otherwise.
[[nodiscard]] Expected<void> checkGEP(const VerifyCtx &ctx);

/// @brief Verify a load instruction reads from a valid pointer.
/// @details Confirms the operand is a pointer type and the result type matches
///          the declared element type of the load.
/// @param ctx Verification context containing the load instruction.
/// @return Success when pointer and result types are consistent; diagnostic error otherwise.
[[nodiscard]] Expected<void> checkLoad(const VerifyCtx &ctx);

/// @brief Verify a store instruction writes through a valid pointer.
/// @details Confirms the address operand is a pointer and the value operand
///          type is compatible with the store's declared type annotation.
/// @param ctx Verification context containing the store instruction.
/// @return Success when address and value types match; diagnostic error otherwise.
[[nodiscard]] Expected<void> checkStore(const VerifyCtx &ctx);

/// @brief Verify an addr.of instruction that addresses a string global.
/// @param ctx Verification context containing the addr.of instruction.
/// @return Success when operand and result types are valid; diagnostic error otherwise.
[[nodiscard]] Expected<void> checkAddrOf(const VerifyCtx &ctx);

/// @brief Verify a const.str instruction that loads a string constant.
/// @details Checks that the global string label references a valid global and
///          the result type is the string type.
/// @param ctx Verification context containing the const.str instruction.
/// @return Success when the string reference is valid; diagnostic error otherwise.
[[nodiscard]] Expected<void> checkConstStr(const VerifyCtx &ctx);

/// @brief Verify a gaddr instruction that returns mutable global storage.
/// @param ctx Verification context containing the gaddr instruction.
/// @return Success when the global reference is valid and addressable.
[[nodiscard]] Expected<void> checkGAddr(const VerifyCtx &ctx);

/// @brief Verify a const.null instruction that produces a nullable value.
/// @details Accepts ptr, str, error, and resumetok result annotations.
/// @param ctx Verification context containing the const.null instruction.
/// @return Success for a supported nullable result kind; diagnostic otherwise.
[[nodiscard]] Expected<void> checkConstNull(const VerifyCtx &ctx);

// --- Runtime call verification helpers ---

/// @brief Verify a call instruction (call.extern or call.func).
/// @details Validates callee existence, argument count, argument types, and
///          return type against the callee's declared signature.
/// @param ctx Verification context containing the call instruction.
/// @return Success when the call matches the callee signature; diagnostic error otherwise.
[[nodiscard]] Expected<void> checkCall(const VerifyCtx &ctx);

/// @brief Verify a trap instruction that triggers a runtime error.
/// @details Checks the trap kind annotation is valid and operand types are
///          consistent with the trap classification.
/// @param ctx Verification context containing the trap instruction.
/// @return Success when trap metadata is well-formed; diagnostic error otherwise.
[[nodiscard]] Expected<void> checkTrapKind(const VerifyCtx &ctx);

/// @brief Verify a trap.err instruction that raises a user-level error.
/// @param ctx Verification context containing the trap.err instruction.
/// @return Success when operand types conform to error-raising semantics.
[[nodiscard]] Expected<void> checkTrapErr(const VerifyCtx &ctx);

/// @brief Verify a trap.from.err instruction that re-raises from an error context.
/// @param ctx Verification context containing the trap.from.err instruction.
/// @return Success when the error context and operands are valid.
[[nodiscard]] Expected<void> checkTrapFromErr(const VerifyCtx &ctx);

/// @brief Accept a shift after table-driven structural and type validation.
/// @details IL shift counts are defined modulo 64, so constant amounts require
///          no additional range diagnostic.
/// @param ctx Verification context containing the shift instruction.
/// @return Always succeeds.
[[nodiscard]] Expected<void> checkShift(const VerifyCtx &ctx);

/// @brief Default validator for instructions needing no specialized semantics.
/// @details Opcode-signature checking and type recording are handled by the
///          surrounding dispatch pipeline.
/// @param ctx Verification context that owns the instruction.
/// @return Always empty because structural checks handle failures.
[[nodiscard]] Expected<void> checkDefault(const VerifyCtx &ctx);

} // namespace il::verify::checker
