//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/builtins/Common.cpp
// Purpose: Implements the shared builtin lowering context, coercion matrix,
//          variant selection, feature application, and guarded conversion CFGs.
// Key invariants:
//   - Registry variants are selected in declaration order.
//   - Arguments are lowered lazily and cached once per call.
//   - Coercion failures emit diagnostics and deterministic typed fallbacks.
// Ownership/Lifetime:
//   - BuiltinLowerContext borrows the Lowerer, AST call, and registry entries.
//   - Synthetic arguments are owned by the context for the duration of lowering.
// Links: src/frontends/basic/lower/BuiltinCommon.hpp,
//        src/frontends/basic/BuiltinRegistry.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements registry-driven BASIC builtin lowering infrastructure.

#include "frontends/basic/lower/BuiltinCommon.hpp"

#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/lower/Emitter.hpp"

#include "zanna/il/Module.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace il::frontends::basic::lower {
namespace {
using IlType = il::core::Type;
using IlKind = IlType::Kind;
using Value = il::core::Value;
using Opcode = il::core::Opcode;
using Variant = BuiltinLoweringRule::Variant;
using Transform = BuiltinLoweringRule::ArgTransform;
using TransformKind = BuiltinLoweringRule::ArgTransform::Kind;
using Feature = BuiltinLoweringRule::Feature;
using FeatureAction = BuiltinLoweringRule::Feature::Action;
using TypeKind = IlKind;
using Emitter = lower::Emitter;

constexpr const char *kDiagBuiltinCoerceFailed = "B4005";

enum class CoerceRule : std::uint8_t {
    /// Source and destination representations already agree.
    Exact,
    /// Widen or sign-extend an integral value.
    PromoteInt,
    /// Convert an integral or narrower numeric value to floating point.
    PromoteFloat,
    /// Convert a numeric value through a runtime string formatter.
    ToString,
    /// Reject the source/destination combination.
    Forbid,
};

constexpr std::array<TypeKind, 6> kTypeKinds = {
    TypeKind::I1, TypeKind::I16, TypeKind::I32, TypeKind::I64, TypeKind::F64, TypeKind::Str};

constexpr std::size_t kTypeCount = kTypeKinds.size();

constexpr int typeIndex(TypeKind kind) noexcept {
    for (int i = 0; i < static_cast<int>(kTypeCount); ++i) {
        if (kTypeKinds[static_cast<std::size_t>(i)] == kind)
            return i;
    }
    return -1;
}

constexpr bool isIntegral(TypeKind kind) noexcept {
    switch (kind) {
        case TypeKind::I1:
        case TypeKind::I16:
        case TypeKind::I32:
        case TypeKind::I64:
            return true;
        default:
            return false;
    }
}

constexpr int bitWidth(TypeKind kind) noexcept {
    switch (kind) {
        case TypeKind::I1:
            return 1;
        case TypeKind::I16:
            return 16;
        case TypeKind::I32:
            return 32;
        case TypeKind::I64:
            return 64;
        default:
            return 0;
    }
}

constexpr CoerceRule F = CoerceRule::Forbid;
constexpr CoerceRule E = CoerceRule::Exact;
constexpr CoerceRule PI = CoerceRule::PromoteInt;
constexpr CoerceRule PF = CoerceRule::PromoteFloat;
constexpr CoerceRule TS = CoerceRule::ToString;

constexpr std::array<std::array<CoerceRule, kTypeCount>, kTypeCount> kCoerce = {{
    // From I1
    {{E, PI, PI, PI, PF, TS}},
    // From I16
    {{PI, E, PI, PI, PF, TS}},
    // From I32 (narrowing to I16 is forbidden to mirror legacy behaviour)
    {{PI, F, E, PI, PF, TS}},
    // From I64
    {{PI, PI, PI, E, PF, TS}},
    // From F64 (rounded-to-even when targeting integers)
    {{PF, PF, PF, PF, E, TS}},
    // From Str
    {{F, F, F, F, F, E}},
}};

static const char *ruleName(CoerceRule rule) noexcept {
    switch (rule) {
        case CoerceRule::Exact:
            return "Exact";
        case CoerceRule::PromoteInt:
            return "PromoteInt";
        case CoerceRule::PromoteFloat:
            return "PromoteFloat";
        case CoerceRule::ToString:
            return "ToString";
        case CoerceRule::Forbid:
            return "Forbid";
    }
    return "Unknown";
}

static bool canCoerce(TypeKind from, TypeKind to) noexcept {
    const int fromIdx = typeIndex(from);
    const int toIdx = typeIndex(to);
    if (fromIdx < 0 || toIdx < 0)
        return false;
    return kCoerce[static_cast<std::size_t>(fromIdx)][static_cast<std::size_t>(toIdx)] !=
           CoerceRule::Forbid;
}

static thread_local TypeKind gActiveCoerceFrom = TypeKind::I64;

struct CoerceScope {
    TypeKind prev;

