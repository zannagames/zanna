//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/lsp-common/ICompilerBridge.cpp
// Purpose: Default implementations for runtime query methods on ICompilerBridge.
// Key invariants:
//   - Runtime queries use the shared RuntimeClasses singleton registry
//   - These defaults work for any language server (Zia, BASIC, etc.)
// Ownership/Lifetime:
//   - All returned data is fully owned
// Links: tools/lsp-common/ICompilerBridge.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements conservative default IDE features and runtime-catalog queries.

#include "tools/lsp-common/ICompilerBridge.hpp"

#include "il/runtime/classes/RuntimeClasses.hpp"

#include <algorithm>
#include <cctype>

namespace zanna::server {

/// @brief Return an ASCII-lowercased copy of @p s (used for case-insensitive search).
/// @param s Text to normalize.
/// @return Lowercase copy.
static std::string toLowerStr(const std::string &s) {
    std::string lower;
    lower.reserve(s.size());
    for (char c : s)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

/// @brief List every registered runtime class.
/// @return Owned summaries in catalog order.
std::vector<RuntimeClassSummary> ICompilerBridge::runtimeClasses() {
    const auto &catalog = il::runtime::runtimeClassCatalog();
    std::vector<RuntimeClassSummary> result;
    result.reserve(catalog.size());
    for (const auto &cls : catalog) {
        result.push_back({cls.qname,
                          static_cast<int>(cls.properties.size()),
                          static_cast<int>(cls.methods.size()),
                          cls.summary ? cls.summary : "",
                          cls.details ? cls.details : ""});
    }
    return result;
}

/// @brief Default no-op notification for an opened or changed document.
/// @param path Document path.
/// @param source Current full source text.
void ICompilerBridge::updateDocument(const std::string & /*path*/, const std::string & /*source*/) {
}

/// @brief Default no-op notification for a closed document.
/// @param path Document path.
void ICompilerBridge::removeDocument(const std::string & /*path*/) {}

/// @brief Report that definition lookup is unsupported by default.
/// @return false.
bool ICompilerBridge::supportsDefinition() const {
    return false;
}

/// @brief Report that reference lookup is unsupported by default.
/// @return false.
bool ICompilerBridge::supportsReferences() const {
    return false;
}

/// @brief Report that semantic rename is unsupported by default.
/// @return false.
bool ICompilerBridge::supportsRename() const {
    return false;
}

/// @brief Report that signature help is unsupported by default.
/// @return false.
bool ICompilerBridge::supportsSignatureHelp() const {
    return false;
}

/// @brief Report that workspace-symbol search is unsupported by default.
/// @return false.
bool ICompilerBridge::supportsWorkspaceSymbols() const {
    return false;
}

/// @brief Report that semantic-token generation is unsupported by default.
/// @return false.
bool ICompilerBridge::supportsSemanticTokens() const {
    return false;
}

/// @brief Return no definition from the conservative default implementation.
/// @param source Current source text.
/// @param line Zero-based query line.
/// @param col Zero-based query column.
/// @param path Document path.
/// @return `std::nullopt`.
std::optional<LocationInfo> ICompilerBridge::definition(const std::string & /*source*/,
                                                        int /*line*/,
                                                        int /*col*/,
                                                        const std::string & /*path*/) {
    return std::nullopt;
}

/// @brief Return no references from the conservative default implementation.
/// @param source Current source text.
/// @param line Zero-based query line.
/// @param col Zero-based query column.
/// @param path Document path.
/// @return Empty location list.
std::vector<LocationInfo> ICompilerBridge::references(const std::string & /*source*/,
                                                      int /*line*/,
                                                      int /*col*/,
                                                      const std::string & /*path*/) {
    return {};
}

/// @brief Return an empty rename result from the default implementation.
/// @param source Current source text.
/// @param line Zero-based query line.
/// @param col Zero-based query column.
/// @param path Document path.
/// @param newName Requested replacement identifier.
/// @return Empty rename result.
RenameResult ICompilerBridge::rename(const std::string & /*source*/,
                                     int /*line*/,
                                     int /*col*/,
                                     const std::string & /*path*/,
                                     const std::string & /*newName*/) {
    return {};
}

/// @brief Return empty signature help from the default implementation.
/// @param source Current source text.
/// @param line Zero-based query line.
/// @param col Zero-based query column.
/// @param path Document path.
/// @return Empty signature-help value.
SignatureHelpInfo ICompilerBridge::signatureHelp(const std::string & /*source*/,
                                                 int /*line*/,
                                                 int /*col*/,
                                                 const std::string & /*path*/) {
    return {};
}

/// @brief Return no workspace symbols from the default implementation.
/// @param query Search text.
/// @return Empty symbol list.
std::vector<SymbolInfo> ICompilerBridge::workspaceSymbols(const std::string & /*query*/) {
    return {};
}

/// @brief Return no semantic tokens from the default implementation.
/// @param source Current source text.
/// @param path Document path.
/// @return Empty token list.
std::vector<SemanticTokenInfo> ICompilerBridge::semanticTokens(const std::string & /*source*/,
                                                               const std::string & /*path*/) {
    return {};
}

/// @brief List properties and methods for one runtime class.
/// @param className Fully qualified runtime class name.
/// @return Owned members in property-then-method catalog order, or empty if unknown.
std::vector<RuntimeMemberInfo> ICompilerBridge::runtimeMembers(const std::string &className) {
    const auto *cls = il::runtime::findRuntimeClassByQName(className);
    if (!cls)
        return {};

    std::vector<RuntimeMemberInfo> result;
    for (const auto &prop : cls->properties)
        result.push_back({prop.name, "property", prop.type ? prop.type : ""});
    for (const auto &method : cls->methods)
        result.push_back({method.name, "method", method.signature ? method.signature : ""});
    return result;
}

/// @brief Search runtime class and member names case-insensitively.
/// @param keyword Non-empty substring query.
/// @return Matching classes, methods, and properties in catalog order.
std::vector<RuntimeMemberInfo> ICompilerBridge::runtimeSearch(const std::string &keyword) {
    std::string lowerKw = toLowerStr(keyword);
    if (lowerKw.empty())
        return {};
    const auto &catalog = il::runtime::runtimeClassCatalog();
    std::vector<RuntimeMemberInfo> result;

    for (const auto &cls : catalog) {
        std::string lowerName = toLowerStr(cls.qname);
        if (lowerName.find(lowerKw) != std::string::npos)
            result.push_back({cls.qname, "class", ""});

        for (const auto &method : cls.methods) {
            std::string lowerMethod = toLowerStr(method.name);
            if (lowerMethod.find(lowerKw) != std::string::npos) {
                result.push_back({std::string(cls.qname) + "." + method.name,
                                  "method",
                                  method.signature ? method.signature : ""});
            }
        }

        for (const auto &prop : cls.properties) {
            std::string lowerProp = toLowerStr(prop.name);
            if (lowerProp.find(lowerKw) != std::string::npos) {
                result.push_back({std::string(cls.qname) + "." + prop.name,
                                  "property",
                                  prop.type ? prop.type : ""});
            }
        }
    }
    return result;
}

} // namespace zanna::server
