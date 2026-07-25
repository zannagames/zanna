//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/TypeResolver.hpp
// Purpose: Declare namespace- and USING-aware BASIC type-name resolution with
//          deterministic ambiguity reporting.
// Key invariants:
//   * Qualified names do not fall back to unrelated USING imports.
//   * Simple-name resolution searches enclosing namespaces before imports.
//   * Ambiguous import matches are reported in stable case-insensitive order.
// Ownership: TypeResolver borrows its NamespaceRegistry and UsingContext; the
//            caller guarantees both outlive the resolver.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares compile-time BASIC type-name resolution.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/sem/NamespaceRegistry.hpp"
#include "frontends/basic/sem/UsingContext.hpp"
#include <string>
#include <vector>

namespace il::frontends::basic {

/// @brief Resolves type names using namespace registry and using context.
///
/// @details Implements compile-time type resolution with the following precedence:
///          - Qualified names (containing '.') bypass USING imports
///          - Simple names walk up current namespace chain, then try USING imports
///          - Ambiguity is detected and reported with sorted contender list
///
/// @invariant All lookups are case-insensitive.
/// @invariant Ambiguity produces deterministic (sorted) contender lists.
class TypeResolver {
  public:
    /// @brief Type kind discriminator.
    enum class Kind {
        Unknown,   ///< Type not found or ambiguous.
        Class,     ///< Resolved to a class type.
        Interface, ///< Resolved to an interface type.
    };

    /// @brief Result of type name resolution.
    struct Result {
        /// True if exactly one type was found; false if none or ambiguous.
        bool found{false};

        /// Fully-qualified canonical name if found; empty otherwise.
        std::string qname;

        /// Type kind if found; Unknown otherwise.
        Kind kind{Kind::Unknown};

        /// If ambiguous (found=false && !contenders.empty()), list of matching FQ names.
        /// Sorted case-insensitively for deterministic diagnostics.
        std::vector<std::string> contenders;
    };

    /// @brief Construct a resolver with registry and using context.
    /// @param ns NamespaceRegistry containing declared types.
    /// @param uc UsingContext containing file-scoped imports.
    TypeResolver(const NamespaceRegistry &ns, const UsingContext &uc);

    /// @brief Resolve a type name in the given namespace context.
    /// @details Implements the resolution algorithm:
    ///          1. If name contains '.':
    ///             - If first segment is an alias, expand and check existence
    ///             - Else treat as fully-qualified and check existence
    ///          2. If simple name:
    ///             - Walk up current namespace chain (A.B.C → A.B → A → global)
    ///             - Try USING imports in declaration order
    ///             - Return found/ambiguous/not-found
    /// @param name Type name to resolve (simple or qualified).
    /// @param currentNsChain Current namespace path segments (e.g., {"A", "B", "C"}).
    /// @return Result with found flag, qualified name, kind, and contenders if ambiguous.
    [[nodiscard]] Result resolve(std::string_view name,
                                 const std::vector<std::string> &currentNsChain) const;

  private:
    /// @brief Convert a string to lowercase for case-insensitive comparison.
    /// @param str Spelling to normalize.
    /// @return Lowercase copy of @p str.
    [[nodiscard]] static std::string toLower(const std::string &str);

    /// @brief Join namespace segments with '.' separator.
    /// @param segments Ordered path segments.
    /// @return Dotted path, or an empty string for no segments.
    [[nodiscard]] static std::string joinPath(const std::vector<std::string> &segments);

    /// @brief Split a dotted name into segments.
    /// @param path Qualified path to tokenize.
    /// @return Ordered non-empty path segments.
    [[nodiscard]] static std::vector<std::string> splitPath(std::string_view path);

    /// @brief Try to resolve name in a specific namespace.
    /// @param ns Namespace prefix, or an empty string for global scope.
    /// @param typeName Simple type name to append to @p ns.
    /// @return Fully-qualified name if found; empty otherwise.
    [[nodiscard]] std::string tryResolveInNamespace(const std::string &ns,
                                                    std::string_view typeName) const;

    /// @brief Convert NamespaceRegistry::TypeKind to TypeResolver::Kind.
    /// @param nsk Registry type classification.
    /// @return Equivalent public resolver classification.
    [[nodiscard]] static Kind convertKind(NamespaceRegistry::TypeKind nsk);

    /// @brief Borrowed namespace/type registry.
    const NamespaceRegistry &registry_;
    /// @brief Borrowed file-scoped import context.
    const UsingContext &using_;
};

} // namespace il::frontends::basic
