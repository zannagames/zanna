//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/lower/oop/Lower_OOP_Alloc.cpp
// Purpose: Lower BASIC OOP allocation, construction, and destruction operations.
// Key invariants: Object allocations route through runtime helpers; constructors
//                 and destructors follow the recorded class layouts.
// Ownership/Lifetime: Operates on Lowerer state without owning AST or module.
// Links: docs/internals/codemap.md
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements BASIC object allocation and constructor-call lowering.
/// @details Handles both catalogued runtime classes and user-defined class
///          layouts, including argument coercion, allocation metadata, runtime
///          helper selection, and optional OOP-context cache priming.

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/ILTypeUtils.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/NameMangler_OOP.hpp"
#include "frontends/basic/OopIndex.hpp"
#include "frontends/basic/OopLoweringContext.hpp"
#include "frontends/basic/Options.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/lower/oop/Lower_OOP_Internal.hpp"
#include "il/runtime/RuntimeClassNames.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace il::frontends::basic {

/// @brief Lower a BASIC @c NEW expression into IL runtime calls.
///
/// @details Queries the cached class layout to determine the allocation size
///          and class identifier, requests the object-allocation runtime helper,
///          and emits the constructor call with the newly created object prepended
///          to the argument list.  The resulting pointer value is packaged in an
///          @ref RVal ready for further lowering.
///
/// @param expr AST node representing the @c NEW expression.
/// @return Runtime value describing the allocated object pointer.
Lowerer::RVal Lowerer::lowerNewExpr(const NewExpr &expr) {
    curLoc = expr.loc;

    // Runtime class ctor mapping via catalog (e.g., Zanna.String.FromStr)
    {
        std::string qname = qualify(expr.className);
        if (const auto *c = il::runtime::findRuntimeClassByQName(qname)) {
            if (c->ctor && std::string(c->ctor).size()) {
                // BUG-023 fix: Look up ctor signature for argument type coercion
                const auto *ctorDesc = il::runtime::findRuntimeDescriptor(c->ctor);
                const std::vector<il::core::Type> *ctorExpectedTypes = nullptr;
                if (ctorDesc)
                    ctorExpectedTypes = &ctorDesc->signature.paramTypes;

                std::vector<Value> args;
                args.reserve(expr.args.size());
                for (size_t i = 0; i < expr.args.size(); ++i) {
                    const auto &a = expr.args[i];
                    RVal v = a ? lowerExpr(*a) : RVal{Value::constInt(0), Type(Type::Kind::I64)};
                    // Coerce argument type if needed
                    if (ctorExpectedTypes && i < ctorExpectedTypes->size()) {
                        auto expected = (*ctorExpectedTypes)[i];
                        // String→Ptr coercion (str passed where obj expected)
                        if (expected.kind == Type::Kind::Ptr && v.type.kind == Type::Kind::Str) {
                            // Strings are already pointer-compatible in IL
                            v.type = Type(Type::Kind::Ptr);
                        }
                        // I64→F64 coercion
                        else if (expected.kind == Type::Kind::F64 &&
                                 v.type.kind == Type::Kind::I64) {
                            v = coerceToF64(std::move(v), expr.loc);
                        }
                    }
                    args.push_back(v.value);
                }
                Type ret = ctorDesc ? ctorDesc->signature.retType
                                    : (string_utils::iequals(qname, il::runtime::RTCLASS_STRING)
                                           ? Type(Type::Kind::Str)
                                           : Type(Type::Kind::Ptr));
                Value obj = emitCallRet(ret, c->ctor, args);
                return {obj, ret};
            }
        }
    }

    // Minimal runtime type bridging: NEW Zanna.Text.StringBuilder()
    if (FrontendOptions::enableRuntimeTypeBridging()) {
        if (expr.args.empty()) {
            // Match fully-qualified type
            bool isQualified = false;
            if (!expr.qualifiedType.empty()) {
                const auto &q = expr.qualifiedType;
                if (q.size() == 3 && string_utils::iequals(q[0], "Zanna") &&
                    string_utils::iequals(q[1], "Text") &&
                    string_utils::iequals(q[2], "StringBuilder")) {
                    isQualified = true;
                }
            }
            // Fallback: check dot-joined className
            if (!isQualified) {
                if (string_utils::iequals(expr.className,
                                          std::string(il::runtime::RTCLASS_STRINGBUILDER)))
                    isQualified = true;
            }
            if (isQualified) {
                // Emit direct call to the canonical Text ctor that returns an object pointer.
                const char *ctorCanonical = "Zanna.Text.StringBuilder.New";
                if (builder)
                    builder->addExtern(ctorCanonical, Type(Type::Kind::Ptr), {});
                Value obj = emitCallRet(Type(Type::Kind::Ptr), ctorCanonical, {});
                return {obj, Type(Type::Kind::Ptr)};
            }
        }
    }
    std::size_t objectSize = 0;
    std::int64_t classId = 0;
    if (auto layoutIt = classLayouts_.find(expr.className); layoutIt != classLayouts_.end()) {
        objectSize = layoutIt->second.size;
        classId = layoutIt->second.classId;
    }

    // Ensure space for vptr at offset 0 even when class has no fields.
    if (objectSize < 8)
        objectSize = 8;
    requestHelper(RuntimeFeature::ObjNew);
    Value obj = emitCallRet(
        Type(Type::Kind::Ptr),
        "rt_obj_new_i64",
        {Value::constInt(classId), Value::constInt(static_cast<long long>(objectSize))});

    // Pre-initialize vptr from canonical per-class vtable pointer via registry
    if (oopIndex_.findClass(qualify(expr.className))) {
        auto itLayout = classLayouts_.find(expr.className);
        if (itLayout != classLayouts_.end()) {
            const long long typeId = (long long)itLayout->second.classId;
            Value vtblPtr = emitCallRet(
                Type(Type::Kind::Ptr), "rt_get_class_vtable", {Value::constInt(typeId)});
            // Store the vptr at offset 0 in the object
            emitStore(Type(Type::Kind::Ptr), obj, vtblPtr);
        }
    }

    std::vector<Value> ctorArgs;
    ctorArgs.reserve(expr.args.size() + 1);
    ctorArgs.push_back(obj);

    // BUG-OOP-007 fix: Look up constructor signature for argument coercion
    std::vector<::il::frontends::basic::Type> ctorParamTypes;
    std::string qname = qualify(expr.className);
    if (const ClassInfo *ci = oopIndex_.findClass(qname)) {
        for (const auto &p : ci->ctorParams)
            ctorParamTypes.push_back(p.type);
    }

    for (std::size_t i = 0; i < expr.args.size(); ++i) {
        const auto &arg = expr.args[i];
        if (!arg)
            continue;
        RVal lowered = lowerExpr(*arg);
        // BUG-OOP-007 fix: Coerce argument to match constructor parameter type
        if (i < ctorParamTypes.size()) {
            auto astTy = ctorParamTypes[i];
            if (astTy == ::il::frontends::basic::Type::Bool)
                lowered = coerceToBool(std::move(lowered), expr.loc);
            else if (astTy == ::il::frontends::basic::Type::F64)
                lowered = coerceToF64(std::move(lowered), expr.loc);
            else if (astTy == ::il::frontends::basic::Type::I64)
                lowered = coerceToI64(std::move(lowered), expr.loc);
        }
        ctorArgs.push_back(lowered.value);
    }

    curLoc = expr.loc;
    emitCall(mangleClassCtor(expr.className), ctorArgs);
    return {obj, Type(Type::Kind::Ptr)};
}

// -------------------------------------------------------------------------
// OopLoweringContext-aware implementations
// -------------------------------------------------------------------------

/// @brief Lower a @c NEW expression, pre-warming the OOP context's class-info cache.
/// @param expr The @c NEW expression.
/// @param ctx OOP lowering context used to pre-cache the constructor target's class info.
/// @return Runtime value describing the allocated object pointer.
/// @details Primes @p ctx with the target class info (benefiting later vtable-init lookups),
///          then delegates to the single-argument lowerNewExpr().
Lowerer::RVal Lowerer::lowerNewExpr(const NewExpr &expr, OopLoweringContext &ctx) {
    // Pre-cache the class info for the constructor target.
    // This benefits subsequent lookups during vtable initialization.
    (void)ctx.findClassInfo(qualify(expr.className));
    return lowerNewExpr(expr);
}

} // namespace il::frontends::basic
