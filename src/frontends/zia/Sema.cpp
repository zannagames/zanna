//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Sema.cpp
/// @brief Implementation of Zia semantic analyzer.
///
/// @details This file implements the core Sema constructor, callable overload
/// resolution, runtime-pointer safety checks, declaration registration, and the
/// top-level analyze() pass sequence. Scope management, diagnostics, builtins,
/// namespace/module orchestration, and type narrowing live in focused companion
/// files.
///
/// Other Sema method groups are split into separate files:
/// - Sema_Generics.cpp: Generic type/function substitution and instantiation
/// - Sema_TypeResolution.cpp: Type resolution, extern functions, captures
/// - Sema_Runtime.cpp: Runtime function registration
/// - Sema_Decl.cpp: Declaration analysis (bind, struct, class, interface, etc.)
/// - Sema_Stmt.cpp: Statement analysis
/// - Sema_Expr.cpp: Expression analysis
/// - Sema_Scope.cpp: Scope stack, symbol visibility, and narrowing helpers
/// - Sema_Diagnostics.cpp: Coded diagnostics and warning policy handling
/// - Sema_Module.cpp: Builtins, namespaces, and declaration-body orchestration
///
/// @see Sema.hpp for the class interface
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Sema.hpp"
#include "frontends/zia/Types.hpp"
#include "frontends/zia/ZiaSupport.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <limits>
#include <map>
#include <sstream>

namespace il::frontends::zia {

namespace {

/// @brief Return parameter names in source order for callable symbols.
/// @param params Parsed parameter declarations.
/// @return Vector of parameter names suitable for named-argument binding.
std::vector<std::string> paramNamesFor(const std::vector<Param> &params) {
    std::vector<std::string> names;
    names.reserve(params.size());
    for (const auto &param : params)
        names.push_back(param.name);
    return names;
}

/// @brief Convert a semantic type spelling into a stable symbol-name fragment.
/// @param input Human-readable type or signature text.
/// @return A copy in which characters other than letters, digits, underscores, and periods
///         are replaced with underscores.
std::string sanitizeForSymbol(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '_' || ch == '.')
            out.push_back(ch);
        else
            out.push_back('_');
    }
    return out;
}

/// @brief Join semantic type spellings into an overload-mangling suffix.
/// @param types Parameter types in declaration order; null entries are represented by `?`.
/// @return Sanitized type keys separated by `__`, or `void` for an empty list.
std::string joinTypeKeys(const std::vector<TypeRef> &types) {
    std::string result;
    bool first = true;
    for (const auto &type : types) {
        if (!first)
            result += "__";
        first = false;
        result += sanitizeForSymbol(type ? type->toString() : "?");
    }
    if (result.empty())
        result = "void";
    return result;
}

/// @brief Count the parameters that must be supplied at a call site
///        (excludes those with a default value or the variadic param).
/// @param params Source declaration parameters.
/// @return Number of fixed parameters without default values.
size_t requiredParamCount(const std::vector<Param> &params) {
    size_t required = 0;
    for (const auto &param : params) {
        if (!param.defaultValue && !param.isVariadic)
            ++required;
    }
    return required;
}

/// @brief Return true when @p argCount can satisfy @p params by arity alone.
/// @details Defaulted fixed parameters can be omitted, and a final variadic parameter
///          accepts any number of additional source arguments.
/// @param params Source declaration parameters.
/// @param argCount Number of supplied source arguments.
/// @return True when the argument count is within the callable's permitted arity.
bool callArityMatches(const std::vector<Param> &params, size_t argCount) {
    const size_t required = requiredParamCount(params);
    const bool hasVariadic = !params.empty() && params.back().isVariadic;
    const size_t total = params.size();
    if (argCount < required)
        return false;
    if (!hasVariadic && argCount > total)
        return false;
    return true;
}

/// @brief Select the semantic parameter type that should receive one source argument.
/// @param functionType Function or method type whose parameter list mirrors @p params.
/// @param params Source declaration parameters, used to detect the variadic tail.
/// @param argIndex Source argument index being scored.
/// @return Fixed parameter type, variadic element type, or null when no target exists.
TypeRef parameterTypeForSourceArg(TypeRef functionType,
                                  const std::vector<Param> &params,
                                  size_t argIndex) {
    if (!functionType || functionType->kind != TypeKindSem::Function)
        return nullptr;
    const auto &paramTypes = functionType->paramTypes();
    const bool hasVariadic = !params.empty() && params.back().isVariadic;
    const size_t fixedCount = hasVariadic ? params.size() - 1 : params.size();
    if (argIndex < fixedCount && argIndex < paramTypes.size())
        return paramTypes[argIndex];
    if (hasVariadic && !paramTypes.empty()) {
        TypeRef listType = paramTypes.back();
        return listType ? listType->elementType() : nullptr;
    }
    return nullptr;
}

/// @brief Apply contextual typing for empty `{}` collection literals.
/// @param arg Expression being passed to a contextual target.
/// @param argType The type inferred without context.
/// @param paramType The expected target type, if any.
/// @return @p paramType for empty-brace Map/Set contexts; otherwise @p argType.
TypeRef contextualizeEmptyCollectionArg(const Expr *arg, TypeRef argType, TypeRef paramType) {
    if (!arg || !paramType || arg->kind != ExprKind::MapLiteral)
        return argType;
    const auto *map = static_cast<const MapLiteralExpr *>(arg);
    if (!map->entries.empty())
        return argType;
    if (paramType->kind == TypeKindSem::Set || paramType->kind == TypeKindSem::Map)
        return paramType;
    return argType;
}

/// @brief True if @p argType may be implicitly coerced to a runtime object
///        pointer at a call boundary (excludes void/optional/function/tuple/
///        fixed-array/unknown/never/type-param/module, which never coerce).
/// @param argType Candidate semantic argument type.
/// @return True when the value has a runtime representation that may cross an opaque object
///         pointer boundary.
bool canRuntimeObjectCoerce(TypeRef argType) {
    if (!argType)
        return false;

    switch (argType->kind) {
        case TypeKindSem::Void:
        case TypeKindSem::Optional:
        case TypeKindSem::Function:
        case TypeKindSem::Tuple:
        case TypeKindSem::FixedArray:
        case TypeKindSem::Unknown:
        case TypeKindSem::Never:
        case TypeKindSem::TypeParam:
        case TypeKindSem::Module:
            return false;
        default:
            return true;
    }
}

/// @brief Heuristic: does parameter @p name look like a callback slot
///        (fn/func/handler/callback/predicate/comparator/…)? Used to permit
///        a function-pointer argument where the signature is otherwise opaque.
/// @param name Runtime parameter name to classify case-insensitively.
/// @return True when the name denotes a recognized callback role.
bool allowsFunctionPointerParam(std::string_view name) {
    std::string lower;
    lower.reserve(name.size());
    for (char ch : name)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));

    return lower == "fn" || lower == "func" || lower == "entry" || lower == "callback" ||
           lower == "handler" || lower == "pred" || lower == "predicate" || lower == "transform" ||
           lower == "mapper" || lower == "reducer" || lower == "action" || lower == "compare" ||
           lower == "comparator" || lower.find("callback") != std::string::npos ||
           lower.find("handler") != std::string::npos || lower.find("entry") != std::string::npos ||
           lower.find("predicate") != std::string::npos ||
           lower.find("transform") != std::string::npos ||
           lower.find("mapper") != std::string::npos ||
           lower.find("reducer") != std::string::npos ||
           lower.find("comparator") != std::string::npos;
}

/// @brief True if @p type is safe to pass across the function bridge: bare
///        function types are rejected, and an unnamed (opaque) pointer is
///        rejected; everything else is allowed.
/// @param type Payload type to validate; a null type is treated as unconstrained.
/// @return True when the payload does not expose an untyped pointer or nested function value.
bool isSafeFunctionBridgePayload(TypeRef type) {
    if (!type)
        return true;
    if (type->kind == TypeKindSem::Function)
        return false;
    if (type->kind == TypeKindSem::Ptr)
        return !type->name.empty();
    return true;
}

/// @brief Suggest a memory-safe replacement for a runtime call that returns a
///        raw pointer/buffer, for use in the
///        diagnostic when such a call is rejected. Empty if none is known.
/// @param calleeName Fully qualified runtime function name.
/// @return Suggested safe API or language construct, or an empty string when no tailored
///         alternative is registered.
std::string saferRuntimePointerAlternative(std::string_view calleeName) {
    static const std::unordered_map<std::string_view, std::string_view> alternatives = {
        {"Zanna.Core.Parse.TryInt", "Zanna.Core.Parse.IntOr"},
        {"Zanna.Core.Parse.TryDouble", "Zanna.Core.Parse.DoubleOr"},
        {"Zanna.Core.Parse.TryBool", "Zanna.Core.Parse.BoolOr"},
        {"Zanna.Threads.Pool.Submit",
         "Zanna.Threads.Thread.Start or Zanna.Threads.Async.Run for managed Zia callbacks"},
        {"Zanna.Core.MessageBus.Callback",
         "a managed callback returned by Zanna.Core.MessageBus.Callback(&handler)"},
        {"Zanna.Option.Map", "ordinary Zia optional control flow or a managed function reference"},
        {"Zanna.Option.AndThen",
         "ordinary Zia optional control flow or a managed function reference"},
        {"Zanna.Option.OrElse",
         "ordinary Zia optional control flow or a managed function reference"},
        {"Zanna.Option.Filter",
         "ordinary Zia optional control flow or a managed function reference"},
        {"Zanna.Result.Map", "ordinary Zia result control flow or a managed function reference"},
        {"Zanna.Result.MapErr", "ordinary Zia result control flow or a managed function reference"},
        {"Zanna.Result.AndThen",
         "ordinary Zia result control flow or a managed function reference"},
        {"Zanna.Result.OrElse", "ordinary Zia result control flow or a managed function reference"},
    };

    auto it = alternatives.find(calleeName);
    return it == alternatives.end() ? std::string{} : std::string(it->second);
}

