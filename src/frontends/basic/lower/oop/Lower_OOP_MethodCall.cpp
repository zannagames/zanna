//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/lower/oop/Lower_OOP_MethodCall.cpp
// Purpose: Lower BASIC OOP method calls and virtual dispatch operations.
// Key invariants: Method calls use vtable for virtual dispatch; property
//                 accessors follow get_/set_ naming conventions.
// Ownership/Lifetime: Operates on Lowerer state without owning AST or module.
// Links: docs/internals/codemap.md
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements static, runtime-catalog, virtual, interface, and direct
///        BASIC method-call lowering.

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/ILTypeUtils.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/NameMangler_OOP.hpp"
#include "frontends/basic/OopIndex.hpp"
#include "frontends/basic/OopLoweringContext.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/lower/oop/Lower_OOP_Internal.hpp"
#include "frontends/basic/sem/OverloadResolution.hpp"
#include "frontends/basic/sem/RuntimeMethodIndex.hpp"
#include "il/runtime/RuntimeClassNames.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <string>
#include <vector>

namespace il::frontends::basic {

namespace {

/// @brief True if @p target is an HTTP(S) server route-registration method (Get/Post/Put/Delete).
/// @param target Canonical runtime method name.
/// @return True for supported HTTP or HTTPS route-registration methods.
bool isHttpServerRouteTarget(const std::string &target) {
    return target == "Zanna.Network.HttpServer.Get" || target == "Zanna.Network.HttpServer.Post" ||
           target == "Zanna.Network.HttpServer.Put" ||
           target == "Zanna.Network.HttpServer.Delete" ||
           target == "Zanna.Network.HttpsServer.Get" ||
           target == "Zanna.Network.HttpsServer.Post" ||
           target == "Zanna.Network.HttpsServer.Put" ||
           target == "Zanna.Network.HttpsServer.Delete";
}

/// @brief True if a procedure signature matches the HTTP handler shape `void(ptr, ptr)`.
/// @param sig Candidate procedure signature; may be nullptr.
/// @return True when @p sig accepts two pointers and returns void.
bool isValidHttpHandlerSignature(const ::il::frontends::basic::ProcedureSignature *sig) {
    return sig && sig->retType.kind == il::core::Type::Kind::Void && sig->paramTypes.size() == 2 &&
           sig->paramTypes[0].kind == il::core::Type::Kind::Ptr &&
           sig->paramTypes[1].kind == il::core::Type::Kind::Ptr;
}

/// @brief Resolve an HTTP handler tag (a procedure name) to its lowered callee symbol.
/// @param lowerer Lowerer supplying procedure signatures and callee resolution.
/// @param tag BASIC procedure name stored in the handler tag.
/// @return The callee name if the named procedure has a valid handler signature; else "".
std::string resolveHttpHandlerTarget(const Lowerer &lowerer, const std::string &tag) {
    const auto *sig = lowerer.findProcSignature(tag);
    if (!isValidHttpHandlerSignature(sig))
        return {};
    return lowerer.resolveCalleeName(tag);
}

/// @brief Select the runtime BindHandler target (HTTP vs HTTPS) for a route-registration target.
/// @param target Canonical route-registration runtime method.
/// @return Process-lifetime canonical BindHandler function name.
const char *httpServerBindHandlerTarget(const std::string &target) {
    if (target.rfind("Zanna.Network.HttpsServer.", 0) == 0)
        return "Zanna.Network.HttpsServer.BindHandler";
    return "Zanna.Network.HttpServer.BindHandler";
}

/// @brief Map a Lowerer::ExprType to the corresponding runtime BasicType.
/// @param ty Scanned expression category to translate.
/// @return Matching runtime overload category, or `BasicType::Unknown`.
BasicType basicTypeFromExprType(Lowerer::ExprType ty) {
    switch (ty) {
        case Lowerer::ExprType::F64:
            return BasicType::Float;
        case Lowerer::ExprType::Str:
            return BasicType::String;
        case Lowerer::ExprType::Bool:
            return BasicType::Bool;
        case Lowerer::ExprType::Obj:
            return BasicType::Object;
        case Lowerer::ExprType::I64:
        default:
            return BasicType::Int;
    }
}

/// @brief Map a Lowerer::ExprType to the corresponding BASIC AST Type (objects map to I64).
::il::frontends::basic::Type astTypeFromExprType(Lowerer::ExprType ty) {
    switch (ty) {
        case Lowerer::ExprType::F64:
            return ::il::frontends::basic::Type::F64;
        case Lowerer::ExprType::Str:
            return ::il::frontends::basic::Type::Str;
        case Lowerer::ExprType::Bool:
            return ::il::frontends::basic::Type::Bool;
        case Lowerer::ExprType::Obj:
        case Lowerer::ExprType::I64:
        default:
            return ::il::frontends::basic::Type::I64;
    }
}

} // namespace

/// @brief Compute the runtime BasicType of each argument of a method call.
/// @param expr The method-call expression.
/// @return One BasicType per argument (Unknown for a null argument), used for runtime-catalog
///         overload lookup.
std::vector<BasicType> Lowerer::methodCallRuntimeArgTypes(const MethodCallExpr &expr) {
    std::vector<BasicType> result;
    result.reserve(expr.args.size());
    for (const auto &arg : expr.args)
        result.push_back(arg ? basicTypeFromExprType(scanExpr(*arg)) : BasicType::Unknown);
    return result;
}

/// @brief Compute the BASIC AST Type of each argument of a method call.
/// @param expr The method-call expression.
/// @return One AST Type per argument (I64 for a null argument), used for overload resolution.
std::vector<::il::frontends::basic::Type> Lowerer::methodCallAstArgTypes(
    const MethodCallExpr &expr) {
    std::vector<::il::frontends::basic::Type> result;
    result.reserve(expr.args.size());
    for (const auto &arg : expr.args)
        result.push_back(arg ? astTypeFromExprType(scanExpr(*arg))
                             : ::il::frontends::basic::Type::I64);
    return result;
}

/// @brief Try to lower a method call as a static call (`Class.Method(...)`).
/// @param expr The method-call expression.
/// @return The lowered result, or nullopt when the base is not a static class reference (so the
///         caller falls through to instance dispatch).
/// @details Only applies when the base is a bare class name with no shadowing variable. Handles
///          user classes (overload-resolved, argument-coerced, object/scalar return types) and
///          static calls on runtime-catalog classes (reporting E_NO_SUCH_METHOD with candidates).
std::optional<Lowerer::RVal> Lowerer::tryLowerStaticMethodCall(const MethodCallExpr &expr) {
    // Static method calls: Class.Method(...)
    if (const auto *vb = as<const VarExpr>(*expr.base)) {
        // If a symbol with this name exists (local/param/global), treat as instance, not static.
        // Module-level variables do not have slots; rely on symbol presence alone.
        if (const auto *sym = findSymbol(vb->name); sym) {
            // fall through to instance path below
        } else {
            std::string qname = resolveQualifiedClassCasing(qualify(vb->name));
            if (const ClassInfo *ci = oopIndex_.findClass(qname)) {
                // Overload resolution for static call
                std::vector<::il::frontends::basic::Type> argAstTypes = methodCallAstArgTypes(expr);
                std::string selected = expr.method;
                if (auto resolved = sem::resolveMethodOverload(oopIndex_,
                                                               qname,
                                                               expr.method,
                                                               /*isStatic*/ true,
                                                               argAstTypes,
                                                               currentClass(),
                                                               diagnosticEmitter(),
                                                               expr.loc)) {
                    selected = resolved->methodName;
                }
                std::vector<::il::frontends::basic::Type> expectParamAst;
                if (auto it = ci->methods.find(selected); it != ci->methods.end())
                    expectParamAst = it->second.sig.paramTypes;

                std::vector<Value> args;
                args.reserve(expr.args.size());
                for (std::size_t i = 0; i < expr.args.size(); ++i) {
                    RVal lowered = lowerExpr(*expr.args[i]);
                    if (i < expectParamAst.size()) {
                        auto astTy = expectParamAst[i];
                        if (astTy == ::il::frontends::basic::Type::Bool)
                            lowered = coerceToBool(std::move(lowered), expr.loc);
                        else if (astTy == ::il::frontends::basic::Type::F64)
                            lowered = coerceToF64(std::move(lowered), expr.loc);
                        else if (astTy == ::il::frontends::basic::Type::I64)
                            lowered = coerceToI64(std::move(lowered), expr.loc);
                    }
                    args.push_back(lowered.value);
                }

                std::string callee = mangleMethod(ci->qualifiedName, selected);
                // BUG-CARDS-010 fix: Check for object-returning methods first
                std::string retClassName =
                    findMethodReturnClassName(qname, selected, expr.args.size());
                if (!retClassName.empty()) {
                    // Method returns a custom class type - use ptr
                    Type ilRetTy(Type::Kind::Ptr);
                    Value result = emitCallRet(ilRetTy, callee, args);
                    deferReleaseObj(result, retClassName);
                    return RVal{result, ilRetTy};
                }
                if (auto retType = findMethodReturnType(qname, selected)) {
                    Type ilRetTy = type_conv::astToIlType(*retType);
                    Value result = emitCallRet(ilRetTy, callee, args);
                    if (ilRetTy.kind == Type::Kind::Str)
                        deferReleaseStr(result);
                    else if (ilRetTy.kind == Type::Kind::Ptr)
                        deferReleaseObj(result, qname);
                    return RVal{result, ilRetTy};
                }
                emitCall(callee, args);
                return RVal{Value::constInt(0), Type(Type::Kind::I64)};
            } else {
                // Static call on a runtime class from the catalog (no receiver)
                if (il::runtime::findRuntimeClassByQName(qname)) {
                    auto &midx = runtimeMethodIndex();
                    auto info = midx.find(qname, expr.method, methodCallRuntimeArgTypes(expr));
                    if (info && info->hasReceiver)
                        info = std::nullopt;
                    if (!info) {
                        if (auto *em = diagnosticEmitter()) {
                            auto cands = midx.candidates(qname, expr.method);
                            std::string msg =
                                "no such method '" + expr.method + "' on '" + qname + "'";
                            if (!cands.empty()) {
                                msg += "; candidates: ";
                                for (size_t i = 0; i < cands.size(); ++i) {
                                    if (i)
                                        msg += ", ";
                                    msg += cands[i];
                                }
                            }
                            em->emit(il::support::Severity::Error,
                                     "E_NO_SUCH_METHOD",
                                     expr.loc,
                                     static_cast<uint32_t>(expr.method.size()),
                                     std::move(msg));
                        }
                        return RVal{Value::constInt(0), Type(Type::Kind::I64)};
                    }
                    std::vector<Value> args;
                    args.reserve(expr.args.size());
                    for (size_t i = 0; i < expr.args.size(); ++i) {
                        const auto &a = expr.args[i];
                        RVal av = lowerExpr(*a);
                        BasicType expect = (i < info->args.size()) ? info->args[i] : BasicType::Int;
                        if (expect == BasicType::Bool)
                            av = coerceToBool(std::move(av), expr.loc);
                        else if (expect == BasicType::Float)
                            av = coerceToF64(std::move(av), expr.loc);
                        else if (expect == BasicType::Int)
                            av = coerceToI64(std::move(av), expr.loc);
                        args.push_back(av.value);
                    }
                    Type retTy(type_conv::basicTypeToIlKind(info->ret));
                    runtimeTracker.trackCalleeName(info->target);
                    curLoc = expr.loc;
                    Value result = retTy.kind == Type::Kind::Void
                                       ? (emitCall(info->target, args), Value::constInt(0))
                                       : emitCallRet(retTy, info->target, args);
                    if (retTy.kind == Type::Kind::Str)
                        deferReleaseStr(result);
                    else if (retTy.kind == Type::Kind::Ptr)
                        deferReleaseObj(result);
                    return RVal{result,
                                retTy.kind == Type::Kind::Void ? Type(Type::Kind::I64) : retTy};
                }
            }
        }
    }
    return std::nullopt;
}

/// @brief Lower a method-call expression (`base.method(args)`) to IL.
/// @param expr The method-call expression.
/// @return The call result and its IL type.
/// @details Resolution order: static call (tryLowerStaticMethodCall); runtime-catalog instance
///          method (with HTTP route → BindHandler special-casing); `Zanna.Core.Object` fallback
///          (ToString/Equals) when the user class does not override; interface dispatch for
///          `(x AS IFace).m()` via itable lookup; virtual dispatch via the object's method table
///          slot; and finally a direct mangled call. Applies private-access checks, overload
///          resolution (using the declaring class for inherited methods), argument coercion, and
///          return-value release scheduling. BASE-qualified calls force direct dispatch to the
///          base class using the `ME` receiver.
Lowerer::RVal Lowerer::lowerMethodCallExpr(const MethodCallExpr &expr) {
    if (!expr.base)
        return {Value::constInt(0), Type(Type::Kind::I64)};

    if (auto staticResult = tryLowerStaticMethodCall(expr))
        return *staticResult;

    // Runtime class method calls via catalog (e.g., Zanna.String)
    {
        // Determine runtime class qname
        std::string qClass;
        {
            std::string cls = resolveObjectClass(*expr.base);
            if (!cls.empty())
                qClass = qualify(cls);
        }
        if (qClass.empty()) {
            if (scanExpr(*expr.base) == ExprType::Str)
                qClass = std::string(il::runtime::RTCLASS_STRING);
        }
        // Only consult the runtime method catalog for true runtime classes
        if (!qClass.empty() && il::runtime::findRuntimeClassByQName(qClass)) {
            auto &midx = runtimeMethodIndex();
            auto info = midx.find(qClass, expr.method, methodCallRuntimeArgTypes(expr));
            if (info && !info->hasReceiver)
                info = std::nullopt;
            if (!info) {
                if (auto *em = diagnosticEmitter()) {
                    auto cands = midx.candidates(qClass, expr.method);
                    std::string msg = "no such method '" + expr.method + "' on '" + qClass + "'";
                    if (!cands.empty()) {
                        msg += "; candidates: ";
                        for (size_t i = 0; i < cands.size(); ++i) {
                            if (i)
                                msg += ", ";
                            msg += cands[i];
                        }
                    }
                    em->emit(il::support::Severity::Error,
                             "E_NO_SUCH_METHOD",
                             expr.loc,
                             static_cast<uint32_t>(expr.method.size()),
                             std::move(msg));
                }
                return {Value::constInt(0), Type(Type::Kind::I64)};
            }
            // Lower base and build (receiver, args...)
            RVal base = lowerExpr(*expr.base);
            std::vector<Value> args;
            args.reserve(1 + expr.args.size());
            args.push_back(base.value);

            // Coerce each user arg to expected BasicType
            for (size_t i = 0; i < expr.args.size(); ++i) {
                RVal av = lowerExpr(*expr.args[i]);
                BasicType expect = (i < info->args.size()) ? info->args[i] : BasicType::Int;
                if (expect == BasicType::Bool)
                    av = coerceToBool(std::move(av), expr.loc);
                else if (expect == BasicType::Float)
                    av = coerceToF64(std::move(av), expr.loc);
                else if (expect == BasicType::Int)
                    av = coerceToI64(std::move(av), expr.loc);
                args.push_back(av.value);
            }
            // Declare extern with receiver + arg types
            std::vector<Type> paramTypes;
            paramTypes.reserve(1 + info->args.size());
            // Receiver: strings use str; others default to ptr
            paramTypes.push_back(qClass == il::runtime::RTCLASS_STRING ? Type(Type::Kind::Str)
                                                                       : Type(Type::Kind::Ptr));
            for (BasicType bt : info->args)
                paramTypes.push_back(Type(type_conv::basicTypeToIlKind(bt)));

            Type retTy(type_conv::basicTypeToIlKind(info->ret));
            // Record the catalog target spelling (e.g., Zanna.String.Substring)
            // so extern declarations can include the accessor alongside
            // canonical function names selected at call sites.
            runtimeTracker.trackCalleeName(info->target);
            if (isHttpServerRouteTarget(info->target) && expr.args.size() == 2 &&
                args.size() >= 3) {
                if (const auto *tagExpr = as<const StringExpr>(*expr.args[1])) {
                    std::string handlerTarget = resolveHttpHandlerTarget(*this, tagExpr->value);
                    if (!handlerTarget.empty()) {
                        const char *bindHandlerTarget = httpServerBindHandlerTarget(info->target);
                        runtimeTracker.trackCalleeName(bindHandlerTarget);
                        emitCall(bindHandlerTarget,
                                 {args[0], args[2], Value::global(handlerTarget)});
                    }
                }
            }
            curLoc = expr.loc;
            Value result = retTy.kind == Type::Kind::Void
                               ? (emitCall(info->target, args), Value::constInt(0))
                               : emitCallRet(retTy, info->target, args);
            if (retTy.kind == Type::Kind::Str)
                deferReleaseStr(result);
            else if (retTy.kind == Type::Kind::Ptr)
                deferReleaseObj(result);
            return {result, retTy.kind == Type::Kind::Void ? Type(Type::Kind::I64) : retTy};
        }

        // Fallback: Object methods on any instance (Zanna.Core.Object.*, System alias supported)
        // BUT only if the user-defined class doesn't override the method.
        {
            // First check if the user-defined class has this method - if so, skip the
            // Zanna.Core.Object fallback and let the user-defined method handling below take over.
            bool userClassHasMethod = false;
            if (!qClass.empty()) {
                if (oopIndex_.findMethodInHierarchy(qClass, expr.method))
                    userClassHasMethod = true;
            }

            auto &midx = runtimeMethodIndex();
            auto info = midx.find(std::string(il::runtime::RTCLASS_OBJECT),
                                  expr.method,
                                  methodCallRuntimeArgTypes(expr));
            if (info && !info->hasReceiver)
                info = std::nullopt;
            if (info && !userClassHasMethod) {
                // Lower base and build (receiver, args...)
                RVal base = lowerExpr(*expr.base);
                std::vector<Value> args;
                args.reserve(1 + expr.args.size());
                args.push_back(base.value);
                for (size_t i = 0; i < expr.args.size(); ++i) {
                    RVal av = lowerExpr(*expr.args[i]);
                    args.push_back(av.value);
                }
                // Receiver is ptr; args are passed as-is; ret type from info
                Type retTy(type_conv::basicTypeToIlKind(info->ret));
                runtimeTracker.trackCalleeName(info->target);
                curLoc = expr.loc;
                Value result = retTy.kind == Type::Kind::Void
                                   ? (emitCall(info->target, args), Value::constInt(0))
                                   : emitCallRet(retTy, info->target, args);
                if (retTy.kind == Type::Kind::Str)
                    deferReleaseStr(result);
                else if (retTy.kind == Type::Kind::Ptr)
                    deferReleaseObj(result);
                return {result, retTy.kind == Type::Kind::Void ? Type(Type::Kind::I64) : retTy};
            }
            // As a last resort, special-case common Object methods to canonical targets
            // (only if user class doesn't override)
            if (!userClassHasMethod && string_utils::iequals(expr.method, "ToString") &&
                expr.args.size() == 0) {
                curLoc = expr.loc;
                RVal base = lowerExpr(*expr.base);
                runtimeTracker.trackCalleeName(std::string(il::runtime::RTCLASS_OBJECT) +
                                               ".ToString");
                Value result = emitCallRet(Type(Type::Kind::Str),
                                           std::string(il::runtime::RTCLASS_OBJECT) + ".ToString",
                                           {base.value});
                deferReleaseStr(result);
                return {result, Type(Type::Kind::Str)};
            }
            if (!userClassHasMethod && string_utils::iequals(expr.method, "Equals") &&
                expr.args.size() == 1) {
                curLoc = expr.loc;
                RVal base = lowerExpr(*expr.base);
                RVal rhs = lowerExpr(*expr.args[0]);
                runtimeTracker.trackCalleeName(std::string(il::runtime::RTCLASS_OBJECT) +
                                               ".Equals");
                Value result = emitCallRet(Type(Type::Kind::I1),
                                           std::string(il::runtime::RTCLASS_OBJECT) + ".Equals",
                                           {base.value, rhs.value});
                return {result, Type(Type::Kind::I1)};
            }
        }
    }

    std::string className = resolveObjectClass(*expr.base);
    // Compute the instance (self) argument. For BASE-qualified calls, use ME.
    Value selfArg;
    if (const auto *v = as<const VarExpr>(*expr.base); v && v->name == "BASE") {
        const auto *sym = findSymbol("ME");
        if (sym && sym->slotId) {
            curLoc = expr.loc;
            selfArg = emitLoad(Type(Type::Kind::Ptr), Value::temp(*sym->slotId));
        } else {
            selfArg = Value::null();
        }
    } else {
        RVal base = lowerExpr(*expr.base);
        selfArg = base.value;
    }
    // Access control for methods: Private may only be called within the declaring class.
    if (!className.empty()) {
        std::string qname = qualify(className);
        if (const ClassInfo *cinfo = oopIndex_.findClass(qname)) {
            auto it = cinfo->methods.find(expr.method);
            if (it != cinfo->methods.end() && it->second.sig.access == Access::Private &&
                currentClass() != cinfo->qualifiedName) {
                if (auto *em = diagnosticEmitter()) {
                    std::string msg = "cannot access private member '" + expr.method +
                                      "' of class '" + cinfo->qualifiedName + "'";
                    em->emit(il::support::Severity::Error,
                             "B2021",
                             expr.loc,
                             static_cast<uint32_t>(expr.method.size()),
                             std::move(msg));
                } else {
                    std::fprintf(stderr,
                                 "B2021: cannot access private member '%s' of class '%s'\n",
                                 expr.method.c_str(),
                                 qname.c_str());
                }
                return {Value::constInt(0), Type(Type::Kind::I64)};
            }
        }
    }

    curLoc = expr.loc;
    const std::string qname = qualify(className);

    // Get expected parameter types for type coercion
    std::vector<::il::frontends::basic::Type> expectParamAst;
    if (!qname.empty() && !expr.args.empty()) {
        if (const ClassInfo *ci = oopIndex_.findClass(qname)) {
            auto it = ci->methods.find(expr.method);
            if (it != ci->methods.end())
                expectParamAst = it->second.sig.paramTypes;
        }
    }

    // Lower arguments ONCE and apply coercions as needed
    // Bug #021 fix: previously arguments were lowered twice when coercion was needed,
    // causing side effects (like function calls) to execute twice.
    std::vector<Value> args;
    args.reserve(expr.args.size() + 1);
    args.push_back(selfArg);
    for (std::size_t i = 0; i < expr.args.size(); ++i) {
        const auto &arg = expr.args[i];
        if (!arg)
            continue;
        RVal lowered = lowerExpr(*arg);

        // Apply type coercion if parameter type is known
        if (i < expectParamAst.size()) {
            auto astTy = expectParamAst[i];
            if (astTy == ::il::frontends::basic::Type::Bool)
                lowered = coerceToBool(std::move(lowered), expr.loc);
            else if (astTy == ::il::frontends::basic::Type::F64)
                lowered = coerceToF64(std::move(lowered), expr.loc);
            else if (astTy == ::il::frontends::basic::Type::I64)
                lowered = coerceToI64(std::move(lowered), expr.loc);
        }
        args.push_back(lowered.value);
    }

    // Detect BASE-qualified calls conservatively: treat `BASE` as a direct call cue.
    bool baseQualified = false;
    if (const auto *v = as<const VarExpr>(*expr.base))
        baseQualified = (v->name == "BASE");

    // Determine if the target is virtual via OOP index.
    int slot = -1;
    if (!qname.empty())
        slot = getVirtualSlot(oopIndex_, qname, expr.method);

    // Determine the target class for direct dispatch. For BASE-qualified calls,
    // we must resolve to the immediate base of the current lowering class.
    std::string directQClass = qname;
    if (baseQualified) {
        const std::string cur = currentClass();
        if (!cur.empty()) {
            if (const ClassInfo *ci = oopIndex_.findClass(cur)) {
                if (!ci->baseQualified.empty())
                    directQClass = ci->baseQualified;
            }
        }
    }

    // Resolve overload to select the best callee among same-name methods.
    // Build argument AST types (excluding implicit self).
    std::vector<::il::frontends::basic::Type> argAstTypes = methodCallAstArgTypes(expr);

    std::string qc = qname.empty() ? directQClass : qname;
    std::string curClass = currentClass();
    std::string selectedName = expr.method;
    // BUG-OOP-002/003 fix: Track declaring class for inherited methods
    std::string declaringClass = qc;
    if (!qc.empty()) {
        if (auto resolved = sem::resolveMethodOverload(oopIndex_,
                                                       qc,
                                                       expr.method,
                                                       false,
                                                       argAstTypes,
                                                       curClass,
                                                       diagnosticEmitter(),
                                                       expr.loc)) {
            selectedName = resolved->methodName;
            declaringClass = resolved->qualifiedClass; // Use declaring class for dispatch
        } else if (diagnosticEmitter()) {
            return {Value::constInt(0), Type(Type::Kind::I64)};
        }
    }
    // BUG-OOP-002/003 fix: Use declaring class for mangling, not receiver class
    std::string emitClassName = declaringClass;
    if (!declaringClass.empty()) {
        if (const ClassInfo *ci = oopIndex_.findClass(declaringClass))
            emitClassName = ci->qualifiedName;
    }
    std::string directCallee =
        emitClassName.empty() ? selectedName : mangleMethod(emitClassName, selectedName);

    // If virtual and not BASE-qualified, emit call.indirect; otherwise direct call or interface
    // dispatch. Interface dispatch via (expr AS IFACE).Method: detect AS with interface target.
    /// @brief Attempts interface-slot dispatch for an `AS`-qualified receiver.
    /// @return Lowered result when interface dispatch applies; otherwise `std::nullopt`.
    auto tryInterfaceDispatch = [&]() -> std::optional<RVal> {
        const AsExpr *asBase = as<const AsExpr>(*expr.base);
        if (!asBase)
            return std::nullopt;
        // Build dotted name for interface and locate InterfaceInfo
        std::string dotted;
        for (size_t i = 0; i < asBase->typeName.size(); ++i) {
            if (i)
                dotted.push_back('.');
            dotted += asBase->typeName[i];
        }
        const InterfaceInfo *iface = nullptr;
        for (const auto &p : oopIndex_.interfacesByQname()) {
            if (p.first == dotted) {
                iface = &p.second;
                break;
            }
        }
        if (!iface)
            return std::nullopt;
        // Recover slot index by name match (and simple arity check when possible)
        int slotIndex = -1;
        std::size_t userArity = expr.args.size();
        for (std::size_t idx = 0; idx < iface->slots.size(); ++idx) {
            const auto &sig = iface->slots[idx];
            if (sig.name != expr.method)
                continue;
            if (sig.paramTypes.size() == userArity) {
                slotIndex = static_cast<int>(idx);
                break;
            }
            // Fallback: pick first name match when arity differs (best-effort)
            if (slotIndex < 0)
                slotIndex = static_cast<int>(idx);
        }
        if (slotIndex < 0)
            return std::nullopt;

        // Lookup itable, load function pointer at slot, and call.indirect
        // Ensure runtime extern is declared for itable lookup
        if (builder) {
            if (const auto *desc = il::runtime::findRuntimeDescriptor("rt_itable_lookup"))
                builder->addExtern(
                    std::string(desc->name), desc->signature.retType, desc->signature.paramTypes);
            else
                builder->addExtern("rt_itable_lookup",
                                   Type(Type::Kind::Ptr),
                                   {Type(Type::Kind::Ptr), Type(Type::Kind::I64)});
        }
        Value itable = emitCallRet(
            Type(Type::Kind::Ptr), "rt_itable_lookup", {selfArg, Value::constInt(iface->ifaceId)});
        const long long offset = static_cast<long long>(slotIndex * 8ULL);
        Value entryPtr =
            emitBinary(Opcode::GEP, Type(Type::Kind::Ptr), itable, Value::constInt(offset));
        Value fnPtr = emitLoad(Type(Type::Kind::Ptr), entryPtr);

        // Determine return type from interface signature when available.
        Type retTy = Type(Type::Kind::Void);
        if (slotIndex >= 0 && static_cast<std::size_t>(slotIndex) < iface->slots.size()) {
            if (iface->slots[static_cast<std::size_t>(slotIndex)].returnType) {
                retTy = type_conv::astToIlType(
                    *iface->slots[static_cast<std::size_t>(slotIndex)].returnType);
            }
        }

        if (retTy.kind != Type(Type::Kind::Void).kind) {
            Value result = emitCallIndirectRet(retTy, fnPtr, args);
            if (retTy.kind == Type::Kind::Str)
                deferReleaseStr(result);
            else if (retTy.kind == Type::Kind::Ptr && !className.empty())
                deferReleaseObj(result, className);
            return RVal{result, retTy};
        }
        emitCallIndirect(fnPtr, args);
        return RVal{Value::constInt(0), Type(Type::Kind::I64)};
    };

    if (auto dispatched = tryInterfaceDispatch())
        return *dispatched;

    // If virtual and not BASE-qualified, attempt dynamic dispatch by reading a per-object
    // method pointer table address from a module-level binding when available. As a
    // conservative fallback, construct the pointer from the object's class slot table by
    // loading the function address at 'slot' from a contiguous array starting at the
    // indirect callee pointer. This preserves correct behaviour for projects that populate
    // per-class tables in module init.
    if (slot >= 0 && !baseQualified) {
        // Pointer-based table lookup: treat operand 0 as a pointer to the table base, then GEP.
        // Load the callee-table pointer from the object (projects may store a table pointer
        // at offset 0). If unavailable, this yields null and the indirect call path below
        // will trap with a clear message.
        Value tablePtr = emitLoad(Type(Type::Kind::Ptr), selfArg);
        const long long offset = static_cast<long long>(slot * 8LL);
        Value entryPtr =
            emitBinary(Opcode::GEP, Type(Type::Kind::Ptr), tablePtr, Value::constInt(offset));
        Value fnPtr = emitLoad(Type(Type::Kind::Ptr), entryPtr);

        // BUG-OOP-003 fix: Use declaring class for return type lookup in virtual dispatch
        // BUG-CARDS-010 fix: Check for object-returning methods first
        std::string retClassName =
            findMethodReturnClassName(declaringClass, selectedName, expr.args.size());
        if (!retClassName.empty()) {
            Type ilRetTy(Type::Kind::Ptr);
            Value result = emitCallIndirectRet(ilRetTy, fnPtr, args);
            deferReleaseObj(result, retClassName);
            return {result, ilRetTy};
        }
        if (auto retType = findMethodReturnType(declaringClass, selectedName)) {
            Type ilRetTy = type_conv::astToIlType(*retType);
            Value result = emitCallIndirectRet(ilRetTy, fnPtr, args);
            if (ilRetTy.kind == Type::Kind::Str)
                deferReleaseStr(result);
            else if (ilRetTy.kind == Type::Kind::Ptr && !className.empty())
                deferReleaseObj(result, className);
            return {result, ilRetTy};
        }
        emitCallIndirect(fnPtr, args);
        return {Value::constInt(0), Type(Type::Kind::I64)};
    }

    // Direct call path.
    // For BASE-qualified direct calls, consult the resolved base class for return type.
    // BUG-OOP-002/003 fix: Use declaring class for return type lookup
    const std::string retClassLookup = baseQualified ? directQClass : declaringClass;
    // BUG-CARDS-010 fix: Check for object-returning methods first
    std::string retClassName =
        findMethodReturnClassName(retClassLookup, selectedName, expr.args.size());
    if (!retClassName.empty()) {
        Type ilRetTy(Type::Kind::Ptr);
        Value result = emitCallRet(ilRetTy, directCallee, args);
        deferReleaseObj(result, retClassName);
        return {result, ilRetTy};
    }
    if (auto retType = findMethodReturnType(retClassLookup, selectedName)) {
        Type ilRetTy = type_conv::astToIlType(*retType);
        Value result = emitCallRet(ilRetTy, directCallee, args);
        if (ilRetTy.kind == Type::Kind::Str)
            deferReleaseStr(result);
        else if (ilRetTy.kind == Type::Kind::Ptr && !className.empty())
            deferReleaseObj(result, className);
        return {result, ilRetTy};
    }
    emitCall(directCallee, args);
    return {Value::constInt(0), Type(Type::Kind::I64)};
}

// -------------------------------------------------------------------------
// OopLoweringContext-aware implementations
// -------------------------------------------------------------------------

/// @brief Context-aware overload of lowerMethodCallExpr() that pre-warms the class-info cache.
/// @details Pre-caches the receiver's class info in @p ctx (accelerating access-control and
///          overload resolution), then delegates to the single-argument overload.
/// @param expr Method call whose receiver metadata is cached and lowered.
/// @param ctx OOP metadata cache to pre-warm.
/// @return Lowered call result and IL type.
Lowerer::RVal Lowerer::lowerMethodCallExpr(const MethodCallExpr &expr, OopLoweringContext &ctx) {
    // Pre-cache class info for method dispatch target.
    // This accelerates access control and overload resolution.
    if (expr.base) {
        std::string cls = resolveObjectClass(*expr.base);
        if (!cls.empty())
            (void)ctx.findClassInfo(qualify(cls));
    }
    return lowerMethodCallExpr(expr);
}

} // namespace il::frontends::basic
