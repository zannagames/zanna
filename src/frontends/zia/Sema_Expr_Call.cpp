//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Sema_Expr_Call.cpp
/// @brief Call expression analysis and collection method resolution for the
///        Zia semantic analyzer.
/// @details Resolves direct, qualified, generic, runtime, constructor-style, collection, and
///          optional method calls. It also binds named/default/variadic arguments, enforces safe
///          runtime boundaries, and refines registry-erased collection result types.
///
//===----------------------------------------------------------------------===//

#include "frontends/common/CollectionMethodCatalog.hpp"
#include "frontends/common/StringUtils.hpp"
#include "frontends/zia/RuntimeAdapter.hpp"
#include "frontends/zia/RuntimeNames.hpp"
#include "frontends/zia/Sema.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <string_view>

using il::frontends::common::string_utils::iequals;

namespace il::frontends::zia {

//=============================================================================
// Collection Method Resolution Helpers
//=============================================================================

namespace {

/// @brief Convert a source member spelling to the runtime catalog's leading-capital form.
/// @param name Source-visible method name.
/// @return Copy with its first byte uppercased when nonempty.
std::string capitalizedRuntimeMember(std::string_view name) {
    std::string out(name);
    if (!out.empty())
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

/// @brief Determine whether a runtime method symbol exposes a leading receiver parameter.
/// @param sym Backing extern function symbol.
/// @param method Parsed runtime member metadata.
/// @return One when the symbol has an extra leading receiver or metadata is unavailable; zero
///         when symbol and surface arities already match.
size_t runtimeMethodReceiverSkip(const Symbol *sym, const il::runtime::ParsedMethod *method) {
    if (!sym || !sym->type || sym->type->kind != TypeKindSem::Function || !method)
        return 1;

    const size_t symbolArity = sym->type->paramTypes().size();
    const size_t surfaceArity = method->signature.params.size();
    return symbolArity == surfaceArity + 1 ? 1 : 0;
}

/// @brief Resolve a return type from a collection method return category.
/// @param kind The return kind.
/// @param baseType The collection type (for element/key/struct type resolution).
/// @return The resolved type.
TypeRef resolveMethodReturnType(common::CollectionReturnKind kind, TypeRef baseType) {
    using common::CollectionReturnKind;
    switch (kind) {
        case CollectionReturnKind::ElementType:
            return baseType->elementType() ? baseType->elementType() : types::unknown();
        case CollectionReturnKind::KeyType:
            return baseType->keyType() ? baseType->keyType() : types::unknown();
        case CollectionReturnKind::ValueType:
            return baseType->valueType() ? baseType->valueType() : types::unknown();
        case CollectionReturnKind::OptionalValueType:
            return baseType->valueType() ? types::optional(baseType->valueType())
                                         : types::optional(types::unknown());
        case CollectionReturnKind::KeySeqType:
            return types::seqOf(baseType->keyType() ? baseType->keyType() : types::string());
        case CollectionReturnKind::ValueSeqType:
            return types::seqOf(baseType->valueType() ? baseType->valueType() : types::unknown());
        case CollectionReturnKind::Integer:
            return types::integer();
        case CollectionReturnKind::Boolean:
            return types::boolean();
        case CollectionReturnKind::Void:
            return types::voidType();
        case CollectionReturnKind::Unknown:
        default:
            return types::unknown();
    }
}

/// @brief Try to extract a dotted name from a field access chain.
/// @param expr The expression to extract from.
/// @param out The output string to append to.
/// @return True if successful, false otherwise.
static bool extractDottedName(Expr *expr, std::string &out) {
    if (!expr)
        return false;
    if (expr->kind == ExprKind::Ident) {
        auto *ident = static_cast<IdentExpr *>(expr);
        out = ident->name;
        return true;
    }
    if (expr->kind == ExprKind::Field) {
        auto *fieldExpr = static_cast<FieldExpr *>(expr);
        if (!extractDottedName(fieldExpr->base.get(), out))
            return false;
        out += ".";
        out += fieldExpr->field;
        return true;
    }
    return false;
}

/// @brief Walk a `range.step(n).rev()`-style modifier chain back to its base
///        Range, tallying `.step` calls in @p stepCount.
/// @param expr Candidate chain node.
/// @param stepCount Accumulator incremented for every `.step` modifier.
/// @param depth Current recursion depth used to enforce a defensive bound.
/// @return true if @p expr is a (possibly modified) range expression.
static bool inspectRangeModifierChain(const Expr *expr, unsigned &stepCount, unsigned depth = 0) {
    // The parser already bounds expression nesting, but guard the recursion
    // explicitly so a pathological chain can never overflow the stack.
    if (!expr || depth > 1024)
        return false;
    if (expr->kind == ExprKind::Range)
        return true;
    if (expr->kind != ExprKind::Call)
        return false;

    const auto *call = static_cast<const CallExpr *>(expr);
    if (!call->callee || call->callee->kind != ExprKind::Field)
        return false;

    const auto *field = static_cast<const FieldExpr *>(call->callee.get());
    if (field->field == "rev" && call->args.empty())
        return inspectRangeModifierChain(field->base.get(), stepCount, depth + 1);
    if (field->field == "step" && call->args.size() == 1) {
        ++stepCount;
        return inspectRangeModifierChain(field->base.get(), stepCount, depth + 1);
    }
    return false;
}

/// @brief Public wrapper over @ref inspectRangeModifierChain; optionally
///        reports the number of `.step` modifiers via @p stepCountOut.
/// @param expr Candidate range-modifier chain.
/// @param stepCountOut Optional destination for the number of `.step` calls.
/// @return True when the chain terminates at a Range and contains only supported modifiers.
static bool isRangeModifierChain(const Expr *expr, unsigned *stepCountOut = nullptr) {
    unsigned stepCount = 0;
    bool ok = inspectRangeModifierChain(expr, stepCount);
    if (stepCountOut)
        *stepCountOut = stepCount;
    return ok;
}

/// @brief Flatten an identifier / dotted-field expression into its dotted
///        type-name string (e.g. `Zanna.GUI.Canvas`) in @p out.
/// @param expr Candidate type-name expression.
/// @param out Destination overwritten or extended with the dotted spelling.
/// @return false if @p expr is not a pure name/field chain.
static bool exprToTypeName(const Expr *expr, std::string &out) {
    if (!expr)
        return false;
    if (expr->kind == ExprKind::Ident) {
        out = static_cast<const IdentExpr *>(expr)->name;
        return true;
    }
    if (expr->kind == ExprKind::Field) {
        const auto *fieldExpr = static_cast<const FieldExpr *>(expr);
        if (!exprToTypeName(fieldExpr->base.get(), out))
            return false;
        out += ".";
        out += fieldExpr->field;
        return true;
    }
    return false;
}

static TypePtr exprToTypeNode(const Expr *expr);

/// @brief Collect generic type-argument nodes from @p expr into @p out,
///        flattening a tuple of args (`Foo[(A, B)]`) into separate entries.
/// @param expr Expression encoding one type argument or a tuple of arguments.
/// @param out Destination type-node list.
static void collectTypeArgNodes(const Expr *expr, std::vector<TypePtr> &out) {
    if (!expr)
        return;
    if (expr->kind == ExprKind::Tuple) {
        const auto *tuple = static_cast<const TupleExpr *>(expr);
        for (const auto &elem : tuple->elements) {
            if (TypePtr typeNode = exprToTypeNode(elem.get()))
                out.push_back(std::move(typeNode));
        }
        return;
    }

    if (TypePtr typeNode = exprToTypeNode(expr))
        out.push_back(std::move(typeNode));
}

/// @brief Reinterpret an expression that is syntactically a type (a name, a
///        dotted name, or `Name[Args]`) as a TypeNode, for explicit
///        type-argument syntax at call sites. Returns nullptr if not a type.
/// @param expr Candidate expression.
/// @return Newly allocated named or generic type node, or nullptr when the syntax is not a type.
static TypePtr exprToTypeNode(const Expr *expr) {
    if (!expr)
        return nullptr;

    if (expr->kind == ExprKind::Index) {
        const auto *indexExpr = static_cast<const IndexExpr *>(expr);
        std::string name;
        if (!exprToTypeName(indexExpr->base.get(), name))
            return nullptr;

        std::vector<TypePtr> args;
        collectTypeArgNodes(indexExpr->index.get(), args);
        if (args.empty())
            return nullptr;
        return std::make_unique<GenericType>(expr->loc, name, std::move(args));
    }

    std::string name;
    if (exprToTypeName(expr, name))
        return std::make_unique<NamedType>(expr->loc, name);

    return nullptr;
}

/// @brief Bind generic @p typeParamName to @p argType during inference;
///        an unknown arg is ignored. If already bound, succeeds only when the
///        new binding is consistent with the existing one.
/// @param typeParamName Generic parameter being inferred.
/// @param argType Concrete argument type.
/// @param inferredTypes Mutable inference map.
/// @return True when the binding is absent, new, or equal to its prior value.
static bool bindInferredType(const std::string &typeParamName,
                             TypeRef argType,
                             std::map<std::string, TypeRef> &inferredTypes) {
    if (!argType || argType->kind == TypeKindSem::Unknown)
        return true;

    auto it = inferredTypes.find(typeParamName);
    if (it == inferredTypes.end()) {
        inferredTypes[typeParamName] = argType;
        return true;
    }

    return it->second && it->second->equals(*argType);
}

/// @brief Structurally match a generic parameter's declared type pattern
///        @p paramNode against the concrete @p argType, binding any type
///        parameters in @p typeParamNames into @p inferredTypes.
/// @param paramNode Declared parameter type pattern.
/// @param argType Concrete argument type being matched.
/// @param typeParamNames Generic parameter names eligible for inference.
/// @param inferredTypes Mutable inferred substitution map.
/// @return false on a structural conflict that defeats inference.
static bool inferTypeParamsFromPattern(const TypeNode *paramNode,
                                       TypeRef argType,
                                       const std::set<std::string> &typeParamNames,
                                       std::map<std::string, TypeRef> &inferredTypes) {
    if (!paramNode || !argType || argType->kind == TypeKindSem::Unknown)
        return true;

    switch (paramNode->kind) {
        case TypeKind::Named: {
            const auto *named = static_cast<const NamedType *>(paramNode);
            if (typeParamNames.count(named->name) == 0)
                return true;
            return bindInferredType(named->name, argType, inferredTypes);
        }

        case TypeKind::Generic: {
            const auto *generic = static_cast<const GenericType *>(paramNode);
            std::vector<TypeRef> argTypeArgs;
            if (generic->name == "List") {
                if (argType->kind != TypeKindSem::List || !argType->elementType())
                    return false;
                argTypeArgs.push_back(argType->elementType());
            } else if (generic->name == "Set") {
                if (argType->kind != TypeKindSem::Set || !argType->elementType())
                    return false;
                argTypeArgs.push_back(argType->elementType());
            } else if (generic->name == "Map") {
                if (argType->kind != TypeKindSem::Map || !argType->keyType() ||
                    !argType->valueType())
                    return false;
                argTypeArgs.push_back(argType->keyType());
                argTypeArgs.push_back(argType->valueType());
            } else if (generic->name == "Result") {
                if (argType->kind != TypeKindSem::Result || argType->typeArgs.empty())
                    return false;
                argTypeArgs.push_back(argType->typeArgs[0]);
            } else {
                if (argType->typeArgs.size() != generic->args.size())
                    return false;
                argTypeArgs = argType->typeArgs;
            }

            if (generic->args.size() != argTypeArgs.size())
                return false;

            for (size_t i = 0; i < generic->args.size(); ++i) {
                if (!inferTypeParamsFromPattern(
                        generic->args[i].get(), argTypeArgs[i], typeParamNames, inferredTypes))
                    return false;
            }
            return true;
        }

        case TypeKind::Optional: {
            const auto *opt = static_cast<const OptionalType *>(paramNode);
            if (argType->kind != TypeKindSem::Optional || !argType->innerType())
                return false;
            return inferTypeParamsFromPattern(
                opt->inner.get(), argType->innerType(), typeParamNames, inferredTypes);
        }

        case TypeKind::Tuple: {
            const auto *tuple = static_cast<const TupleType *>(paramNode);
            if (argType->kind != TypeKindSem::Tuple ||
                tuple->elements.size() != argType->typeArgs.size())
                return false;
            for (size_t i = 0; i < tuple->elements.size(); ++i) {
                if (!inferTypeParamsFromPattern(tuple->elements[i].get(),
                                                argType->typeArgs[i],
                                                typeParamNames,
                                                inferredTypes))
                    return false;
            }
            return true;
        }

        case TypeKind::Function: {
            const auto *func = static_cast<const FunctionType *>(paramNode);
            auto argParams = argType->paramTypes();
            if (argType->kind != TypeKindSem::Function || func->params.size() != argParams.size())
                return false;
            for (size_t i = 0; i < func->params.size(); ++i) {
                if (!inferTypeParamsFromPattern(
                        func->params[i].get(), argParams[i], typeParamNames, inferredTypes))
                    return false;
            }
            return inferTypeParamsFromPattern(
                func->returnType.get(), argType->returnType(), typeParamNames, inferredTypes);
        }

        case TypeKind::FixedArray: {
            const auto *arr = static_cast<const FixedArrayType *>(paramNode);
            if (argType->kind != TypeKindSem::FixedArray || argType->elementCount != arr->count ||
                !argType->elementType())
                return false;
            return inferTypeParamsFromPattern(
                arr->elementType.get(), argType->elementType(), typeParamNames, inferredTypes);
        }
    }

    return true;
}

/// @brief True if @p calleeName is a Terminal text-output runtime (Say/Print)
///        whose argument may be auto-stringified.
/// @param calleeName Fully qualified runtime function name.
/// @return True for the two terminal text-output entry points.
bool isTerminalTextRuntime(std::string_view calleeName) {
    return calleeName == runtime::kTerminalSay || calleeName == runtime::kTerminalPrint;
}

/// @brief True if @p type can be implicitly converted to text for a Terminal
///        Say/Print call (string and the scalar kinds with a string form).
/// @param type Candidate argument type.
/// @return True when the lowerer can produce a stable textual representation.
bool canAutoStringifyForTerminal(TypeRef type) {
    if (!type)
        return false;
    switch (type->kind) {
        case TypeKindSem::String:
        case TypeKindSem::Integer:
        case TypeKindSem::Number:
        case TypeKindSem::Boolean:
        case TypeKindSem::Byte:
        case TypeKindSem::Enum:
        case TypeKindSem::List:
        case TypeKindSem::Map:
        case TypeKindSem::Set:
        case TypeKindSem::Struct:
        case TypeKindSem::Class:
        case TypeKindSem::Interface:
        case TypeKindSem::Error:
        case TypeKindSem::Ptr:
            return true;
        default:
            return false;
    }
}

} // anonymous namespace

//=============================================================================
// Call Argument Validation
//=============================================================================

/// @brief Validate arguments for a function value without declaration-specific binding metadata.
/// @param expr Call expression whose arguments were already analyzed.
/// @param funcType Callable semantic type.
/// @param calleeName Display name used in diagnostics and declaration lookup.
/// @details Rejects named arguments, validates arity including declaration defaults/variadics,
///          and checks each fixed or variadic argument against its target type.
void Sema::validateCallArgs(CallExpr *expr, TypeRef funcType, const std::string &calleeName) {
    if (!funcType || funcType->kind != TypeKindSem::Function)
        return;

    for (const auto &arg : expr->args) {
        if (arg.name) {
            error(arg.value ? arg.value->loc : expr->loc,
                  "Named arguments require a direct function, method, or constructor declaration");
            return;
        }
    }

    const auto paramTys = funcType->paramTypes();
    const size_t numParams = paramTys.size();
    const size_t numArgs = expr->args.size();

    // Check if the last parameter is variadic (typed as List[T] from a ...T declaration)
    FunctionDecl *funcDecl = getFunctionDecl(calleeName);
    bool hasVariadic = funcDecl && !funcDecl->params.empty() && funcDecl->params.back().isVariadic;
    size_t fixedParams = hasVariadic ? numParams - 1 : numParams;

    // Check argument count (with default parameter and variadic support)
    if (!hasVariadic && numArgs > numParams) {
        error(expr->loc,
              "Too many arguments to '" + calleeName + "': expected " + std::to_string(numParams) +
                  ", got " + std::to_string(numArgs));
    } else if (numArgs < fixedParams) {
        // Check if missing fixed arguments have default values
        bool allDefaulted = false;
        if (funcDecl && funcDecl->params.size() == numParams) {
            allDefaulted = true;
            for (size_t i = numArgs; i < fixedParams; ++i) {
                if (!funcDecl->params[i].defaultValue) {
                    allDefaulted = false;
                    break;
                }
            }
        }
        if (!allDefaulted) {
            size_t minRequired = fixedParams;
            if (funcDecl && funcDecl->params.size() == numParams) {
                minRequired = 0;
                for (size_t i = 0; i < fixedParams; ++i) {
                    if (!funcDecl->params[i].defaultValue)
                        ++minRequired;
                    else
                        break;
                }
            }
            error(expr->loc,
                  "Too few arguments to '" + calleeName + "': expected at least " +
                      std::to_string(minRequired) + ", got " + std::to_string(numArgs));
        }
    }

    // Type-check fixed arguments
    const size_t checkCount = std::min(numArgs, fixedParams);
    for (size_t i = 0; i < checkCount; ++i) {
        TypeRef argType = exprTypes_.count(expr->args[i].value.get())
                              ? exprTypes_[expr->args[i].value.get()]
                              : nullptr;
        TypeRef paramType = paramTys[i];

        if (!argType || !paramType || argType->kind == TypeKindSem::Unknown ||
            paramType->kind == TypeKindSem::Unknown)
            continue;

        if (!paramType->isAssignableFrom(*argType)) {
            errorTypeMismatch(expr->args[i].value->loc, paramType, argType);
        }
    }

    // Type-check variadic arguments against the element type
    if (hasVariadic && numArgs > fixedParams && numParams > 0) {
        TypeRef variadicListType = paramTys.back(); // List[T]
        TypeRef elemType = variadicListType ? variadicListType->elementType() : nullptr;
        if (elemType) {
            for (size_t i = fixedParams; i < numArgs; ++i) {
                TypeRef argType = exprTypes_.count(expr->args[i].value.get())
                                      ? exprTypes_[expr->args[i].value.get()]
                                      : nullptr;
                if (!argType || argType->kind == TypeKindSem::Unknown)
                    continue;
                if (!elemType->isAssignableFrom(*argType)) {
                    errorTypeMismatch(expr->args[i].value->loc, elemType, argType);
                }
            }
        }
    }
}

//=============================================================================
// Call Expression Analysis
//=============================================================================

/// @brief Bind an extern call and enforce its safe runtime pointer contract.
/// @param expr Call expression whose arguments have been analyzed.
/// @param calleeName Fully qualified runtime function name.
/// @param sym Extern function symbol.
/// @param skipLeadingParams Hidden leading parameters not exposed at this call site.
/// @return True for a valid binding or a non-extern symbol; false after a binding/safety error.
bool Sema::bindExternCallOnCall(CallExpr *expr,
                                const std::string &calleeName,
                                Symbol *sym,
                                size_t skipLeadingParams) {
    if (!sym || !sym->isExtern || !sym->type || sym->type->kind != TypeKindSem::Function)
        return true;

    CallArgBinding binding;
    auto specs = makeExternParamSpecs(*sym, skipLeadingParams);
    if (!bindCallArgs(expr->args, specs, expr->loc, calleeName, binding, nullptr, true, true)) {
        return false;
    }
    if (!checkRuntimePointerSafety(
            calleeName, expr->args, specs, binding, skipLeadingParams, expr->loc)) {
        return false;
    }
    callArgBindings_[expr] = binding;
    return true;
}

/// @brief Apply the Terminal Say/Print single-argument auto-stringification exception.
/// @param expr Candidate call expression.
/// @param calleeName Fully qualified runtime callee.
/// @param sym Extern function symbol.
/// @param outType Receives the normalized call result type on success.
/// @return True when the call matches and a binding was recorded.
bool Sema::tryBindTerminalTextCall(CallExpr *expr,
                                   const std::string &calleeName,
                                   Symbol *sym,
                                   TypeRef &outType) {
    if (!sym || !sym->isExtern || !sym->type || sym->type->kind != TypeKindSem::Function ||
        !isTerminalTextRuntime(calleeName) || expr->args.size() != 1) {
        return false;
    }

    TypeRef argType = exprTypes_.count(expr->args[0].value.get())
                          ? exprTypes_.at(expr->args[0].value.get())
                          : nullptr;
    if (!canAutoStringifyForTerminal(argType))
        return false;

    auto specs = makeExternParamSpecs(*sym);
    if (specs.empty())
        return false;
    specs.resize(1);
    specs[0].type = types::any();
    CallArgBinding binding;
    if (!bindCallArgs(expr->args, specs, expr->loc, calleeName, binding, nullptr, true, true))
        return false;

    runtimeCallees_[expr] = calleeName;
    exprTypes_[expr->callee.get()] = sym->type;
    callArgBindings_[expr] = binding;
    outType = normalizeRuntimeSurfaceType(sym->type->returnType());
    return true;
}

/// @brief Decide whether a dotted call target must bypass ordinary receiver analysis.
/// @param expr Call expression with a potentially qualified callee.
/// @return True when the root denotes Zanna, a bind alias, a file module, or a type/module symbol.
bool Sema::shouldDeferDottedCalleeToQualifiedLookup(const CallExpr *expr) const {
    std::string dottedName;
    if (!extractDottedName(expr->callee.get(), dottedName))
        return false;

    auto dotPos = dottedName.find('.');
    if (dotPos == std::string::npos)
        return false;

    std::string root = dottedName.substr(0, dotPos);
    if (root == "Zanna" || aliasToNamespace_.find(root) != aliasToNamespace_.end() ||
        importedSymbols_.find(root) != importedSymbols_.end() ||
        hasModuleExports(root, expr->callee ? expr->callee->loc : expr->loc)) {
        return true;
    }

    Symbol *rootSym = currentScope_ ? currentScope_->lookup(root) : nullptr;
    return rootSym &&
           (rootSym->kind == Symbol::Kind::Module || rootSym->kind == Symbol::Kind::Type);
}

/// @brief Refine an erased runtime call result using registry and receiver element metadata.
/// @param expr Call expression supplying receiver and argument types.
/// @param calleeName Fully qualified runtime function name.
/// @param fallback Type inferred from the extern symbol.
/// @return Most specific normalized result type available.
/// @details Specializes sequence element, map key/value, set item, and conversion results that
///          are represented as opaque object pointers in the low-level ABI.
TypeRef Sema::refineRuntimeCallReturnType(const CallExpr *expr,
                                          const std::string &calleeName,
                                          TypeRef fallback) const {
    fallback = normalizeRuntimeSurfaceType(fallback);

    const auto &registry = il::runtime::RuntimeRegistry::instance();
    if (auto sig = registry.findFunction(calleeName); sig && sig->isValid()) {
        TypeRef refined = normalizeRuntimeSurfaceType(toZiaReturnType(*sig));
        auto isOpaquePtr = [](TypeRef type) {
            return type && type->kind == TypeKindSem::Ptr && type->name.empty();
        };
        auto isTypedSeq = [](TypeRef type) {
            return type && type->kind == TypeKindSem::Ptr &&
                   type->name == "Zanna.Collections.Seq" && !type->typeArgs.empty();
        };
        auto isConcreteRuntimeClass = [](TypeRef type) {
            return type && type->kind == TypeKindSem::Ptr && !type->name.empty() &&
                   type->name != "Zanna.Collections.Seq";
        };

        if (!fallback || fallback->kind == TypeKindSem::Unknown ||
            (refined && refined->kind != TypeKindSem::Unknown &&
             (isTypedSeq(refined) || (isConcreteRuntimeClass(refined) && isOpaquePtr(fallback)) ||
              (!isOpaquePtr(refined) && isOpaquePtr(fallback))))) {
            fallback = refined;
        }
    }

    auto argTypeAt = [&](size_t index) -> TypeRef {
        if (index >= expr->args.size())
            return nullptr;
        auto it = exprTypes_.find(expr->args[index].value.get());
        if (it == exprTypes_.end())
            return nullptr;
        TypeRef ty = it->second;
        if (ty && ty->kind == TypeKindSem::Optional && ty->innerType())
            ty = ty->innerType();
        return ty;
    };

    TypeRef firstArg = argTypeAt(0);
    TypeRef receiverArg = nullptr;
    if (expr->callee && expr->callee->kind == ExprKind::Field) {
        auto *fieldExpr = static_cast<FieldExpr *>(expr->callee.get());
        auto it = exprTypes_.find(fieldExpr->base.get());
        if (it != exprTypes_.end()) {
            receiverArg = it->second;
            if (receiverArg && receiverArg->kind == TypeKindSem::Optional &&
                receiverArg->innerType())
                receiverArg = receiverArg->innerType();
        }
    }
    TypeRef elementReceiver = receiverArg && receiverArg->elementType() ? receiverArg : firstArg;
    TypeRef mapReceiver = receiverArg && receiverArg->valueType() ? receiverArg : firstArg;
    TypeRef setReceiver =
        receiverArg && receiverArg->kind == TypeKindSem::Set ? receiverArg : firstArg;
    auto asSeq = [&](TypeRef elemType) -> TypeRef {
        return elemType ? normalizeRuntimeSurfaceType(types::seqOf(elemType)) : fallback;
    };

    if (calleeName == "Zanna.Collections.Seq.Get" || calleeName == "Zanna.Collections.Seq.First" ||
        calleeName == "Zanna.Collections.Seq.Last" || calleeName == "Zanna.Collections.Seq.Peek" ||
        calleeName == "Zanna.Collections.Seq.Pop" || calleeName == "Zanna.Collections.Seq.RemoveAt" ||
        calleeName == "Zanna.Collections.Seq.FindWhere") {
        return elementReceiver && elementReceiver->elementType()
                   ? normalizeRuntimeSurfaceType(elementReceiver->elementType())
                   : fallback;
    }

    if (calleeName == "Zanna.Collections.List.Get" ||
        calleeName == "Zanna.Collections.List.First" ||
        calleeName == "Zanna.Collections.List.Last" || calleeName == "Zanna.Collections.List.Pop" ||
        calleeName == "Zanna.Collections.Queue.Peek" ||
        calleeName == "Zanna.Collections.Queue.Pop" ||
        calleeName == "Zanna.Collections.Queue.TryPop" ||
        calleeName == "Zanna.Collections.Stack.Peek" ||
        calleeName == "Zanna.Collections.Stack.Pop" ||
        calleeName == "Zanna.Collections.Stack.TryPop" ||
        calleeName == "Zanna.Collections.Ring.Get" || calleeName == "Zanna.Collections.Ring.Peek" ||
        calleeName == "Zanna.Collections.Ring.Pop" || calleeName == "Zanna.Collections.Heap.Peek" ||
        calleeName == "Zanna.Collections.Heap.Pop" ||
        calleeName == "Zanna.Collections.Heap.TryPeek" ||
        calleeName == "Zanna.Collections.Heap.TryPop" ||
        calleeName == "Zanna.Collections.Deque.Get" ||
        calleeName == "Zanna.Collections.Deque.PeekFront" ||
        calleeName == "Zanna.Collections.Deque.PeekBack" ||
        calleeName == "Zanna.Collections.Deque.PopFront" ||
        calleeName == "Zanna.Collections.Deque.PopBack" ||
        calleeName == "Zanna.Collections.Deque.TryPopFront" ||
        calleeName == "Zanna.Collections.Deque.TryPopBack") {
        return elementReceiver && elementReceiver->elementType()
                   ? normalizeRuntimeSurfaceType(elementReceiver->elementType())
                   : fallback;
    }

    if (calleeName == "Zanna.Collections.Map.Get" || calleeName == "Zanna.Collections.Map.GetOr" ||
        calleeName == "Zanna.Collections.OrderedMap.Get" ||
        calleeName == "Zanna.Collections.SortedMap.Get" ||
        calleeName == "Zanna.Collections.Trie.Get" ||
        calleeName == "Zanna.Collections.FrozenMap.Get" ||
        calleeName == "Zanna.Collections.FrozenMap.GetOr" ||
        calleeName == "Zanna.Collections.DefaultMap.Get" ||
        calleeName == "Zanna.Collections.WeakMap.Get" ||
        calleeName == "Zanna.Collections.LruCache.Get" ||
        calleeName == "Zanna.Collections.LruCache.Peek" ||
        calleeName == "Zanna.Collections.MultiMap.Get" ||
        calleeName == "Zanna.Collections.MultiMap.GetFirst") {
        return mapReceiver && mapReceiver->valueType()
                   ? normalizeRuntimeSurfaceType(mapReceiver->valueType())
                   : fallback;
    }

    if (calleeName == "Zanna.Collections.Map.Keys" ||
        calleeName == "Zanna.Collections.OrderedMap.Keys" ||
        calleeName == "Zanna.Collections.SortedMap.Keys" ||
        calleeName == "Zanna.Collections.Trie.Keys" ||
        calleeName == "Zanna.Collections.FrozenMap.Keys" ||
        calleeName == "Zanna.Collections.DefaultMap.Keys" ||
        calleeName == "Zanna.Collections.WeakMap.Keys" ||
        calleeName == "Zanna.Collections.LruCache.Keys" ||
        calleeName == "Zanna.Collections.MultiMap.Keys") {
        return mapReceiver && mapReceiver->keyType() ? asSeq(mapReceiver->keyType()) : fallback;
    }

    if (calleeName == "Zanna.Collections.Map.Values" ||
        calleeName == "Zanna.Collections.OrderedMap.Values" ||
        calleeName == "Zanna.Collections.SortedMap.Values" ||
        calleeName == "Zanna.Collections.FrozenMap.Values" ||
        calleeName == "Zanna.Collections.LruCache.Values") {
        return mapReceiver && mapReceiver->valueType() ? asSeq(mapReceiver->valueType()) : fallback;
    }

    if (calleeName == "Zanna.Collections.Set.Items") {
        return setReceiver && setReceiver->kind == TypeKindSem::Set && setReceiver->elementType()
                   ? asSeq(setReceiver->elementType())
                   : fallback;
    }

    if (calleeName == "Zanna.Collections.List.ToSeq" ||
        calleeName == "Zanna.Collections.Queue.ToSeq" ||
        calleeName == "Zanna.Collections.Stack.ToSeq" ||
        calleeName == "Zanna.Collections.Deque.ToSeq") {
        return elementReceiver && elementReceiver->elementType()
                   ? asSeq(elementReceiver->elementType())
                   : fallback;
    }

    return fallback;
}

/// @brief Analyze a higher-order List combinator call.
/// @param expr Call expression containing combinator arguments.
/// @param m Normalized method name (`sum`, `reduce`, `map`, `filter`, `firstWhere`, `any`, or
///        `all`).
/// @param baseType Receiver list type.
/// @return Specialized result when @p m is handled, or std::nullopt for other methods.
/// @details Supplies expected function types to lambda analysis and validates reducer,
///          transformer, or predicate shapes.
std::optional<TypeRef> Sema::analyzeListCombinatorCall(CallExpr *expr,
                                                       const std::string &m,
                                                       TypeRef baseType) {
    TypeRef elemType =
        baseType && baseType->elementType() ? baseType->elementType() : types::unknown();
    auto analyzeAll = [&]() {
        for (auto &arg : expr->args)
            analyzeExpr(arg.value.get());
    };

    if (m == "sum") {
        analyzeAll();
        if (!expr->args.empty())
            error(expr->loc, "sum() takes no arguments");
        if (elemType->kind != TypeKindSem::Integer && elemType->kind != TypeKindSem::Number &&
            elemType->kind != TypeKindSem::Unknown)
            error(expr->loc, "sum() requires a List of Integer or Number, got List of " +
                                 elemType->toDisplayString());
        return elemType;
    }

    if (m == "reduce") {
        if (expr->args.size() != 2) {
            analyzeAll();
            error(expr->loc, "reduce() expects (initial, (accumulator, item) => accumulator)");
            return TypeRef(types::unknown());
        }
        TypeRef accType = analyzeExpr(expr->args[0].value.get());
        if (!accType)
            accType = types::unknown();
        lambdaTypeHint_ = types::function({accType, elemType}, types::unknown());
        TypeRef fnType = analyzeExpr(expr->args[1].value.get());
        lambdaTypeHint_ = nullptr;
        if (fnType && fnType->kind != TypeKindSem::Function && fnType->kind != TypeKindSem::Unknown)
            error(expr->args[1].value->loc, "reduce() second argument must be a function");
        return accType;
    }

    // Unary-closure combinators: map / filter / firstWhere / any / all
    if (expr->args.size() != 1) {
        analyzeAll();
        error(expr->loc, m + "() expects a single function argument");
        return TypeRef(types::unknown());
    }
    lambdaTypeHint_ = types::function({elemType}, types::unknown());
    TypeRef fnType = analyzeExpr(expr->args[0].value.get());
    lambdaTypeHint_ = nullptr;
    if (fnType && fnType->kind != TypeKindSem::Function && fnType->kind != TypeKindSem::Unknown) {
        error(expr->args[0].value->loc, m + "() argument must be a function");
        return TypeRef(types::unknown());
    }
    TypeRef fnRet = (fnType && fnType->kind == TypeKindSem::Function) ? fnType->returnType()
                                                                      : types::unknown();
    if (m == "map")
        return TypeRef(types::list(fnRet));
    if (fnRet && fnRet->kind != TypeKindSem::Boolean && fnRet->kind != TypeKindSem::Unknown)
        error(expr->args[0].value->loc,
              m + "() predicate must return Boolean, got " + fnRet->toDisplayString());
    if (m == "filter")
        return baseType;
    if (m == "firstWhere")
        return TypeRef(types::optional(elemType));
    return TypeRef(types::boolean()); // any / all
}

/// @brief Analyze a function or method call expression.
/// @param expr The call expression node.
/// @return The return type of the called function or method.
/// @details Handles optional method chains, generic calls with explicit or inferred arguments,
///          bound and qualified symbols, collection methods, runtime classes, constructors, and
///          ordinary function/method overloads. Successful paths record lowered callees and
///          argument bindings for code generation.
TypeRef Sema::analyzeCall(CallExpr *expr) {
    auto analyzeArgTypes = [&]() {
        std::vector<TypeRef> argTypes;
        argTypes.reserve(expr->args.size());
        for (auto &arg : expr->args)
            argTypes.push_back(analyzeExpr(arg.value.get()));
        return argTypes;
    };

    if (auto *optionalCallee = dynamic_cast<OptionalChainExpr *>(expr->callee.get())) {
        TypeRef baseType = analyzeExpr(optionalCallee->base.get());
        baseType = declaredOptionalSurfaceType(optionalCallee->base.get(), baseType);
        if (baseType)
            exprTypes_[optionalCallee->base.get()] = baseType;

        if (!baseType || baseType->kind != TypeKindSem::Optional || !baseType->innerType()) {
            analyzeArgTypes();
            error(expr->loc, "Optional method chaining requires an optional receiver");
            return types::unknown();
        }

        TypeRef receiverType = baseType->innerType();
        if (!receiverType || (receiverType->kind != TypeKindSem::Struct &&
                              receiverType->kind != TypeKindSem::Class &&
                              receiverType->kind != TypeKindSem::Interface)) {
            analyzeArgTypes();
            std::string receiverName =
                receiverType ? receiverType->toDisplayString() : std::string("Unknown");
            error(expr->loc,
                  "Type '" + receiverName + "' has no optional method '" + optionalCallee->field +
                      "'");
            return types::unknown();
        }

        analyzeArgTypes();

        std::string resolvedOwner;
        CallArgBinding binding;
        MethodDecl *method = resolveMethodCallOverload(receiverType->name,
                                                       optionalCallee->field,
                                                       expr,
                                                       expr->loc,
                                                       &resolvedOwner,
                                                       receiverType->kind != TypeKindSem::Interface,
                                                       &binding);
        if (!method) {
            error(expr->loc,
                  "Type '" + receiverType->toDisplayString() + "' has no method '" +
                      optionalCallee->field + "'");
            return types::unknown();
        }

        bool isInsideType = currentSelfType_ && currentSelfType_->name == resolvedOwner;
        if (method->visibility == Visibility::Private && !isInsideType) {
            error(expr->loc,
                  "Cannot access private member '" + optionalCallee->field + "' of type '" +
                      resolvedOwner + "'");
            return types::unknown();
        }

        resolvedMethodDecls_[expr] = method;
        resolvedMethodOwnerTypes_[expr] = resolvedOwner;
        resolvedMethodSlotKeys_[expr] = methodSlotKey(resolvedOwner, method);
        callArgBindings_[expr] = binding;

        TypeRef methodType = getMethodType(resolvedOwner, method);
        exprTypes_[expr->callee.get()] = methodType;
        TypeRef returnType = methodType && methodType->kind == TypeKindSem::Function
                                 ? normalizeRuntimeSurfaceType(methodType->returnType())
                                 : types::unknown();
        if (!returnType || returnType->kind == TypeKindSem::Unknown)
            return types::unknown();
        if (returnType->kind == TypeKindSem::Void || returnType->kind == TypeKindSem::Optional)
            return returnType;
        return types::optional(returnType);
    }

    if (auto *ident = dynamic_cast<IdentExpr *>(expr->callee.get())) {
        if (ident->name == "Ok" || ident->name == "Err") {
            std::vector<TypeRef> argTypes = analyzeArgTypes();
            if (argTypes.size() != 1) {
                error(expr->loc,
                      ident->name + " expects exactly one argument when constructing Result");
                return types::result(types::unknown());
            }
            TypeRef payload = argTypes.empty() || !argTypes[0] ? types::unknown() : argTypes[0];
            TypeRef success = ident->name == "Ok" ? payload : types::unknown();
            TypeRef resultType = types::result(success);
            exprTypes_[expr->callee.get()] = types::function({payload}, resultType);
            return resultType;
        }
    }

    if (auto *fieldExpr = dynamic_cast<FieldExpr *>(expr->callee.get());
        fieldExpr && !shouldDeferDottedCalleeToQualifiedLookup(expr)) {
        TypeRef baseType = analyzeExpr(fieldExpr->base.get());
        if (baseType && baseType->kind == TypeKindSem::Result) {
            TypeRef successType =
                !baseType->typeArgs.empty() ? baseType->typeArgs[0] : types::unknown();
            std::vector<TypeRef> argTypes;
            argTypes.reserve(expr->args.size());
            for (auto &arg : expr->args)
                argTypes.push_back(analyzeExpr(arg.value.get()));

            if (fieldExpr->field == "isOk" || fieldExpr->field == "isErr") {
                if (!argTypes.empty())
                    error(expr->loc, fieldExpr->field + "() does not take arguments");
                return types::boolean();
            }
            if (fieldExpr->field == "unwrap") {
                if (!argTypes.empty())
                    error(expr->loc, "unwrap() does not take arguments");
                return successType ? successType : types::unknown();
            }
            if (fieldExpr->field == "unwrapOr") {
                if (argTypes.size() != 1) {
                    error(expr->loc, "unwrapOr() expects exactly one default value");
                } else if (successType && argTypes[0] &&
                           !successType->isAssignableFrom(*argTypes[0])) {
                    errorTypeMismatch(expr->args[0].value->loc, successType, argTypes[0]);
                }
                return successType ? successType : types::unknown();
            }
            if (fieldExpr->field == "unwrapErr") {
                if (!argTypes.empty())
                    error(expr->loc, "unwrapErr() does not take arguments");
                return types::string();
            }
            error(expr->loc, "Result has no method '" + fieldExpr->field + "'");
            return types::unknown();
        }
    }

    // Handle explicit generic method calls: value.method[Type](args).
    if (expr->callee->kind == ExprKind::Index) {
        auto *indexExpr = static_cast<IndexExpr *>(expr->callee.get());
        if (indexExpr->base->kind == ExprKind::Field) {
            auto *methodAccess = static_cast<FieldExpr *>(indexExpr->base.get());
            TypeRef receiverType = analyzeExpr(methodAccess->base.get());
            if (receiverType && receiverType->kind == TypeKindSem::Optional &&
                receiverType->innerType())
                receiverType = receiverType->innerType();

            std::vector<TypePtr> typeArgNodes;
            collectTypeArgNodes(indexExpr->index.get(), typeArgNodes);
            if (receiverType &&
                (receiverType->kind == TypeKindSem::Class ||
                 receiverType->kind == TypeKindSem::Struct) &&
                !typeArgNodes.empty()) {
                std::vector<TypeRef> typeArgs;
                for (auto &typeNode : typeArgNodes) {
                    TypeRef typeArg = resolveTypeNode(typeNode.get());
                    if (!typeArg || typeArg->kind == TypeKindSem::Unknown) {
                        error(typeNode->loc, "Unknown type argument for generic method call");
                        return types::unknown();
                    }
                    typeArgs.push_back(typeArg);
                }

                analyzeArgTypes();

                MethodDecl *best = nullptr;
                std::string bestOwner;
                TypeRef bestConcreteType;
                TypeRef bestErasedType;
                CallArgBinding bestBinding;
                int bestScore = std::numeric_limits<int>::max();
                bool ambiguous = false;

                for (auto *candidate :
                     collectMethodOverloads(receiverType->name, methodAccess->field, true)) {
                    if (candidate->genericParams.size() != typeArgs.size())
                        continue;

                    std::map<std::string, TypeRef> substitutions;
                    for (size_t i = 0; i < candidate->genericParams.size(); ++i)
                        substitutions[candidate->genericParams[i]] = typeArgs[i];

                    pushTypeParams(substitutions);
                    std::vector<TypeRef> paramTypes;
                    for (const auto &param : candidate->params)
                        paramTypes.push_back(param.type ? resolveTypeNode(param.type.get())
                                                        : types::unknown());
                    TypeRef returnType = candidate->returnType
                                             ? resolveTypeNode(candidate->returnType.get())
                                             : types::voidType();
                    popTypeParams();

                    TypeRef concreteType = types::function(paramTypes, returnType);
                    CallArgBinding binding;
                    int score = 0;
                    auto specs = makeParamSpecs(candidate->params, paramTypes);
                    if (!bindCallArgs(expr->args,
                                      specs,
                                      expr->loc,
                                      receiverType->name + "." + methodAccess->field,
                                      binding,
                                      &score,
                                      false))
                        continue;

                    std::string owner = receiverType->name;
                    if (MethodDecl *resolved = resolveMethodCallOverload(receiverType->name,
                                                                         methodAccess->field,
                                                                         expr,
                                                                         expr->loc,
                                                                         &owner,
                                                                         true,
                                                                         nullptr)) {
                        if (resolved != candidate)
                            owner = receiverType->name;
                    }

                    TypeRef erasedType = getMethodType(owner, candidate);
                    if (!erasedType)
                        erasedType = methodTypeForDecl(*candidate);

                    if (score < bestScore) {
                        best = candidate;
                        bestOwner = owner;
                        bestConcreteType = concreteType;
                        bestErasedType = erasedType;
                        bestBinding = binding;
                        bestScore = score;
                        ambiguous = false;
                    } else if (score == bestScore) {
                        ambiguous = true;
                    }
                }

                if (ambiguous) {
                    error(expr->loc,
                          "Ambiguous generic method call to '" + methodAccess->field + "'");
                    return types::unknown();
                }
                if (best) {
                    if (!validateGenericConstraints(best->genericParams,
                                                    best->genericParamConstraints,
                                                    typeArgs,
                                                    expr->loc,
                                                    receiverType->name + "." + methodAccess->field))
                        return types::unknown();
                    resolvedMethodDecls_[expr] = best;
                    resolvedMethodOwnerTypes_[expr] = bestOwner;
                    resolvedMethodSlotKeys_[expr] = methodSlotKey(bestOwner, best);
                    genericMethodConcreteTypes_[expr] = bestConcreteType;
                    genericMethodErasedTypes_[expr] = bestErasedType;
                    callArgBindings_[expr] = bestBinding;
                    exprTypes_[expr->callee.get()] = bestConcreteType;
                    return bestConcreteType && bestConcreteType->kind == TypeKindSem::Function
                               ? normalizeRuntimeSurfaceType(bestConcreteType->returnType())
                               : types::unknown();
                }
            }
        }
    }

    // Handle generic function calls: identity[Integer](100)
    // Parser produces: CallExpr(callee=IndexExpr(base=IdentExpr, index=IdentExpr/expr), args)
    // We need to detect when the "index" is actually a type argument
    if (expr->callee->kind == ExprKind::Index) {
        auto *indexExpr = static_cast<IndexExpr *>(expr->callee.get());
        if (indexExpr->base->kind == ExprKind::Ident) {
            auto *identExpr = static_cast<IdentExpr *>(indexExpr->base.get());
            std::string genericName = fileScopedDeclName(identExpr->loc.file_id, identExpr->name);
            if (isGenericFunction(genericName)) {
                // This is a generic function call!
                std::vector<TypeRef> typeArgs;

                std::vector<TypePtr> typeArgNodes;
                collectTypeArgNodes(indexExpr->index.get(), typeArgNodes);
                if (typeArgNodes.empty()) {
                    error(indexExpr->index->loc,
                          "Expected type argument for generic function call");
                    return types::unknown();
                }

                for (auto &typeNode : typeArgNodes) {
                    TypeRef typeArg = resolveTypeNode(typeNode.get());
                    if (typeArg && typeArg->kind != TypeKindSem::Unknown) {
                        typeArgs.push_back(typeArg);
                    } else {
                        error(typeNode->loc, "Unknown type argument for generic function call");
                        return types::unknown();
                    }
                }

                // Instantiate the generic function with the type arguments
                TypeRef funcType = instantiateGenericFunction(genericName, typeArgs, expr->loc);

                // Store the mangled name for the lowerer
                std::string mangledName = mangleGenericName(genericName, typeArgs);
                genericFunctionCallees_[expr] = mangledName;

                // Store the instantiated function type so the lowerer can access it
                exprTypes_[expr->callee.get()] = funcType;

                // Analyze arguments
                for (auto &arg : expr->args) {
                    analyzeExpr(arg.value.get());
                }

                CallArgBinding binding;
                FunctionDecl *genericDecl = getGenericFunction(genericName);
                if (genericDecl && funcType && funcType->kind == TypeKindSem::Function) {
                    auto specs = makeParamSpecs(genericDecl->params, funcType->paramTypes());
                    if (!bindCallArgs(
                            expr->args, specs, expr->loc, genericName, binding, nullptr, true))
                        return types::unknown();
                    resolvedFunctionDecls_[expr] = genericDecl;
                    callArgBindings_[expr] = binding;
                } else {
                    validateCallArgs(expr, funcType, mangledName);
                }

                // Return the function's return type
                if (funcType && funcType->kind == TypeKindSem::Function) {
                    return normalizeRuntimeSurfaceType(funcType->returnType());
                }
                return types::unknown();
            }
        }
    }

    // Type inference for generic function calls without explicit type arguments
    // e.g., identity(42) instead of identity[Integer](42)
    // This must come BEFORE the dotted name lookup to catch simple IdentExpr callees
    if (expr->callee->kind == ExprKind::Ident) {
        auto *identExpr = static_cast<IdentExpr *>(expr->callee.get());
        std::string genericName = fileScopedDeclName(identExpr->loc.file_id, identExpr->name);
        if (isGenericFunction(genericName)) {
            FunctionDecl *genericDecl = getGenericFunction(genericName);
            if (genericDecl && !genericDecl->genericParams.empty() && !expr->args.empty()) {
                // Analyze all arguments first to get their types
                std::vector<TypeRef> argTypes;
                for (auto &arg : expr->args) {
                    TypeRef argType = analyzeExpr(arg.value.get());
                    argTypes.push_back(argType);
                }

                std::vector<CallParamSpec> inferenceSpecs;
                inferenceSpecs.reserve(genericDecl->params.size());
                for (const auto &param : genericDecl->params) {
                    CallParamSpec spec;
                    spec.name = param.name;
                    spec.type = types::any();
                    spec.hasDefault = param.defaultValue != nullptr;
                    spec.isVariadic = param.isVariadic;
                    inferenceSpecs.push_back(std::move(spec));
                }

                CallArgBinding provisionalBinding;
                if (!bindCallArgs(expr->args,
                                  inferenceSpecs,
                                  expr->loc,
                                  genericName,
                                  provisionalBinding,
                                  nullptr,
                                  true)) {
                    return types::unknown();
                }

                // Build set of type parameter names for quick lookup
                std::set<std::string> typeParamNames(genericDecl->genericParams.begin(),
                                                     genericDecl->genericParams.end());

                // Infer type parameters from arguments after applying named/default/variadic
                // binding. Omitted default parameters cannot contribute inference.
                std::map<std::string, TypeRef> inferredTypes;
                const bool hasVariadic =
                    !genericDecl->params.empty() && genericDecl->params.back().isVariadic;
                const size_t fixedCount =
                    hasVariadic ? genericDecl->params.size() - 1 : genericDecl->params.size();
                for (size_t i = 0; i < fixedCount; ++i) {
                    int sourceIndex = i < provisionalBinding.fixedParamSources.size()
                                          ? provisionalBinding.fixedParamSources[i]
                                          : -1;
                    if (sourceIndex < 0)
                        continue;
                    TypeNode *paramTypeNode = genericDecl->params[i].type.get();
                    size_t argIndex = static_cast<size_t>(sourceIndex);
                    if (!inferTypeParamsFromPattern(
                            paramTypeNode, argTypes[argIndex], typeParamNames, inferredTypes)) {
                        error(expr->args[argIndex].value->loc,
                              "Type mismatch in generic function call: cannot infer type "
                              "arguments from parameter '" +
                                  genericDecl->params[i].name + "'");
                        return types::unknown();
                    }
                }
                if (hasVariadic) {
                    TypeNode *paramTypeNode = genericDecl->params.back().type.get();
                    for (int sourceIndex : provisionalBinding.variadicSources) {
                        size_t argIndex = static_cast<size_t>(sourceIndex);
                        if (!inferTypeParamsFromPattern(
                                paramTypeNode, argTypes[argIndex], typeParamNames, inferredTypes)) {
                            error(expr->args[argIndex].value->loc,
                                  "Type mismatch in generic function call: cannot infer type "
                                  "arguments from variadic parameter '" +
                                      genericDecl->params.back().name + "'");
                            return types::unknown();
                        }
                    }
                }

                // Check that all type parameters were inferred
                std::vector<TypeRef> typeArgs;
                for (const auto &paramName : genericDecl->genericParams) {
                    auto it = inferredTypes.find(paramName);
                    if (it != inferredTypes.end()) {
                        typeArgs.push_back(it->second);
                    } else {
                        error(expr->loc,
                              "Cannot infer type argument for '" + paramName +
                                  "' in generic function call");
                        return types::unknown();
                    }
                }

                // Instantiate the generic function with inferred type arguments
                TypeRef funcType = instantiateGenericFunction(genericName, typeArgs, expr->loc);

                // Store the mangled name for the lowerer
                std::string mangledName = mangleGenericName(genericName, typeArgs);
                genericFunctionCallees_[expr] = mangledName;

                // Store the instantiated function type
                exprTypes_[expr->callee.get()] = funcType;

                CallArgBinding binding;
                if (funcType && funcType->kind == TypeKindSem::Function) {
                    auto specs = makeParamSpecs(genericDecl->params, funcType->paramTypes());
                    if (!bindCallArgs(
                            expr->args, specs, expr->loc, genericName, binding, nullptr, true))
                        return types::unknown();
                    resolvedFunctionDecls_[expr] = genericDecl;
                    callArgBindings_[expr] = binding;
                } else {
                    validateCallArgs(expr, funcType, mangledName);
                }

                // Return the function's return type
                if (funcType && funcType->kind == TypeKindSem::Function) {
                    return normalizeRuntimeSurfaceType(funcType->returnType());
                }
                return types::unknown();
            }
        }
    }

    // Check if callee is an imported symbol from a bound namespace
    // This handles unqualified calls like Say() when Zanna.Terminal is bound
    if (expr->callee->kind == ExprKind::Ident) {
        auto *identExpr = static_cast<IdentExpr *>(expr->callee.get());
        auto importIt = importedSymbols_.find(identExpr->name);
        if (importIt != importedSymbols_.end()) {
            // Resolve to the full qualified name
            const std::string &fullName = importIt->second;
            Symbol *sym = lookupAccessibleSymbol(fullName, expr->loc, true);
            if (sym && sym->kind == Symbol::Kind::Function && sym->isExtern) {
                // Store the resolved callee for the lowerer
                runtimeCallees_[expr] = fullName;
                exprTypes_[expr->callee.get()] = sym->type;

                // Analyze arguments
                for (auto &arg : expr->args) {
                    analyzeExpr(arg.value.get());
                }

                TypeRef terminalTextResult = nullptr;
                if (tryBindTerminalTextCall(expr, fullName, sym, terminalTextResult))
                    return terminalTextResult;

                if (!bindExternCallOnCall(expr, fullName, sym))
                    return types::unknown();

                // Skip validation for extern/runtime functions — their signatures
                // include implicit self parameters that don't appear in call syntax.

                // Return the function's return type
                if (sym->type && sym->type->kind == TypeKindSem::Function) {
                    return refineRuntimeCallReturnType(expr, fullName, sym->type->returnType());
                }
                return normalizeRuntimeSurfaceType(sym->type);
            }
        }
    }

    if (expr->callee->kind == ExprKind::Ident) {
        auto *identExpr = static_cast<IdentExpr *>(expr->callee.get());
        analyzeArgTypes();

        if (currentSelfType_ && (currentSelfType_->kind == TypeKindSem::Class ||
                                 currentSelfType_->kind == TypeKindSem::Struct ||
                                 currentSelfType_->kind == TypeKindSem::Interface)) {
            Symbol *local = currentScope_->lookupLocal(identExpr->name);
            if (!local || local->kind == Symbol::Kind::Method) {
                std::string resolvedOwner;
                CallArgBinding binding;
                if (MethodDecl *method = resolveMethodCallOverload(currentSelfType_->name,
                                                                   identExpr->name,
                                                                   expr,
                                                                   expr->loc,
                                                                   &resolvedOwner,
                                                                   true,
                                                                   &binding)) {
                    resolvedMethodDecls_[expr] = method;
                    resolvedMethodOwnerTypes_[expr] = resolvedOwner;
                    resolvedMethodSlotKeys_[expr] = methodSlotKey(resolvedOwner, method);
                    callArgBindings_[expr] = binding;
                    TypeRef methodType = getMethodType(resolvedOwner, method);
                    exprTypes_[expr->callee.get()] = methodType;
                    return methodType && methodType->kind == TypeKindSem::Function
                               ? normalizeRuntimeSurfaceType(methodType->returnType())
                               : types::unknown();
                }
            }
        }

        std::string loweredName;
        CallArgBinding binding;
        std::string callName = fileScopedDeclName(identExpr->loc.file_id, identExpr->name);
        if (FunctionDecl *func =
                resolveFunctionCallOverload(callName, expr, expr->loc, &loweredName, &binding)) {
            TypeRef funcType = functionDeclTypes_[func];
            resolvedFunctionCallees_[expr] = loweredName;
            resolvedFunctionDecls_[expr] = func;
            callArgBindings_[expr] = binding;
            exprTypes_[expr->callee.get()] = funcType;
            return funcType && funcType->kind == TypeKindSem::Function
                       ? normalizeRuntimeSurfaceType(funcType->returnType())
                       : types::unknown();
        }
    }

    // First, try to resolve dotted function names like Zanna.Terminal.Say or MyLib.helper
    // This unified lookup works for both runtime functions and user-defined namespaced functions
    std::string dottedName;
    if (extractDottedName(expr->callee.get(), dottedName)) {
        bool viaQualifiedModule = dottedName.find('.') != std::string::npos;

        // Check if the first part is a module alias or imported symbol that needs expansion
        // e.g., "T.Say" where T is an alias for "Zanna.Terminal" becomes "Zanna.Terminal.Say"
        // or "Canvas.New" where Canvas is imported from Zanna.Graphics becomes
        // "Zanna.Graphics.Canvas.New"
        auto dotPos = dottedName.find('.');
        if (dotPos != std::string::npos) {
            std::string firstPart = dottedName.substr(0, dotPos);
            std::string rest = dottedName.substr(dotPos + 1);

            if (Symbol *rootSym = currentScope_->lookup(firstPart);
                rootSym && rootSym->kind != Symbol::Kind::Module &&
                rootSym->kind != Symbol::Kind::Type) {
                dottedName.clear();
            } else if (auto moduleExports = findModuleExports(firstPart, expr->callee->loc)) {
                auto exportDot = rest.find('.');
                std::string exportName =
                    exportDot == std::string::npos ? rest : rest.substr(0, exportDot);
                auto exportIt = moduleExports->find(exportName);
                if (exportIt != moduleExports->end()) {
                    const Symbol &exportSym = exportIt->second;
                    std::string suffix =
                        exportDot == std::string::npos ? "" : rest.substr(exportDot + 1);
                    if (suffix.empty()) {
                        if (exportSym.kind == Symbol::Kind::Function) {
                            dottedName = exportSym.name;
                        } else if (exportSym.kind == Symbol::Kind::Module && exportSym.type) {
                            dottedName = exportSym.type->name;
                        }
                    } else if (exportSym.kind == Symbol::Kind::Module && exportSym.type) {
                        dottedName = exportSym.type->name + "." + suffix;
                    }
                    viaQualifiedModule = true;
                }
            }

            // Check if firstPart is a module alias (bound namespace with alias)
            if (!dottedName.empty()) {
                auto aliasIt = aliasToNamespace_.find(firstPart);
                if (aliasIt != aliasToNamespace_.end()) {
                    // Expand the alias: T.Say -> Zanna.Terminal.Say
                    dottedName = aliasIt->second + "." + rest;
                    viaQualifiedModule = true;
                }
            }

            // Check if firstPart is an imported symbol (e.g., Canvas from Zanna.Graphics)
            if (!dottedName.empty()) {
                auto importIt = importedSymbols_.find(firstPart);
                if (importIt != importedSymbols_.end()) {
                    // Expand: Canvas.New -> Zanna.Graphics.Canvas.New
                    dottedName = importIt->second + "." + rest;
                    viaQualifiedModule = true;
                }
            }
        }

        if (!dottedName.empty()) {
            if (dottedName == "Zanna.Collections.Seq.New" && expr->args.size() == 1)
                dottedName = "Zanna.Collections.Seq.NewSized";
            else if (dottedName == "Zanna.String.FromInt")
                dottedName = "Zanna.Core.Convert.ToStringInt";
            else if (dottedName == "Zanna.String.FromDouble")
                dottedName = "Zanna.Core.Convert.ToStringDouble";

            analyzeArgTypes();

            std::string loweredName;
            CallArgBinding binding;
            if (FunctionDecl *func = resolveFunctionCallOverload(
                    dottedName, expr, expr->loc, &loweredName, &binding, viaQualifiedModule)) {
                TypeRef funcType = functionDeclTypes_[func];
                resolvedFunctionCallees_[expr] = loweredName;
                resolvedFunctionDecls_[expr] = func;
                callArgBindings_[expr] = binding;
                exprTypes_[expr->callee.get()] = funcType;
                return funcType && funcType->kind == TypeKindSem::Function
                           ? normalizeRuntimeSurfaceType(funcType->returnType())
                           : types::unknown();
            }

            // Check if it's a known function (runtime or user-defined with qualified name)
            Symbol *sym = lookupAccessibleSymbol(dottedName, expr->loc, viaQualifiedModule);
            if (sym && sym->kind == Symbol::Kind::Function) {
                // Bug #024 fix: Store the callee's type so the lowerer can access it
                // The lowerer uses sema_.typeOf(expr->callee.get()) to determine return type
                TypeRef funcType = sym->type;
                exprTypes_[expr->callee.get()] = funcType;

                // Analyze arguments
                for (auto &arg : expr->args) {
                    analyzeExpr(arg.value.get());
                }

                // Only validate user-defined functions — runtime externs have
                // implicit self params that don't appear in Zia call syntax.
                if (!sym->isExtern) {
                    validateCallArgs(expr, funcType, dottedName);
                } else {
                    TypeRef terminalTextResult = nullptr;
                    if (tryBindTerminalTextCall(expr, dottedName, sym, terminalTextResult))
                        return terminalTextResult;
                    if (!bindExternCallOnCall(expr, dottedName, sym))
                        return types::unknown();
                }

                // For extern functions (runtime library), store the resolved call info
                // so the lowerer knows to emit an extern call
                if (sym->isExtern) {
                    runtimeCallees_[expr] = dottedName;
                }
                // Bug #023 fix: Return the function's return type, not the function type itself
                if (funcType && funcType->kind == TypeKindSem::Function) {
                    return refineRuntimeCallReturnType(expr, dottedName, funcType->returnType());
                }

                return normalizeRuntimeSurfaceType(funcType);
            }
        }
    }

    // Handle special built-in method calls on collections
    // This allows list.count() as an alternative to list.count
    if (expr->callee->kind == ExprKind::Field) {
        auto *fieldExpr = static_cast<FieldExpr *>(expr->callee.get());

        // Functional combinators are target-typed: their closure argument must be
        // analyzed with an expected function type, so handle them before the eager
        // argument analysis below would reject an untyped lambda.
        {
            const std::string &f = fieldExpr->field;
            if (f == "map" || f == "filter" || f == "reduce" || f == "firstWhere" || f == "any" ||
                f == "all" || f == "sum") {
                TypeRef combinatorBase = analyzeExpr(fieldExpr->base.get());
                if (combinatorBase && combinatorBase->kind == TypeKindSem::List) {
                    if (auto result = analyzeListCombinatorCall(expr, f, combinatorBase))
                        return *result;
                }
            }
        }

        analyzeArgTypes();

        if (fieldExpr->base->kind == ExprKind::SuperExpr && currentSelfType_ &&
            currentSelfType_->kind == TypeKindSem::Class) {
            auto classIt = classDecls_.find(currentSelfType_->name);
            if (classIt != classDecls_.end() && !classIt->second->baseClass.empty()) {
                std::string resolvedOwner;
                CallArgBinding binding;
                if (MethodDecl *method = resolveMethodCallOverload(classIt->second->baseClass,
                                                                   fieldExpr->field,
                                                                   expr,
                                                                   expr->loc,
                                                                   &resolvedOwner,
                                                                   true,
                                                                   &binding)) {
                    resolvedMethodDecls_[expr] = method;
                    resolvedMethodOwnerTypes_[expr] = resolvedOwner;
                    resolvedMethodSlotKeys_[expr] = methodSlotKey(resolvedOwner, method);
                    callArgBindings_[expr] = binding;
                    TypeRef methodType = getMethodType(resolvedOwner, method);
                    exprTypes_[expr->callee.get()] = methodType;
                    return methodType && methodType->kind == TypeKindSem::Function
                               ? normalizeRuntimeSurfaceType(methodType->returnType())
                               : types::unknown();
                }
            }
        }

        TypeRef baseType = analyzeExpr(fieldExpr->base.get());

        // Helper to analyze all arguments
        auto analyzeArgs = [&]() {
            for (auto &arg : expr->args) {
                analyzeExpr(arg.value.get());
            }
        };

        if (baseType &&
            (baseType->kind == TypeKindSem::Struct || baseType->kind == TypeKindSem::Class ||
             baseType->kind == TypeKindSem::Interface)) {
            std::string resolvedOwner;
            CallArgBinding binding;
            if (MethodDecl *method =
                    resolveMethodCallOverload(baseType->name,
                                              fieldExpr->field,
                                              expr,
                                              expr->loc,
                                              &resolvedOwner,
                                              baseType->kind != TypeKindSem::Interface,
                                              &binding)) {
                bool isInsideType = currentSelfType_ && currentSelfType_->name == resolvedOwner;
                if (method->visibility == Visibility::Private && !isInsideType) {
                    error(expr->loc,
                          "Cannot access private member '" + fieldExpr->field + "' of type '" +
                              resolvedOwner + "'");
                    return types::unknown();
                }

                resolvedMethodDecls_[expr] = method;
                resolvedMethodOwnerTypes_[expr] = resolvedOwner;
                resolvedMethodSlotKeys_[expr] = methodSlotKey(resolvedOwner, method);
                callArgBindings_[expr] = binding;
                TypeRef methodType = getMethodType(resolvedOwner, method);
                exprTypes_[expr->callee.get()] = methodType;
                return methodType && methodType->kind == TypeKindSem::Function
                           ? normalizeRuntimeSurfaceType(methodType->returnType())
                           : types::unknown();
            }
        }

        auto analyzeAllArgs = [&]() {
            for (auto &arg : expr->args)
                analyzeExpr(arg.value.get());
        };

        auto checkArgCount = [&](size_t expected, const std::string &methodName) -> bool {
            analyzeAllArgs();
            if (expr->args.size() == expected)
                return true;
            error(expr->loc,
                  methodName + "() expects " + std::to_string(expected) + " argument" +
                      (expected == 1 ? "" : "s") + ", got " + std::to_string(expr->args.size()));
            return false;
        };

        auto checkArgType = [&](size_t index, TypeRef expected, const std::string &label) {
            if (index >= expr->args.size() || !expected)
                return;
            TypeRef actual = exprTypes_.count(expr->args[index].value.get())
                                 ? exprTypes_[expr->args[index].value.get()]
                                 : analyzeExpr(expr->args[index].value.get());
            if (actual && actual->kind != TypeKindSem::Unknown &&
                actual->kind != TypeKindSem::Error && expected->kind != TypeKindSem::Unknown &&
                expected->kind != TypeKindSem::Error && !expected->isAssignableFrom(*actual)) {
                error(expr->args[index].value->loc,
                      label + " expects " + expected->toDisplayString() + ", got " +
                          actual->toDisplayString());
            }
        };

        // Handle List methods using lookup table
        if (baseType && baseType->kind == TypeKindSem::List) {
            // Range modifier methods — .rev() and .step(n) return same type
            if (fieldExpr->field == "rev") {
                if (!isRangeModifierChain(expr)) {
                    error(expr->loc, "rev() is only supported on range expressions");
                    return types::unknown();
                }
                checkArgCount(0, "rev");
                return baseType;
            }
            if (fieldExpr->field == "step") {
                unsigned stepCount = 0;
                if (!isRangeModifierChain(expr, &stepCount)) {
                    error(expr->loc, "step() is only supported on range expressions");
                    return types::unknown();
                }
                if (stepCount > 1)
                    error(expr->loc, "range expressions cannot apply step() more than once");
                if (checkArgCount(1, "step")) {
                    checkArgType(0, types::integer(), "step() argument");
                    if (expr->args[0].value && expr->args[0].value->kind == ExprKind::IntLiteral) {
                        auto *lit = static_cast<IntLiteralExpr *>(expr->args[0].value.get());
                        if (lit->value <= 0)
                            error(expr->args[0].value->loc,
                                  "step() argument must be a positive non-zero integer");
                    }
                }
                return baseType;
            }
            if (auto method =
                    common::findCollectionMethod(common::CollectionKind::List, fieldExpr->field)) {
                TypeRef elemType =
                    baseType->elementType() ? baseType->elementType() : types::unknown();
                if (fieldExpr->field == "get") {
                    if (checkArgCount(1, fieldExpr->field))
                        checkArgType(0, types::integer(), "get() index");
                } else if (fieldExpr->field == "first" || fieldExpr->field == "last" ||
                           fieldExpr->field == "pop" || fieldExpr->field == "len" ||
                           fieldExpr->field == "count" || fieldExpr->field == "size" ||
                           fieldExpr->field == "length" || fieldExpr->field == "isEmpty" ||
                           fieldExpr->field == "clear" || fieldExpr->field == "reverse" ||
                           fieldExpr->field == "sort" || fieldExpr->field == "sortDesc" ||
                           fieldExpr->field == "shuffle") {
                    checkArgCount(0, fieldExpr->field);
                } else if (fieldExpr->field == "contains" || fieldExpr->field == "remove" ||
                           fieldExpr->field == "find" || fieldExpr->field == "indexOf") {
                    if (checkArgCount(1, fieldExpr->field)) {
                        TypeRef argType = exprTypes_[expr->args[0].value.get()];
                        if (fieldExpr->field == "remove" && argType &&
                            argType->kind == TypeKindSem::Integer &&
                            elemType->kind != TypeKindSem::Integer) {
                            error(expr->args[0].value->loc,
                                  "remove() expects element type; use removeAt() to remove by "
                                  "index");
                        } else {
                            checkArgType(0, elemType, fieldExpr->field + "() value");
                        }
                    }
                } else if (fieldExpr->field == "push" || fieldExpr->field == "add") {
                    if (checkArgCount(1, fieldExpr->field))
                        checkArgType(0, elemType, fieldExpr->field + "() value");
                } else if (fieldExpr->field == "insert" || fieldExpr->field == "set") {
                    if (checkArgCount(2, fieldExpr->field)) {
                        checkArgType(0, types::integer(), fieldExpr->field + "() index");
                        checkArgType(1, elemType, fieldExpr->field + "() value");
                    }
                } else if (fieldExpr->field == "removeAt") {
                    if (checkArgCount(1, fieldExpr->field))
                        checkArgType(0, types::integer(), "removeAt() index");
                } else {
                    analyzeAllArgs();
                }
                return resolveMethodReturnType(method->returnKind, baseType);
            }
        }

        // Handle Map methods using lookup table
        if (baseType && baseType->kind == TypeKindSem::Map) {
            if (auto method =
                    common::findCollectionMethod(common::CollectionKind::Map, fieldExpr->field)) {
                TypeRef keyType = baseType->keyType() ? baseType->keyType() : types::unknown();
                TypeRef valueType =
                    baseType->valueType() ? baseType->valueType() : types::unknown();
                if (fieldExpr->field == "get") {
                    if (checkArgCount(1, fieldExpr->field))
                        checkArgType(0, keyType, "Map key");
                } else if (fieldExpr->field == "getOr") {
                    if (checkArgCount(2, fieldExpr->field)) {
                        checkArgType(0, keyType, "Map key");
                        checkArgType(1, valueType, "getOr() fallback");
                    }
                } else if (fieldExpr->field == "set" || fieldExpr->field == "put" ||
                           fieldExpr->field == "setIfMissing") {
                    if (checkArgCount(2, fieldExpr->field)) {
                        checkArgType(0, keyType, "Map key");
                        checkArgType(1, valueType, fieldExpr->field + "() value");
                    }
                } else if (fieldExpr->field == "containsKey" || fieldExpr->field == "hasKey" ||
                           fieldExpr->field == "has" || fieldExpr->field == "remove") {
                    if (checkArgCount(1, fieldExpr->field))
                        checkArgType(0, keyType, "Map key");
                } else if (fieldExpr->field == "len" || fieldExpr->field == "size" ||
                           fieldExpr->field == "count" || fieldExpr->field == "length" ||
                           fieldExpr->field == "clear" || fieldExpr->field == "keys" ||
                           fieldExpr->field == "values") {
                    checkArgCount(0, fieldExpr->field);
                } else {
                    analyzeAllArgs();
                }
                return resolveMethodReturnType(method->returnKind, baseType);
            }
        }

        // Handle Set methods using lookup table
        if (baseType && baseType->kind == TypeKindSem::Set) {
            if (auto method =
                    common::findCollectionMethod(common::CollectionKind::Set, fieldExpr->field)) {
                TypeRef elemType =
                    baseType->elementType() ? baseType->elementType() : types::unknown();
                if (fieldExpr->field == "contains" || fieldExpr->field == "has" ||
                    fieldExpr->field == "add" || fieldExpr->field == "remove") {
                    if (checkArgCount(1, fieldExpr->field))
                        checkArgType(0, elemType, fieldExpr->field + "() value");
                } else if (fieldExpr->field == "len" || fieldExpr->field == "size" ||
                           fieldExpr->field == "count" || fieldExpr->field == "length" ||
                           fieldExpr->field == "clear") {
                    checkArgCount(0, fieldExpr->field);
                } else {
                    analyzeAllArgs();
                }
                return resolveMethodReturnType(method->returnKind, baseType);
            }
        }

        // Fallback: Map semantic collection types to runtime class methods.
        // Handles runtime-specific methods (get_Length, Put, First, etc.) that aren't
        // in the built-in Zia-friendly method tables above.
        if (baseType &&
            (baseType->kind == TypeKindSem::Set || baseType->kind == TypeKindSem::List ||
             baseType->kind == TypeKindSem::Map)) {
            std::string className;
            if (baseType->kind == TypeKindSem::Set)
                className = "Zanna.Collections.Set";
            else if (baseType->kind == TypeKindSem::List)
                className = "Zanna.Collections.List";
            else
                className = "Zanna.Collections.Map";

            std::string fullMethodName = className + "." + fieldExpr->field;
            std::optional<il::runtime::ParsedMethod> resolvedRuntimeMethod;
            Symbol *sym = lookupSymbol(fullMethodName);
            if (!sym) {
                std::string fallbackMethodName =
                    className + "." + capitalizedRuntimeMember(fieldExpr->field);
                sym = lookupSymbol(fallbackMethodName);
                if (sym && sym->kind == Symbol::Kind::Function)
                    fullMethodName = fallbackMethodName;
            }
            const auto &registry = il::runtime::RuntimeRegistry::instance();
            if (auto method = registry.findMethod(className, fieldExpr->field, expr->args.size());
                method && method->target && *method->target) {
                if (Symbol *methodSym = lookupSymbol(method->target);
                    methodSym && methodSym->kind == Symbol::Kind::Function) {
                    resolvedRuntimeMethod = *method;
                    sym = methodSym;
                    fullMethodName = method->target;
                }
            }
            if (sym && sym->kind == Symbol::Kind::Function) {
                analyzeArgs();
                const size_t receiverSkip = runtimeMethodReceiverSkip(
                    sym, resolvedRuntimeMethod ? &*resolvedRuntimeMethod : nullptr);
                if (!bindExternCallOnCall(expr, fullMethodName, sym, receiverSkip))
                    return types::unknown();
                if (sym->isExtern) {
                    runtimeCallees_[expr] = fullMethodName;
                }
                if (sym->type && sym->type->kind == TypeKindSem::Function) {
                    return refineRuntimeCallReturnType(
                        expr, fullMethodName, sym->type->returnType());
                }
                return normalizeRuntimeSurfaceType(sym->type);
            }
            analyzeAllArgs();
            std::string typeName = baseType->kind == TypeKindSem::List  ? "List"
                                   : baseType->kind == TypeKindSem::Map ? "Map"
                                                                        : "Set";
            error(expr->loc, typeName + " has no method '" + fieldExpr->field + "'");
            return types::unknown();
        }

        // Handle String methods using lookup table
        if (baseType && baseType->kind == TypeKindSem::String) {
            if (auto method = common::findCollectionMethod(common::CollectionKind::String,
                                                           fieldExpr->field)) {
                checkArgCount(0, fieldExpr->field);
                return resolveMethodReturnType(method->returnKind, baseType);
            }

            std::string fullMethodName = "Zanna.String." + fieldExpr->field;
            Symbol *sym = nullptr;
            const auto &registry = il::runtime::RuntimeRegistry::instance();
            std::optional<il::runtime::ParsedMethod> resolvedRuntimeMethod;
            if (auto method =
                    registry.findMethod("Zanna.String", fieldExpr->field, expr->args.size());
                method && method->target && *method->target) {
                sym = lookupSymbol(method->target);
                if (sym && sym->kind == Symbol::Kind::Function) {
                    resolvedRuntimeMethod = *method;
                    fullMethodName = method->target;
                }
            }

            if (!sym)
                sym = lookupSymbol(fullMethodName);

            if (sym && sym->kind == Symbol::Kind::Function) {
                analyzeArgs();
                const size_t receiverSkip = runtimeMethodReceiverSkip(
                    sym, resolvedRuntimeMethod ? &*resolvedRuntimeMethod : nullptr);
                if (!bindExternCallOnCall(expr, fullMethodName, sym, receiverSkip))
                    return types::unknown();

                if (sym->isExtern)
                    runtimeCallees_[expr] = fullMethodName;

                if (sym->type && sym->type->kind == TypeKindSem::Function) {
                    return refineRuntimeCallReturnType(
                        expr, fullMethodName, sym->type->returnType());
                }
                return normalizeRuntimeSurfaceType(sym->type);
            }

            analyzeArgs();
            error(expr->loc, "String has no method '" + fieldExpr->field + "'");
            return types::unknown();
        }

        // Emit a diagnostic for method calls on an untyped opaque pointer (plain 'obj').
        // This occurs when a runtime function that returns obj/ptr without a typed seq
        // annotation is used as a method receiver. The typed Seq API (Seq.Get, Seq.get_Count)
        // must be used instead, or the runtime.def entry should be annotated with seq<T>.
        if (baseType && baseType->kind == TypeKindSem::Ptr && baseType->name.empty()) {
            error(expr->loc,
                  "Cannot call method on an untyped object reference. "
                  "Use Seq.Get/Seq.get_Count for sequence results, or check the runtime.def "
                  "annotation for the function returning this value.");
            for (auto &arg : expr->args)
                analyzeExpr(arg.value.get());
            return types::unknown();
        }

        // Handle runtime class method calls (e.g., canvas.Poll(), canvas.Clear())
        // Runtime classes have names starting with "Zanna." and are registered in typeRegistry_
        if (baseType && baseType->name.find("Zanna.") == 0) {
            // Construct full method name: ClassName.MethodName
            std::string fullMethodName = baseType->name + "." + fieldExpr->field;

            Symbol *sym = nullptr;
            const auto &registry = il::runtime::RuntimeRegistry::instance();
            std::optional<il::runtime::ParsedMethod> resolvedRuntimeMethod;
            std::vector<std::string> methodNames = {fieldExpr->field};
            std::string capitalized = capitalizedRuntimeMember(fieldExpr->field);
            if (capitalized != fieldExpr->field)
                methodNames.push_back(std::move(capitalized));
            for (const auto &methodName : methodNames) {
                if (auto method =
                        registry.findMethod(baseType->name, methodName, expr->args.size());
                    method && method->target && *method->target) {
                    sym = lookupSymbol(method->target);
                    if (sym && sym->kind == Symbol::Kind::Function) {
                        resolvedRuntimeMethod = *method;
                        fullMethodName = method->target;
                        break;
                    }
                }
            }

            // Fall back to direct qualified name lookup
            if (!sym) {
                for (const auto &methodName : methodNames) {
                    std::string candidateName = baseType->name + "." + methodName;
                    sym = lookupSymbol(candidateName);
                    if (sym && sym->kind == Symbol::Kind::Function) {
                        fullMethodName = std::move(candidateName);
                        break;
                    }
                }
            }

            // If not found and this is a GUI widget class, try falling back to Widget base class
            // This handles inherited methods like SetSize, AddChild, SetVisible, etc.
            if (!sym && baseType->name.find("Zanna.GUI.") == 0 &&
                baseType->name != "Zanna.GUI.Widget") {
                for (const auto &methodName : methodNames) {
                    if (auto method =
                            registry.findMethod("Zanna.GUI.Widget", methodName, expr->args.size());
                        method && method->target && *method->target) {
                        sym = lookupSymbol(method->target);
                        if (sym && sym->kind == Symbol::Kind::Function) {
                            resolvedRuntimeMethod = *method;
                            fullMethodName = method->target;
                            break;
                        }
                    }

                    std::string widgetMethodName = "Zanna.GUI.Widget." + methodName;
                    Symbol *widgetSym = lookupSymbol(widgetMethodName);
                    if (widgetSym && widgetSym->kind == Symbol::Kind::Function) {
                        sym = widgetSym;
                        fullMethodName = std::move(widgetMethodName);
                        break;
                    }
                }
            }

            if (sym && sym->kind == Symbol::Kind::Function) {
                // Analyze arguments
                for (auto &arg : expr->args) {
                    analyzeExpr(arg.value.get());
                }

                const size_t receiverSkip = runtimeMethodReceiverSkip(
                    sym, resolvedRuntimeMethod ? &*resolvedRuntimeMethod : nullptr);
                if (!bindExternCallOnCall(expr, fullMethodName, sym, receiverSkip))
                    return types::unknown();

                // Skip validation for runtime class methods — their signatures
                // include implicit self parameters.

                // Store the resolved runtime call info for the lowerer
                if (sym->isExtern) {
                    runtimeCallees_[expr] = fullMethodName;
                }
                // Return the function's return type, not the function type itself.
                // This is critical for chained method calls (e.g., bytes.Slice(x,y).ToStr())
                // where the caller needs the return type to resolve the next method.
                if (sym->type && sym->type->kind == TypeKindSem::Function) {
                    return refineRuntimeCallReturnType(
                        expr, fullMethodName, sym->type->returnType());
                }
                return normalizeRuntimeSurfaceType(sym->type);
            }

            for (auto &arg : expr->args) {
                analyzeExpr(arg.value.get());
            }
            error(expr->loc,
                  "Runtime class '" + baseType->name + "' has no method '" + fieldExpr->field +
                      "'");
            return types::unknown();
        }
    }

    TypeRef calleeType = analyzeExpr(expr->callee.get());

    // Analyze arguments
    for (auto &arg : expr->args) {
        analyzeExpr(arg.value.get());
    }

    if (!calleeType)
        return types::unknown();

    // If callee is a function type, validate args and return its return type
    if (calleeType->kind == TypeKindSem::Function) {
        validateCallArgs(expr, calleeType, "function");
        return normalizeRuntimeSurfaceType(calleeType->returnType());
    }

    // If callee is unknown, return unknown
    if (calleeType->kind == TypeKindSem::Unknown) {
        return types::unknown();
    }

    error(expr->loc, "Expression is not callable");
    return types::unknown();
}

} // namespace il::frontends::zia
