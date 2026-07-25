//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/SemanticAnalyzer_Exprs.cpp
// Purpose: Implement expression analysis for the BASIC semantic analyser,
//          including variable resolution, operator checking, and array access
//          validation.
// Key invariants: Expression analysis reports type mismatches and symbol
//                 resolution issues while visitor overrides defer to
//                 SemanticAnalyzer helpers.
// Ownership/Lifetime: Analyzer borrows DiagnosticEmitter; AST nodes owned
//                     externally.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md#expressions
//
//===----------------------------------------------------------------------===//

/// @file SemanticAnalyzer_Exprs.cpp
/// @brief Implements BASIC expression resolution, typing, and annotation.
/// @details This translation unit contains qualified-name and runtime-type
///          helpers, safe-pointer bridge checks, the mutable expression visitor,
///          user/runtime member and method resolution, object construction
///          validation, implicit-conversion recording, and array-bound queries.

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/SemanticAnalyzer_Internal.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/sem/OverloadResolution.hpp"
#include "frontends/basic/sem/RuntimeMethodIndex.hpp"
#include "frontends/basic/sem/TypeRegistry.hpp"
#include "il/runtime/RuntimeClassNames.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <algorithm>
#include <limits>

namespace il::frontends::basic::semantic_analyzer_detail {

/// @brief Tests whether an expression is a `Zanna`-rooted name chain.
/// @details Walks variable/member nodes from leaf to root, rejects every other
///          node kind, reverses the collected segments, and compares the first
///          segment with `Zanna` case-insensitively. It does not verify that the
///          remaining chain names an existing runtime class or member.
/// @param expr Candidate variable or member-access chain.
/// @return @c true when the reconstructed chain is non-empty and rooted at
///         `Zanna`.
static bool isRuntimeNamespaceChain(const Expr &expr) {
    std::vector<std::string> parts;
    const Expr *cur = &expr;
    while (cur) {
        if (auto *var = as<const VarExpr>(*cur)) {
            parts.push_back(var->name);
            break;
        }
        if (auto *mem = as<const MemberAccessExpr>(*cur)) {
            parts.push_back(mem->member);
            cur = mem->base.get();
            continue;
        }
        return false;
    }

    if (parts.empty())
        return false;
    std::reverse(parts.begin(), parts.end());
    return string_utils::iequals(parts.front(), "Zanna");
}

/// @brief Appends source-order segments from a qualified expression chain.
/// @details Accepts a variable root followed by zero or more member accesses.
///          Segments are collected leaf-first and then reversed in place. On
///          failure, any segments already appended to @p out are not rolled
///          back; callers therefore pass an empty vector.
/// @param expr Candidate variable/member chain.
/// @param out Destination receiving root-to-leaf names.
/// @return @c true for a non-empty chain ending at a variable root.
static bool collectQualifiedChain(const Expr &expr, std::vector<std::string> &out) {
    const Expr *cur = &expr;
    while (cur) {
        if (auto *var = as<const VarExpr>(*cur)) {
            out.push_back(var->name);
            break;
        }
        if (auto *mem = as<const MemberAccessExpr>(*cur)) {
            out.push_back(mem->member);
            cur = mem->base.get();
            continue;
        }
        return false;
    }
    if (out.empty())
        return false;
    std::reverse(out.begin(), out.end());
    return true;
}

/// @brief Reconstructs a potential fully qualified runtime name.
/// @param expr Candidate qualified expression chain.
/// @return Dot-joined source spellings when the root is case-insensitively
///         `Zanna`; otherwise @c std::nullopt.
/// @note This performs syntactic reconstruction only, not registry lookup.
static std::optional<std::string> runtimeClassQNameFromExpr(const Expr &expr) {
    std::vector<std::string> parts;
    if (!collectQualifiedChain(expr, parts))
        return std::nullopt;
    if (!string_utils::iequals(parts.front(), "Zanna"))
        return std::nullopt;
    return JoinDots(parts);
}

/// @brief Safety role assigned to a raw-pointer runtime parameter.
/// @details Only explicitly recognized callback bridge APIs receive non-default
///          roles.
enum class RuntimePointerBridgeRole { None, Callback, Payload };

/// @brief Classifies a runtime API parameter for safe callback bridging.
/// @details Matches canonical target names case-sensitively against an explicit
///          allowlist for thread, async, and HTTP server APIs. Callback and
///          payload positions differ by API; all unlisted positions return
///          @ref RuntimePointerBridgeRole::None.
/// @param target Canonical runtime method/function name.
/// @param argIndex Zero-based parameter index.
/// @return Allowlisted bridge role for the position.
static RuntimePointerBridgeRole runtimePointerBridgeRole(std::string_view target,
                                                         std::size_t argIndex) {
    auto is = [&](std::string_view name) { return target == name; };

    if (is("Zanna.Threads.Thread.Start") || is("Zanna.Threads.Thread.StartSafe") ||
        is("Zanna.Threads.Async.Run")) {
        if (argIndex == 0)
            return RuntimePointerBridgeRole::Callback;
        if (argIndex == 1)
            return RuntimePointerBridgeRole::Payload;
    }

    if (is("Zanna.Threads.Thread.StartOwned") || is("Zanna.Threads.Thread.StartSafeOwned") ||
        is("Zanna.Threads.Async.RunOwned")) {
        return argIndex == 0 ? RuntimePointerBridgeRole::Callback : RuntimePointerBridgeRole::None;
    }

    if (is("Zanna.Threads.Async.RunCancellable")) {
        if (argIndex == 0)
            return RuntimePointerBridgeRole::Callback;
        if (argIndex == 1)
            return RuntimePointerBridgeRole::Payload;
    }

    if (is("Zanna.Threads.Async.RunCancellableOwned"))
        return argIndex == 0 ? RuntimePointerBridgeRole::Callback : RuntimePointerBridgeRole::None;

    if (is("Zanna.Threads.Async.Map")) {
        if (argIndex == 1)
            return RuntimePointerBridgeRole::Callback;
        if (argIndex == 2)
            return RuntimePointerBridgeRole::Payload;
    }

    if (is("Zanna.Threads.Async.MapOwned"))
        return argIndex == 1 ? RuntimePointerBridgeRole::Callback : RuntimePointerBridgeRole::None;

    if (is("Zanna.Network.HttpServer.BindHandler") || is("Zanna.Network.HttpsServer.BindHandler")) {
        return argIndex == 1 ? RuntimePointerBridgeRole::Callback : RuntimePointerBridgeRole::None;
    }

    return RuntimePointerBridgeRole::None;
}

/// @brief Suggests a typed alternative to selected raw-pointer parse APIs.
/// @param target Canonical runtime API name, matched case-sensitively.
/// @return Replacement name for `TryInt`, `TryDouble`, or `TryBool`; otherwise
///         an empty string.
static std::string saferRuntimePointerAlternative(std::string_view target) {
    if (target == "Zanna.Core.Parse.TryInt")
        return "Zanna.Core.Parse.IntOr";
    if (target == "Zanna.Core.Parse.TryDouble")
        return "Zanna.Core.Parse.DoubleOr";
    if (target == "Zanna.Core.Parse.TryBool")
        return "Zanna.Core.Parse.BoolOr";
    return {};
}

/// @brief Tests whether an argument node is an address-of expression.
/// @param expr Possibly null argument pointer.
/// @return @c true only for a non-null node whose kind is @ref Expr::Kind::AddressOf.
static bool isAddressOfArg(const ExprPtr &expr) {
    return expr && expr->kind() == Expr::Kind::AddressOf;
}

/// @brief Maps runtime signature type text to a BASIC semantic category.
/// @details Trims ASCII space/tab/newline/carriage-return at both ends and one
///          trailing nullable marker (`?`). Scalar `i64`, `f64`, `i1`, and
///          `str` map directly. Object/pointer/sequence/list spellings, including
///          their `<...>` prefixes, map to object.
/// @param ty Runtime type token to normalize and classify.
/// @return Corresponding type, or @c std::nullopt for unsupported text.
static std::optional<SemanticAnalyzer::Type> semanticTypeFromRuntimeType(std::string_view ty) {
    /// Removes surrounding ASCII whitespace and one nullable suffix.
    auto trimRuntimeToken = [](std::string_view token) {
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t' ||
                                  token.front() == '\n' || token.front() == '\r'))
            token.remove_prefix(1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t' ||
                                  token.back() == '\n' || token.back() == '\r'))
            token.remove_suffix(1);
        if (!token.empty() && token.back() == '?')
            token.remove_suffix(1);
        return token;
    };
    ty = trimRuntimeToken(ty);
    if (ty == "i64")
        return SemanticAnalyzer::Type::Int;
    if (ty == "f64")
        return SemanticAnalyzer::Type::Float;
    if (ty == "i1")
        return SemanticAnalyzer::Type::Bool;
    if (ty == "str")
        return SemanticAnalyzer::Type::String;
    if (ty == "obj" || ty == "ptr" || ty.rfind("obj<", 0) == 0 || ty.rfind("ptr<", 0) == 0 ||
        ty == "seq" || ty.rfind("seq<", 0) == 0 || ty == "list" || ty.rfind("list<", 0) == 0)
        return SemanticAnalyzer::Type::Object;
    return std::nullopt;
}