/// @brief Score how well @p argType converts to @p paramType for overload
///        ranking: 0 = exact, small positive = an implicit conversion
///        (optional-wrap, runtime-object coercion, …), 1000 = incompatible.
///        Lower is preferred; @ref resolveFunctionOverload sums these.
/// @param paramType Target parameter type.
/// @param argType Source argument type.
/// @param allowRuntimeObjectCoercion Whether opaque runtime pointer parameters may accept
///        representable Zia values.
/// @return Nonnegative ranking cost, with 1000 denoting an invalid conversion.
int conversionCost(TypeRef paramType, TypeRef argType, bool allowRuntimeObjectCoercion) {
    if (!paramType || !argType)
        return 1000;
    if (paramType->equals(*argType))
        return 0;
    if (allowRuntimeObjectCoercion && paramType->kind == TypeKindSem::Ptr &&
        canRuntimeObjectCoerce(argType)) {
        return 2;
    }
    if (paramType->kind == TypeKindSem::Optional) {
        if (argType->kind == TypeKindSem::Optional)
            return paramType->innerType() && argType->innerType() &&
                           paramType->innerType()->equals(*argType->innerType())
                       ? 1
                       : (paramType->isAssignableFrom(*argType) ? 3 : 1000);
        if (paramType->isAssignableFrom(*argType))
            return 2;
    }
    if (paramType->kind == TypeKindSem::Number && argType->kind == TypeKindSem::Integer)
        return 2;
    if (paramType->kind == TypeKindSem::Integer && argType->kind == TypeKindSem::Byte)
        return 2;
    if (paramType->kind == TypeKindSem::Number && argType->kind == TypeKindSem::Byte)
        return 3;
    if (paramType->isAssignableFrom(*argType))
        return 4;
    return 1000;
}

} // namespace

//=============================================================================
// Sema Implementation
//=============================================================================

/// @brief Resolve the declaring owner for a field visible from a type.
std::optional<Sema::FieldResolution> Sema::resolveFieldEntry(const std::string &typeName,
                                                             const std::string &fieldName) const {
    // Fast path: a directly-declared field needs no base-class walk and no
    // cycle-detection allocation (the common case for structs and own fields).
    auto it = fieldTypes_.find(typeName + "." + fieldName);
    if (it != fieldTypes_.end())
        return FieldResolution{typeName, it->second};

    auto classIt = classDecls_.find(typeName);
    if (classIt == classDecls_.end() || !classIt->second || classIt->second->baseClass.empty())
        return std::nullopt;

    // Slow path: walk base classes, guarding against inheritance cycles.
    std::unordered_set<std::string> visited;
    visited.insert(typeName);
    std::string current = classIt->second->baseClass;
    while (!current.empty() && visited.insert(current).second) {
        it = fieldTypes_.find(current + "." + fieldName);
        if (it != fieldTypes_.end())
            return FieldResolution{current, it->second};

        classIt = classDecls_.find(current);
        if (classIt == classDecls_.end() || !classIt->second || classIt->second->baseClass.empty())
            break;
        current = classIt->second->baseClass;
    }

    return std::nullopt;
}

/// @brief Find the declaring type for a visible field.
/// @param typeName Struct or class type from which lookup begins.
/// @param fieldName Field identifier to resolve.
/// @return Declaring owner name, or std::nullopt when the field is not visible in the hierarchy.
std::optional<std::string> Sema::findFieldOwner(const std::string &typeName,
                                                const std::string &fieldName) const {
    auto resolved = resolveFieldEntry(typeName, fieldName);
    if (!resolved)
        return std::nullopt;
    return resolved->owner;
}

/// @brief Resolve a field type visible from a type.
/// @param typeName The type whose member access is being analyzed.
/// @param fieldName The field name to find.
/// @return The field's semantic type, or nullptr when absent.
/// @details Delegates owner resolution to @ref findFieldOwner so inherited class fields are
///          found without rewriting their declaring-owner metadata.
TypeRef Sema::getFieldType(const std::string &typeName, const std::string &fieldName) const {
    auto resolved = resolveFieldEntry(typeName, fieldName);
    return resolved ? resolved->type : nullptr;
}

/// @brief Construct a semantic analyzer with a fresh root scope and builtin type state.
/// @param diag Diagnostic sink that receives all semantic errors and warnings.
/// @details Clears process-wide nominal relationship registries before builtins are registered,
///          so each analyzer starts from an isolated module-analysis state.
Sema::Sema(il::support::DiagnosticEngine &diag) : diag_(diag) {
    scopes_.push_back(std::make_unique<Scope>(nullptr, nextScopeId_, 0));
    currentScope_ = scopes_.back().get();
    narrowedTypes_.push_back({});
    scopeSnapshots_[nextScopeId_] = ScopeSnapshot{nextScopeId_, 0, 0, {}, {}};
    nextScopeId_++;
    types::clearInterfaceImplementations();
    types::clearClassInheritance();
    registerBuiltins();
}

/// @brief Build the callable semantic type declared by a top-level function.
/// @param decl Function declaration whose parameter and result annotations are resolved.
/// @return Function type with a `List[T]` variadic tail and a `Future[T]` result for async
///         declarations.
TypeRef Sema::functionTypeForDecl(const FunctionDecl &decl) const {
    TypeRef declaredReturn =
        decl.returnType ? resolveType(decl.returnType.get()) : types::voidType();
    TypeRef returnType = decl.isAsync ? types::futureOf(declaredReturn) : declaredReturn;
    std::vector<TypeRef> paramTypes;
    paramTypes.reserve(decl.params.size());
    for (const auto &param : decl.params) {
        TypeRef resolved = param.type ? resolveType(param.type.get()) : types::unknown();
        if (param.isVariadic)
            resolved = types::list(resolved); // ...Integer → List[Integer]
        paramTypes.push_back(resolved);
    }
    return types::function(paramTypes, returnType);
}

/// @brief Build the callable semantic type declared by a method.
/// @param decl Method declaration to resolve.
/// @return Function type containing the explicit method parameters and result.
/// @details Generic method parameters are temporarily installed as semantic type parameters
///          while annotations are resolved.
TypeRef Sema::methodTypeForDecl(const MethodDecl &decl) const {
    bool pushedParams = false;
    if (!decl.genericParams.empty()) {
        std::map<std::string, TypeRef> params;
        for (const auto &param : decl.genericParams)
            params[param] = types::typeParam(param);
        const_cast<Sema *>(this)->pushTypeParams(params);
        pushedParams = true;
    }

    TypeRef returnType = decl.returnType ? resolveType(decl.returnType.get()) : types::voidType();
    std::vector<TypeRef> paramTypes;
    paramTypes.reserve(decl.params.size());
    for (const auto &param : decl.params)
        paramTypes.push_back(param.type ? resolveType(param.type.get()) : types::unknown());

    if (pushedParams)
        const_cast<Sema *>(this)->popTypeParams();
    return types::function(paramTypes, returnType);
}

/// @brief Produce the source-signature identity for a function overload.
/// @param decl Function declaration to identify.
/// @return Function name followed by sanitized semantic parameter-type keys.
std::string Sema::functionSignatureKey(const FunctionDecl &decl) const {
    TypeRef funcType = functionTypeForDecl(decl);
    return decl.name + "#" + joinTypeKeys(funcType->paramTypes());
}

/// @brief Produce the source-signature identity for a method overload.
/// @param decl Method declaration whose type is resolved for the key.
/// @return Method identity including static/instance status and parameter types.
std::string Sema::methodSignatureKey(const MethodDecl &decl) const {
    TypeRef methodType = methodTypeForDecl(decl);
    return methodSignatureKey(decl, methodType);
}

/// @brief Produce a method signature key from an already resolved callable type.
/// @param decl Method declaration supplying its name and static/instance status.
/// @param methodType Resolved function type, or null to use an empty parameter list.
/// @return Stable method-overload identity used by semantic registries.
std::string Sema::methodSignatureKey(const MethodDecl &decl, TypeRef methodType) const {
    const auto paramTypes = methodType && methodType->kind == TypeKindSem::Function
                                ? methodType->paramTypes()
                                : std::vector<TypeRef>{};
    return decl.name + "#" + (decl.isStatic ? "static#" : "inst#") + joinTypeKeys(paramTypes);
}

/// @brief Produce the dispatch identity for a method declaration.
/// @param decl Method declaration to resolve and identify.
/// @return Dispatch key currently equivalent to the semantic signature key.
std::string Sema::methodDispatchKey(const MethodDecl &decl) const {
    TypeRef methodType = methodTypeForDecl(decl);
    return methodDispatchKey(decl, methodType);
}

/// @brief Produce a dispatch identity from a pre-resolved method type.
/// @param decl Method declaration supplying source identity.
/// @param methodType Resolved callable type.
/// @return Dispatch key used for override and lowered-name bookkeeping.
std::string Sema::methodDispatchKey(const MethodDecl &decl, TypeRef methodType) const {
    return methodSignatureKey(decl, methodType);
}

