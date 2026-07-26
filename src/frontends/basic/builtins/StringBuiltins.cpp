//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the lowering entry points for BASIC string builtins.  The helpers
// contained here translate AST builtin invocations into IL sequences, request
// runtime helpers, and normalise argument types so that string operations follow
// the language's historical semantics.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief BASIC string builtin lowering routines and registry utilities.
/// @details Builtins are stored in a compile-time table that records names,
///          arities, and lowering callbacks.  The lowering context class defined
///          in this file provides rich utilities for coercing arguments and
///          emitting runtime calls while keeping track of result types for later
///          stages of the lowering pipeline.

#include "frontends/basic/builtins/StringBuiltins.hpp"

#include "frontends/basic/TypeRules.hpp"
#include "zanna/il/Module.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <unordered_map>

namespace il::frontends::basic::builtins {
namespace {
using il::runtime::RuntimeFeature;

/// @brief Lower the LEN builtin to a runtime helper invocation.
///
/// @details LEN returns the length of its string argument.  Lowering simply
///          forwards the pre-lowered operand to `rt_len`, records the result
///          type as `I64`, and relies on the runtime to compute the length.
///          The arguments span is unused because the runtime always operates on
///          the first operand.
///
/// @param ctx Lowering context that tracks argument coercions and result type.
/// @param args Array of lowered argument values; unused but required by the
///        callback signature.
/// @return IL value representing the runtime call result.
Value lowerLen(LowerCtx &ctx, ArrayRef<Value> args) {
    (void)args;
    ctx.setResultType(Type(Type::Kind::I64));
    std::vector<Value> callArgs{ctx.argValue(0)};
    return ctx.emitCallRet(Type(Type::Kind::I64), "rt_str_len", callArgs, ctx.call().loc);
}

/// @brief Lower the MID$ builtin, handling both two- and three-argument forms.
///
/// @details MID$ extracts a substring.  The lowering routine normalises the
///          start index (BASIC uses 1-based positions) and selects the runtime
///          helper based on whether a length argument is present.  Any numeric
///          operands are coerced to I64, and the relevant runtime feature flag
///          is recorded so the runtime shim can be emitted.
///
/// @param ctx Lowering context describing the call site and argument helpers.
/// @param args Pre-lowered argument values (ignored because the context lazily
///        materialises them).
/// @return Runtime call result representing the substring.
Value lowerMid(LowerCtx &ctx, ArrayRef<Value> args) {
    (void)args;
    ctx.setResultType(Type(Type::Kind::Str));
    Value source = ctx.argValue(0);
    const bool hasLength = ctx.hasArg(2);
    const il::support::SourceLoc startLoc = ctx.argLoc(1);
    // BASIC MID$ uses one-based start positions and the runtime helpers
    // (rt_mid2/rt_mid3) already interpret the start argument as one-based.
    // Coerce to i64 but do NOT subtract 1 here; leave index normalization to
    // the runtime to avoid double-adjusting and triggering start==0 traps.
    ctx.ensureI64(1, startLoc);

    std::vector<Value> callArgs;
    callArgs.reserve(hasLength ? 3 : 2);
    callArgs.push_back(source);
    callArgs.push_back(ctx.argValue(1));

    const char *runtime = nullptr;
    il::support::SourceLoc callLoc = ctx.call().loc;
    if (hasLength) {
        const il::support::SourceLoc lengthLoc = ctx.argLoc(2);
        ctx.ensureI64(2, lengthLoc);
        callArgs.push_back(ctx.argValue(2));
        runtime = "rt_str_mid_len";
        callLoc = lengthLoc;
        ctx.requestHelper(RuntimeFeature::Mid3);
    } else {
        runtime = "rt_str_mid";
        ctx.requestHelper(RuntimeFeature::Mid2);
    }
    return ctx.emitCallRet(Type(Type::Kind::Str), runtime, callArgs, callLoc);
}

/// @brief Lower the LEFT$ builtin that extracts a prefix of a string.
///
/// @details The helper checks that the count argument is an integer, requests
///          the `Left` runtime helper, and emits the call returning a string
///          result.  Argument values are retrieved lazily from the context so
///          shared coercions remain centralised.
///
/// @param ctx Active lowering context.
/// @param args Ignored argument cache required by the callback signature.
/// @return Value produced by calling the runtime helper.
Value lowerLeft(LowerCtx &ctx, ArrayRef<Value> args) {
    (void)args;
    ctx.setResultType(Type(Type::Kind::Str));
    Value source = ctx.argValue(0);
    const il::support::SourceLoc countLoc = ctx.argLoc(1);
    ctx.ensureI64(1, countLoc);
    std::vector<Value> callArgs{source, ctx.argValue(1)};
    ctx.requestHelper(RuntimeFeature::Left);
    return ctx.emitCallRet(Type(Type::Kind::Str), "rt_str_left", callArgs, ctx.call().loc);
}

/// @brief Lower the RIGHT$ builtin that extracts a suffix of a string.
///
/// @details RIGHT$ mirrors LEFT$ but pulls characters from the end of the
///          input.  The lowering routine validates the count operand, records
///          the helper requirement, and emits `rt_right` with the coerced
///          arguments.
///
/// @param ctx Active lowering context.
/// @param args Unused placeholder complying with the callback signature.
/// @return Result of invoking the runtime helper.
Value lowerRight(LowerCtx &ctx, ArrayRef<Value> args) {
    (void)args;
    ctx.setResultType(Type(Type::Kind::Str));
    Value source = ctx.argValue(0);
    const il::support::SourceLoc countLoc = ctx.argLoc(1);
    ctx.ensureI64(1, countLoc);
    std::vector<Value> callArgs{source, ctx.argValue(1)};
    ctx.requestHelper(RuntimeFeature::Right);
    return ctx.emitCallRet(Type(Type::Kind::Str), "rt_str_right", callArgs, ctx.call().loc);
}

/// @brief Lower the STR$ builtin that formats numeric values to strings.
///
/// @details STR$ inspects the argument's numeric classification to choose the
///          appropriate runtime helper.  Integers may require narrowing, while
///          floating-point operands are coerced to F64.  The runtime feature
///          tracker records which allocator is required so the final binary can
///          import the correct helper.
///
/// @param ctx Context supplying argument coercions and helper tracking.
/// @param args Unused placeholder that satisfies the callback signature.
/// @return Value representing the string produced by the runtime helper.
Value lowerStr(LowerCtx &ctx, ArrayRef<Value> args) {
    (void)args;
    ctx.setResultType(Type(Type::Kind::Str));
    if (!ctx.hasArg(0))
        return Value::constInt(0);

    const il::support::SourceLoc argLoc = ctx.argLoc(0);
    TypeRules::NumericType numericType = TypeRules::NumericType::Double;
    if (ctx.call().args[0])
        numericType = ctx.classifyNumericType(*ctx.call().args[0]);

    const char *runtime = nullptr;
    RuntimeFeature feature = RuntimeFeature::StrFromDouble;

    /// @brief Narrows the first argument to a selected BASIC integer kind.
    /// @param target Destination integer kind.
    auto narrowInteger = [&](Type::Kind target) { ctx.narrowInt(0, Type(target), argLoc); };

    switch (numericType) {
        case TypeRules::NumericType::Integer:
            runtime = "rt_str_i16_alloc";
            feature = RuntimeFeature::StrFromI16;
            narrowInteger(Type::Kind::I16);
            break;
        case TypeRules::NumericType::Long:
            runtime = "rt_str_i32_alloc";
            feature = RuntimeFeature::StrFromI32;
            narrowInteger(Type::Kind::I32);
            break;
        case TypeRules::NumericType::Single:
            runtime = "rt_str_f_alloc";
            feature = RuntimeFeature::StrFromSingle;
            ctx.ensureF64(0, argLoc);
            break;
        case TypeRules::NumericType::Double:
        default:
            runtime = "rt_f64_to_str";
            feature = RuntimeFeature::F64ToStr;
            ctx.ensureF64(0, argLoc);
            break;
    }

    ctx.requestHelper(feature);
    std::vector<Value> callArgs{ctx.argValue(0)};
    return ctx.emitCallRet(Type(Type::Kind::Str), runtime, callArgs, ctx.call().loc);
}

/// @brief Lower the INSTR builtin that searches for a substring.
///
/// @details Depending on whether a starting offset is supplied, the lowering
///          routine calls `rt_instr2` or `rt_str_instr3`.  Offsets are converted
///          from BASIC's 1-based convention to the zero-based indices expected by
///          the runtime.  Helper feature flags are recorded to ensure the
///          runtime exports the required entry points.
///
/// @param ctx Active lowering context describing the call site.
/// @param args Ignored placeholder array.
/// @return Value storing the index returned by the runtime helper.
Value lowerInstr(LowerCtx &ctx, ArrayRef<Value> args) {
    (void)args;
    ctx.setResultType(Type(Type::Kind::I64));
    std::vector<Value> callArgs;
    const bool hasStart = ctx.hasArg(2);
    const char *runtime = nullptr;
    il::support::SourceLoc callLoc = ctx.call().loc;
    if (hasStart) {
        const il::support::SourceLoc startLoc = ctx.argLoc(0);
        ctx.ensureI64(0, startLoc);
        ctx.addConst(0, -1, startLoc);
        callArgs = {ctx.argValue(0), ctx.argValue(1), ctx.argValue(2)};
        runtime = "rt_str_instr3";
        callLoc = ctx.argLoc(2);
        ctx.requestHelper(RuntimeFeature::Instr3);
    } else {
        callArgs = {ctx.argValue(0), ctx.argValue(1)};
        runtime = "rt_str_index_of";
        callLoc = ctx.argLoc(1);
        ctx.requestHelper(RuntimeFeature::Instr2);
    }
    return ctx.emitCallRet(Type(Type::Kind::I64), runtime, callArgs, callLoc);
}

/// @brief Shared implementation for TRIM family builtins.
///
/// @details TRIM$, LTRIM$, RTRIM$, UCASE$, and LCASE$ all simply call a runtime
///          helper on a single string argument.  This helper emits the call,
///          tracks the relevant runtime feature, and records the string result
///          type while ignoring the unused argument cache.
///
/// @param ctx Lowering context used to fetch arguments and emit IL.
/// @param args Placeholder array required by the builtin callback type.
/// @param runtime Name of the runtime helper to invoke.
/// @param feature Runtime capability that must be tracked for the program.
/// @return Value produced by the selected runtime helper.
Value lowerTrim(LowerCtx &ctx, ArrayRef<Value> args, const char *runtime, RuntimeFeature feature) {
    (void)args;
    ctx.setResultType(Type(Type::Kind::Str));
    std::vector<Value> callArgs{ctx.argValue(0)};
    ctx.requestHelper(feature);
    return ctx.emitCallRet(Type(Type::Kind::Str), runtime, callArgs, ctx.call().loc);
}

/// @brief Lower the LTRIM$ builtin by delegating to the shared trim helper.
///
/// @param ctx Active lowering context.
/// @param args Placeholder argument cache; unused.
/// @return Runtime call result representing the trimmed string.
Value lowerLTrim(LowerCtx &ctx, ArrayRef<Value> args) {
    return lowerTrim(ctx, args, "rt_str_ltrim", RuntimeFeature::Ltrim);
}

/// @brief Lower the RTRIM$ builtin that trims trailing whitespace.
///
/// @param ctx Active lowering context.
/// @param args Placeholder argument array; unused.
/// @return String value returned by the runtime helper.
Value lowerRTrim(LowerCtx &ctx, ArrayRef<Value> args) {
    return lowerTrim(ctx, args, "rt_str_rtrim", RuntimeFeature::Rtrim);
}

/// @brief Lower the TRIM$ builtin that removes whitespace on both sides.
///
/// @param ctx Active lowering context.
/// @param args Placeholder argument list; unused.
/// @return Runtime-produced string result.
Value lowerTrimBoth(LowerCtx &ctx, ArrayRef<Value> args) {
    return lowerTrim(ctx, args, "rt_str_trim", RuntimeFeature::Trim);
}

/// @brief Lower the UCASE$ builtin that upper-cases a string argument.
///
/// @param ctx Active lowering context.
/// @param args Placeholder argument span; unused.
/// @return Upper-cased string emitted by the runtime helper.
Value lowerUcase(LowerCtx &ctx, ArrayRef<Value> args) {
    return lowerTrim(ctx, args, "rt_str_ucase", RuntimeFeature::Ucase);
}

/// @brief Lower the LCASE$ builtin that converts a string to lower case.
///
/// @param ctx Active lowering context.
/// @param args Placeholder argument array; unused.
/// @return Lower-cased string emitted by the runtime helper.
Value lowerLcase(LowerCtx &ctx, ArrayRef<Value> args) {
    return lowerTrim(ctx, args, "rt_str_lcase", RuntimeFeature::Lcase);
}

/// @brief Lower the CHR$ builtin that turns a character code into a string.
///
/// @details The helper ensures the argument is an integer, requests the
///          corresponding runtime helper, and emits the call returning a one-
///          character string.
///
/// @param ctx Active lowering context.
/// @param args Placeholder argument array; unused.
/// @return Resulting string value provided by the runtime helper.
Value lowerChr(LowerCtx &ctx, ArrayRef<Value> args) {
    (void)args;
    ctx.setResultType(Type(Type::Kind::Str));
    const il::support::SourceLoc loc = ctx.argLoc(0);
    ctx.ensureI64(0, loc);
    std::vector<Value> callArgs{ctx.argValue(0)};
    ctx.requestHelper(RuntimeFeature::Chr);
    return ctx.emitCallRet(Type(Type::Kind::Str), "rt_str_chr", callArgs, ctx.call().loc);
}

/// @brief Lower the ASC builtin that yields the code point of a string.
///
/// @details ASC returns an integer, so the helper forwards the argument to the
///          runtime while tracking the appropriate helper requirement.
///
/// @param ctx Active lowering context.
/// @param args Placeholder argument span; unused.
/// @return Integer value returned by the runtime helper.
Value lowerAsc(LowerCtx &ctx, ArrayRef<Value> args) {
    (void)args;
    ctx.setResultType(Type(Type::Kind::I64));
    std::vector<Value> callArgs{ctx.argValue(0)};
    ctx.requestHelper(RuntimeFeature::Asc);
    return ctx.emitCallRet(Type(Type::Kind::I64), "rt_str_asc", callArgs, ctx.call().loc);
}

// Note: not constexpr because std::string is not constexpr in MSVC's STL
const std::array<BuiltinSpec, 13> kStringBuiltins = {{{"LEN", 1, 1, &lowerLen},
                                                      {"MID$", 2, 3, &lowerMid},
                                                      {"LEFT$", 2, 2, &lowerLeft},
                                                      {"RIGHT$", 2, 2, &lowerRight},
                                                      {"STR$", 1, 1, &lowerStr},
                                                      {"INSTR", 2, 3, &lowerInstr},
                                                      {"LTRIM$", 1, 1, &lowerLTrim},
                                                      {"RTRIM$", 1, 1, &lowerRTrim},
                                                      {"TRIM$", 1, 1, &lowerTrimBoth},
                                                      {"UCASE$", 1, 1, &lowerUcase},
                                                      {"LCASE$", 1, 1, &lowerLcase},
                                                      {"CHR$", 1, 1, &lowerChr},
                                                      {"ASC", 1, 1, &lowerAsc}}};

} // namespace

/// @brief Look up a string builtin specification by name.
///
/// @details Uses a lazily-initialized hash map for O(1) average lookup.
///          The map is built once on first call from the kStringBuiltins table
///          and remains valid for the lifetime of the process.
///
/// @param name Builtin identifier as it appears in BASIC source.
/// @return Pointer to the builtin specification or `nullptr` if not found.
const BuiltinSpec *findBuiltin(StringRef name) {
    // Build the lookup map once on first call using a function-local static.
    // The map stores string_view -> pointer mappings from the static table.
    /// @brief Builds the process-lifetime string-builtin lookup map.
    /// @return Heap-owned map retained for process lifetime.
    static const auto &map = *[] {
        auto *m = new std::unordered_map<std::string_view, const BuiltinSpec *>();
        m->reserve(kStringBuiltins.size());
        for (const auto &spec : kStringBuiltins)
            m->emplace(std::string_view(spec.name), &spec);
        return m;
    }();
    auto it = map.find(name);
    return it != map.end() ? it->second : nullptr;
}

/// @brief Construct a lowering context bound to a builtin call.
///
/// @details Copies basic information about each argument slot (presence and
///          source location) while deferring expression lowering until the value
///          is explicitly requested.  This keeps coercions cheap for builtins
///          that only inspect a subset of their operands.
///
/// @param lowerer Owning lowering driver used to materialise IL.
/// @param call AST node describing the builtin invocation.
LowerCtx::LowerCtx(Lowerer &lowerer, const BuiltinCallExpr &call) : lowerer_(lowerer), call_(call) {
    const std::size_t count = call.args.size();
    loweredArgs_.resize(count);
    argValues_.assign(count, Value::constInt(0));
    argLocs_.resize(count);
    hasArg_.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto &expr = call.args[i];
        if (expr) {
            hasArg_[i] = true;
            argLocs_[i] = expr->loc;
        } else {
            hasArg_[i] = false;
            argLocs_[i] = call.loc;
        }
    }
}

