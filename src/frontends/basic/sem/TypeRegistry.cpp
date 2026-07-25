//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/TypeRegistry.cpp
// Purpose: Implement case-insensitive runtime type classification and seed
//          catalog-only built-in names into the BASIC namespace registry.
// Key invariants:
//   * Qualified type keys are stored in lowercase.
//   * Every registered built-in type has its namespace prefix chain installed.
//   * BASIC STRING compatibility aliases remain distinct from the canonical
//     Zanna.System.String classification.
// Ownership: TypeRegistry owns its classification map; namespace seeding copies
//            names into a caller-owned NamespaceRegistry.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements BASIC runtime type classification and namespace seeding.
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/sem/TypeRegistry.hpp"
#include "frontends/basic/sem/NamespaceRegistry.hpp"
#include "il/runtime/RuntimeClassNames.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <string>
#include <vector>

namespace il::frontends::basic {

namespace {

/// @brief Register every prefix of a qualified namespace (`A`, `A.B`, `A.B.C`).
/// @param registry Namespace registry to populate.
/// @param qualifiedNs Dotted namespace path; empty is ignored.
static void ensureNamespaceChain(NamespaceRegistry &registry, const std::string &qualifiedNs) {
    if (qualifiedNs.empty())
        return;
    // Register each prefix namespace: Zanna -> Zanna.System -> Zanna.System.Text
    std::vector<std::string> segs;
    std::string cur;
    for (char c : qualifiedNs) {
        if (c == '.') {
            segs.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        segs.push_back(cur);

    std::string accum;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        if (i)
            accum.push_back('.');
        accum += segs[i];
        registry.registerNamespace(accum);
    }
}

} // namespace

/// @brief Lowercase a string to form the case-insensitive lookup key.
/// @param s Qualified or aliased type spelling to normalize.
/// @return Lowercase copy used as a registry key.
std::string TypeRegistry::toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

/// @brief Classify the runtime class catalog into the registry's type-kind map.
/// @param classes Runtime class catalog entries.
/// @details Defaults each class to BuiltinExternalType; promotes `Zanna.System.String` to
///          BuiltinExternalClass and registers the BASIC `Zanna.String` compatibility alias.
void TypeRegistry::seedRuntimeClasses(const std::vector<il::runtime::RuntimeClass> &classes) {
    for (const auto &cls : classes) {
        std::string key = toLower(cls.qname);
        kinds_[key] = TypeKind::BuiltinExternalType; // default classification for compatibility
    }
    // Prefer the newer BuiltinExternalClass tag for Zanna.System.String specifically.
    // Keep Zanna.String under BuiltinExternalType for backward compatibility.
    auto itSysStr = kinds_.find("zanna.system.string");
    if (itSysStr != kinds_.end())
        itSysStr->second = TypeKind::BuiltinExternalClass;

    // Add BASIC alias: STRING → Zanna.String (compat choice). Both names refer to
    // the same nominal runtime class surface in practice.
    kinds_[toLower("Zanna.String")] = TypeKind::BuiltinExternalType;
}

/// @brief Return the registered kind of a qualified type name.
/// @param qualifiedName Type name (the BASIC alias `string` resolves to `Zanna.String`, or
///        `Zanna.System.String` as a fallback).
/// @return The type kind, or TypeKind::Unknown if not registered.
TypeKind TypeRegistry::kindOf(std::string_view qualifiedName) const {
    std::string key = toLower(qualifiedName);
    if (key == "string") {
        // BASIC alias resolves to Zanna.String for compatibility; callers that
        // want the System-qualified name can ask for "Zanna.System.String".
        auto it = kinds_.find("zanna.string");
        if (it != kinds_.end())
            return it->second;
        // Fallback: try Zanna.System.String when present
        auto it2 = kinds_.find("zanna.system.string");
        if (it2 != kinds_.end())
            return it2->second;
    }
    auto it = kinds_.find(key);
    if (it == kinds_.end())
        return TypeKind::Unknown;
    return it->second;
}

/// @brief Return the process-wide runtime TypeRegistry singleton.
/// @return Reference to the lazily initialized type registry.
TypeRegistry &runtimeTypeRegistry() {
    static TypeRegistry reg;
    return reg;
}

/// @brief Seed a namespace registry with the canonical built-in runtime types.
/// @param registry Namespace registry to populate.
/// @details Registers a minimal catalog (Object, String, StringBuilder, File, List) into their
///          namespaces (creating the namespace chain first), plus the `Zanna`/`Zanna.Console`
///          namespaces so `USING` of builtin procedure groups resolves.
void seedRuntimeTypeCatalog(NamespaceRegistry &registry) {
    // Seed a minimal catalog of built-in runtime types. Canonical names live
    // under Zanna.* and are defined by the runtime class catalog
    // (src/il/runtime/classes/RuntimeClasses.inc).
    static const BuiltinExternalType kTypes[] = {
        // Canonical forms (from RuntimeClassNames.hpp constants)
        {il::runtime::RTCLASS_OBJECT.data(), ExternalTypeCategory::Class, "zanna:Object"},
        {il::runtime::RTCLASS_STRING.data(), ExternalTypeCategory::Class, "zanna:String"},
        {il::runtime::RTCLASS_STRINGBUILDER.data(),
         ExternalTypeCategory::Class,
         "zanna.text:StringBuilder"},
        {il::runtime::RTCLASS_FILE.data(), ExternalTypeCategory::Class, "zanna.io:File"},
        {il::runtime::RTCLASS_LIST.data(), ExternalTypeCategory::Class, "zanna.coll:List"},
    };

    for (const auto &entry : kTypes) {
        // Split qualified name into namespace and leaf type name.
        std::string qn(entry.qualifiedName);
        std::size_t lastDot = qn.rfind('.');
        if (lastDot == std::string::npos)
            continue;
        std::string ns = qn.substr(0, lastDot);
        std::string leaf = qn.substr(lastDot + 1);

        // Ensure namespace chain exists.
        ensureNamespaceChain(registry, ns);

        // Register the type into its namespace.
        switch (entry.category) {
            case ExternalTypeCategory::Class:
                registry.registerClass(ns, leaf);
                break;
            case ExternalTypeCategory::Interface:
                registry.registerInterface(ns, leaf);
                break;
        }
    }

    // Also register namespaces for builtin extern procedure groups so USING works:
    // e.g., USING Zanna.Console -> enables unqualified PrintI64 resolution.
    ensureNamespaceChain(registry, std::string("Zanna"));
    ensureNamespaceChain(registry, std::string("Zanna.Console"));
}

} // namespace il::frontends::basic