/// @brief Converts a declared BASIC type to semantic-analysis type space.
/// @param ty BASIC declaration type.
/// @return Matching scalar/object category; void, unknown, and defensive
///         fall-through map to @ref SemanticAnalyzer::Type::Unknown.
static SemanticAnalyzer::Type semanticTypeFromBasicType(BasicType ty) {
    switch (ty) {
        case BasicType::Int:
            return SemanticAnalyzer::Type::Int;
        case BasicType::Float:
            return SemanticAnalyzer::Type::Float;
        case BasicType::String:
            return SemanticAnalyzer::Type::String;
        case BasicType::Bool:
            return SemanticAnalyzer::Type::Bool;
        case BasicType::Object:
            return SemanticAnalyzer::Type::Object;
        case BasicType::Void:
        case BasicType::Unknown:
        default:
            return SemanticAnalyzer::Type::Unknown;
    }
}

/// @brief Converts a semantic category back to declaration type space.
/// @param ty Semantic type to translate.
/// @return Matching scalar/object type. Arrays, unknown, and defensive
///         fall-through map to @ref BasicType::Unknown.
static BasicType basicTypeFromSemanticType(SemanticAnalyzer::Type ty) {
    switch (ty) {
        case SemanticAnalyzer::Type::Int:
            return BasicType::Int;
        case SemanticAnalyzer::Type::Float:
            return BasicType::Float;
        case SemanticAnalyzer::Type::String:
            return BasicType::String;
        case SemanticAnalyzer::Type::Bool:
            return BasicType::Bool;
        case SemanticAnalyzer::Type::Object:
            return BasicType::Object;
        case SemanticAnalyzer::Type::ArrayInt:
        case SemanticAnalyzer::Type::ArrayString:
        case SemanticAnalyzer::Type::ArrayObject:
        case SemanticAnalyzer::Type::Unknown:
        default:
            return BasicType::Unknown;
    }
}

/// @brief Converts supported semantic scalars to compact AST types.
/// @param ty Semantic type to translate.
/// @return AST integer, float, string, or boolean type; arrays, objects, and
///         unknown yield @c std::nullopt.
static std::optional<::il::frontends::basic::Type> astTypeFromSemanticType(
    SemanticAnalyzer::Type ty) {
    switch (ty) {
        case SemanticAnalyzer::Type::Int:
            return ::il::frontends::basic::Type::I64;
        case SemanticAnalyzer::Type::Float:
            return ::il::frontends::basic::Type::F64;
        case SemanticAnalyzer::Type::String:
            return ::il::frontends::basic::Type::Str;
        case SemanticAnalyzer::Type::Bool:
            return ::il::frontends::basic::Type::Bool;
        default:
            return std::nullopt;
    }
}

/// @brief Determines the semantic value type of an indexed OOP field.
/// @details A non-empty object class annotation takes precedence over the
///          field's scalar AST type.
/// @param field Indexed field metadata.
/// @return Object for class-typed fields, otherwise the corresponding scalar
///         category; defensive fall-through is unknown.
SemanticAnalyzer::Type semanticTypeFromOopField(const ClassInfo::FieldInfo &field) {
    if (!field.objectClassName.empty())
        return SemanticAnalyzer::Type::Object;

    switch (field.type) {
        case ::il::frontends::basic::Type::I64:
            return SemanticAnalyzer::Type::Int;
        case ::il::frontends::basic::Type::F64:
            return SemanticAnalyzer::Type::Float;
        case ::il::frontends::basic::Type::Str:
            return SemanticAnalyzer::Type::String;
        case ::il::frontends::basic::Type::Bool:
            return SemanticAnalyzer::Type::Bool;
    }
    return SemanticAnalyzer::Type::Unknown;
}

/// @brief Looks up an unqualified field on the active implicit instance.
/// @param analyzer Analyzer supplying active receiver and OOP hierarchy state.
/// @param name Field spelling to resolve through the hierarchy.
/// @return Pointer to index-owned field metadata, or @c nullptr when no
///         implicit receiver/field exists.
const ClassInfo::FieldInfo *findActiveInstanceField(const SemanticAnalyzer &analyzer,
                                                    std::string_view name) {
    auto className = analyzer.activeInstanceClassQName();
    if (!className)
        return nullptr;
    return analyzer.oopIndex().findFieldInHierarchy(*className, name);
}

/// @brief Resolves an expression to an indexed user-class qualified name.
/// @details First treats a syntactic name chain as a class name, then falls back
///          to general object-class inference. Either candidate is returned
///          only if @ref OopIndex contains the class.
/// @param analyzer Analyzer supplying the OOP index.
/// @param expr Expression to classify.
/// @return Indexed qualified class name, or @c std::nullopt.
static std::optional<std::string> resolveUserClassQNameFromExpr(SemanticAnalyzer &analyzer,
                                                                const Expr &expr) {
    std::vector<std::string> parts;
    if (collectQualifiedChain(expr, parts)) {
        std::string dotted = JoinDots(parts);
        if (analyzer.oopIndex().findClass(dotted))
            return dotted;
    }

    if (auto inferred = inferObjectClassQName(analyzer, expr)) {
        if (analyzer.oopIndex().findClass(*inferred))
            return inferred;
    }

    return std::nullopt;
}

/// @brief Emits the standard missing-method diagnostic.
/// @param diagnostics Diagnostic facade to receive `E_NO_SUCH_METHOD`.
/// @param className Display name of the resolved receiver class.
/// @param method Missing method spelling.
/// @param loc Source location of the method access.
static void emitNoSuchMethod(SemanticDiagnostics &diagnostics,
                             const std::string &className,
                             const std::string &method,
                             il::support::SourceLoc loc) {
    std::string msg = "no such method '" + method + "' on '" + className + "'";
    diagnostics.emit(il::support::Severity::Error,
                     "E_NO_SUCH_METHOD",
                     loc,
                     static_cast<uint32_t>(method.size()),
                     std::move(msg));
}

/// @brief Infers the concrete class returned by a runtime function call.
/// @details Queries the runtime registry by canonical name. Explicit concrete
///          return metadata wins; otherwise an object-returning `.New` method
///          is inferred to return its registered class. An exact name is tried
///          first. Only unqualified callees are retried beneath active imported
///          namespaces, whose iteration order is unspecified.
/// @param analyzer Analyzer supplying active imports.
/// @param calleeName Qualified or unqualified runtime callee spelling.
/// @return Concrete runtime class qualified name when inferable.
static std::optional<std::string> resolveRuntimeFunctionReturnClassQName(
    SemanticAnalyzer &analyzer, std::string_view calleeName) {
    const auto &registry = il::runtime::RuntimeRegistry::instance();

    /// Resolves concrete return metadata for one canonical registry name.
    auto resolveConcrete = [&](std::string_view canonicalName) -> std::optional<std::string> {
        auto sig = registry.findFunction(canonicalName);
        if (!sig)
            return std::nullopt;

        if (std::string concrete = il::runtime::concreteRuntimeReturnClassQName(*sig);
            !concrete.empty()) {
            return concrete;
        }

        if (sig->returnType != il::runtime::ILScalarType::Object)
            return std::nullopt;

        auto lastDot = canonicalName.rfind('.');
        if (lastDot == std::string_view::npos)
            return std::nullopt;

        std::string prefix(canonicalName.substr(0, lastDot));
        std::string_view method = canonicalName.substr(lastDot + 1);
        if (string_utils::iequals(method, "New") && il::runtime::findRuntimeClassByQName(prefix))
            return prefix;

        return std::nullopt;
    };

    if (auto exact = resolveConcrete(calleeName))
        return exact;

    if (calleeName.find('.') != std::string_view::npos)
        return std::nullopt;

    for (const auto &ns : analyzer.getUsingImports()) {
        std::string qualified = ns;
        if (!qualified.empty())
            qualified.push_back('.');
        qualified.append(calleeName);
        if (auto imported = resolveConcrete(qualified))
            return imported;
    }

    return std::nullopt;
}