/// @brief Retrieve the lowering driver powering this context.
/// @return Reference to the owning lowering driver.
Lowerer &LowerCtx::lowerer() const noexcept {
    return lowerer_;
}

/// @brief Access the builtin call node being processed.
/// @return Immutable reference to the builtin call expression.
const BuiltinCallExpr &LowerCtx::call() const noexcept {
    return call_;
}

/// @brief Compute the total number of argument slots available.
/// @return Number of argument positions recorded on the call.
std::size_t LowerCtx::argCount() const noexcept {
    return call_.args.size();
}

/// @brief Determine whether a particular argument slot was supplied in source.
///
/// @param idx Zero-based argument index.
/// @return True when the call expression contains an expression at @p idx.
bool LowerCtx::hasArg(std::size_t idx) const noexcept {
    return idx < hasArg_.size() && hasArg_[idx];
}

/// @brief Retrieve the best-effort source location for an argument slot.
///
/// @details If the argument was omitted (for optional parameters) the call site
///          location is returned instead so diagnostics still have a sensible
///          anchor.
///
/// @param idx Argument index to inspect.
/// @return Location of the argument expression or the call site fallback.
il::support::SourceLoc LowerCtx::argLoc(std::size_t idx) const noexcept {
    if (idx < argLocs_.size())
        return argLocs_[idx];
    return call_.loc;
}

