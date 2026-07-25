//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/common/StringHash.hpp
// Purpose: Heterogeneous string hash functor for C++20 unordered containers.
//
// Key invariants:
//   * Equal string and string_view inputs have identical hashes.
//   * Case-insensitive hashing and equality use ASCII folding only.
//   * Transparent functors permit string_view lookup without temporary strings.
// Ownership: Header-only stateless functors; all key storage remains
//            container- or caller-owned.
// References: src/frontends/common/CharUtils.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares transparent ASCII string hashing and equality helpers.
//
//===----------------------------------------------------------------------===//
#pragma once

#include "frontends/common/CharUtils.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace il::frontends::common {

/// @brief Hash functor for heterogeneous string lookup (C++20).
/// @details Enables lookup with std::string_view keys in unordered_map
///          without allocating temporary std::string objects.
struct StringHash {
    using is_transparent = void;

    /// @brief Hash any key convertible to std::string_view.
    /// @tparam T String-like key type.
    /// @param key Key whose bytes are hashed.
    /// @return std::hash value of the equivalent string view.
    template <typename T> [[nodiscard]] std::size_t operator()(const T &key) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(key));
    }
};

/// @brief Utility to convert a string to lowercase.
/// @param s Input string.
/// @return Lowercase copy of the string.
inline std::string toLower(const std::string &s) {
    std::string result = s;
    for (char &c : result)
        c = char_utils::toLower(c);
    return result;
}

/// @brief Case-insensitive string comparison.
/// @param a First string.
/// @param b Second string.
/// @return True if strings are equal ignoring case.
inline bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (char_utils::toLower(a[i]) != char_utils::toLower(b[i]))
            return false;
    }
    return true;
}

/// @brief Case-insensitive string hash functor.
/// @details Enables case-insensitive lookup in unordered containers.
struct CaseInsensitiveHash {
    using is_transparent = void;

    /// @brief Hash a string view using ASCII case folding.
    /// @param key Key bytes to hash.
    /// @return Case-insensitive polynomial hash value.
    [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept {
        std::size_t hash = 0;
        for (char c : key) {
            hash = hash * 31 + static_cast<std::size_t>(
                                   static_cast<unsigned char>(char_utils::toLower(c)));
        }
        return hash;
    }

    /// @brief Hash an owned string using the string-view overload.
    /// @param key Key string to hash.
    /// @return Case-insensitive hash value.
    [[nodiscard]] std::size_t operator()(const std::string &key) const noexcept {
        return (*this)(std::string_view(key));
    }
};

/// @brief Case-insensitive string equality functor.
struct CaseInsensitiveEqual {
    using is_transparent = void;

    /// @brief Compare two keys with ASCII case folding.
    /// @param a First key.
    /// @param b Second key.
    /// @return True when the strings are equal ignoring ASCII case.
    [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept {
        return equalsIgnoreCase(a, b);
    }
};

} // namespace il::frontends::common