/// @brief Register a top-level function in its overload family and assign its lowered name.
/// @param name Semantic function name, including module qualification when applicable.
/// @param decl Function declaration to register.
/// @param funcType Resolved callable type for @p decl.
/// @param loc Location used for duplicate or invalid-entry-point diagnostics.
/// @return True on registration; false for duplicate signatures or an overloaded `start`.
/// @details When a family gains its second overload, the first declaration is retroactively
///          remangled so every member has an unambiguous lowered symbol.
bool Sema::registerFunctionOverload(const std::string &name,
                                    FunctionDecl *decl,
                                    TypeRef funcType,
                                    SourceLoc loc) {
    auto &family = functionOverloads_[name];
    const bool isEntryStart =
        decl && decl->name == "start" && currentModule_ &&
        (currentModule_->loc.file_id == 0 || decl->loc.file_id == currentModule_->loc.file_id);
    if (isEntryStart) {
        if (!family.empty()) {
            error(loc, "Entry function 'start' cannot be overloaded");
            return false;
        }
    }

    const std::string sigKey = functionSignatureKey(*decl);
    for (auto *existing : family) {
        if (functionSignatureKey(*existing) == sigKey) {
            error(loc, "Duplicate definition of '" + name + "' with the same signature");
            return false;
        }
    }

    family.push_back(decl);
    functionDeclTypes_[decl] = funcType;

    auto overloadLoweredName = [&](FunctionDecl *fn) {
        auto typeIt = functionDeclTypes_.find(fn);
        ZANNA_ZIA_ASSERT(typeIt != functionDeclTypes_.end(),
                         "function type must be registered before building its lowered name");
        TypeRef type = typeIt->second;
        return name + "__ov__" + std::to_string(type->paramTypes().size()) + "__" +
               joinTypeKeys(type->paramTypes());
    };
    if (family.size() == 2) {
        FunctionDecl *first = family.front();
        const std::string oldName = loweredFunctionNames_[first];
        const std::string remangled = overloadLoweredName(first);
        loweredFunctionNames_[first] = remangled;
        if (auto it = functionDecls_.find(oldName);
            it != functionDecls_.end() && it->second == first)
            functionDecls_.erase(it);
        functionDecls_[remangled] = first;
        if (auto it = functionDecls_.find(name); it != functionDecls_.end() && it->second == first)
            functionDecls_.erase(it);
    }

    const bool overloaded = family.size() > 1;
    if (isEntryStart) {
        loweredFunctionNames_[decl] = "main";
    } else if (overloaded) {
        loweredFunctionNames_[decl] = overloadLoweredName(decl);
    } else {
        loweredFunctionNames_[decl] = name;
    }
    functionDecls_[loweredFunctionNames_[decl]] = decl;
    if (family.size() == 1)
        functionDecls_[name] = decl;
    return true;
}

/// @brief Register a method overload for one concrete owner type.
/// @param ownerType Semantic owner name.
/// @param decl Method declaration to register.
/// @param methodType Resolved callable type for this owner instance.
/// @param loc Location used for duplicate-signature diagnostics.
/// @return True when the method was registered, or false for a duplicate signature.
/// @details Owner-qualified caches preserve distinct instantiations of the same generic method
///          declaration and remangle the first member when a family becomes overloaded.
bool Sema::registerMethodOverload(const std::string &ownerType,
                                  MethodDecl *decl,
                                  TypeRef methodType,
                                  SourceLoc loc) {
    const std::string familyKey = ownerType + "." + decl->name;
    auto &family = methodOverloads_[familyKey];
    const MethodInstanceKey instanceKey{ownerType, decl};
    const std::string sigKey = methodSignatureKey(*decl, methodType);
    for (auto *existing : family) {
        // On a cache miss the lookup returns ""; recompute from the decl so a
        // genuine duplicate is still detected (mirrors the lookup+recompute
        // pattern used elsewhere) and an empty key is never a false match.
        std::string existingKey = cachedMethodSignatureKey(ownerType, existing);
        if (existingKey.empty())
            existingKey = methodSignatureKey(*existing);
        if (existingKey == sigKey) {
            error(loc,
                  "Duplicate definition of '" + decl->name + "' in type '" + ownerType +
                      "' with the same signature");
            return false;
        }
    }

    family.push_back(decl);
    ownerMethodTypes_[instanceKey] = methodType;
    ownerMethodSignatureKeys_[instanceKey] = sigKey;
    ownerMethodDispatchKeys_[instanceKey] = methodDispatchKey(*decl, methodType);
    methodDeclTypes_.try_emplace(decl, methodType);
    methodSignatureKeys_.try_emplace(decl, sigKey);
    methodDispatchKeys_.try_emplace(decl, methodDispatchKey(*decl, methodType));

    auto overloadLoweredMethodName = [&](MethodDecl *method) {
        MethodInstanceKey key{ownerType, method};
        auto typeIt = ownerMethodTypes_.find(key);
        ZANNA_ZIA_ASSERT(typeIt != ownerMethodTypes_.end(),
                         "method type must be registered before building its lowered name");
        TypeRef type = typeIt->second;
        return ownerType + "." + method->name + "__ov__" +
               std::to_string(type->paramTypes().size()) + "__" + joinTypeKeys(type->paramTypes());
    };

    if (family.size() == 2) {
        MethodDecl *first = family.front();
        MethodInstanceKey firstKey{ownerType, first};
        ownerLoweredMethodNames_[firstKey] = overloadLoweredMethodName(first);
        loweredMethodNames_[first] = ownerLoweredMethodNames_[firstKey];
    }

    if (family.size() > 1) {
        ownerLoweredMethodNames_[instanceKey] = ownerType + "." + decl->name + "__ov__" +
                                                std::to_string(methodType->paramTypes().size()) +
                                                "__" + joinTypeKeys(methodType->paramTypes());
    } else {
        ownerLoweredMethodNames_[instanceKey] = ownerType + "." + decl->name;
    }
    loweredMethodNames_[decl] = ownerLoweredMethodNames_[instanceKey];
    return true;
}

/// @brief Collect the visible overload family for a method name.
/// @param typeName Owner type where lookup starts.
/// @param methodName Source method name.
/// @param includeInherited Whether base-class families participate.
/// @return Declarations in nearest-owner-first order, deduplicated by semantic signature.
std::vector<MethodDecl *> Sema::collectMethodOverloads(const std::string &typeName,
                                                       const std::string &methodName,
                                                       bool includeInherited) const {
    std::vector<MethodDecl *> result;
    std::unordered_set<std::string> seenSignatures;

    auto addFamily = [&](const std::string &owner) {
        auto it = methodOverloads_.find(owner + "." + methodName);
        if (it == methodOverloads_.end())
            return;
        for (auto *method : it->second) {
            std::string sigKey = cachedMethodSignatureKey(owner, method);
            if (sigKey.empty())
                sigKey = methodSignatureKey(*method);
            if (seenSignatures.insert(sigKey).second)
                result.push_back(method);
        }
    };

    addFamily(typeName);
    if (!includeInherited)
        return result;

    auto classIt = lookupClassDeclForType(typeName);
    while (classIt && !classIt->baseClass.empty()) {
        const std::string &parentName = classIt->baseClass;
        addFamily(parentName);
        classIt = lookupClassDeclForType(parentName);
    }

    return result;
}

/// @brief Resolve a positional function call against an overload family.
/// @param name Semantic function name.
/// @param argTypes Argument types in source order.
/// @param loc Call location for access and ambiguity diagnostics.
/// @param loweredName Optional destination for the selected lowered symbol.
/// @param viaQualifiedModule Whether access occurs through an explicit module qualifier.
/// @return Best accessible declaration, or nullptr when no unique viable overload exists.
/// @details Candidates are ranked by conversion cost, with penalties for defaults and variadic
///          packing. Equal best scores produce an ambiguity diagnostic.
FunctionDecl *Sema::resolveFunctionOverload(const std::string &name,
                                            const std::vector<TypeRef> &argTypes,
                                            SourceLoc loc,
                                            std::string *loweredName,
                                            bool viaQualifiedModule) {
    auto it = functionOverloads_.find(name);
    if (it == functionOverloads_.end())
        return nullptr;

    FunctionDecl *best = nullptr;
    FunctionDecl *firstInaccessible = nullptr;
    int bestScore = std::numeric_limits<int>::max();
    bool ambiguous = false;
    std::vector<const FunctionDecl *> viableDecls;

    for (auto *decl : it->second) {
        // Fetch the declared type once via find(); operator[] would insert a
        // null entry for an unregistered decl and silently mask the omission.
        auto declTypeIt = functionDeclTypes_.find(decl);
        TypeRef funcType = declTypeIt != functionDeclTypes_.end() ? declTypeIt->second : nullptr;

        Symbol accessSym;
        accessSym.kind = Symbol::Kind::Function;
        accessSym.name = name;
        accessSym.type = funcType;
        accessSym.decl = decl;
        accessSym.loc = decl->loc;
        accessSym.isExported = decl->isExported;
        if (!canAccessSymbol(accessSym, loc, name, viaQualifiedModule)) {
            if (!firstInaccessible)
                firstInaccessible = decl;
            continue;
        }

        const auto &params = decl->params;
        if (!callArityMatches(params, argTypes.size()))
            continue;
        const bool hasVariadic = !params.empty() && params.back().isVariadic;
        const size_t fixedCount = hasVariadic ? params.size() - 1 : params.size();

        int score = 0;
        bool viable = true;
        for (size_t i = 0; i < argTypes.size(); ++i) {
            TypeRef targetType = parameterTypeForSourceArg(funcType, params, i);
            if (!targetType) {
                viable = false;
                break;
            }
            int cost = conversionCost(targetType, argTypes[i], false);
            if (cost >= 1000) {
                viable = false;
                break;
            }
            score += cost;
        }
        if (!viable)
            continue;
        if (fixedCount > argTypes.size())
            score += static_cast<int>(fixedCount - argTypes.size()) * 3;
        if (hasVariadic && argTypes.size() > fixedCount)
            score += static_cast<int>(argTypes.size() - fixedCount);
        viableDecls.push_back(decl);

        if (score < bestScore) {
            best = decl;
            bestScore = score;
            ambiguous = false;
        } else if (score == bestScore) {
            ambiguous = true;
        }
    }

    if (!best) {
        if (firstInaccessible) {
            auto inaccTypeIt = functionDeclTypes_.find(firstInaccessible);
            Symbol accessSym;
            accessSym.kind = Symbol::Kind::Function;
            accessSym.name = name;
            accessSym.type =
                inaccTypeIt != functionDeclTypes_.end() ? inaccTypeIt->second : nullptr;
            accessSym.decl = firstInaccessible;
            accessSym.loc = firstInaccessible->loc;
            accessSym.isExported = firstInaccessible->isExported;
            reportInaccessibleSymbol(loc, name, accessSym, viaQualifiedModule);
        }
        return nullptr;
    }
    if (ambiguous) {
        // Build candidate signature strings only on the (rare) ambiguous path.
        std::vector<std::string> candidateSigs;
        candidateSigs.reserve(viableDecls.size());
        for (auto *cand : viableDecls)
            candidateSigs.push_back(loweredFunctionName(cand));
        error(loc, "Ambiguous call to '" + name + "': " + formatOverloadCandidates(candidateSigs));
        return nullptr;
    }
    if (loweredName)
        *loweredName = loweredFunctionName(best);
    return best;
}