/// @brief Access the lazily-lowered r-value for an argument.
///
/// @details Expressions are lowered on first access so unused optional
///          arguments do not incur any cost.  The lowered value is cached for
///          subsequent accesses.
///
/// @param idx Argument index to materialise.
/// @return Reference to the cached lowering result.
Lowerer::RVal &LowerCtx::arg(std::size_t idx) {
    return ensureLowered(idx);
}

/// @brief Fetch the raw IL value associated with an argument slot.
///
/// @details Forces the argument to be lowered and then exposes the cached
///          `Value` object, which callers often need when constructing runtime
///          call argument lists.
///
/// @param idx Argument index to inspect.
/// @return Mutable reference to the cached IL value.
Value &LowerCtx::argValue(std::size_t idx) {
    ensureLowered(idx);
    assert(idx < argValues_.size());
    return argValues_[idx];
}

/// @brief Provide a view over all materialised argument values.
///
/// @details The view reflects the internal cache and therefore updates as soon
///          as additional arguments are lowered.  Unused slots retain their
///          default-initialised placeholder until accessed.
///
/// @return Lightweight array reference covering the cached values.
ArrayRef<Value> LowerCtx::values() noexcept {
    return ArrayRef<Value>(argValues_.data(), argValues_.size());
}

/// @brief Record the result type that the handler will synthesize.
/// @param ty Result type chosen by the lowering routine.
void LowerCtx::setResultType(Type ty) noexcept {
    resultType_ = ty;
}

