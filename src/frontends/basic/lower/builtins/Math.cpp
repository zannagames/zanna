//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/builtins/Math.cpp
// Purpose: Implements and registers generic mathematical builtin lowering plus
//          guarded numeric and VAL conversion dispatch.
// Key invariants: Selected variants control argument transformations and
//                 runtime feature application; failed selection yields a typed
//                 zero fallback.
// Ownership/Lifetime: Registry callbacks borrow their per-call lowering
//                     contexts and return IL values owned by Lowerer state.
// Links: src/frontends/basic/lower/BuiltinCommon.hpp,
//        src/frontends/basic/builtins/MathBuiltins.cpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements BASIC math and conversion builtin callback registration.

#include "frontends/basic/lower/BuiltinCommon.hpp"
#include "frontends/basic/lower/builtins/Registrars.hpp"

#include "frontends/basic/BuiltinRegistry.hpp"

#include <cassert>
#include <vector>

namespace il::frontends::basic::lower {
namespace {
using IlType = il::core::Type;
using IlKind = IlType::Kind;
using Value = il::core::Value;
using Variant = BuiltinLoweringRule::Variant;
} // namespace

/// @brief Lower numeric conversion builtins that require runtime guard logic.
/// @details Chooses the appropriate lowering variant for the call, determines
///          which builtin is being processed, and routes to either the shared
///          numeric conversion helper or a specialised lowering path.  After the
///          call is lowered the helper applies any feature gates advertised by
///          the variant so downstream passes know which runtime helpers were
///          touched.
/// @param ctx Context describing the builtin invocation to lower.
/// @return Resulting value/type pair produced by the lowering process.
Lowerer::RVal lowerConversionBuiltinImpl(BuiltinLowerContext &ctx) {
    const Variant *variant = ctx.selectVariant();
    if (!variant)
        return ctx.makeZeroResult();

    Lowerer::RVal result = ctx.makeZeroResult();
    switch (ctx.call().builtin) {
        case BuiltinCallExpr::Builtin::Cint:
            result =
                lowerNumericConversion(ctx, *variant, IlType(IlKind::I64), "cint_ok", "cint_trap");
            break;
        case BuiltinCallExpr::Builtin::Clng:
            result =
                lowerNumericConversion(ctx, *variant, IlType(IlKind::I64), "clng_ok", "clng_trap");
            break;
        case BuiltinCallExpr::Builtin::Csng:
            result =
                lowerNumericConversion(ctx, *variant, IlType(IlKind::F64), "csng_ok", "csng_trap");
            break;
        case BuiltinCallExpr::Builtin::Val:
            result = lowerValBuiltin(ctx, *variant);
            break;
        default:
            result = emitBuiltinVariant(ctx, *variant);
            break;
    }

    ctx.applyFeatures(*variant);
    return result;
}

/// @brief Lower a numeric conversion builtin that traps on invalid input.
/// @details Emits the runtime conversion call, creates success/trap guard
///          blocks, and triggers a conversion trap diagnostic when the runtime
///          reports failure.  The helper is parameterised by the target IL type
///          and block hint strings so different builtins can share the same
///          implementation while still surfacing meaningful block names in
///          generated IL.
/// @param ctx Lowering context used to emit IR and diagnostics.
/// @param variant Rule describing the runtime entry point and argument transforms.
/// @param resultType IL type produced by the conversion.
/// @param contHint Suggested name for the success continuation block.
/// @param trapHint Suggested name for the trap block.
/// @return Resulting value and type from the lowered call.
Lowerer::RVal lowerNumericConversion(BuiltinLowerContext &ctx,
                                     const Variant &variant,
                                     IlType resultType,
                                     const char *contHint,
                                     const char *trapHint) {
    assert(!variant.arguments.empty());
    const auto &argSpec = variant.arguments.front();
    Lowerer::RVal &argVal = ctx.applyTransforms(argSpec, argSpec.transforms);
    const il::support::SourceLoc callLoc = ctx.callLoc(variant.callLocArg);

    Value okSlot = ctx.emitAlloca(1);
    std::vector<Value> callArgs{argVal.value, okSlot};
    ctx.setCurrentLoc(callLoc);
    Value callRes = ctx.emitCall(resultType, variant.runtime, callArgs);

    ctx.setCurrentLoc(callLoc);
    Value okVal = ctx.emitLoad(ctx.boolType(), okSlot);

    BuiltinLowerContext::BranchPair guards = ctx.createGuardBlocks(contHint, trapHint);
    if (!guards.cont || !guards.trap)
        return {callRes, resultType};

    ctx.emitCBr(okVal, guards.cont, guards.trap);

    ctx.setCurrentBlock(guards.trap);
    ctx.emitConversionTrap(callLoc);

    ctx.setCurrentBlock(guards.cont);
    return {callRes, resultType};
}

} // namespace il::frontends::basic::lower

namespace il::frontends::basic::lower::builtins {
namespace {
using Builtin = BuiltinCallExpr::Builtin;

/// @brief Defer math builtins to the generic rule-based lowering pipeline.
/// @details Math builtins currently require no bespoke handling beyond what the
///          declarative lowering rules provide.  Returning the generic lowering
///          result keeps the implementation uniform and makes future
///          customisations straightforward.
/// @param ctx Call-specific lowering context.
/// @return Lowered value/type pair obtained from the generic pipeline.
Lowerer::RVal lowerMathBuiltin(BuiltinLowerContext &ctx) {
    return lowerGenericBuiltin(ctx);
}

/// @brief Entry point that lowers any conversion builtin routed through the registrar.
/// @details Serves as a thin wrapper around @ref lowerConversionBuiltinImpl so the
///          registrar can bind a stable function pointer while leaving room for
///          additional bookkeeping if conversion lowering ever needs to grow.
/// @param ctx Call-specific lowering context.
/// @return Lowered value/type pair for the conversion builtin.
Lowerer::RVal lowerConversionBuiltin(BuiltinLowerContext &ctx) {
    return lowerConversionBuiltinImpl(ctx);
}

} // namespace

/// @brief Register core math builtins with the lowering registry.
/// @details Associates each math builtin enumerator with
///          @ref lowerMathBuiltin, enabling the dispatcher to invoke the generic
///          lowering path when those builtins appear in the source program.
void registerMathBuiltins() {
    register_builtin(getBuiltinInfo(Builtin::Cdbl).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Int).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Fix).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Round).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Sqr).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Abs).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Floor).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Ceil).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Sin).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Cos).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Tan).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Atn).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Exp).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Log).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Sgn).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Pow).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Rnd).name, &lowerMathBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Timer).name, &lowerMathBuiltin);
}

/// @brief Register numeric conversion builtins with the lowering registry.
/// @details Installs @ref lowerConversionBuiltin as the lowering hook for every
///          conversion builtin so the dispatcher can route calls that require
///          runtime guard handling.
void registerConversionBuiltins() {
    register_builtin(getBuiltinInfo(Builtin::Val).name, &lowerConversionBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Cint).name, &lowerConversionBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Clng).name, &lowerConversionBuiltin);
    register_builtin(getBuiltinInfo(Builtin::Csng).name, &lowerConversionBuiltin);
}

} // namespace il::frontends::basic::lower::builtins