/// @brief Resolve a positional method call across an owner hierarchy.
/// @param ownerType Receiver's semantic type name.
/// @param methodName Source method name.
/// @param argTypes Argument types in source order.
/// @param loc Call location for ambiguity diagnostics.
/// @param resolvedOwnerType Optional destination for the owner that declares the selected method.
/// @param includeInherited Whether base-class overload families participate.
/// @return Best declaration, or nullptr when no unique viable candidate exists.
MethodDecl *Sema::resolveMethodOverload(const std::string &ownerType,
                                        const std::string &methodName,
                                        const std::vector<TypeRef> &argTypes,
                                        SourceLoc loc,
                                        std::string *resolvedOwnerType,
                                        bool includeInherited) {
    MethodDecl *best = nullptr;
    std::string bestOwner;
    int bestScore = std::numeric_limits<int>::max();
    bool ambiguous = false;
    std::vector<std::string> candidates;
    std::unordered_set<std::string> seenSignatures;

    auto considerOwner = [&](const std::string &candidateOwner) {
        auto it = methodOverloads_.find(candidateOwner + "." + methodName);
        if (it == methodOverloads_.end())
            return;
        for (auto *decl : it->second) {
            std::string sigKey = cachedMethodSignatureKey(candidateOwner, decl);
            if (sigKey.empty())
                sigKey = methodSignatureKey(*decl);
            if (!seenSignatures.insert(sigKey).second)
                continue;
            TypeRef methodType = getMethodType(candidateOwner, decl);
            if (!methodType)
                continue;
            const auto &params = decl->params;
            if (!callArityMatches(params, argTypes.size()))
                continue;
            const bool hasVariadic = !params.empty() && params.back().isVariadic;
            const size_t fixedCount = hasVariadic ? params.size() - 1 : params.size();
            int score = 0;
            bool viable = true;
            for (size_t i = 0; i < argTypes.size(); ++i) {
                TypeRef targetType = parameterTypeForSourceArg(methodType, params, i);
                if (!targetType) {
                    viable = false;
                    break;
                }
                int cost = conversionCost(targetType, argTypes[i], false);
                if (cost >= 1000) {
                    viable = false;
                    break;
                }
                score += cost;
            }
            if (!viable)
                continue;
            if (fixedCount > argTypes.size())
                score += static_cast<int>(fixedCount - argTypes.size()) * 3;
            if (hasVariadic && argTypes.size() > fixedCount)
                score += static_cast<int>(argTypes.size() - fixedCount);
            std::string lowered = loweredMethodName(candidateOwner, decl);
            candidates.push_back(lowered.empty() ? (candidateOwner + "." + decl->name) : lowered);
            if (score < bestScore) {
                best = decl;
                bestOwner = candidateOwner;
                bestScore = score;
                ambiguous = false;
            } else if (score == bestScore) {
                ambiguous = true;
            }
        }
    };

    considerOwner(ownerType);
    if (includeInherited) {
        auto classIt = lookupClassDeclForType(ownerType);
        while (classIt && !classIt->baseClass.empty()) {
            considerOwner(classIt->baseClass);
            classIt = lookupClassDeclForType(classIt->baseClass);
        }
    }

    if (!best)
        return nullptr;
    if (ambiguous) {
        error(loc,
              "Ambiguous call to method '" + methodName +
                  "': " + formatOverloadCandidates(candidates));
        return nullptr;
    }
    if (resolvedOwnerType)
        *resolvedOwnerType = bestOwner;
    return best;
}

/// @brief Pair source parameters with their resolved semantic types for call binding.
/// @param params Source parameter declarations.
/// @param paramTypes Resolved types in matching order.
/// @return Binding specifications carrying names, defaults, and variadic metadata.
std::vector<Sema::CallParamSpec> Sema::makeParamSpecs(
    const std::vector<Param> &params, const std::vector<TypeRef> &paramTypes) const {
    std::vector<CallParamSpec> specs;
    specs.reserve(params.size());
    for (size_t i = 0; i < params.size(); ++i) {
        CallParamSpec spec;
        spec.name = params[i].name;
        spec.type = i < paramTypes.size() ? paramTypes[i] : nullptr;
        spec.hasDefault = params[i].defaultValue != nullptr;
        spec.isVariadic = params[i].isVariadic;
        specs.push_back(std::move(spec));
    }
    return specs;
}

/// @brief Build binding specifications for a runtime or external function symbol.
/// @param sym Function symbol whose type and optional registry parameter names are used.
/// @param skipLeadingParams Number of implementation-only leading parameters hidden from Zia.
/// @return Exposed parameter specifications, or an empty list for a non-function symbol.
std::vector<Sema::CallParamSpec> Sema::makeExternParamSpecs(const Symbol &sym,
                                                            size_t skipLeadingParams) const {
    std::vector<CallParamSpec> specs;
    if (!sym.type || sym.type->kind != TypeKindSem::Function)
        return specs;

    const auto paramTypes = sym.type->paramTypes();
    if (skipLeadingParams >= paramTypes.size())
        return specs;

    const size_t paramCount = paramTypes.size() - skipLeadingParams;
    specs.reserve(paramCount);

    size_t nameOffset = 0;
    if (sym.paramNames.size() > paramTypes.size())
        nameOffset = sym.paramNames.size() - paramTypes.size();

    for (size_t i = skipLeadingParams; i < paramTypes.size(); ++i) {
        CallParamSpec spec;
        if (nameOffset + i < sym.paramNames.size() && !sym.paramNames[nameOffset + i].empty()) {
            spec.name = sym.paramNames[nameOffset + i];
        } else {
            spec.name = "arg" + std::to_string((i - skipLeadingParams) + 1);
        }
        spec.type = paramTypes[i];
        specs.push_back(std::move(spec));
    }

    return specs;
}

/// @brief Append constructor-style field parameters for a class hierarchy.
/// @param typeName Class whose inherited and directly declared instance fields are collected.
/// @param out Destination list; base-class fields are appended before derived fields.
/// @details Private fields outside the current self type receive an empty binding name so they
///          cannot be targeted by named arguments.
void Sema::appendClassFieldSpecs(const std::string &typeName,
                                 std::vector<CallParamSpec> &out) const {
    auto classIt = classDecls_.find(typeName);
    if (classIt == classDecls_.end())
        return;

    ClassDecl *decl = classIt->second;
    if (!decl->baseClass.empty())
        appendClassFieldSpecs(decl->baseClass, out);

    for (const auto &member : decl->members) {
        if (member->kind != DeclKind::Field)
            continue;
        auto *field = static_cast<FieldDecl *>(member.get());
        if (field->isStatic)
            continue;
        auto visIt = memberVisibility_.find(typeName + "." + field->name);
        const bool externallyPrivate = visIt != memberVisibility_.end() &&
                                       visIt->second == Visibility::Private &&
                                       (!currentSelfType_ || currentSelfType_->name != typeName);

        CallParamSpec spec;
        spec.name = externallyPrivate ? "" : field->name;
        spec.type = getFieldType(typeName, field->name);
        if (!spec.type && field->type)
            spec.type = resolveType(field->type.get());
        spec.hasDefault = true;
        out.push_back(std::move(spec));
    }
}

/// @brief Build constructor-style field parameters for a class.
/// @param typeName Semantic class name.
/// @return Inherited and directly declared instance-field specifications in layout order.
std::vector<Sema::CallParamSpec> Sema::makeClassFieldSpecs(const std::string &typeName) const {
    std::vector<CallParamSpec> specs;
    appendClassFieldSpecs(typeName, specs);
    return specs;
}

/// @brief Build constructor-style field parameters for a struct.
/// @param typeName Semantic struct name.
/// @return Non-static field specifications in declaration order, or an empty list if unknown.
std::vector<Sema::CallParamSpec> Sema::makeStructFieldSpecs(const std::string &typeName) const {
    std::vector<CallParamSpec> specs;
    auto structIt = structDecls_.find(typeName);
    if (structIt == structDecls_.end())
        return specs;

    for (const auto &member : structIt->second->members) {
        if (member->kind != DeclKind::Field)
            continue;
        auto *field = static_cast<FieldDecl *>(member.get());
        if (field->isStatic)
            continue;
        auto visIt = memberVisibility_.find(typeName + "." + field->name);
        const bool externallyPrivate = visIt != memberVisibility_.end() &&
                                       visIt->second == Visibility::Private &&
                                       (!currentSelfType_ || currentSelfType_->name != typeName);

        CallParamSpec spec;
        spec.name = externallyPrivate ? "" : field->name;
        spec.type = getFieldType(typeName, field->name);
        if (!spec.type && field->type)
            spec.type = resolveType(field->type.get());
        spec.hasDefault = true;
        specs.push_back(std::move(spec));
    }
    return specs;
}

