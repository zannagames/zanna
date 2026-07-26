//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/control_flow.hpp
// Purpose: Declare switch dispatch cache structures for VM control flow helpers.
// Key invariants: Cached dispatch data remains valid for the lifetime of the owning
// Ownership/Lifetime: SwitchCache owns memoized backend data keyed by instruction pointer.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares switch-dispatch modes and cached backend representations.
/// @details Switch instructions may use dense, sorted, hashed, or linear
///          lookup. The cache stores immutable instruction-derived metadata so
///          repeated execution can reuse the selected representation.

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <variant>
#include <vector>

namespace zanna::vm {

/// @brief Enumerates dispatch strategies available for switch instructions.
enum class SwitchMode {
    Auto,   ///< Allow heuristics to select the backend.
    Dense,  ///< Force dense jump table construction.
    Sorted, ///< Force sorted case list construction.
    Hashed, ///< Force hashed case mapping construction.
    Linear  ///< Bypass caching and perform a linear scan.
};

/// @brief Query the global switch dispatch mode override.
/// @return Current process-wide mode used when constructing switch caches.
SwitchMode getSwitchMode();

/// @brief Set the global switch dispatch mode override.
/// @param mode Mode applied to subsequently selected switch backends.
void setSwitchMode(SwitchMode mode);

/// @brief Dense jump table backing switch dispatch.
struct DenseJumpTable {
    int32_t base = 0;             ///< Minimum case value encoded in the table.
    std::vector<int32_t> targets; ///< Successor indices; -1 designates default branch.
};

/// @brief Sorted case list supporting binary search dispatch.
struct SortedCases {
    std::vector<int32_t> keys;      ///< Sorted case values.
    std::vector<int32_t> targetIdx; ///< Parallel successor indices for @ref keys.
};

/// @brief Hashed case mapping for sparse switch dispatch.
struct HashedCases {
    std::unordered_map<int32_t, int32_t> map; ///< Maps case value to successor index.
};

/// @brief Variant selecting which backend implementation to use for an instruction.
using SwitchBackend = std::variant<std::monostate, DenseJumpTable, SortedCases, HashedCases>;

/// @brief Cached dispatch metadata for a single switch instruction.
struct SwitchCacheEntry {
    SwitchBackend backend;   ///< Backend-specific data.
    int32_t defaultIdx = -1; ///< Index used when no case matches.

    /// @brief Enumerates which backend was materialised for diagnostics.
    enum Kind { Linear, Dense, Sorted, Hashed } kind = Sorted;
};

/// @brief Memoization table keyed by instruction identity.
struct SwitchCache {
    std::unordered_map<const void *, SwitchCacheEntry> entries; ///< Instruction cache.

    /// @brief Clear all cached dispatch entries.
    void clear() {
        entries.clear();
    }
};

} // namespace zanna::vm