/// @brief Query the result type previously recorded by the handler.
/// @return Type reported by the lowering routine.
Type LowerCtx::resultType() const noexcept {
    return resultType_;
}

/// @brief Ensure that an argument is represented as a 64-bit integer.
///
/// @details Delegates to the owning `Lowerer` to perform any necessary
///          conversions and synchronises the cached `Value` so subsequent users
///          see the coerced operand.
///
/// @param idx Argument index to coerce.
/// @param loc Source location used for diagnostics should the coercion fail.
/// @return Reference to the coerced r-value slot.
Lowerer::RVal &LowerCtx::ensureI64(std::size_t idx, il::support::SourceLoc loc) {
    Lowerer::RVal &slot = ensureLowered(idx);
    Lowerer::RVal coerced = lowerer_.ensureI64(slot, loc);
    slot = coerced;
    syncValue(idx);
    return slot;
}

/// @brief Ensure that an argument is promoted to a 64-bit floating-point value.
///
/// @param idx Argument index to coerce.
/// @param loc Location used for diagnostics.
/// @return Reference to the coerced r-value slot.
Lowerer::RVal &LowerCtx::ensureF64(std::size_t idx, il::support::SourceLoc loc) {
    Lowerer::RVal &slot = ensureLowered(idx);
    Lowerer::RVal coerced = lowerer_.ensureF64(slot, loc);
    slot = coerced;
    syncValue(idx);
    return slot;
}

