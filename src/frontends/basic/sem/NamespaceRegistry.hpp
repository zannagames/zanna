//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/NamespaceRegistry.hpp
// Purpose: Declare the compilation-scoped registry of BASIC namespaces,
//          classes, and interfaces.
// Key invariants:
//   * Lookup keys are normalized case-insensitively.
//   * Namespace metadata retains the first registered spelling for diagnostics.
//   * Re-registering a namespace merges declarations without replacing its
//     canonical spelling.
// Ownership: The registry owns all stored names and sets. Runtime descriptors
//            and class catalogs passed to seed methods remain caller-owned.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the case-insensitive BASIC namespace and type registry.
/// @details The registry combines user declarations with namespace prefixes
///          derived from runtime catalogs for semantic name resolution.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace il::runtime {
struct RuntimeDescriptor; // fwd decl to avoid including runtime headers here
struct RuntimeClass;      // fwd decl for class catalog seeding
} // namespace il::runtime

namespace il::frontends::basic {

/// @brief Records declared namespaces and their types with case-insensitive lookups.
///
/// @details This registry maintains a mapping of namespace paths to their declared
///          types (classes and interfaces). All lookups are case-insensitive, but
///          the first-seen spelling is preserved for use in diagnostic messages.
///          Repeated declarations of the same namespace are merged into a single
///          logical namespace.
///
/// @invariant All namespace and type names use case-insensitive comparison.
/// @invariant Canonical (first-seen) spellings are preserved for error messages.
class NamespaceRegistry {
  public:
    /// @brief Type discriminator for registered types.
    enum class TypeKind {
        None,      ///< Type not found or namespace-only.
        Class,     ///< Registered class type.
        Interface, ///< Registered interface type.
    };

    /// @brief Information about a registered namespace.
    struct NamespaceInfo {
        /// Fully-qualified namespace path in canonical casing, e.g., "A.B.C".
        std::string full;

        /// Fully-qualified class names declared in this namespace, e.g., "A.B.C.MyClass".
        std::unordered_set<std::string> classes;

        /// Fully-qualified interface names declared in this namespace, e.g., "A.B.C.IFoo".
        std::unordered_set<std::string> interfaces;
    };

    /// @brief Register a namespace for later type declarations.
    /// @details If the namespace was already registered, this is a no-op but preserves
    ///          the first-seen canonical spelling.
    /// @param full Fully-qualified namespace path, e.g., "A.B.C".
    void registerNamespace(const std::string &full);

    /// @brief Register a class within a namespace.
    /// @details Creates the namespace if it doesn't exist. Stores the fully-qualified
    ///          class name as "nsFull.className" in canonical casing.
    /// @param nsFull Fully-qualified namespace path, e.g., "A.B".
    /// @param className Simple class name, e.g., "MyClass".
    void registerClass(const std::string &nsFull, const std::string &className);

    /// @brief Register an interface within a namespace.
    /// @details Creates the namespace if it doesn't exist. Stores the fully-qualified
    ///          interface name as "nsFull.ifaceName" in canonical casing.
    /// @param nsFull Fully-qualified namespace path, e.g., "A.B".
    /// @param ifaceName Simple interface name, e.g., "IFoo".
    void registerInterface(const std::string &nsFull, const std::string &ifaceName);

    /// @brief Check if a namespace exists (case-insensitive).
    /// @param full Fully-qualified namespace path to test.
    /// @return True if the namespace was registered.
    [[nodiscard]] bool namespaceExists(const std::string &full) const;

    /// @brief Check if a type (class or interface) exists (case-insensitive).
    /// @param qualified Fully-qualified type name, e.g., "A.B.MyClass".
    /// @return True if the type was registered as a class or interface.
    [[nodiscard]] bool typeExists(const std::string &qualified) const;

    /// @brief Get the kind of a registered type (case-insensitive).
    /// @param qualified Fully-qualified type name, e.g., "A.B.MyClass".
    /// @return TypeKind::Class, TypeKind::Interface, or TypeKind::None.
    [[nodiscard]] TypeKind getTypeKind(const std::string &qualified) const;

    /// @brief Retrieve namespace information (case-insensitive).
    /// @param full Fully-qualified namespace path to query.
    /// @return Pointer to NamespaceInfo if found; nullptr otherwise.
    [[nodiscard]] const NamespaceInfo *info(const std::string &full) const;

    /// @brief Seed known namespaces from runtime built-in descriptors.
    /// @details For each runtime descriptor with a dotted name (e.g., "Zanna.Terminal.PrintI64"),
    ///          insert all namespace prefixes into the registry: "Zanna", "Zanna.Console".
    ///          Names are handled case-insensitively; canonical casing from descriptors is
    ///          preserved.
    /// @param descs Runtime descriptor list (typically il::runtime::runtimeRegistry()).
    void seedFromRuntimeBuiltins(const std::vector<il::runtime::RuntimeDescriptor> &descs);

    /// @brief Seed namespaces and dotted prefixes from runtime class catalog.
    /// @details For each class qualified name (e.g., "Zanna.String"), registers
    ///          all dotted prefixes as namespaces: "Zanna", "Zanna.String".
    ///          Idempotent; preserves first-seen casing.
    /// @param classes Runtime class catalog (runtimeClassCatalog()).
    void seedRuntimeClassNamespaces(const std::vector<il::runtime::RuntimeClass> &classes);

  private:
    /// @brief Convert a string to lowercase for case-insensitive comparison.
    /// @param str Input string.
    /// @return Lowercase copy of the input.
    [[nodiscard]] static std::string toLower(const std::string &str);

    /// @brief Map from lowercase namespace path to namespace information.
    std::unordered_map<std::string, NamespaceInfo> namespaces_;

    /// @brief Map from lowercase fully-qualified type name to type kind.
    std::unordered_map<std::string, TypeKind> types_;
};

} // namespace il::frontends::basic
