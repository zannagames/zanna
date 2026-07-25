//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/TypeRegistry.hpp
// Purpose: Seed read-only entries for built-in namespaced runtime types.
// Key invariants:
//   * Entries are catalog-only; member metadata lives in dedicated indexes.
//   * Qualified names live under the reserved root `Zanna`.
//   * Seeding registers namespace chains before class or interface names.
// Ownership: TypeRegistry owns copied classification keys. Catalog and
//            NamespaceRegistry arguments remain caller-owned.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares BASIC runtime type classification and legacy catalog seeding.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace il::runtime {
struct RuntimeClass; // fwd decl
}

namespace il::frontends::basic {

class NamespaceRegistry;

/// @brief Category for built-in external types to seed via legacy catalog.
enum class ExternalTypeCategory {
    /// Seed the entry as a namespace class.
    Class,
    /// Seed the entry as a namespace interface.
    Interface,
};

/// @brief Catalog entry describing a built-in external type (legacy seed).
struct BuiltinExternalType {
    /// Fully qualified runtime type name.
    const char *qualifiedName;
    /// Namespace registry declaration kind.
    ExternalTypeCategory category;
    /// Legacy descriptive tag retained for compatibility.
    const char *tag;
};

/// @brief Type classification for TypeRegistry entries.
enum class TypeKind {
    /// Name is absent from the runtime type catalog.
    Unknown,
    /// Legacy classification retained for compatibility.
    BuiltinExternalType,  // Legacy name kept for compatibility with existing tests
    /// Preferred classification for runtime classes.
    BuiltinExternalClass, // Preferred name for runtime class entries
};

/// @brief Registry of known type names discovered from the runtime class catalog.
/// @details Provides case-insensitive lookup. BASIC alias "STRING" resolves to
///          the same entry as "Zanna.String".
class TypeRegistry {
  public:
    /// @brief Register all runtime classes as BuiltinExternalType entries.
    /// @param classes Runtime class catalog whose qualified names are copied.
    void seedRuntimeClasses(const std::vector<il::runtime::RuntimeClass> &classes);

    /// @brief Lookup kind for a qualified type name (case-insensitive).
    /// @param qualifiedName Fully qualified name or BASIC `STRING` alias.
    /// @return Registered classification, or TypeKind::Unknown when absent.
    [[nodiscard]] TypeKind kindOf(std::string_view qualifiedName) const;

  private:
    /// @brief Normalize a type spelling for case-insensitive lookup.
    /// @param s Type spelling to normalize.
    /// @return Lowercase copy of @p s.
    static std::string toLower(std::string_view s);

    /// @brief Runtime type classifications keyed by normalized qualified name.
    std::unordered_map<std::string, TypeKind> kinds_;
};

/// @brief Access the process-wide TypeRegistry singleton.
/// @return Reference to the lazily initialized registry.
TypeRegistry &runtimeTypeRegistry();

/// @brief Seed known built-in external types into the namespace registry (legacy seed).
/// @param registry Namespace registry that receives built-in namespace/type names.
void seedRuntimeTypeCatalog(NamespaceRegistry &registry);

} // namespace il::frontends::basic