/// @brief Bind positional and named call arguments to a callable parameter list.
/// @param args Source arguments in written order.
/// @param params Target parameter specifications.
/// @param loc Fallback call location for diagnostics.
/// @param calleeName Display name used in diagnostics.
/// @param binding Receives fixed-parameter source indexes and variadic source indexes.
/// @param score Optional destination for the total overload-conversion cost.
/// @param reportErrors Whether invalid binding emits diagnostics.
/// @param allowRuntimeObjectCoercion Whether safe runtime object-boundary conversions are ranked.
/// @return True when all arguments bind and their types are compatible.
/// @details The routine enforces named-argument ordering and uniqueness, applies contextual typing
///          to empty collections, accounts for omitted defaults, and scores variadic elements.
bool Sema::bindCallArgs(const std::vector<CallArg> &args,
                        const std::vector<CallParamSpec> &params,
                        SourceLoc loc,
                        const std::string &calleeName,
                        CallArgBinding &binding,
                        int *score,
                        bool reportErrors,
                        bool allowRuntimeObjectCoercion) const {
    binding.fixedParamSources.assign(params.size(), -1);
    binding.variadicSources.clear();
    if (score)
        *score = 0;

    const bool hasVariadic = !params.empty() && params.back().isVariadic;
    const size_t fixedCount = hasVariadic ? params.size() - 1 : params.size();

    auto fail = [&](const std::string &message, SourceLoc errLoc) {
        if (reportErrors)
            const_cast<Sema *>(this)->error(errLoc, message);
        return false;
    };

    bool sawNamed = false;
    size_t nextPositional = 0;

    for (size_t argIndex = 0; argIndex < args.size(); ++argIndex) {
        const auto &arg = args[argIndex];
        if (arg.name) {
            sawNamed = true;
            int targetIndex = -1;
            for (size_t i = 0; i < fixedCount; ++i) {
                if (params[i].name == *arg.name) {
                    targetIndex = static_cast<int>(i);
                    break;
                }
            }

            if (targetIndex < 0) {
                if (hasVariadic && params.back().name == *arg.name) {
                    return fail("Named binding for variadic parameter '" + *arg.name +
                                    "' is not supported in call to '" + calleeName + "'",
                                arg.value ? arg.value->loc : loc);
                }
                return fail("Unknown argument '" + *arg.name + "' in call to '" + calleeName + "'",
                            arg.value ? arg.value->loc : loc);
            }

            if (binding.fixedParamSources[static_cast<size_t>(targetIndex)] >= 0) {
                return fail("Argument '" + *arg.name + "' is provided more than once in call to '" +
                                calleeName + "'",
                            arg.value ? arg.value->loc : loc);
            }

            binding.fixedParamSources[static_cast<size_t>(targetIndex)] =
                static_cast<int>(argIndex);
        } else {
            if (sawNamed) {
                return fail("Positional arguments cannot follow named arguments in call to '" +
                                calleeName + "'",
                            arg.value ? arg.value->loc : loc);
            }

            while (nextPositional < fixedCount && binding.fixedParamSources[nextPositional] >= 0) {
                ++nextPositional;
            }

            if (nextPositional < fixedCount) {
                binding.fixedParamSources[nextPositional] = static_cast<int>(argIndex);
                ++nextPositional;
            } else if (hasVariadic) {
                binding.variadicSources.push_back(static_cast<int>(argIndex));
            } else {
                return fail("Too many arguments to '" + calleeName + "'",
                            arg.value ? arg.value->loc : loc);
            }
        }
    }

    for (size_t i = 0; i < fixedCount; ++i) {
        if (binding.fixedParamSources[i] < 0 && !params[i].hasDefault) {
            return fail("Missing required argument '" + params[i].name + "' in call to '" +
                            calleeName + "'",
                        loc);
        }
    }

    int totalScore = 0;
    for (size_t i = 0; i < fixedCount; ++i) {
        int sourceIndex = binding.fixedParamSources[i];
        if (sourceIndex < 0) {
            totalScore += 3;
            continue;
        }

        TypeRef paramType = params[i].type;
        Expr *argExpr = args[static_cast<size_t>(sourceIndex)].value.get();
        auto rawArgTypeIt = exprTypes_.find(argExpr);
        TypeRef rawArgType = rawArgTypeIt != exprTypes_.end() ? rawArgTypeIt->second : nullptr;
        TypeRef argType = contextualizeEmptyCollectionArg(argExpr, rawArgType, paramType);
        if (argType != rawArgType)
            exprTypes_[argExpr] = argType;
        int cost = conversionCost(paramType, argType, allowRuntimeObjectCoercion);
        if (allowRuntimeObjectCoercion && paramType && paramType->kind == TypeKindSem::Ptr &&
            argType && argType->kind == TypeKindSem::Function &&
            allowsFunctionPointerParam(params[i].name)) {
            cost = 2;
        }
        if (cost >= 1000) {
            if (reportErrors) {
                const_cast<Sema *>(this)->errorTypeMismatch(
                    args[static_cast<size_t>(sourceIndex)].value->loc, paramType, argType);
            }
            return false;
        }
        totalScore += cost;
    }

    if (hasVariadic) {
        TypeRef listType = params.back().type;
        TypeRef elemType = listType ? listType->elementType() : nullptr;
        for (int sourceIndex : binding.variadicSources) {
            Expr *argExpr = args[static_cast<size_t>(sourceIndex)].value.get();
            auto rawArgTypeIt = exprTypes_.find(argExpr);
            TypeRef rawArgType = rawArgTypeIt != exprTypes_.end() ? rawArgTypeIt->second : nullptr;
            TypeRef argType = contextualizeEmptyCollectionArg(argExpr, rawArgType, elemType);
            if (argType != rawArgType)
                exprTypes_[argExpr] = argType;
            int cost = conversionCost(elemType, argType, allowRuntimeObjectCoercion);
            if (allowRuntimeObjectCoercion && elemType && elemType->kind == TypeKindSem::Ptr &&
                argType && argType->kind == TypeKindSem::Function &&
                allowsFunctionPointerParam(params.back().name)) {
                cost = 2;
            }
            if (cost >= 1000) {
                if (reportErrors) {
                    const_cast<Sema *>(this)->errorTypeMismatch(
                        args[static_cast<size_t>(sourceIndex)].value->loc, elemType, argType);
                }
                return false;
            }
            totalScore += cost;
        }
    }

    if (score)
        *score = totalScore;
    return true;
}

/// @brief Enforce safe-Zia restrictions on raw-pointer runtime APIs.
/// @param calleeName Fully qualified runtime API name.
/// @param args Source call arguments.
/// @param params Exposed parameter specifications after hidden parameters are removed.
/// @param binding Previously validated source-to-parameter binding.
/// @param skipLeadingParams Number of hidden leading runtime parameters.
/// @param loc Call location used when no argument-specific location is available.
/// @return True when the call does not expose a raw handle, or when callback/payload bridge
///         parameters form an explicitly safe pair.
/// @details Safety metadata comes from the semantic override table first and the live runtime
///          registry otherwise. Rejections include a tailored safer alternative when known.
bool Sema::checkRuntimePointerSafety(const std::string &calleeName,
                                     const std::vector<CallArg> &args,
                                     const std::vector<CallParamSpec> &params,
                                     const CallArgBinding &binding,
                                     size_t skipLeadingParams,
                                     SourceLoc loc) const {
    RuntimePointerSafety safety;
    bool hasSafety = false;
    if (auto it = runtimePointerSafety_.find(calleeName); it != runtimePointerSafety_.end()) {
        safety = it->second;
        hasSafety = true;
    } else {
        const auto &registry = il::runtime::RuntimeRegistry::instance();
        if (auto sig = registry.findFunction(calleeName)) {
            safety.rawPointerReturn = sig->rawPointerReturn;
            safety.rawPointerParams = sig->rawPointerParams;
            hasSafety = true;
        }
    }

    if (!hasSafety)
        return true;

    auto diagnosticMessage = [&](std::string detail) {
        std::string message = "Runtime API '" + calleeName + "' exposes " + detail +
                              " and is unavailable in safe Zia";
        if (std::string alternative = saferRuntimePointerAlternative(calleeName);
            !alternative.empty()) {
            message += "; use " + alternative;
        } else {
            message += "; use a typed runtime class/API";
        }
        return message;
    };

    if (safety.rawPointerReturn) {
        const_cast<Sema *>(this)->error(loc, diagnosticMessage("a runtime-internal return handle"));
        return false;
    }

    auto isRawParam = [&](size_t exposedParamIndex) -> bool {
        size_t fullIndex = skipLeadingParams + exposedParamIndex;
        if (fullIndex < safety.rawPointerParams.size())
            return static_cast<bool>(safety.rawPointerParams[fullIndex]);
        if (skipLeadingParams > 0 && exposedParamIndex < safety.rawPointerParams.size())
            return static_cast<bool>(safety.rawPointerParams[exposedParamIndex]);
        return false;
    };

    auto bridgeRoleFor = [&](size_t exposedParamIndex) -> RuntimePointerBridgeRole {
        size_t fullIndex = skipLeadingParams + exposedParamIndex;
        if (fullIndex < safety.bridgeRoles.size())
            return safety.bridgeRoles[fullIndex];
        if (skipLeadingParams > 0 && exposedParamIndex < safety.bridgeRoles.size())
            return safety.bridgeRoles[exposedParamIndex];
        return RuntimePointerBridgeRole::None;
    };

    std::vector<bool> safeFunctionBridge(params.size(), false);
    for (size_t i = 0; i < params.size(); ++i) {
        RuntimePointerBridgeRole bridgeRole = bridgeRoleFor(i);
        const bool rawParam = isRawParam(i);
        if (!rawParam && bridgeRole == RuntimePointerBridgeRole::None)
            continue;

        int sourceIndex = i < binding.fixedParamSources.size() ? binding.fixedParamSources[i] : -1;
        TypeRef argType = nullptr;
        const CallArg *sourceArg = nullptr;
        SourceLoc errLoc = loc;
        if (sourceIndex >= 0 && static_cast<size_t>(sourceIndex) < args.size()) {
            sourceArg = &args[static_cast<size_t>(sourceIndex)];
            if (sourceArg->value) {
                errLoc = sourceArg->value->loc;
                auto typeIt = exprTypes_.find(sourceArg->value.get());
                if (typeIt != exprTypes_.end())
                    argType = typeIt->second;
            }
        }

        const std::string paramName = params[i].name.empty() ? "argument " + std::to_string(i + 1)
                                                             : "parameter '" + params[i].name + "'";

        if (bridgeRole == RuntimePointerBridgeRole::Callback) {
            if (sourceArg && argType && argType->kind == TypeKindSem::Function) {
                safeFunctionBridge[i] = true;
                continue;
            }

            const_cast<Sema *>(this)->error(
                errLoc,
                "Runtime API '" + calleeName + "' callback " + paramName +
                    " requires a function reference such as 'handler' or '&handler'");
            return false;
        }

        bool pairedWithFunctionBridge = false;
        if (bridgeRole == RuntimePointerBridgeRole::Payload) {
            for (size_t prior = 0; prior < i; ++prior) {
                if (safeFunctionBridge[prior]) {
                    pairedWithFunctionBridge = true;
                    break;
                }
            }
            if (!pairedWithFunctionBridge) {
                const_cast<Sema *>(this)->error(
                    errLoc,
                    "Runtime API '" + calleeName + "' payload " + paramName +
                        " requires a preceding callback function argument");
                return false;
            }
            if (isSafeFunctionBridgePayload(argType))
                continue;
        }

        const_cast<Sema *>(this)->error(errLoc,
                                        diagnosticMessage("runtime-internal handle " + paramName));
        return false;
    }

    return true;
}