    explicit CoerceScope(TypeKind from) noexcept : prev(gActiveCoerceFrom) {
        gActiveCoerceFrom = from;
    }

    ~CoerceScope() {
        gActiveCoerceFrom = prev;
    }
};

static Value narrowFromI64(Value value, TypeKind to, Emitter &emit) {
    const int targetBits = bitWidth(to);
    if (targetBits <= 0 || targetBits == 64)
        return value;
    if (targetBits == 1)
        return emit.emitUnary(Opcode::Trunc1, IlType(TypeKind::I1), value);
    return emit.emitUnary(Opcode::CastSiNarrowChk, IlType(to), value);
}

static Value signExtendToI64(Value value, TypeKind from, Emitter &emit) {
    const int fromBits = bitWidth(from);
    if (fromBits <= 0 || fromBits == 64)
        return value;
    if (fromBits == 1)
        return emit.emitUnary(Opcode::Zext1, IlType(TypeKind::I64), value);

    const std::int64_t mask = (fromBits == 16) ? 0xFFFFll : 0xFFFFFFFFll;
    Value masked =
        emit.emitBinary(Opcode::And, IlType(TypeKind::I64), value, Value::constInt(mask));
    const int shift = (fromBits == 32) ? 32 : 48;
    Value shl = emit.emitBinary(Opcode::Shl, IlType(TypeKind::I64), masked, Value::constInt(shift));
    return emit.emitBinary(Opcode::AShr, IlType(TypeKind::I64), shl, Value::constInt(shift));
}

static Value applyCoerceRule(CoerceRule rule, const Value &v, TypeKind to, Emitter &emit) {
    const TypeKind from = gActiveCoerceFrom;
    switch (rule) {
        case CoerceRule::Exact:
            return v;

        case CoerceRule::PromoteInt: {
            Value widened = isIntegral(from) ? signExtendToI64(v, from, emit) : v;
            return narrowFromI64(widened, to, emit);
        }

        case CoerceRule::PromoteFloat: {
            if (to == TypeKind::F64) {
                Value widened = isIntegral(from) ? signExtendToI64(v, from, emit) : v;
                return emit.emitUnary(Opcode::CastSiToFp, IlType(TypeKind::F64), widened);
            }
            Value asInt = emit.emitUnary(Opcode::CastFpToSiRteChk, IlType(TypeKind::I64), v);
            return narrowFromI64(asInt, to, emit);
        }

        case CoerceRule::ToString:
            return v;

        case CoerceRule::Forbid:
        default:
            return v;
    }
}

static void emitCoerceDiagnostic(
    Lowerer &lowerer, il::support::SourceLoc loc, TypeKind from, TypeKind to, CoerceRule rule) {
    if (auto *diag = lowerer.diagnosticEmitter()) {
        std::string message = "failed to coerce builtin argument from ";
        message += il::core::kindToString(from);
        message += " to ";
        message += il::core::kindToString(to);
        message += " using rule ";
        message += ruleName(rule);
        diag->emit(
            il::support::Severity::Error, kDiagBuiltinCoerceFailed, loc, 0, std::move(message));
    }
}

static IlType typeForKind(BuiltinLowerContext &ctx, TypeKind kind) {
    if (kind == TypeKind::I1)
        return ctx.boolType();
    return IlType(kind);
}

static Lowerer::RVal makeTypedZero(IlType type) {
    switch (type.kind) {
        case IlKind::F64:
            return {Value::constFloat(0.0), type};
        case IlKind::Str:
            return {Value::constStr(""), type};
        case IlKind::I1:
            return {Value::constBool(false), type};
        case IlKind::Ptr:
            return {Value::null(), type};
        case IlKind::I16:
        case IlKind::I32:
        case IlKind::I64:
        default:
            return {Value::constInt(0), type};
    }
}

static bool applyBuiltinCoercion(BuiltinLowerContext &ctx,
                                 Lowerer::RVal &slot,
                                 TypeKind to,
                                 il::support::SourceLoc loc) {
    const TypeKind from = slot.type.kind;
    if (from == to) {
        slot.type = typeForKind(ctx, to);
        return true;
    }

    if (!canCoerce(from, to)) {
        emitCoerceDiagnostic(ctx.lowerer(), loc, from, to, CoerceRule::Forbid);
        return false;
    }

    const int fromIdx = typeIndex(from);
    const int toIdx = typeIndex(to);
    const CoerceRule rule =
        kCoerce[static_cast<std::size_t>(fromIdx)][static_cast<std::size_t>(toIdx)];

    CoerceScope scope(from);
    ctx.setCurrentLoc(loc);
    Emitter emitter(ctx.lowerer());
    slot.value = applyCoerceRule(rule, slot.value, to, emitter);
    slot.type = typeForKind(ctx, to);
    return true;
}

Lowerer::RVal emitCallRuntime(BuiltinLowerContext &ctx, const Variant &variant);
Lowerer::RVal emitUnary(BuiltinLowerContext &ctx, const Variant &variant);
Lowerer::RVal emitCustom(BuiltinLowerContext &ctx, const Variant &variant);

} // namespace

