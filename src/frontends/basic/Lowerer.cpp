//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Lowerer.cpp
// Purpose: Provide the orchestration entry points that drive BASIC to IL
//          lowering. The implementation wires together the specialised helper
//          translation units while keeping this TU as a lightweight dispatcher.
// Key invariants: Sub-components remain singletons owned by the Lowerer
//                 instance and are re-used across procedure invocations.
// Ownership/Lifetime: Owns lowering/emitter facades; temporarily borrows the
//                     destination builder/module and caller-owned AST nodes.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md
//
//===----------------------------------------------------------------------===//

/**
 * @file Lowerer.cpp
 * @brief Implements BASIC lowering orchestration and OOP type-query helpers.
 *
 * The central Lowerer owns specialized lowering facades and transient
 * bookkeeping, borrows AST inputs, and returns completed IL modules by value.
 * This unit also implements namespace qualification and metadata queries used
 * when lowering fields and method results.
 */

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/BasicTypes.hpp"
#include "frontends/basic/ControlStatementLowerer.hpp"
#include "frontends/basic/IoStatementLowerer.hpp"
#include "frontends/basic/LoweringPipeline.hpp"
#include "frontends/basic/OopLoweringContext.hpp"
#include "frontends/basic/RuntimeStatementLowerer.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/TypeSuffix.hpp"
#include "frontends/basic/lower/Emitter.hpp"
#include "frontends/basic/sem/RuntimeMethodIndex.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <algorithm>
#include <cctype>

using il::core::Type;
using il::frontends::basic::BasicType;

namespace {

/// @brief Select an IL return type from a BASIC hint and legacy name suffix.
/// @details Explicit String, Float, Int, Bool, and Void hints take precedence.
///          For other hints, a trailing `$` selects `str`, `#` selects `f64`,
///          and every remaining spelling defaults to `i64`.
/// @param fnName Source-level function name, possibly carrying a type suffix.
/// @param hint Semantic return-type hint.
/// @return IL type used for the emitted function signature.
Type ilTypeForBasicRet(const std::string &fnName, BasicType hint) {
    using K = Type::Kind;
    if (hint == BasicType::String)
        return Type(K::Str);
    if (hint == BasicType::Float)
        return Type(K::F64);
    if (hint == BasicType::Int)
        return Type(K::I64);
    if (hint == BasicType::Bool)
        return Type(K::I1);
    if (hint == BasicType::Void)
        return Type(K::Void);
    if (!fnName.empty()) {
        char c = fnName.back();
        if (c == '$')
            return Type(K::Str);
        if (c == '#')
            return Type(K::F64);
    }
    return Type(K::I64);
}

} // namespace

