//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/verify/BlockMap.hpp
// Purpose: Define a block label lookup map using string_view keys to avoid
//          unnecessary string copies when mapping labels to basic blocks.
// Key invariants: The string_view keys reference BasicBlock::label strings
//                 which are owned by the Function. The map must not outlive
//                 the function whose blocks it indexes.
// Ownership/Lifetime: Maps borrow label strings from BasicBlock::label. Callers
//                     must ensure the source Function remains valid for the
//                     lifetime of any BlockMap built from it.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the borrowed-label map used to resolve IL branch targets.
/// @details The map supports allocation-free lookup with `std::string`,
///          `std::string_view`, and compatible key forms. Its keys remain views
///          into block-owned label storage and therefore inherit that storage's
///          lifetime and invalidation constraints.

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace il::core {
struct BasicBlock;
} // namespace il::core

namespace il::verify {

/// @brief Transparent hash functor enabling heterogeneous lookup by string_view.
/// @details The `is_transparent` tag allows std::unordered_map::find() to accept
///          std::string_view arguments without converting them to std::string,
///          eliminating temporary allocations on every lookup.
struct BlockMapHash {
    using is_transparent = void;

    /// @brief Hash a non-owning label view.
    /// @param sv Label contents to hash.
    /// @return Hash value produced by `std::hash<std::string_view>`.
    std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }

    /// @brief Hash an owning label without allocating a temporary string.
    /// @param s Label contents to hash through a transient view.
    /// @return Hash value compatible with the string-view overload.
    std::size_t operator()(const std::string &s) const noexcept {
        return std::hash<std::string_view>{}(std::string_view{s});
    }
};

/// @brief Transparent equality comparator for heterogeneous lookup.
/// @details Works in conjunction with BlockMapHash to enable find() calls
///          that accept string_view without constructing temporary strings.
struct BlockMapEqual {
    using is_transparent = void;

    /// @brief Compare two label views by character content.
    /// @param lhs First label.
    /// @param rhs Second label.
    /// @return `true` when both views contain the same sequence.
    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
        return lhs == rhs;
    }
};

/// @brief Map from block labels to block pointers using string_view keys.
/// @details Uses transparent hashing and equality to allow lookups with
///          std::string_view arguments, avoiding allocations when resolving
///          branch targets. The string_view keys reference BasicBlock::label
///          strings, so the map must not outlive the owning Function.
///
/// @note Lifetime safety: Maps of this type borrow label strings from
///       BasicBlock::label fields. The source Function must remain valid
///       for the entire lifetime of the map. Do not store these maps beyond
///       the scope of the verification pass operating on a single function.
using BlockMap =
    std::unordered_map<std::string_view, const il::core::BasicBlock *, BlockMapHash, BlockMapEqual>;

} // namespace il::verify
