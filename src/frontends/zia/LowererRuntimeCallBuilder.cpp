//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file LowererRuntimeCallBuilder.cpp
/// @brief Implements Zia runtime-call ABI argument coercion.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/LowererRuntimeCallBuilder.hpp"
#include "frontends/zia/ZiaSupport.hpp"

#include "il/runtime/RuntimeSignatures.hpp"

namespace il::frontends::zia {

/// @brief Return true when @p type is an integer-like IL scalar.
/// @param type IL type to classify.
/// @return True for i64, i32, i16, and i1; false otherwise.
static bool isIntegralRuntimeArgType(Lowerer::Type type) {
    switch (type.kind) {
        case Lowerer::Type::Kind::I64:
        case Lowerer::Type::Kind::I32:
        case Lowerer::Type::Kind::I16:
        case Lowerer::Type::Kind::I1:
            return true;
        default:
            return false;
    }
}

/// @brief Emit the runtime ABI coercion for one explicit call argument.
/// @param argValue Lowered argument value.
/// @param argIlType IL type currently attached to @p argValue.
/// @param semanticType Zia type for value-type boxing decisions.
/// @param expectedType Runtime signature parameter type, or null for legacy fallback.
/// @return Value with the type/representation expected by the runtime call.
Lowerer::Value RuntimeCallBuilder::coerceRuntimeArgument(Lowerer::Value argValue,
                                                         Lowerer::Type argIlType,
                                                         TypeRef semanticType,
                                                         const Lowerer::Type *expectedType) {
    if (!expectedType) {
        if (argIlType.kind == Lowerer::Type::Kind::I32)
            return lowerer_.zeroExtendI32(argValue);
        return argValue;
    }

    if (expectedType->kind == Lowerer::Type::Kind::I64 && isIntegralRuntimeArgType(argIlType))
        return lowerer_.widenIntegralToI64(argValue, argIlType);

    if (expectedType->kind == Lowerer::Type::Kind::F64 && isIntegralRuntimeArgType(argIlType)) {
        Lowerer::Value widened = lowerer_.widenIntegralToI64(argValue, argIlType);
        return lowerer_.emitUnary(
            Lowerer::Opcode::Sitofp, Lowerer::Type(Lowerer::Type::Kind::F64), widened);
    }

    if (expectedType->kind == Lowerer::Type::Kind::Ptr &&
        argIlType.kind != Lowerer::Type::Kind::Ptr && argIlType.kind != Lowerer::Type::Kind::Void) {
        LowerResult boxedInput{argValue, argIlType};
        if (argIlType.kind == Lowerer::Type::Kind::I32 ||
            argIlType.kind == Lowerer::Type::Kind::I16) {
            boxedInput = {lowerer_.widenIntegralToI64(argValue, argIlType),
                          Lowerer::Type(Lowerer::Type::Kind::I64)};
        }
        return lowerer_.emitBoxValue(boxedInput.value, boxedInput.type, semanticType);
    }

    if (argIlType.kind == Lowerer::Type::Kind::I32)
        return lowerer_.zeroExtendI32(argValue);

    return argValue;
}

/// @brief Lower and coerce call arguments selected by @p orderedSources.
/// @param args Original source arguments.
/// @param orderedSources Source argument indices in runtime parameter order.
/// @param paramOffset Number of already-emitted implicit runtime parameters.
/// @param descriptor Runtime descriptor used for ABI types, or null.
/// @return Coerced values for the explicit portion of the runtime call.
std::vector<Lowerer::Value> RuntimeCallBuilder::lowerExplicitArgs(
    const std::vector<CallArg> &args,
    const std::vector<int> &orderedSources,
    size_t paramOffset,
    const il::runtime::RuntimeDescriptor *descriptor) {
    std::vector<Lowerer::Value> loweredArgs;
    loweredArgs.reserve(orderedSources.size());

    for (size_t i = 0; i < orderedSources.size(); ++i) {
        const size_t sourceIndex = toIndex(orderedSources[i]);
        auto result = lowerer_.lowerExpr(args[sourceIndex].value.get());
        TypeRef semanticType = lowerer_.sema_.typeOf(args[sourceIndex].value.get());

        const Lowerer::Type *expectedType = nullptr;
        if (descriptor && (paramOffset + i) < descriptor->signature.paramTypes.size())
            expectedType = &descriptor->signature.paramTypes[paramOffset + i];

        loweredArgs.push_back(
            coerceRuntimeArgument(result.value, result.type, semanticType, expectedType));
    }

    return loweredArgs;
}

} // namespace il::frontends::zia
