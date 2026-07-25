//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/SemanticAnalyzer_Builtins.cpp
// Purpose: Implement BASIC builtin function analysis, validating argument
//          counts and types for the semantic analyser.
// Key invariants: Builtin usage diagnostics rely on centralised helpers for
//                 arity/type checking so call sites share consistent rules.
// Ownership/Lifetime: Analyzer borrows DiagnosticEmitter; AST nodes owned
//                     externally.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md#builtins
//
//===----------------------------------------------------------------------===//

/// @file SemanticAnalyzer_Builtins.cpp
/// @brief Implements signature-driven semantic checks for BASIC builtins.
/// @details Argument expressions are analyzed before builtin-specific
///          dispatch. Arity comes from the central builtin registry, while
///          semantic signature views provide per-position type masks and result
///          types. A legacy static table remains as a bounded fallback.

#include "frontends/basic/SemanticAnalyzer_Internal.hpp"

#include <array>
#include <span>
#include <sstream>

#include "frontends/basic/BuiltinRegistry.hpp"

namespace il::frontends::basic {

using semantic_analyzer_detail::builtinName;

/// @brief Analyzes a builtin call and infers its semantic result type.
/// @details Visits every argument in source order, using @ref Type::Unknown for
///          null argument nodes. It obtains translated signature metadata, then
///          invokes the registry's specialized member-function hook when one
///          exists; otherwise it follows the generic signature path.
/// @param c Builtin call AST borrowed for analysis.
/// @return Specialized or declarative result type. Argument diagnostics do not
///         prevent a recovery type from being returned.
SemanticAnalyzer::Type SemanticAnalyzer::analyzeBuiltinCall(const BuiltinCallExpr &c) {
    std::vector<Type> argTys;
    for (auto &a : c.args)
        argTys.push_back(a ? visitExpr(*a) : Type::Unknown);

    const auto &signature = builtinSignature(c.builtin);
    const auto &info = getBuiltinInfo(c.builtin);
    if (info.analyze)
        return (this->*(info.analyze))(c, argTys, signature);
    return analyzeBuiltinWithSignature(c, argTys, signature);
}

/// @brief Validates builtin arity against an inclusive interval.
/// @details On failure emits `B2001` at the call location. Equal bounds produce
///          singular/plural exact-count text; differing bounds produce a
///          `min-max` range, and both forms report the actual count.
/// @param c Call supplying builtin name and source location.
/// @param args Analyzed arguments; only the vector size is inspected.
/// @param min Minimum accepted count.
/// @param max Maximum accepted count.
/// @return @c true exactly when `min <= args.size() <= max`.
bool SemanticAnalyzer::checkArgCount(const BuiltinCallExpr &c,
                                     const std::vector<Type> &args,
                                     size_t min,
                                     size_t max) {
    if (args.size() < min || args.size() > max) {
        std::ostringstream oss;
        oss << builtinName(c.builtin) << ": expected ";
        if (min == max)
            oss << min << " arg" << (min == 1 ? "" : "s");
        else
            oss << min << '-' << max << " args";
        oss << " (got " << args.size() << ')';
        de.emit(il::support::Severity::Error, "B2001", c.loc, 1, oss.str());
        return false;
    }
    return true;
}

/// @brief Checks one builtin argument against its accepted type categories.
/// @details Unknown types pass to avoid cascading diagnostics, and `STR$`
///          explicitly accepts boolean in addition to signature metadata.
///          Otherwise exact enum membership is required. Failure emits `B2001`
///          at the argument location when present, falling back to the call.
///          Diagnostic expectations collapse the allowed set to string,
///          number, or general value wording.
/// @param c Call supplying builtin identity and argument locations.
/// @param idx Zero-based source argument index.
/// @param argTy Inferred actual type.
/// @param allowed Exact accepted semantic types.
/// @return @c true for unknown, the `STR$` boolean exception, or exact
///         membership; @c false after a mismatch diagnostic.
bool SemanticAnalyzer::checkArgType(const BuiltinCallExpr &c,
                                    size_t idx,
                                    Type argTy,
                                    std::span<const Type> allowed) {
    if (argTy == Type::Unknown)
        return true;
    // Special case: STR$ accepts BOOLEAN (fixes BUG-012)
    if (c.builtin == BuiltinCallExpr::Builtin::Str && argTy == Type::Bool)
        return true;
    for (Type t : allowed)
        if (t == argTy)
            return true;
    il::support::SourceLoc loc = (idx < c.args.size() && c.args[idx]) ? c.args[idx]->loc : c.loc;
    bool wantString = false;
    bool wantNumber = false;
    for (Type t : allowed) {
        if (t == Type::String)
            wantString = true;
        if (t == Type::Int || t == Type::Float)
            wantNumber = true;
    }
    const char *need = wantString ? (wantNumber ? "value" : "string") : "number";
    const char *got = "unknown";
    if (argTy == Type::String)
        got = "string";
    else if (argTy == Type::Int || argTy == Type::Float)
        got = "number";
    std::ostringstream oss;
    oss << builtinName(c.builtin) << ": arg " << (idx + 1) << " must be " << need << " (got " << got
        << ')';
    de.emit(il::support::Severity::Error, "B2001", loc, 1, oss.str());
    return false;
}

namespace {
/// @brief Local shorthand for semantic value categories.
using SemanticType = SemanticAnalyzer::Type;

/// @brief Local shorthand for one argument-position specification.
using BuiltinArgSpec = SemanticAnalyzer::BuiltinArgSpec;

/// @brief Local shorthand for a complete builtin signature.
using BuiltinSignature = SemanticAnalyzer::BuiltinSignature;

/// @brief Allowed-type storage for a string-only position.
static constexpr std::array<SemanticType, 1> kStringType{{SemanticType::String}};

/// @brief Allowed-type storage for an integer-or-float position.
static constexpr std::array<SemanticType, 2> kNumericTypes{
    {SemanticType::Int, SemanticType::Float}};

/// @brief Allowed-type storage for an integer-only position.
static constexpr std::array<SemanticType, 1> kIntType{{SemanticType::Int}};

/// @brief Signature arguments for one required string.
static constexpr std::array<BuiltinArgSpec, 1> kSingleStringArg{{
    BuiltinArgSpec{false, kStringType.data(), kStringType.size()},
}};

/// @brief Signature arguments for one required numeric value.
static constexpr std::array<BuiltinArgSpec, 1> kSingleNumericArg{{
    BuiltinArgSpec{false, kNumericTypes.data(), kNumericTypes.size()},
}};

/// @brief Signature arguments for one required integer.
static constexpr std::array<BuiltinArgSpec, 1> kSingleIntArg{{
    BuiltinArgSpec{false, kIntType.data(), kIntType.size()},
}};

/// @brief Signature arguments for required string then numeric values.
static constexpr std::array<BuiltinArgSpec, 2> kStringNumericArgs{{
    BuiltinArgSpec{false, kStringType.data(), kStringType.size()},
    BuiltinArgSpec{false, kNumericTypes.data(), kNumericTypes.size()},
}};

/// @brief Signature arguments for two required numeric values.
static constexpr std::array<BuiltinArgSpec, 2> kNumericNumericArgs{{
    BuiltinArgSpec{false, kNumericTypes.data(), kNumericTypes.size()},
    BuiltinArgSpec{false, kNumericTypes.data(), kNumericTypes.size()},
}};

/// @brief Signature arguments for `MID$`: string, start, optional length.
static constexpr std::array<BuiltinArgSpec, 3> kMidArgs{{
    BuiltinArgSpec{false, kStringType.data(), kStringType.size()},
    BuiltinArgSpec{false, kNumericTypes.data(), kNumericTypes.size()},
    BuiltinArgSpec{true, kNumericTypes.data(), kNumericTypes.size()},
}};

/// @brief Signature arguments for `ROUND`: value and optional precision.
static constexpr std::array<BuiltinArgSpec, 2> kRoundArgs{{
    BuiltinArgSpec{false, kNumericTypes.data(), kNumericTypes.size()},
    BuiltinArgSpec{true, kNumericTypes.data(), kNumericTypes.size()},
}};

/// @brief Positional specification for INSTR's optional-start form.
/// @details The optional leading numeric slot is aligned against supplied
///          arguments by @ref SemanticAnalyzer::validateBuiltinArgs.
static constexpr std::array<BuiltinArgSpec, 3> kInstrArgs{{
    BuiltinArgSpec{true, kNumericTypes.data(), kNumericTypes.size()},
    BuiltinArgSpec{false, kStringType.data(), kStringType.size()},
    BuiltinArgSpec{false, kStringType.data(), kStringType.size()},
}};

/// @brief Legacy ordinal-indexed signature fallback.
/// @details Entries mirror the builtin enumeration available when this table
///          was authored. Registry semantic views take precedence, and the
///          lookup bounds-checks the enum before accessing this array.
static constexpr std::array<BuiltinSignature, 41> kBuiltinSignatures{{
    BuiltinSignature{1, 0, kSingleStringArg.data(), kSingleStringArg.size(), SemanticType::Int},
    BuiltinSignature{2, 1, kMidArgs.data(), kMidArgs.size(), SemanticType::String},
    BuiltinSignature{
        2, 0, kStringNumericArgs.data(), kStringNumericArgs.size(), SemanticType::String},
    BuiltinSignature{
        2, 0, kStringNumericArgs.data(), kStringNumericArgs.size(), SemanticType::String},
    BuiltinSignature{
        1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::String},
    BuiltinSignature{1, 0, kSingleStringArg.data(), kSingleStringArg.size(), SemanticType::Float},
    BuiltinSignature{
        1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Int}, // Cint
    BuiltinSignature{
        1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Int}, // Clng
    BuiltinSignature{
        1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Float}, // Csng
    BuiltinSignature{
        1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Float}, // Cdbl
    BuiltinSignature{1,
                     0,
                     kSingleNumericArg.data(),
                     kSingleNumericArg.size(),
                     SemanticType::Int}, // Int - BUG-OOP-016 fix
    BuiltinSignature{
        1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Int},  // Fix
    BuiltinSignature{1, 1, kRoundArgs.data(), kRoundArgs.size(), SemanticType::Float}, // Round
    BuiltinSignature{
        1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Float}, // Sqr
    BuiltinSignature{
        1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Int}, // Abs
    BuiltinSignature{
        1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Int}, // Floor
    BuiltinSignature{
        1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Int}, // Ceil
    BuiltinSignature{
        1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Float}, // Sin
    BuiltinSignature{
        1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Float}, // Cos
    // Tan, Atn, Exp, Log - all take 1 numeric arg and return Float
    BuiltinSignature{1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Float},
    BuiltinSignature{1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Float},
    BuiltinSignature{1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Float},
    BuiltinSignature{1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Float},
    // Sgn - takes 1 numeric arg and returns Int
    BuiltinSignature{1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::Int},
    BuiltinSignature{
        2, 0, kNumericNumericArgs.data(), kNumericNumericArgs.size(), SemanticType::Float},
    BuiltinSignature{0, 0, nullptr, 0, SemanticType::Float},
    BuiltinSignature{2, 1, kInstrArgs.data(), kInstrArgs.size(), SemanticType::Int},
    BuiltinSignature{1, 0, kSingleStringArg.data(), kSingleStringArg.size(), SemanticType::String},
    BuiltinSignature{1, 0, kSingleStringArg.data(), kSingleStringArg.size(), SemanticType::String},
    BuiltinSignature{1, 0, kSingleStringArg.data(), kSingleStringArg.size(), SemanticType::String},
    BuiltinSignature{1, 0, kSingleStringArg.data(), kSingleStringArg.size(), SemanticType::String},
    BuiltinSignature{1, 0, kSingleStringArg.data(), kSingleStringArg.size(), SemanticType::String},
    BuiltinSignature{
        1, 0, kSingleNumericArg.data(), kSingleNumericArg.size(), SemanticType::String},
    BuiltinSignature{1, 0, kSingleStringArg.data(), kSingleStringArg.size(), SemanticType::Int},
    BuiltinSignature{0, 0, nullptr, 0, SemanticType::String},
    BuiltinSignature{0, 0, nullptr, 0, SemanticType::String},
    BuiltinSignature{1, 0, kSingleIntArg.data(), kSingleIntArg.size(), SemanticType::Int},
    BuiltinSignature{1, 0, kSingleIntArg.data(), kSingleIntArg.size(), SemanticType::Int},
    BuiltinSignature{1, 0, kSingleIntArg.data(), kSingleIntArg.size(), SemanticType::Int},
    BuiltinSignature{0, 0, nullptr, 0, SemanticType::Int},
    BuiltinSignature{0, 0, nullptr, 0, SemanticType::Int}, // ERR
}};

} // namespace

/// @brief Produces semantic signature metadata for one builtin.
/// @details Four command-line/error builtins use explicit static signatures.
///          Other builtins first request a registry semantic view, translating
///          its type masks into thread-local storage capped at 32 argument
///          positions. Without a registry view, a bounds-checked legacy entry
///          is copied to thread-local scratch and its fixed result type is
///          refreshed from the registry. An out-of-range enum falls back to a
///          zero-argument unknown-result signature.
/// @param builtin Builtin identifier to translate.
/// @return Reference to explicit static storage or thread-local translated
///         storage. A later call on the same thread may overwrite the latter.
const SemanticAnalyzer::BuiltinSignature &SemanticAnalyzer::builtinSignature(
    BuiltinCallExpr::Builtin builtin) {
    using B = BuiltinCallExpr::Builtin;
    // Single-source truth for argument counts for key runtime builtins that were
    // added later; avoid relying on a potentially out-of-sync static table.
    static const BuiltinArgSpec kOneIntArg[] = {
        BuiltinArgSpec{false, nullptr, 0}, // Allow unknown; type-checked separately
    };
    static const BuiltinSignature kArgcSig{/*required*/ 0,
                                           /*optional*/ 0,
                                           /*args*/ nullptr,
                                           /*count*/ 0,
                                           /*result*/ Type::Int};
    static const BuiltinSignature kArgGetSig{/*required*/ 1,
                                             /*optional*/ 0,
                                             /*args*/ kOneIntArg,
                                             /*count*/ 1,
                                             /*result*/ Type::String};
    static const BuiltinSignature kCommandSig{/*required*/ 0,
                                              /*optional*/ 0,
                                              /*args*/ nullptr,
                                              /*count*/ 0,
                                              /*result*/ Type::String};
    static const BuiltinSignature kErrSig{/*required*/ 0,
                                          /*optional*/ 0,
                                          /*args*/ nullptr,
                                          /*count*/ 0,
                                          /*result*/ Type::Int};
    switch (builtin) {
        case B::Argc:
            return kArgcSig;
        case B::ArgGet:
            return kArgGetSig;
        case B::Command:
            return kCommandSig;
        case B::Err:
            return kErrSig;
        default:
            break;
    }
    // Prefer a full semantic signature view from the registry when available.
    if (const auto *view = getBuiltinSemanticSignature(builtin)) {
        thread_local BuiltinSignature cached;
        cached.requiredArgs = view->minArgs;
        cached.optionalArgs =
            (view->maxArgs >= view->minArgs) ? (view->maxArgs - view->minArgs) : 0;
        thread_local SemanticAnalyzer::BuiltinArgSpec argSpecs[32];
        static const SemanticAnalyzer::Type allowedInt[] = {Type::Int};
        static const SemanticAnalyzer::Type allowedFloat[] = {Type::Float};
        static const SemanticAnalyzer::Type allowedString[] = {Type::String};
        static const SemanticAnalyzer::Type allowedBool[] = {Type::Bool};
        static const SemanticAnalyzer::Type allowedNumber[] = {Type::Int, Type::Float};
        static const SemanticAnalyzer::Type allowedAny[] = {
            Type::Int, Type::Float, Type::String, Type::Bool};

        /// Maps one registry bit-mask category to static semantic-type storage.
        auto maskToAllowed =
            [&](BuiltinArgTypeMask m) -> std::pair<const SemanticAnalyzer::Type *, std::size_t> {
            using M = BuiltinArgTypeMask;
            switch (m) {
                case M::Int:
                    return {allowedInt, 1};
                case M::Float:
                    return {allowedFloat, 1};
                case M::String:
                    return {allowedString, 1};
                case M::Bool:
                    return {allowedBool, 1};
                case M::Number:
                    return {allowedNumber, 2};
                case M::Any:
                    return {allowedAny, 4};
                case M::None:
                default:
                    return {nullptr, 0};
            }
        };

        constexpr std::size_t kMaxRegistryBuiltinArgs = 32;
        const std::size_t n = std::min<std::size_t>(view->argCount, kMaxRegistryBuiltinArgs);
        for (std::size_t i = 0; i < n; ++i) {
            argSpecs[i].optional = view->args[i].optional;
            auto [ptr, cnt] = maskToAllowed(view->args[i].allowed);
            argSpecs[i].allowed = ptr;
            argSpecs[i].allowedCount = cnt;
        }
        cached.arguments = (n > 0) ? argSpecs : nullptr;
        cached.argumentCount = n;
        switch (view->result) {
            case BuiltinResultKind::Int:
                cached.result = Type::Int;
                break;
            case BuiltinResultKind::Float:
                cached.result = Type::Float;
                break;
            case BuiltinResultKind::String:
                cached.result = Type::String;
                break;
            case BuiltinResultKind::Unknown:
            default:
                cached.result = Type::Unknown;
                break;
        }
        return cached;
    }

    // Fallback to legacy static table but override fixed result kind when possible to reduce drift.
    // Guard against legacy table drift; default to a minimally useful signature.
    const std::size_t idx = static_cast<std::size_t>(builtin);
    static const BuiltinSignature kDefault{
        /*required*/ 0, /*optional*/ 0, nullptr, 0, Type::Unknown};
    const BuiltinSignature &legacy =
        (idx < kBuiltinSignatures.size()) ? kBuiltinSignatures[idx] : kDefault;
    thread_local BuiltinSignature scratch;
    scratch = legacy;
    switch (getBuiltinFixedResult(builtin)) {
        case BuiltinResultKind::Int:
            scratch.result = Type::Int;
            break;
        case BuiltinResultKind::Float:
            scratch.result = Type::Float;
            break;
        case BuiltinResultKind::String:
            scratch.result = Type::String;
            break;
        case BuiltinResultKind::Unknown:
        default:
            break;
    }
    return scratch;
}

/// @brief Validates builtin arguments using registry arity and type metadata.
/// @details Arity is deliberately obtained from @ref getBuiltinArity rather
///          than the supplied signature to avoid legacy-table drift. With no
///          argument specifications, valid arity is sufficient. Otherwise the
///          routine aligns optional specification slots with actual arguments,
///          skipping an optional slot when needed to preserve enough remaining
///          slots, and accumulates all type-check results rather than stopping
///          at the first mismatch.
/// @param c Call supplying registry identity and diagnostic context.
/// @param args Actual types in source order.
/// @param signature Per-position type metadata; its arity fields are not used
///                  for the initial count check.
/// @return @c false on arity failure or any position mismatch; @c true
///         otherwise.
bool SemanticAnalyzer::validateBuiltinArgs(const BuiltinCallExpr &c,
                                           const std::vector<Type> &args,
                                           const BuiltinSignature &signature) {
    // Derive arity from the BuiltinRegistry to avoid table drift.
    const auto arity = getBuiltinArity(c.builtin);
    const std::size_t minArgs = arity.minArgs;
    const std::size_t maxArgs = arity.maxArgs;
    if (!checkArgCount(c, args, minArgs, maxArgs))
        return false;

    if (signature.argumentCount == 0 || signature.arguments == nullptr)
        return true;

    bool ok = true;
    std::size_t missing =
        signature.argumentCount > args.size() ? signature.argumentCount - args.size() : 0;
    std::size_t argIndex = 0;
    for (std::size_t specIndex = 0; specIndex < signature.argumentCount && argIndex < args.size();
         ++specIndex) {
        const BuiltinArgSpec &spec = signature.arguments[specIndex];
        if (spec.optional && missing > 0) {
            const std::size_t remainingSpecs = signature.argumentCount - specIndex - 1;
            const std::size_t remainingArgs = args.size() - argIndex;
            if (remainingSpecs >= remainingArgs) {
                --missing;
                continue;
            }
        }

        if (spec.allowed != nullptr && spec.allowedCount > 0) {
            std::span<const Type> allowed(spec.allowed, spec.allowedCount);
            if (!checkArgType(c, argIndex, args[argIndex], allowed))
                ok = false;
        }
        ++argIndex;
    }

    return ok;
}

/// @brief Applies generic validation and returns the declared result type.
/// @details Validation diagnostics do not alter the result, allowing downstream
///          analysis to continue with the builtin's known type.
/// @param c Call to validate.
/// @param args Actual argument types.
/// @param signature Type constraints and recovery/result type.
/// @return @p signature.result regardless of validation success.
SemanticAnalyzer::Type SemanticAnalyzer::analyzeBuiltinWithSignature(
    const BuiltinCallExpr &c, const std::vector<Type> &args, const BuiltinSignature &signature) {
    validateBuiltinArgs(c, args, signature);
    return signature.result;
}

/// @brief Applies operand-dependent result typing for `ABS`.
/// @details Runs normal validation first. A valid float operand produces float;
///          an integer or unknown operand produces integer. Invalid arity, an
///          empty argument list, or an already-diagnosed type mismatch also
///          recovers as integer to preserve legacy behavior.
/// @param c ABS call supplying diagnostics.
/// @param args Actual argument types.
/// @param signature ABS argument metadata.
/// @return Float only for a valid first float argument; integer otherwise.
SemanticAnalyzer::Type SemanticAnalyzer::analyzeAbs(const BuiltinCallExpr &c,
                                                    const std::vector<Type> &args,
                                                    const BuiltinSignature &signature) {
    const bool countOk = validateBuiltinArgs(c, args, signature);
    if (!countOk || args.empty())
        return Type::Int;

    if (args[0] == Type::Float)
        return Type::Float;
    if (args[0] == Type::Int || args[0] == Type::Unknown)
        return Type::Int;

    // Type mismatch already diagnosed; fall back to integer type to match legacy behaviour.
    return Type::Int;
}

/// @brief Applies INSTR's specialized-handler hook.
/// @details Currently performs the same validation as the generic path,
///          including optional-leading-argument alignment, then returns the
///          signature result regardless of errors.
/// @param c INSTR call supplying diagnostics.
/// @param args Actual argument types.
/// @param signature INSTR signature metadata.
/// @return @p signature.result.
SemanticAnalyzer::Type SemanticAnalyzer::analyzeInstr(const BuiltinCallExpr &c,
                                                      const std::vector<Type> &args,
                                                      const BuiltinSignature &signature) {
    validateBuiltinArgs(c, args, signature);
    return signature.result;
}

} // namespace il::frontends::basic
