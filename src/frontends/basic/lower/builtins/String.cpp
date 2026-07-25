//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/builtins/String.cpp
// Purpose: Implements VAL's specialized guarded conversion and registers
//          string builtin callbacks that otherwise use generic lowering rules.
// Key invariants: VAL distinguishes parse failure, NaN, overflow, and success;
//                 other string builtins preserve registry-selected semantics.
// Ownership/Lifetime: Callbacks borrow BuiltinLowerContext and return Lowerer-
//                     owned IL values.
// Links: src/frontends/basic/lower/BuiltinCommon.hpp,
//        src/frontends/basic/builtins/StringBuiltins.cpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements BASIC string builtin lowering and registration.

#include "frontends/basic/lower/BuiltinCommon.hpp"
#include "frontends/basic/lower/builtins/Registrars.hpp"

#include "frontends/basic/BuiltinRegistry.hpp"
#include "frontends/basic/builtins/StringBuiltins.hpp"

#include <cassert>
#include <limits>
#include <vector>

namespace il::frontends::basic::lower {
namespace {
using IlType = il::core::Type;
using IlKind = IlType::Kind;
using Value = il::core::Value;
using Variant = BuiltinLoweringRule::Variant;
using Opcode = il::core::Opcode;
} // namespace

/// @brief Lower the VAL builtin through the runtime conversion helper.
/// @details Applies builtin-specific transforms to the argument, emits the
///          runtime call that performs string-to-number conversion, and wires up
///          the guard blocks that differentiate success, trap, NaN, and overflow
///          exits.  The helper mirrors the numeric conversion flow used by
///          math builtins so diagnostics remain consistent.
/// @param ctx Lowering context providing block builders and runtime hooks.
/// @param variant Concrete lowering rule chosen for the builtin invocation.
/// @return Result value paired with its IL type.
Lowerer::RVal lowerValBuiltin(BuiltinLowerContext &ctx, const Variant &variant) {
    assert(!variant.arguments.empty());
    const auto &argSpec = variant.arguments.front();
    Lowerer::RVal &argVal = ctx.applyTransforms(argSpec, argSpec.transforms);
    const il::support::SourceLoc conversionLoc = ctx.callLoc(variant.callLocArg);

    ctx.setCurrentLoc(conversionLoc);
    Value cstr = ctx.emitCall(IlType(IlKind::Ptr), "rt_string_cstr", {argVal.value});

    Value okSlot = ctx.emitAlloca(1);
    std::vector<Value> callArgs{cstr, okSlot};
    IlType resultType = ctx.resolveResultType();
    ctx.setCurrentLoc(conversionLoc);
    Value callRes = ctx.emitCall(resultType, variant.runtime, callArgs);

    ctx.setCurrentLoc(conversionLoc);
    Value okVal = ctx.emitLoad(ctx.boolType(), okSlot);

    BuiltinLowerContext::ValBlocks blocks = ctx.createValBlocks();
    if (!blocks.cont || !blocks.trap || !blocks.nan || !blocks.overflow)
        return {callRes, resultType};

    ctx.emitCBr(okVal, blocks.cont, blocks.trap);

    ctx.setCurrentBlock(blocks.trap);
    ctx.setCurrentLoc(conversionLoc);
    Value isNan = ctx.emitBinary(Opcode::FCmpNE, ctx.boolType(), callRes, callRes);
    ctx.emitCBr(isNan, blocks.nan, blocks.overflow);

    ctx.setCurrentBlock(blocks.nan);
    ctx.emitConversionTrap(conversionLoc);

    ctx.setCurrentBlock(blocks.overflow);
    ctx.setCurrentLoc(conversionLoc);
    Value overflowSentinel = ctx.emitUnary(Opcode::CastFpToSiRteChk,
                                           IlType(IlKind::I64),
                                           Value::constFloat(std::numeric_limits<double>::max()));
    (void)overflowSentinel;
    ctx.emitTrap();

    ctx.setCurrentBlock(blocks.cont);
    return {callRes, resultType};
}

} // namespace il::frontends::basic::lower

namespace il::frontends::basic::lower::builtins {
namespace {
using Builtin = BuiltinCallExpr::Builtin;
namespace string_builtins = il::frontends::basic::builtins;

/// @brief Dispatch a BASIC string builtin to either specialised or generic lowering.
/// @details Looks up the builtin in the dedicated string registry.  When a
///          specialised lowering routine is available it executes the
///          user-provided callback to obtain the lowered value and result type;
///          otherwise it falls back to the generic rule-driven lowering path so
///          less common functions still compile.
/// @param ctx Call-specific lowering context.
/// @return Lowered value/type pair ready for emission.
Lowerer::RVal lowerStringBuiltin(BuiltinLowerContext &ctx) {
    const auto *stringSpec = string_builtins::findBuiltin(ctx.info().name);
    if (!stringSpec)
        return lowerGenericBuiltin(ctx);

    const std::size_t argCount = ctx.call().args.size();
    if (argCount < static_cast<std::size_t>(stringSpec->minArity) ||
        argCount > static_cast<std::size_t>(stringSpec->maxArity))
        return lowerGenericBuiltin(ctx);

    string_builtins::LowerCtx strCtx(ctx.lowerer(), ctx.call());
    il::core::Value resultValue = stringSpec->fn(strCtx, strCtx.values());
    return {resultValue, strCtx.resultType()};
}

} // namespace

/// @brief Install specialised string builtin lowerers into the shared registry.
/// @details Registers the string dispatcher for builtins that need custom
///          procedural lowering. Builtins with straightforward declarative rules
///          (LEN, MID$, LEFT$, RIGHT$, INSTR, LTRIM$, RTRIM$, TRIM$, UCASE$,
///          LCASE$, CHR$, ASC) are handled by the generic rule-driven lowering
///          path in Common.cpp using specifications from builtin_registry.inc.
void registerStringBuiltins() {
    // STR$ requires type-based dispatch logic to select the right runtime helper
    register_builtin(getBuiltinInfo(Builtin::Str).name, &lowerStringBuiltin);
    // INKEY$/GETKEY$ are registered for string dispatch but fall through to generic
    register_builtin(getBuiltinInfo(Builtin::InKey).name, &lowerStringBuiltin);
    register_builtin(getBuiltinInfo(Builtin::GetKey).name, &lowerStringBuiltin);
}

} // namespace il::frontends::basic::lower::builtins
