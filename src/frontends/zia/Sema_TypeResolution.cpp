//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Sema_TypeResolution.cpp
/// @brief Type resolution, extern function registration, and closure capture
/// collection for the Zia semantic analyzer.
///
/// @details This file implements:
/// - resolveNamedType: maps type names to semantic types (built-ins, registry,
///   binds, cross-module references)
/// - resolveTypeNode: resolves AST TypeNode trees to semantic TypeRefs
/// - defineExternFunction: registers runtime/extern functions in scope
/// - collectCaptures: collects captured variables from lambda bodies
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Sema.hpp"
#include "il/runtime/RuntimeNameMap.hpp"
#include <functional>
#include <set>
#include <string_view>

namespace il::frontends::zia {

namespace {

/// @brief Return true when @p name is a concrete runtime object namespace.
/// @details Runtime classes from the catalog are registered in Sema::typeRegistry_. Some
///          legacy runtime objects are still represented only by runtime.def functions such as
///          `Zanna.Game.Entity.New` and `Zanna.Game.Behavior.Update`. This helper recognizes
///          those object namespaces without treating broad modules like `Zanna.Game` or arbitrary
///          unknown `Zanna.*` names as types.
/// @param name Fully-qualified runtime namespace candidate.
/// @return True if the generated runtime name map has at least one direct member under @p name.
bool isKnownRuntimeObjectNamespace(std::string_view name) {
    if (name.rfind("Zanna.", 0) != 0)
        return false;

    std::string prefix(name);
    prefix.push_back('.');
    for (const auto &alias : il::runtime::kRuntimeNameAliases) {
        std::string_view canonical = alias.canonical;
        if (canonical.size() <= prefix.size() || canonical.compare(0, prefix.size(), prefix) != 0)
            continue;
        std::string_view remainder = canonical.substr(prefix.size());
        if (!remainder.empty() && remainder.find('.') == std::string_view::npos)
            return true;
    }
    return false;
}

/// @brief Return a canonical runtime type spelling for source-level compatibility aliases.
/// @details A few Zia examples use older concise names for runtime objects whose catalog name is
///          now nested. Keep those aliases explicit so unknown `Zanna.*` names are still rejected.
/// @param name Fully-qualified candidate type name.
/// @return Canonical runtime type name, or @p name when no alias applies.
std::string canonicalRuntimeTypeName(std::string_view name) {
    if (name == "Zanna.GUI.TreeNode")
        return "Zanna.GUI.TreeView.Node";
    return std::string(name);
}

} // namespace

//=============================================================================
// Type Resolution
//=============================================================================

/// @brief Resolve a type name to a semantic type.
/// @param name The (possibly qualified) type name as written in source.
/// @param useLoc Location of the reference (for file-scoping and diagnostics).
/// @return The resolved type, or nullptr if the name cannot be resolved.
/// @details Checks, in order: built-in scalars/collections (PascalCase and lowercase forms),
///          file-scoped names, accessible type symbols, the type registry, type aliases,
///          imported namespace symbols, and module-qualified references (`Module.Type`). Raw
///          `Ptr` is rejected with an error since it is not part of the Zia source surface.
TypeRef Sema::resolveNamedType(const std::string &name, SourceLoc useLoc) const {
    // Built-in types (accept both PascalCase and lowercase variants)
    if (name == "Integer" || name == "integer" || name == "Int" || name == "int")
        return types::integer();
    if (name == "Number" || name == "number" || name == "Float" || name == "float" ||
        name == "Double" || name == "double")
        return types::number();
    if (name == "Boolean" || name == "boolean" || name == "Bool" || name == "bool")
        return types::boolean();
    if (name == "String" || name == "string")
        return types::string();
    if (name == "Byte" || name == "byte")
        return types::byte();
    if (name == "Unit" || name == "unit")
        return types::unit();
    if (name == "Void" || name == "void")
        return types::voidType();
    if (name == "Error" || name == "error")
        return types::error();
    if (name == "Any" || name == "any")
        return types::any();
    if (name == "Never" || name == "never")
        return types::never();
    if (name == "Ptr" || name == "ptr" || name == "Zanna.Unsafe.Ptr") {
        const_cast<Sema *>(this)->error(
            useLoc, "Ptr is not part of the Zia source surface; use typed runtime classes or Any");
        return types::unknown();
    }

    // Built-in collection types (default element type is unknown for non-generic usage)
    if (name == "List")
        return types::list(types::unknown());
    if (name == "Set")
        return types::set(types::unknown());
    if (name == "Map")
        return types::map(types::string(), types::unknown());
    if (name == "Seq")
        return types::seqOf(types::unknown());
    if (name == "Queue")
        return types::runtimeClass("Zanna.Collections.Queue", {types::unknown()});
    if (name == "Stack")
        return types::runtimeClass("Zanna.Collections.Stack", {types::unknown()});
    if (name == "Deque")
        return types::runtimeClass("Zanna.Collections.Deque", {types::unknown()});
    if (name == "Bytes")
        return types::runtimeClass("Zanna.Collections.Bytes");
    if (name == "Zanna.Collections.List")
        return types::runtimeClass("Zanna.Collections.List", {types::unknown()});
    if (name == "Zanna.Collections.Seq")
        return types::seqOf(types::unknown());
    if (name == "Zanna.Collections.Queue")
        return types::runtimeClass("Zanna.Collections.Queue", {types::unknown()});
    if (name == "Zanna.Collections.Stack")
        return types::runtimeClass("Zanna.Collections.Stack", {types::unknown()});
    if (name == "Zanna.Collections.Deque")
        return types::runtimeClass("Zanna.Collections.Deque", {types::unknown()});
    if (name == "Zanna.Collections.Ring")
        return types::runtimeClass("Zanna.Collections.Ring", {types::unknown()});
    if (name == "Zanna.Collections.Heap")
        return types::runtimeClass("Zanna.Collections.Heap", {types::unknown()});
    if (name == "Zanna.Collections.Map" || name == "Zanna.Collections.OrderedMap" ||
        name == "Zanna.Collections.SortedMap" || name == "Zanna.Collections.Trie" ||
        name == "Zanna.Collections.FrozenMap" || name == "Zanna.Collections.DefaultMap" ||
        name == "Zanna.Collections.WeakMap" || name == "Zanna.Collections.LruCache" ||
        name == "Zanna.Collections.MultiMap")
        return types::runtimeClass(name, {types::string(), types::unknown()});

    if (name.find('.') == std::string::npos && useLoc.file_id != 0) {
        const std::string scopedName = fileScopedTypeName(useLoc.file_id, name);
        if (scopedName != name) {
            if (Symbol *sym = const_cast<Sema *>(this)->lookupAccessibleSymbol(scopedName, useLoc);
                sym && sym->kind == Symbol::Kind::Type) {
                return sym->type;
            }
            auto scopedIt = typeRegistry_.find(scopedName);
            if (scopedIt != typeRegistry_.end())
                return scopedIt->second;
        }
    }

    if (Symbol *sym = const_cast<Sema *>(this)->lookupAccessibleSymbol(name, useLoc);
        sym && sym->kind == Symbol::Kind::Type) {
        return sym->type;
    }

    // Look up in registry
    auto it = typeRegistry_.find(name);
    if (it != typeRegistry_.end())
        return it->second;

    // Check type aliases (type Name = TargetType;)
    auto aliasIt = typeAliases_.find(name);
    if (aliasIt != typeAliases_.end())
        return aliasIt->second;

    auto lookupRegisteredType = [&](const std::string &candidate) -> TypeRef {
        auto typeIt = typeRegistry_.find(candidate);
        if (typeIt != typeRegistry_.end())
            return typeIt->second;
        auto alias = typeAliases_.find(candidate);
        if (alias != typeAliases_.end())
            return alias->second;
        return nullptr;
    };

    auto resolveBoundFileModuleType = [&](const std::string &moduleName,
                                          const std::string &suffix) -> TypeRef {
        uint32_t boundFileId = 0;
        if (useLoc.file_id != 0) {
            auto fileIt = fileBoundModuleIds_.find(useLoc.file_id);
            if (fileIt != fileBoundModuleIds_.end()) {
                auto moduleIt = fileIt->second.find(moduleName);
                if (moduleIt != fileIt->second.end())
                    boundFileId = moduleIt->second;
            }
        }
        if (boundFileId == 0) {
            auto moduleIt = boundFileModuleIds_.find(moduleName);
            if (moduleIt != boundFileModuleIds_.end())
                boundFileId = moduleIt->second;
        }
        if (boundFileId == 0)
            return nullptr;

        const std::string scopedSuffix = fileScopedTypeName(boundFileId, suffix);
        if (TypeRef resolved = lookupRegisteredType(scopedSuffix))
            return resolved;
        if (TypeRef resolved = lookupRegisteredType(suffix))
            return resolved;

        auto itemDot = suffix.find('.');
        if (itemDot != std::string::npos) {
            const std::string firstItem = suffix.substr(0, itemDot);
            const std::string remaining = suffix.substr(itemDot + 1);
            const std::string scopedFirst = fileScopedTypeName(boundFileId, firstItem);
            if (TypeRef resolved = lookupRegisteredType(scopedFirst + "." + remaining))
                return resolved;
        }

        return nullptr;
    };

    // Check if this is an imported type from a bound namespace
    // e.g., "Canvas" imported from "Zanna.Graphics"
    auto importIt = importedSymbols_.find(name);
    if (importIt != importedSymbols_.end()) {
        const std::string &fullName = importIt->second;

        // Check if the imported type is a built-in collection type
        if (fullName == "Zanna.Collections.List")
            return types::list(types::unknown());
        if (fullName == "Zanna.Collections.Set")
            return types::set(types::unknown());
        if (fullName == "Zanna.Collections.Map")
            return types::map(types::string(), types::unknown());

        // Look up the full qualified name in the registry
        std::string canonicalFullName = canonicalRuntimeTypeName(fullName);
        it = typeRegistry_.find(canonicalFullName);
        if (it != typeRegistry_.end())
            return it->second;

        // Runtime classes must be registered in typeRegistry_. Legacy runtime object
        // namespaces backed by runtime.def direct members remain valid object types, but
        // broad namespaces and functions are not valid type names.
        if (canonicalFullName.rfind("Zanna.", 0) == 0) {
            if (isKnownRuntimeObjectNamespace(canonicalFullName))
                return types::runtimeClass(canonicalFullName);
            return nullptr;
        }
    }

    // Handle qualified type references (e.g., "token.Token", "Mod.Ns.Type")
    auto dotPos = name.find('.');
    if (dotPos != std::string::npos) {
        std::string prefix = name.substr(0, dotPos);
        std::string suffix = name.substr(dotPos + 1);

        auto moduleExports = findModuleExports(prefix, useLoc);
        if (moduleExports) {
            std::string firstItem = suffix;
            std::string remaining;
            auto itemDot = suffix.find('.');
            if (itemDot != std::string::npos) {
                firstItem = suffix.substr(0, itemDot);
                remaining = suffix.substr(itemDot + 1);
            }

            auto exportIt = moduleExports->find(firstItem);
            if (exportIt != moduleExports->end()) {
                const Symbol &exportSym = exportIt->second;
                if (!remaining.empty() && exportSym.kind == Symbol::Kind::Module && exportSym.type)
                    return resolveNamedType(exportSym.type->name + "." + remaining, useLoc);
                if (remaining.empty() && exportSym.kind == Symbol::Kind::Type)
                    return exportSym.type;
                if (remaining.empty() && exportSym.kind == Symbol::Kind::Module)
                    return exportSym.type;
                return nullptr;
            }
        }

        if (TypeRef resolved = resolveBoundFileModuleType(prefix, suffix))
            return resolved;

        auto fileModuleIdIt = useLoc.file_id != 0 ? fileBoundModuleIds_.find(useLoc.file_id)
                                                  : fileBoundModuleIds_.end();
        const bool visibleFileModule =
            moduleExports != nullptr ||
            boundFileModuleIds_.find(prefix) != boundFileModuleIds_.end() ||
            (fileModuleIdIt != fileBoundModuleIds_.end() &&
             fileModuleIdIt->second.find(prefix) != fileModuleIdIt->second.end());
        if (visibleFileModule && prefix != "Zanna") {
            if (TypeRef resolved = lookupRegisteredType(suffix))
                return resolved;
        }

        // Check if prefix is a namespace alias (e.g., GUI -> Zanna.GUI)
        auto prefixIt = importedSymbols_.find(prefix);
        if (prefixIt != importedSymbols_.end()) {
            std::string fullName = prefixIt->second + "." + suffix;
            std::string canonicalFullName = canonicalRuntimeTypeName(fullName);
            it = typeRegistry_.find(canonicalFullName);
            if (it != typeRegistry_.end())
                return it->second;
            if (canonicalFullName.rfind("Zanna.", 0) == 0) {
                if (isKnownRuntimeObjectNamespace(canonicalFullName))
                    return types::runtimeClass(canonicalFullName);
                return nullptr;
            }
        }

        // Look up the fully-qualified type name directly (used for namespaces).
        std::string canonicalName = canonicalRuntimeTypeName(name);
        it = typeRegistry_.find(canonicalName);
        if (it != typeRegistry_.end())
            return it->second;
        if (canonicalName.rfind("Zanna.", 0) == 0 && isKnownRuntimeObjectNamespace(canonicalName))
            return types::runtimeClass(canonicalName);

        // Backwards-compatible fallback only for spelling the current module prefix explicitly.
        if (currentModule_ && prefix == currentModule_->name) {
            it = typeRegistry_.find(suffix);
            if (it != typeRegistry_.end())
                return it->second;
        }
    }

    return nullptr;
}

/// @brief Resolve an AST type node tree to a semantic type.
/// @param node The type node (named, generic, optional, function, tuple, or fixed-array).
/// @return The resolved type; `unknown` on error or for a null node.
/// @details Recursion-guarded (kMaxTypeResolveDepth, 256). Named nodes resolve type parameters
///          first then delegate to resolveNamedType(); generic nodes build built-in collection
///          types or instantiate user generics; optional/function/tuple/fixed-array nodes
///          resolve their components. Unknown names are reported as errors.
TypeRef Sema::resolveTypeNode(const TypeNode *node) {
    if (!node)
        return types::unknown();

    if (++typeResolveDepth_ > kMaxTypeResolveDepth) {
        --typeResolveDepth_;
        error(node->loc, "type nesting too deep during resolution (limit: 256)");
        return types::unknown();
    }

    struct DepthGuard {
        unsigned &d;

        ~DepthGuard() {
            --d;
        }
    } typeGuard_{typeResolveDepth_};

    switch (node->kind) {
        case TypeKind::Named: {
            auto *named = static_cast<const NamedType *>(node);

            // Check if this is a type parameter in current generic context
            if (TypeRef substituted = lookupTypeParam(named->name))
                return substituted;

            TypeRef resolved = resolveNamedType(named->name, node->loc);
            if (!resolved) {
                error(node->loc, "Unknown type: " + named->name);
                return types::unknown();
            }
            return resolved;
        }

        case TypeKind::Generic: {
            auto *generic = static_cast<const GenericType *>(node);
            std::vector<TypeRef> args;
            for (const auto &arg : generic->args) {
                args.push_back(resolveTypeNode(arg.get()));
            }

            auto requireArity = [&](size_t expected) {
                if (args.size() == expected)
                    return true;
                error(node->loc,
                      "Type '" + generic->name + "' expects " + std::to_string(expected) +
                          " type argument" + (expected == 1 ? "" : "s") + ", got " +
                          std::to_string(args.size()));
                return false;
            };

            // Built-in generic types
            if (generic->name == "List") {
                if (!requireArity(1))
                    return types::unknown();
                return types::list(args.empty() ? types::unknown() : args[0]);
            }
            if (generic->name == "Set") {
                if (!requireArity(1))
                    return types::unknown();
                return types::set(args.empty() ? types::unknown() : args[0]);
            }
            if (generic->name == "Map") {
                if (!requireArity(2))
                    return types::unknown();
                TypeRef keyType = args.size() > 0 ? args[0] : types::unknown();
                TypeRef valueType = args.size() > 1 ? args[1] : types::unknown();
                if (keyType && keyType->kind != TypeKindSem::Unknown &&
                    keyType->kind != TypeKindSem::String && keyType->kind != TypeKindSem::Integer) {
                    error(node->loc, "Map keys must be String or Integer");
                }
                return types::map(keyType, valueType);
            }
            if (generic->name == "Result") {
                if (!requireArity(1))
                    return types::unknown();
                return types::result(args.empty() ? types::unit() : args[0]);
            }
            if (generic->name == "Seq") {
                if (!requireArity(1))
                    return types::unknown();
                return types::seqOf(args.empty() ? types::unknown() : args[0]);
            }
            if (generic->name == "Queue") {
                if (!requireArity(1))
                    return types::unknown();
                return types::runtimeClass("Zanna.Collections.Queue",
                                           {args.empty() ? types::unknown() : args[0]});
            }
            if (generic->name == "Stack") {
                if (!requireArity(1))
                    return types::unknown();
                return types::runtimeClass("Zanna.Collections.Stack",
                                           {args.empty() ? types::unknown() : args[0]});
            }
            if (generic->name == "Deque") {
                if (!requireArity(1))
                    return types::unknown();
                return types::runtimeClass("Zanna.Collections.Deque",
                                           {args.empty() ? types::unknown() : args[0]});
            }

            // User-defined generic type - check if registered for instantiation
            std::string genericName = generic->name;
            if (genericName.find('.') == std::string::npos && node->loc.file_id != 0)
                genericName = fileScopedTypeName(node->loc.file_id, genericName);
            if (genericTypeDecls_.count(genericName)) {
                return instantiateGenericType(genericName, args, node->loc);
            }

            // Fallback: resolve as named type with type arguments
            TypeRef baseType = resolveNamedType(genericName, node->loc);
            if (!baseType) {
                error(node->loc, "Unknown type: " + generic->name);
                return types::unknown();
            }

            // Create type with arguments (for built-in-like types)
            return std::make_shared<ZannaType>(baseType->kind, baseType->name, args);
        }

        case TypeKind::Optional: {
            auto *opt = static_cast<const OptionalType *>(node);
            TypeRef inner = resolveTypeNode(opt->inner.get());
            return types::optional(inner);
        }

        case TypeKind::Function: {
            auto *func = static_cast<const FunctionType *>(node);
            std::vector<TypeRef> params;
            for (const auto &param : func->params) {
                params.push_back(resolveTypeNode(param.get()));
            }
            TypeRef ret =
                func->returnType ? resolveTypeNode(func->returnType.get()) : types::voidType();
            return types::function(params, ret);
        }

        case TypeKind::Tuple: {
            const auto *tupleType = static_cast<const TupleType *>(node);
            std::vector<TypeRef> elementTypes;
            for (const auto &elem : tupleType->elements) {
                elementTypes.push_back(resolveType(elem.get()));
            }
            return types::tuple(std::move(elementTypes));
        }

        case TypeKind::FixedArray: {
            const auto *arr = static_cast<const FixedArrayType *>(node);
            TypeRef elemType = resolveTypeNode(arr->elementType.get());
            return types::fixedArray(elemType, arr->count);
        }
    }

    return types::unknown();
}

//=============================================================================
// Extern Function Registration
//=============================================================================

/// @brief Register a runtime/extern function as a symbol in the current scope.
/// @param name Function name.
/// @param returnType Return type.
/// @param paramTypes Parameter types.
/// @param paramNames Parameter names (inherited from a prior extern decl when empty).
/// @param pointerSafety Optional pointer-safety classification recorded for the function.
/// @param documentation Tooling documentation, inherited from a prior extern when empty.
/// @details The symbol is marked extern with no AST declaration; its function type is built
///          from the parameter and return types. A conflicting AST-backed extern signature is
///          diagnosed instead of silently replacing the declaration.
void Sema::defineExternFunction(const std::string &name,
                                TypeRef returnType,
                                const std::vector<TypeRef> &paramTypes,
                                const std::vector<std::string> &paramNames,
                                std::optional<RuntimePointerSafety> pointerSafety,
                                const std::string &documentation) {
    TypeRef externType = types::function(paramTypes, returnType);
    if (Symbol *existing = currentScope_->lookupLocal(name);
        existing && existing->decl && existing->isExtern &&
        existing->kind == Symbol::Kind::Function && existing->type &&
        !existing->type->equals(*externType)) {
        error(SourceLoc{}, "Conflicting runtime extern signature for '" + name + "'");
        return;
    }

    Symbol sym;
    sym.kind = Symbol::Kind::Function;
    sym.name = name;
    sym.type = externType;
    sym.isExtern = true;
    sym.decl = nullptr; // No AST declaration for extern functions
    if (!documentation.empty())
        sym.documentation = documentation;
    if (!paramNames.empty()) {
        sym.paramNames = paramNames;
    } else if (Symbol *existing = currentScope_->lookupLocal(name);
               existing && existing->isExtern && existing->kind == Symbol::Kind::Function) {
        sym.paramNames = existing->paramNames;
        if (sym.documentation.empty())
            sym.documentation = existing->documentation;
    }
    if (pointerSafety)
        runtimePointerSafety_[name] = std::move(*pointerSafety);
    defineSymbol(name, std::move(sym));
}

//=============================================================================
// Closure Capture Collection
//=============================================================================

/// @brief Collect the free variables a lambda body captures from its enclosing scope.
/// @param expr The lambda body expression.
/// @param lambdaLocals The lambda's own parameters/locals (not captured).
/// @param[out] captures Receives the captured variable list.
/// @details Seeds a CaptureContext whose innermost local scope is the lambda's own names, then
///          walks the body via collectExprCaptures().
void Sema::collectCaptures(const Expr *expr,
                           const std::set<std::string> &lambdaLocals,
                           std::vector<CapturedVar> &captures) {
    if (!expr)
        return;

    CaptureContext ctx{{}, {lambdaLocals}, captures};
    collectExprCaptures(ctx, expr);
}

/// @brief Record @p name as a capture if it names an outer variable/parameter.
/// @param ctx Mutable capture traversal state.
/// @param name Referenced identifier spelling.
/// @details Ignores names bound in any active local scope, and only captures symbols that are
///          variables or parameters; each name is captured at most once.
void Sema::recordCapture(CaptureContext &ctx, const std::string &name) {
    for (auto it = ctx.localScopes.rbegin(); it != ctx.localScopes.rend(); ++it) {
        if (it->find(name) != it->end())
            return;
    }
    Symbol *sym = lookupSymbol(name);
    if (!sym || (sym->kind != Symbol::Kind::Variable && sym->kind != Symbol::Kind::Parameter))
        return;
    if (!ctx.captured.insert(name).second)
        return;
    ctx.captures.push_back({name});
}

/// @brief Add a match pattern's binding names to the current capture-tracking scope.
/// @param ctx Mutable capture traversal state.
/// @param pattern Pattern whose binding nodes are collected.
/// @details Binding patterns introduce a local name (ignoring `Some`/`None` constructors);
///          tuple/constructor/or patterns recurse into their subpatterns. This prevents
///          pattern-bound names from being mistaken for captures.
void Sema::collectPatternBindings(CaptureContext &ctx, const MatchArm::Pattern &pattern) {
    switch (pattern.kind) {
        case MatchArm::Pattern::Kind::Binding:
            if (pattern.binding != "Some" && pattern.binding != "None" &&
                !pattern.binding.empty()) {
                ctx.localScopes.back().insert(pattern.binding);
            }
            break;
        case MatchArm::Pattern::Kind::Tuple:
        case MatchArm::Pattern::Kind::Constructor:
        case MatchArm::Pattern::Kind::Or:
            for (const auto &subpattern : pattern.subpatterns)
                collectPatternBindings(ctx, subpattern);
            break;
        default:
            break;
    }
}

/// @brief Walk a statement to collect captured variables, tracking nested scopes.
/// @param ctx Mutable capture traversal state.
/// @param stmt Statement to traverse; null is ignored.
/// @details Pushes a fresh local scope for each block/loop/branch/handler so names declared
///          inside (loop variables, `var`s, catch bindings, pattern bindings) are treated as
///          locals rather than captures, and recurses into sub-statements/expressions.
void Sema::collectStmtCaptures(CaptureContext &ctx, const Stmt *stmt) {
    if (!stmt)
        return;

    switch (stmt->kind) {
        case StmtKind::Block: {
            auto *block = static_cast<const BlockStmt *>(stmt);
            ctx.localScopes.push_back({});
            for (const auto &inner : block->statements)
                collectStmtCaptures(ctx, inner.get());
            ctx.localScopes.pop_back();
            break;
        }
        case StmtKind::Expr:
            collectExprCaptures(ctx, static_cast<const ExprStmt *>(stmt)->expr.get());
            break;
        case StmtKind::Var: {
            auto *var = static_cast<const VarStmt *>(stmt);
            if (var->initializer)
                collectExprCaptures(ctx, var->initializer.get());
            ctx.localScopes.back().insert(var->name);
            if (var->isTupleDestructure) {
                if (!var->tupleNames.empty()) {
                    for (const auto &name : var->tupleNames)
                        ctx.localScopes.back().insert(name);
                } else if (!var->secondName.empty()) {
                    ctx.localScopes.back().insert(var->secondName);
                }
            }
            break;
        }
        case StmtKind::If: {
            auto *ifStmt = static_cast<const IfStmt *>(stmt);
            collectExprCaptures(ctx, ifStmt->condition.get());
            if (ifStmt->thenBranch) {
                ctx.localScopes.push_back({});
                collectStmtCaptures(ctx, ifStmt->thenBranch.get());
                ctx.localScopes.pop_back();
            }
            if (ifStmt->elseBranch) {
                ctx.localScopes.push_back({});
                collectStmtCaptures(ctx, ifStmt->elseBranch.get());
                ctx.localScopes.pop_back();
            }
            break;
        }
        case StmtKind::While: {
            auto *whileStmt = static_cast<const WhileStmt *>(stmt);
            collectExprCaptures(ctx, whileStmt->condition.get());
            ctx.localScopes.push_back({});
            collectStmtCaptures(ctx, whileStmt->body.get());
            ctx.localScopes.pop_back();
            break;
        }
        case StmtKind::For: {
            auto *forStmt = static_cast<const ForStmt *>(stmt);
            ctx.localScopes.push_back({});
            collectStmtCaptures(ctx, forStmt->init.get());
            collectExprCaptures(ctx, forStmt->condition.get());
            collectExprCaptures(ctx, forStmt->update.get());
            collectStmtCaptures(ctx, forStmt->body.get());
            ctx.localScopes.pop_back();
            break;
        }
        case StmtKind::ForIn: {
            auto *forIn = static_cast<const ForInStmt *>(stmt);
            ctx.localScopes.push_back({});
            collectExprCaptures(ctx, forIn->iterable.get());
            ctx.localScopes.back().insert(forIn->variable);
            if (forIn->isTuple && !forIn->secondVariable.empty())
                ctx.localScopes.back().insert(forIn->secondVariable);
            collectStmtCaptures(ctx, forIn->body.get());
            ctx.localScopes.pop_back();
            break;
        }
        case StmtKind::Return:
            collectExprCaptures(ctx, static_cast<const ReturnStmt *>(stmt)->value.get());
            break;
        case StmtKind::Break:
        case StmtKind::Continue:
            break;
        case StmtKind::Defer: {
            auto *deferStmt = static_cast<const DeferStmt *>(stmt);
            ctx.localScopes.push_back({});
            collectStmtCaptures(ctx, deferStmt->action.get());
            ctx.localScopes.pop_back();
            break;
        }
        case StmtKind::Guard: {
            auto *guard = static_cast<const GuardStmt *>(stmt);
            collectExprCaptures(ctx, guard->condition.get());
            ctx.localScopes.push_back({});
            collectStmtCaptures(ctx, guard->elseBlock.get());
            ctx.localScopes.pop_back();
            break;
        }
        case StmtKind::Match: {
            auto *match = static_cast<const MatchStmt *>(stmt);
            collectExprCaptures(ctx, match->scrutinee.get());
            for (const auto &arm : match->arms) {
                ctx.localScopes.push_back({});
                collectPatternBindings(ctx, arm.pattern);
                collectExprCaptures(ctx, arm.pattern.guard.get());
                collectExprCaptures(ctx, arm.body.get());
                ctx.localScopes.pop_back();
            }
            break;
        }
        case StmtKind::Try: {
            auto *tryStmt = static_cast<const TryStmt *>(stmt);
            ctx.localScopes.push_back({});
            collectStmtCaptures(ctx, tryStmt->tryBody.get());
            ctx.localScopes.pop_back();
            for (const auto &catchClause : tryStmt->catches) {
                ctx.localScopes.push_back({});
                if (!catchClause.var.empty())
                    ctx.localScopes.back().insert(catchClause.var);
                collectStmtCaptures(ctx, catchClause.body.get());
                ctx.localScopes.pop_back();
            }
            if (tryStmt->finallyBody) {
                ctx.localScopes.push_back({});
                collectStmtCaptures(ctx, tryStmt->finallyBody.get());
                ctx.localScopes.pop_back();
            }
            break;
        }
        case StmtKind::Throw:
            collectExprCaptures(ctx, static_cast<const ThrowStmt *>(stmt)->value.get());
            break;
    }
}

/// @brief Walk an expression to collect captured variables.
/// @param ctx Mutable capture traversal state.
/// @param e Expression to traverse; null is ignored.
/// @details Identifier references are recorded via recordCapture(); compound expressions
///          recurse into their sub-expressions, and block/match expressions push local scopes.
///          Nested lambdas are not descended into — they collect their own captures separately.
void Sema::collectExprCaptures(CaptureContext &ctx, const Expr *e) {
    if (!e)
        return;

    switch (e->kind) {
        case ExprKind::Ident:
            recordCapture(ctx, static_cast<const IdentExpr *>(e)->name);
            break;
        case ExprKind::Binary: {
            auto *bin = static_cast<const BinaryExpr *>(e);
            // A lambda captures free variables by value, so assigning to a bare
            // captured name mutates a private copy and is almost always a bug.
            // (Mutating a captured object's field/index is fine — reference
            // semantics — and is not a bare-identifier target, so it is allowed.)
            if (bin->op == BinaryOp::Assign && bin->left &&
                bin->left->kind == ExprKind::Ident) {
                const std::string &tgt =
                    static_cast<const IdentExpr *>(bin->left.get())->name;
                bool isLocal = false;
                for (auto it = ctx.localScopes.rbegin(); it != ctx.localScopes.rend(); ++it) {
                    if (it->find(tgt) != it->end()) {
                        isLocal = true;
                        break;
                    }
                }
                if (!isLocal) {
                    Symbol *sym = lookupSymbol(tgt);
                    if (sym && (sym->kind == Symbol::Kind::Variable ||
                                sym->kind == Symbol::Kind::Parameter)) {
                        error(bin->loc,
                              "Cannot assign to captured variable '" + tgt +
                                  "'; lambda captures are by value. Return the new value, or use "
                                  "a class field for shared mutable state");
                    }
                }
            }
            collectExprCaptures(ctx, bin->left.get());
            collectExprCaptures(ctx, bin->right.get());
            break;
        }
        case ExprKind::Unary:
            collectExprCaptures(ctx, static_cast<const UnaryExpr *>(e)->operand.get());
            break;
        case ExprKind::Ternary: {
            auto *ternary = static_cast<const TernaryExpr *>(e);
            collectExprCaptures(ctx, ternary->condition.get());
            collectExprCaptures(ctx, ternary->thenExpr.get());
            collectExprCaptures(ctx, ternary->elseExpr.get());
            break;
        }
        case ExprKind::Call: {
            auto *call = static_cast<const CallExpr *>(e);
            collectExprCaptures(ctx, call->callee.get());
            for (const auto &arg : call->args)
                collectExprCaptures(ctx, arg.value.get());
            break;
        }
        case ExprKind::Index: {
            auto *idx = static_cast<const IndexExpr *>(e);
            collectExprCaptures(ctx, idx->base.get());
            collectExprCaptures(ctx, idx->index.get());
            break;
        }
        case ExprKind::Field:
            collectExprCaptures(ctx, static_cast<const FieldExpr *>(e)->base.get());
            break;
        case ExprKind::OptionalChain:
            collectExprCaptures(ctx, static_cast<const OptionalChainExpr *>(e)->base.get());
            break;
        case ExprKind::Coalesce: {
            auto *coalesce = static_cast<const CoalesceExpr *>(e);
            collectExprCaptures(ctx, coalesce->left.get());
            collectExprCaptures(ctx, coalesce->right.get());
            break;
        }
        case ExprKind::Is:
            collectExprCaptures(ctx, static_cast<const IsExpr *>(e)->value.get());
            break;
        case ExprKind::As:
            collectExprCaptures(ctx, static_cast<const AsExpr *>(e)->value.get());
            break;
        case ExprKind::Range: {
            auto *range = static_cast<const RangeExpr *>(e);
            collectExprCaptures(ctx, range->start.get());
            collectExprCaptures(ctx, range->end.get());
            break;
        }
        case ExprKind::Try:
            collectExprCaptures(ctx, static_cast<const TryExpr *>(e)->operand.get());
            break;
        case ExprKind::ForceUnwrap:
            collectExprCaptures(ctx, static_cast<const ForceUnwrapExpr *>(e)->operand.get());
            break;
        case ExprKind::Await:
            collectExprCaptures(ctx, static_cast<const AwaitExpr *>(e)->operand.get());
            break;
        case ExprKind::New: {
            auto *created = static_cast<const NewExpr *>(e);
            for (const auto &arg : created->args)
                collectExprCaptures(ctx, arg.value.get());
            break;
        }
        case ExprKind::StructLiteral: {
            auto *literal = static_cast<const StructLiteralExpr *>(e);
            for (const auto &field : literal->fields)
                collectExprCaptures(ctx, field.value.get());
            break;
        }
        case ExprKind::Lambda:
            break;
        case ExprKind::ListLiteral: {
            auto *list = static_cast<const ListLiteralExpr *>(e);
            for (const auto &elem : list->elements)
                collectExprCaptures(ctx, elem.get());
            break;
        }
        case ExprKind::MapLiteral: {
            auto *map = static_cast<const MapLiteralExpr *>(e);
            for (const auto &entry : map->entries) {
                collectExprCaptures(ctx, entry.key.get());
                collectExprCaptures(ctx, entry.value.get());
            }
            break;
        }
        case ExprKind::SetLiteral: {
            auto *set = static_cast<const SetLiteralExpr *>(e);
            for (const auto &elem : set->elements)
                collectExprCaptures(ctx, elem.get());
            break;
        }
        case ExprKind::Tuple: {
            auto *tuple = static_cast<const TupleExpr *>(e);
            for (const auto &elem : tuple->elements)
                collectExprCaptures(ctx, elem.get());
            break;
        }
        case ExprKind::TupleIndex:
            collectExprCaptures(ctx, static_cast<const TupleIndexExpr *>(e)->tuple.get());
            break;
        case ExprKind::If: {
            auto *ifExpr = static_cast<const IfExpr *>(e);
            collectExprCaptures(ctx, ifExpr->condition.get());
            collectExprCaptures(ctx, ifExpr->thenBranch.get());
            collectExprCaptures(ctx, ifExpr->elseBranch.get());
            break;
        }
        case ExprKind::Match: {
            auto *match = static_cast<const MatchExpr *>(e);
            collectExprCaptures(ctx, match->scrutinee.get());
            for (const auto &arm : match->arms) {
                ctx.localScopes.push_back({});
                collectPatternBindings(ctx, arm.pattern);
                collectExprCaptures(ctx, arm.pattern.guard.get());
                collectExprCaptures(ctx, arm.body.get());
                ctx.localScopes.pop_back();
            }
            break;
        }
        case ExprKind::Block: {
            auto *block = static_cast<const BlockExpr *>(e);
            ctx.localScopes.push_back({});
            for (const auto &stmt : block->statements)
                collectStmtCaptures(ctx, stmt.get());
            collectExprCaptures(ctx, block->value.get());
            ctx.localScopes.pop_back();
            break;
        }
        default:
            break;
    }
}

} // namespace il::frontends::zia