/// @brief Infers a concrete object class from a typed expression shape.
/// @details Handles `ME`, tracked object variables, `NEW`, runtime function
///          calls, runtime/user method calls, and member access. Runtime method
///          inference uses unknown argument types, then user methods/array
///          fields are consulted. The routine does not emit diagnostics.
/// @param analyzer Analyzer supplying active receiver, variable mappings,
///                 imports, and class indexes.
/// @param expr Expression whose object provenance is inspected.
/// @return Qualified class name when a concrete result is known.
std::optional<std::string> inferObjectClassQName(SemanticAnalyzer &analyzer, const Expr &expr) {
    if (as<const MeExpr>(expr)) {
        return analyzer.activeInstanceClassQName();
    }

    if (const auto *var = as<const VarExpr>(expr))
        return analyzer.lookupObjectClassQName(var->name);

    if (const auto *alloc = as<const NewExpr>(expr)) {
        if (!alloc->className.empty())
            return alloc->className;
        if (!alloc->qualifiedType.empty())
            return JoinDots(alloc->qualifiedType);
        return std::nullopt;
    }

    if (const auto *call = as<const CallExpr>(expr)) {
        std::string calleeName =
            !call->calleeQualified.empty() ? JoinDots(call->calleeQualified) : call->callee;
        if (calleeName.empty())
            return std::nullopt;
        return resolveRuntimeFunctionReturnClassQName(analyzer, calleeName);
    }

    if (const auto *call = as<const MethodCallExpr>(expr)) {
        if (!call->base)
            return std::nullopt;

        std::string baseClass;
        if (auto runtimeBase = runtimeClassQNameFromExpr(*call->base))
            baseClass = *runtimeBase;
        else if (auto inferredBase = inferObjectClassQName(analyzer, *call->base))
            baseClass = *inferredBase;

        if (baseClass.empty())
            return std::nullopt;

        std::vector<BasicType> argTypes(call->args.size(), BasicType::Unknown);

        if (auto entry = runtimeMethodIndex().find(baseClass, call->method, argTypes);
            entry && !entry->returnClassQName.empty()) {
            return entry->returnClassQName;
        }

        if (const auto *klass = analyzer.oopIndex().findClass(baseClass)) {
            auto it = klass->methods.find(std::string(call->method));
            if (it != klass->methods.end() && !it->second.sig.returnClassName.empty())
                return it->second.sig.returnClassName;

            if (const auto *field =
                    analyzer.oopIndex().findFieldInHierarchy(baseClass, call->method);
                field && field->isArray && !field->objectClassName.empty()) {
                return field->objectClassName;
            }
        }

        return std::nullopt;
    }

    if (const auto *access = as<const MemberAccessExpr>(expr)) {
        if (!access->base)
            return std::nullopt;

        if (auto runtimeBase = runtimeClassQNameFromExpr(*access->base)) {
            if (const auto *klass = analyzer.oopIndex().findClass(*runtimeBase)) {
                if (const auto *field =
                        analyzer.oopIndex().findField(klass->qualifiedName, access->member);
                    field && !field->objectClassName.empty()) {
                    return field->objectClassName;
                }
            }
        }

        if (auto baseClass = inferObjectClassQName(analyzer, *access->base)) {
            if (const auto *field = analyzer.oopIndex().findField(*baseClass, access->member);
                field && !field->objectClassName.empty()) {
                return field->objectClassName;
            }
        }
    }

    return std::nullopt;
}

/// @brief Resolves the semantic type of a runtime property access.
/// @details Determines the base class from a `Zanna...` chain or inferred
///          object class, locates that runtime class, and searches properties
///          case-insensitively. The property's runtime type text is translated
///          with @ref semanticTypeFromRuntimeType.
/// @param analyzer Analyzer supplying object-class inference.
/// @param expr Member access whose base and member are inspected.
/// @return Mapped property type, or @c std::nullopt when the base class,
///         property, or runtime type is unknown.
static std::optional<SemanticAnalyzer::Type> resolveRuntimePropertyType(
    SemanticAnalyzer &analyzer, const MemberAccessExpr &expr) {
    const il::runtime::RuntimeClass *klass = nullptr;
    if (expr.base) {
        if (auto qname = runtimeClassQNameFromExpr(*expr.base)) {
            klass = il::runtime::findRuntimeClassByQName(*qname);
        } else if (auto qname2 = inferObjectClassQName(analyzer, *expr.base)) {
            klass = il::runtime::findRuntimeClassByQName(*qname2);
        }
    }

    if (!klass)
        return std::nullopt;

    for (const auto &prop : klass->properties) {
        if (string_utils::iequals(prop.name, expr.member))
            return semanticTypeFromRuntimeType(prop.type ? prop.type : "");
    }

    return std::nullopt;
}

} // namespace il::frontends::basic::semantic_analyzer_detail

namespace il::frontends::basic {

using semantic_analyzer_detail::astToSemanticType;
using semantic_analyzer_detail::astTypeFromSemanticType;
using semantic_analyzer_detail::emitNoSuchMethod;
using semantic_analyzer_detail::inferObjectClassQName;
using semantic_analyzer_detail::isRuntimeNamespaceChain;
using semantic_analyzer_detail::levenshtein;
using semantic_analyzer_detail::resolveRuntimePropertyType;
using semantic_analyzer_detail::resolveUserClassQNameFromExpr;
using semantic_analyzer_detail::runtimeClassQNameFromExpr;
using semantic_analyzer_detail::semanticTypeName;

/// @brief Validates raw-pointer exposure in a runtime call signature.
/// @details Returns immediately for pointer-free signatures. Raw-pointer
///          returns are always rejected. Raw-pointer parameters pass only for
///          explicitly classified callback positions containing `ADDRESSOF`,
///          or classified payload positions following an accepted callback and
///          not themselves containing `ADDRESSOF`. The first violation emits
///          `B2131`, optionally suggesting a known typed alternative.
/// @param target Canonical runtime target, or empty to use @p displayName.
/// @param rawPointerReturn Whether the signature returns a raw pointer.
/// @param rawPointerParams Per-position raw-pointer flags.
/// @param args Actual argument nodes used for bridge-shape validation.
/// @param loc Fallback source location.
/// @param displayName Source-facing method/function name used for diagnostics.
/// @return @c true when no unsafe exposure remains; @c false after one
///         diagnostic.
/// @note The reserved @ref allowUnsafePointers_ flag is not consulted here.
bool SemanticAnalyzer::checkRuntimePointerSafety(std::string_view target,
                                                 bool rawPointerReturn,
                                                 const std::vector<bool> &rawPointerParams,
                                                 const std::vector<ExprPtr> &args,
                                                 il::support::SourceLoc loc,
                                                 std::string_view displayName) {
    bool hasRawParam = false;
    for (bool raw : rawPointerParams) {
        if (raw) {
            hasRawParam = true;
            break;
        }
    }
    if (!rawPointerReturn && !hasRawParam)
        return true;

    std::string targetName(target);
    if (targetName.empty())
        targetName = std::string(displayName);

    /// Formats the common safe-BASIC rejection and a typed replacement hint.
    auto diagnosticMessage = [&](std::string detail) {
        std::string message = "Runtime API '" + targetName + "' exposes " + detail +
                              " and is unavailable in safe BASIC";
        if (std::string alternative =
                semantic_analyzer_detail::saferRuntimePointerAlternative(targetName);
            !alternative.empty()) {
            message += "; use " + alternative;
        } else {
            message += "; use a typed runtime class/API";
        }
        return message;
    };

    const uint32_t displayLength =
        static_cast<uint32_t>(std::max<std::size_t>(displayName.size(), 1));
    if (rawPointerReturn) {
        de.emit(il::support::Severity::Error,
                "B2131",
                loc,
                displayLength,
                diagnosticMessage("a raw pointer return"));
        return false;
    }

    std::vector<bool> safeCallbacks(rawPointerParams.size(), false);
    for (std::size_t i = 0; i < rawPointerParams.size(); ++i) {
        if (!rawPointerParams[i])
            continue;

        semantic_analyzer_detail::RuntimePointerBridgeRole role =
            semantic_analyzer_detail::runtimePointerBridgeRole(targetName, i);
        if (role == semantic_analyzer_detail::RuntimePointerBridgeRole::Callback &&
            i < args.size() && semantic_analyzer_detail::isAddressOfArg(args[i])) {
            safeCallbacks[i] = true;
            continue;
        }

        if (role == semantic_analyzer_detail::RuntimePointerBridgeRole::Payload &&
            i < args.size() && !semantic_analyzer_detail::isAddressOfArg(args[i])) {
            bool pairedWithCallback = false;
            for (std::size_t prior = 0; prior < i && prior < safeCallbacks.size(); ++prior) {
                if (safeCallbacks[prior]) {
                    pairedWithCallback = true;
                    break;
                }
            }
            if (pairedWithCallback)
                continue;
        }

        il::support::SourceLoc argLoc = loc;
        if (i < args.size() && args[i])
            argLoc = args[i]->loc;
        de.emit(il::support::Severity::Error,
                "B2131",
                argLoc,
                1,
                diagnosticMessage("raw pointer argument " + std::to_string(i + 1)));
        return false;
    }

    return true;
}

/// @brief Mutable AST visitor that computes one expression's semantic type.
/// @details Literal overrides assign immediate categories; compound nodes
///          delegate to analyzer helpers or perform member/method/type-test
///          resolution locally. Each visit replaces @ref result_, initialized
///          to unknown, and may annotate mutable nodes or emit diagnostics.
/// @invariant @ref analyzer_ outlives the visitor.
class SemanticAnalyzerExprVisitor final : public MutExprVisitor {
  public:
    /// @brief Binds a visitor to shared analyzer state.
    /// @param analyzer Analyzer borrowed for resolution and diagnostics.
    /// @post The initial result is @ref SemanticAnalyzer::Type::Unknown.
    explicit SemanticAnalyzerExprVisitor(SemanticAnalyzer &analyzer) noexcept
        : analyzer_(analyzer) {}