/// @brief Coerce an argument to a 64-bit integer without narrowing checks.
///
/// @details Unlike `ensureI64`, this helper allows the lowerer to perform
///          best-effort coercions (for example, from floating-point values)
///          without guaranteeing success.  The cached value is updated to match
///          whatever representation the lowerer produced.
///
/// @param idx Argument index to coerce.
/// @param loc Diagnostic location for conversion failures.
/// @return Reference to the coerced slot.
Lowerer::RVal &LowerCtx::coerceToI64(std::size_t idx, il::support::SourceLoc loc) {
    Lowerer::RVal &slot = ensureLowered(idx);
    Lowerer::RVal coerced = lowerer_.coerceToI64(slot, loc);
    slot = coerced;
    syncValue(idx);
    return slot;
}

/// @brief Coerce an argument to a 64-bit floating-point value.
///
/// @param idx Argument index being coerced.
/// @param loc Location used when reporting conversion diagnostics.
/// @return Reference to the coerced slot.
Lowerer::RVal &LowerCtx::coerceToF64(std::size_t idx, il::support::SourceLoc loc) {
    Lowerer::RVal &slot = ensureLowered(idx);
    Lowerer::RVal coerced = lowerer_.coerceToF64(slot, loc);
    slot = coerced;
    syncValue(idx);
    return slot;
}

