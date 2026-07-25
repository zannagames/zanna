//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares lookup tables and type classification enums used by the IL
// verifier to validate instruction structure and semantics. These tables provide
// a lightweight metadata layer for opcodes with simple, regular verification rules.
//
// The IL verifier needs to know basic properties of each opcode: how many operands
// it accepts, what types those operands must have, whether it produces a result,
// and whether it has side effects. For instructions with regular patterns (like
// arithmetic and comparison opcodes), this file provides compact table-driven
// metadata that avoids duplicating verification logic across similar instructions.
//
// Key Responsibilities:
// - Define TypeClass enum mapping verifier type constraints to IL types
// - Provide OpProps describing arity, operand/result types, and trap behavior
// - Declare OpCheckSpec with detailed operand/result validation constraints
// - Offer lookup functions for querying opcode verification metadata
// - Support side-effect queries for optimization and reordering safety
//
// Design Notes:
// The tables in this file complement but don't replace the comprehensive SpecTables.
// SpecTables contains generated metadata for ALL opcodes, while VerifierTable
// provides legacy lookup interfaces for opcodes with simple regular structure.
// The lookup() functions return std::optional to indicate when an opcode requires
// specialized verification logic beyond table-driven validation. Over time, more
// verification may migrate to the SpecTables-based approach.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares legacy verifier metadata views and side-effect queries.
/// @details The compact arithmetic table coexists with schema-derived opcode
///          metadata. Optional lookups distinguish covered arithmetic patterns
///          from opcodes requiring other verification paths.

#pragma once

#include "il/core/Opcode.hpp"
#include "il/core/OpcodeInfo.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace il::verify {

/// @brief Classification used by the verifier to describe operand/result kinds.
enum class TypeClass : uint8_t {
    None,      ///< No constraint or unused slot.
    Void,      ///< Void type constraint.
    I1,        ///< 1-bit integer type.
    I16,       ///< 16-bit integer type.
    I32,       ///< 32-bit integer type.
    I64,       ///< 64-bit integer type.
    F64,       ///< 64-bit floating point type.
    Ptr,       ///< Pointer type.
    Str,       ///< Runtime string handle type.
    Error,     ///< Error object type.
    ResumeTok, ///< Resume token type.
    InstrType  ///< Use the instruction's declared type.
};

/// @brief Verification properties describing a simple arithmetic opcode.
struct OpProps {
    uint8_t arity;      ///< Number of value operands required.
    TypeClass operands; ///< Shared operand type requirement.
    TypeClass result;   ///< Result type produced on success.
    bool canTrap;       ///< Whether the opcode may trap at runtime.
};

/// @brief Specification of operand and result constraints for opcode verification.
struct OpCheckSpec {
    uint8_t numOperandsMin; ///< Minimum number of operands accepted.
    uint8_t numOperandsMax; ///< Maximum number of operands accepted; may be variadic.
    std::array<TypeClass, il::core::kMaxOperandCategories>
        operandTypes;    ///< Per-operand type constraints.
    TypeClass result;    ///< Result type constraint; InstrType refers to instruction type.
    bool hasSideEffects; ///< Whether the opcode performs side effects.
};

/// @brief Look up verification properties for supported arithmetic opcodes.
/// @param opcode Opcode to query.
/// @return Populated properties when @p opcode is described by the table; empty otherwise.
std::optional<OpProps> lookup(il::core::Opcode opcode);

/// @brief Look up detailed operand/result constraints for an opcode.
/// @param opcode Opcode to query.
/// @return Populated specification when @p opcode is described by the table; empty otherwise.
std::optional<OpCheckSpec> lookupSpec(il::core::Opcode opcode);

/// @brief Determine whether an opcode performs side effects observable by the verifier.
/// @param opcode Opcode to query.
/// @return True when @p opcode has side effects; false otherwise.
bool hasSideEffects(il::core::Opcode opcode);

/// @brief Determine whether an opcode is pure (side-effect free).
/// @param opcode Opcode to query.
/// @return True when @p opcode has no side effects; false otherwise.
inline bool isPure(il::core::Opcode opcode) {
    return !hasSideEffects(opcode);
}

} // namespace il::verify
