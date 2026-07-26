//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/lower/oop/Lower_OOP_MemberAccess.cpp
// Purpose: Lower BASIC OOP field and property access operations.
// Key invariants: Field access respects recorded offsets; nullable receivers
//                 are handled with appropriate runtime checks.
// Ownership/Lifetime: Operates on Lowerer state without owning AST or module.
// Links: docs/internals/codemap.md
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements ME, field, property, enum, static-member, and implicit
///        member-access lowering for BASIC objects.

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
#include "frontends/basic/sem/RuntimePropertyIndex.hpp"
#include "il/runtime/RuntimeClassNames.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <optional>
#include <string>
#include <vector>

namespace il::frontends::basic {

/// @brief Lower the implicit @c ME expression to a pointer load.
///
/// @details Looks up the @c ME symbol in the current scope, falling back to a
///          null pointer when the binding is absent (for example, outside a
///          method).  When present the helper emits a load from the associated
///          slot so callers receive a runtime object pointer.
///
/// @param expr AST node representing the @c ME keyword.
/// @return Runtime value describing the current object instance.
Lowerer::RVal Lowerer::lowerMeExpr(const MeExpr &expr) {
    curLoc = expr.loc;
    const auto *sym = findSymbol("ME");
    if (!sym || !sym->slotId)
        return {Value::null(), Type(Type::Kind::Ptr)};
    Value slot = Value::temp(*sym->slotId);
    Value self = emitLoad(Type(Type::Kind::Ptr), slot);
    return {self, Type(Type::Kind::Ptr)};
}

/// @brief Lower a member access expression to loads from the object layout.
///
/// @details Evaluates the base expression, consults the cached class layout for
///          the member, and emits a @c GEP followed by a load using the field's
///          static type.  When any prerequisite (base, layout, or field) is
///          missing, the helper returns a zero-valued integer as a defensive
///          fallback.
///
/// @param expr AST node describing the member access.
/// @return Runtime value of the selected field, or zero when unresolved.
std::optional<Lowerer::MemberFieldAccess> Lowerer::resolveMemberField(
    const MemberAccessExpr &expr) {
    if (!expr.base)
        return std::nullopt;

    // Only resolve member fields for instance receivers. If the base does not
    // represent an object (e.g., a class name in static access), bail out early
    // so callers can apply property sugar or static-field logic without forcing
    // a load of the base expression (which may not have storage).
    std::string className;
    if (const auto *vbase = as<const VarExpr>(*expr.base))
        className = getSlotType(vbase->name).objectClass;
    if (className.empty())
        className = resolveObjectClass(*expr.base);
    if (className.empty())
        return std::nullopt;

    RVal base = lowerExpr(*expr.base);
    // Access control for fields: Private may only be accessed within the declaring class.
    if (!className.empty()) {
        std::string qname = qualify(className);
        if (const ClassInfo *cinfo = oopIndex_.findClass(qname)) {
            for (const auto &f : cinfo->fields) {
                if (f.name == expr.member) {
                    if (f.access == Access::Private && currentClass() != cinfo->qualifiedName) {
                        if (auto *em = diagnosticEmitter()) {
                            std::string msg = "cannot access private member '" + expr.member +
                                              "' of class '" + cinfo->qualifiedName + "'";
                            em->emit(il::support::Severity::Error,
                                     "B2021",
                                     expr.loc,
                                     static_cast<uint32_t>(expr.member.size()),
                                     std::move(msg));
                        } else {
                            std::fprintf(stderr,
                                         "B2021: cannot access private member '%s' of class '%s'\n",
                                         expr.member.c_str(),
                                         qname.c_str());
                        }
                        return std::nullopt;
                    }
                    break;
                }
            }
        }
    }
    const ClassLayout *layout = findClassLayout(className);
    if (!layout)
        return std::nullopt;

    const ClassLayout::Field *field = layout->findField(expr.member);
    if (!field)
        return std::nullopt;

    curLoc = expr.loc;
    Value fieldPtr = emitBinary(Opcode::GEP,
                                Type(Type::Kind::Ptr),
                                base.value,
                                Value::constInt(static_cast<long long>(field->offset)));
    // BUG-082 fix: Object fields are pointers, not I64
    Type fieldTy = !field->objectClassName.empty() ? Type(Type::Kind::Ptr)
                                                   : type_conv::astToIlType(field->type);
    MemberFieldAccess access;
    access.ptr = fieldPtr;
    access.ilType = fieldTy;
    access.astType = field->type;
    access.objectClassName = field->objectClassName; // BUG-082 fix
    return access;
}

/// @brief Resolve an unqualified name as an implicit field of the current instance (`ME.name`).
/// @param name Field name.
/// @param loc Source location for emitted IL.
/// @return The field access (pointer, IL/AST type, object class), or nullopt if there is no
///         active field scope, no such field, or no `ME` receiver in scope.
/// @details Loads the `ME` self pointer and GEPs to the field offset; object fields are typed
///          as pointers (BUG-082).
std::optional<Lowerer::MemberFieldAccess> Lowerer::resolveImplicitField(
    std::string_view name, il::support::SourceLoc loc) {
    const FieldScope *scope = activeFieldScope();
    if (!scope || !scope->layout)
        return std::nullopt;

    const auto *field = scope->layout->findField(name);
    if (!field)
        return std::nullopt;

    const auto *selfInfo = findSymbol("ME");
    if (!selfInfo || !selfInfo->slotId)
        return std::nullopt;

    curLoc = loc;
    Value selfPtr = emitLoad(Type(Type::Kind::Ptr), Value::temp(*selfInfo->slotId));
    curLoc = loc;
    Value fieldPtr = emitBinary(Opcode::GEP,
                                Type(Type::Kind::Ptr),
                                selfPtr,
                                Value::constInt(static_cast<long long>(field->offset)));
    MemberFieldAccess access;
    access.ptr = fieldPtr;
    // BUG-082 fix: Object fields are pointers, not I64
    access.ilType = !field->objectClassName.empty() ? Type(Type::Kind::Ptr)
                                                    : type_conv::astToIlType(field->type);
    access.astType = field->type;
    access.objectClassName = field->objectClassName; // BUG-082 fix
    return access;
}

/// @brief Lower a member-access expression (`base.member`) to a value.
/// @param expr The member-access expression.
/// @return The accessed value and its IL type.
/// @details Resolution order: enum variant constant (`Enum.Variant`); instance field load via
///          resolveMemberField(); runtime-class property getter; instance property getter sugar
///          (`get_member`); static property getter or static field load on a class name. Falls
///          back to a null/zero value when nothing matches.
Lowerer::RVal Lowerer::lowerMemberAccessExpr(const MemberAccessExpr &expr) {
    // Check for enum variant access: Color.Red → ConstInt(I64, value)
    if (expr.base && expr.base->kind() == Expr::Kind::Var) {
        const auto &varExpr = static_cast<const VarExpr &>(*expr.base);
        auto enumVal = oopIndex_.findEnumVariant(varExpr.name, expr.member);
        if (enumVal.has_value()) {
            curLoc = expr.loc;
            return {Value::constInt(static_cast<long long>(*enumVal)), Type(Type::Kind::I64)};
        }
    }

    auto access = resolveMemberField(expr);
    if (!access) {
        if (expr.base) {
            if (auto qClass = runtimeClassQNameFrom(*expr.base)) {
                if (il::runtime::findRuntimeClassByQName(*qClass)) {
                    auto prop = runtimePropertyIndex().find(*qClass, expr.member);
                    if (prop) {
                        Type retTy = type_conv::runtimeScalarToType(prop->type);
                        runtimeTracker.trackCalleeName(prop->getter);
                        curLoc = expr.loc;
                        Value result = emitCallRet(retTy, prop->getter, {});
                        if (retTy.kind == Type::Kind::Str)
                            deferReleaseStr(result);
                        return {result, retTy};
                    }
                }
            }
        }

        // Runtime class property (e.g., Zanna.String) getter sugar via catalog
        if (expr.base) {
            // Lower base to inspect IL kind and to pass as arg0
            RVal base = lowerExpr(*expr.base);
            // Detect STRING alias → Zanna.String
            std::string qClass;
            // Prefer object class resolution when available
            {
                std::string cls = resolveObjectClass(*expr.base);
                if (!cls.empty())
                    qClass = qualify(cls);
            }
            if (qClass.empty() && base.type.kind == Type::Kind::Str)
                qClass = std::string(il::runtime::RTCLASS_STRING);

            // Only use runtime property catalog for known runtime classes
            if (!qClass.empty() && il::runtime::findRuntimeClassByQName(qClass)) {
                auto prop = runtimePropertyIndex().find(qClass, expr.member);
                if (prop) {
                    // Map scalar token to IL type
                    Type retTy = type_conv::runtimeScalarToType(prop->type);
                    // Record the property getter spelling so extern declarations
                    // can include the accessor alongside canonical function names.
                    runtimeTracker.trackCalleeName(prop->getter);
                    curLoc = expr.loc;
                    Value result = emitCallRet(retTy, prop->getter, {base.value});
                    if (retTy.kind == Type::Kind::Str)
                        deferReleaseStr(result);
                    return {result, retTy};
                } else if (auto *em = diagnosticEmitter()) {
                    std::string msg = "no such property '" + expr.member + "' on '" + qClass + "'";
                    em->emit(il::support::Severity::Error,
                             "E_PROP_NO_SUCH_PROPERTY",
                             expr.loc,
                             static_cast<uint32_t>(expr.member.size()),
                             std::move(msg));
                    return {Value::constInt(0), Type(Type::Kind::I64)};
                }
            }
        }
        // Fallbacks:
        // 1) Instance property getter sugar: base.member -> call get_member(base)
        // 2) Static property getter sugar:  Class.member -> call Class.get_member()
        // 3) Static field access:           Class.field  -> load @Class::field

        // 1) Instance property sugar
        if (expr.base) {
            std::string instClass = resolveObjectClass(*expr.base);
            if (!instClass.empty()) {
                std::string qname = qualify(instClass);
                std::string getter = std::string("get_") + expr.member;
                // Overload resolution for property getter (0 user params)
                std::string curClass = currentClass();
                if (auto resolved = sem::resolveMethodOverload(oopIndex_,
                                                               qname,
                                                               expr.member,
                                                               /*isStatic*/ false,
                                                               /*args*/ {},
                                                               curClass,
                                                               diagnosticEmitter(),
                                                               expr.loc)) {
                    getter = resolved->methodName;
                } else if (diagnosticEmitter()) {
                    return {Value::constInt(0), Type(Type::Kind::I64)};
                }
                std::string callee = mangleMethod(qname, getter);
                RVal base = lowerExpr(*expr.base);
                std::vector<Value> args;
                args.push_back(base.value);
                Type retTy = Type(Type::Kind::I64);
                if (auto rt = findMethodReturnType(qname, getter))
                    retTy = type_conv::astToIlType(*rt);
                Value result = (retTy.kind == Type(Type::Kind::Void).kind)
                                   ? (emitCall(callee, args), Value::constInt(0))
                                   : emitCallRet(retTy, callee, args);
                return {result, retTy};
            }

            // 2/3) Static property or static field on a class name
            if (const auto *v = as<const VarExpr>(*expr.base)) {
                // If a symbol with this name exists (local/param/global), it's not a static access
                // Module-level symbols may not have a slot yet; presence in the symbol table is
                // sufficient to classify this as an instance access.
                if (const auto *sym = findSymbol(v->name); sym) {
                    return {Value::null(), Type(Type::Kind::Ptr)};
                }
                // Attempt to resolve the class by current namespace context
                std::string qname = resolveQualifiedClassCasing(qualify(v->name));
                if (const ClassInfo *ci = oopIndex_.findClass(qname)) {
                    // Prefer property getter sugar when present (resolve overloads)
                    std::string getter = std::string("get_") + expr.member;
                    if (auto resolved = sem::resolveMethodOverload(oopIndex_,
                                                                   qname,
                                                                   expr.member,
                                                                   /*isStatic*/ true,
                                                                   /*args*/ {},
                                                                   currentClass(),
                                                                   diagnosticEmitter(),
                                                                   expr.loc))
                        getter = resolved->methodName;
                    else if (diagnosticEmitter())
                        return {Value::constInt(0), Type(Type::Kind::I64)};
                    auto it = ci->methods.find(getter);
                    if (it != ci->methods.end() && it->second.isStatic) {
                        Type retTy = Type(Type::Kind::I64);
                        if (auto rt = findMethodReturnType(qname, getter))
                            retTy = type_conv::astToIlType(*rt);
                        std::string callee = mangleMethod(ci->qualifiedName, getter);
                        Value result = (retTy.kind == Type(Type::Kind::Void).kind)
                                           ? (emitCall(callee, {}), Value::constInt(0))
                                           : emitCallRet(retTy, callee, {});
                        return {result, retTy};
                    }

                    // Otherwise, try a static field load
                    for (const auto &sf : ci->staticFields) {
                        if (sf.name == expr.member) {
                            Type ilTy = sf.objectClassName.empty() ? type_conv::astToIlType(sf.type)
                                                                   : Type(Type::Kind::Ptr);
                            curLoc = expr.loc;
                            std::string gname = ci->qualifiedName + "::" + expr.member;
                            Value addr = emitUnary(
                                Opcode::AddrOf, Type(Type::Kind::Ptr), Value::global(gname));
                            Value loaded = emitLoad(ilTy, addr);
                            return {loaded, ilTy};
                        }
                    }
                }
            }
        }

        return {Value::null(), Type(Type::Kind::Ptr)};
    }

    curLoc = expr.loc;
    Value loaded = emitLoad(access->ilType, access->ptr);
    return {loaded, access->ilType};
}

// -------------------------------------------------------------------------
// OopLoweringContext-aware implementations
// -------------------------------------------------------------------------

/// @brief Context-aware overload of lowerMeExpr(); @p ctx is unused (no caching benefit).
/// @param expr ME expression delegated to the active lowering state.
/// @param ctx Explicit OOP context retained for interface symmetry.
/// @return Lowered implicit-instance value and pointer type.
Lowerer::RVal Lowerer::lowerMeExpr(const MeExpr &expr, OopLoweringContext &ctx) {
    // ME resolution is simple slot lookup - no class caching benefits.
    (void)ctx;
    return lowerMeExpr(expr);
}

/// @brief Context-aware overload of lowerMemberAccessExpr() that pre-warms the class-info cache.
/// @details Pre-caches the base object's class info in @p ctx (accelerating access-control
///          checks), then delegates to the single-argument overload.
/// @param expr Member access whose receiver class is cached and lowered.
/// @param ctx OOP metadata cache to pre-warm.
/// @return Lowered member value and IL type.
Lowerer::RVal Lowerer::lowerMemberAccessExpr(const MemberAccessExpr &expr,
                                             OopLoweringContext &ctx) {
    // Pre-cache class info when base is a known object type.
    // This accelerates access control checks in resolveMemberField.
    if (expr.base) {
        std::string cls = resolveObjectClass(*expr.base);
        if (!cls.empty())
            (void)ctx.findClassInfo(qualify(cls));
    }
    return lowerMemberAccessExpr(expr);
}

/// @brief Context-aware overload of resolveMemberField() that pre-warms the class-info cache.
/// @details Pre-caches the base object's class info in @p ctx for access-control checks, then
///          delegates to the single-argument overload.
/// @param expr Member access whose field is resolved.
/// @param ctx OOP metadata cache to pre-warm.
/// @return Resolved field address and type metadata, or `std::nullopt`.
std::optional<Lowerer::MemberFieldAccess> Lowerer::resolveMemberField(const MemberAccessExpr &expr,
                                                                      OopLoweringContext &ctx) {
    // Pre-cache class info for access control checks.
    if (expr.base) {
        std::string cls = resolveObjectClass(*expr.base);
        if (!cls.empty())
            (void)ctx.findClassInfo(qualify(cls));
    }
    return resolveMemberField(expr);
}

/// @brief Context-aware overload of resolveImplicitField(); @p ctx is unused.
/// @details Implicit field resolution uses the active field scope rather than the OOP index, so
///          there is nothing to pre-cache; delegates to the two-argument overload.
/// @param name Unqualified implicit field name.
/// @param loc Source location used by resolution diagnostics.
/// @param ctx Explicit OOP context retained for interface symmetry.
/// @return Resolved field address and type metadata, or `std::nullopt`.
std::optional<Lowerer::MemberFieldAccess> Lowerer::resolveImplicitField(std::string_view name,
                                                                        il::support::SourceLoc loc,
                                                                        OopLoweringContext &ctx) {
    // Implicit field resolution uses active field scope, not OOP index.
    (void)ctx;
    return resolveImplicitField(name, loc);
}

} // namespace il::frontends::basic
