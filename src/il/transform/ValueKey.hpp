//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/ValueKey.hpp
// Purpose: Internal helpers for computing expression identity keys used by
//          EarlyCSE and GVN. Normalises commutative operands, provides stable
//          hashing for Value operands, and gates which opcodes are safe for
//          CSE (pure, deterministic, and free of memory effects).
// Key invariants:
//   - Commutative operands are sorted to produce canonical keys.
//   - Trapping expressions are eligible only when a dominating identical
//     evaluation proves the reused execution completed successfully.
// Ownership/Lifetime: ValueKey is a value type owning a vector of operands.
//          Hash and equality functors are stateless.
// Links: il/core/Instr.hpp, il/core/OpcodeInfo.hpp, il/core/Value.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares canonical expression keys shared by EarlyCSE and GVN.
 *
 * @details This interface defines semantic value hashing and equality,
 *          canonicalizes commutative instruction operands, identifies opcodes
 *          eligible for common-subexpression elimination, and constructs
 *          stable keys for pure result-producing instructions.
 */

#pragma once

#include "il/core/Instr.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"

#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace il::transform {

/// @brief Hash a Value by kind and payload for use in expression keys.
struct ValueHash {
    /// @brief Compute the shared semantic hash for an IL value.
    /// @param v Value whose kind and payload are hashed.
    /// @return Hash suitable for unordered expression-key containers.
    size_t operator()(const il::core::Value &v) const noexcept;
};

/// @brief Equality on Value payloads (ignores name metadata).
struct ValueEq {
    /// @brief Compare IL values by semantic kind and payload.
    /// @param a First value.
    /// @param b Second value.
    /// @return True when both represent the same temporary or literal.
    bool operator()(const il::core::Value &a, const il::core::Value &b) const noexcept;
};

/// @brief Normalised key describing a pure instruction.
struct ValueKey {
    /// @brief Opcode contributing to expression identity.
    il::core::Opcode op{il::core::Opcode::Count};
    /// @brief Result type discriminator.
    il::core::Type::Kind type{il::core::Type::Kind::Void};
    /// @brief Canonically ordered operand payloads.
    std::vector<il::core::Value> operands;

    /// @brief Compare two normalized expression identities.
    /// @param o Other key.
    /// @return True when opcode, type, and operands match.
    bool operator==(const ValueKey &o) const noexcept;
};

/// @brief Hash functor for normalized instruction keys.
struct ValueKeyHash {
    /// @brief Combine opcode, type, and operand hashes.
    /// @param k Key to hash.
    /// @return Hash suitable for unordered containers.
    size_t operator()(const ValueKey &k) const noexcept;
};

/// @brief Returns true when @p op is treated as commutative for CSE purposes.
/// @param op Opcode to classify.
/// @return True when swapping the two operands preserves semantics.
bool isCommutativeCSE(il::core::Opcode op) noexcept;

/// @brief Returns true if @p op is safe for expression-based CSE/GVN.
/// @details Includes deterministic non-memory arithmetic, bitwise operations,
///          comparisons, and boolean casts, plus checked arithmetic whose
///          dominating evaluation makes reuse safe.
/// @param op Opcode to classify.
/// @return True when the expression can participate in value-based CSE.
bool isSafeCSEOpcode(il::core::Opcode op) noexcept;

/// @brief Build a normalised expression key for @p instr when eligible.
/// @param instr Candidate result-producing instruction.
/// @return Populated ValueKey or std::nullopt when @p instr is not a CSE
///         candidate (side effects, memory, disallowed opcode, missing result).
std::optional<ValueKey> makeValueKey(const il::core::Instr &instr);

} // namespace il::transform
