//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/Type.hpp
// Purpose: Declares the Type struct -- a lightweight value-based wrapper
//          around a Type::Kind enum representing the 10 primitive types in
//          Zanna IL (void, i1, i16, i32, i64, f64, ptr, str, error,
//          resumetok). Provides string conversion to spec mnemonics.
// Key invariants:
//   - Type is a POD-like struct with value semantics; safe to copy freely.
//   - No parametric types: pointer and string types are opaque.
//   - toString()/kindToString() return an empty string for invalid enum values.
// Ownership/Lifetime: Value type with no dynamic resources.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

#pragma once

#include <optional>
#include <string>

namespace il::core {

/// @brief Simple type wrapper for IL primitive types.
struct Type {
    /// @brief Enumerates primitive IL types.
    enum class Kind { Void, I1, I16, I32, I64, F64, Ptr, Str, Error, ResumeTok };
    Kind kind; ///< Discriminator specifying the active kind

    /// @brief Construct a type of kind @p k.
    /// @param k Desired kind.
    explicit Type(Kind k = Kind::Void);

    /// @brief Convert type to string representation.
    /// @return Lowercase type mnemonic.
    std::string toString() const;
};

/// @brief Convert kind @p k to its mnemonic string.
/// @param k Kind to convert.
/// @return Lowercase mnemonic defined in the spec.
std::string kindToString(Type::Kind k);

/// @brief Return the storage width for a scalar IL type when it is defined.
/// @details The verifier, alias analysis, and memory optimizers share this
///          helper so byte-width reasoning cannot drift between subsystems.
///          Void has no storage size and returns @c std::nullopt.
/// @param kind Primitive type kind to inspect.
/// @return Size in bytes, or empty when @p kind has no object representation.
std::optional<unsigned> storageSizeBytes(Type::Kind kind);

/// @brief Return the storage width for a concrete IL type.
/// @param type Type wrapper whose kind should be inspected.
/// @return Size in bytes, or empty when @p type has no object representation.
std::optional<unsigned> storageSizeBytes(Type type);

} // namespace il::core