namespace il::frontends::basic {

/// @brief Map a function name and semantic hint to its emitted IL return type.
/// @param fnName Source-level function name, possibly carrying `$` or `#`.
/// @param hint Semantic BASIC return category.
/// @return Type selected by the shared hint/suffix mapping.
Lowerer::Type Lowerer::functionRetTypeFromHint(const std::string &fnName, BasicType hint) const {
    return ilTypeForBasicRet(fnName, hint);
}

/// @brief Append namespace segments to the active qualification stack.
/// @param path Ordered segments to append; an empty path leaves the stack unchanged.
/// @post Existing segments retain their order and @p path forms the new suffix.
void Lowerer::pushNamespace(const std::vector<std::string> &path) {
    nsStack_.insert(nsStack_.end(), path.begin(), path.end());
}

/// @brief Remove trailing segments from the active namespace stack.
/// @details Zero and an empty stack are no-ops; an excessive count is clamped
///          so the operation can empty but never underflow the stack.
/// @param count Requested number of trailing segments to remove.
void Lowerer::popNamespace(std::size_t count) {
    if (count == 0 || nsStack_.empty())
        return;
    if (count > nsStack_.size())
        count = nsStack_.size();
    nsStack_.erase(nsStack_.end() - static_cast<std::ptrdiff_t>(count), nsStack_.end());
}

/// @brief Qualify an unqualified class name with the active namespace.
/// @details Empty names, already dotted names, and names observed with an empty
///          stack are returned unchanged. Otherwise stack segments and @p klass
///          are joined with dots.
/// @param klass Candidate class name.
/// @return Qualified or unchanged name, preserving all input casing.
std::string Lowerer::qualify(const std::string &klass) const {
    if (klass.empty())
        return klass;

    if (klass.find('.') != std::string::npos)
        return klass;

    if (nsStack_.empty())
        return klass;

    // Qualify with current namespace: currentNs + "." + klass
    std::string out;
    // Compute final size to reserve capacity conservatively.
    std::size_t size = klass.size();
    for (const auto &s : nsStack_)
        size += s.size() + 1; // segment + dot
    out.reserve(size);
    for (std::size_t i = 0; i < nsStack_.size(); ++i) {
        if (i)
            out.push_back('.');
        out.append(nsStack_[i]);
    }
    out.push_back('.');
    out.append(klass);
    return out;
}

/// @brief Find the AST return type recorded for a user-defined method.
/// @details Looks up the class and method in the OOP index. An explicit
///          signature return type wins; otherwise a recognized method-name
///          suffix supplies the type. Runtime classes are not consulted.
/// @param className Qualified user-defined class name.
/// @param methodName Method spelling used as the method-map key.
/// @return Recorded or suffix-inferred AST type, otherwise `std::nullopt`.
std::optional<::il::frontends::basic::Type> Lowerer::findMethodReturnType(
    std::string_view className, std::string_view methodName) const {
    if (className.empty())
        return std::nullopt;

    const ClassInfo *info = oopIndex_.findClass(std::string(className));
    if (!info)
        return std::nullopt;

    auto it = info->methods.find(std::string(methodName));
    if (it == info->methods.end())
        return std::nullopt;
    if (it->second.sig.returnType)
        return it->second.sig.returnType;

    if (auto suffixType = inferAstTypeFromSuffix(methodName))
        return suffixType;

    return std::nullopt;
}

/// @brief Infer the concrete class returned by an object-valued method.
/// @details User-defined metadata is checked first. Runtime metadata then uses
///          the optional arity, explicit return-class annotations, cross-class
///          target prefixes, and parsed signatures. As a fallback, object
///          methods on runtime types without Push, Set, or Enqueue are treated
///          as returning their receiver class; collection-like results remain
///          unknown.
/// @param className Qualified receiver class name.
/// @param methodName Method name, compared case-insensitively for runtime classes.
/// @param arity Optional argument count used to disambiguate overloads.
/// @return Qualified concrete result class, or an empty string when unknown.
std::string Lowerer::findMethodReturnClassName(std::string_view className,
                                               std::string_view methodName,
                                               std::optional<std::size_t> arity) const {
    if (className.empty())
        return {};

    // Check user-defined classes first
    const ClassInfo *info = oopIndex_.findClass(std::string(className));
    if (info) {
        auto it = info->methods.find(std::string(methodName));
        if (it != info->methods.end() && !it->second.sig.returnClassName.empty())
            return it->second.sig.returnClassName;
    }

    // Check runtime classes: if a method returns 'obj' and belongs to a known
    // runtime class, infer the class name from the method's target function.
    if (const auto *rtClass = il::runtime::findRuntimeClassByQName(std::string(className))) {
        if (arity) {
            if (auto entry = runtimeMethodIndex().find(className, methodName, *arity)) {
                if (!entry->returnClassQName.empty())
                    return entry->returnClassQName;

                if (entry->ret == BasicType::Object && !entry->target.empty()) {
                    std::string_view target(entry->target);
                    auto lastDot = target.rfind('.');
                    if (lastDot != std::string_view::npos) {
                        std::string prefix(target.substr(0, lastDot));
                        if (!string_utils::iequals(prefix, className) &&
                            il::runtime::findRuntimeClassByQName(prefix)) {
                            return prefix;
                        }
                    }
                }
            }
        }

        for (const auto &m : rtClass->methods) {
            if (m.name && string_utils::iequals(m.name, methodName) && m.signature) {
                auto sig = il::runtime::parseRuntimeSignature(m.signature);
                if (arity && sig.arity() != *arity)
                    continue;
                if (sig.returnType == il::runtime::ILScalarType::Object) {
                    if (std::string concrete = il::runtime::concreteRuntimeReturnClassQName(sig);
                        !concrete.empty()) {
                        return concrete;
                    }

                    // Method returns obj — check if target function belongs to
                    // a DIFFERENT runtime class (cross-class factory method).
                    if (m.target) {
                        std::string_view target(m.target);
                        auto lastDot = target.rfind('.');
                        if (lastDot != std::string_view::npos) {
                            std::string prefix(target.substr(0, lastDot));
                            if (!string_utils::iequals(prefix, className)) {
                                if (il::runtime::findRuntimeClassByQName(prefix))
                                    return prefix;
                            }
                        }
                    }
                    // Target belongs to the same class. Distinguish between:
                    // - Value/math types (Vec2/Vec3/Quat) where obj-returning
                    //   methods return the same type
                    // - Collection types (List/Seq/Map/Queue/Stack/etc.) where
                    //   getters return generic stored elements
                    // Collections have a "Get" or "Push" method — check for this.
                    {
                        bool isCollection = false;
                        for (const auto &cm : rtClass->methods) {
                            if (cm.name && (std::string_view(cm.name) == "Push" ||
                                            std::string_view(cm.name) == "Set" ||
                                            std::string_view(cm.name) == "Enqueue")) {
                                isCollection = true;
                                break;
                            }
                        }
                        if (!isCollection)
                            return std::string(className);
                    }
                    return {};
                }
                break;
            }
        }
    }

    return {};
}

/// @brief Query a user-defined field's AST type.
/// @param className Qualified class name; an empty name cannot resolve.
/// @param fieldName Field spelling, matched through the OOP index.
/// @return Field type when present, otherwise `std::nullopt`.
std::optional<::il::frontends::basic::Type> Lowerer::findFieldType(
    std::string_view className, std::string_view fieldName) const {
    if (className.empty())
        return std::nullopt;

    // Use the OopIndex API for case-insensitive field lookup
    const auto *field = oopIndex_.findField(std::string(className), fieldName);
    if (!field)
        return std::nullopt;

    return field->type;
}

/// @brief Test whether a user-defined field is declared as an array.
/// @param className Qualified class name; an empty name returns false.
/// @param fieldName Field spelling, matched through the OOP index.
/// @return The field's array flag, or false when the class/field is absent.
bool Lowerer::isFieldArray(std::string_view className, std::string_view fieldName) const {
    if (className.empty())
        return false;

    // Use the OopIndex API for case-insensitive field lookup
    const auto *field = oopIndex_.findField(std::string(className), fieldName);
    if (!field)
        return false;

    return field->isArray;
}

/// @brief Construct a lowering driver composed of specialised helper stages.
/// @details Instantiates the program-, procedure-, and statement-level lowering
///          helpers together with the IL emitter facade.  The @p boundsChecks
///          flag is threaded through to downstream helpers so they can reserve
///          additional metadata (for example, array bounds slots) when runtime
///          checking is enabled.
/// @param boundsChecks Enables generation of defensive array bounds logic when true.
Lowerer::Lowerer(bool boundsChecks)
    : programLowering(std::make_unique<ProgramLowering>(*this)),
      procedureLowering(std::make_unique<ProcedureLowering>(*this)),
      statementLowering(std::make_unique<StatementLowering>(*this)),
      symbols(symbolTable_.raw()), // Legacy alias for backward compatibility
      boundsChecks(boundsChecks), emitter_(std::make_unique<lower::Emitter>(*this)),
      coercionEngine_(std::make_unique<TypeCoercionEngine>(*this)),
      ioStmtLowerer_(std::make_unique<IoStatementLowerer>(*this)),
      ctrlStmtLowerer_(std::make_unique<ControlStatementLowerer>(*this)),
      runtimeStmtLowerer_(std::make_unique<RuntimeStatementLowerer>(*this)) {}

/// @brief Access the owned type-coercion service.
/// @return Non-null service reference valid for this Lowerer's lifetime.
TypeCoercionEngine &Lowerer::coercion() noexcept {
    return *coercionEngine_;
}

/// @brief Default destructor to anchor unique_ptr members in the translation unit.
/// @details The helpers own caches and refer back to the parent @ref Lowerer, so
///          providing the destructor here keeps ownership consistent even when
///          compiled as part of a shared library.
Lowerer::~Lowerer() = default;

/// @brief Lower an entire BASIC program into IL.
/// @details Constructs an empty @ref Module, delegates to the program-level
///          lowering helper to populate it, and returns the finished module by
///          value.  Per-procedure state is reset by the helper as each routine is
///          emitted, so the @ref Lowerer instance can be reused for subsequent
///          compilation phases.
/// @param prog Parsed BASIC program to translate.
/// @return Populated IL module representing @p prog.
Lowerer::Module Lowerer::lowerProgram(const Program &prog) {
    Module module;
    programLowering->run(prog, module);
    return module;
}

/// @brief Convenience wrapper that lowers an entire program.
/// @details Present to keep the legacy API surface intact.  Forwarding to
///          @ref lowerProgram allows embedders to keep calling @ref lower while
///          the implementation details reside in a single code path.
/// @param prog Parsed BASIC program to translate.
/// @return Populated IL module representing @p prog.
Lowerer::Module Lowerer::lower(const Program &prog) {
    return lowerProgram(prog);
}

/// @brief Lower a single procedure into IL using the configured helpers.
/// @details Materialises a lowering context, recomputes procedure metadata,
///          allocates required basic blocks, and finally emits IL for the
///          procedure body.  The @p config callbacks allow callers to inject
///          custom prologue/epilogue behaviour (for example, specialised return
///          handling) without reimplementing the lowering pipeline.
/// @param name Procedure identifier.
/// @param params Parameter declarations describing the procedure signature.
/// @param body Ordered statement list comprising the procedure body.
/// @param config Behavioural hooks that customise prologue/epilogue emission.
void Lowerer::lowerProcedure(const std::string &name,
                             const std::vector<Param> &params,
                             const std::vector<StmtPtr> &body,
                             const ProcedureConfig &config) {
    auto ctx = procedureLowering->makeContext(name, params, body, config);
    procedureLowering->resetContext(ctx);
    procedureLowering->collectProcedureInfo(ctx);
    procedureLowering->scheduleBlocks(ctx);
    procedureLowering->emitProcedureIL(ctx);
}

} // namespace il::frontends::basic