    /// @brief Classifies an integer literal as integer.
    void visit(IntExpr &) override {
        result_ = SemanticAnalyzer::Type::Int;
    }

    /// @brief Classifies a floating literal as float.
    void visit(FloatExpr &) override {
        result_ = SemanticAnalyzer::Type::Float;
    }

    /// @brief Classifies a string literal as string.
    void visit(StringExpr &) override {
        result_ = SemanticAnalyzer::Type::String;
    }

    /// @brief Classifies a boolean literal as boolean.
    void visit(BoolExpr &) override {
        result_ = SemanticAnalyzer::Type::Bool;
    }

    /// @brief Resolves and classifies a variable expression.
    /// @param expr Mutable variable passed to @ref SemanticAnalyzer::analyzeVar.
    void visit(VarExpr &expr) override {
        result_ = analyzer_.analyzeVar(expr);
    }

    /// @brief Validates and classifies an array element access.
    /// @param expr Mutable access passed to @ref SemanticAnalyzer::analyzeArray.
    void visit(ArrayExpr &expr) override {
        result_ = analyzer_.analyzeArray(expr);
    }

    /// @brief Validates and classifies a unary expression.
    /// @param expr Expression passed to @ref SemanticAnalyzer::analyzeUnary.
    void visit(UnaryExpr &expr) override {
        result_ = analyzer_.analyzeUnary(expr);
    }

    /// @brief Validates and classifies a binary expression.
    /// @param expr Expression passed to @ref SemanticAnalyzer::analyzeBinary.
    void visit(BinaryExpr &expr) override {
        result_ = analyzer_.analyzeBinary(expr);
    }

    /// @brief Validates and classifies a builtin call.
    /// @param expr Call passed to @ref SemanticAnalyzer::analyzeBuiltinCall.
    void visit(BuiltinCallExpr &expr) override {
        result_ = analyzer_.analyzeBuiltinCall(expr);
    }

    /// @brief Validates and classifies an `LBOUND` query.
    /// @param expr Query passed to @ref SemanticAnalyzer::analyzeLBound.
    void visit(LBoundExpr &expr) override {
        result_ = analyzer_.analyzeLBound(expr);
    }

    /// @brief Validates and classifies a `UBOUND` query.
    /// @param expr Query passed to @ref SemanticAnalyzer::analyzeUBound.
    void visit(UBoundExpr &expr) override {
        result_ = analyzer_.analyzeUBound(expr);
    }

    /// @brief Resolves and classifies a user/runtime function call.
    /// @param expr Call passed to @ref SemanticAnalyzer::analyzeCall.
    void visit(CallExpr &expr) override {
        result_ = analyzer_.analyzeCall(expr);
    }

    /// @brief Resolves and validates object construction.
    /// @param expr Construction passed to @ref SemanticAnalyzer::analyzeNew.
    void visit(NewExpr &expr) override {
        result_ = analyzer_.analyzeNew(expr);
    }

    /// @brief Validates an implicit-instance `ME` reference.
    /// @details Produces object only while an instance member and non-empty
    ///          active class are recorded; otherwise emits `B2130` and produces
    ///          unknown.
    /// @param expr Reference supplying the diagnostic location.
    void visit(MeExpr &expr) override {
        if (analyzer_.activeMemberHasMe_ && !analyzer_.activeClassQName_.empty()) {
            result_ = SemanticAnalyzer::Type::Object;
            return;
        }

        analyzer_.de.emit(il::support::Severity::Error,
                          "B2130",
                          expr.loc,
                          2,
                          "ME can only be used inside an instance class member");
        result_ = SemanticAnalyzer::Type::Unknown;
    }

    /// @brief Resolves a runtime property or user-class field access.
    /// @details Non-runtime namespace bases are visited first so undefined
    ///          variables are diagnosed. Runtime properties take precedence.
    ///          Then a user class is inferred and its hierarchy searched for a
    ///          field. Failure is silent here and produces unknown.
    /// @param expr Mutable member access to resolve.
    void visit(MemberAccessExpr &expr) override {
        // Validate base expression (catches undefined variables like 'A' in 'A.B')
        if (expr.base && !isRuntimeNamespaceChain(*expr.base)) {
            analyzer_.visitExpr(*expr.base);
        }
        if (auto rtType = resolveRuntimePropertyType(analyzer_, expr)) {
            result_ = *rtType;
            return;
        }
        if (expr.base) {
            if (auto className = resolveUserClassQNameFromExpr(analyzer_, *expr.base)) {
                if (const auto *field =
                        analyzer_.oopIndex().findFieldInHierarchy(*className, expr.member)) {
                    result_ = semantic_analyzer_detail::semanticTypeFromOopField(*field);
                    return;
                }
            }
        }
        result_ = SemanticAnalyzer::Type::Unknown;
    }