/// @brief Construct a lowering context for the given builtin call.
/// @details Captures references to the active @ref Lowerer, the builtin rule,
///          and the builtin metadata. The constructor eagerly scans each
///          argument to record source locations and static types so variant
///          selection and diagnostic emission can access them without re-lowering.
/// @param lowerer Active lowering engine responsible for emitting IL.
/// @param call Builtin call expression currently being lowered.
BuiltinLowerContext::BuiltinLowerContext(Lowerer &lowerer, const BuiltinCallExpr &call)
    : lowerer_(&lowerer), call_(&call), rule_(&getBuiltinLoweringRule(call.builtin)),
      info_(&getBuiltinInfo(call.builtin)), originalTypes_(call.args.size()),
      argLocs_(call.args.size()), loweredArgs_(call.args.size()), lowering_(lowerer) {
    for (std::size_t i = 0; i < call.args.size(); ++i) {
        const auto &arg = call.args[i];
        if (!arg)
            continue;
        argLocs_[i] = arg->loc;
        originalTypes_[i] = lowerer.scanExpr(*arg);
    }
}

/// @brief Check whether the builtin call provides an argument at @p idx.
/// @details Evaluates the call's argument vector and ensures the pointer at the
///          requested index is non-null. The function never triggers lowering of
///          the argument, making it safe for speculative checks.
/// @param idx Zero-based argument index to query.
/// @return True when an argument exists and is non-null.
bool BuiltinLowerContext::hasArg(std::size_t idx) const noexcept {
    return idx < call_->args.size() && call_->args[idx] != nullptr;
}

/// @brief Retrieve the statically scanned type for an argument.
/// @details Returns the type recorded during construction by
///          @ref Lowerer::scanExpr. If the index is out of range or the type was
///          unavailable, std::nullopt is returned so callers can fall back to
///          default behaviour.
/// @param idx Argument index whose type should be queried.
/// @return Optional expression type reported by the scanner.
std::optional<Lowerer::ExprType> BuiltinLowerContext::originalType(std::size_t idx) const noexcept {
    if (idx >= originalTypes_.size())
        return std::nullopt;
    return originalTypes_[idx];
}

/// @brief Fetch the source location associated with an argument index.
/// @details Returns the argument's location when present, otherwise falls back
///          to the call site location. The helper keeps diagnostic emission
///          consistent even for synthesized arguments.
/// @param idx Argument index whose source location is requested.
/// @return Source location for diagnostics.
il::support::SourceLoc BuiltinLowerContext::argLoc(std::size_t idx) const noexcept {
    if (idx < argLocs_.size() && argLocs_[idx])
        return *argLocs_[idx];
    return call_->loc;
}

/// @brief Resolve the source location used for runtime calls emitted by a variant.
/// @details Some variants attribute diagnostics to a specific argument. When
///          @p idx is provided and references a valid argument location, that
///          location is returned; otherwise the call site location is used.
/// @param idx Optional argument index provided by the lowering rule.
/// @return Source location that should own diagnostics emitted by the variant.
il::support::SourceLoc BuiltinLowerContext::callLoc(
    const std::optional<std::size_t> &idx) const noexcept {
    if (idx && *idx < argLocs_.size() && argLocs_[*idx])
        return *argLocs_[*idx];
    return call_->loc;
}

/// @brief Ensure the argument at @p idx has been lowered.
/// @details On first use the argument is lowered and cached so subsequent calls
///          reuse the same IL value and type. The helper asserts the argument is
///          present to catch rule mismatches early.
/// @param idx Argument index to lower.
/// @return Reference to the cached lowered argument.
Lowerer::RVal &BuiltinLowerContext::ensureLowered(std::size_t idx) {
    assert(hasArg(idx) && "builtin lowering referenced missing argument");
    auto &slot = loweredArgs_[idx];
    if (!slot)
        slot = lowerer_->lowerExpr(*call_->args[idx]);
    return *slot;
}

/// @brief Append a synthetic argument produced during lowering.
/// @details Stores @p value in an internal vector to extend the lifetime of
///          temporary results that mimic call arguments. Returns a reference so
///          callers can mutate the stored value if needed.
/// @param value Lowered value/type pair to append.
/// @return Reference to the stored synthetic argument.
Lowerer::RVal &BuiltinLowerContext::appendSynthetic(Lowerer::RVal value) {
    syntheticArgs_.push_back(std::move(value));
    return syntheticArgs_.back();
}

