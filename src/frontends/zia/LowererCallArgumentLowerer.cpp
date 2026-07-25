//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file LowererCallArgumentLowerer.cpp
/// @brief CallArgumentLowerer implementation.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/LowererCallArgumentLowerer.hpp"

#include "frontends/zia/RuntimeNames.hpp"
#include "frontends/zia/ZiaSupport.hpp"

namespace il::frontends::zia {

using namespace runtime;

/// @brief Lower each call argument expression in source order.
/// @param args The call's argument expressions.
/// @return The lowered values, parallel to @p args (no reordering or coercion applied).
std::vector<LowerResult> CallArgumentLowerer::lowerSourceArgs(const std::vector<CallArg> &args) {
    std::vector<LowerResult> lowered;
    lowered.reserve(args.size());
    for (const auto &arg : args)
        lowered.push_back(lowerer_.lowerExpr(arg.value.get()));
    return lowered;
}

/// @brief Compute the source-argument indices in resolved parameter order.
/// @param args The call's argument expressions.
/// @param binding Sema's argument binding, or null for positional calls.
/// @return Source indices ordered as fixed parameters followed by variadics; identity order
///         (0..n-1) when there is no binding.
std::vector<int> CallArgumentLowerer::orderedArgSources(const std::vector<CallArg> &args,
                                                        const Sema::CallArgBinding *binding) {
    std::vector<int> order;
    if (!binding || binding->fixedParamSources.empty()) {
        order.reserve(args.size());
        for (size_t i = 0; i < args.size(); ++i)
            order.push_back(static_cast<int>(i));
        return order;
    }

    order.reserve(binding->fixedParamSources.size() + binding->variadicSources.size());
    for (int sourceIndex : binding->fixedParamSources) {
        if (sourceIndex >= 0)
            order.push_back(sourceIndex);
    }
    for (int sourceIndex : binding->variadicSources)
        order.push_back(sourceIndex);
    return order;
}

/// @brief Lower call arguments into final values laid out in parameter order.
/// @param args The call's argument expressions.
/// @param paramTypes The callee's parameter types (the last is the element-list type if variadic).
/// @param params The callee's parameter declarations (for defaults and variadic detection), or
/// null.
/// @param binding Sema's argument binding; null falls back to positional lowering.
/// @return The argument values in parameter order, ready to pass to the call.
/// @details Each fixed argument is coerced to its parameter type; omitted parameters with a
///          default expression are filled from that default. Variadic parameters collect their
///          source arguments into a freshly built, boxed runtime list passed as the final value.
std::vector<Lowerer::Value> CallArgumentLowerer::lowerResolvedArgs(
    const std::vector<CallArg> &args,
    const std::vector<TypeRef> &paramTypes,
    const std::vector<Param> *params,
    const Sema::CallArgBinding *binding) {
    auto lowered = lowerSourceArgs(args);

    if (!binding) {
        std::vector<Value> fallback;
        fallback.reserve(args.size());
        for (size_t i = 0; i < args.size(); ++i) {
            TypeRef argType = lowerer_.sema_.typeOf(args[i].value.get());
            TypeRef paramType = i < paramTypes.size() ? paramTypes[i] : nullptr;
            auto coerced =
                lowerer_.coerceValueToType(lowered[i].value, lowered[i].type, argType, paramType);
            fallback.push_back(coerced.value);
        }
        return fallback;
    }

    const bool hasVariadic = params && !params->empty() && params->back().isVariadic;
    const size_t fixedCount = hasVariadic ? paramTypes.size() - 1 : paramTypes.size();

    std::vector<Value> finalArgs;
    finalArgs.reserve(paramTypes.size());

    for (size_t i = 0; i < fixedCount; ++i) {
        int sourceIndex =
            i < binding->fixedParamSources.size() ? binding->fixedParamSources[i] : -1;
        if (sourceIndex >= 0) {
            size_t idx = toIndex(sourceIndex);
            TypeRef argType = lowerer_.sema_.typeOf(args[idx].value.get());
            auto coerced = lowerer_.coerceValueToType(
                lowered[idx].value, lowered[idx].type, argType, paramTypes[i]);
            finalArgs.push_back(coerced.value);
            continue;
        }

        if (params && i < params->size() && (*params)[i].defaultValue) {
            auto defaultResult = lowerer_.lowerExpr((*params)[i].defaultValue.get());
            TypeRef defaultType = lowerer_.sema_.typeOf((*params)[i].defaultValue.get());
            auto coerced = lowerer_.coerceValueToType(
                defaultResult.value, defaultResult.type, defaultType, paramTypes[i]);
            finalArgs.push_back(coerced.value);
            continue;
        }

        lowerer_.reportLoweringInvariant(params && i < params->size() ? (*params)[i].loc
                                                                      : il::support::SourceLoc{},
                                         "V-ZIA-LOWER-MISSING-CALL-ARG",
                                         "resolved call binding omitted a required argument");
        Type fallbackType =
            i < paramTypes.size() ? lowerer_.mapType(paramTypes[i]) : Type(Type::Kind::I64);
        finalArgs.push_back(fallbackType.kind == Type::Kind::Ptr ? Value::null()
                                                                 : Value::constInt(0));
    }

    if (hasVariadic && !paramTypes.empty()) {
        Value list = lowerer_.emitCallRet(Type(Type::Kind::Ptr), kListNew, {});
        TypeRef listType = paramTypes.back();
        TypeRef elemType = listType ? listType->elementType() : nullptr;
        Type elemIlType = elemType ? lowerer_.mapType(elemType) : Type(Type::Kind::I64);

        for (int sourceIndex : binding->variadicSources) {
            size_t idx = toIndex(sourceIndex);
            TypeRef argType = lowerer_.sema_.typeOf(args[idx].value.get());
            auto coerced = lowerer_.coerceValueToType(
                lowered[idx].value, lowered[idx].type, argType, elemType);
            Value boxed =
                lowerer_.emitBoxValue(coerced.value, elemIlType, elemType ? elemType : argType);
            lowerer_.emitCall(kListAdd, {list, boxed});
        }

        finalArgs.push_back(list);
    }

    return finalArgs;
}

/// @brief Lower a call expression's arguments using its resolved call binding.
/// @param expr Call expression whose source arguments are lowered.
/// @param paramTypes Resolved parameter types in declaration order.
/// @param params Parameter declarations used for defaults and variadic
///        metadata, or null when unavailable.
/// @return Argument values in parameter order (see lowerResolvedArgs()).
std::vector<Lowerer::Value> CallArgumentLowerer::lowerResolvedCallArgs(
    CallExpr *expr, const std::vector<TypeRef> &paramTypes, const std::vector<Param> *params) {
    return lowerResolvedArgs(expr->args, paramTypes, params, lowerer_.sema_.callArgBinding(expr));
}

/// @brief Lower a `new` expression's constructor arguments using its resolved binding.
/// @param expr Allocation expression whose constructor arguments are lowered.
/// @param paramTypes Resolved constructor parameter types.
/// @param params Constructor parameter declarations used for defaults and
///        variadic metadata, or null when unavailable.
/// @return Argument values in parameter order (see lowerResolvedArgs()).
std::vector<Lowerer::Value> CallArgumentLowerer::lowerResolvedNewArgs(
    NewExpr *expr, const std::vector<TypeRef> &paramTypes, const std::vector<Param> *params) {
    return lowerResolvedArgs(expr->args, paramTypes, params, lowerer_.sema_.newArgBinding(expr));
}

} // namespace il::frontends::zia