    /// @brief Reject primitive arguments bound to object parameters of a runtime method.
    /// @details Runtime object parameters are pointers at the IL level; an INTEGER,
    ///          FLOAT, or BOOLEAN argument would otherwise reach IL verification as an
    ///          invalid i64/f64 operand. Strings are permitted (the lowerer passes them
    ///          as managed references), as are OBJECT and untyped/unknown arguments and
    ///          the literal `0` null-object idiom.
    /// @param info Resolved runtime method signature.
    /// @param argTypes Actual arguments translated to BASIC declaration types.
    /// @param args AST arguments used to recognize literal integer zero.
    /// @param method Display method name for diagnostics.
    /// @param loc Call location used for `B2001`.
    /// @return @c true when every inspected object parameter accepts its
    ///         argument; @c false after the first primitive mismatch.
    bool checkObjectParamArgs(const RuntimeMethodInfo &info,
                              const std::vector<BasicType> &argTypes,
                              const std::vector<ExprPtr> &args,
                              const std::string &method,
                              il::support::SourceLoc loc) {
        const std::size_t count = std::min(info.args.size(), argTypes.size());
        for (std::size_t i = 0; i < count; ++i) {
            if (info.args[i] != BasicType::Object)
                continue;
            const BasicType argTy = argTypes[i];
            if (argTy == BasicType::Int || argTy == BasicType::Float ||
                argTy == BasicType::Bool) {
                if (argTy == BasicType::Int && i < args.size() && args[i]) {
                    if (const auto *lit = as<const IntExpr>(*args[i]); lit && lit->value == 0)
                        continue; // Literal 0 is the null-object idiom.
                }
                std::string msg = "argument " + std::to_string(i + 1) + " to '" + method +
                                  "' expects an OBJECT; box primitives with Zanna.Core.Box";
                analyzer_.de.emit(il::support::Severity::Error, "B2001", loc, 1, std::move(msg));
                return false;
            }
        }
        return true;
    }

    /// @brief Resolves and validates a runtime or user-class method invocation.
    /// @details Analyzes the non-namespace base and every argument first.
    ///          Resolution order is: concrete runtime class (including strings),
    ///          user-class instance/static overload, array-field indexing,
    ///          root runtime-object fallback, then unknown. Runtime methods pass
    ///          pointer-safety and object-parameter checks. User overload
    ///          resolution maps unsupported argument categories to integer for
    ///          recovery. Inaccessible same-name user methods deliberately
    ///          defer diagnostics to lowering; truly absent methods emit
    ///          `E_NO_SUCH_METHOD`.
    /// @param expr Mutable method-call expression.
    void visit(MethodCallExpr &expr) override {
        SemanticAnalyzer::Type baseType = SemanticAnalyzer::Type::Unknown;
        const bool runtimeNamespaceBase = expr.base && isRuntimeNamespaceChain(*expr.base);
        if (expr.base && !runtimeNamespaceBase)
            baseType = analyzer_.visitExpr(*expr.base);

        std::vector<SemanticAnalyzer::Type> argTypes;
        argTypes.reserve(expr.args.size());
        for (auto &arg : expr.args) {
            argTypes.push_back(arg ? analyzer_.visitExpr(*arg) : SemanticAnalyzer::Type::Unknown);
        }

        std::vector<BasicType> runtimeArgTypes;
        runtimeArgTypes.reserve(argTypes.size());
        for (auto ty : argTypes)
            runtimeArgTypes.push_back(semantic_analyzer_detail::basicTypeFromSemanticType(ty));

        std::string runtimeClass;
        if (expr.base) {
            if (auto qname = runtimeClassQNameFromExpr(*expr.base)) {
                if (il::runtime::findRuntimeClassByQName(*qname))
                    runtimeClass = *qname;
            }
            if (runtimeClass.empty()) {
                if (auto inferred = inferObjectClassQName(analyzer_, *expr.base)) {
                    if (il::runtime::findRuntimeClassByQName(*inferred))
                        runtimeClass = *inferred;
                }
            }
            if (runtimeClass.empty() && baseType == SemanticAnalyzer::Type::String)
                runtimeClass = il::runtime::RTCLASS_STRING;
        }

        if (!runtimeClass.empty()) {
            auto info = runtimeMethodIndex().find(runtimeClass, expr.method, runtimeArgTypes);
            if (!info) {
                emitNoSuchMethod(analyzer_.de, runtimeClass, expr.method, expr.loc);
                result_ = SemanticAnalyzer::Type::Unknown;
                return;
            }
            if (!analyzer_.checkRuntimePointerSafety(info->target,
                                                     info->rawPointerReturn,
                                                     info->rawPointerParams,
                                                     expr.args,
                                                     expr.loc,
                                                     expr.method)) {
                result_ = SemanticAnalyzer::Type::Unknown;
                return;
            }
            if (!checkObjectParamArgs(*info, runtimeArgTypes, expr.args, expr.method, expr.loc)) {
                result_ = SemanticAnalyzer::Type::Unknown;
                return;
            }
            result_ = semantic_analyzer_detail::semanticTypeFromBasicType(info->ret);
            return;
        }

        if (expr.base) {
            if (auto className = resolveUserClassQNameFromExpr(analyzer_, *expr.base)) {
                std::vector<::il::frontends::basic::Type> astArgTypes;
                astArgTypes.reserve(argTypes.size());
                for (auto ty : argTypes) {
                    if (auto astTy = astTypeFromSemanticType(ty)) {
                        astArgTypes.push_back(*astTy);
                    } else {
                        astArgTypes.push_back(::il::frontends::basic::Type::I64);
                    }
                }

                auto resolved = sem::resolveMethodOverload(analyzer_.oopIndex(),
                                                           *className,
                                                           expr.method,
                                                           /*isStatic*/ false,
                                                           astArgTypes,
                                                           "",
                                                           nullptr,
                                                           expr.loc);
                if (!resolved) {
                    resolved = sem::resolveMethodOverload(analyzer_.oopIndex(),
                                                          *className,
                                                          expr.method,
                                                          /*isStatic*/ true,
                                                          astArgTypes,
                                                          "",
                                                          nullptr,
                                                          expr.loc);
                }

                if (!resolved) {
                    if (const auto *field =
                            analyzer_.oopIndex().findFieldInHierarchy(*className, expr.method);
                        field && field->isArray) {
                        validateArrayFieldIndexArgs(expr, argTypes);
                        result_ = semantic_analyzer_detail::semanticTypeFromOopField(*field);
                        return;
                    }

                    auto objectInfo = runtimeMethodIndex().find(
                        std::string(il::runtime::RTCLASS_OBJECT), expr.method, runtimeArgTypes);
                    if (objectInfo) {
                        if (!analyzer_.checkRuntimePointerSafety(objectInfo->target,
                                                                 objectInfo->rawPointerReturn,
                                                                 objectInfo->rawPointerParams,
                                                                 expr.args,
                                                                 expr.loc,
                                                                 expr.method)) {
                            result_ = SemanticAnalyzer::Type::Unknown;
                            return;
                        }
                        if (!checkObjectParamArgs(
                                *objectInfo, runtimeArgTypes, expr.args, expr.method, expr.loc)) {
                            result_ = SemanticAnalyzer::Type::Unknown;
                            return;
                        }
                        result_ =
                            semantic_analyzer_detail::semanticTypeFromBasicType(objectInfo->ret);
                    } else {
                        // A method with this name may exist but be inaccessible
                        // (e.g. PRIVATE): defer so the lowering-stage access check
                        // reports the precise visibility diagnostic (B2021) instead
                        // of a misleading "no such method".
                        bool nameExists = false;
                        if (const auto *klass = analyzer_.oopIndex().findClass(*className))
                            nameExists = klass->methods.contains(expr.method);
                        if (!nameExists)
                            emitNoSuchMethod(analyzer_.de, *className, expr.method, expr.loc);
                        result_ = SemanticAnalyzer::Type::Unknown;
                    }
                    return;
                }

                if (!resolved->method->sig.returnClassName.empty()) {
                    result_ = SemanticAnalyzer::Type::Object;
                    return;
                }
                if (resolved->method->sig.returnType) {
                    result_ = astToSemanticType(*resolved->method->sig.returnType);
                    return;
                }
                result_ = SemanticAnalyzer::Type::Unknown;
                return;
            }
        }

        if (baseType == SemanticAnalyzer::Type::Object) {
            auto info = runtimeMethodIndex().find(
                std::string(il::runtime::RTCLASS_OBJECT), expr.method, runtimeArgTypes);
            if (info) {
                if (!analyzer_.checkRuntimePointerSafety(info->target,
                                                         info->rawPointerReturn,
                                                         info->rawPointerParams,
                                                         expr.args,
                                                         expr.loc,
                                                         expr.method)) {
                    result_ = SemanticAnalyzer::Type::Unknown;
                    return;
                }
                if (!checkObjectParamArgs(*info, runtimeArgTypes, expr.args, expr.method, expr.loc)) {
                    result_ = SemanticAnalyzer::Type::Unknown;
                    return;
                }
                result_ = semantic_analyzer_detail::semanticTypeFromBasicType(info->ret);
                return;
            }
        }

        result_ = SemanticAnalyzer::Type::Unknown;
    }