/// @brief Obtain the lowered value for an argument defined by @p spec.
/// @details If the argument exists it is lowered (or retrieved from cache).
///          Otherwise a default value is synthesized when permitted by the rule;
///          failing that, a defensive zero is returned after triggering an
///          assertion in debug builds.
/// @param spec Argument specification from the lowering rule.
/// @return Reference to the lowered or synthesized argument slot.
Lowerer::RVal &BuiltinLowerContext::ensureArgument(const BuiltinLoweringRule::Argument &spec) {
    const std::size_t idx = spec.index;
    if (hasArg(idx))
        return ensureLowered(idx);
    if (spec.defaultValue) {
        const auto &def = *spec.defaultValue;
        Lowerer::RVal value{Value::constInt(def.i64), IlType(IlKind::I64)};
        switch (def.type) {
            case Lowerer::ExprType::F64:
                value = {Value::constFloat(def.f64), IlType(IlKind::F64)};
                break;
            case Lowerer::ExprType::Str:
                value = {lowerer_->emitConstStr(lowerer_->getStringLabel(std::string(def.str))),
                         IlType(IlKind::Str)};
                break;
            case Lowerer::ExprType::Bool:
                value = {lowerer_->emitBoolConst(def.i64 != 0), lowerer_->ilBoolTy()};
                break;
            case Lowerer::ExprType::Obj:
                value = {Value::null(), IlType(IlKind::Ptr)};
                break;
            case Lowerer::ExprType::I64:
            default:
                value = {Value::constInt(def.i64), IlType(IlKind::I64)};
                break;
        }
        return appendSynthetic(std::move(value));
    }
    if (auto *diag = lowerer_->diagnosticEmitter()) {
        diag->emit(il::support::Severity::Error,
                   "B9004",
                   call_->loc,
                   1,
                   "builtin lowering rule referenced a missing argument without a default");
    }
    return appendSynthetic(makeTypedZero(IlType(IlKind::I64)));
}

/// @brief Resolve the diagnostic location for an argument described by @p spec.
/// @details Prefers the argument's own source location when available and falls
///          back to the call location. This ensures transforms report errors at
///          intuitive positions.
/// @param spec Argument description from the lowering rule.
/// @return Source location for diagnostics.
il::support::SourceLoc BuiltinLowerContext::selectArgLoc(
    const BuiltinLoweringRule::Argument &spec) const {
    if (spec.index < argLocs_.size() && argLocs_[spec.index])
        return *argLocs_[spec.index];
    return call_->loc;
}

/// @brief Apply a sequence of transformations to an argument.
/// @details Ensures the argument exists, then iterates @p transforms to coerce
///          or adjust the argument value and type. Each transform leverages
///          helper routines on the underlying @ref Lowerer to remain consistent
///          with normal expression lowering.
/// @param spec Argument specification describing the source operand.
/// @param transforms Ordered list of transformations to apply.
/// @return Reference to the transformed argument slot.
Lowerer::RVal &BuiltinLowerContext::applyTransforms(
    const BuiltinLoweringRule::Argument &spec,
    const std::vector<BuiltinLoweringRule::ArgTransform> &transforms) {
    Lowerer::RVal &slot = ensureArgument(spec);
    il::support::SourceLoc loc = selectArgLoc(spec);
    for (const auto &transform : transforms) {
        switch (transform.kind) {
            case TransformKind::EnsureI64:
                if (!applyBuiltinCoercion(*this, slot, IlKind::I64, loc))
                    return slot;
                break;
            case TransformKind::EnsureF64:
                if (!applyBuiltinCoercion(*this, slot, IlKind::F64, loc))
                    return slot;
                break;
            case TransformKind::EnsureI32:
                if (!applyBuiltinCoercion(*this, slot, IlKind::I32, loc))
                    return slot;
                break;
            case TransformKind::CoerceI64:
                if (!applyBuiltinCoercion(*this, slot, IlKind::I64, loc))
                    return slot;
                break;
            case TransformKind::CoerceF64:
                if (!applyBuiltinCoercion(*this, slot, IlKind::F64, loc))
                    return slot;
                break;
            case TransformKind::CoerceBool:
                if (!applyBuiltinCoercion(*this, slot, IlKind::I1, loc))
                    return slot;
                break;
            case TransformKind::AddConst:
                slot.value = lowerer_->emitCommon(loc).add_checked(
                    slot.value, Value::constInt(transform.immediate), OverflowPolicy::Checked);
                slot.type = IlType(IlKind::I64);
                break;
        }
    }
    return slot;
}

/// @brief Translate a BASIC expression type into the corresponding IL type.
/// @details Covers the subset of expression kinds used by builtin lowering and
///          uses @p lowerer to access shared boolean type handles. Defaults to
///          64-bit integers when no specialised mapping exists.
/// @param lowerer Lowerer used to access cached IL types.
/// @param type BASIC expression type classification.
/// @return Equivalent IL type for code generation.
IlType BuiltinLowerContext::typeFromExpr(Lowerer &lowerer, Lowerer::ExprType type) {
    switch (type) {
        case Lowerer::ExprType::F64:
            return IlType(IlKind::F64);
        case Lowerer::ExprType::Str:
            return IlType(IlKind::Str);
        case Lowerer::ExprType::Bool:
            return lowerer.ilBoolTy();
        case Lowerer::ExprType::Obj:
            return IlType(IlKind::Ptr);
        case Lowerer::ExprType::I64:
        default:
            return IlType(IlKind::I64);
    }
}

