//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/UsingContext.hpp
// Purpose: Tracks file-scoped USING directives with declaration order and alias resolution.
// Key invariants:
//   * Import iteration preserves source declaration order.
//   * Alias lookup is case-insensitive while stored spellings are preserved.
//   * clear() removes imports and aliases together.
// Ownership: UsingContext owns all stored import and alias strings for the
//            lifetime of its file-level semantic context.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares ordered BASIC USING imports and alias resolution.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "support/source_location.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace il::frontends::basic {

/// @brief Represents file-scoped USING directives with deterministic order.
///
/// @details Tracks namespace imports and their optional aliases in the order
///          they appear in source. Provides case-insensitive alias resolution
///          for use during type name resolution.
///
/// @invariant Import order matches source declaration order.
/// @invariant Alias lookups are case-insensitive.
class UsingContext {
  public:
    /// @brief Represents a single USING directive.
    struct Import {
        /// Namespace path being imported (e.g., "Foo.Bar").
        std::string ns;

        /// Optional alias for the namespace; empty if no AS clause.
        std::string alias;

        /// Source location of the USING statement for diagnostics.
        il::support::SourceLoc loc;
    };

    /// @brief Add a USING directive to this context.
    /// @details Appends the import to the end of the declaration list and registers
    ///          the alias (if present) for case-insensitive lookup.
    /// @param ns Fully-qualified namespace path (e.g., "Foo.Bar").
    /// @param alias Optional alias string; empty if no AS clause.
    /// @param loc Source location of the USING statement.
    void add(std::string_view ns, std::string_view alias, il::support::SourceLoc loc);

    /// @brief Retrieve all imports in declaration order.
    /// @return Read-only reference to the import vector.
    [[nodiscard]] const std::vector<Import> &imports() const noexcept {
        return imports_;
    }

    /// @brief Check if an alias is registered (case-insensitive).
    /// @param alias Alias name to test.
    /// @return True if the alias was registered in a USING directive.
    [[nodiscard]] bool hasAlias(std::string_view alias) const;

    /// @brief Resolve an alias to its namespace path (case-insensitive).
    /// @param alias Alias name to resolve.
    /// @return Namespace path if alias exists; empty string otherwise.
    [[nodiscard]] std::string resolveAlias(std::string_view alias) const;

    /// @brief Clear all imports and aliases.
    /// @details Used when starting a new file in multi-file compilation.
    void clear();

  private:
    /// @brief Convert a string to lowercase for case-insensitive comparison.
    /// @param str Input string.
    /// @return Lowercase copy of the input.
    [[nodiscard]] static std::string toLower(std::string_view str);

    /// @brief Imports in declaration order.
    std::vector<Import> imports_;

    /// @brief Map from lowercase alias to namespace path (for case-insensitive lookup).
    std::unordered_map<std::string, std::string> alias_;
};

} // namespace il::frontends::basic