    /// @brief Validates an object/interface `IS` type test.
    /// @details Resolves an unqualified target by walking enclosing namespace
    ///          prefixes, then root, then active imports; qualified names are
    ///          joined as written. Unknown targets emit `B2111`. A resolved
    ///          target with a primitive/array left operand emits `B2121`.
    ///          Result type is boolean even after errors.
    /// @param expr Type-test expression with a required value operand.
    void visit(IsExpr &expr) override {
        // Check left operand type; reject obvious primitives.
        SemanticAnalyzer::Type lhsType = analyzer_.visitExpr(*expr.value);
        /// Classifies scalar and array categories rejected as object operands.
        auto isPrimitive = [&](SemanticAnalyzer::Type t) {
            using T = SemanticAnalyzer::Type;
            return t == T::Int || t == T::Float || t == T::Bool || t == T::String ||
                   t == T::ArrayInt || t == T::ArrayString || t == T::ArrayObject;
        };

        // Resolve right-hand dotted type to class or interface.
        /// Tests an exact qualified name against interface and class indexes.
        auto existsQ = [&](const std::string &q) -> bool {
            if (analyzer_.oopIndex_.interfacesByQname().contains(q))
                return true;
            return analyzer_.oopIndex_.findClass(q) != nullptr;
        };

        std::string dotted;
        if (!expr.typeName.empty()) {
            if (expr.typeName.size() == 1) {
                // Unqualified: resolve with parent-walk then imports.
                std::string ident = Canon(expr.typeName[0]);
                std::vector<std::string> prefix;
                for (const auto &seg : analyzer_.nsStack_)
                    prefix.push_back(Canon(seg));
                std::vector<std::string> hits;
                for (std::size_t n = prefix.size(); n > 0; --n) {
                    std::vector<std::string> parts(prefix.begin(),
                                                   prefix.begin() + static_cast<std::ptrdiff_t>(n));
                    parts.push_back(ident);
                    std::string q = JoinDots(parts);
                    if (existsQ(q))
                        hits.push_back(q);
                }
                if (existsQ(ident))
                    hits.push_back(ident);
                if (hits.size() == 1)
                    dotted = hits[0];
                else if (hits.empty()) {
                    if (!analyzer_.usingStack_.empty()) {
                        const auto &cur = analyzer_.usingStack_.back();
                        for (const auto &ns : cur.imports) {
                            std::string q = ns + "." + ident;
                            if (existsQ(q))
                                hits.push_back(q);
                        }
                    }
                    if (hits.size() == 1)
                        dotted = hits[0];
                }
                if (dotted.empty()) {
                    // Default to single ident (unknown handling occurs below)
                    dotted = ident;
                }
            } else {
                // Qualified: join as-is
                for (size_t i = 0; i < expr.typeName.size(); ++i) {
                    if (i)
                        dotted.push_back('.');
                    dotted += expr.typeName[i];
                }
            }
        }
        bool resolved = existsQ(dotted);

        if (!resolved) {
            std::string msg = std::string("unknown type '") + dotted + "'";
            analyzer_.de.emit(il::support::Severity::Error,
                              "B2111",
                              expr.loc,
                              static_cast<uint32_t>(dotted.size()),
                              std::move(msg));
        } else if (isPrimitive(lhsType)) {
            analyzer_.de.emit(il::support::Severity::Error,
                              "B2121",
                              expr.loc,
                              2,
                              "'IS' requires an object/interface value on the left");
        }

        result_ = SemanticAnalyzer::Type::Bool;
    }

    /// @brief Validates an object/interface `AS` cast.
    /// @details Preserves the operand's semantic type because runtime failure
    ///          yields null rather than changing the static category. An
    ///          unqualified target is resolved through namespace parents, root,
    ///          and imports. A qualified target expands an active alias in its
    ///          first segment. Interface lookup is exact; class lookup receives
    ///          a case-insensitive fallback. Unknown targets emit `B2111`, and
    ///          primitive/array sources emit `E_CAST_INVALID`.
    /// @param expr Cast expression with a required value operand.
    void visit(AsExpr &expr) override {
        // Preserve operand type; runtime returns NULL on failure.
        SemanticAnalyzer::Type lhsType = analyzer_.visitExpr(*expr.value);
        /// Classifies scalar and array categories rejected as cast sources.
        auto isPrimitive = [&](SemanticAnalyzer::Type t) {
            using T = SemanticAnalyzer::Type;
            return t == T::Int || t == T::Float || t == T::Bool || t == T::String ||
                   t == T::ArrayInt || t == T::ArrayString || t == T::ArrayObject;
        };

        std::string dotted;
        if (!expr.typeName.empty()) {
            if (expr.typeName.size() == 1) {
                std::string ident = Canon(expr.typeName[0]);
                std::vector<std::string> prefix;
                for (const auto &seg : analyzer_.nsStack_)
                    prefix.push_back(Canon(seg));
                std::vector<std::string> hits;
                /// Tests an exact qualified name against interface/class indexes.
                auto existsQ = [&](const std::string &q) -> bool {
                    if (analyzer_.oopIndex_.interfacesByQname().contains(q))
                        return true;
                    return analyzer_.oopIndex_.findClass(q) != nullptr;
                };
                for (std::size_t n = prefix.size(); n > 0; --n) {
                    std::vector<std::string> parts(prefix.begin(),
                                                   prefix.begin() + static_cast<std::ptrdiff_t>(n));
                    parts.push_back(ident);
                    std::string q = JoinDots(parts);
                    if (existsQ(q))
                        hits.push_back(q);
                }
                if (existsQ(ident))
                    hits.push_back(ident);
                if (hits.size() == 1)
                    dotted = hits[0];
                else if (hits.empty()) {
                    if (!analyzer_.usingStack_.empty()) {
                        const auto &cur = analyzer_.usingStack_.back();
                        for (const auto &ns : cur.imports) {
                            std::string q = ns + "." + ident;
                            if (existsQ(q))
                                hits.push_back(q);
                        }
                    }
                    if (hits.size() == 1)
                        dotted = hits[0];
                }
                if (dotted.empty())
                    dotted = ident;
            } else {
                // Qualified: apply alias expansion to the first segment, if present.
                std::vector<std::string> segs = expr.typeName;
                if (!analyzer_.usingStack_.empty() && !segs.empty()) {
                    std::string firstCanon = Canon(segs[0]);
                    const auto &aliases = analyzer_.usingStack_.back().aliases;
                    auto it = aliases.find(firstCanon);
                    if (it != aliases.end()) {
                        std::vector<std::string> expanded = SplitDots(it->second);
                        expanded.insert(expanded.end(), segs.begin() + 1, segs.end());
                        segs = std::move(expanded);
                    }
                }
                for (size_t i = 0; i < segs.size(); ++i) {
                    if (i)
                        dotted.push_back('.');
                    dotted += segs[i];
                }
            }
        }
        bool resolved = false;
        if (analyzer_.oopIndex_.interfacesByQname().contains(dotted))
            resolved = true;
        if (!resolved) {
            for (const auto &entry : analyzer_.oopIndex_.classes()) {
                // Case-insensitive comparison since BASIC is case-insensitive
                if (string_utils::iequals(entry.second.qualifiedName, dotted)) {
                    resolved = true;
                    break;
                }
            }
        }

        if (!resolved) {
            std::string msg = std::string("unknown type '") + dotted + "'";
            analyzer_.de.emit(il::support::Severity::Error,
                              "B2111",
                              expr.loc,
                              static_cast<uint32_t>(dotted.size()),
                              std::move(msg));
        } else if (isPrimitive(lhsType)) {
            // E_CAST_INVALID: cannot cast value of type '{From}' to '{To}'.
            std::string from = std::string(semantic_analyzer_detail::semanticTypeName(lhsType));
            std::string to = dotted;
            std::string msg = "cannot cast value of type '" + from + "' to '" + to + "'.";
            analyzer_.de.emit(
                il::support::Severity::Error, "E_CAST_INVALID", expr.loc, 2, std::move(msg));
        }

        result_ = lhsType;
    }

