//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: support/alignment.hpp
// Purpose: Provides alignment utilities for memory and offset calculations.
// Key invariants: Alignment arguments must be powers of two.
// Ownership/Lifetime: Pure value-level functions, no allocations or state.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Provides checked power-of-two alignment utilities.
/// @details The templates accept integral values, reject negative offsets and
///          invalid alignment boundaries, and expose both optional-returning
///          and throwing overflow behavior without allocating state.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace il::support {

/// @brief Check whether @p value is a positive power of two.
/// @tparam T Integral type of the value being checked.
/// @param value Candidate alignment value.
/// @return True when @p value is non-zero, positive, and has exactly one bit set.
/// @details Alignment utilities use this helper to validate preconditions before
///          applying bit-mask formulas.  Signed negative values are rejected.
template <typename T> [[nodiscard]] constexpr bool isPowerOfTwo(T value) noexcept {
    static_assert(std::is_integral_v<T>, "isPowerOfTwo requires an integral type");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, bool>, "isPowerOfTwo does not accept bool");
    if (value <= 0)
        return false;
    using Unsigned = std::make_unsigned_t<T>;
    const Unsigned unsignedValue = static_cast<Unsigned>(value);
    return (unsignedValue & (unsignedValue - 1)) == 0;
}

/// @brief Try to round a value up to the next multiple of alignment.
/// @tparam T Integral type of the value being aligned.
/// @param n Value to align.
/// @param alignment Alignment boundary (must be power of two).
/// @return Smallest aligned value, or std::nullopt for invalid alignment/overflow.
/// @details This checked form avoids the overflow-prone `(n + alignment - 1)`
///          expression.  Signed negative values are treated as invalid because
///          byte and offset alignment in this support library is non-negative.
template <typename T>
[[nodiscard]] constexpr std::optional<T> checkedAlignUp(T n, T alignment) noexcept {
    static_assert(std::is_integral_v<T>, "checkedAlignUp requires an integral type");
    if (!isPowerOfTwo(alignment))
        return std::nullopt;
    if constexpr (std::is_signed_v<T>) {
        if (n < 0)
            return std::nullopt;
    }

    using Unsigned = std::make_unsigned_t<T>;
    const Unsigned unsignedValue = static_cast<Unsigned>(n);
    const Unsigned unsignedAlignment = static_cast<Unsigned>(alignment);
    const Unsigned mask = unsignedAlignment - 1;
    const Unsigned remainder = unsignedValue & mask;
    if (remainder == 0)
        return n;

    const Unsigned delta = unsignedAlignment - remainder;
    if (unsignedValue > std::numeric_limits<Unsigned>::max() - delta)
        return std::nullopt;

    const Unsigned aligned = unsignedValue + delta;
    if constexpr (std::is_signed_v<T>) {
        if (aligned > static_cast<Unsigned>(std::numeric_limits<T>::max()))
            return std::nullopt;
    }
    return static_cast<T>(aligned);
}

/// @brief Round a value up to the next multiple of alignment.
/// @details Computes the smallest value >= n that is a multiple of alignment.
///          Invalid alignments or overflow throw @ref std::overflow_error so
///          layout code cannot accidentally continue with an unaligned value.
///          Callers that need non-throwing behavior should use
///          @ref checkedAlignUp directly.
/// @tparam T Integral type of the value being aligned.
/// @param n Value to align.
/// @param alignment Alignment boundary (must be power of two).
/// @return Smallest value >= n that is a multiple of alignment.
/// @throws std::overflow_error when @p alignment is invalid or the result overflows.
template <typename T> [[nodiscard]] constexpr T alignUp(T n, T alignment) {
    static_assert(std::is_integral_v<T>, "alignUp requires an integral type");
    auto aligned = checkedAlignUp(n, alignment);
    if (!aligned)
        throw std::overflow_error("alignUp invalid alignment or overflow");
    return *aligned;
}

/// @brief Check if a value is aligned to a given boundary.
/// @tparam T Integral type of the value being checked.
/// @param n Value to check.
/// @param alignment Alignment boundary (must be power of two).
/// @return true if n is a multiple of alignment.
template <typename T> [[nodiscard]] constexpr bool isAligned(T n, T alignment) noexcept {
    static_assert(std::is_integral_v<T>, "isAligned requires an integral type");
    if (!isPowerOfTwo(alignment))
        return false;
    if constexpr (std::is_signed_v<T>) {
        if (n < 0)
            return false;
    }
    using Unsigned = std::make_unsigned_t<T>;
    return (static_cast<Unsigned>(n) & (static_cast<Unsigned>(alignment) - 1)) == 0;
}

} // namespace il::support