/// @brief Determine the IL result type described by @p spec.
/// @details Evaluates whether the rule requests a fixed type or wants to mirror
///          the type of a specific argument. When the referenced argument is
///          absent the method gracefully falls back to the fixed type.
/// @param spec Result specification from the lowering rule.
/// @return IL type that should be used for the builtin result.
IlType BuiltinLowerContext::resolveResultType(const BuiltinLoweringRule::ResultSpec &spec) {
    switch (spec.kind) {
        case BuiltinLoweringRule::ResultSpec::Kind::Fixed:
            return typeFromExpr(*lowerer_, spec.type);
        case BuiltinLoweringRule::ResultSpec::Kind::FromArg:
            if (hasArg(spec.argIndex))
                return ensureLowered(spec.argIndex).type;
            return typeFromExpr(*lowerer_, spec.type);
    }
    return IlType(IlKind::I64);
}

/// @brief Resolve the IL result type for the active variant.
/// @details Convenience wrapper that delegates to the rule's stored result
///          specification.
/// @return IL type for the builtin result.
IlType BuiltinLowerContext::resolveResultType() {
    return resolveResultType(rule_->result);
}

IlType BuiltinLowerContext::fallbackResultType() const {
    if (!rule_)
        return IlType(IlKind::I64);

    switch (rule_->result.kind) {
        case BuiltinLoweringRule::ResultSpec::Kind::Fixed:
            return typeFromExpr(*lowerer_, rule_->result.type);
        case BuiltinLoweringRule::ResultSpec::Kind::FromArg:
            if (rule_->result.argIndex < originalTypes_.size() &&
                originalTypes_[rule_->result.argIndex]) {
                return typeFromExpr(*lowerer_, *originalTypes_[rule_->result.argIndex]);
            }
            return typeFromExpr(*lowerer_, rule_->result.type);
    }
    return IlType(IlKind::I64);
}

/// @brief Create a default zero-valued result.
/// @details Used when lowering fails or when a variant is missing so downstream
///          lowering can continue with a type-correct placeholder.
/// @return Pair containing a zero/null/empty constant matching the builtin result type.
Lowerer::RVal BuiltinLowerContext::makeZeroResult() const {
    return makeTypedZero(fallbackResultType());
}

/// @brief Choose the lowering variant that matches the current call shape.
/// @details Iterates the rule's variants and evaluates each condition against
///          the recorded arguments and types. The first matching variant is
///          selected, defaulting to the first entry when none match.
/// @return Pointer to the selected variant or nullptr if none exist.
const BuiltinLoweringRule::Variant *BuiltinLowerContext::selectVariant() const {
    const auto &variants = rule_->variants;
    const Variant *selected = nullptr;
    for (const auto &candidate : variants) {
        bool matches = false;
        switch (candidate.condition) {
            case Variant::Condition::Always:
                matches = true;
                break;
            case Variant::Condition::IfArgPresent:
                matches = hasArg(candidate.conditionArg);
                break;
            case Variant::Condition::IfArgMissing:
                matches = !hasArg(candidate.conditionArg);
                break;
            case Variant::Condition::IfArgTypeIs:
                if (hasArg(candidate.conditionArg) && originalType(candidate.conditionArg))
                    matches = *originalType(candidate.conditionArg) == candidate.conditionType;
                break;
            case Variant::Condition::IfArgTypeIsNot:
                if (hasArg(candidate.conditionArg) && originalType(candidate.conditionArg))
                    matches = *originalType(candidate.conditionArg) != candidate.conditionType;
                break;
        }
        if (matches) {
            selected = &candidate;
            break;
        }
    }

    if (!selected && !variants.empty())
        selected = &variants.front();
    return selected;
}

/// @brief Apply feature requests declared by a variant.
/// @details Invokes @ref Lowerer::requestHelper or @ref Lowerer::trackRuntime
///          based on the feature action so runtime support code is emitted when
///          necessary.
/// @param variant Variant whose features should be applied.
void BuiltinLowerContext::applyFeatures(const BuiltinLoweringRule::Variant &variant) {
    for (const auto &feature : variant.features) {
        switch (feature.action) {
            case FeatureAction::Request:
                lowerer_->requestHelper(feature.feature);
                break;
            case FeatureAction::Track:
                lowerer_->trackRuntime(feature.feature);
                break;
        }
    }
}

/// @brief Update the lowering context's current source location.
/// @details Forwards the location to the underlying lowerer so subsequent IL
///          instructions inherit accurate diagnostic information.
/// @param loc Source location to apply.
void BuiltinLowerContext::setCurrentLoc(il::support::SourceLoc loc) {
    lowerer_->curLoc = loc;
}

/// @brief Retrieve the canonical IL boolean type.
/// @details Forwards to @ref Lowerer::ilBoolTy so all boolean operations share
///          the same type handle.
/// @return Boolean IL type.
il::core::Type BuiltinLowerContext::boolType() const {
    return lowering_.ilBoolTy();
}

