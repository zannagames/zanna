//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/TypeResolver.cpp
// Purpose: Implements compile-time type resolution with namespace/using context.
// Key invariants:
//   * Qualified names bypass ordinary USING imports, except that an alias in
//     the first segment is expanded.
//   * Simple names use precedence: current namespace chain, then USING imports.
//   * Ambiguity produces sorted contender lists for stable diagnostics.
// Ownership: TypeResolver borrows NamespaceRegistry and UsingContext references;
//            both must outlive it.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements namespace- and USING-aware BASIC type-name resolution.
/// @details Resolves qualified, alias-qualified, and simple names while
///          preserving deterministic ambiguity diagnostics.
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/sem/TypeResolver.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace il::frontends::basic {

/// @brief Construct a resolver over a namespace registry and USING import context.
/// @param ns Registry containing namespace and declared-type metadata.
/// @param uc File-scoped USING imports and aliases.
/// @note Neither argument is owned; both must outlive the resolver.
TypeResolver::TypeResolver(const NamespaceRegistry &ns, const UsingContext &uc)
    : registry_(ns), using_(uc) {}

/// @brief Lowercase a string (used for case-insensitive ambiguity ordering).
/// @param str Spelling to normalize.
/// @return Lowercase copy of @p str.
std::string TypeResolver::toLower(const std::string &str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

/// @brief Join path segments into a dotted qualified name (`A.B.C`).
/// @param segments Ordered namespace or qualified-name segments.
/// @return Dot-separated path, or an empty string when @p segments is empty.
std::string TypeResolver::joinPath(const std::vector<std::string> &segments) {
    if (segments.empty())
        return "";

    std::string result = segments[0];
    for (size_t i = 1; i < segments.size(); ++i) {
        result += ".";
        result += segments[i];
    }
    return result;
}

/// @brief Split a dotted path into its non-empty segments.
/// @details Consecutive, leading, and trailing dots do not produce empty
///          segments.
/// @param path Dotted path to tokenize.
/// @return Ordered vector of non-empty path segments.
std::vector<std::string> TypeResolver::splitPath(std::string_view path) {
    std::vector<std::string> segments;
    std::string current;

    for (char c : path) {
        if (c == '.') {
            if (!current.empty()) {
                segments.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }

    if (!current.empty())
        segments.push_back(current);

    return segments;
}

/// @brief Try to resolve a simple type name within a single namespace.
/// @param ns Namespace to qualify with (empty means the global namespace).
/// @param typeName Simple type name.
/// @return The qualified candidate name if it exists in the registry, otherwise "".
std::string TypeResolver::tryResolveInNamespace(const std::string &ns,
                                                std::string_view typeName) const {
    std::string candidate = ns.empty() ? std::string(typeName) : (ns + "." + std::string(typeName));

    if (registry_.typeExists(candidate)) {
        // Return the canonical spelling from registry.
        const auto kind = registry_.getTypeKind(candidate);
        if (kind == NamespaceRegistry::TypeKind::None)
            return "";
        // We need to get the canonical name; for now return the candidate
        // since registry doesn't expose a "get canonical name" method.
        // The registry stores canonical spellings internally.
        return candidate;
    }

    return "";
}

/// @brief Map a NamespaceRegistry type kind to the resolver's public Kind enum.
/// @param nsk Registry classification to translate.
/// @return Corresponding resolver classification, or `Kind::Unknown`.
TypeResolver::Kind TypeResolver::convertKind(NamespaceRegistry::TypeKind nsk) {
    switch (nsk) {
        case NamespaceRegistry::TypeKind::Class:
            return Kind::Class;
        case NamespaceRegistry::TypeKind::Interface:
            return Kind::Interface;
        case NamespaceRegistry::TypeKind::None:
            return Kind::Unknown;
    }
    return Kind::Unknown;
}

/// @brief Resolve a type name to a qualified form recognized by the registry.
/// @param name The type name as written (qualified or simple, possibly alias-prefixed).
/// @param currentNsChain The enclosing namespace chain, outermost first.
/// @return A Result with `found`, resolved `qname`, and `kind` set, or (for
///         simple names) a sorted `contenders` list when the name is ambiguous.
/// @details Qualified names bypass USING imports: an alias first segment is expanded, otherwise
///          the name is treated as fully qualified. Simple names are resolved by walking the
///          namespace chain from innermost to global first, then USING imports in declaration
///          order; multiple USING matches are ambiguous (contenders sorted case-insensitively).
TypeResolver::Result TypeResolver::resolve(std::string_view name,
                                           const std::vector<std::string> &currentNsChain) const {
    Result result;

    // Check if name contains '.'.
    bool isQualified = name.find('.') != std::string_view::npos;

    if (isQualified) {
        // Qualified name handling.
        auto segments = splitPath(name);
        if (segments.empty()) {
            // Malformed name.
            return result;
        }

        std::string firstSegment = segments[0];

        // Check if first segment is an alias.
        if (using_.hasAlias(firstSegment)) {
            // Expand alias.
            std::string aliasedNs = using_.resolveAlias(firstSegment);

            // Build expanded path: aliasedNs + tail segments.
            std::vector<std::string> expandedSegments;
            auto aliasSegs = splitPath(aliasedNs);
            expandedSegments.insert(expandedSegments.end(), aliasSegs.begin(), aliasSegs.end());
            expandedSegments.insert(expandedSegments.end(), segments.begin() + 1, segments.end());

            std::string expandedPath = joinPath(expandedSegments);

            if (registry_.typeExists(expandedPath)) {
                result.found = true;
                result.qname = expandedPath;
                result.kind = convertKind(registry_.getTypeKind(expandedPath));
                return result;
            }

            // Not found after alias expansion.
            return result;
        }

        // Treat as fully-qualified name.
        std::string nameStr(name);
        if (registry_.typeExists(nameStr)) {
            result.found = true;
            result.qname = nameStr;
            result.kind = convertKind(registry_.getTypeKind(nameStr));
            return result;
        }

        // Not found.
        return result;
    }

    // Simple name: walk up namespace chain.
    std::vector<std::string> candidates;

    // Try current namespace chain walk-up: A.B.C.T → A.B.T → A.T → T.
    for (int depth = static_cast<int>(currentNsChain.size()); depth >= 0; --depth) {
        std::vector<std::string> nsSegments(currentNsChain.begin(), currentNsChain.begin() + depth);
        std::string ns = joinPath(nsSegments);

        std::string resolved = tryResolveInNamespace(ns, name);
        if (!resolved.empty()) {
            result.found = true;
            result.qname = resolved;
            result.kind = convertKind(registry_.getTypeKind(resolved));
            return result;
        }
    }

    // Try USING imports in declaration order.
    for (const auto &import : using_.imports()) {
        std::string resolved = tryResolveInNamespace(import.ns, name);
        if (!resolved.empty()) {
            candidates.push_back(resolved);
        }
    }

    // Check candidate count.
    if (candidates.empty()) {
        // Not found.
        return result;
    }

    if (candidates.size() == 1) {
        // Found unique match.
        result.found = true;
        result.qname = candidates[0];
        result.kind = convertKind(registry_.getTypeKind(candidates[0]));
        return result;
    }

    // Ambiguous: sort candidates case-insensitively for stable diagnostics.
    /// @brief Orders ambiguous type candidates case-insensitively.
    /// @param a Left qualified name.
    /// @param b Right qualified name.
    /// @return `true` when the lowercase form of `a` precedes `b`.
    std::sort(candidates.begin(), candidates.end(), [](const std::string &a, const std::string &b) {
        return toLower(a) < toLower(b);
    });

    result.found = false;
    result.contenders = std::move(candidates);
    return result;
}

} // namespace il::frontends::basic