/// @brief Resolve a function overload using full named-argument binding.
/// @param name Semantic function name.
/// @param args Source call arguments.
/// @param loc Call location for access and ambiguity diagnostics.
/// @param loweredName Optional destination for the selected lowered symbol.
/// @param bindingOut Optional destination for the winning argument mapping.
/// @param viaQualifiedModule Whether the name was accessed through a module qualifier.
/// @return Best accessible declaration, or nullptr when no unique candidate binds.
FunctionDecl *Sema::resolveFunctionArgOverload(const std::string &name,
                                               const std::vector<CallArg> &args,
                                               SourceLoc loc,
                                               std::string *loweredName,
                                               CallArgBinding *bindingOut,
                                               bool viaQualifiedModule) {
    auto it = functionOverloads_.find(name);
    if (it == functionOverloads_.end())
        return nullptr;

    FunctionDecl *best = nullptr;
    FunctionDecl *firstInaccessible = nullptr;
    CallArgBinding bestBinding;
    int bestScore = std::numeric_limits<int>::max();
    bool ambiguous = false;
    std::vector<std::string> candidateSigs;

    for (auto *decl : it->second) {
        Symbol accessSym;
        accessSym.kind = Symbol::Kind::Function;
        accessSym.name = name;
        auto declTypeIt = functionDeclTypes_.find(decl);
        accessSym.type = declTypeIt != functionDeclTypes_.end() ? declTypeIt->second : nullptr;
        accessSym.decl = decl;
        accessSym.loc = decl->loc;
        accessSym.isExported = decl->isExported;
        if (!canAccessSymbol(accessSym, loc, name, viaQualifiedModule)) {
            if (!firstInaccessible)
                firstInaccessible = decl;
            continue;
        }

        TypeRef funcType = getFunctionType(decl);
        if (!funcType || funcType->kind != TypeKindSem::Function)
            continue;

        CallArgBinding binding;
        int score = 0;
        auto specs = makeParamSpecs(decl->params, funcType->paramTypes());
        if (!bindCallArgs(args, specs, loc, name, binding, &score, false))
            continue;

        std::string lowered = loweredFunctionName(decl);
        candidateSigs.push_back(lowered);
        if (score < bestScore) {
            best = decl;
            bestBinding = std::move(binding);
            bestScore = score;
            ambiguous = false;
        } else if (score == bestScore) {
            ambiguous = true;
        }
    }

    if (!best) {
        if (firstInaccessible) {
            Symbol accessSym;
            accessSym.kind = Symbol::Kind::Function;
            accessSym.name = name;
            auto inaccTypeIt = functionDeclTypes_.find(firstInaccessible);
            accessSym.type =
                inaccTypeIt != functionDeclTypes_.end() ? inaccTypeIt->second : nullptr;
            accessSym.decl = firstInaccessible;
            accessSym.loc = firstInaccessible->loc;
            accessSym.isExported = firstInaccessible->isExported;
            reportInaccessibleSymbol(loc, name, accessSym, viaQualifiedModule);
        }
        return nullptr;
    }
    if (ambiguous) {
        error(loc, "Ambiguous call to '" + name + "': " + formatOverloadCandidates(candidateSigs));
        return nullptr;
    }
    if (loweredName)
        *loweredName = loweredFunctionName(best);
    if (bindingOut)
        *bindingOut = bestBinding;
    return best;
}

/// @brief Resolve a method overload using full named-argument binding.
/// @param ownerType Receiver's semantic type name.
/// @param methodName Source method name.
/// @param args Source call arguments.
/// @param loc Call location for ambiguity diagnostics.
/// @param resolvedOwnerType Optional destination for the declaring owner.
/// @param includeInherited Whether base-class overload families participate.
/// @param bindingOut Optional destination for the winning argument mapping.
/// @return Best declaration, or nullptr when no unique candidate binds.
MethodDecl *Sema::resolveMethodArgOverload(const std::string &ownerType,
                                           const std::string &methodName,
                                           const std::vector<CallArg> &args,
                                           SourceLoc loc,
                                           std::string *resolvedOwnerType,
                                           bool includeInherited,
                                           CallArgBinding *bindingOut) {
    MethodDecl *best = nullptr;
    CallArgBinding bestBinding;
    std::string bestOwner;
    int bestScore = std::numeric_limits<int>::max();
    bool ambiguous = false;
    std::vector<std::string> candidates;
    std::unordered_set<std::string> seenSignatures;

    auto considerOwner = [&](const std::string &candidateOwner) {
        auto it = methodOverloads_.find(candidateOwner + "." + methodName);
        if (it == methodOverloads_.end())
            return;

        for (auto *decl : it->second) {
            std::string sigKey = cachedMethodSignatureKey(candidateOwner, decl);
            if (sigKey.empty())
                sigKey = methodSignatureKey(*decl);
            if (!seenSignatures.insert(sigKey).second)
                continue;

            TypeRef methodType = getMethodType(candidateOwner, decl);
            if (!methodType || methodType->kind != TypeKindSem::Function)
                continue;

            CallArgBinding binding;
            int score = 0;
            auto specs = makeParamSpecs(decl->params, methodType->paramTypes());
            if (!bindCallArgs(
                    args, specs, loc, candidateOwner + "." + methodName, binding, &score, false))
                continue;

            std::string lowered = loweredMethodName(candidateOwner, decl);
            candidates.push_back(lowered.empty() ? (candidateOwner + "." + decl->name) : lowered);
            if (score < bestScore) {
                best = decl;
                bestBinding = std::move(binding);
                bestOwner = candidateOwner;
                bestScore = score;
                ambiguous = false;
            } else if (score == bestScore) {
                ambiguous = true;
            }
        }
    };

    considerOwner(ownerType);
    if (includeInherited) {
        auto classIt = lookupClassDeclForType(ownerType);
        while (classIt && !classIt->baseClass.empty()) {
            considerOwner(classIt->baseClass);
            classIt = lookupClassDeclForType(classIt->baseClass);
        }
    }

    if (!best)
        return nullptr;
    if (ambiguous) {
        error(loc,
              "Ambiguous call to method '" + methodName +
                  "': " + formatOverloadCandidates(candidates));
        return nullptr;
    }
    if (resolvedOwnerType)
        *resolvedOwnerType = bestOwner;
    if (bindingOut)
        *bindingOut = bestBinding;
    return best;
}

/// @brief Resolve a function call expression and preserve its argument binding.
/// @param name Semantic function name.
/// @param expr Call expression whose argument list is examined.
/// @param loc Call location for diagnostics.
/// @param loweredName Optional destination for the selected lowered symbol.
/// @param bindingOut Optional destination for the selected argument mapping.
/// @param viaQualifiedModule Whether the call uses an explicit module qualifier.
/// @return Selected declaration, or nullptr on failure or ambiguity.
FunctionDecl *Sema::resolveFunctionCallOverload(const std::string &name,
                                                CallExpr *expr,
                                                SourceLoc loc,
                                                std::string *loweredName,
                                                CallArgBinding *bindingOut,
                                                bool viaQualifiedModule) {
    return resolveFunctionArgOverload(
        name, expr->args, loc, loweredName, bindingOut, viaQualifiedModule);
}

/// @brief Resolve a method call expression and preserve its argument binding.
/// @param ownerType Receiver's semantic type name.
/// @param methodName Source method name.
/// @param expr Call expression whose argument list is examined.
/// @param loc Call location for diagnostics.
/// @param resolvedOwnerType Optional destination for the declaring owner.
/// @param includeInherited Whether base-class families participate.
/// @param bindingOut Optional destination for the selected argument mapping.
/// @return Selected method declaration, or nullptr on failure or ambiguity.
MethodDecl *Sema::resolveMethodCallOverload(const std::string &ownerType,
                                            const std::string &methodName,
                                            CallExpr *expr,
                                            SourceLoc loc,
                                            std::string *resolvedOwnerType,
                                            bool includeInherited,
                                            CallArgBinding *bindingOut) {
    return resolveMethodArgOverload(
        ownerType, methodName, expr->args, loc, resolvedOwnerType, includeInherited, bindingOut);
}