/// @brief Emit a runtime call returning @p type.
/// @details Delegates to @ref Lowerer::emitCallRet, centralising all runtime
///          invocations through the lowering context for easier testing.
/// @param type Expected IL return type.
/// @param runtime Name of the runtime helper to call.
/// @param args Argument values to pass to the runtime function.
/// @return IL value produced by the runtime call.
il::core::Value BuiltinLowerContext::emitCall(il::core::Type type,
                                              const char *runtime,
                                              const std::vector<il::core::Value> &args) {
    // Use lowerer_->emitCallRet to ensure runtime tracking happens
    return lowerer_->emitCallRet(type, runtime, args);
}

/// @brief Emit a unary IL instruction.
/// @details Convenience wrapper around @ref Lowerer::emitUnary so builtin
///          lowering maintains consistent instruction construction.
/// @param opcode Opcode to emit.
/// @param type Result type of the instruction.
/// @param value Operand value.
/// @return Resulting IL value.
il::core::Value BuiltinLowerContext::emitUnary(il::core::Opcode opcode,
                                               il::core::Type type,
                                               il::core::Value value) {
    return lowering_.emitUnary(opcode, type, value);
}

/// @brief Emit a binary IL instruction.
/// @details Wraps @ref Lowerer::emitBinary to retain a uniform entry point for
///          builtin lowering.
/// @param opcode Opcode to emit.
/// @param type Result type of the instruction.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Resulting IL value.
il::core::Value BuiltinLowerContext::emitBinary(il::core::Opcode opcode,
                                                il::core::Type type,
                                                il::core::Value lhs,
                                                il::core::Value rhs) {
    return lowering_.emitBinary(opcode, type, lhs, rhs);
}

/// @brief Emit a load from the given address.
/// @details Delegates to @ref Lowerer::emitLoad.
/// @param type Type of the value being loaded.
/// @param addr Address to load from.
/// @return Loaded IL value.
il::core::Value BuiltinLowerContext::emitLoad(il::core::Type type, il::core::Value addr) {
    return lowering_.emitLoad(type, addr);
}

/// @brief Allocate stack storage via the lowerer.
/// @details Used by builtin lowering to create temporary slots for runtime
///          helpers that return results via out-parameters.
/// @param bytes Number of bytes to allocate.
/// @return IL value representing the address of the allocation.
il::core::Value BuiltinLowerContext::emitAlloca(int bytes) {
    return lowering_.emitAlloca(bytes);
}

/// @brief Emit a conditional branch between two blocks.
/// @details Forwards to @ref Lowerer::emitCBr so builtin lowering integrates
///          with the procedure context's current block.
/// @param cond Branch condition.
/// @param t Target when @p cond is true.
/// @param f Target when @p cond is false.
void BuiltinLowerContext::emitCBr(il::core::Value cond,
                                  il::core::BasicBlock *t,
                                  il::core::BasicBlock *f) {
    lowering_.emitCBr(cond, t, f);
}

/// @brief Emit a trap instruction signalling an unrecoverable error.
/// @details Defers to the lowerer so diagnostics inherit the current location.
void BuiltinLowerContext::emitTrap() {
    lowerer_->emitTrap();
}

/// @brief Set the procedure context's current basic block.
/// @details Allows builtin lowering helpers to continue emitting instructions in
///          newly created blocks after control flow splits.
/// @param block Block that should become current.
void BuiltinLowerContext::setCurrentBlock(il::core::BasicBlock *block) {
    lowerer_->context().setCurrent(block);
}

/// @brief Construct a block label using either the block namer or mangler.
/// @details Prefers the structured naming strategy provided by
///          @ref Lowerer::BlockNamer, falling back to the mangler when the namer
///          is unavailable.
/// @param hint Semantic hint that seeds the label.
/// @return Fresh block label string.
std::string BuiltinLowerContext::makeBlockLabel(const char *hint) {
    return lowering_.makeBlockLabel(hint);
}

/// @brief Create continuation and trap blocks for guard checks.
/// @details Appends new blocks to the active function, locates them by label,
///          and returns pointers so callers can wire up control flow. The
///          origin block is restored so the caller can emit the conditional
///          branch immediately after creation.
/// @param contHint Label hint for the success block.
/// @param trapHint Label hint for the failure block.
/// @return Pair of basic blocks representing success and failure continuations.
BuiltinLowerContext::BranchPair BuiltinLowerContext::createGuardBlocks(const char *contHint,
                                                                       const char *trapHint) {
    BranchPair pair{};
    Lowerer::ProcedureContext &procCtx = lowerer_->context();
    il::core::Function *func = procCtx.function();
    il::core::BasicBlock *origin = procCtx.current();
    if (!func || !origin)
        return pair;

    const std::string originLabel = origin->label;
    const std::string contLabel = makeBlockLabel(contHint);
    const std::string trapLabel = makeBlockLabel(trapHint);

    lowerer_->builder->addBlock(*func, contLabel);
    lowerer_->builder->addBlock(*func, trapLabel);

    const auto findBlock = [&](const std::string &label) {
        auto it = std::find_if(func->blocks.begin(),
                               func->blocks.end(),
                               [&](const il::core::BasicBlock &bb) { return bb.label == label; });
        assert(it != func->blocks.end());
        return &*it;
    };

    auto originIt =
        std::find_if(func->blocks.begin(), func->blocks.end(), [&](const il::core::BasicBlock &bb) {
            return bb.label == originLabel;
        });
    assert(originIt != func->blocks.end());
    procCtx.setCurrent(&*originIt);

    pair.cont = findBlock(contLabel);
    pair.trap = findBlock(trapLabel);
    return pair;
}

