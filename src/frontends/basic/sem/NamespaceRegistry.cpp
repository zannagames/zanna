//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/NamespaceRegistry.cpp
// Purpose: Implement namespace and type registration with case-insensitive
//          lookup and runtime-catalog namespace seeding.
// Key invariants:
//   * All internal keys use lowercase for case-insensitive comparison.
//   * First-seen spellings are preserved in NamespaceInfo::full.
//   * Repeated namespace registrations are merged.
// Ownership: NamespaceRegistry owns its namespace/type maps; catalog inputs are
//            borrowed only for the duration of each seeding call.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Case-insensitive BASIC namespace and declared-type registry.
/// @details Implements explicit registration and derives namespace prefixes
///          from runtime function and class catalogs.
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/sem/NamespaceRegistry.hpp"
#include <algorithm>
#include <cctype>
#include <string_view>

#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

namespace il::frontends::basic {

/// @brief Lowercase a string to form the case-insensitive lookup key.
/// @details Casts each byte to unsigned char before calling std::tolower so
///          non-ASCII byte values never invoke undefined behavior.
/// @param str Source spelling to normalize.
/// @return A lowercase copy suitable for indexing the registry maps.
std::string NamespaceRegistry::toLower(const std::string &str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

/// @brief Register a namespace by its full dotted name.
/// @param full The namespace spelling (the first-seen casing is preserved as canonical).
/// @details Idempotent: re-registering an existing namespace is a no-op.
void NamespaceRegistry::registerNamespace(const std::string &full) {
    std::string key = toLower(full);
    auto it = namespaces_.find(key);
    if (it == namespaces_.end()) {
        // First time seeing this namespace; store canonical spelling.
        NamespaceInfo info;
        info.full = full;
        namespaces_[key] = std::move(info);
    }
    // If already exists, preserve first-seen spelling (no-op).
}

/// @brief Register a class within a namespace.
/// @param nsFull Owning namespace (registered if not already present; "" = global).
/// @param className Simple class name.
/// @details Stores the fully-qualified name (using the canonical namespace spelling) on the
///          namespace and records its kind as Class for type lookups.
void NamespaceRegistry::registerClass(const std::string &nsFull, const std::string &className) {
    // Ensure namespace exists.
    registerNamespace(nsFull);

    std::string key = toLower(nsFull);
    auto it = namespaces_.find(key);
    // Should always succeed since we just registered it.
    if (it == namespaces_.end())
        return;

    // Build fully-qualified class name using canonical namespace spelling.
    // Handle global namespace (empty string) specially.
    std::string fqClassName;
    if (it->second.full.empty())
        fqClassName = className;
    else
        fqClassName = it->second.full + "." + className;
    it->second.classes.insert(fqClassName);

    // Record type kind for lookups.
    std::string typeKey = toLower(fqClassName);
    types_[typeKey] = TypeKind::Class;
}

/// @brief Register an interface within a namespace.
/// @param nsFull Owning namespace (registered if not already present; "" = global).
/// @param ifaceName Simple interface name.
/// @details Stores the fully-qualified name on the namespace and records its kind as Interface.
void NamespaceRegistry::registerInterface(const std::string &nsFull, const std::string &ifaceName) {
    // Ensure namespace exists.
    registerNamespace(nsFull);

    std::string key = toLower(nsFull);
    auto it = namespaces_.find(key);
    // Should always succeed since we just registered it.
    if (it == namespaces_.end())
        return;

    // Build fully-qualified interface name using canonical namespace spelling.
    // Handle global namespace (empty string) specially.
    std::string fqIfaceName;
    if (it->second.full.empty())
        fqIfaceName = ifaceName;
    else
        fqIfaceName = it->second.full + "." + ifaceName;
    it->second.interfaces.insert(fqIfaceName);

    // Record type kind for lookups.
    std::string typeKey = toLower(fqIfaceName);
    types_[typeKey] = TypeKind::Interface;
}

/// @brief Test whether a namespace is registered (case-insensitive).
/// @param full Fully qualified namespace spelling to query.
/// @return True when the normalized namespace key is present.
bool NamespaceRegistry::namespaceExists(const std::string &full) const {
    std::string key = toLower(full);
    return namespaces_.find(key) != namespaces_.end();
}

/// @brief Test whether a fully-qualified type is registered (case-insensitive).
/// @param qualified Fully qualified class or interface name to query.
/// @return True when the normalized type key is present.
bool NamespaceRegistry::typeExists(const std::string &qualified) const {
    std::string key = toLower(qualified);
    return types_.find(key) != types_.end();
}

/// @brief Return the kind (Class/Interface) of a qualified type, or None if unknown.
/// @param qualified Fully qualified class or interface name to query.
/// @return The registered type kind, or TypeKind::None when absent.
NamespaceRegistry::TypeKind NamespaceRegistry::getTypeKind(const std::string &qualified) const {
    std::string key = toLower(qualified);
    auto it = types_.find(key);
    if (it == types_.end())
        return TypeKind::None;
    return it->second;
}

/// @brief Return the stored info (canonical spelling, classes, interfaces) for a namespace.
/// @param full Fully qualified namespace spelling to query.
/// @return Pointer to the NamespaceInfo, or nullptr if the namespace is unknown.
const NamespaceRegistry::NamespaceInfo *NamespaceRegistry::info(const std::string &full) const {
    std::string key = toLower(full);
    auto it = namespaces_.find(key);
    if (it == namespaces_.end())
        return nullptr;
    return &it->second;
}

/// @brief Pre-register every namespace prefix implied by runtime builtin descriptor names.
/// @param descs Runtime descriptors (e.g. `Zanna.Terminal.PrintI64`).
/// @details For each dotted name, registers each prefix up to but excluding the final segment
///          (the function/type), so `USING Zanna.Terminal` resolves against builtins.
void NamespaceRegistry::seedFromRuntimeBuiltins(
    const std::vector<il::runtime::RuntimeDescriptor> &descs) {
    for (const auto &d : descs) {
        std::string_view name = d.name;
        // Only consider dotted names: treat as Namespace.Type or Namespace.Member
        if (name.find('.') == std::string_view::npos)
            continue;

        // Generate all namespace prefixes up to (but not including) the last segment.
        // Example: "Zanna.Terminal.PrintI64" → prefixes: "Zanna", "Zanna.Console".
        std::string current;
        current.reserve(name.size());
        std::size_t start = 0;
        while (true) {
            std::size_t dot = name.find('.', start);
            if (dot == std::string_view::npos)
                break; // stop before final segment (function/type)
            if (!current.empty())
                current.push_back('.');
            current.append(name.substr(start, dot - start));
            // Register this namespace prefix (idempotent; preserves first-seen casing).
            registerNamespace(current);
            start = dot + 1;
        }
    }
}

/// @brief Pre-register namespace prefixes (including full qnames) for runtime classes.
/// @param classes Runtime class catalog entries.
/// @details Unlike seedFromRuntimeBuiltins(), this also registers the full class qname as a
///          namespace so `USING Zanna.String` does not error even though `String` is a class.
void NamespaceRegistry::seedRuntimeClassNamespaces(
    const std::vector<il::runtime::RuntimeClass> &classes) {
    for (const auto &cls : classes) {
        std::string_view name = cls.qname;
        if (name.empty())
            continue;
        // Build prefixes including the full class qname as a namespace entry to
        // avoid USING errors when users write USING Zanna.String.
        std::string current;
        current.reserve(name.size());
        std::size_t start = 0;
        while (start < name.size()) {
            std::size_t dot = name.find('.', start);
            if (dot == std::string_view::npos)
                dot = name.size();
            if (!current.empty())
                current.push_back('.');
            current.append(name.substr(start, dot - start));
            registerNamespace(current);
            if (dot == name.size())
                break;
            start = dot + 1;
        }
    }
}

} // namespace il::frontends::basic