/// @brief Find an inherited method whose semantic signature exactly matches a declaration.
/// @param ownerType Derived owner where the base-class search starts.
/// @param decl Method whose cached owner-specific signature is matched.
/// @return Nearest matching base declaration, or nullptr when none exists.
MethodDecl *Sema::findInheritedExactMethod(const std::string &ownerType,
                                           const MethodDecl &decl) const {
    auto classIt = lookupClassDeclForType(ownerType);
    if (!classIt)
        return nullptr;
    const std::string wanted = cachedMethodSignatureKey(ownerType, &decl);
    std::string parentName = classIt->baseClass;
    while (!parentName.empty()) {
        auto famIt = methodOverloads_.find(parentName + "." + decl.name);
        if (famIt != methodOverloads_.end()) {
            for (auto *candidate : famIt->second) {
                std::string candidateKey = cachedMethodSignatureKey(parentName, candidate);
                if (candidateKey.empty())
                    candidateKey = methodSignatureKey(*candidate);
                if (candidateKey == wanted)
                    return candidate;
            }
        }
        ClassDecl *nextIt = lookupClassDeclForType(parentName);
        if (!nextIt)
            break;
        parentName = nextIt->baseClass;
    }
    return nullptr;
}

/// @brief Look up a class declaration for a concrete or generic-instantiated type.
/// @param typeName Semantic type name.
/// @return Registered class declaration, its generic origin declaration, or nullptr.
ClassDecl *Sema::lookupClassDeclForType(const std::string &typeName) const {
    auto it = classDecls_.find(typeName);
    if (it != classDecls_.end())
        return it->second;
    if (Decl *genericDecl = getGenericDeclForInstantiation(typeName)) {
        if (genericDecl->kind == DeclKind::Class)
            return static_cast<ClassDecl *>(genericDecl);
    }
    return nullptr;
}

/// @brief Look up a struct declaration for a concrete or generic-instantiated type.
/// @param typeName Semantic type name.
/// @return Registered struct declaration, its generic origin declaration, or nullptr.
StructDecl *Sema::lookupStructDeclForType(const std::string &typeName) const {
    auto it = structDecls_.find(typeName);
    if (it != structDecls_.end())
        return it->second;
    if (Decl *genericDecl = getGenericDeclForInstantiation(typeName)) {
        if (genericDecl->kind == DeclKind::Struct)
            return static_cast<StructDecl *>(genericDecl);
    }
    return nullptr;
}

/// @brief Test whether a function name identifies a true overload family.
/// @param name Semantic function name.
/// @return True when more than one declaration is registered under @p name.
bool Sema::hasOverloadedFunctionName(const std::string &name) const {
    auto it = functionOverloads_.find(name);
    return it != functionOverloads_.end() && it->second.size() > 1;
}

/// @brief Format lowered overload names for an ambiguity diagnostic.
/// @param candidates Candidate symbol names in diagnostic order.
/// @return Comma-separated candidate list.
std::string Sema::formatOverloadCandidates(const std::vector<std::string> &candidates) const {
    std::string result;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (i != 0)
            result += ", ";
        result += candidates[i];
    }
    return result;
}

/// @brief Begin warning collection under a caller-owned policy.
/// @param policy Warning policy that remains valid for the analysis lifetime.
/// @details Replaces the current policy and clears all previously scanned source suppressions.
void Sema::initWarnings(const WarningPolicy &policy) {
    warningPolicy_ = &policy;
    suppressions_.clear();
}

/// @brief Scan one source buffer for warning-suppression directives.
/// @param fileId Diagnostic file identifier associated with @p source.
/// @param source Complete source text to scan.
void Sema::addWarningSuppressions(uint32_t fileId, std::string_view source) {
    suppressions_.scan(fileId, source);
}

/// @brief Register a nominal type declaration in the current semantic scope.
/// @param decl Struct, class, interface, or enum declaration.
/// @param semanticName Fully qualified name used by type and declaration registries.
/// @return True when the type is registered or an existing entry for the same declaration is
///         refreshed; false for unsupported declaration kinds or symbol conflicts.
/// @details Generic declarations receive type-parameter placeholder arguments and are also
///          recorded in the generic-type registry.
bool Sema::registerTypeDeclarationSymbol(Decl &decl, const std::string &semanticName) {
    if (Symbol *existing = currentScope_->lookupLocal(semanticName)) {
        auto typeIt = typeRegistry_.find(semanticName);
        if (existing->kind == Symbol::Kind::Type && existing->decl == &decl &&
            typeIt != typeRegistry_.end()) {
            existing->type = typeIt->second;
            return true;
        }
    }

    TypeRef type;
    Symbol sym;
    sym.kind = Symbol::Kind::Type;
    sym.name = semanticName;
    sym.decl = &decl;
    sym.isExported = decl.isExported;

    auto makeGenericArgs = [](const std::vector<std::string> &params) {
        std::vector<TypeRef> args;
        args.reserve(params.size());
        for (const auto &param : params)
            args.push_back(types::typeParam(param));
        return args;
    };

    switch (decl.kind) {
        case DeclKind::Struct: {
            auto &value = static_cast<StructDecl &>(decl);
            if (!value.genericParams.empty()) {
                registerGenericType(semanticName, &value);
                type = std::make_shared<ZannaType>(
                    TypeKindSem::Struct, semanticName, makeGenericArgs(value.genericParams));
            } else {
                type = types::structType(semanticName);
            }
            structDecls_[semanticName] = &value;
            break;
        }
        case DeclKind::Class: {
            auto &cls = static_cast<ClassDecl &>(decl);
            if (!cls.genericParams.empty()) {
                registerGenericType(semanticName, &cls);
                type = std::make_shared<ZannaType>(
                    TypeKindSem::Class, semanticName, makeGenericArgs(cls.genericParams));
            } else {
                type = types::classType(semanticName);
            }
            classDecls_[semanticName] = &cls;
            break;
        }
        case DeclKind::Interface: {
            auto &iface = static_cast<InterfaceDecl &>(decl);
            if (!iface.genericParams.empty()) {
                registerGenericType(semanticName, &iface);
                type = std::make_shared<ZannaType>(
                    TypeKindSem::Interface, semanticName, makeGenericArgs(iface.genericParams));
            } else {
                type = types::interface(semanticName);
            }
            interfaceDecls_[semanticName] = &iface;
            break;
        }
        case DeclKind::Enum: {
            auto &enumDecl = static_cast<EnumDecl &>(decl);
            type = types::enumType(semanticName);
            enumDecls_[semanticName] = &enumDecl;
            break;
        }
        default:
            return false;
    }

    sym.type = type;
    if (Symbol *existing = currentScope_->lookupLocal(semanticName)) {
        if (existing->kind == Symbol::Kind::Type && existing->decl == &decl) {
            existing->type = type;
            typeRegistry_[semanticName] = type;
            return true;
        }
        return defineSymbol(semanticName, sym);
    }

    if (defineSymbol(semanticName, sym)) {
        typeRegistry_[semanticName] = type;
        return true;
    }
    return false;
}

/// @brief Install an unresolved type-alias symbol before alias targets are resolved.
/// @param decl Alias declaration represented by the placeholder.
/// @param semanticName Fully qualified alias name.
/// @details The unknown placeholder permits forward references among aliases during the
///          subsequent fixed-point resolution pass.
void Sema::registerTypeAliasPlaceholder(TypeAliasDecl &decl, const std::string &semanticName) {
    Symbol sym;
    sym.kind = Symbol::Kind::Type;
    sym.name = semanticName;
    sym.type = types::unknown();
    sym.decl = &decl;
    sym.isExported = decl.isExported;

    if (Symbol *existing = currentScope_->lookupLocal(semanticName)) {
        if (existing->kind == Symbol::Kind::Type && existing->decl == &decl) {
            existing->type = types::unknown();
        } else {
            defineSymbol(semanticName, sym);
        }
    } else {
        defineSymbol(semanticName, sym);
    }
    typeAliases_[semanticName] = types::unknown();
}

/// @brief Resolve forward-referencing type aliases to a fixed point.
/// @param aliases Alias declarations paired with their fully qualified semantic names.
/// @details At most one pass per alias plus a final convergence pass is needed. Any placeholder
///          that remains unknown receives an unresolved-target diagnostic.
void Sema::resolvePendingTypeAliases(
    const std::vector<std::pair<TypeAliasDecl *, std::string>> &aliases) {
    for (size_t pass = 0; pass <= aliases.size(); ++pass) {
        bool changed = false;
        for (const auto &[alias, semanticName] : aliases) {
            auto currentIt = typeAliases_.find(semanticName);
            if (currentIt != typeAliases_.end() && currentIt->second &&
                currentIt->second->kind != TypeKindSem::Unknown) {
                continue;
            }

            TypeRef resolved = resolveTypeNode(alias->targetType.get());
            if (!resolved || resolved->kind == TypeKindSem::Unknown)
                continue;

            typeAliases_[semanticName] = resolved;
            if (Symbol *sym = currentScope_->lookupLocal(semanticName))
                sym->type = resolved;
            changed = true;
        }
        if (!changed)
            break;
    }

    for (const auto &[alias, semanticName] : aliases) {
        auto it = typeAliases_.find(semanticName);
        if (it == typeAliases_.end() || !it->second || it->second->kind == TypeKindSem::Unknown) {
            error(alias->loc, "Cannot resolve type alias target for '" + alias->name + "'");
        }
    }
}