/// @brief Create the block structure used by the VAL builtin lowering.
/// @details Adds four blocks (continue, trap, NaN, overflow) and resolves their
///          pointers so lowering logic can emit structured control flow around
///          conversion traps.
/// @return Structure containing block pointers for the VAL builtin guards.
BuiltinLowerContext::ValBlocks BuiltinLowerContext::createValBlocks() {
    ValBlocks blocks{};
    Lowerer::ProcedureContext &procCtx = lowerer_->context();
    il::core::Function *func = procCtx.function();
    il::core::BasicBlock *origin = procCtx.current();
    if (!func || !origin)
        return blocks;

    const std::string originLabel = origin->label;
    const std::string contLabel = makeBlockLabel("val_ok");
    const std::string trapLabel = makeBlockLabel("val_fail");
    const std::string nanLabel = makeBlockLabel("val_nan");
    const std::string overflowLabel = makeBlockLabel("val_over");

    lowerer_->builder->addBlock(*func, contLabel);
    lowerer_->builder->addBlock(*func, trapLabel);
    lowerer_->builder->addBlock(*func, nanLabel);
    lowerer_->builder->addBlock(*func, overflowLabel);

    const auto findBlock = [&](const std::string &label) {
        auto it = std::find_if(func->blocks.begin(),
                               func->blocks.end(),
                               [&](const il::core::BasicBlock &bb) { return bb.label == label; });
        assert(it != func->blocks.end());
        return &*it;
    };

    auto originIt =
        std::find_if(func->blocks.begin(), func->blocks.end(), [&](const il::core::BasicBlock &bb) {
            return bb.label == originLabel;
        });
    assert(originIt != func->blocks.end());
    procCtx.setCurrent(&*originIt);

    blocks.cont = findBlock(contLabel);
    blocks.trap = findBlock(trapLabel);
    blocks.nan = findBlock(nanLabel);
    blocks.overflow = findBlock(overflowLabel);
    return blocks;
}

/// @brief Emit the trap sequence used when conversions fail.
/// @details Emits a sentinel CastFpToSiRteChk instruction with NaN input to
///          surface runtime diagnostics, then emits a trap.
/// @param loc Source location associated with the failing conversion.
void BuiltinLowerContext::emitConversionTrap(il::support::SourceLoc loc) {
    setCurrentLoc(loc);
    il::core::Value sentinel =
        lowerer_->emitUnary(Opcode::CastFpToSiRteChk,
                            IlType(IlKind::I64),
                            Value::constFloat(std::numeric_limits<double>::quiet_NaN()));
    (void)sentinel;
    lowerer_->emitTrap();
}

/// @brief Lower a builtin using the rule-driven generic pipeline.
/// @details Selects the best-matching variant, emits it via
///          @ref emitBuiltinVariant, and applies any requested features. When no
///          variant matches a zero result is returned to keep lowering
///          progressing.
/// @param ctx Builtin lowering context.
/// @return Lowered builtin result.
Lowerer::RVal lowerGenericBuiltin(BuiltinLowerContext &ctx) {
    const Variant *variant = ctx.selectVariant();
    if (!variant)
        return ctx.makeZeroResult();
    Lowerer::RVal result = emitBuiltinVariant(ctx, *variant);
    ctx.applyFeatures(*variant);
    return result;
}

/// @brief Lower conversion builtins with specialised guard handling.
/// @details Chooses the appropriate variant and routes to specialised helper
///          implementations for numeric conversions or VAL-specific flows. The
///          helpers emit guard blocks and traps to replicate BASIC semantics.
/// @param ctx Builtin lowering context.
/// @return Lowered conversion result.
/// @brief Dispatch a lowering variant based on its kind.
/// @details Invokes the dedicated helper for runtime calls, unary operations,
///          or custom lowering logic. Unknown kinds fall back to a zero result
///          to avoid crashes during development.
/// @param ctx Builtin lowering context.
/// @param variant Variant describing how to lower the builtin.
/// @return Lowered builtin result.
Lowerer::RVal emitBuiltinVariant(BuiltinLowerContext &ctx, const Variant &variant) {
    switch (variant.kind) {
        case Variant::Kind::CallRuntime:
            return emitCallRuntime(ctx, variant);
        case Variant::Kind::EmitUnary:
            return emitUnary(ctx, variant);
        case Variant::Kind::Custom:
            return emitCustom(ctx, variant);
    }
    return ctx.makeZeroResult();
}