    /// @brief Represents an `ADDRESSOF` result as unknown semantic type.
    /// @details The BASIC semantic enum has no function-pointer category; bridge
    ///          validation recognizes the node structurally where permitted.
    void visit(AddressOfExpr &) override {
        // ADDRESSOF produces a function pointer, which is represented as Unknown
        // in BASIC's type system since it's not a traditional BASIC value type.
        result_ = SemanticAnalyzer::Type::Unknown;
    }

    /// @brief Returns the most recent visit result.
    /// @return Stored semantic type without mutation.
    [[nodiscard]] SemanticAnalyzer::Type result() const noexcept {
        return result_;
    }

  private:
    /// @brief Validates index arguments used through an array-valued field.
    /// @details Float literals are marked for implicit integer conversion and
    ///          emit warning `B2002`; nonliteral floats and other known
    ///          non-integer types emit `B2001`. Unknown types are tolerated.
    ///          Diagnostics use the method-call location.
    /// @param expr Field-as-method call whose arguments represent indices.
    /// @param argTypes Pre-analyzed argument categories.
    void validateArrayFieldIndexArgs(MethodCallExpr &expr,
                                     const std::vector<SemanticAnalyzer::Type> &argTypes) {
        for (std::size_t i = 0; i < expr.args.size(); ++i) {
            auto &arg = expr.args[i];
            if (!arg)
                continue;

            SemanticAnalyzer::Type ty =
                i < argTypes.size() ? argTypes[i] : SemanticAnalyzer::Type::Unknown;
            if (ty == SemanticAnalyzer::Type::Float) {
                if (as<FloatExpr>(*arg)) {
                    analyzer_.insertImplicitCast(*arg, SemanticAnalyzer::Type::Int);
                    std::string msg = "narrowing conversion from FLOAT to INT in array index";
                    analyzer_.de.emit(
                        il::support::Severity::Warning, "B2002", expr.loc, 1, std::move(msg));
                } else {
                    std::string msg = "index type mismatch";
                    analyzer_.de.emit(
                        il::support::Severity::Error, "B2001", expr.loc, 1, std::move(msg));
                }
            } else if (ty != SemanticAnalyzer::Type::Unknown && ty != SemanticAnalyzer::Type::Int) {
                std::string msg = "index type mismatch";
                analyzer_.de.emit(
                    il::support::Severity::Error, "B2001", expr.loc, 1, std::move(msg));
            }
        }
    }

    /// @brief Borrowed shared analyzer state.
    SemanticAnalyzer &analyzer_;

