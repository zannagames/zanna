//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Expr_Call.cpp
/// @brief Lowers direct, indirect, generic, built-in, and runtime call
///        expressions.
///
/// @details Arguments are evaluated in source order, reordered according to
///          semantic bindings, and coerced to the selected callable's
///          parameter representations. Runtime calls additionally follow the
///          central ABI and ownership catalog. Managed results transfer one
///          reference to their consumer or enter deferred cleanup, and special
///          call families select type-specific runtime entry points where
///          their surface types require them.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/LowererCallArgumentLowerer.hpp"
#include "frontends/zia/LowererRuntimeCallBuilder.hpp"
#include "frontends/zia/RuntimeNames.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

namespace il::frontends::zia {

/// @brief Byte offset of the environment pointer in `[funcPtr, envPtr]`
///        closure storage.
static constexpr int kClosureEnvOffset = 8;

using namespace runtime;

namespace {

/// @brief Return @p expr as @p T when its AST tag is @p kind.
/// @tparam T Concrete expression type paired with @p kind.
/// @param expr Expression pointer to inspect.
/// @param kind Expected AST expression kind.
/// @return Cast pointer when the tag matches; nullptr otherwise.
template <typename T> T *exprAs(Expr *expr, ExprKind kind) {
    return expr && expr->kind == kind ? static_cast<T *>(expr) : nullptr;
}

/// @brief True if @p callee is an HttpServer route registration runtime call
///        (Get/Post/Put/Delete) needing handler-binding lowering.
/// @param callee Canonical runtime function name.
/// @return True for the four HTTP route-registration entry points.
bool isHttpServerRouteRuntime(std::string_view callee) {
    return callee == kNetworkHttpServerGet || callee == kNetworkHttpServerPost ||
           callee == kNetworkHttpServerPut || callee == kNetworkHttpServerDelete;
}

/// @brief True if @p callee is an HttpsServer route registration runtime call.
/// @param callee Canonical runtime function name.
/// @return True for the four HTTPS route-registration entry points.
bool isHttpsServerRouteRuntime(std::string_view callee) {
    return callee == kNetworkHttpsServerGet || callee == kNetworkHttpsServerPost ||
           callee == kNetworkHttpsServerPut || callee == kNetworkHttpsServerDelete;
}

/// @brief Runtime BindHandler callee matching @p callee's server flavor
///        (HTTPS vs HTTP).
/// @param callee HTTP or HTTPS route-registration runtime name.
/// @return Canonical handler-binding runtime target for the same server
///         flavor.
const char *httpServerBindHandlerTarget(std::string_view callee) {
    return isHttpsServerRouteRuntime(callee) ? kNetworkHttpsServerBindHandler
                                             : kNetworkHttpServerBindHandler;
}

/// @brief True if @p callee is a Terminal text-output runtime (Say/Print).
/// @param callee Canonical runtime function name.
/// @return True for Terminal.Say and Terminal.Print.
bool isTerminalTextRuntime(std::string_view callee) {
    return callee == kTerminalSay || callee == kTerminalPrint;
}

/// @brief Peel `Optional` wrappers off @p type to get the underlying surface
///        type used to pick a type-specialized runtime callee.
/// @param type Semantic surface type.
/// @return Innermost non-Optional type, or null when the chain has no type.
TypeRef unwrapSurfaceType(TypeRef type) {
    while (type && type->kind == TypeKindSem::Optional && type->innerType())
        type = type->innerType();
    return type;
}

/// @brief Map a generic collection runtime call to a type-specialized variant
///        based on the expected result @p surfaceType (e.g. Seq.Get →
///        Seq.GetStr for a String result, Map.Get → Map.GetInt). Empty when
///        no specialization applies.
/// @param callee Generic canonical runtime function name.
/// @param surfaceType Semantic result type expected by the Zia expression.
/// @return Specialized runtime name, or an empty string when the generic
///         target is already appropriate.
std::string specializedRuntimeReturnCallee(std::string_view callee, TypeRef surfaceType) {
    surfaceType = unwrapSurfaceType(surfaceType);
    if (!surfaceType)
        return {};

    if (callee == kCollectionsSeqGet && surfaceType->kind == TypeKindSem::String)
        return kSeqGetStr;

    if (callee == kCollectionsMapGet) {
        switch (surfaceType->kind) {
            case TypeKindSem::String:
                return kCollectionsMapGetStr;
            case TypeKindSem::Integer:
                return kCollectionsMapGetInt;
            case TypeKindSem::Number:
                return kCollectionsMapGetFloat;
            default:
                break;
        }
    }

    if (callee == kCollectionsMapGetOr) {
        switch (surfaceType->kind) {
            case TypeKindSem::Integer:
                return kCollectionsMapGetIntOr;
            case TypeKindSem::Number:
                return kCollectionsMapGetFloatOr;
            default:
                break;
        }
    }

    return {};
}

/// @brief True if @p type is a pointer type (the shape expected for an HTTP
///        handler's request/response parameters).
/// @param type Semantic parameter type to classify.
/// @return True only for the Zia `Ptr` semantic kind.
bool isHttpHandlerPtrType(TypeRef type) {
    return type && type->kind == TypeKindSem::Ptr;
}

/// @brief Resolve the user function bound as an HTTP route handler for @p tag,
///        requiring the `void(ptr, ptr)` request/response signature; returns
///        nullptr if no uniquely matching declaration exists.
/// @param sema Semantic registry containing function declarations and types.
/// @param tag Handler function name supplied to route registration.
/// @return Unique matching handler declaration, or null when absent or
///         ambiguous.
FunctionDecl *resolveHttpHandlerDecl(Sema &sema, const std::string &tag) {
    if (FunctionDecl *decl = sema.getFunctionDecl(tag)) {
        TypeRef fnType = sema.getFunctionType(decl);
        if (fnType && fnType->kind == TypeKindSem::Function && fnType->returnType() &&
            fnType->returnType()->kind == TypeKindSem::Void && fnType->paramTypes().size() == 2 &&
            isHttpHandlerPtrType(fnType->paramTypes()[0]) &&
            isHttpHandlerPtrType(fnType->paramTypes()[1])) {
            return decl;
        }
    }

    FunctionDecl *match = nullptr;
    for (FunctionDecl *decl : sema.getFunctionOverloads(tag)) {
        TypeRef fnType = sema.getFunctionType(decl);
        if (!fnType || fnType->kind != TypeKindSem::Function || !fnType->returnType() ||
            fnType->returnType()->kind != TypeKindSem::Void || fnType->paramTypes().size() != 2 ||
            !isHttpHandlerPtrType(fnType->paramTypes()[0]) ||
            !isHttpHandlerPtrType(fnType->paramTypes()[1])) {
            continue;
        }
        if (match)
            return nullptr;
        match = decl;
    }

    return match;
}

/// @brief Resolve the lowered symbol name of an HTTP route handler.
/// @param sema Semantic registry containing function declarations and names.
/// @param tag Handler function name supplied to route registration.
/// @return Lowered function name, `main` for source handler `start`, or an
///         empty string when no unique valid handler exists.
std::string httpHandlerTargetName(Sema &sema, const std::string &tag) {
    FunctionDecl *decl = resolveHttpHandlerDecl(sema, tag);
    if (!decl)
        return {};

    std::string lowered = sema.loweredFunctionName(decl);
    if (!lowered.empty())
        return lowered;
    return tag == "start" ? "main" : tag;
}

/// @brief Pick the type-specialised `Zanna.Result.Ok*` callee for @p type.
/// @param type Semantic success-payload type.
/// @return Canonical Result.Ok runtime entry point.
const char *resultOkCalleeFor(TypeRef type) {
    if (!type)
        return kResultOk;
    switch (type->kind) {
        case TypeKindSem::String:
            return kResultOkStr;
        case TypeKindSem::Integer:
        case TypeKindSem::Enum:
            return kResultOkI64;
        case TypeKindSem::Number:
            return kResultOkF64;
        default:
            return kResultOk;
    }
}

/// @brief Pick the type-specialised `Zanna.Result.Unwrap*` callee.
/// @param type Semantic success-payload type.
/// @return Canonical Result.Unwrap runtime entry point.
const char *resultUnwrapCalleeFor(TypeRef type) {
    if (!type)
        return kResultUnwrap;
    switch (type->kind) {
        case TypeKindSem::String:
            return kResultUnwrapStr;
        case TypeKindSem::Integer:
        case TypeKindSem::Enum:
            return kResultUnwrapI64;
        case TypeKindSem::Number:
            return kResultUnwrapF64;
        default:
            return kResultUnwrap;
    }
}

/// @brief Pick the type-specialised `Zanna.Result.UnwrapOr*` callee.
/// @param type Semantic success-payload type.
/// @return Canonical Result.UnwrapOr runtime entry point.
const char *resultUnwrapOrCalleeFor(TypeRef type) {
    if (!type)
        return kResultUnwrapOr;
    switch (type->kind) {
        case TypeKindSem::String:
            return kResultUnwrapOrStr;
        case TypeKindSem::Integer:
        case TypeKindSem::Enum:
            return kResultUnwrapOrI64;
        case TypeKindSem::Number:
            return kResultUnwrapOrF64;
        default:
            return kResultUnwrapOr;
    }
}

/// @brief True when @p typeName belongs to the generated runtime namespace.
/// @param typeName Qualified semantic type name.
/// @return True when @p typeName starts with the runtime namespace prefix.
bool hasRuntimeNamespacePrefix(const std::string &typeName) {
    std::string_view prefix{kRuntimeNamespacePrefix};
    return typeName.size() >= prefix.size() &&
           std::string_view(typeName).substr(0, prefix.size()) == prefix;
}

/// @brief True when @p type can be a receiver for runtime member syntax.
/// @param type Semantic receiver type.
/// @return True for built-in collections/strings and registered or
///         namespace-qualified runtime classes.
bool isRuntimeReceiverSurfaceType(TypeRef type) {
    if (!type)
        return false;
    if (type->kind == TypeKindSem::Set || type->kind == TypeKindSem::List ||
        type->kind == TypeKindSem::Map || type->kind == TypeKindSem::String)
        return true;
    return !type->name.empty() && (il::runtime::findRuntimeClassByQName(type->name) != nullptr ||
                                   hasRuntimeNamespacePrefix(type->name));
}

/// @brief True when the resolved runtime ABI has one implicit receiver parameter.
/// @param rtDesc Runtime descriptor for the selected callee.
/// @param binding Semantic binding for explicit source arguments, or null.
/// @param explicitArgCount Source argument count used without a binding.
/// @return True when the ABI parameter count exceeds the exposed argument
///         count by exactly one.
bool runtimeCallExpectsImplicitReceiver(const il::runtime::RuntimeDescriptor *rtDesc,
                                        const Sema::CallArgBinding *binding,
                                        size_t explicitArgCount) {
    if (!rtDesc)
        return false;

    size_t exposedParamCount = explicitArgCount;
    if (binding)
        exposedParamCount = binding->fixedParamSources.size() + binding->variadicSources.size();

    const size_t abiParamCount = rtDesc->signature.paramTypes.size();
    return abiParamCount == exposedParamCount + 1;
}

} // namespace

//=============================================================================
// Bound Argument Lowering Helpers
//=============================================================================

/// @brief Lower call arguments in source evaluation order.
/// @param args Source arguments to evaluate.
/// @return One LowerResult per argument, before parameter reordering.
std::vector<LowerResult> Lowerer::lowerSourceArgs(const std::vector<CallArg> &args) {
    return CallArgumentLowerer(*this).lowerSourceArgs(args);
}

/// @brief Map a resolved call binding to source argument indices.
/// @param args Source arguments supplied by the call site.
/// @param binding Semantic argument binding, or null for positional order.
/// @return Fixed-parameter source indices followed by variadic sources.
std::vector<int> Lowerer::orderedArgSources(const std::vector<CallArg> &args,
                                            const Sema::CallArgBinding *binding) const {
    return CallArgumentLowerer::orderedArgSources(args, binding);
}

/// @brief Lower, reorder, default, coerce, and variadically pack call arguments.
/// @param args Source arguments supplied by the call site.
/// @param paramTypes Resolved parameter types in declaration order.
/// @param params Parameter declarations for defaults and variadic metadata, or
///        null when unavailable.
/// @param binding Semantic argument binding, or null for positional coercion.
/// @return Final argument values in parameter order.
std::vector<Lowerer::Value> Lowerer::lowerResolvedArgs(const std::vector<CallArg> &args,
                                                       const std::vector<TypeRef> &paramTypes,
                                                       const std::vector<Param> *params,
                                                       const Sema::CallArgBinding *binding) {
    return CallArgumentLowerer(*this).lowerResolvedArgs(args, paramTypes, params, binding);
}

/// @brief Lower arguments for an ordinary resolved call expression.
/// @param expr Call expression whose semantic binding is used.
/// @param paramTypes Resolved parameter types in declaration order.
/// @param params Parameter declarations, or null when unavailable.
/// @return Final argument values in parameter order.
std::vector<Lowerer::Value> Lowerer::lowerResolvedCallArgs(CallExpr *expr,
                                                           const std::vector<TypeRef> &paramTypes,
                                                           const std::vector<Param> *params) {
    return CallArgumentLowerer(*this).lowerResolvedCallArgs(expr, paramTypes, params);
}

/// @brief Lower arguments for a resolved constructor invocation.
/// @param expr Allocation expression whose constructor binding is used.
/// @param paramTypes Resolved constructor parameter types.
/// @param params Constructor parameter declarations, or null when unavailable.
/// @return Final constructor argument values in parameter order.
std::vector<Lowerer::Value> Lowerer::lowerResolvedNewArgs(NewExpr *expr,
                                                          const std::vector<TypeRef> &paramTypes,
                                                          const std::vector<Param> *params) {
    return CallArgumentLowerer(*this).lowerResolvedNewArgs(expr, paramTypes, params);
}

//=============================================================================
// Built-in Function Call Helper
//=============================================================================

/// @brief Try to lower a compiler-recognized free-function built-in.
/// @param name Unqualified callee name.
/// @param expr Call expression containing source arguments.
/// @return Lowered `print`/`println`/`toString` result, or `std::nullopt` when
///         @p name is not handled here.
std::optional<LowerResult> Lowerer::lowerBuiltinCall(const std::string &name, CallExpr *expr) {
    if (name == "print" || name == "println") {
        if (!expr->args.empty()) {
            auto arg = lowerExpr(expr->args[0].value.get());
            TypeRef argType = sema_.typeOf(expr->args[0].value.get());

            Value strVal = arg.value;
            if (argType && argType->kind != TypeKindSem::String) {
                if (argType->kind == TypeKindSem::Integer) {
                    strVal = emitCallRet(Type(Type::Kind::Str), kStringFromInt, {arg.value});
                } else if (argType->kind == TypeKindSem::Number) {
                    strVal = emitCallRet(Type(Type::Kind::Str), kStringFromNum, {arg.value});
                }
            }

            emitCall(kTerminalSay, {strVal});
        }
        return LowerResult{Value::constInt(0), Type(Type::Kind::Void)};
    }

    if (name == "toString") {
        if (expr->args.empty())
            return LowerResult{Value::constInt(0), Type(Type::Kind::Str)};

        auto *argExpr = expr->args[0].value.get();
        auto arg = lowerExpr(argExpr);
        TypeRef argType = sema_.typeOf(argExpr);

        if (argType) {
            switch (argType->kind) {
                case TypeKindSem::String:
                    return LowerResult{arg.value, Type(Type::Kind::Str)};
                case TypeKindSem::Integer: {
                    Value strVal = emitCallRet(Type(Type::Kind::Str), kStringFromInt, {arg.value});
                    return LowerResult{strVal, Type(Type::Kind::Str)};
                }
                case TypeKindSem::Number: {
                    Value strVal = emitCallRet(Type(Type::Kind::Str), kStringFromNum, {arg.value});
                    return LowerResult{strVal, Type(Type::Kind::Str)};
                }
                case TypeKindSem::Boolean: {
                    Value strVal = emitCallRet(Type(Type::Kind::Str), kTextFmtBool, {arg.value});
                    return LowerResult{strVal, Type(Type::Kind::Str)};
                }
                default:
                    break;
            }
        }

        if (arg.type.kind == Type::Kind::Ptr) {
            Value strVal = emitCallRet(Type(Type::Kind::Str), kObjectToString, {arg.value});
            return LowerResult{strVal, Type(Type::Kind::Str)};
        }

        return LowerResult{Value::constInt(0), Type(Type::Kind::Str)};
    }

    return std::nullopt;
}

//=============================================================================
// Main Call Expression Lowering
//=============================================================================

/// @brief Lower any Zia call expression.
/// @param expr Semantically analyzed call expression.
/// @return Call result paired with its surface IL representation.
/// @details Dispatches range modifier chains, Result constructors, generic
///          functions, resolved user methods, built-ins, closure invocation,
///          direct user functions, and canonical runtime functions. It applies
///          semantic argument bindings, receiver insertion, specialized
///          runtime result entry points, HTTP handler binding, ownership
///          effects, and managed return materialization as required by the
///          selected target.
LowerResult Lowerer::lowerCall(CallExpr *expr) {
    RangeModifierInfo rangeInfo;
    if (collectRangeModifierChain(expr, rangeInfo) && rangeInfo.range)
        return lowerRangeWithModifiers(rangeInfo.range, rangeInfo.reversed, rangeInfo.stepArg);

    if (auto *ident = exprAs<IdentExpr>(expr->callee.get(), ExprKind::Ident)) {
        if (ident->name == "Ok" || ident->name == "Err") {
            if (expr->args.empty())
                return {Value::null(), Type(Type::Kind::Ptr)};
            auto payload = lowerExpr(expr->args[0].value.get());
            TypeRef payloadType = sema_.typeOf(expr->args[0].value.get());
            Value argValue = payload.value;
            std::string callee;
            if (ident->name == "Ok") {
                callee = resultOkCalleeFor(payloadType);
                if (callee == kResultOk && payload.type.kind != Type::Kind::Ptr)
                    argValue = emitBoxValue(payload.value, payload.type, payloadType);
            } else {
                if (payloadType && payloadType->kind == TypeKindSem::String) {
                    callee = kResultErrStr;
                } else {
                    callee = kResultErr;
                    if (payload.type.kind != Type::Kind::Ptr)
                        argValue = emitBoxValue(payload.value, payload.type, payloadType);
                }
            }
            Value result = emitCallRet(Type(Type::Kind::Ptr), callee, {argValue});
            return {result, Type(Type::Kind::Ptr)};
        }
    }

    // Check for generic function call: identity[Integer](42)
    std::string genericCallee = sema_.genericFunctionCallee(expr);
    if (!genericCallee.empty()) {
        return lowerGenericFunctionCall(genericCallee, expr);
    }

    if (MethodDecl *resolvedMethod = sema_.resolvedMethodDecl(expr)) {
        std::string ownerType = sema_.resolvedMethodOwnerType(expr);
        std::string slotKey = sema_.resolvedMethodSlotKey(expr);

        if (auto *optionalCallee =
                exprAs<OptionalChainExpr>(expr->callee.get(), ExprKind::OptionalChain))
            return lowerOptionalMethodCall(optionalCallee, expr);

        FieldExpr *fieldExpr = exprAs<FieldExpr>(expr->callee.get(), ExprKind::Field);
        if (!fieldExpr) {
            if (auto *indexExpr = exprAs<IndexExpr>(expr->callee.get(), ExprKind::Index))
                fieldExpr = exprAs<FieldExpr>(indexExpr->base.get(), ExprKind::Field);
        }
        if (fieldExpr) {
            if (fieldExpr->base->kind == ExprKind::SuperExpr) {
                Value selfPtr;
                if (getSelfPtr(selfPtr))
                    return lowerMethodCall(resolvedMethod, ownerType, selfPtr, expr);
            }

            auto baseResult = lowerExpr(fieldExpr->base.get());
            TypeRef baseType = sema_.typeOf(fieldExpr->base.get());
            if (baseType && baseType->kind == TypeKindSem::Optional && baseType->innerType())
                baseType = baseType->innerType();

            if (sema_.genericMethodConcreteType(expr))
                return lowerMethodCall(resolvedMethod,
                                       ownerType.empty() ? (baseType ? baseType->name : "")
                                                         : ownerType,
                                       baseResult.value,
                                       expr);

            if (baseType && baseType->kind == TypeKindSem::Interface) {
                auto ifaceIt = interfaceTypes_.find(baseType->name);
                if (ifaceIt != interfaceTypes_.end())
                    return lowerInterfaceMethodCall(ifaceIt->second,
                                                    slotKey,
                                                    ownerType.empty() ? baseType->name : ownerType,
                                                    resolvedMethod,
                                                    baseResult.value,
                                                    expr);
            }

            if (baseType && baseType->kind == TypeKindSem::Class && !resolvedMethod->isStatic) {
                const ClassTypeInfo *entityInfoPtr = getOrCreateClassTypeInfo(baseType->name);
                if (entityInfoPtr) {
                    return lowerVirtualMethodCall(*entityInfoPtr,
                                                  slotKey,
                                                  ownerType.empty() ? baseType->name : ownerType,
                                                  resolvedMethod,
                                                  baseResult.value,
                                                  expr);
                }
            }

            return lowerMethodCall(resolvedMethod,
                                   ownerType.empty() ? (baseType ? baseType->name : "") : ownerType,
                                   baseResult.value,
                                   expr);
        }

        Value selfPtr;
        if (getSelfPtr(selfPtr)) {
            if (currentClassType_ && !resolvedMethod->isStatic)
                return lowerVirtualMethodCall(*currentClassType_,
                                              slotKey,
                                              ownerType.empty() ? currentClassType_->name
                                                                : ownerType,
                                              resolvedMethod,
                                              selfPtr,
                                              expr);

            std::string implicitOwner = ownerType;
            if (implicitOwner.empty()) {
                if (currentClassType_)
                    implicitOwner = currentClassType_->name;
                else if (currentStructType_)
                    implicitOwner = currentStructType_->name;
            }
            return lowerMethodCall(resolvedMethod, implicitOwner, selfPtr, expr);
        }
    }

    std::string resolvedFunction = sema_.resolvedFunctionCallee(expr);
    if (!resolvedFunction.empty()) {
        TypeRef calleeType = sema_.typeOf(expr->callee.get());
        TypeRef returnType = calleeType ? calleeType->returnType() : nullptr;
        Type ilReturnType = returnType ? mapType(returnType) : Type(Type::Kind::Void);
        TypeRef surfaceReturnType = sema_.typeOf(expr);
        Type surfaceIlReturnType = surfaceReturnType ? mapType(surfaceReturnType) : ilReturnType;

        std::vector<TypeRef> paramTypes;
        if (calleeType)
            paramTypes = calleeType->paramTypes();
        FunctionDecl *funcDecl = sema_.resolvedFunctionDecl(expr);
        std::vector<Value> args =
            lowerResolvedCallArgs(expr, paramTypes, funcDecl ? &funcDecl->params : nullptr);

        if (resolvedFunction == kHeapRelease && args.size() == 1) {
            TypeRef argType = sema_.typeOf(expr->args[0].value.get());
            return emitExplicitMemoryRelease(args[0], argType);
        }

        Symbol *externSym = sema_.findExternFunction(resolvedFunction);
        const bool isRuntimeExtern =
            externSym && externSym->kind == Symbol::Kind::Function && externSym->isExtern;

        if (surfaceIlReturnType.kind == Type::Kind::Void) {
            emitCall(resolvedFunction, args);
            return {Value::constInt(0), Type(Type::Kind::Void)};
        }

        if (isRuntimeExtern) {
            return emitRuntimeCallResult(resolvedFunction,
                                         surfaceReturnType ? surfaceReturnType : returnType,
                                         surfaceIlReturnType,
                                         args);
        }

        Value result = emitCallRet(ilReturnType, resolvedFunction, args);
        return materializeCallResult(
            result, surfaceReturnType ? surfaceReturnType : returnType, ilReturnType);
    }

    // Handle generic function calls that weren't detected during semantic analysis
    // This happens for calls inside generic function bodies like: identity[T](x)
    // where T is a type parameter that needs to be substituted
    if (expr->callee->kind == ExprKind::Index) {
        auto *indexExpr = static_cast<IndexExpr *>(expr->callee.get());
        if (indexExpr->base->kind == ExprKind::Ident) {
            auto *identExpr = static_cast<IdentExpr *>(indexExpr->base.get());
            // Check if this is a call to a generic function
            if (sema_.isGenericFunction(identExpr->name)) {
                // Get the type argument from the index expression
                if (indexExpr->index->kind == ExprKind::Ident) {
                    auto *typeArgExpr = static_cast<IdentExpr *>(indexExpr->index.get());
                    std::string typeArgName = typeArgExpr->name;

                    // If the type arg is a type parameter, substitute it
                    TypeRef substType = sema_.lookupTypeParam(typeArgName);
                    if (substType) {
                        // Use the type's name if it has one, otherwise use kindToString
                        typeArgName = substType->name.empty() ? kindToString(substType->kind)
                                                              : substType->name;
                    }

                    // Build the mangled name
                    std::string mangledName = identExpr->name + "$" + typeArgName;
                    return lowerGenericFunctionCall(mangledName, expr);
                }
            }
        }
    }

    // Check for method call on value or class type: obj.method()
    if (auto *fieldExpr = exprAs<FieldExpr>(expr->callee.get(), ExprKind::Field)) {
        // Check for super.method() call - dispatch to parent class method
        if (fieldExpr->base->kind == ExprKind::SuperExpr) {
            Value selfPtr;
            if (getSelfPtr(selfPtr) && currentClassType_ && !currentClassType_->baseClass.empty()) {
                auto parentIt = classTypes_.find(currentClassType_->baseClass);
                if (parentIt != classTypes_.end()) {
                    if (auto *method = parentIt->second.findMethod(fieldExpr->field)) {
                        return lowerMethodCall(method, currentClassType_->baseClass, selfPtr, expr);
                    }
                }
            }
        }

        // Get the type of the base expression
        TypeRef baseType = sema_.typeOf(fieldExpr->base.get());
        if (baseType) {
            // Unwrap Optional types for method resolution
            // This handles the case where a variable was assigned from an optional
            // after a null check (e.g., `var table = maybeTable;` after `if maybeTable == null {
            // return; }`)
            if (baseType->kind == TypeKindSem::Optional && baseType->innerType()) {
                baseType = baseType->innerType();
            }

            if (baseType->kind == TypeKindSem::Result) {
                TypeRef successType =
                    !baseType->typeArgs.empty() ? baseType->typeArgs[0] : types::unknown();
                auto baseResult = lowerExpr(fieldExpr->base.get());

                if (fieldExpr->field == "isOk") {
                    Value result =
                        emitCallRet(Type(Type::Kind::I1), kResultGetIsOk, {baseResult.value});
                    return {result, Type(Type::Kind::I1)};
                }
                if (fieldExpr->field == "isErr") {
                    Value result =
                        emitCallRet(Type(Type::Kind::I1), kResultGetIsErr, {baseResult.value});
                    return {result, Type(Type::Kind::I1)};
                }
                if (fieldExpr->field == "unwrap") {
                    Type ilSuccessType = mapType(successType);
                    const char *callee = resultUnwrapCalleeFor(successType);
                    Type runtimeReturn = ilSuccessType;
                    if (std::string_view(callee) == kResultUnwrap)
                        runtimeReturn = Type(Type::Kind::Ptr);
                    Value raw = emitCallRet(runtimeReturn, callee, {baseResult.value});
                    if (runtimeReturn.kind == ilSuccessType.kind)
                        return {raw, ilSuccessType};
                    return emitUnboxValue(raw, ilSuccessType, successType);
                }
                if (fieldExpr->field == "unwrapOr") {
                    auto def = lowerExpr(expr->args[0].value.get());
                    TypeRef defType = sema_.typeOf(expr->args[0].value.get());
                    auto coerced = coerceValueToType(def.value, def.type, defType, successType);
                    Value defaultValue = coerced.value;
                    const char *callee = resultUnwrapOrCalleeFor(successType);
                    Type ilSuccessType = mapType(successType);
                    Type runtimeReturn = ilSuccessType;
                    if (std::string_view(callee) == kResultUnwrapOr) {
                        runtimeReturn = Type(Type::Kind::Ptr);
                        if (coerced.type.kind != Type::Kind::Ptr)
                            defaultValue = emitBoxValue(coerced.value, coerced.type, successType);
                    }
                    Value raw =
                        emitCallRet(runtimeReturn, callee, {baseResult.value, defaultValue});
                    if (runtimeReturn.kind == ilSuccessType.kind)
                        return {raw, ilSuccessType};
                    return emitUnboxValue(raw, ilSuccessType, successType);
                }
                if (fieldExpr->field == "unwrapErr") {
                    Value result =
                        emitCallRet(Type(Type::Kind::Str), kResultUnwrapErrStr, {baseResult.value});
                    return {result, Type(Type::Kind::Str)};
                }
            }

            std::string typeName = baseType->name;

            // Check struct type methods
            const StructTypeInfo *valueInfo = getOrCreateStructTypeInfo(typeName);
            if (valueInfo) {
                if (auto *method = valueInfo->findMethod(fieldExpr->field)) {
                    auto baseResult = lowerExpr(fieldExpr->base.get());
                    return lowerMethodCall(method, typeName, baseResult.value, expr);
                }
            }

            // Check class type methods with virtual dispatch
            const ClassTypeInfo *entityInfoPtr = getOrCreateClassTypeInfo(typeName);
            if (entityInfoPtr) {
                const ClassTypeInfo &entityInfo = *entityInfoPtr;
                MethodDecl *namedMethod = entityInfo.findMethod(fieldExpr->field);

                if (namedMethod) {
                    std::string slotKey = sema_.methodSlotKey(typeName, namedMethod);
                    size_t vtableSlot = entityInfo.findVtableSlot(slotKey);
                    if (vtableSlot != SIZE_MAX) {
                        auto baseResult = lowerExpr(fieldExpr->base.get());
                        return lowerVirtualMethodCall(
                            entityInfo, slotKey, typeName, namedMethod, baseResult.value, expr);
                    }
                }

                if (namedMethod) {
                    auto baseResult = lowerExpr(fieldExpr->base.get());
                    return lowerMethodCall(namedMethod, typeName, baseResult.value, expr);
                }

                // Check parent class for inherited methods
                std::string parentName = entityInfo.baseClass;
                while (!parentName.empty()) {
                    auto parentIt = classTypes_.find(parentName);
                    if (parentIt == classTypes_.end())
                        break;
                    if (auto *method = parentIt->second.findMethod(fieldExpr->field)) {
                        auto baseResult = lowerExpr(fieldExpr->base.get());
                        return lowerMethodCall(method, parentName, baseResult.value, expr);
                    }
                    parentName = parentIt->second.baseClass;
                }

                // Class type found but method not in it or any parent — emit error
                diag_.report(
                    {il::support::Severity::Error,
                     "Class type '" + typeName + "' has no method '" + fieldExpr->field + "'",
                     expr->loc,
                     "V3100"});
                return {Value::constInt(0), Type(Type::Kind::Void)};
            }

            // Handle interface method calls
            if (baseType->kind == TypeKindSem::Interface) {
                auto ifaceIt = interfaceTypes_.find(typeName);
                if (ifaceIt != interfaceTypes_.end()) {
                    auto methodIt = ifaceIt->second.methodMap.find(fieldExpr->field);
                    if (methodIt != ifaceIt->second.methodMap.end()) {
                        auto baseResult = lowerExpr(fieldExpr->base.get());
                        std::string slotKey = sema_.methodSlotKey(typeName, methodIt->second);
                        return lowerInterfaceMethodCall(ifaceIt->second,
                                                        slotKey,
                                                        typeName,
                                                        methodIt->second,
                                                        baseResult.value,
                                                        expr);
                    }
                }
            }

            // Handle module-qualified function calls
            if (baseType->kind == TypeKindSem::Module) {
                // Check if sema resolved a runtime callee name for this call
                // (e.g., "ResultOk" for Zanna.Result.Ok)
                std::string funcName = sema_.runtimeCallee(expr);
                if (funcName.empty()) {
                    // Try qualified name for runtime functions (e.g., Zanna.Result.Ok)
                    std::string qualName = baseType->name + "." + fieldExpr->field;
                    if (il::runtime::findRuntimeDescriptor(qualName))
                        funcName = qualName;
                    else
                        funcName = fieldExpr->field; // user-defined module function
                }

                const auto *rtDesc = il::runtime::findRuntimeDescriptor(funcName);
                const auto *binding = sema_.callArgBinding(expr);
                std::vector<int> orderedSources = orderedArgSources(expr->args, binding);
                std::vector<Value> args = RuntimeCallBuilder(*this).lowerExplicitArgs(
                    expr->args, orderedSources, 0, rtDesc);

                TypeRef exprType = sema_.typeOf(expr);
                Type ilReturnType = exprType ? mapType(exprType) : Type(Type::Kind::Void);

                if (funcName == kHeapRelease && args.size() == 1) {
                    TypeRef argType = sema_.typeOf(expr->args[0].value.get());
                    return emitExplicitMemoryRelease(args[0], argType);
                }

                // Use the extern's declared return type for the call instruction
                // to match the function signature. The sema type may differ (e.g.,
                // String? maps to Ptr, but the extern returns str). We'll use the
                // extern type for the call and the sema type for the result.
                if (ilReturnType.kind == Type::Kind::Void) {
                    emitCall(funcName, args);
                    return {Value::constInt(0), Type(Type::Kind::Void)};
                } else {
                    return emitRuntimeCallResult(funcName, exprType, ilReturnType, args);
                }
            }

            // Handle String method calls - Bug #018 fix
            // String.length() should be treated as a property access, not a method call
            if (baseType->kind == TypeKindSem::String) {
                if (equalsIgnoreCase(fieldExpr->field, "length")) {
                    auto baseResult = lowerExpr(fieldExpr->base.get());
                    Value result =
                        emitCallRet(Type(Type::Kind::I64), kStringLength, {baseResult.value});
                    return {result, Type(Type::Kind::I64)};
                }
            }

            // Handle Integer method calls - Bug #018 fix
            // Integer.toString() should convert to string
            if (baseType->kind == TypeKindSem::Integer) {
                if (equalsIgnoreCase(fieldExpr->field, "toString")) {
                    auto baseResult = lowerExpr(fieldExpr->base.get());
                    Value result =
                        emitCallRet(Type(Type::Kind::Str), kStringFromInt, {baseResult.value});
                    return {result, Type(Type::Kind::Str)};
                }
            }

            // Handle Number method calls - Bug #018 fix
            // Number.toString() should convert to string
            if (baseType->kind == TypeKindSem::Number) {
                if (equalsIgnoreCase(fieldExpr->field, "toString")) {
                    auto baseResult = lowerExpr(fieldExpr->base.get());
                    Value result =
                        emitCallRet(Type(Type::Kind::Str), kStringFromNum, {baseResult.value});
                    return {result, Type(Type::Kind::Str)};
                }
            }

            // Handle List method calls
            if (baseType->kind == TypeKindSem::List) {
                auto baseResult = lowerExpr(fieldExpr->base.get());
                auto listResult =
                    lowerListMethodCall(baseResult.value, baseType, fieldExpr->field, expr);
                if (listResult)
                    return *listResult;
            }

            // Handle Map method calls
            if (baseType->kind == TypeKindSem::Map) {
                auto baseResult = lowerExpr(fieldExpr->base.get());
                auto mapResult =
                    lowerMapMethodCall(baseResult.value, baseType, fieldExpr->field, expr);
                if (mapResult)
                    return *mapResult;
            }

            // Handle Set method calls
            if (baseType->kind == TypeKindSem::Set) {
                auto baseResult = lowerExpr(fieldExpr->base.get());
                auto setResult =
                    lowerSetMethodCall(baseResult.value, baseType, fieldExpr->field, expr);
                if (setResult)
                    return *setResult;
            }
        }
    }

    // Check if this is a resolved runtime call
    std::string runtimeCallee = sema_.runtimeCallee(expr);
    if (!runtimeCallee.empty()) {
        // Auto-dispatch Say/Print to typed variants based on argument type.
        // This allows Say(42), Say(3.14), Say(true) to work without requiring
        // explicit string conversion. The typed runtime functions (SayInt, SayNum,
        // SayBool, PrintInt, PrintNum, PrintBool) already exist — we just redirect.
        if (isTerminalTextRuntime(runtimeCallee) && expr->args.size() == 1) {
            auto *argExpr = expr->args[0].value.get();
            TypeRef argType = sema_.typeOf(argExpr);

            if (argType && argType->kind != TypeKindSem::String) {
                std::string typedCallee;
                auto arg = lowerExpr(argExpr);
                Value argVal = arg.value;

                if (arg.type.kind == Type::Kind::I32) {
                    argVal = widenByteToInteger(argVal);
                }

                if (runtimeCallee == kTerminalSay) {
                    if (argType->kind == TypeKindSem::Integer ||
                        argType->kind == TypeKindSem::Byte || argType->kind == TypeKindSem::Enum)
                        typedCallee = kTerminalSayInt;
                    else if (argType->kind == TypeKindSem::Number)
                        typedCallee = kTerminalSayNum;
                    else if (argType->kind == TypeKindSem::Boolean)
                        typedCallee = kTerminalSayBool;
                } else if (runtimeCallee == kTerminalPrint) {
                    if (argType->kind == TypeKindSem::Integer ||
                        argType->kind == TypeKindSem::Byte || argType->kind == TypeKindSem::Enum)
                        typedCallee = kTerminalPrintInt;
                    else if (argType->kind == TypeKindSem::Number)
                        typedCallee = kTerminalPrintNum;
                    else if (argType->kind == TypeKindSem::Boolean)
                        typedCallee = kTerminalPrintBool;
                }

                if (!typedCallee.empty()) {
                    emitCall(typedCallee, {argVal});
                    return {Value::constInt(0), Type(Type::Kind::Void)};
                }

                emitCall(runtimeCallee, {emitToString(argVal, argType)});
                return {Value::constInt(0), Type(Type::Kind::Void)};
            }
        }

        const auto *rtDesc = il::runtime::findRuntimeDescriptor(runtimeCallee);
        const auto *binding = sema_.callArgBinding(expr);
        std::vector<int> orderedSources = orderedArgSources(expr->args, binding);

        std::vector<Value> args;

        if (auto *fieldExpr = exprAs<FieldExpr>(expr->callee.get(), ExprKind::Field)) {
            TypeRef baseType = sema_.typeOf(fieldExpr->base.get());
            if (isRuntimeReceiverSurfaceType(baseType) &&
                runtimeCallExpectsImplicitReceiver(rtDesc, binding, orderedSources.size())) {
                auto baseResult = lowerExpr(fieldExpr->base.get());
                args.push_back(baseResult.value);
            }
        }

        args.reserve(args.size() + orderedSources.size());
        size_t paramOffset = args.size(); // Account for implicit self parameter if present
        std::vector<Value> explicitArgs = RuntimeCallBuilder(*this).lowerExplicitArgs(
            expr->args, orderedSources, paramOffset, rtDesc);
        args.insert(args.end(), explicitArgs.begin(), explicitArgs.end());

        if ((isHttpServerRouteRuntime(runtimeCallee) || isHttpsServerRouteRuntime(runtimeCallee)) &&
            orderedSources.size() == 2 && args.size() >= 3) {
            size_t tagSourceIndex = toIndex(orderedSources[1]);
            if (auto *tagExpr = exprAs<StringLiteralExpr>(expr->args[tagSourceIndex].value.get(),
                                                          ExprKind::StringLiteral)) {
                std::string handlerTarget = httpHandlerTargetName(sema_, tagExpr->value);
                if (!handlerTarget.empty()) {
                    emitCall(httpServerBindHandlerTarget(runtimeCallee),
                             {args[0], args[paramOffset + 1], Value::global(handlerTarget)});
                }
            }
        }

        TypeRef exprType = sema_.typeOf(expr);
        Type ilReturnType = exprType ? mapType(exprType) : Type(Type::Kind::Void);

        if (runtimeCallee == kHeapRelease && args.size() == 1) {
            TypeRef argType = sema_.typeOf(expr->args[0].value.get());
            return emitExplicitMemoryRelease(args[0], argType);
        }

        // Use the extern's declared return type for the call instruction so it
        // matches the function signature. The sema type may differ for optional
        // returns (e.g., String? maps to Ptr, but the extern returns str).
        // Handle void return types correctly - don't try to store void results
        if (ilReturnType.kind == Type::Kind::Void) {
            emitCall(runtimeCallee, args);
            return {Value::constInt(0), Type(Type::Kind::Void)};
        } else {
            return emitRuntimeCallResult(runtimeCallee, exprType, ilReturnType, args);
        }
    }

    // Check for built-in functions
    if (auto *ident = exprAs<IdentExpr>(expr->callee.get(), ExprKind::Ident)) {
        auto builtinResult = lowerBuiltinCall(ident->name, expr);
        if (builtinResult)
            return *builtinResult;
    }

    // Handle direct or indirect function calls
    std::string calleeName;
    bool isIndirectCall = false;
    Value funcPtr;

    TypeRef calleeType = sema_.typeOf(expr->callee.get());
    bool isLambdaClosure = calleeType && calleeType->isCallable();

    if (auto *ident = exprAs<IdentExpr>(expr->callee.get(), ExprKind::Ident)) {
        // Check for implicit method call
        if (currentClassType_) {
            if (auto *method = currentClassType_->findMethod(ident->name)) {
                Value selfPtr;
                if (getSelfPtr(selfPtr)) {
                    return lowerMethodCall(method, currentClassType_->name, selfPtr, expr);
                }
            }
        }

        // Check if this is a variable holding a function pointer
        if (definedFunctions_.find(mangleFunctionName(ident->name)) == definedFunctions_.end()) {
            auto slotIt = slots_.find(ident->name);
            if (slotIt != slots_.end()) {
                unsigned loadId = nextTempId();
                il::core::Instr loadInstr;
                loadInstr.result = loadId;
                loadInstr.op = Opcode::Load;
                loadInstr.type = Type(Type::Kind::Ptr);
                loadInstr.operands = {slotIt->second};
                loadInstr.loc = curLoc_;
                blockMgr_.currentBlock()->instructions.push_back(loadInstr);
                funcPtr = Value::temp(loadId);
                isIndirectCall = true;
            } else {
                auto localIt = locals_.find(ident->name);
                if (localIt != locals_.end()) {
                    funcPtr = localIt->second;
                    isIndirectCall = true;
                }
            }
        }

        if (!isIndirectCall) {
            calleeName = mangleFunctionName(ident->name);
        }
    } else if (exprAs<FieldExpr>(expr->callee.get(), ExprKind::Field) != nullptr) {
        // Check if this is a namespace-qualified function call (e.g., Math.add or
        // Outer.Inner.getValue) Recursively build the qualified name from nested FieldExpr nodes
        std::string qualifiedName;
        /// @brief Recursively appends an identifier/field callee to `qualifiedName`.
        /// @param e Callee expression node.
        /// @return `true` when the expression is a pure qualified-name chain.
        std::function<bool(Expr *)> buildQualifiedName = [&](Expr *e) -> bool {
            if (auto *identLeaf = exprAs<IdentExpr>(e, ExprKind::Ident)) {
                qualifiedName = identLeaf->name;
                return true;
            }
            if (auto *field = exprAs<FieldExpr>(e, ExprKind::Field)) {
                if (buildQualifiedName(field->base.get())) {
                    qualifiedName += "." + field->field;
                    return true;
                }
            }
            return false;
        };
        buildQualifiedName(expr->callee.get());

        // Check if the qualified name is a defined function.
        // Try both mangled and unmangled names (mirrors line 596 which
        // correctly uses mangleFunctionName for ident-based calls).
        if (!qualifiedName.empty()) {
            std::string mangledQN = mangleFunctionName(qualifiedName);
            if (definedFunctions_.find(mangledQN) != definedFunctions_.end()) {
                calleeName = mangledQN;
                isIndirectCall = false;
            } else if (definedFunctions_.find(qualifiedName) != definedFunctions_.end()) {
                calleeName = qualifiedName;
                isIndirectCall = false;
            } else {
                // Regular field access on a value - lower and use as indirect call
                auto calleeResult = lowerExpr(expr->callee.get());
                funcPtr = calleeResult.value;
                isIndirectCall = true;
            }
        } else {
            // Regular field access on a value - lower and use as indirect call
            auto calleeResult = lowerExpr(expr->callee.get());
            funcPtr = calleeResult.value;
            isIndirectCall = true;
        }
    } else {
        auto calleeResult = lowerExpr(expr->callee.get());
        funcPtr = calleeResult.value;
        isIndirectCall = true;
    }

    // Prefer the call expression's fully resolved semantic type. The callee's
    // function type can expose an unwrapped/generic declaration type (notably
    // for Optional<Struct>), which would lose the boxed-result ownership fact.
    TypeRef returnType = sema_.typeOf(expr);
    if (!returnType || returnType->kind == TypeKindSem::Unknown ||
        returnType->kind == TypeKindSem::Error) {
        returnType = calleeType ? calleeType->returnType() : nullptr;
    }
    Type ilReturnType = returnType ? mapType(returnType) : Type(Type::Kind::Void);

    // Lower arguments
    std::vector<TypeRef> paramTypes;
    if (calleeType)
        paramTypes = calleeType->paramTypes();

    std::vector<Value> args;
    args.reserve(expr->args.size());
    for (size_t i = 0; i < expr->args.size(); ++i) {
        auto &arg = expr->args[i];
        auto result = lowerExpr(arg.value.get());
        Value argValue = result.value;

        if (i < paramTypes.size()) {
            TypeRef paramType = paramTypes[i];
            TypeRef argType = sema_.typeOf(arg.value.get());
            auto coerced = coerceValueToType(argValue, result.type, argType, paramType);
            argValue = coerced.value;
        }

        args.push_back(argValue);
    }

    if (isIndirectCall) {
        if (isLambdaClosure) {
            Value closurePtr = funcPtr;
            Value actualFuncPtr = emitLoad(closurePtr, Type(Type::Kind::Ptr));
            Value envFieldAddr = emitGEP(closurePtr, kClosureEnvOffset);
            Value envPtr = emitLoad(envFieldAddr, Type(Type::Kind::Ptr));

            std::vector<Value> closureArgs;
            closureArgs.reserve(args.size() + 1);
            closureArgs.push_back(envPtr);
            for (const auto &arg : args) {
                closureArgs.push_back(arg);
            }

            if (ilReturnType.kind == Type::Kind::Void) {
                emitCallIndirect(actualFuncPtr, closureArgs);
                return {Value::constInt(0), Type(Type::Kind::Void)};
            } else {
                Value result = emitCallIndirectRet(ilReturnType, actualFuncPtr, closureArgs);
                return materializeCallResult(result, returnType, ilReturnType);
            }
        } else {
            if (ilReturnType.kind == Type::Kind::Void) {
                emitCallIndirect(funcPtr, args);
                return {Value::constInt(0), Type(Type::Kind::Void)};
            } else {
                Value result = emitCallIndirectRet(ilReturnType, funcPtr, args);
                return materializeCallResult(result, returnType, ilReturnType);
            }
        }
    } else {
        // Pack variadic arguments if the callee has a variadic last param.
        {
            FunctionDecl *vDecl = sema_.resolvedFunctionDecl(expr);
            if (!vDecl)
                vDecl = sema_.getFunctionDecl(calleeName);
            if (!vDecl && expr->callee->kind == ExprKind::Ident) {
                auto *ident = static_cast<IdentExpr *>(expr->callee.get());
                vDecl = sema_.getFunctionDecl(ident->name);
            }
            if (vDecl && !vDecl->params.empty() && vDecl->params.back().isVariadic) {
                size_t fixedCount = vDecl->params.size() - 1;
                Value list = emitCallRet(Type(Type::Kind::Ptr), kListNew, {});
                for (size_t vi = fixedCount; vi < args.size(); ++vi) {
                    TypeRef argType = (vi < expr->args.size())
                                          ? sema_.typeOf(expr->args[vi].value.get())
                                          : nullptr;
                    Type ilArgType = argType ? mapType(argType) : Type(Type::Kind::I64);
                    Value boxed = emitBoxValue(args[vi], ilArgType, argType);
                    emitCall(kListAdd, {list, boxed});
                }
                args.erase(args.begin() + static_cast<ptrdiff_t>(fixedCount), args.end());
                args.push_back(list);
            }
        }

        // Pad missing trailing arguments with default values from function declaration
        padDefaultArgs(calleeName, args, expr);

        if (ilReturnType.kind == Type::Kind::Void) {
            emitCall(calleeName, args);
            return {Value::constInt(0), Type(Type::Kind::Void)};
        } else {
            Value result = emitCallRet(ilReturnType, calleeName, args);
            return materializeCallResult(result, returnType, ilReturnType);
        }
    }
}

/// @brief Emit a runtime call and adapt its ABI return to the Zia surface type.
/// @param calleeName Canonical generic runtime target.
/// @param surfaceType Semantic type expected by the call expression.
/// @param ilSurfaceType IL representation of @p surfaceType.
/// @param callArgs ABI-ordered call arguments.
/// @return Runtime result in the requested surface representation.
/// @details Selects type-specialized collection accessors where available,
///          narrows arguments required by `i16`/`i32` ABI parameters, and
///          unboxes pointer-returning helpers when the surface value is
///          primitive.
LowerResult Lowerer::emitRuntimeCallResult(const std::string &calleeName,
                                           TypeRef surfaceType,
                                           Type ilSurfaceType,
                                           const std::vector<Value> &callArgs) {
    std::string effectiveCallee = calleeName;
    Type callReturnType = ilSurfaceType;
    if (const auto *desc = il::runtime::findRuntimeDescriptor(effectiveCallee))
        callReturnType = desc->signature.retType;

    if (std::string specialized = specializedRuntimeReturnCallee(calleeName, surfaceType);
        !specialized.empty()) {
        effectiveCallee = specialized;
        if (const auto *specializedDesc = il::runtime::findRuntimeDescriptor(effectiveCallee))
            callReturnType = specializedDesc->signature.retType;
        else
            callReturnType = ilSurfaceType;
    }

    // Zia integer expressions are uniformly i64, but some runtime entry
    // points declare narrow i16/i32 parameters (e.g. String.FromI16/FromI32).
    // Insert checked narrowing conversions so those calls verify instead of
    // reaching the IL verifier with mismatched argument types (VDOC-163).
    std::vector<Value> adjustedArgs = callArgs;
    if (const auto *desc = il::runtime::findRuntimeDescriptor(effectiveCallee)) {
        const auto &params = desc->signature.paramTypes;
        if (params.size() == adjustedArgs.size()) {
            for (size_t i = 0; i < params.size(); ++i) {
                if (params[i].kind == Type::Kind::I16 || params[i].kind == Type::Kind::I32)
                    adjustedArgs[i] =
                        emitUnary(Opcode::CastSiNarrowChk, params[i], adjustedArgs[i]);
            }
        }
    }

    Value rawResult = emitCallRet(callReturnType, effectiveCallee, adjustedArgs);
    if (callReturnType.kind == ilSurfaceType.kind)
        return {rawResult, ilSurfaceType};

    if (callReturnType.kind == Type::Kind::Ptr && ilSurfaceType.kind != Type::Kind::Ptr)
        return emitUnboxValue(rawResult, ilSurfaceType, surfaceType);

    return {rawResult, ilSurfaceType};
}

/// @brief Lower an explicit unsafe release operation.
/// @param argValue Managed handle whose caller-owned reference is consumed.
/// @param argType Semantic type of @p argValue.
/// @return Post-release reference count as `i64`.
/// @details Cancels deferred cleanup for fresh owned arguments, uses the
///          string-specific release helper for strings, preserves Zia class
///          destructor dispatch, and otherwise calls the generic heap release
///          runtime.
LowerResult Lowerer::emitExplicitMemoryRelease(Value argValue, TypeRef argType) {
    // Unsafe.Release consumes the caller's reference.  If the argument is a
    // freshly produced owned value, transfer it to the explicit release so it
    // is not also emitted by statement-boundary deferred cleanup.
    consumeDeferred(argValue);

    if (isStringType(argType)) {
        Value releaseCount = emitCallRet(Type(Type::Kind::I64), kHeapReleaseStr, {argValue});
        return {releaseCount, Type(Type::Kind::I64)};
    }

    TypeRef unwrapped = unwrapSurfaceType(argType);
    const bool isUserClass = unwrapped && unwrapped->kind == TypeKindSem::Class &&
                             classTypes_.find(unwrapped->name) != classTypes_.end();
    if (isUserClass) {
        Value releaseCount = emitManagedReleaseRet(argValue, false);
        return {releaseCount, Type(Type::Kind::I64)};
    }

    Value releaseCount = emitCallRet(Type(Type::Kind::I64), kHeapRelease, {argValue});
    return {releaseCount, Type(Type::Kind::I64)};
}

//=============================================================================
// Default Parameter Padding
//=============================================================================

/// @brief Append lowered trailing default arguments for a direct function call.
/// @param calleeName Resolved or mangled function name.
/// @param args Existing argument vector, modified in place.
/// @param callExpr Original call used for pre-mangling declaration fallback.
/// @details Stops at the first missing default and does nothing when the
///          function declaration cannot be resolved.
void Lowerer::padDefaultArgs(const std::string &calleeName,
                             std::vector<Value> &args,
                             CallExpr *callExpr) {
    FunctionDecl *funcDecl = sema_.getFunctionDecl(calleeName);
    // Fall back to original ident name (before mangling)
    if (!funcDecl && callExpr->callee->kind == ExprKind::Ident) {
        auto *ident = static_cast<IdentExpr *>(callExpr->callee.get());
        funcDecl = sema_.getFunctionDecl(ident->name);
    }
    if (!funcDecl)
        return;

    size_t numParams = funcDecl->params.size();
    size_t numArgs = args.size();
    if (numArgs >= numParams)
        return;

    // Pad missing trailing arguments with their default values
    for (size_t i = numArgs; i < numParams; ++i) {
        const auto &param = funcDecl->params[i];
        if (!param.defaultValue)
            break; // No default — shouldn't happen if sema validated

        auto result = lowerExpr(param.defaultValue.get());
        args.push_back(result.value);
    }
}

//=============================================================================
// Generic Function Call Lowering
//=============================================================================

/// @brief Lower a call to one concrete generic-function instantiation.
/// @param mangledName Substitution-keyed callee name containing `$`.
/// @param expr Generic call expression.
/// @return Materialized call result and instantiated IL return type.
/// @details Resolves the substituted function type when available, otherwise
///          temporarily pushes the substitution context to recover the return
///          type. Arguments follow the semantic binding, and the instantiation
///          is queued for later body emission when not already defined.
LowerResult Lowerer::lowerGenericFunctionCall(const std::string &mangledName, CallExpr *expr) {
    const size_t dollarPos = mangledName.find('$');
    if (dollarPos == std::string::npos) {
        reportLoweringInvariant(expr ? expr->loc : SourceLoc{},
                                "V-ZIA-LOWER-BAD-GENERIC-CALLEE",
                                "generic callee '" + mangledName +
                                    "' is missing type-argument delimiter");
        return {Value::constInt(0), Type(Type::Kind::I64)};
    }
    const std::string baseName = mangledName.substr(0, dollarPos);

    // Get the function type from Sema
    TypeRef funcType = sema_.typeOf(expr->callee.get());
    if (!funcType || funcType->kind != TypeKindSem::Function) {
        // Fallback - compute return type from generic function declaration
        FunctionDecl *genericDecl = sema_.getGenericFunction(baseName);

        Type ilReturnType = Type(Type::Kind::I64); // Default fallback
        bool pushedContext = sema_.pushSubstitutionContext(mangledName);
        if (genericDecl) {
            // Resolve return type from declaration and substitute type parameters
            if (genericDecl->returnType) {
                TypeRef declReturnType = sema_.resolveType(genericDecl->returnType.get());
                if (declReturnType) {
                    ilReturnType = mapType(declReturnType);
                }
            } else {
                ilReturnType = Type(Type::Kind::Void);
            }
        }
        if (pushedContext)
            sema_.popTypeParams();

        // Lower arguments
        std::vector<Value> args;
        for (auto &arg : expr->args) {
            auto result = lowerExpr(arg.value.get());
            args.push_back(result.value);
        }

        // Queue the instantiated generic function for later lowering
        if (genericDecl && definedFunctions_.find(mangledName) == definedFunctions_.end()) {
            // Mark as defined now to avoid re-queuing, but queue for actual lowering
            definedFunctions_.insert(mangledName);
            pendingFunctionInstantiations_.push_back({mangledName, genericDecl});
        }

        // Call the function
        if (ilReturnType.kind == Type::Kind::Void) {
            emitCall(mangledName, args);
            return {Value::constInt(0), Type(Type::Kind::Void)};
        } else {
            Value result = emitCallRet(ilReturnType, mangledName, args);
            TypeRef returnType = nullptr;
            if (genericDecl && genericDecl->returnType) {
                returnType = sema_.resolveType(genericDecl->returnType.get());
            }
            return materializeCallResult(result, returnType, ilReturnType);
        }
    }

    // We have proper function type info
    TypeRef returnType = funcType->returnType();
    Type ilReturnType = returnType ? mapType(returnType) : Type(Type::Kind::Void);

    // Lower arguments
    const auto &paramTypes = funcType->paramTypes();
    FunctionDecl *genericDecl = sema_.getGenericFunction(baseName);
    std::vector<Value> args =
        lowerResolvedCallArgs(expr, paramTypes, genericDecl ? &genericDecl->params : nullptr);

    // Queue the instantiated generic function for later lowering
    if (genericDecl && definedFunctions_.find(mangledName) == definedFunctions_.end()) {
        // Mark as defined now to avoid re-queuing, but queue for actual lowering
        definedFunctions_.insert(mangledName);
        pendingFunctionInstantiations_.push_back({mangledName, genericDecl});
    }

    // Call the function
    if (ilReturnType.kind == Type::Kind::Void) {
        emitCall(mangledName, args);
        return {Value::constInt(0), Type(Type::Kind::Void)};
    } else {
        Value result = emitCallRet(ilReturnType, mangledName, args);
        return materializeCallResult(result, returnType, ilReturnType);
    }
}

} // namespace il::frontends::zia