/// @brief Register concrete class inheritance and interface implementation relationships.
/// @param declarations Top-level declarations whose non-generic nominal relationships are read.
/// @details Base and interface names are resolved to canonical semantic names before the global
///          assignability registries are populated. Generic relationships are deferred until
///          instantiation.
void Sema::registerNominalTypeRelationships(std::vector<DeclPtr> &declarations) {
    auto registerInterfaces = [this](const std::string &ownerName,
                                     const SourceLoc &loc,
                                     const std::vector<std::string> &interfaces) {
        for (const auto &ifaceName : interfaces) {
            TypeRef ifaceType = resolveNamedType(ifaceName, loc);
            if (ifaceType && ifaceType->kind == TypeKindSem::Interface)
                types::registerInterfaceImplementation(ownerName, ifaceType->name);
        }
    };

    for (auto &decl : declarations) {
        switch (decl->kind) {
            case DeclKind::Struct: {
                auto *value = static_cast<StructDecl *>(decl.get());
                if (value->genericParams.empty()) {
                    const std::string ownerName = semanticNameForDecl(*value, value->name);
                    registerInterfaces(ownerName, value->loc, value->interfaces);
                }
                break;
            }
            case DeclKind::Class: {
                auto *cls = static_cast<ClassDecl *>(decl.get());
                if (!cls->genericParams.empty())
                    break;

                const std::string ownerName = semanticNameForDecl(*cls, cls->name);
                if (!cls->baseClass.empty()) {
                    TypeRef baseType = resolveNamedType(cls->baseClass, cls->loc);
                    if (baseType && baseType->kind == TypeKindSem::Class) {
                        cls->baseClass = baseType->name;
                        types::registerClassInheritance(ownerName, cls->baseClass);
                    }
                }
                registerInterfaces(ownerName, cls->loc, cls->interfaces);
                break;
            }
            default:
                break;
        }
    }
}

/// @brief Run multi-pass semantic analysis on a module.
/// @details Pass 1: Register all top-level declarations (types, functions, globals).
///          Pass 1b: Process namespace declarations (recursive multi-pass).
///          Pass 2: Register member signatures (fields, method types) for type declarations.
///          Pass 3: Analyze declaration bodies (function bodies, method bodies, initializers).
/// @param module The module AST to analyze.
/// @return True if analysis succeeded without errors, false otherwise.
bool Sema::analyze(ModuleDecl &module) {
    currentModule_ = &module;
    fileModuleNames_.clear();
    moduleExports_.clear();
    fileModuleExports_.clear();
    fileBoundModuleIds_.clear();
    boundFileModuleIds_.clear();

    for (auto &bind : module.binds) {
        analyzeBind(bind);
    }

    prepareModuleScopedTypeNames(module);

    std::vector<std::pair<TypeAliasDecl *, std::string>> pendingTypeAliases;

    for (auto &decl : module.declarations) {
        switch (decl->kind) {
            case DeclKind::Struct: {
                auto *value = static_cast<StructDecl *>(decl.get());
                registerTypeDeclarationSymbol(*value, semanticNameForDecl(*value, value->name));
                break;
            }
            case DeclKind::Class: {
                auto *cls = static_cast<ClassDecl *>(decl.get());
                registerTypeDeclarationSymbol(*cls, semanticNameForDecl(*cls, cls->name));
                break;
            }
            case DeclKind::Interface: {
                auto *iface = static_cast<InterfaceDecl *>(decl.get());
                registerTypeDeclarationSymbol(*iface, semanticNameForDecl(*iface, iface->name));
                break;
            }
            case DeclKind::Enum: {
                auto *enumDecl = static_cast<EnumDecl *>(decl.get());
                registerTypeDeclarationSymbol(*enumDecl,
                                              semanticNameForDecl(*enumDecl, enumDecl->name));
                break;
            }
            case DeclKind::TypeAlias: {
                auto *alias = static_cast<TypeAliasDecl *>(decl.get());
                const std::string semanticName = semanticNameForDecl(*alias, alias->name);
                registerTypeAliasPlaceholder(*alias, semanticName);
                pendingTypeAliases.emplace_back(alias, semanticName);
                break;
            }
            default:
                break;
        }
    }

    resolvePendingTypeAliases(pendingTypeAliases);
    registerNominalTypeRelationships(module.declarations);

    // First pass: register all top-level declarations
    for (auto &decl : module.declarations) {
        switch (decl->kind) {
            case DeclKind::Function: {
                auto *func = static_cast<FunctionDecl *>(decl.get());
                const std::string semanticName = semanticNameForDecl(*func, func->name);

                if (!func->genericParams.empty()) {
                    // Generic function: register for later instantiation
                    registerGenericFunction(semanticName, func);

                    // Create a placeholder type with type parameters as param types
                    // The actual function type will be created when instantiated
                    std::vector<TypeRef> paramTypes;
                    for (const auto &param : func->genericParams) {
                        paramTypes.push_back(types::typeParam(param));
                    }
                    auto placeholderType = types::function(paramTypes, types::unknown());

                    Symbol sym;
                    sym.kind = Symbol::Kind::Function;
                    sym.name = semanticName;
                    sym.type = placeholderType;
                    sym.decl = func;
                    sym.isExported = func->isExported;
                    sym.paramNames = paramNamesFor(func->params);
                    if (!currentScope_->lookupLocal(semanticName))
                        defineSymbol(semanticName, sym);
                } else {
                    auto funcType = functionTypeForDecl(*func);

                    Symbol sym;
                    sym.kind = Symbol::Kind::Function;
                    sym.name = semanticName;
                    sym.type = funcType;
                    sym.decl = func;
                    sym.isExported = func->isExported;
                    sym.paramNames = paramNamesFor(func->params);
                    Symbol *existing = currentScope_->lookupLocal(semanticName);
                    if (!existing) {
                        defineSymbol(semanticName, sym);
                    } else if (existing->kind != Symbol::Kind::Function) {
                        reportDuplicateDefinition(semanticName, func->loc);
                    }
                    registerFunctionOverload(semanticName, func, funcType, func->loc);
                }
                break;
            }
            case DeclKind::Struct: {
                auto *value = static_cast<StructDecl *>(decl.get());
                registerTypeDeclarationSymbol(*value, semanticNameForDecl(*value, value->name));
                break;
            }
            case DeclKind::Class: {
                auto *cls = static_cast<ClassDecl *>(decl.get());
                registerTypeDeclarationSymbol(*cls, semanticNameForDecl(*cls, cls->name));
                break;
            }
            case DeclKind::Interface: {
                auto *iface = static_cast<InterfaceDecl *>(decl.get());
                registerTypeDeclarationSymbol(*iface, semanticNameForDecl(*iface, iface->name));
                break;
            }
            case DeclKind::Enum: {
                auto *enumDecl = static_cast<EnumDecl *>(decl.get());
                registerTypeDeclarationSymbol(*enumDecl,
                                              semanticNameForDecl(*enumDecl, enumDecl->name));
                break;
            }
            case DeclKind::GlobalVar: {
                auto *gvar = static_cast<GlobalVarDecl *>(decl.get());
                const std::string semanticName = semanticNameForDecl(*gvar, gvar->name);
                // Determine the variable type
                TypeRef varType;
                if (gvar->type) {
                    varType = resolveTypeNode(gvar->type.get());
                } else if (gvar->initializer) {
                    // Type inference from initializer - defer to second pass
                    varType = types::unknown();
                } else {
                    varType = types::unknown();
                }

                Symbol sym;
                sym.kind = Symbol::Kind::Variable;
                sym.name = semanticName;
                sym.type = varType;
                sym.isFinal = gvar->isFinal;
                sym.decl = gvar;
                sym.isExported = gvar->isExported;
                if (defineSymbol(semanticName, sym)) {
                    // Global variables are always considered initialized
                    // (either explicitly or default-initialized)
                    markInitialized(semanticName);
                }
                break;
            }
            case DeclKind::Namespace: {
                auto *ns = static_cast<NamespaceDecl *>(decl.get());
                Symbol sym;
                sym.kind = Symbol::Kind::Module;
                sym.name = ns->name;
                sym.type = types::module(ns->name);
                sym.decl = ns;
                sym.isFinal = true;
                defineSymbol(ns->name, sym);
                break;
            }
            case DeclKind::TypeAlias: {
                break;
            }
            default:
                break;
        }
    }

    // Process namespace declarations (they handle their own multi-pass analysis)
    for (auto &decl : module.declarations) {
        if (decl->kind == DeclKind::Namespace) {
            analyzeNamespaceDecl(*static_cast<NamespaceDecl *>(decl.get()));
        }
    }

    // Pre-pass: eagerly resolve types of final constants from literal initializers
    // This allows forward references to final constants in class/function bodies
    registerFinalConstantTypes(module.declarations);

    // File-module export maps snapshot top-level symbol types. Build them only
    // after final constants have had their literal types registered.
    buildBoundFileExports(module.binds, module.declarations);

    // Second pass: register all method/field signatures (before analyzing bodies)
    // This ensures cross-module method calls can be resolved regardless of declaration order
    registerMemberSignatures(module.declarations);

    // Third pass: analyze declarations (bodies)
    analyzeDeclarationBodies(module.declarations);

    return !hasError_;
}

/// @brief Get the resolved semantic type of an expression.
/// @details Returns the cached type from exprTypes_, applying type parameter
///          substitution if currently in a generic context.
/// @param expr The expression to query.
/// @return The resolved type, or unknown() if the expression has not been analyzed.
TypeRef Sema::typeOf(const Expr *expr) const {
    auto it = exprTypes_.find(expr);
    if (it == exprTypes_.end())
        return types::unknown();
    // Apply type parameter substitution if in generic context
    return substituteTypeParams(it->second);
}

/// @brief Resolve a type AST node to a semantic type reference.
/// @param node The type node to resolve.
/// @return The resolved semantic type.
TypeRef Sema::resolveType(const TypeNode *node) const {
    return const_cast<Sema *>(this)->resolveTypeNode(node);
}

} // namespace il::frontends::zia
