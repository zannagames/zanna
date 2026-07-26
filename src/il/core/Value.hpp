//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/Value.hpp
// Purpose: Declares the Value struct -- a tagged union representing operands
//          and constants in IL instructions (Temp, ConstInt, ConstFloat,
//          ConstStr, GlobalAddr, NullPtr). Includes factory methods, semantic
//          equality, and hashing utilities for use in unordered containers.
// Key invariants:
//   - Only one payload member is active at a time per the Kind discriminant.
//   - Factory methods enforce valid discriminant/payload combinations.
//   - isBool is meaningful only when kind == Kind::ConstInt.
// Ownership/Lifetime: Value type with lightweight value semantics; no heap
//          allocation beyond std::string for string/global payloads.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares tagged IL operand values, factories, equality, and hashing.
 *
 * @details `Value` compactly represents SSA temporaries, scalar literals,
 *          string literals, global addresses, and null pointers. Factory
 *          functions establish valid tag/payload pairs, while the free
 *          comparison and hashing helpers apply payload-aware semantics
 *          suitable for analysis maps and sets.
 */

#pragma once

#include <string>

namespace il::core {

/// @brief Tagged value used as operands and results in IL.
/// @details Uses a union for i64/f64/id to save 8+ bytes per Value since only
///          one payload is active at a time based on the kind discriminant.
struct Value {
    /// @brief Enumerates the different value forms.
    /// @details The active kind selects either the numeric union member, the
    ///          owned string payload, or no payload for `NullPtr`.
    enum class Kind { Temp, ConstInt, ConstFloat, ConstStr, GlobalAddr, NullPtr };
    /// @brief Discriminant selecting which payload is active.
    Kind kind{Kind::NullPtr};

    /// @brief Union of mutually exclusive payloads (only one active per kind).
    /// @details Saves 8+ bytes per Value vs separate fields. Access the member
    ///          matching the current kind: i64 for ConstInt, f64 for ConstFloat,
    ///          id for Temp. Other kinds use the str field instead.
    union {
        /// @brief Integer payload used when kind == Kind::ConstInt.
        long long i64;
        /// @brief Floating-point payload used when kind == Kind::ConstFloat.
        double f64;
        /// @brief Temporary identifier used when kind == Kind::Temp.
        unsigned id;
    };

    /// @brief String payload for string constants and global names.
    std::string str;

    /// @brief Flag set when the integer literal represents an i1 boolean.
    /// @invariant Only meaningful when kind == Kind::ConstInt.
    bool isBool{false};

    /// @brief Default constructor initializes to NullPtr with zeroed union.
    Value() : kind{Kind::NullPtr}, i64{0}, str{}, isBool{false} {}

    /// @brief Construct a temporary value.
    /// @param t Identifier of the temporary.
    /// @return Value with kind Kind::Temp and id set to t.
    /// @invariant result.kind == Kind::Temp.
    static Value temp(unsigned t);

    /// @brief Construct an integer constant value.
    /// @param v Signed integer literal.
    /// @return Value with kind Kind::ConstInt and i64 set to v.
    /// @invariant result.kind == Kind::ConstInt.
    static Value constInt(long long v);

    /// @brief Construct a boolean constant value.
    /// @param v Boolean literal to encode as an i1 constant.
    /// @return Value with kind Kind::ConstInt and boolean flag set.
    /// @invariant result.kind == Kind::ConstInt and result.isBool == true.
    static Value constBool(bool v);

    /// @brief Construct a floating-point constant value.
    /// @param v IEEE-754 double literal.
    /// @return Value with kind Kind::ConstFloat and f64 set to v.
    /// @invariant result.kind == Kind::ConstFloat.
    static Value constFloat(double v);

    /// @brief Construct a string constant value.
    /// @param s String literal; moved into the value.
    /// @return Value with kind Kind::ConstStr and str set to @p s.
    /// @invariant result.kind == Kind::ConstStr.
    static Value constStr(std::string s);

    /// @brief Construct a global address value.
    /// @param s Name of the global symbol; moved into the value.
    /// @return Value with kind Kind::GlobalAddr and str set to @p s.
    /// @invariant result.kind == Kind::GlobalAddr.
    static Value global(std::string s);

    /// @brief Construct a null pointer value.
    /// @return Value with kind Kind::NullPtr.
    /// @invariant result.kind == Kind::NullPtr.
    static Value null();
};

/// @brief Render a value using canonical textual IL spelling.
/// @param v Value to format.
/// @return Temporary, literal, global-address, or null representation suitable
///         for serialization and diagnostics.
std::string toString(const Value &v);

//===----------------------------------------------------------------------===//
// Value Comparison and Hashing Utilities
//===----------------------------------------------------------------------===//

/// @brief Compare two Values for semantic equality.
/// @details Compares values based on their kind and payload. Temporaries compare
///          by id, integers by value and boolean flag, floats by bit pattern,
///          and string-backed values by string content. NullPtr values are
///          always equal to other NullPtr values.
/// @param a First value to compare.
/// @param b Second value to compare.
/// @return True if both values represent the same semantic payload.
bool valueEquals(const Value &a, const Value &b) noexcept;

/// @brief Hash a Value for use in unordered containers.
/// @details Produces a stable hash combining the value kind with its payload.
///          Uses mixing constants to reduce collisions across different value
///          kinds. The hash is deterministic but not cryptographically secure.
/// @param v Value to hash.
/// @return Hash code suitable for std::unordered_map/set.
size_t valueHash(const Value &v) noexcept;

/// @name Hash Constants
/// @brief Named constants for hash mixing operations.
/// @details Using named constants improves readability and documents the
///          provenance of magic numbers used in hash functions.
/// @{

/// @brief Murmur-like mixing constant for combining hash values.
inline constexpr size_t kHashKindMix = 1469598103934665603ULL;

/// @brief Golden ratio fractional constant used in hash mixing.
inline constexpr size_t kHashPhiMix = 0x9e3779b97f4a7c15ULL;

/// @brief Sentinel hash value for null pointers.
inline constexpr size_t kHashNullSentinel = 0xabcdefULL;

/// @brief Sentinel hash bit for boolean flag discrimination.
inline constexpr size_t kHashBoolFlag = 0xBEEF;

/// @}

} // namespace il::core