/// @brief Add an integer immediate to an argument in place.
///
/// @details Used by builtins that adjust 1-based indices to zero-based offsets.
///          The helper rewrites the cached argument value so downstream code can
///          reuse it without recomputing the adjustment.
///
/// @param idx Argument slot to mutate.
/// @param immediate Constant delta to add.
/// @param loc Location attributed to the arithmetic for diagnostics.
/// @return Reference to the mutated slot.
Lowerer::RVal &LowerCtx::addConst(std::size_t idx,
                                  std::int64_t immediate,
                                  il::support::SourceLoc loc) {
    Lowerer::RVal &slot = ensureLowered(idx);
    slot.value = lowerer_.emitCommon(loc).add_checked(
        slot.value, Value::constInt(immediate), OverflowPolicy::Checked);
    slot.type = Type(Type::Kind::I64);
    syncValue(idx);
    return slot;
}

/// @brief Narrow an integer argument to a smaller integral type when safe.
///
/// @details Some runtime helpers expose specialised entry points for narrower
///          integer types.  This helper coerces the operand to I64 first, then
///          emits a checked narrowing cast to the requested type and updates the
///          cached slot accordingly.
///
/// @param idx Argument slot to narrow.
/// @param target Target integer type that the runtime expects.
/// @param loc Diagnostic location associated with the conversion.
/// @return Reference to the narrowed slot.
Lowerer::RVal &LowerCtx::narrowInt(std::size_t idx, Type target, il::support::SourceLoc loc) {
    Lowerer::RVal &slot = ensureLowered(idx);
    if (slot.type.kind != target.kind) {
        Lowerer::RVal coerced = lowerer_.coerceToI64(slot, loc);
        slot = coerced;
        int targetBits = 64;
        switch (target.kind) {
            case Type::Kind::I1:
                targetBits = 1;
                break;
            case Type::Kind::I16:
                targetBits = 16;
                break;
            case Type::Kind::I32:
                targetBits = 32;
                break;
            case Type::Kind::I64:
                targetBits = 64;
                break;
            default:
                targetBits = 64;
                break;
        }
        slot.value = lowerer_.emitCommon(loc).narrow_to(slot.value, 64, targetBits);
        slot.type = target;
        syncValue(idx);
    }
    return slot;
}

