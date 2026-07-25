//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/RuntimePropertyIndex.hpp
// Purpose: Declare the case-insensitive runtime property metadata index used by
//          BASIC semantic analysis.
// Key invariants:
//   * Keys combine normalized class and property names.
//   * Indexed values own stable copies of catalog strings.
//   * Missing setters are represented as read-only properties.
// Ownership: Index stores copies of strings for stable access.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares runtime property metadata and its BASIC lookup index.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace il::runtime {
struct RuntimeClass; // fwd
}

namespace il::frontends::basic {

/// @brief Runtime property metadata required for type checking and lowering.
struct RuntimePropertyInfo {
    std::string type;     ///< IL scalar type string (e.g., "i64", "i1").
    std::string getter;   ///< Canonical extern target for getter.
    std::string setter;   ///< Canonical extern target for setter (empty if none).
    bool readonly{false}; ///< True if setter is empty.
};

/// @brief Case-insensitive snapshot of runtime class properties.
/// @details Seeding replaces the stored catalog. Lookups return value copies so
///          callers do not depend on the index's internal container lifetime.
class RuntimePropertyIndex {
  public:
    /// @brief Replace the index with properties from the runtime class catalog.
    /// @param classes Runtime classes whose property metadata will be copied.
    void seed(const std::vector<il::runtime::RuntimeClass> &classes);

    /// @brief Find property info for class + property name.
    /// @param classQName Fully qualified runtime class name.
    /// @param propName Property identifier to resolve.
    /// @return Copied metadata, or std::nullopt when the property is unknown.
    [[nodiscard]] std::optional<RuntimePropertyInfo> find(std::string_view classQName,
                                                          std::string_view propName) const;

  private:
    /// @brief Lowercase one identifier for key construction.
    /// @param s Source spelling.
    /// @return Lowercase copy of @p s.
    static std::string toLower(std::string_view s);

    /// @brief Construct a normalized composite class/property key.
    /// @param cls Fully qualified class name.
    /// @param prop Property name.
    /// @return Stable key used by map_.
    static std::string keyFor(std::string_view cls, std::string_view prop);

    /// @brief Property metadata keyed by normalized `class|property`.
    std::unordered_map<std::string, RuntimePropertyInfo> map_;
};

/// @brief Access singleton property index.
/// @return Reference to the process-wide RuntimePropertyIndex.
RuntimePropertyIndex &runtimePropertyIndex();

} // namespace il::frontends::basic
