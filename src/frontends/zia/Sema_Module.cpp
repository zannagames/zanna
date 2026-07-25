//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Sema_Module.cpp
/// @brief Built-in symbol registration plus module, namespace, type-signature, and declaration-body
/// orchestration for Zia Sema.
///
/// @details This file was split out of Sema.cpp to keep semantic analysis
/// responsibilities navigable without changing the Sema public interface or
/// diagnostic behavior. Member functions remain declared in Sema.hpp.
///
/// @see frontends/zia/Sema.hpp
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Sema.hpp"

#include <map>
#include <utility>

namespace il::frontends::zia {

namespace {

/// @brief Return the surface parameter names for a function-like declaration.
/// @param params Parsed parameter list.
/// @return Parameter names in source order.
std::vector<std::string> paramNamesFor(const std::vector<Param> &params) {
    std::vector<std::string> names;
    names.reserve(params.size());
    for (const auto &param : params)
        names.push_back(param.name);
    return names;
}

/// @brief True if a declaration kind participates in module-scope name resolution.
/// @param kind AST declaration kind.
/// @return True for top-level declarations that can be exported/imported by name.
bool isTopLevelModuleScopedDeclKind(DeclKind kind) {
    switch (kind) {
        case DeclKind::Function:
        case DeclKind::Struct:
        case DeclKind::Class:
        case DeclKind::Interface:
        case DeclKind::Enum:
        case DeclKind::GlobalVar:
        case DeclKind::TypeAlias:
            return true;
        default:
            return false;
    }
}

/// @brief Extract the source-level name from a module-scoped declaration.
/// @param decl Declaration whose kind is accepted by isTopLevelModuleScopedDeclKind().
/// @return The declaration name, or an empty string for unsupported kinds.
std::string topLevelModuleScopedDeclName(const Decl &decl) {
    switch (decl.kind) {
        case DeclKind::Function:
            return static_cast<const FunctionDecl &>(decl).name;
        case DeclKind::Struct:
            return static_cast<const StructDecl &>(decl).name;
        case DeclKind::Class:
            return static_cast<const ClassDecl &>(decl).name;
        case DeclKind::Interface:
            return static_cast<const InterfaceDecl &>(decl).name;
        case DeclKind::Enum:
            return static_cast<const EnumDecl &>(decl).name;
        case DeclKind::GlobalVar:
            return static_cast<const GlobalVarDecl &>(decl).name;
        case DeclKind::TypeAlias:
            return static_cast<const TypeAliasDecl &>(decl).name;
        default:
            return "";
    }
}

} // namespace

//=============================================================================
// Built-in Functions
//=============================================================================

/// @brief Register built-in functions and runtime library functions.
/// @details Registers print, println, input, toString as built-in symbols,
///          then loads all Zanna.* runtime functions from runtime.def.
void Sema::registerBuiltins() {
    // print(String) -> Void
    {
        auto printType = types::function({types::string()}, types::voidType());
        Symbol sym;
        sym.kind = Symbol::Kind::Function;
        sym.name = "print";
        sym.type = printType;
        defineSymbol("print", sym);
    }

    // println(String) -> Void (alias for print with newline)
    {
        auto printlnType = types::function({types::string()}, types::voidType());
        Symbol sym;
        sym.kind = Symbol::Kind::Function;
        sym.name = "println";
        sym.type = printlnType;
        defineSymbol("println", sym);
    }

    // input() -> String
    {
        auto inputType = types::function({}, types::string());
        Symbol sym;
        sym.kind = Symbol::Kind::Function;
        sym.name = "input";
        sym.type = inputType;
        defineSymbol("input", sym);
    }

    // toString(Any) -> String
    {
        auto toStringType = types::function({types::any()}, types::string());
        Symbol sym;
        sym.kind = Symbol::Kind::Function;
        sym.name = "toString";
        sym.type = toStringType;
        defineSymbol("toString", sym);
    }

    // Register all Zanna.* runtime functions from runtime.def
    // Generated from src/il/runtime/runtime.def (1002 functions)
    initRuntimeFunctions();
}

//===----------------------------------------------------------------------===//
// Namespace Support
//===----------------------------------------------------------------------===//

/// @brief Precompute collision-safe semantic names for top-level declarations.
/// @param module Flattened module compilation unit.
/// @details Declarations with the same source spelling in different files are qualified by their
///          declared file-module name; unique spellings remain unqualified for compatibility.
void Sema::prepareModuleScopedTypeNames(const ModuleDecl &module) {
    semanticDeclNames_.clear();
    fileScopedDeclNames_.clear();

    if (module.loc.file_id != 0)
        fileModuleNames_[module.loc.file_id] = module.name;

    std::unordered_map<std::string, std::unordered_set<uint32_t>> filesByName;
    for (const auto &decl : module.declarations) {
        if (!decl || !isTopLevelModuleScopedDeclKind(decl->kind))
            continue;
        filesByName[topLevelModuleScopedDeclName(*decl)].insert(decl->loc.file_id);
    }

    for (const auto &decl : module.declarations) {
        if (!decl || !isTopLevelModuleScopedDeclKind(decl->kind))
            continue;

        const std::string shortName = topLevelModuleScopedDeclName(*decl);
        std::string semanticName = shortName;
        auto collisionIt = filesByName.find(shortName);
        if (collisionIt != filesByName.end() && collisionIt->second.size() > 1) {
            std::string moduleName = moduleNameForFile(decl->loc.file_id);
            if (!moduleName.empty())
                semanticName = moduleName + "." + shortName;
        }

        semanticDeclNames_[decl.get()] = semanticName;
        fileScopedDeclNames_[decl->loc.file_id][shortName] = semanticName;
    }
}

/// @brief Find the declared module name associated with a source file.
/// @param fileId Source manager file identifier.
/// @return Recorded module name, the current module's name when applicable, or an empty string.
std::string Sema::moduleNameForFile(uint32_t fileId) const {
    if (fileId == 0)
        return "";
    auto it = fileModuleNames_.find(fileId);
    if (it != fileModuleNames_.end())
        return it->second;
    if (currentModule_ && currentModule_->loc.file_id == fileId)
        return currentModule_->name;
    return "";
}

/// @brief Return the precomputed semantic name of a declaration.
/// @param decl Declaration whose collision-aware identity is requested.
/// @param name Unqualified fallback name.
/// @return Precomputed file-qualified name, or the namespace-qualified fallback.
std::string Sema::semanticNameForDecl(const Decl &decl, const std::string &name) const {
    auto it = semanticDeclNames_.find(&decl);
    if (it != semanticDeclNames_.end())
        return it->second;
    return qualifyName(name);
}

/// @brief Resolve a top-level spelling as seen from one source file.
/// @param fileId Source file containing the use.
/// @param name Unqualified declaration spelling.
/// @return Collision-safe semantic name, or @p name when no remapping exists.
std::string Sema::fileScopedDeclName(uint32_t fileId, const std::string &name) const {
    auto fileIt = fileScopedDeclNames_.find(fileId);
    if (fileIt == fileScopedDeclNames_.end())
        return name;
    auto nameIt = fileIt->second.find(name);
    return nameIt != fileIt->second.end() ? nameIt->second : name;
}

/// @brief Resolve a top-level type spelling as seen from one source file.
/// @param fileId Source file containing the type use.
/// @param name Unqualified type spelling.
/// @return Collision-safe semantic name, or @p name when no remapping exists.
/// @details Type and value declarations currently share the same file-scoped naming map.
std::string Sema::fileScopedTypeName(uint32_t fileId, const std::string &name) const {
    return fileScopedDeclName(fileId, name);
}

/// @brief Qualify a name with the current namespace prefix.
/// @param name The unqualified name.
/// @return The qualified name (prefix.name), or the original name if no namespace is active.
std::string Sema::qualifyName(const std::string &name) const {
    if (namespacePrefix_.empty())
        return name;
    return namespacePrefix_ + "." + name;
}

/// @brief Pass 2: Register member signatures (fields, methods) for type declarations.
/// @param declarations The declaration list to process.
void Sema::registerMemberSignatures(std::vector<DeclPtr> &declarations) {
    for (auto &decl : declarations) {
        switch (decl->kind) {
            case DeclKind::Struct:
                registerStructMembers(*static_cast<StructDecl *>(decl.get()));
                break;
            case DeclKind::Class:
                registerClassMembers(*static_cast<ClassDecl *>(decl.get()));
                break;
            case DeclKind::Interface:
                registerInterfaceMembers(*static_cast<InterfaceDecl *>(decl.get()));
                break;
            case DeclKind::Enum:
                analyzeEnumDecl(*static_cast<EnumDecl *>(decl.get()));
                break;
            default:
                break;
        }
    }
}

/// @brief Pre-pass: Eagerly resolve types of final constants from literal initializers.
/// @details Scans declarations for final globals with literal initializers and updates
///          the registered symbol type from unknown() to the concrete literal type.
///          This allows forward references to final constants in class/function bodies.
/// @param declarations The declaration list to process.
void Sema::registerFinalConstantTypes(std::vector<DeclPtr> &declarations) {
    for (auto &decl : declarations) {
        if (decl->kind == DeclKind::GlobalVar) {
            auto *gvar = static_cast<GlobalVarDecl *>(decl.get());
            if (!gvar->isFinal || !gvar->initializer)
                continue;

            // Look up the symbol — it was registered in Pass 1 with unknown type
            std::string name = semanticNameForDecl(*gvar, gvar->name);
            Symbol *sym = lookupSymbol(name);
            if (!sym || !sym->type->isUnknown())
                continue;

            // Infer type directly from literal initializer
            Expr *init = gvar->initializer.get();
            TypeRef inferredType = nullptr;
            if (dynamic_cast<IntLiteralExpr *>(init))
                inferredType = types::integer();
            else if (dynamic_cast<NumberLiteralExpr *>(init))
                inferredType = types::number();
            else if (dynamic_cast<BoolLiteralExpr *>(init))
                inferredType = types::boolean();
            else if (dynamic_cast<StringLiteralExpr *>(init))
                inferredType = types::string();
            else if (auto *unary = dynamic_cast<UnaryExpr *>(init)) {
                // Handle negated literals: final X = -42
                if (unary->op == UnaryOp::Neg) {
                    if (dynamic_cast<IntLiteralExpr *>(unary->operand.get()))
                        inferredType = types::integer();
                    else if (dynamic_cast<NumberLiteralExpr *>(unary->operand.get()))
                        inferredType = types::number();
                }
            }

            if (inferredType)
                sym->type = inferredType;
        } else if (decl->kind == DeclKind::Namespace) {
            // Recurse into namespace declarations
            auto *ns = static_cast<NamespaceDecl *>(decl.get());
            std::string savedPrefix = namespacePrefix_;
            if (namespacePrefix_.empty())
                namespacePrefix_ = ns->name;
            else
                namespacePrefix_ = namespacePrefix_ + "." + ns->name;

            registerFinalConstantTypes(ns->declarations);

            namespacePrefix_ = savedPrefix;
        }
    }
}

/// @brief Pass 3: Analyze declaration bodies (functions, types, globals).
/// @param declarations The declaration list to process.
void Sema::analyzeDeclarationBodies(std::vector<DeclPtr> &declarations) {
    for (auto &decl : declarations) {
        switch (decl->kind) {
            case DeclKind::Function:
                analyzeFunctionDecl(*static_cast<FunctionDecl *>(decl.get()));
                break;
            case DeclKind::Struct:
                analyzeStructDecl(*static_cast<StructDecl *>(decl.get()));
                break;
            case DeclKind::Class:
                analyzeClassDecl(*static_cast<ClassDecl *>(decl.get()));
                break;
            case DeclKind::Interface:
                analyzeInterfaceDecl(*static_cast<InterfaceDecl *>(decl.get()));
                break;
            case DeclKind::GlobalVar:
                analyzeGlobalVarDecl(*static_cast<GlobalVarDecl *>(decl.get()));
                break;
            default:
                break;
        }
    }
}

/// @brief Analyze a namespace declaration with recursive multi-pass processing.
/// @details Saves the current namespace prefix, computes a new qualified prefix,
///          then runs the same three-pass strategy (register, member sigs, bodies)
///          on the namespace's nested declarations. Handles nested namespaces recursively.
/// @param decl The namespace declaration to analyze.
void Sema::analyzeNamespaceDecl(NamespaceDecl &decl) {
    // Save current namespace prefix
    std::string savedPrefix = namespacePrefix_;

    // Compute new prefix: append this namespace's name
    if (namespacePrefix_.empty())
        namespacePrefix_ = decl.name;
    else
        namespacePrefix_ = namespacePrefix_ + "." + decl.name;

    std::vector<std::pair<TypeAliasDecl *, std::string>> pendingTypeAliases;

    for (auto &innerDecl : decl.declarations) {
        switch (innerDecl->kind) {
            case DeclKind::Struct: {
                auto *value = static_cast<StructDecl *>(innerDecl.get());
                registerTypeDeclarationSymbol(*value, qualifyName(value->name));
                break;
            }
            case DeclKind::Class: {
                auto *cls = static_cast<ClassDecl *>(innerDecl.get());
                registerTypeDeclarationSymbol(*cls, qualifyName(cls->name));
                break;
            }
            case DeclKind::Interface: {
                auto *iface = static_cast<InterfaceDecl *>(innerDecl.get());
                registerTypeDeclarationSymbol(*iface, qualifyName(iface->name));
                break;
            }
            case DeclKind::Enum: {
                auto *enumDecl = static_cast<EnumDecl *>(innerDecl.get());
                registerTypeDeclarationSymbol(*enumDecl, qualifyName(enumDecl->name));
                break;
            }
            case DeclKind::TypeAlias: {
                auto *alias = static_cast<TypeAliasDecl *>(innerDecl.get());
                std::string qualifiedName = qualifyName(alias->name);
                registerTypeAliasPlaceholder(*alias, qualifiedName);
                pendingTypeAliases.emplace_back(alias, qualifiedName);
                break;
            }
            default:
                break;
        }
    }

    resolvePendingTypeAliases(pendingTypeAliases);
    registerNominalTypeRelationships(decl.declarations);

    // Process declarations inside this namespace
    // First pass: register declarations
    for (auto &innerDecl : decl.declarations) {
        switch (innerDecl->kind) {
            case DeclKind::Function: {
                auto *func = static_cast<FunctionDecl *>(innerDecl.get());
                std::string qualifiedName = qualifyName(func->name);
                auto funcType = functionTypeForDecl(*func);

                Symbol sym;
                sym.kind = Symbol::Kind::Function;
                sym.name = qualifiedName;
                sym.type = funcType;
                sym.decl = func;
                sym.isExported = func->isExported;
                sym.paramNames = paramNamesFor(func->params);
                Symbol *existing = currentScope_->lookupLocal(qualifiedName);
                if (!existing) {
                    defineSymbol(qualifiedName, sym);
                } else if (existing->kind != Symbol::Kind::Function) {
                    reportDuplicateDefinition(qualifiedName, func->loc);
                }
                registerFunctionOverload(qualifiedName, func, funcType, func->loc);
                break;
            }
            case DeclKind::Struct: {
                auto *value = static_cast<StructDecl *>(innerDecl.get());
                std::string qualifiedName = qualifyName(value->name);
                registerTypeDeclarationSymbol(*value, qualifiedName);
                break;
            }
            case DeclKind::Class: {
                auto *cls = static_cast<ClassDecl *>(innerDecl.get());
                std::string qualifiedName = qualifyName(cls->name);
                registerTypeDeclarationSymbol(*cls, qualifiedName);
                break;
            }
            case DeclKind::Interface: {
                auto *iface = static_cast<InterfaceDecl *>(innerDecl.get());
                std::string qualifiedName = qualifyName(iface->name);
                registerTypeDeclarationSymbol(*iface, qualifiedName);
                break;
            }
            case DeclKind::Enum: {
                auto *enumDecl = static_cast<EnumDecl *>(innerDecl.get());
                std::string qualifiedName = qualifyName(enumDecl->name);
                registerTypeDeclarationSymbol(*enumDecl, qualifiedName);
                break;
            }
            case DeclKind::GlobalVar: {
                auto *gvar = static_cast<GlobalVarDecl *>(innerDecl.get());
                std::string qualifiedName = qualifyName(gvar->name);

                TypeRef varType;
                if (gvar->type)
                    varType = resolveTypeNode(gvar->type.get());
                else
                    varType = types::unknown();

                Symbol sym;
                sym.kind = Symbol::Kind::Variable;
                sym.name = qualifiedName;
                sym.type = varType;
                sym.isFinal = gvar->isFinal;
                sym.decl = gvar;
                sym.isExported = gvar->isExported;
                defineSymbol(qualifiedName, sym);
                break;
            }
            case DeclKind::TypeAlias: {
                break;
            }
            case DeclKind::Namespace: {
                auto *ns = static_cast<NamespaceDecl *>(innerDecl.get());
                std::string qualifiedName = qualifyName(ns->name);
                Symbol sym;
                sym.kind = Symbol::Kind::Module;
                sym.name = qualifiedName;
                sym.type = types::module(qualifiedName);
                sym.decl = ns;
                sym.isFinal = true;
                defineSymbol(qualifiedName, sym);
                analyzeNamespaceDecl(*ns);
                break;
            }
            default:
                break;
        }
    }

    // Pre-pass: resolve final constant types for forward references
    registerFinalConstantTypes(decl.declarations);

    // Second pass: register members for types
    registerMemberSignatures(decl.declarations);

    // Third pass: analyze bodies
    analyzeDeclarationBodies(decl.declarations);

    // Restore previous namespace prefix
    namespacePrefix_ = savedPrefix;
}

// initRuntimeFunctions() implementation moved to Sema_Runtime.cpp
} // namespace il::frontends::zia