/// @brief Classify an expression according to BASIC's numeric type rules.
///
/// @param expr AST node to classify.
/// @return Inferred numeric category used when selecting runtime helpers.
TypeRules::NumericType LowerCtx::classifyNumericType(const Expr &expr) {
    return lowerer_.classifyNumericType(expr);
}

/// @brief Request that the program imports a specific runtime helper.
///
/// @param feature Runtime capability needed by the lowering routine.
void LowerCtx::requestHelper(RuntimeFeature feature) {
    lowerer_.requestHelper(feature);
}

/// @brief Record that a runtime helper was used so manifests remain accurate.
///
/// @param feature Runtime capability that was consumed.
void LowerCtx::trackRuntime(RuntimeFeature feature) {
    lowerer_.trackRuntime(feature);
}

/// @brief Emit a runtime call returning a value of the specified type.
///
/// @details Updates the lowerer's current location so diagnostics on the emitted
///          instructions are attributed to the runtime helper call.
///
/// @param ty Result type reported by the runtime helper.
/// @param runtime Name of the runtime function to invoke.
/// @param args IL values forming the argument list.
/// @param loc Source location associated with the call site.
/// @return Value returned by the runtime helper.
Value LowerCtx::emitCallRet(Type ty,
                            const char *runtime,
                            const std::vector<Value> &args,
                            il::support::SourceLoc loc) {
    lowerer_.curLoc = loc;
    return lowerer_.emitCallRet(ty, runtime, args);
}

/// @brief Materialise the lowering result for an argument slot on demand.
///
/// @details Expressions are lowered lazily so unused optional operands never hit
///          the lowering pipeline.  When an argument is missing, a default zero
///          literal is synthesised to keep downstream code simple.
///
/// @param idx Argument index to materialise.
/// @return Reference to the cached lowering result for the argument.
Lowerer::RVal &LowerCtx::ensureLowered(std::size_t idx) {
    assert(idx < loweredArgs_.size());
    auto &slot = loweredArgs_[idx];
    if (!slot) {
        if (hasArg(idx)) {
            const auto &expr = call_.args[idx];
            assert(expr && "expected expression for present argument");
            slot = lowerer_.lowerExpr(*expr);
        } else {
            slot = Lowerer::RVal{Value::constInt(0), Type(Type::Kind::I64)};
        }
        argValues_[idx] = slot->value;
    }
    return *slot;
}

/// @brief Synchronise the cached `Value` with the most recent lowered result.
///
/// @param idx Argument index whose cached value should be refreshed.
void LowerCtx::syncValue(std::size_t idx) noexcept {
    assert(idx < argValues_.size());
    if (idx < loweredArgs_.size() && loweredArgs_[idx])
        argValues_[idx] = loweredArgs_[idx]->value;
}

} // namespace il::frontends::basic::builtins