namespace {

/// @brief Emit a variant that calls directly into the runtime library.
/// @details Lowers all specified arguments, emits the runtime call, and packages
///          the result using the resolved result type.
/// @param ctx Builtin lowering context.
/// @param variant Variant metadata describing arguments and runtime symbol.
/// @return Lowered builtin result.
Lowerer::RVal emitCallRuntime(BuiltinLowerContext &ctx, const Variant &variant) {
    std::vector<Value> callArgs;
    callArgs.reserve(variant.arguments.size());
    for (const auto &argSpec : variant.arguments) {
        Lowerer::RVal &argVal = ctx.applyTransforms(argSpec, argSpec.transforms);
        callArgs.push_back(argVal.value);
    }
    IlType resultType = ctx.resolveResultType();
    ctx.setCurrentLoc(ctx.callLoc(variant.callLocArg));
    Value resultValue = ctx.emitCall(resultType, variant.runtime, callArgs);
    return {resultValue, resultType};
}

/// @brief Emit a variant that performs a unary IL operation.
/// @details Lowers the operand, resolves the result type, and emits the unary
///          opcode recorded in the variant.
/// @param ctx Builtin lowering context.
/// @param variant Variant metadata describing the unary operation.
/// @return Lowered builtin result.
Lowerer::RVal emitUnary(BuiltinLowerContext &ctx, const Variant &variant) {
    assert(!variant.arguments.empty() && "unary builtin requires an operand");
    const auto &argSpec = variant.arguments.front();
    Lowerer::RVal &argVal = ctx.applyTransforms(argSpec, argSpec.transforms);
    IlType resultType = ctx.resolveResultType();
    ctx.setCurrentLoc(ctx.callLoc(variant.callLocArg));
    Value resultValue = ctx.emitUnary(variant.opcode, resultType, argVal.value);
    return {resultValue, resultType};
}

/// @brief Emit a variant that requires bespoke lowering logic.
/// @details Switches on the builtin enumerator to delegate to specialised
///          helpers. Unsupported builtins trigger a diagnostic and return zero.
/// @param ctx Builtin lowering context.
/// @param variant Variant metadata accompanying the builtin.
/// @return Lowered builtin result.
Lowerer::RVal emitCustom(BuiltinLowerContext &ctx, const Variant &variant) {
    switch (ctx.call().builtin) {
        case BuiltinCallExpr::Builtin::Cint:
            return lowerNumericConversion(
                ctx, variant, IlType(IlKind::I64), "cint_ok", "cint_trap");
        case BuiltinCallExpr::Builtin::Clng:
            return lowerNumericConversion(
                ctx, variant, IlType(IlKind::I64), "clng_ok", "clng_trap");
        case BuiltinCallExpr::Builtin::Csng:
            return lowerNumericConversion(
                ctx, variant, IlType(IlKind::F64), "csng_ok", "csng_trap");
        case BuiltinCallExpr::Builtin::Val:
            return lowerValBuiltin(ctx, variant);
        default:
            if (auto *diag = ctx.lowerer().diagnosticEmitter()) {
                ctx.setCurrentLoc(ctx.call().loc);
                diag->emit(il::support::Severity::Error,
                           "B4003",
                           ctx.call().loc,
                           0,
                           "custom builtin lowering variant is not supported");
            }
            return ctx.makeZeroResult();
    }
}

} // namespace

namespace builtins {

namespace {

using Builtin = BuiltinCallExpr::Builtin;

/// @brief Lower builtins that have no specialised implementation.
/// @details Dispatches to the generic lowering logic that emits an indirect call
///          following the runtime registry metadata. Used as the catch-all for
///          the majority of BASIC builtins.
/// @param ctx Builtin lowering context describing the invocation.
/// @return Lowered r-value generated by the generic dispatcher.
Lowerer::RVal lowerDefaultBuiltin(BuiltinLowerContext &ctx) {
    return lowerGenericBuiltin(ctx);
}

} // namespace

/// @brief Install fallback handlers for every builtin without a special case.
/// @details Iterates the builtin enumeration and binds @ref lowerDefaultBuiltin
///          except for file I/O intrinsics that are emitted through dedicated
///          lowering routines elsewhere.
void registerDefaultBuiltins() {
    for (std::size_t ordinal = 0; ordinal <= static_cast<std::size_t>(Builtin::Err); ++ordinal) {
        auto builtin = static_cast<Builtin>(ordinal);
        switch (builtin) {
            case Builtin::Eof:
            case Builtin::Lof:
            case Builtin::Loc:
            case Builtin::Err:
                break;
            default:
                register_builtin(getBuiltinInfo(builtin).name, &lowerDefaultBuiltin);
                break;
        }
    }
}

} // namespace builtins

} // namespace il::frontends::basic::lower