    /// @brief Semantic type produced by the latest override.
    SemanticAnalyzer::Type result_{SemanticAnalyzer::Type::Unknown};
};

/// @brief Resolves and classifies a variable reference.
/// @details Delegates to @ref sem::analyzeVarExpr for scoped name mapping,
///          tracked types, implicit-instance fields, suffix defaults, and
///          undefined-name diagnostics/suggestions.
/// @param v Mutable variable node that may receive a resolved name.
/// @return Inferred semantic type or unknown recovery type.
SemanticAnalyzer::Type SemanticAnalyzer::analyzeVar(VarExpr &v) {
    return sem::analyzeVarExpr(*this, v);
}

/// @brief Applies the shared semantic rule for a unary expression.
/// @param u Unary expression whose operand and operator are validated.
/// @return Type computed by @ref sem::analyzeUnaryExpr, including recovery
///         types after diagnostics.
SemanticAnalyzer::Type SemanticAnalyzer::analyzeUnary(const UnaryExpr &u) {
    return sem::analyzeUnaryExpr(*this, u);
}

/// @brief Applies the shared semantic rule for a binary expression.
/// @param b Binary expression whose operands and operator are validated.
/// @return Type computed by @ref sem::analyzeBinaryExpr, including promotions
///         and recovery behavior.
SemanticAnalyzer::Type SemanticAnalyzer::analyzeBinary(const BinaryExpr &b) {
    return sem::analyzeBinaryExpr(*this, b);
}

/// @brief Resolves a `NEW` target and validates constructor compatibility.
/// @details Mutates a single-segment qualified type into an unqualified name,
///          expands an active alias for qualified names, and resolves
///          unqualified names through namespace parents, the global scope, then
///          imports. Multiple matches emit `B2110`; no match emits `B2111` with
///          at most eight attempted names. Once resolution succeeds, all
///          arguments are visited. Runtime external classes require a catalog
///          constructor (`E_RUNTIME_CLASS_NO_CTOR`). User classes reject
///          abstract construction (`B2106`), enforce exact arity (`B2008`), and
///          validate ByRef array variables and scalar types; integer-to-float is
///          accepted. This analyzer currently returns unknown for every outcome,
///          including valid construction.
/// @param expr Mutable construction node whose class name/qualified segments
///             may be normalized to declared spelling.
/// @return Always @ref Type::Unknown; diagnostics distinguish success/recovery.
/// @note Resolution failures occur before argument visitation.
SemanticAnalyzer::Type SemanticAnalyzer::analyzeNew(NewExpr &expr) {
    // Helper: map canonical qualified name to declared-case name in OOP index.
    /// Maps canonical qualified spelling back to the class index's declared case.
    auto mapCanonicalToDeclared = [&](const std::string &qcanon) -> std::string {
        if (const ClassInfo *ci = oopIndex_.findClass(qcanon))
            return ci->qualifiedName;
        /// Canonicalizes every segment of one dotted name.
        auto toCanonQ = [](const std::string &q) -> std::string {
            std::vector<std::string> segs = SplitDots(q);
            return CanonJoin(segs);
        };
        std::string want = toCanonQ(qcanon);
        for (const auto &kv : oopIndex_.classes()) {
            const std::string &decl = kv.second.qualifiedName;
            if (toCanonQ(decl) == want)
                return decl;
        }
        return qcanon;
    };

    // Treat a single qualified segment as an unqualified type name so USING and
    // parent-walk resolution apply.
    if (expr.qualifiedType.size() == 1) {
        expr.className = expr.qualifiedType.front();
        expr.qualifiedType.clear();
    }

    // Apply alias expansion for qualified type, if present.
    if (!expr.qualifiedType.empty() && !usingStack_.empty()) {
        std::string firstCanon = Canon(expr.qualifiedType[0]);
        const auto &aliases = usingStack_.back().aliases;
        auto it = aliases.find(firstCanon);
        if (it != aliases.end()) {
            std::vector<std::string> expanded = SplitDots(it->second);
            expanded.insert(
                expanded.end(), expr.qualifiedType.begin() + 1, expr.qualifiedType.end());
            // Rebuild className from canonicalized expanded segments.
            expr.className = mapCanonicalToDeclared(CanonJoin(expanded));
        }
    }

    // If unqualified type, resolve using parent-walk then USING imports.
    if (expr.qualifiedType.empty()) {
        std::vector<std::string> attempts;
        /// Tests a canonical candidate against class and interface indexes.
        auto existsQ = [&](const std::string &q) -> bool {
            std::string decl = mapCanonicalToDeclared(q);
            if (oopIndex_.interfacesByQname().contains(decl))
                return true;
            return oopIndex_.findClass(decl) != nullptr;
        };

        std::string ident = Canon(expr.className);
        // Parent-walk: A.B.Point -> A.Point -> Point
        std::vector<std::string> prefixCanon;
        prefixCanon.reserve(nsStack_.size());
        for (const auto &seg : nsStack_) {
            std::string cseg = Canon(seg);
            prefixCanon.push_back(cseg.empty() ? seg : cseg);
        }
        std::vector<std::string> hits;
        for (std::size_t n = prefixCanon.size(); n > 0; --n) {
            std::vector<std::string> parts(prefixCanon.begin(),
                                           prefixCanon.begin() + static_cast<std::ptrdiff_t>(n));
            parts.push_back(ident);
            std::string q = JoinDots(parts);
            attempts.push_back(q);
            if (existsQ(q))
                hits.push_back(q);
        }
        // Try global
        attempts.push_back(ident);
        if (existsQ(ident))
            hits.push_back(ident);

        /// Emits deterministic candidate text for an ambiguous type lookup.
        auto reportAmbiguous = [&](const std::vector<std::string> &cands) {
            std::vector<std::string> sorted = cands;
            std::sort(sorted.begin(), sorted.end());
            std::string msg = std::string("ambiguous type '") + expr.className + "' (candidates: ";
            for (size_t i = 0; i < sorted.size(); ++i) {
                if (i)
                    msg += ", ";
                msg += sorted[i];
            }
            msg += ")";
            de.emit(il::support::Severity::Error,
                    "B2110",
                    expr.loc,
                    static_cast<uint32_t>(expr.className.size()),
                    std::move(msg));
        };

        if (hits.size() == 1) {
            expr.className = mapCanonicalToDeclared(hits[0]);
        } else if (hits.size() > 1) {
            reportAmbiguous(hits);
            return Type::Unknown;
        } else {
            // Try USING imports
            std::vector<std::string> importHits;
            if (!usingStack_.empty()) {
                const UsingScope &cur = usingStack_.back();
                for (const auto &ns : cur.imports) {
                    std::string q = ns;
                    if (!q.empty())
                        q.push_back('.');
                    q += ident;
                    attempts.push_back(q);
                    if (existsQ(q))
                        importHits.push_back(q);
                }
            }
            if (importHits.size() == 1) {
                expr.className = mapCanonicalToDeclared(importHits[0]);
            } else if (importHits.size() > 1) {
                reportAmbiguous(importHits);
                return Type::Unknown;
            } else {
                // Unknown type with tried list
                std::string msg = std::string("unknown type '") + expr.className + "'";
                if (!attempts.empty()) {
                    msg += " (tried: ";
                    size_t limit = std::min<std::size_t>(attempts.size(), 8);
                    for (size_t i = 0; i < limit; ++i) {
                        if (i)
                            msg += ", ";
                        msg += attempts[i];
                    }
                    if (attempts.size() > limit) {
                        msg += ", +" + std::to_string(attempts.size() - limit) + " more";
                    }
                    msg += ")";
                }
                de.emit(il::support::Severity::Error,
                        "B2111",
                        expr.loc,
                        static_cast<uint32_t>(expr.className.size()),
                        std::move(msg));
                return Type::Unknown;
            }
        }
    }

    std::vector<Type> argTypes;
    argTypes.reserve(expr.args.size());
    for (auto &arg : expr.args)
        argTypes.push_back(arg ? visitExpr(*arg) : Type::Unknown);

    // Allow NEW for runtime classes defined in the catalog when a ctor helper exists.
    {
        auto &tyreg = runtimeTypeRegistry();
        if (tyreg.kindOf(expr.className) == TypeKind::BuiltinExternalType) {
            if (const auto *found = il::runtime::findRuntimeClassByQName(expr.className)) {
                if (!found->ctor || std::string(found->ctor).empty()) {
                    std::string msg = "runtime class '" + expr.className + "' has no constructor";
                    de.emit(il::support::Severity::Error,
                            "E_RUNTIME_CLASS_NO_CTOR",
                            expr.loc,
                            static_cast<uint32_t>(expr.className.size()),
                            std::move(msg));
                    return Type::Unknown;
                }
                // Accept; arguments already visited above; return Unknown semantic type.
                return Type::Unknown;
            }
        }
    }

    const ClassInfo *klass = oopIndex_.findClass(expr.className);
    if (!klass)
        return Type::Unknown;

    // Instantiation of abstract classes is not allowed.
    if (klass->isAbstract) {
        std::string msg = "cannot instantiate abstract class '" + expr.className + "'";
        de.emit(il::support::Severity::Error,
                "B2106",
                expr.loc,
                static_cast<uint32_t>(expr.className.size()),
                std::move(msg));
        // Still analyze arguments for nested diagnostics, but return Unknown.
        return Type::Unknown;
    }

    const std::size_t expectedCount = klass->ctorParams.size();
    if (expr.args.size() != expectedCount) {
        std::string msg = "constructor for '" + expr.className + "' expects " +
                          std::to_string(expectedCount) + " argument" +
                          (expectedCount == 1 ? "" : "s") + ", got " +
                          std::to_string(expr.args.size());
        de.emit(il::support::Severity::Error,
                "B2008",
                expr.loc,
                static_cast<uint32_t>(expr.className.size()),
                std::move(msg));
        return Type::Unknown;
    }

    for (std::size_t i = 0; i < expectedCount; ++i) {
        const auto &param = klass->ctorParams[i];
        const Expr *argExpr = expr.args[i].get();
        Type argTy = argTypes[i];

        if (param.isArray) {
            const auto *var = as<const VarExpr>(*argExpr);
            if (!var || !arrays_.contains(var->name)) {
                il::support::SourceLoc loc = argExpr ? argExpr->loc : expr.loc;
                std::string msg = "constructor argument " + std::to_string(i + 1) + " for '" +
                                  expr.className + "' must be an array variable (ByRef)";
                de.emit(il::support::Severity::Error, "B2006", loc, 1, std::move(msg));
            }
            continue;
        }

        auto expectTy = param.type;
        if (expectTy == ::il::frontends::basic::Type::F64 && argTy == Type::Int)
            continue;

        Type want = astToSemanticType(expectTy);
        if (argTy != Type::Unknown && argTy != want) {
            il::support::SourceLoc loc = argExpr ? argExpr->loc : expr.loc;
            std::string msg = "constructor argument type mismatch for '" + expr.className + "'";
            de.emit(il::support::Severity::Error, "B2001", loc, 1, std::move(msg));
        }
    }

    return Type::Unknown;
}

/// @brief Records or replaces an expression's lowering-time conversion.
/// @details Uses the borrowed AST node address as the map key. The AST must
///          therefore remain alive and at the same address while lowering
///          consumes analyzer metadata.
/// @param expr Expression whose address identifies the conversion site.
/// @param targetType Destination semantic type.
/// @post @ref implicitConversions_ maps `&expr` to @p targetType.
void SemanticAnalyzer::markImplicitConversion(const Expr &expr, Type targetType) {
    implicitConversions_[&expr] = targetType;
}

/// @brief Requests a lowering-time implicit cast without rewriting the AST.
/// @details Returns early when the identical target is already recorded;
///          otherwise delegates to @ref markImplicitConversion, replacing any
///          different prior target.
/// @param expr Mutable expression retained as a stable-address metadata key.
/// @param target Destination semantic type.
void SemanticAnalyzer::insertImplicitCast(Expr &expr, Type target) {
    auto it = implicitConversions_.find(&expr);
    if (it != implicitConversions_.end() && it->second == target)
        return;
    markImplicitConversion(expr, target);
}

/// @brief Validates and classifies an array element access.
/// @details Delegates to @ref sem::analyzeArrayExpr for symbol/shape lookup,
///          index count/type checking, narrowing metadata, and constant-bound
///          diagnostics.
/// @param a Mutable array expression whose name/indices may be annotated.
/// @return Element semantic type or unknown recovery type.
SemanticAnalyzer::Type SemanticAnalyzer::analyzeArray(ArrayExpr &a) {
    return sem::analyzeArrayExpr(*this, a);
}

/// @brief Validates and classifies an `LBOUND` query.
/// @param expr Mutable bound-query expression passed to
///             @ref sem::analyzeLBoundExpr.
/// @return Integer for a valid query or unknown after validation failure.
SemanticAnalyzer::Type SemanticAnalyzer::analyzeLBound(LBoundExpr &expr) {
    return sem::analyzeLBoundExpr(*this, expr);
}

/// @brief Validates and classifies a `UBOUND` query.
/// @param expr Mutable bound-query expression passed to
///             @ref sem::analyzeUBoundExpr.
/// @return Integer for a valid query or unknown after validation failure.
SemanticAnalyzer::Type SemanticAnalyzer::analyzeUBound(UBoundExpr &expr) {
    return sem::analyzeUBoundExpr(*this, expr);
}

/// @brief Dispatches one expression through the mutable semantic visitor.
/// @param e Root expression; visitor overrides may resolve names, record
///          conversions, or mutate normalized type names.
/// @return Type stored by the selected visitor override.
SemanticAnalyzer::Type SemanticAnalyzer::visitExpr(Expr &e) {
    SemanticAnalyzerExprVisitor visitor(*this);
    e.accept(visitor);
    return visitor.result();
}

} // namespace il::frontends::basic
