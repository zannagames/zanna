//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Sema_Decl.cpp
/// @brief Declaration analysis for the Zia semantic analyzer.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Sema.hpp"
#include "il/runtime/RuntimeNameMap.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <algorithm>
#include <map>
#include <string_view>
#include <unordered_set>

namespace il::frontends::zia {

namespace {

/// @brief True if @p expr is an integer literal whose value fits 0–255,
///        i.e. can be narrowed to a Byte without loss.
/// @param expr Candidate initializer expression.
/// @return True only for an in-range integer literal.
bool integerLiteralFitsByte(Expr *expr) {
    auto *lit = dynamic_cast<IntLiteralExpr *>(expr);
    return lit && lit->value >= 0 && lit->value <= 255;
}

/// @brief Refine an integer-literal initializer's type to match a declared
///        `Byte` when the literal fits, so `var b: Byte = 5` types as Byte
///        rather than Integer. Returns @p initType unchanged otherwise.
/// @param declaredType Explicit destination type.
/// @param initType Initially inferred initializer type.
/// @param initializer Initializer expression used to verify literal range.
/// @return Byte for a compatible contextual literal; otherwise @p initType.
TypeRef contextualizeIntegerLiteralForDeclaredType(TypeRef declaredType,
                                                   TypeRef initType,
                                                   Expr *initializer) {
    if (declaredType && initType && declaredType->kind == TypeKindSem::Byte &&
        initType->kind == TypeKindSem::Integer && integerLiteralFitsByte(initializer)) {
        return types::byte();
    }
    return initType;
}

/// @brief True if @p type may not be used as a stored value/field type
///        (Void, Never, or Module).
/// @param type Semantic type to classify.
/// @return True when the type cannot have a stored runtime value.
bool isForbiddenValueType(TypeRef type) {
    return type && (type->kind == TypeKindSem::Void || type->kind == TypeKindSem::Never ||
                    type->kind == TypeKindSem::Module);
}

/// @brief Render a forbidden value type for diagnostics.
/// @param type Semantic type, which may be null.
/// @return Semantic spelling, or `unknown` for a null type.
std::string forbiddenValueTypeName(TypeRef type) {
    return type ? type->toString() : "unknown";
}

/// @brief True for compatibility alias namespaces that should not be imported
///        as child symbols during a broad namespace bind.
/// @details Explicit binds to these aliases remain valid. Suppressing the broad
///          child keeps compatibility namespaces from competing with canonical classes.
/// @param ns Parent namespace of a broad bind.
/// @param child Immediate child namespace being considered.
/// @return True when the qualified child is a compatibility-only alias.
bool suppressBroadRuntimeAliasNamespaceImport(std::string_view ns, std::string_view child) {
    const std::string qualified = std::string(ns) + "." + std::string(child);
    static constexpr std::string_view suppressed[] = {
        "Zanna.Graphics.Lighting2D",
        "Zanna.Graphics3D.Sound3D",
        "Zanna.Graphics3D.Assets3D",
        "Zanna.Text.NumberFormat",
        "Zanna.GUI.Clipboard",
        "Zanna.Workspace.Watcher",
        "Zanna.System.Process.Handle",
        "Zanna.Zia.SemanticJob.Handle",
        "Zanna.Zia.ProjectIndex.Handle",
        "Zanna.Game.UI.Label",
        "Zanna.Game.UI.TextInput",
        "Zanna.Game.UI.Slider",
        "Zanna.Game.UI.Dropdown",
        "Zanna.Game.UI.Tooltip",
        "Zanna.Game3D.Environment",
        "Zanna.Game3D.Diagnostics",
    };

    for (std::string_view name : suppressed) {
        if (name == qualified)
            return true;
    }
    return false;
}

} // namespace

//=============================================================================
// Declaration Analysis
//=============================================================================

/// @brief Analyze a bind declaration (file import or namespace bind).
/// @details Routes to analyzeNamespaceBind() for namespace paths, or registers
///          the file module name as a Module symbol for qualified access.
/// @param decl The bind declaration to analyze.
void Sema::analyzeBind(BindDecl &decl) {
    if (decl.path.empty()) {
        error(decl.loc, "Bind path cannot be empty");
        return;
    }

    // Handle namespace binds (e.g., "Zanna.Terminal")
    if (decl.isNamespaceBind) {
        analyzeNamespaceBind(decl);
        return;
    }

    std::string bindTargetKey =
        decl.resolvedFileId != 0 ? std::to_string(decl.resolvedFileId) : decl.path;
    std::string bindKey = std::to_string(decl.loc.file_id) + ":" + bindTargetKey;

    // W012: Duplicate import within the same importing file.
    if (binds_.count(bindKey)) {
        warn(
            WarningCode::W012_DuplicateImport, decl.loc, "Duplicate import of '" + decl.path + "'");
    }

    // Handle file binds (existing logic)
    binds_.insert(bindKey);

    std::string moduleName = fileBindModuleName(decl);
    if (decl.resolvedFileId != 0) {
        fileModuleNames_[decl.resolvedFileId] =
            !decl.resolvedModuleName.empty() ? decl.resolvedModuleName : moduleName;
        auto &fileModuleIds = fileBoundModuleIds_[decl.loc.file_id];
        for (const auto &visibleName : fileBindVisibleModuleNames(decl)) {
            auto existing = fileModuleIds.find(visibleName);
            if (existing == fileModuleIds.end() || existing->second == decl.resolvedFileId)
                fileModuleIds[visibleName] = decl.resolvedFileId;
            boundFileModuleIds_.emplace(visibleName, decl.resolvedFileId);
        }
    }

    // File binds are resolved through moduleExports_ during semantic analysis
    // instead of occupying ordinary symbol-table slots. That avoids collisions
    // between a bound module name and an exported declaration from that file
    // (for example `module Inner; class Inner { ... }`).
}

/// @brief Rebuild file-module export maps for all resolved file binds.
/// @param binds Module binds in the current compilation unit.
/// @param decls Flattened declarations from which each bound file's exports are collected.
/// @details Records both importer-file-specific visibility and globally unambiguous compatibility
///          entries. Conflicting aliases in the same importing file are diagnosed.
void Sema::buildBoundFileExports(const std::vector<BindDecl> &binds,
                                 const std::vector<DeclPtr> &decls) {
    moduleExports_.clear();
    fileModuleExports_.clear();
    fileBoundModuleIds_.clear();
    boundFileModuleIds_.clear();
    std::unordered_map<uint32_t, std::unordered_map<std::string, uint32_t>> moduleIdsByImporterFile;

    for (const auto &bind : binds) {
        if (bind.isNamespaceBind || bind.resolvedFileId == 0)
            continue;

        std::unordered_map<std::string, Symbol> exports;
        collectExportedSymbolsForFile(bind.resolvedFileId, decls, exports);

        auto &idsForImporter = moduleIdsByImporterFile[bind.loc.file_id];
        const auto visibleModuleNames = fileBindVisibleModuleNames(bind);
        for (size_t moduleNameIndex = 0; moduleNameIndex < visibleModuleNames.size();
             ++moduleNameIndex) {
            const auto &moduleName = visibleModuleNames[moduleNameIndex];
            auto existingId = idsForImporter.find(moduleName);
            if (existingId != idsForImporter.end() && existingId->second != bind.resolvedFileId) {
                if (moduleNameIndex != 0 && bind.alias.empty())
                    continue;
                error(bind.loc, "Duplicate bound module name or alias '" + moduleName + "'");
                continue;
            }
            idsForImporter[moduleName] = bind.resolvedFileId;
            boundFileModuleIds_.emplace(moduleName, bind.resolvedFileId);
            fileBoundModuleIds_[bind.loc.file_id][moduleName] = bind.resolvedFileId;

            if (moduleExports_.find(moduleName) == moduleExports_.end())
                moduleExports_[moduleName] = exports;

            fileModuleExports_[bind.loc.file_id][moduleName] = exports;
        }
    }
}

/// @brief Find exports for a file module visible at a source location.
/// @param moduleName The visible module root, such as an alias or file stem.
/// @param useLoc Location of the use whose file-local imports should be consulted.
/// @return Export map for the module, or nullptr when no matching file module is visible.
const std::unordered_map<std::string, Symbol> *Sema::findModuleExports(
    const std::string &moduleName, SourceLoc useLoc) const {
    if (useLoc.file_id != 0) {
        auto fileIt = fileModuleExports_.find(useLoc.file_id);
        if (fileIt != fileModuleExports_.end()) {
            auto moduleIt = fileIt->second.find(moduleName);
            if (moduleIt != fileIt->second.end())
                return &moduleIt->second;
        }
    }

    auto moduleIt = moduleExports_.find(moduleName);
    if (moduleIt == moduleExports_.end())
        return nullptr;
    return &moduleIt->second;
}

/// @brief Return true if a file-module root is visible at a source location.
/// @param moduleName The visible module root to test.
/// @param useLoc Location whose file-local imports should be consulted.
bool Sema::hasModuleExports(const std::string &moduleName, SourceLoc useLoc) const {
    return findModuleExports(moduleName, useLoc) != nullptr;
}

/// @brief Collect exported top-level symbols originating in one source file.
/// @param fileId Source file whose declarations are selected.
/// @param decls Candidate declarations, including nested namespaces.
/// @param out Destination map keyed by source-visible export name.
/// @details Externs and non-exported declarations are excluded; namespace exports are traversed
///          recursively while preserving their qualified lookup names.
void Sema::collectExportedSymbolsForFile(uint32_t fileId,
                                         const std::vector<DeclPtr> &decls,
                                         std::unordered_map<std::string, Symbol> &out) const {
    /// @brief Copies one visible exported symbol into the file export map.
    /// @param exportName Source-visible exported name.
    /// @param lookupName Semantic lookup name.
    auto addLookupSymbol = [&](const std::string &exportName, const std::string &lookupName) {
        Symbol *sym = const_cast<Sema *>(this)->currentScope_->lookup(lookupName);
        if (!sym || !sym->isExported || sym->isExtern)
            return;
        out[exportName] = *sym;
    };

    /// @brief Recursively collects exports from declarations and namespaces.
    /// @param items Declarations to inspect.
    /// @param exportPrefix Prefix exposed through the containing namespace.
    /// @param self Recursive callback reference.
    auto collectFromDecls = [&](const std::vector<DeclPtr> &items,
                                const std::string &exportPrefix,
                                const auto &self) -> void {
        for (const auto &decl : items) {
            if (!decl || decl->loc.file_id != fileId || !decl->isExported)
                continue;

            /// @brief Builds the semantic lookup name for one exported declaration.
            /// @param baseName Declaration's local name.
            /// @return Qualified semantic name.
            auto lookupNameFor = [&](const std::string &baseName) {
                if (!exportPrefix.empty())
                    return exportPrefix + baseName;
                return semanticNameForDecl(*decl, baseName);
            };

            switch (decl->kind) {
                case DeclKind::Function:
                    addLookupSymbol(exportPrefix + static_cast<FunctionDecl *>(decl.get())->name,
                                    lookupNameFor(static_cast<FunctionDecl *>(decl.get())->name));
                    break;
                case DeclKind::Struct:
                    addLookupSymbol(exportPrefix + static_cast<StructDecl *>(decl.get())->name,
                                    lookupNameFor(static_cast<StructDecl *>(decl.get())->name));
                    break;
                case DeclKind::Class:
                    addLookupSymbol(exportPrefix + static_cast<ClassDecl *>(decl.get())->name,
                                    lookupNameFor(static_cast<ClassDecl *>(decl.get())->name));
                    break;
                case DeclKind::Interface:
                    addLookupSymbol(exportPrefix + static_cast<InterfaceDecl *>(decl.get())->name,
                                    lookupNameFor(static_cast<InterfaceDecl *>(decl.get())->name));
                    break;
                case DeclKind::Enum:
                    addLookupSymbol(exportPrefix + static_cast<EnumDecl *>(decl.get())->name,
                                    lookupNameFor(static_cast<EnumDecl *>(decl.get())->name));
                    break;
                case DeclKind::GlobalVar:
                    addLookupSymbol(exportPrefix + static_cast<GlobalVarDecl *>(decl.get())->name,
                                    lookupNameFor(static_cast<GlobalVarDecl *>(decl.get())->name));
                    break;
                case DeclKind::TypeAlias:
                    addLookupSymbol(exportPrefix + static_cast<TypeAliasDecl *>(decl.get())->name,
                                    lookupNameFor(static_cast<TypeAliasDecl *>(decl.get())->name));
                    break;
                case DeclKind::Namespace: {
                    const auto *ns = static_cast<NamespaceDecl *>(decl.get());
                    std::string exportName = ns->name;
                    auto dot = exportName.find('.');
                    if (dot != std::string::npos)
                        exportName = exportName.substr(0, dot);

                    Symbol sym;
                    sym.kind = Symbol::Kind::Module;
                    sym.name = exportName;
                    sym.type = types::module(ns->name);
                    sym.isFinal = true;
                    sym.isExported = true;
                    sym.decl = decl.get();
                    sym.loc = decl->loc;
                    out[exportName] = sym;
                    self(ns->declarations, ns->name + ".", self);
                    break;
                }
                default:
                    break;
            }
        }
    };
    collectFromDecls(decls, "", collectFromDecls);
}

/// @brief Choose the primary qualifier introduced by a file bind.
/// @param decl File bind declaration.
/// @return Explicit alias, declared module name, or path stem in precedence order.
std::string Sema::fileBindModuleName(const BindDecl &decl) const {
    if (!decl.alias.empty())
        return decl.alias;
    if (!decl.resolvedModuleName.empty())
        return decl.resolvedModuleName;

    return fileBindPathStem(decl);
}

/// @brief Derive the unaliased module qualifier from a bind path.
/// @param decl The bind declaration whose path should be reduced.
/// @return The basename of the bind path without a trailing `.zia` extension.
std::string Sema::fileBindPathStem(const BindDecl &decl) const {
    std::string path = decl.path;
    auto lastSlash = path.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        path = path.substr(lastSlash + 1);

    auto extPos = path.rfind(".zia");
    if (extPos != std::string::npos)
        path = path.substr(0, extPos);

    return path;
}

/// @brief Return every visible module qualifier introduced by a file bind.
/// @param decl The file bind declaration to inspect.
/// @return Ordered list containing the primary qualifier and, for unaliased
///         binds, the path-stem compatibility qualifier when distinct.
std::vector<std::string> Sema::fileBindVisibleModuleNames(const BindDecl &decl) const {
    std::vector<std::string> names;
    /// @brief Appends one nonempty, nonduplicate visible module name.
    /// @param name Candidate module qualifier.
    auto addName = [&](std::string name) {
        if (name.empty())
            return;
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(std::move(name));
    };

    addName(fileBindModuleName(decl));
    if (decl.alias.empty())
        addName(fileBindPathStem(decl));
    return names;
}

/// @brief Analyze a bind declaration that exposes a runtime namespace.
/// @details Handles three forms of namespace binding:
///          1. Selective bind: `bind Zanna.Terminal { Say, ReadLine };`
///             - Only listed symbols are exposed in scope
///          2. Alias bind: `bind Zanna.Terminal as T;`
///             - Namespace accessible via alias (e.g., T.Say())
///          3. Full bind: `bind Zanna.Terminal;`
///             - All namespace symbols are exposed in the current scope
///          Validates that the namespace exists and checks for symbol conflicts.
/// @param decl The bind declaration AST node to analyze.
void Sema::analyzeNamespaceBind(BindDecl &decl) {
    const std::string &ns = decl.path;

    // Validate this is a known runtime namespace
    if (!isValidRuntimeNamespace(ns)) {
        error(decl.loc, "Unknown runtime namespace: " + ns);
        return;
    }

    // Store the bound namespace
    if (!decl.alias.empty()) {
        auto aliasIt = aliasToNamespace_.find(decl.alias);
        if (aliasIt != aliasToNamespace_.end() && aliasIt->second != ns) {
            error(decl.loc,
                  "Namespace alias '" + decl.alias + "' is already bound to " + aliasIt->second);
            return;
        }
    }
    boundNamespaces_[ns] = decl.alias;
    if (!decl.alias.empty())
        aliasToNamespace_[decl.alias] = ns;

    if (!decl.specificItems.empty()) {
        // Selective import: bind Zanna.Terminal { Say, ReadLine };
        for (const auto &item : decl.specificItems) {
            std::string fullName = ns + "." + item;
            Symbol *sym = lookupSymbol(fullName);
            if (!sym) {
                error(decl.loc, "Unknown symbol '" + item + "' in namespace " + ns);
                continue;
            }
            // Check for conflicts with existing imports
            auto existingIt = importedSymbols_.find(item);
            if (existingIt != importedSymbols_.end() && existingIt->second != fullName) {
                error(decl.loc,
                      "Symbol '" + item + "' conflicts with existing import from " +
                          existingIt->second);
                continue;
            }
            importedSymbols_[item] = fullName;
        }
    } else if (!decl.alias.empty()) {
        // Alias import: bind Zanna.Terminal as T;
        // Register alias as a module symbol for qualified access
        Symbol sym;
        sym.kind = Symbol::Kind::Module;
        sym.name = decl.alias;
        sym.type = types::module(ns);
        sym.isFinal = true;
        defineSymbol(decl.alias, sym);
        // Also register in importedSymbols_ so type resolution can expand
        // aliased dotted names (e.g., T.Canvas → Zanna.Graphics.Canvas)
        importedSymbols_[decl.alias] = ns;
    } else {
        // Full namespace import: bind Zanna.Terminal;
        // Import all symbols from this namespace into scope
        importNamespaceSymbols(ns, decl.loc);
    }
}

/// @brief Check whether a runtime namespace exists.
/// @details Scans type registry, runtime class catalog, and scope symbols
///          for any entry matching the given namespace prefix.
/// @param ns The namespace string to validate.
/// @return True if any registered type, class, or method matches the namespace prefix.
bool Sema::isValidRuntimeNamespace(const std::string &ns) {
    // A namespace is valid if any runtime class or method starts with "ns."
    const std::string prefix = ns + ".";
    const std::string_view prefixView(prefix);

    // Check typeRegistry_ for runtime class types
    for (const auto &[name, type] : typeRegistry_) {
        if (name.rfind(prefix, 0) == 0 || name == ns) {
            return true;
        }
    }

    // Also check the RuntimeRegistry for methods that match this namespace
    const auto &registry = il::runtime::RuntimeRegistry::instance();
    const auto &catalog = registry.rawCatalog();
    for (const auto &cls : catalog) {
        // Check if class is in this namespace
        std::string clsName = cls.qname ? cls.qname : "";
        if (clsName.rfind(prefix, 0) == 0 || clsName == ns)
            return true;

        // Check if any method target starts with this namespace
        for (const auto &m : cls.methods) {
            if (m.target && std::string(m.target).rfind(prefix, 0) == 0)
                return true;
        }
    }

    // Function-only namespaces can exist even when they do not publish a
    // duplicate runtime class in the catalog.
    for (const auto &alias : il::runtime::kRuntimeNameAliases) {
        std::string_view canonical = alias.canonical;
        if (canonical.size() >= prefixView.size() &&
            canonical.substr(0, prefixView.size()) == prefixView)
            return true;
    }

    // Check scope symbols for extern functions registered via defineExternFunction
    // (e.g., Zanna.Box.* functions that aren't part of a runtime class)
    for (Scope *s = currentScope_; s != nullptr; s = s->parent()) {
        if (s->hasSymbolWithPrefix(prefix))
            return true;
    }

    return false;
}

/// @brief Import all symbols from a runtime namespace into the current scope.
/// @details Imports types from typeRegistry_, classes and methods from RuntimeRegistry,
///          properties by display name, and direct functions from kRuntimeNameAliases.
/// @param ns The namespace whose symbols should be imported.
/// @param loc Bind location used for name-conflict warnings.
void Sema::importNamespaceSymbols(const std::string &ns, SourceLoc loc) {
    // Import all symbols from this namespace
    const std::string prefix = ns + ".";

    // Walk through all registered types and import matching class names
    for (const auto &[name, type] : typeRegistry_) {
        if (name.rfind(prefix, 0) == 0) {
            // Extract short name (e.g., "Canvas" from "Zanna.Graphics.Canvas")
            std::string shortName = name.substr(prefix.size());
            // Skip nested namespaces (only import direct children)
            if (shortName.find('.') != std::string::npos)
                continue;

            // Check for conflicts
            auto existingIt = importedSymbols_.find(shortName);
            if (existingIt != importedSymbols_.end() && existingIt->second != name) {
                if (loc.isValid())
                    warning(loc,
                            "Imported symbol '" + shortName + "' from " + name +
                                " conflicts with existing import from " + existingIt->second);
                continue;
            }
            importedSymbols_[shortName] = name;
        }
    }

    // Import class names and function symbols using the RuntimeRegistry
    // This gives us access to all registered runtime classes and methods
    const auto &registry = il::runtime::RuntimeRegistry::instance();
    const auto &catalog = registry.rawCatalog();

    for (const auto &cls : catalog) {
        // Import the class name itself (e.g., "Canvas" from "Zanna.Graphics.Canvas")
        std::string clsName = cls.qname ? cls.qname : "";
        if (!clsName.empty() && clsName.rfind(prefix, 0) == 0) {
            // Extract short name
            std::string shortName = clsName.substr(prefix.size());
            // Skip nested namespaces (only import direct children)
            if (shortName.find('.') == std::string::npos) {
                auto existingIt = importedSymbols_.find(shortName);
                if (existingIt == importedSymbols_.end()) {
                    importedSymbols_[shortName] = clsName;
                }
            }
        }

        // Import methods from classes in this namespace
        for (const auto &m : cls.methods) {
            if (!m.target)
                continue;

            std::string target(m.target);
            if (target.rfind(prefix, 0) != 0)
                continue;

            // Extract short name (e.g., "Say" from "Zanna.Terminal.Say")
            std::string shortName = target.substr(prefix.size());

            // Skip nested namespaces (only import direct children)
            if (shortName.find('.') != std::string::npos)
                continue;

            // Check for conflicts
            auto existingIt = importedSymbols_.find(shortName);
            if (existingIt != importedSymbols_.end() && existingIt->second != target) {
                if (loc.isValid())
                    warning(loc,
                            "Imported symbol '" + shortName + "' from " + target +
                                " conflicts with existing import from " + existingIt->second);
                continue;
            }
            importedSymbols_[shortName] = target;
        }

        // Also import properties by their display name for namespace-like
        // runtime classes (e.g., Length backed by Zanna.String.get_Length).
        // Do not import nested class properties from a broad namespace such as
        // Zanna.Game; otherwise names like Grid2D.Size become global
        // Length() candidates and can shadow Zanna.String.get_Length.
        for (const auto &p : cls.properties) {
            if (p.getter && p.name) {
                std::string getter(p.getter);
                if (getter.rfind(prefix, 0) != 0)
                    continue;
                std::string getterTail = getter.substr(prefix.size());
                if (getterTail.find('.') != std::string::npos)
                    continue;

                std::string shortName(p.name);
                if (shortName.find('.') != std::string::npos)
                    continue;

                auto existingIt = importedSymbols_.find(shortName);
                if (existingIt == importedSymbols_.end())
                    importedSymbols_[shortName] = getter;
            }
        }
    }

    // Import standalone runtime functions and non-conflicting alias-only
    // namespace groups from runtime.def entries not in the RuntimeClasses
    // catalog.
    // Direct children (e.g., Zanna.String.Capitalize for bind Zanna.String)
    // are imported as short name → qualified name mappings.
    // Nested compatibility aliases that were renamed to remove collisions are
    // intentionally skipped during broad imports. Explicit binds to those alias
    // namespaces still validate through isValidRuntimeNamespace().
    for (const auto &alias : il::runtime::kRuntimeNameAliases) {
        std::string canonical(alias.canonical);
        if (canonical.rfind(prefix, 0) != 0)
            continue;
        std::string shortName = canonical.substr(prefix.size());
        auto dotPos = shortName.find('.');
        if (dotPos != std::string::npos) {
            std::string subNs = shortName.substr(0, dotPos);
            if (suppressBroadRuntimeAliasNamespaceImport(ns, subNs))
                continue;
            if (importedSymbols_.find(subNs) == importedSymbols_.end())
                importedSymbols_[subNs] = ns + "." + subNs;
            continue;
        }

        // Direct child — standalone function (e.g., "Capitalize" from bind Zanna.String)
        auto existingIt = importedSymbols_.find(shortName);
        if (existingIt == importedSymbols_.end())
            importedSymbols_[shortName] = canonical;
    }
}

/// @brief Analyze a global variable declaration, type-checking its initializer.
/// @param decl The global variable declaration to analyze.
void Sema::analyzeGlobalVarDecl(GlobalVarDecl &decl) {
    TypeRef declaredType = decl.type ? resolveTypeNode(decl.type.get()) : nullptr;
    if (isForbiddenValueType(declaredType)) {
        error(decl.loc,
              "Type '" + forbiddenValueTypeName(declaredType) +
                  "' cannot be used for a global value");
        declaredType = types::unknown();
    }

    // Analyze initializer if present
    if (decl.initializer) {
        TypeRef initType = analyzeExpr(decl.initializer.get());
        if (declaredType && declaredType->kind == TypeKindSem::Set &&
            decl.initializer->kind == ExprKind::MapLiteral) {
            auto *mapLiteral = static_cast<MapLiteralExpr *>(decl.initializer.get());
            if (mapLiteral->entries.empty()) {
                exprTypes_[decl.initializer.get()] = declaredType;
                initType = declaredType;
            }
        }
        if (initType && initType->kind == TypeKindSem::Unit) {
            error(decl.initializer->loc,
                  "Unit literal cannot be stored; use null for optional values or omit the value "
                  "in a void context");
            initType = types::unknown();
        }

        // If type was inferred, update the symbol
        Symbol *sym = lookupSymbol(semanticNameForDecl(decl, decl.name));
        if (!sym)
            sym = lookupSymbol(decl.name);
        if (sym && sym->type->isUnknown() && decl.initializer->kind == ExprKind::NullLiteral &&
            initType && initType->kind == TypeKindSem::Optional && initType->innerType() &&
            initType->innerType()->kind == TypeKindSem::Unknown) {
            error(decl.loc,
                  "Cannot infer type from null initializer; add an explicit type annotation "
                  "such as 'String?', 'MyType', or 'GUI.Font'");
            return;
        }
        if (sym && sym->type->isUnknown()) {
            sym->type = initType;
        } else if (sym) {
            initType = contextualizeIntegerLiteralForDeclaredType(
                sym->type, initType, decl.initializer.get());
            if (initType && initType->kind != TypeKindSem::Unknown &&
                !sym->type->isAssignableFrom(*initType))
                errorTypeMismatch(decl.initializer->loc, sym->type, initType);
        }
    } else {
        Symbol *sym = lookupSymbol(semanticNameForDecl(decl, decl.name));
        if (!sym)
            sym = lookupSymbol(decl.name);
        if (decl.isFinal) {
            error(decl.loc, "'final' declarations require an initializer");
        } else if (sym && sym->type->isUnknown()) {
            error(decl.loc, "Cannot infer type without initializer");
        }
    }
}

/// @brief Validate that a type implements all required interface methods.
/// @details For each interface method, checks that the implementing type has a method
///          with matching name and compatible signature, and that it is publicly visible.
/// @param typeName The name of the implementing type.
/// @param loc Source location attributed to conformance diagnostics.
/// @param interfaces The list of interface names the type claims to implement.
void Sema::validateInterfaceImplementations(const std::string &typeName,
                                            const SourceLoc &loc,
                                            const std::vector<std::string> &interfaces) {
    for (const auto &ifaceName : interfaces) {
        std::string resolvedIfaceName = ifaceName;
        if (TypeRef ifaceType = resolveNamedType(ifaceName, loc);
            ifaceType && ifaceType->kind == TypeKindSem::Interface) {
            resolvedIfaceName = ifaceType->name;
        }

        auto ifaceIt = interfaceDecls_.find(resolvedIfaceName);
        if (ifaceIt == interfaceDecls_.end()) {
            error(loc, "Unknown interface: " + ifaceName);
            continue;
        }

        bool ok = true;
        InterfaceDecl *iface = ifaceIt->second;
        for (auto &member : iface->members) {
            if (member->kind != DeclKind::Method)
                continue;

            auto *ifaceMethod = static_cast<MethodDecl *>(member.get());
            TypeRef ifaceType = getMethodType(ifaceMethod);
            if (!ifaceType)
                continue;

            MethodDecl *implMethod = nullptr;
            for (auto *candidate : collectMethodOverloads(typeName, ifaceMethod->name, true)) {
                TypeRef candidateType = getMethodType(candidate);
                if (candidateType && candidateType->equals(*ifaceType)) {
                    implMethod = candidate;
                    break;
                }
            }

            if (!implMethod && !ifaceMethod->body) {
                error(loc,
                      "Type '" + typeName + "' does not implement interface method '" +
                          resolvedIfaceName + "." + ifaceMethod->name + "'");
                ok = false;
                continue;
            }

            if (implMethod && implMethod->visibility != Visibility::Public) {
                error(loc,
                      "Method '" + typeName + "." + implMethod->name +
                          "' must be public to satisfy interface '" + resolvedIfaceName + "'");
                ok = false;
            }
        }

        if (ok)
            types::registerInterfaceImplementation(typeName, resolvedIfaceName);
    }
}

/// @brief Analyze a struct type declaration body (fields, methods, interface validation).
/// @param decl The struct type declaration to analyze.
void Sema::analyzeStructDecl(StructDecl &decl) {
    // Generic types are registered in the first pass; skip body analysis
    if (!decl.genericParams.empty())
        return;

    const std::string ownerName = semanticNameForDecl(decl, decl.name);
    auto selfType = types::structType(ownerName);
    currentSelfType_ = selfType;

    pushScope(decl.loc);

    // Analyze fields
    for (auto &member : decl.members) {
        if (member->kind == DeclKind::Field) {
            analyzeFieldDecl(*static_cast<FieldDecl *>(member.get()), selfType);
        }
    }

    // Analyze methods
    for (auto &member : decl.members) {
        if (member->kind == DeclKind::Method) {
            analyzeMethodDecl(*static_cast<MethodDecl *>(member.get()), selfType);
        } else if (member->kind == DeclKind::Property) {
            analyzePropertyDecl(*static_cast<PropertyDecl *>(member.get()), selfType);
        }
    }

    // Validate interface implementations after members are known
    validateInterfaceImplementations(ownerName, decl.loc, decl.interfaces);

    popScope(decl.loc);
    currentSelfType_ = nullptr;
}

/// @brief Register field and method type signatures for a type declaration.
/// @details Populates fieldTypes_ and methodTypes_ maps with qualified keys
///          (typeName.memberName) so cross-type member references resolve correctly.
/// @tparam T The declaration type (ClassDecl, StructDecl, or InterfaceDecl).
/// @param decl The type declaration whose members should be registered.
/// @param includeFields If true, register field types; false for interface-only registration.
template <typename T> void Sema::registerTypeMembers(T &decl, bool includeFields) {
    const std::string ownerName = semanticNameForDecl(decl, decl.name);

    // Register field types (if applicable)
    std::unordered_set<std::string> seenFields;
    std::unordered_set<std::string> seenProperties;
    std::unordered_set<std::string> generatedAccessors;
    if (includeFields) {
        for (auto &member : decl.members) {
            if (member->kind == DeclKind::Field) {
                auto *field = static_cast<FieldDecl *>(member.get());
                if (!seenFields.insert(field->name).second ||
                    seenProperties.contains(field->name)) {
                    error(field->loc,
                          "Duplicate definition of '" + field->name + "' in type '" + decl.name +
                              "'");
                    continue;
                }
                TypeRef fieldType =
                    field->type ? resolveTypeNode(field->type.get()) : types::unknown();
                std::string fieldKey = ownerName + "." + field->name;
                fieldTypes_[fieldKey] = fieldType;
                if (field->isStatic)
                    staticFields_.insert(fieldKey);
                if (field->isFinal)
                    finalFields_.insert(fieldKey);
                memberVisibility_[fieldKey] = field->visibility;
            } else if (member->kind == DeclKind::Property) {
                auto *prop = static_cast<PropertyDecl *>(member.get());
                if (!seenProperties.insert(prop->name).second || seenFields.contains(prop->name)) {
                    error(prop->loc,
                          "Duplicate definition of '" + prop->name + "' in type '" + decl.name +
                              "'");
                    continue;
                }

                TypeRef propType =
                    prop->type ? resolveTypeNode(prop->type.get()) : types::unknown();
                memberVisibility_[ownerName + "." + prop->name] = prop->visibility;

                if (prop->getterBody) {
                    generatedAccessors.insert("get_" + prop->name);
                    std::string getterKey = ownerName + ".get_" + prop->name;
                    TypeRef getterType = types::function({}, propType);
                    methodTypes_[getterKey] = getterType;
                    memberVisibility_[getterKey] = prop->visibility;
                }

                if (prop->setterBody) {
                    generatedAccessors.insert("set_" + prop->name);
                    std::string setterKey = ownerName + ".set_" + prop->name;
                    TypeRef setterType = types::function({propType}, types::voidType());
                    methodTypes_[setterKey] = setterType;
                    memberVisibility_[setterKey] = prop->visibility;
                }
            }
        }
    }

    // Register method types (signatures only, not bodies)
    std::unordered_set<std::string> seenMethods;
    for (auto &member : decl.members) {
        if (member->kind == DeclKind::Method) {
            auto *method = static_cast<MethodDecl *>(member.get());
            if (seenFields.contains(method->name) || seenProperties.contains(method->name)) {
                error(method->loc,
                      "Duplicate definition of '" + method->name + "' in type '" + decl.name + "'");
                continue;
            }
            if (generatedAccessors.contains(method->name)) {
                error(method->loc,
                      "Method '" + method->name +
                          "' conflicts with a generated property accessor in type '" + decl.name +
                          "'");
                continue;
            }
            seenMethods.insert(method->name);
            TypeRef methodType = methodTypeForDecl(*method);
            if (!registerMethodOverload(ownerName, method, methodType, method->loc))
                continue;

            std::string methodKey = ownerName + "." + method->name;
            if (methodTypes_.find(methodKey) == methodTypes_.end())
                methodTypes_[methodKey] = methodType;
            memberVisibility_[methodKey] = method->visibility;
        }
    }
}

// Explicit template instantiations
template void Sema::registerTypeMembers<ClassDecl>(ClassDecl &, bool);
template void Sema::registerTypeMembers<StructDecl>(StructDecl &, bool);
template void Sema::registerTypeMembers<InterfaceDecl>(InterfaceDecl &, bool);

/// @brief Register class member signatures (fields and methods).
/// @details Skips generic types, which are registered during instantiation.
/// @param decl Class declaration whose members are registered.
void Sema::registerClassMembers(ClassDecl &decl) {
    // Skip member registration for generic types - done during instantiation
    if (!decl.genericParams.empty())
        return;
    registerTypeMembers(decl, true);
}

/// @brief Register value member signatures (fields and methods).
/// @details Skips generic types, which are registered during instantiation.
/// @param decl Struct declaration whose members are registered.
void Sema::registerStructMembers(StructDecl &decl) {
    // Skip member registration for generic types - done during instantiation
    if (!decl.genericParams.empty())
        return;
    registerTypeMembers(decl, true);
}

/// @brief Register interface member signatures (methods only, no fields).
/// @param decl Interface declaration whose method requirements are registered.
/// @details Generic parameters are installed temporarily so member annotations resolve to
///          semantic type parameters.
void Sema::registerInterfaceMembers(InterfaceDecl &decl) {
    if (decl.genericParams.empty()) {
        registerTypeMembers(decl, false);
        return;
    }

    std::map<std::string, TypeRef> params;
    for (const auto &param : decl.genericParams)
        params[param] = types::typeParam(param);
    pushTypeParams(params);
    registerTypeMembers(decl, false);
    popTypeParams();
}

/// @brief Analyze a class type declaration body.
/// @details Handles base class inheritance by copying parent fields and methods into scope,
///          pre-defines method symbols for intra-class calls, then analyzes member bodies.
///          Validates interface implementations after all members are analyzed.
/// @param decl The class declaration to analyze.
void Sema::analyzeClassDecl(ClassDecl &decl) {
    // Generic types are registered in the first pass; skip body analysis
    if (!decl.genericParams.empty())
        return;

    const std::string ownerName = semanticNameForDecl(decl, decl.name);
    auto selfType = types::classType(ownerName);
    currentSelfType_ = selfType;

    pushScope(decl.loc);

    std::unordered_set<std::string> declaredFieldNames;
    std::vector<MethodDecl *> declaredMethods;
    for (auto &member : decl.members) {
        if (member->kind == DeclKind::Field) {
            auto *field = static_cast<FieldDecl *>(member.get());
            declaredFieldNames.insert(field->name);
        } else if (member->kind == DeclKind::Method) {
            auto *method = static_cast<MethodDecl *>(member.get());
            declaredMethods.push_back(method);
        }
    }

    // BUG-VL-006 fix: Handle inheritance - add parent's members to scope
    if (!decl.baseClass.empty()) {
        if (TypeRef resolvedBase = resolveNamedType(decl.baseClass, decl.loc);
            resolvedBase && resolvedBase->kind == TypeKindSem::Class) {
            decl.baseClass = resolvedBase->name;
        }

        auto parentIt = classDecls_.find(decl.baseClass);
        if (parentIt == classDecls_.end()) {
            error(decl.loc, "Unknown base class: " + decl.baseClass);
        } else {
            // BUG-VL-007 fix: Register inheritance for polymorphism support
            types::registerClassInheritance(ownerName, decl.baseClass);

            // Add ALL parent's fields to this class's scope (including inherited fields)
            // by scanning fieldTypes_ for entries with parent's name prefix.
            // This ensures grandparent fields are also accessible in grandchildren.
            // NOTE: Collect entries first, then insert — inserting into an
            // unordered_map during iteration can trigger a rehash and
            // invalidate the iterator.
            std::string parentPrefix = decl.baseClass + ".";
            std::vector<std::pair<std::string, TypeRef>> inheritedFields;
            for (const auto &entry : fieldTypes_) {
                if (entry.first.rfind(parentPrefix, 0) == 0) {
                    if (staticFields_.contains(entry.first))
                        continue;
                    std::string fieldName = entry.first.substr(parentPrefix.size());
                    if (!declaredFieldNames.contains(fieldName) &&
                        !currentScope_->lookupLocal(fieldName)) {
                        inheritedFields.emplace_back(fieldName, entry.second);
                    }
                }
            }
            for (const auto &[fieldName, fieldType] : inheritedFields) {
                Symbol sym;
                sym.kind = Symbol::Kind::Field;
                sym.name = fieldName;
                sym.type = fieldType;
                defineSymbol(fieldName, sym);
            }
        }
    }

    if (!decl.baseClass.empty()) {
        auto parentIt = classDecls_.find(decl.baseClass);
        if (parentIt != classDecls_.end()) {
            for (auto *method : declaredMethods) {
                MethodDecl *parentMethod = findInheritedExactMethod(ownerName, *method);
                if (method->isOverride && !parentMethod) {
                    error(method->loc,
                          "Method '" + method->name +
                              "' is marked override but no parent method with the same signature "
                              "exists");
                    continue;
                }

                if (parentMethod && !method->isOverride && method->name != "init") {
                    error(method->loc,
                          "Method '" + method->name +
                              "' must be marked override to replace an inherited overload");
                    continue;
                }

                if (parentMethod) {
                    TypeRef parentType = getMethodType(decl.baseClass, parentMethod);
                    TypeRef childType = getMethodType(ownerName, method);
                    if (parentType && childType && !parentType->equals(*childType)) {
                        error(method->loc,
                              "Method '" + method->name +
                                  "' must preserve the inherited return type for an override");
                    }
                }
            }
        }
    }

    // Analyze fields first (adds them to scope)
    for (auto &member : decl.members) {
        if (member->kind == DeclKind::Field) {
            analyzeFieldDecl(*static_cast<FieldDecl *>(member.get()), selfType);
        }
    }

    // Pre-define method symbols in scope so they can be called without 'self.'
    // This allows methods to call each other by bare name within the class.
    for (auto &member : decl.members) {
        if (member->kind == DeclKind::Method) {
            auto *method = static_cast<MethodDecl *>(member.get());
            TypeRef methodType = getMethodType(ownerName, method);
            Symbol sym;
            sym.kind = Symbol::Kind::Method;
            sym.name = method->name;
            sym.type = methodType ? methodType : methodTypeForDecl(*method);
            sym.isFinal = true;
            sym.decl = method;
            if (!currentScope_->lookupLocal(method->name))
                defineSymbol(method->name, sym);
        }
    }

    // Analyze methods (now they can reference each other by bare name)
    for (auto &member : decl.members) {
        if (member->kind == DeclKind::Method) {
            analyzeMethodDecl(*static_cast<MethodDecl *>(member.get()), selfType);
        } else if (member->kind == DeclKind::Property) {
            analyzePropertyDecl(*static_cast<PropertyDecl *>(member.get()), selfType);
        } else if (member->kind == DeclKind::Destructor) {
            analyzeDestructorDecl(*static_cast<DestructorDecl *>(member.get()), selfType);
        }
    }

    // Validate interface implementations
    validateInterfaceImplementations(ownerName, decl.loc, decl.interfaces);

    SourceLoc endLoc = decl.loc;
    if (!decl.members.empty())
        endLoc = decl.members.back()->loc;
    popScope(endLoc);
    currentSelfType_ = nullptr;
}

/// @brief Analyze an interface declaration, registering method signatures.
/// @param decl The interface declaration to analyze.
void Sema::analyzeInterfaceDecl(InterfaceDecl &decl) {
    const std::string ownerName = semanticNameForDecl(decl, decl.name);
    auto selfType = types::interface(ownerName);
    currentSelfType_ = selfType;

    pushScope(decl.loc);

    // Analyze method signatures and default bodies.
    for (auto &member : decl.members) {
        if (member->kind == DeclKind::Method) {
            auto *method = static_cast<MethodDecl *>(member.get());
            TypeRef methodType = getMethodType(ownerName, method);
            if (!methodType)
                methodType = methodTypeForDecl(*method);

            Symbol sym;
            sym.kind = Symbol::Kind::Method;
            sym.name = method->name;
            sym.type = methodType;
            sym.decl = method;
            if (!currentScope_->lookupLocal(method->name))
                defineSymbol(method->name, sym);
        }
    }

    for (auto &member : decl.members) {
        if (member->kind != DeclKind::Method)
            continue;
        auto *method = static_cast<MethodDecl *>(member.get());
        if (method->body)
            analyzeMethodDecl(*method, selfType);
    }

    popScope(decl.loc);
    currentSelfType_ = nullptr;
}

/// @brief Analyze an enum declaration: validate variants and register them.
/// @param decl The enum declaration to analyze.
void Sema::analyzeEnumDecl(EnumDecl &decl) {
    std::string enumName = semanticNameForDecl(decl, decl.name);
    auto enumT = types::enumType(enumName);
    enumDecls_[enumName] = &decl;

    // Validate variants and resolve values
    std::unordered_set<std::string> seenNames;
    int64_t nextValue = 0;

    for (auto &variant : decl.variants) {
        // Check for duplicate variant names
        if (!seenNames.insert(variant.name).second) {
            error(variant.loc,
                  "Duplicate enum variant '" + variant.name + "' in '" + decl.name + "'");
            nextValue = INT64_MAX;
            continue;
        }

        // Resolve the value (explicit or auto-increment)
        if (variant.explicitValue.has_value()) {
            nextValue = variant.explicitValue.value();
        } else {
            variant.explicitValue = nextValue;
        }

        // Register the variant as a field-like entry: EnumName.VariantName -> EnumType
        std::string key = enumName + "." + variant.name;
        fieldTypes_[key] = enumT;

        if (&variant == &decl.variants.back())
            continue;

        if (nextValue == INT64_MAX) {
            error(variant.loc,
                  "Enum '" + decl.name + "' auto-increment would exceed the maximum Integer value");
            continue;
        }
        ++nextValue;
    }
}

/// @brief Analyze a function declaration body (parameters, return type, body statements).
/// @param decl The function declaration to analyze.
void Sema::analyzeFunctionDecl(FunctionDecl &decl) {
    // Generic functions are registered in the first pass; skip body analysis
    // The body will be analyzed when the function is instantiated
    const bool analyzingGenericInstantiation = !decl.genericParams.empty() && inGenericContext();
    if (!decl.genericParams.empty() && !analyzingGenericInstantiation)
        return;

    if (!decl.isForeign && !decl.body) {
        error(decl.loc, "Function '" + decl.name + "' requires a body");
        return;
    }

    currentFunction_ = &decl;
    TypeRef funcType;
    if (analyzingGenericInstantiation) {
        funcType = functionTypeForDecl(decl);
    } else {
        auto declTypeIt = functionDeclTypes_.find(&decl);
        funcType =
            declTypeIt != functionDeclTypes_.end() ? declTypeIt->second : functionTypeForDecl(decl);
    }
    if (decl.isAsync)
        expectedReturnType_ =
            decl.returnType ? resolveTypeNode(decl.returnType.get()) : types::voidType();
    else
        expectedReturnType_ = funcType && funcType->kind == TypeKindSem::Function
                                  ? funcType->returnType()
                                  : types::voidType();

    if (expectedReturnType_ && expectedReturnType_->kind == TypeKindSem::Tuple) {
        error(decl.loc,
              "Functions cannot return a tuple type; return a struct with named fields, or use "
              "out-parameters");
        expectedReturnType_ = types::unknown();
    }

    pushScope(decl.loc);

    // Define parameters
    for (const auto &param : decl.params) {
        TypeRef paramType = param.type ? resolveTypeNode(param.type.get()) : types::unknown();
        if (param.isVariadic)
            paramType = types::list(paramType); // Body sees ...Integer as List[Integer]
        if (isForbiddenValueType(paramType)) {
            error(param.loc,
                  "Type '" + forbiddenValueTypeName(paramType) +
                      "' cannot be used for a function parameter");
            paramType = types::unknown();
        }

        Symbol sym;
        sym.kind = Symbol::Kind::Parameter;
        sym.name = param.name;
        sym.type = paramType;
        sym.isFinal = true; // Parameters are immutable by default
        defineSymbol(param.name, sym, param.loc);
        markInitialized(param.name);
    }

    // Analyze body
    if (decl.body) {
        analyzeStmt(decl.body.get());

        // W008: Missing return in non-void function
        if (expectedReturnType_ && expectedReturnType_->kind != TypeKindSem::Void) {
            if (!stmtAlwaysExits(decl.body.get())) {
                warn(WarningCode::W008_MissingReturn,
                     decl.loc,
                     "Function '" + decl.name + "' may not return a value on all code paths");
            }
        }
    }

    popScope(decl.body ? scopeEndForStmt(decl.body.get()) : decl.loc);

    currentFunction_ = nullptr;
    expectedReturnType_ = nullptr;
}

/// @brief Analyze a field declaration (type resolution and initializer type checking).
/// @param decl The field declaration to analyze.
/// @param ownerType The type that owns this field.
void Sema::analyzeFieldDecl(FieldDecl &decl, TypeRef ownerType) {
    TypeRef fieldType = decl.type ? resolveTypeNode(decl.type.get()) : types::unknown();
    if (isForbiddenValueType(fieldType)) {
        error(decl.loc,
              "Type '" + forbiddenValueTypeName(fieldType) + "' cannot be used for a field");
        fieldType = types::unknown();
    }

    if (decl.isWeak) {
        auto weakTargetType = fieldType;
        if (weakTargetType && weakTargetType->kind == TypeKindSem::Optional &&
            weakTargetType->innerType()) {
            weakTargetType = weakTargetType->innerType();
        }
        bool weakCompatible = weakTargetType && (weakTargetType->kind == TypeKindSem::Class ||
                                                 weakTargetType->kind == TypeKindSem::Interface ||
                                                 weakTargetType->kind == TypeKindSem::Ptr ||
                                                 weakTargetType->kind == TypeKindSem::Any);
        if (!weakCompatible) {
            error(decl.loc,
                  "weak fields require a class, interface, Ptr, Any, or optional reference type");
        }
        if (decl.isStatic) {
            error(decl.loc, "weak fields cannot be static");
        }
        if (ownerType && ownerType->kind != TypeKindSem::Class) {
            error(decl.loc, "weak fields are only supported on class types");
        }
    }

    // Check initializer type
    if (decl.initializer) {
        TypeRef initType = analyzeExpr(decl.initializer.get());
        if (initType && initType->kind == TypeKindSem::Unit) {
            error(decl.initializer->loc,
                  "Unit literal cannot be stored; use null for optional values or omit the value "
                  "in a void context");
            initType = types::unknown();
        }
        initType =
            contextualizeIntegerLiteralForDeclaredType(fieldType, initType, decl.initializer.get());
        if (fieldType && initType && initType->kind != TypeKindSem::Unknown &&
            !fieldType->isAssignableFrom(*initType)) {
            errorTypeMismatch(decl.initializer->loc, fieldType, initType);
        }
    }

    // Store field type and visibility for access checking
    if (ownerType) {
        std::string fieldKey = ownerType->name + "." + decl.name;
        if (fieldTypes_.find(fieldKey) == fieldTypes_.end()) {
            fieldTypes_[fieldKey] = fieldType;
            memberVisibility_[fieldKey] = decl.visibility;
        }
        if (decl.isFinal)
            finalFields_.insert(fieldKey);
    }

    Symbol sym;
    sym.kind = Symbol::Kind::Field;
    sym.name = decl.name;
    sym.type = fieldType;
    sym.isFinal = decl.isFinal;
    sym.decl = &decl;
    defineSymbol(decl.name, sym);
}

/// @brief Analyze property accessor bodies under their owning type context.
/// @param decl Property declaration with optional getter and setter bodies.
/// @param ownerType Semantic class or struct type that owns the property.
/// @details Defines `self` for instance accessors and the configured setter parameter for
///          setters, then validates getter control flow against the property type.
void Sema::analyzePropertyDecl(PropertyDecl &decl, TypeRef ownerType) {
    TypeRef propType = decl.type ? resolveTypeNode(decl.type.get()) : types::unknown();

    /// @brief Analyzes one property accessor under its owning type context.
    /// @param body Accessor body.
    /// @param returnType Expected accessor return type.
    /// @param defineSetterParam Whether to bind the setter value parameter.
    auto analyzeBody = [&](Stmt *body, TypeRef returnType, bool defineSetterParam) {
        if (!body)
            return;

        TypeRef savedSelfType = currentSelfType_;
        TypeRef savedExpectedReturnType = expectedReturnType_;

        currentSelfType_ = ownerType;
        expectedReturnType_ = returnType;

        pushScope(decl.loc);

        if (!decl.isStatic) {
            Symbol selfSym;
            selfSym.kind = Symbol::Kind::Parameter;
            selfSym.name = "self";
            selfSym.type = ownerType;
            selfSym.isFinal = true;
            defineSymbol("self", selfSym, decl.loc);
            markInitialized("self");
        }

        if (defineSetterParam) {
            Symbol paramSym;
            paramSym.kind = Symbol::Kind::Parameter;
            paramSym.name = decl.setterParam;
            paramSym.type = propType;
            paramSym.isFinal = true;
            defineSymbol(decl.setterParam, paramSym, decl.loc);
            markInitialized(decl.setterParam);
        }

        analyzeStmt(body);

        if (returnType && returnType->kind != TypeKindSem::Void && !stmtAlwaysExits(body)) {
            warn(WarningCode::W008_MissingReturn,
                 decl.loc,
                 "Property '" + decl.name + "' may not return a value on all code paths");
        }

        popScope(scopeEndForStmt(body));
        expectedReturnType_ = savedExpectedReturnType;
        currentSelfType_ = savedSelfType;
    };

    analyzeBody(decl.getterBody.get(), propType, false);
    if (decl.setterBody)
        analyzeBody(decl.setterBody.get(), types::voidType(), true);
}

/// @brief Find a property declaration visible from an owner for lowering.
/// @param ownerName Semantic class or struct owner where lookup begins.
/// @param propertyName Property identifier.
/// @param declaringOwner Optional destination for the actual declaring type.
/// @return Direct or inherited property declaration, or nullptr when absent.
/// @details Class lookup follows the base-class chain; struct lookup examines only the named
///          struct because structs do not inherit.
const PropertyDecl *Sema::propertyDeclForLowering(const std::string &ownerName,
                                                  const std::string &propertyName,
                                                  std::string *declaringOwner) const {
    /// @brief Finds the named property among one type declaration's members.
    /// @param typeDecl Class or struct declaration to scan.
    /// @return Matching property declaration, or null.
    auto scanMembers = [&](const auto *typeDecl) -> const PropertyDecl * {
        if (!typeDecl)
            return nullptr;
        for (const auto &member : typeDecl->members) {
            if (member->kind != DeclKind::Property)
                continue;
            auto *prop = static_cast<PropertyDecl *>(member.get());
            if (prop->name == propertyName)
                return prop;
        }
        return nullptr;
    };

    auto classIt = classDecls_.find(ownerName);
    if (classIt != classDecls_.end()) {
        if (const PropertyDecl *prop = scanMembers(classIt->second)) {
            if (declaringOwner)
                *declaringOwner = ownerName;
            return prop;
        }
        if (!classIt->second->baseClass.empty())
            return propertyDeclForLowering(
                classIt->second->baseClass, propertyName, declaringOwner);
        return nullptr;
    }

    auto structIt = structDecls_.find(ownerName);
    if (structIt != structDecls_.end()) {
        if (const PropertyDecl *prop = scanMembers(structIt->second)) {
            if (declaringOwner)
                *declaringOwner = ownerName;
            return prop;
        }
    }

    return nullptr;
}

/// @brief Find a visible property declaration without requesting its declaring owner.
/// @param ownerName Semantic owner where lookup begins.
/// @param propertyName Property identifier.
/// @return Direct or inherited property declaration, or nullptr when absent.
const PropertyDecl *Sema::findPropertyDecl(const std::string &ownerName,
                                           const std::string &propertyName) const {
    return propertyDeclForLowering(ownerName, propertyName, nullptr);
}

/// @brief Analyze a destructor body with an implicit final `self` parameter.
/// @param decl Destructor declaration to analyze.
/// @param ownerType Semantic class type bound to `self`.
/// @details Destructors are checked in a void-return context and restore any enclosing method or
///          function state after their scope is popped.
void Sema::analyzeDestructorDecl(DestructorDecl &decl, TypeRef ownerType) {
    if (!decl.body)
        return;

    TypeRef savedSelfType = currentSelfType_;
    TypeRef savedExpectedReturnType = expectedReturnType_;
    FunctionDecl *savedFunction = currentFunction_;

    currentSelfType_ = ownerType;
    expectedReturnType_ = types::voidType();
    currentFunction_ = nullptr;

    pushScope(decl.loc);

    Symbol selfSym;
    selfSym.kind = Symbol::Kind::Parameter;
    selfSym.name = "self";
    selfSym.type = ownerType;
    selfSym.isFinal = true;
    defineSymbol("self", selfSym, decl.loc);
    markInitialized("self");

    analyzeStmt(decl.body.get());

    popScope(scopeEndForStmt(decl.body.get()));

    currentFunction_ = savedFunction;
    expectedReturnType_ = savedExpectedReturnType;
    currentSelfType_ = savedSelfType;
}

/// @brief Analyze a method declaration body (implicit self parameter, parameters, body).
/// @param decl The method declaration to analyze.
/// @param ownerType The type that owns this method, used as the type of 'self'.
void Sema::analyzeMethodDecl(MethodDecl &decl, TypeRef ownerType) {
    if (!decl.body && (!ownerType || ownerType->kind != TypeKindSem::Interface)) {
        error(decl.loc, "Method '" + decl.name + "' requires a body");
        return;
    }

    MethodDecl *savedMethod = currentMethod_;
    TypeRef savedReturnType = expectedReturnType_;
    TypeRef savedSelfType = currentSelfType_;
    currentMethod_ = &decl;
    currentSelfType_ = ownerType;
    auto methodTypeIt = methodDeclTypes_.find(&decl);
    TypeRef methodType =
        methodTypeIt != methodDeclTypes_.end() ? methodTypeIt->second : methodTypeForDecl(decl);
    TypeRef returnType = methodType && methodType->kind == TypeKindSem::Function
                             ? methodType->returnType()
                             : types::voidType();
    if (returnType && returnType->kind == TypeKindSem::Tuple) {
        error(decl.loc,
              "Methods cannot return a tuple type; return a struct with named fields, or use "
              "out-parameters");
        returnType = types::unknown();
    }
    expectedReturnType_ = returnType;

    // Register method type: "TypeName.methodName" -> function type
    std::string methodKey = ownerType->name + "." + decl.name;
    if (methodTypes_.find(methodKey) == methodTypes_.end()) {
        methodTypes_[methodKey] = methodType;
        memberVisibility_[methodKey] = decl.visibility;
    }

    pushScope(decl.loc);

    // Define 'self' parameter implicitly
    Symbol selfSym;
    selfSym.kind = Symbol::Kind::Parameter;
    selfSym.name = "self";
    selfSym.type = ownerType;
    selfSym.isFinal = true;
    defineSymbol("self", selfSym, decl.loc);
    markInitialized("self");

    // Define explicit parameters
    for (size_t i = 0; i < decl.params.size(); ++i) {
        const auto &param = decl.params[i];
        TypeRef paramType = types::unknown();
        if (methodType && methodType->kind == TypeKindSem::Function) {
            const auto &paramTypes = methodType->paramTypes();
            if (i < paramTypes.size())
                paramType = paramTypes[i];
        } else if (param.type) {
            paramType = resolveTypeNode(param.type.get());
        }
        if (isForbiddenValueType(paramType)) {
            error(param.loc,
                  "Type '" + forbiddenValueTypeName(paramType) +
                      "' cannot be used for a method parameter");
            paramType = types::unknown();
        }

        Symbol sym;
        sym.kind = Symbol::Kind::Parameter;
        sym.name = param.name;
        sym.type = paramType;
        sym.isFinal = true;
        defineSymbol(param.name, sym, param.loc);
        markInitialized(param.name);
    }

    // Analyze body
    if (decl.body) {
        analyzeStmt(decl.body.get());

        // W008: Missing return in non-void method
        if (expectedReturnType_ && expectedReturnType_->kind != TypeKindSem::Void) {
            if (!stmtAlwaysExits(decl.body.get())) {
                warn(WarningCode::W008_MissingReturn,
                     decl.loc,
                     "Method '" + decl.name + "' may not return a value on all code paths");
            }
        }
    }

    popScope(decl.body ? scopeEndForStmt(decl.body.get()) : decl.loc);

    expectedReturnType_ = savedReturnType;
    currentSelfType_ = savedSelfType;
    currentMethod_ = savedMethod;
}


} // namespace il::frontends::zia
