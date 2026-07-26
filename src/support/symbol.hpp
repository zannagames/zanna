//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: support/symbol.hpp
// Purpose: Defines Symbol handle type for interned strings.
// Key invariants: Value 0 denotes an invalid symbol.
// Ownership/Lifetime: Symbols are value types.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines the compact handle used to identify interned strings.
/// @details A symbol contains only a one-based numeric slot. It does not retain
///          interner identity or own string storage, so resolving or comparing
///          handles semantically requires that they originate from the same
///          @ref il::support::StringInterner.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace il::support {

/// @brief Opaque identifier for interned strings.
/// @invariant 0 denotes an invalid symbol.
/// @ownership Value type, no ownership semantics.
/// @details Nonzero identifiers are assigned by a StringInterner. The handle
///          carries no provenance, generation, or direct reference to text.
struct Symbol {
    uint32_t id = 0; ///< 1-based index into the interner's storage; 0 is invalid.

    /// @brief Check whether the symbol is valid (non-zero id).
    /// @return true if the symbol holds a valid interned string identifier.
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return id != 0;
    }
};

/// @brief Compare two symbols for equality.
/// @param a First symbol.
/// @param b Second symbol.
/// @return True when both symbols contain the same numeric identifier.
/// @note Equal identifiers represent equal text only when both handles came
///       from the same interner state.
[[nodiscard]] constexpr bool operator==(Symbol a, Symbol b) noexcept {
    return a.id == b.id;
}

/// @brief Compare two symbols for inequality.
/// @param a First symbol.
/// @param b Second symbol.
/// @return True when the numeric identifiers differ.
/// @note Different identifiers from one interner always denote different text;
///       handles from unrelated interners have no shared semantic namespace.
[[nodiscard]] constexpr bool operator!=(Symbol a, Symbol b) noexcept {
    return a.id != b.id;
}
} // namespace il::support

namespace std {
/// @brief Standard hash specialization for Symbol.
/// @details Enables Symbol to be used as a key in unordered containers and
///          preserves the same numeric equality used by `operator==`.
template <> struct hash<il::support::Symbol> {
    /// @brief Compute the hash value for a symbol.
    /// @param s Symbol to hash.
    /// @return Hash value derived from the symbol's id.
    /// @details The direct identifier value is sufficient because Symbol
    ///          equality is itself defined by identifier equality.
    constexpr size_t operator()(il::support::Symbol s) const noexcept {
        return s.id;
    }
};
} // namespace std
