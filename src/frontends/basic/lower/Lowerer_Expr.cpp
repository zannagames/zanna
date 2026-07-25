//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/lower/Lowerer_Expr.cpp
// Purpose: Implements the expression lowering visitor wiring that bridges BASIC
//          AST nodes to the shared Lowerer helpers.
// Key invariants: Expression visitors honour the Lowerer context and never
//                 mutate ownership of AST nodes.
// Ownership/Lifetime: Operates on a borrowed Lowerer instance; AST nodes remain
//                     owned by the caller.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the expression visitor and scalar-entry points for BASIC
///        AST-to-IL lowering.

#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/LowerExprBuiltin.hpp"
#include "frontends/basic/LowerExprLogical.hpp"
#include "frontends/basic/LowerExprNumeric.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/NameMangler_OOP.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"
#include "frontends/basic/TypeSuffix.hpp"
#include "frontends/basic/lower/AstVisitor.hpp"
#include "frontends/basic/lower/MemberArrayResolver.hpp"
#include "frontends/basic/sem/OverloadResolution.hpp"
#include "il/runtime/RuntimeSignatures.hpp"

#include "zanna/il/Module.hpp"

#include <utility>
#include <vector>

namespace il::frontends::basic {

using IlType = il::core::Type;
using IlValue = il::core::Value;

/// @brief Visitor that lowers BASIC expressions using Lowerer helpers.
/// @details The visitor implements the generated @ref ExprVisitor interface and
///          redirects each AST node type to the specialised lowering helpers on
///          @ref Lowerer. The instance carries a reference to the current
///          lowering context so it can update source locations, perform type
///          coercions, and capture the produced IL value for the caller.
class LowererExprVisitor final : public lower::AstVisitor, public ExprVisitor {
  public:
    /// @brief Construct a visitor that records results into @p lowerer.
    /// @details The visitor only borrows the lowering context; ownership of the
    ///          surrounding compiler state remains with the caller.
    /// @param lowerer Lowering context that exposes shared helper routines.
    explicit LowererExprVisitor(Lowerer &lowerer) noexcept : lowerer_(lowerer) {}

    /// @brief Dispatch an expression node to the corresponding visit method.
    /// @details Updates the result cache by forwarding the call to
    ///          @ref Expr::accept, allowing dynamic dispatch over the concrete
    ///          expression type.
    /// @param expr Expression node that should be lowered into IL form.
    void visitExpr(const Expr &expr) override {
        expr.accept(*this);
    }

    /// @brief Ignore statement nodes encountered through the generic visitor.
    /// @details Expression lowering never acts on statements, so the override is
    ///          a no-op that satisfies the @ref lower::AstVisitor contract.
    void visitStmt(const Stmt &) override {}

    /// @brief Lower an integer literal expression.
    /// @details Captures the literal value as a 64-bit IL constant and records
    ///          the associated source location in the lowering context.
    /// @param expr Integer literal node from the BASIC AST.
    void visit(const IntExpr &expr) override {
        lowerer_.curLoc = expr.loc;
        result_ = Lowerer::RVal{IlValue::constInt(expr.value), IlType(IlType::Kind::I64)};
    }

    /// @brief Lower a floating-point literal expression.
    /// @details Emits an IL constant containing the literal value and tags it
    ///          with the 64-bit floating-point type descriptor.
    /// @param expr Floating-point literal node from the BASIC AST.
    void visit(const FloatExpr &expr) override {
        lowerer_.curLoc = expr.loc;
        result_ = Lowerer::RVal{IlValue::constFloat(expr.value), IlType(IlType::Kind::F64)};
    }

    /// @brief Lower a string literal expression.
    /// @details Interns the string in the module's constant pool, emits a load
    ///          of the retained runtime string handle, and records the result
    ///          with the IL string type.
    /// @param expr String literal node from the BASIC AST.
    void visit(const StringExpr &expr) override {
        lowerer_.curLoc = expr.loc;
        std::string lbl = lowerer_.getStringLabel(expr.value);
        IlValue tmp = lowerer_.emitConstStr(lbl);
        result_ = Lowerer::RVal{tmp, IlType(IlType::Kind::Str)};
    }

    /// @brief Lower a boolean literal expression.
    /// @details Preserve classic BASIC convention in IL by representing
    ///          booleans as integer values (-1 for TRUE, 0 for FALSE) with
    ///          `i64` type. Downstream call sites perform i64→i1 coercion when
    ///          targeting boolean parameters to satisfy verifier expectations.
    /// @param expr Boolean literal node from the BASIC AST.
    void visit(const BoolExpr &expr) override {
        lowerer_.curLoc = expr.loc;
        IlValue intVal = IlValue::constInt(expr.value ? -1 : 0);
        result_ = Lowerer::RVal{intVal, IlType(IlType::Kind::I64)};
    }

    /// @brief Lower a variable reference expression.
    /// @details Defers to @ref Lowerer::lowerVarExpr so storage class specific
    ///          logic (such as locals versus fields) is centralised in the
    ///          lowering helpers.
    /// @param expr Variable reference node from the BASIC AST.
    void visit(const VarExpr &expr) override {
        result_ = lowerer_.lowerVarExpr(expr);
    }

    /// @brief Lower an array access expression.
    /// @details Computes the base pointer and index using
    ///          @ref Lowerer::lowerArrayAccess, then emits a runtime call to load
    ///          the indexed element. Uses rt_arr_str_get for string arrays or
    ///          rt_arr_i32_get for integer arrays. The result type is determined
    ///          by the array element type.
    /// @param expr Array access node from the BASIC AST.
    void visit(const ArrayExpr &expr) override {
        Lowerer::ArrayAccess access =
            lowerer_.lowerArrayAccess(expr, Lowerer::ArrayAccessKind::Load);
        lowerer_.curLoc = expr.loc;

        // Determine array element type and use appropriate runtime function.
        // Member array field resolution consolidated via resolveMemberArrayField().
        const auto *info = lowerer_.findSymbol(expr.name);
        const MemberArrayInfo fieldInfo = lowerer_.resolveMemberArrayField(expr.name);

        // BUG-097 fix: Check module-level cache if symbol exists but isn't marked as object,
        // or if symbol doesn't exist at all (procedure-local symbol tables lose module info)
        std::string moduleObjectClass;
        if (!info || (info && !info->isObject)) {
            moduleObjectClass = lowerer_.lookupModuleArrayElemClass(expr.name);
        }

        // BUG-OOP-011 fix: Check module-level string array cache
        bool isModuleStrArray = lowerer_.isModuleStrArray(expr.name);

        // BUG-OOP-011 fix: Also check module-level string array cache
        if ((info && info->type == ::il::frontends::basic::Type::Str) ||
            (fieldInfo.isField && fieldInfo.elementAstType == ::il::frontends::basic::Type::Str) ||
            isModuleStrArray) {
            // String array: use rt_arr_str_get (returns retained handle)
            IlValue val = lowerer_.emitCallRet(
                IlType(IlType::Kind::Str), "rt_arr_str_get", {access.base, access.index});
            result_ = Lowerer::RVal{val, IlType(IlType::Kind::Str)};
        } else if ((info && info->isObject) || fieldInfo.isObjectArray ||
                   !moduleObjectClass.empty()) {
            // BUG-089/BUG-097 fix: Object array (member, non-member, or module-level)
            IlValue val = lowerer_.emitCallRet(
                IlType(IlType::Kind::Ptr), "rt_arr_obj_get", {access.base, access.index});
            result_ = Lowerer::RVal{val, IlType(IlType::Kind::Ptr)};
        } else if ((info && info->type == ::il::frontends::basic::Type::F64) ||
                   (fieldInfo.isField &&
                    fieldInfo.elementAstType == ::il::frontends::basic::Type::F64)) {
            // Float array (SINGLE/DOUBLE): use rt_arr_f64_get
            lowerer_.requireArrayF64Get();
            IlValue val = lowerer_.emitCallRet(
                IlType(IlType::Kind::F64), "rt_arr_f64_get", {access.base, access.index});
            result_ = Lowerer::RVal{val, IlType(IlType::Kind::F64)};
        } else {
            // Integer/numeric array: use rt_arr_i64_get (all Zanna integers are 64-bit)
            lowerer_.requireArrayI64Get();
            IlValue val = lowerer_.emitCallRet(
                IlType(IlType::Kind::I64), "rt_arr_i64_get", {access.base, access.index});
            result_ = Lowerer::RVal{val, IlType(IlType::Kind::I64)};
        }
    }

    /// @brief Lower a unary operator expression.
    /// @details Delegates to the shared helper that understands the full matrix
    ///          of BASIC unary operators.
    /// @param expr Unary operator node from the BASIC AST.
    void visit(const UnaryExpr &expr) override {
        result_ = lowerer_.lowerUnaryExpr(expr);
    }

    /// @brief Lower a binary operator expression.
    /// @details Forwards to @ref Lowerer::lowerBinaryExpr which handles operand
    ///          coercion and emits the correct IL opcode for the operator.
    /// @param expr Binary operator node from the BASIC AST.
    void visit(const BinaryExpr &expr) override {
        result_ = lowerer_.lowerBinaryExpr(expr);
    }

    /// @brief Lower a builtin function call expression.
    /// @details Delegates to @ref lowerBuiltinCall so builtin-specific lowering
    ///          stays concentrated in the helper module.
    /// @param expr Builtin invocation node from the BASIC AST.
    void visit(const BuiltinCallExpr &expr) override {
        result_ = lowerBuiltinCall(lowerer_, expr);
    }

    /// @brief Lower the LBOUND intrinsic expression.
    /// @details Emits the constant zero because BASIC arrays are zero based in
    ///          the current runtime configuration.
    /// @param expr LBOUND invocation node from the BASIC AST.
    void visit(const LBoundExpr &expr) override {
        lowerer_.curLoc = expr.loc;
        result_ = Lowerer::RVal{IlValue::constInt(0), IlType(IlType::Kind::I64)};
    }

    /// @brief Lower the UBOUND intrinsic expression.
    /// @details Delegates to the helper that computes the appropriate runtime
    ///          query for the array upper bound.
    /// @param expr UBOUND invocation node from the BASIC AST.
    void visit(const UBoundExpr &expr) override {
        result_ = lowerer_.lowerUBoundExpr(expr);
    }

    /// @brief Lower a user-defined procedure call expression.
    /// @details Resolves the callee signature when available so argument values
    ///          can be coerced into the expected IL types before emitting the
    ///          call. When the signature advertises a return value the helper
    ///          records the call result; otherwise it fabricates a dummy integer
    ///          to keep the return type consistent for expression contexts.
    /// @param expr Call expression node from the BASIC AST.
    void visit(const CallExpr &expr) override {
        // BUG-059 fix: Check if this is actually a field array access
        // In class methods, name(index) might be a field array, not a function call
        std::string className = lowerer_.currentClass();
        if (!className.empty() && expr.calleeQualified.empty()) {
            std::string fieldName = CanonicalizeIdent(expr.callee);
            if (lowerer_.isFieldArray(className, fieldName)) {
                // This is a field array access, not a function call
                // Construct a temporary ArrayExpr with ME.fieldname
                std::string dottedName = "ME." + expr.callee;
                ArrayExpr tempExpr;
                tempExpr.name = dottedName;
                tempExpr.loc = expr.loc;
                // Temporarily move unique_ptrs from expr.args to tempExpr.indices
                // We'll move them back after lowering
                for (auto &arg : const_cast<std::vector<ExprPtr> &>(expr.args)) {
                    tempExpr.indices.push_back(std::move(arg));
                }
                // Now call the visitor as if this were an ArrayExpr
                visit(tempExpr);
                // Move the indices back to expr.args to restore ownership
                for (size_t i = 0; i < tempExpr.indices.size(); ++i) {
                    const_cast<std::vector<ExprPtr> &>(expr.args)[i] =
                        std::move(tempExpr.indices[i]);
                }
                return;
            }
        }

        // Implicit receiver in class methods: treat bare calls as ME.Method.
        // If we're inside a class method and the call is unqualified, try
        // lowering it as a method call on ME first (BUG-102).
        // BUG-OOP-031 fix: Only treat as method call if the method actually
        // exists in the current class or its base classes. Otherwise fall
        // through to global procedure resolution.
        if (lowerer_.currentClass().size() > 0 && expr.calleeQualified.empty()) {
            // Check if this is actually a method of the current class
            const auto *methodInfo =
                lowerer_.oopIndex_.findMethodInHierarchy(lowerer_.currentClass(), expr.callee);
            if (!methodInfo) {
                // Not a method of this class - fall through to global resolution
                lowerGlobalCall(expr);
                return;
            }

            // Load ME pointer as implicit receiver
            const auto *meSym = lowerer_.findSymbol("ME");
            if (meSym && meSym->slotId) {
                lowerer_.curLoc = expr.loc;
                IlValue selfArg =
                    lowerer_.emitLoad(IlType(IlType::Kind::Ptr), IlValue::temp(*meSym->slotId));

                auto exprTypeToAst = [&](Lowerer::ExprType ty) {
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
                };

                std::vector<::il::frontends::basic::Type> argAstTypes;
                argAstTypes.reserve(expr.args.size());
                for (const auto &arg : expr.args)
                    argAstTypes.push_back(arg ? exprTypeToAst(lowerer_.scanExpr(*arg))
                                              : ::il::frontends::basic::Type::I64);

                std::string selectedName = expr.callee;
                std::string declaringClass = lowerer_.currentClass();
                const ClassInfo::MethodInfo *selectedMethod = methodInfo;
                if (auto resolved = sem::resolveMethodOverload(lowerer_.oopIndex_,
                                                               lowerer_.currentClass(),
                                                               expr.callee,
                                                               false,
                                                               argAstTypes,
                                                               lowerer_.currentClass(),
                                                               lowerer_.diagnosticEmitter(),
                                                               expr.loc)) {
                    selectedName = resolved->methodName;
                    declaringClass = resolved->qualifiedClass;
                    selectedMethod = resolved->method;
                } else if (lowerer_.diagnosticEmitter()) {
                    result_ = Lowerer::RVal{IlValue::constInt(0), IlType(IlType::Kind::I64)};
                    return;
                }

                // Lower arguments and prepend receiver
                std::vector<IlValue> args;
                args.reserve(expr.args.size() + 1);
                args.push_back(selfArg);
                const auto &expectParamAst = selectedMethod->sig.paramTypes;
                for (std::size_t i = 0; i < expr.args.size(); ++i) {
                    const auto &a = expr.args[i];
                    if (!a)
                        continue;
                    Lowerer::RVal v = lowerer_.lowerExpr(*a);
                    if (i < expectParamAst.size()) {
                        auto astTy = expectParamAst[i];
                        if (astTy == ::il::frontends::basic::Type::Bool)
                            v = lowerer_.coerceToBool(std::move(v), expr.loc);
                        else if (astTy == ::il::frontends::basic::Type::F64)
                            v = lowerer_.coerceToF64(std::move(v), expr.loc);
                        else if (astTy == ::il::frontends::basic::Type::I64)
                            v = lowerer_.coerceToI64(std::move(v), expr.loc);
                    }
                    args.push_back(v.value);
                }

                // Determine return IL type when available; otherwise emit void call
                IlType retIl = IlType(IlType::Kind::Void);
                if (!selectedMethod->sig.returnClassName.empty())
                    retIl = IlType(IlType::Kind::Ptr);
                else if (selectedMethod->sig.returnType) {
                    switch (*selectedMethod->sig.returnType) {
                        case ::il::frontends::basic::Type::I64:
                            retIl = IlType(IlType::Kind::I64);
                            break;
                        case ::il::frontends::basic::Type::F64:
                            retIl = IlType(IlType::Kind::F64);
                            break;
                        case ::il::frontends::basic::Type::Str:
                            retIl = IlType(IlType::Kind::Str);
                            break;
                        case ::il::frontends::basic::Type::Bool:
                            retIl = lowerer_.ilBoolTy();
                            break;
                    }
                }

                // Mangle and emit call
                const std::string callee = mangleMethod(declaringClass, selectedName);
                lowerer_.curLoc = expr.loc;
                if (retIl.kind != IlType::Kind::Void) {
                    IlValue res = lowerer_.emitCallRet(retIl, callee, args);
                    if (retIl.kind == IlType::Kind::Str)
                        lowerer_.deferReleaseStr(res);
                    else if (retIl.kind == IlType::Kind::Ptr)
                        lowerer_.deferReleaseObj(res, selectedMethod->sig.returnClassName);
                    result_ = Lowerer::RVal{res, retIl};
                } else {
                    lowerer_.emitCall(callee, args);
                    result_ = Lowerer::RVal{IlValue::constInt(0), IlType(IlType::Kind::I64)};
                }
                return;
            }
        }

        lowerGlobalCall(expr);
    }

    /// @brief Resolve and lower a global / qualified procedure call (the path
    ///        taken when a call is not a field array or current-class method).
    /// @details Resolves runtime aliases and USING imports before user
    ///          signatures, lowers BYREF arguments as storage addresses, coerces
    ///          value arguments to declared types, and records either the call
    ///          result or the expression-context zero fallback.
    /// @param expr Call expression not handled as a member-array access or
    ///        current-class method.
    void lowerGlobalCall(const CallExpr &expr) {
        // Resolve callee (supports qualified call syntax). Canonicalize to
        // maintain case-insensitive semantics for lookups.
        std::string calleeResolved;
        if (!expr.calleeQualified.empty()) {
            calleeResolved = CanonicalizeQualified(expr.calleeQualified);
        } else {
            calleeResolved = CanonicalizeIdent(expr.callee);
        }
        const std::string &calleeKey = calleeResolved.empty() ? expr.callee : calleeResolved;
        // Prefer runtime builtin externs when the name matches a canonical
        // runtime descriptor (e.g., "Zanna.Terminal.PrintI64"). Otherwise, fall
        // back to user-defined procedure signatures collected from the AST.
        const il::runtime::RuntimeSignature *rtSig = il::runtime::findRuntimeSignature(calleeKey);
        // If not found and the call is unqualified, try resolving against USING imports.
        // This mirrors semantic resolution where USING imports allow unqualified
        // calls like SetPosition to bind to Zanna.Terminal.SetPosition.
        if (!rtSig && calleeKey.find('.') == std::string::npos && !expr.callee.empty()) {
            // Helper to convert canonical namespace to title-case for runtime lookup
            auto titleCaseNs = [](const std::string &ns) -> std::string {
                std::string out;
                out.reserve(ns.size());
                bool start = true;
                for (char ch : ns) {
                    if (ch == '.') {
                        out.push_back('.');
                        start = true;
                    } else if (start) {
                        out.push_back(
                            static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
                        start = false;
                    } else {
                        out.push_back(ch);
                    }
                }
                return out;
            };

            // Try USING imports from semantic analyzer first
            const SemanticAnalyzer *sema = lowerer_.semanticAnalyzer();
            if (sema) {
                std::vector<std::string> imports = sema->getUsingImports();
                for (const auto &ns : imports) {
                    // Build qualified name: namespace.callee (title-cased for runtime lookup)
                    std::string qualifiedNs = titleCaseNs(ns);
                    std::string candidate = qualifiedNs + "." + expr.callee;
                    if (const auto *sig = il::runtime::findRuntimeSignature(candidate)) {
                        rtSig = sig;
                        calleeResolved = std::move(candidate);
                        break;
                    }
                    // Also try case-insensitive lookup in runtime signatures
                    const auto &rts = il::runtime::runtimeSignatures();
                    std::string candidateLower = ns + "." + calleeKey;
                    for (const auto &kv : rts) {
                        const std::string_view name = kv.first;
                        if (name.size() != candidateLower.size())
                            continue;
                        bool eq = true;
                        for (size_t i = 0; i < name.size(); ++i) {
                            unsigned char a = static_cast<unsigned char>(name[i]);
                            unsigned char b = static_cast<unsigned char>(candidateLower[i]);
                            if (std::tolower(a) != std::tolower(b)) {
                                eq = false;
                                break;
                            }
                        }
                        if (eq) {
                            rtSig = &kv.second;
                            calleeResolved = std::string(name);
                            break;
                        }
                    }
                    if (rtSig)
                        break;
                }
            }
            // Fallback: try common Zanna.* namespaces even without explicit USING
            if (!rtSig) {
                static const char *defaultNamespaces[] = {"Zanna.Terminal", "Zanna.Time"};
                for (const char *ns : defaultNamespaces) {
                    std::string candidate = std::string(ns) + "." + expr.callee;
                    if (const auto *sig = il::runtime::findRuntimeSignature(candidate)) {
                        rtSig = sig;
                        calleeResolved = std::move(candidate);
                        break;
                    }
                }
            }
        }
        // Fallback: case-insensitive match against runtime symbols when dotted and canonicalized.
        if (!rtSig && calleeKey.find('.') != std::string::npos) {
            const auto &rts = il::runtime::runtimeSignatures();
            for (const auto &kv : rts) {
                // Case-insensitive compare
                const std::string_view name = kv.first;
                if (name.size() != calleeKey.size())
                    continue;
                bool eq = true;
                for (size_t i = 0; i < name.size(); ++i) {
                    unsigned char a = static_cast<unsigned char>(name[i]);
                    unsigned char b = static_cast<unsigned char>(calleeKey[i]);
                    if (std::tolower(a) != std::tolower(b)) {
                        eq = false;
                        break;
                    }
                }
                if (eq) {
                    // Bind to exact-cased runtime name/signature
                    rtSig = &kv.second;
                    // Replace calleeResolved with the canonical runtime symbol spelling
                    const_cast<std::string &>(calleeKey) = std::string(name);
                    break;
                }
            }
        }
        const auto *signature = rtSig ? nullptr : lowerer_.findProcSignature(calleeKey);
        std::vector<IlValue> args;
        args.reserve(expr.args.size());
        if (rtSig) {
            // Coerce arguments according to the runtime signature parameter IL types.
            for (size_t i = 0; i < expr.args.size(); ++i) {
                Lowerer::RVal arg = lowerer_.lowerExpr(*expr.args[i]);
                if (i < rtSig->paramTypes.size()) {
                    IlType paramTy = rtSig->paramTypes[i];
                    if (paramTy.kind == IlType::Kind::F64)
                        arg = lowerer_.coerceToF64(std::move(arg), expr.loc);
                    else if (paramTy.kind == IlType::Kind::I64)
                        arg = lowerer_.coerceToI64(std::move(arg), expr.loc);
                    else if (paramTy.kind == IlType::Kind::I1)
                        arg = lowerer_.coerceToBool(std::move(arg), expr.loc);
                    else if (paramTy.kind == IlType::Kind::I32) {
                        arg = lowerer_.ensureI64(std::move(arg), expr.loc);
                        arg.value = lowerer_.emitCommon(expr.loc).narrow_to(arg.value, 64, 32);
                    } else if (paramTy.kind == IlType::Kind::I16) {
                        arg = lowerer_.ensureI64(std::move(arg), expr.loc);
                        arg.value = lowerer_.emitCommon(expr.loc).narrow_to(arg.value, 64, 16);
                    } else if (paramTy.kind == IlType::Kind::Ptr &&
                               arg.type.kind != IlType::Kind::Ptr &&
                               arg.value.kind == IlValue::Kind::ConstInt && arg.value.i64 == 0) {
                        arg = Lowerer::RVal{IlValue::null(), paramTy};
                    }
                }
                args.push_back(arg.value);
            }
            lowerer_.curLoc = expr.loc;
            // Emit direct call to the canonical runtime extern (e.g., @Zanna.Terminal.PrintI64).
            if (rtSig->retType.kind != IlType::Kind::Void) {
                const std::string &target = calleeResolved.empty() ? calleeKey : calleeResolved;
                IlValue res = lowerer_.emitCallRet(rtSig->retType, target, args);
                result_ = Lowerer::RVal{res, rtSig->retType};
            } else {
                const std::string &target = calleeResolved.empty() ? calleeKey : calleeResolved;
                lowerer_.emitCall(target, args);
                result_ = Lowerer::RVal{IlValue::constInt(0), IlType(IlType::Kind::I64)};
            }
        } else {
            for (size_t i = 0; i < expr.args.size(); ++i) {
                // BYREF support: when the signature marks parameter i as BYREF, pass address-of
                // the variable storage when possible.
                if (signature && i < signature->byRefFlags.size() && signature->byRefFlags[i]) {
                    const ExprPtr &argExpr = expr.args[i];
                    if (auto *v = dynamic_cast<const VarExpr *>(argExpr.get())) {
                        if (auto storage = lowerer_.resolveVariableStorage(v->name, expr.loc)) {
                            args.push_back(storage->pointer);
                            continue;
                        }
                    }
                    // Fallback: if cannot take address, coerce as normal (diagnostics may surface
                    // elsewhere)
                }

                Lowerer::RVal arg = lowerer_.lowerExpr(*expr.args[i]);
                if (signature && i < signature->paramTypes.size()) {
                    IlType paramTy = signature->paramTypes[i];
                    if (paramTy.kind == IlType::Kind::F64)
                        arg = lowerer_.coerceToF64(std::move(arg), expr.loc);
                    else if (paramTy.kind == IlType::Kind::I64)
                        arg = lowerer_.coerceToI64(std::move(arg), expr.loc);
                    else if (paramTy.kind == IlType::Kind::I1)
                        arg = lowerer_.coerceToBool(std::move(arg), expr.loc);
                }
                args.push_back(arg.value);
            }
            lowerer_.curLoc = expr.loc;
            const std::string calleeName = lowerer_.resolveCalleeName(calleeKey);
            if (signature && signature->retType.kind != IlType::Kind::Void) {
                IlValue res = lowerer_.emitCallRet(signature->retType, calleeName, args);
                result_ = Lowerer::RVal{res, signature->retType};
            } else {
                lowerer_.emitCall(calleeName, args);
                result_ = Lowerer::RVal{IlValue::constInt(0), IlType(IlType::Kind::I64)};
            }
        }
    }

    /// @brief Lower a @c NEW expression for object construction.
    /// @details Defers to @ref Lowerer::lowerNewExpr so object layout specifics
    ///          reside in the dedicated helper.
    /// @param expr Object construction node from the BASIC AST.
    void visit(const NewExpr &expr) override {
        result_ = lowerer_.lowerNewExpr(expr);
    }

    /// @brief Lower a reference to the implicit @c ME parameter.
    /// @details Hands control to @ref Lowerer::lowerMeExpr which implements the
    ///          details around retrieving the current instance reference.
    /// @param expr ME expression node from the BASIC AST.
    void visit(const MeExpr &expr) override {
        result_ = lowerer_.lowerMeExpr(expr);
    }

    /// @brief Lower a member access expression.
    /// @details Uses @ref Lowerer::lowerMemberAccessExpr to resolve the field or
    ///          property lookup and produce the corresponding IL value.
    /// @param expr Member access node from the BASIC AST.
    void visit(const MemberAccessExpr &expr) override {
        result_ = lowerer_.lowerMemberAccessExpr(expr);
    }

    /// @brief Lower a method call expression.
    /// @details Delegates to @ref Lowerer::lowerMethodCallExpr which handles the
    ///          implicit receiver and method dispatch semantics.
    /// @param expr Method call node from the BASIC AST.
    void visit(const MethodCallExpr &expr) override {
        // BUG-056: If method-like syntax targets a field name with indices, treat as
        // array-field element access rather than a real method call.
        std::string cls = expr.base ? lowerer_.resolveObjectClass(*expr.base) : std::string{};
        if (!cls.empty()) {
            if (const Lowerer::ClassLayout *layout = lowerer_.findClassLayout(cls)) {
                if (const Lowerer::ClassLayout::Field *fld = layout->findField(expr.method)) {
                    // Only treat as array-field access when the field is actually an array.
                    // Otherwise, fall back to lowering a real method call (BUG-106).
                    if (!fld->isArray) {
                        result_ = lowerer_.lowerMethodCallExpr(expr);
                        return;
                    }
                    // Compute array handle pointer from object field
                    Lowerer::RVal self = lowerer_.lowerExpr(*expr.base);
                    lowerer_.curLoc = expr.loc;
                    Lowerer::Value fieldPtr = lowerer_.emitBinary(
                        il::core::Opcode::GEP,
                        Lowerer::Type(Lowerer::Type::Kind::Ptr),
                        self.value,
                        Lowerer::Value::constInt(static_cast<long long>(fld->offset)));
                    lowerer_.curLoc = expr.loc;
                    Lowerer::Value arrHandle =
                        lowerer_.emitLoad(Lowerer::Type(Lowerer::Type::Kind::Ptr), fieldPtr);

                    // BUG-094 fix: Lower all indices and compute flattened index for
                    // multi-dimensional arrays
                    std::vector<Lowerer::Value> indices;
                    indices.reserve(expr.args.size());
                    for (const auto &arg : expr.args) {
                        if (arg) {
                            Lowerer::RVal idx = lowerer_.lowerExpr(*arg);
                            idx = lowerer_.coerceToI64(std::move(idx), expr.loc);
                            indices.push_back(idx.value);
                        }
                    }

                    // Compute flattened index for multi-dimensional arrays
                    Lowerer::Value indexVal = Lowerer::Value::constInt(0);
                    if (!indices.empty()) {
                        if (indices.size() == 1) {
                            indexVal = indices[0];
                        } else if (fld->isArray && !fld->arrayExtents.empty() &&
                                   fld->arrayExtents.size() == indices.size()) {
                            lowerer_.curLoc = expr.loc;
                            indexVal = lowerer_.emitRowMajorFlatIndex(indices, fld->arrayExtents);
                        } else {
                            if (auto *em = lowerer_.diagnosticEmitter()) {
                                em->emit(il::support::Severity::Error,
                                         "B2000",
                                         expr.loc,
                                         static_cast<uint32_t>(expr.method.size()),
                                         "cannot flatten multidimensional field array without "
                                         "matching extents");
                            }
                            lowerer_.emitTrap();
                            indexVal = Lowerer::Value::constInt(0);
                        }
                    }

                    // Select getter and result type based on field element type
                    if (fld->type == ::il::frontends::basic::Type::Str) {
                        lowerer_.requireArrayStrGet();
                        // BUG-071 fix: Don't defer release - consuming code handles lifetime
                        Lowerer::IlValue val =
                            lowerer_.emitCallRet(Lowerer::IlType(Lowerer::IlType::Kind::Str),
                                                 "rt_arr_str_get",
                                                 {arrHandle, indexVal});
                        // Removed: lowerer_.deferReleaseStr(val);
                        result_ = Lowerer::RVal{val, Lowerer::IlType(Lowerer::IlType::Kind::Str)};
                        return;
                    } else if (!fld->objectClassName.empty()) {
                        // BUG-096/BUG-098 fix: Handle object arrays
                        lowerer_.requireArrayObjGet();
                        Lowerer::IlValue val =
                            lowerer_.emitCallRet(Lowerer::IlType(Lowerer::IlType::Kind::Ptr),
                                                 "rt_arr_obj_get",
                                                 {arrHandle, indexVal});
                        result_ = Lowerer::RVal{val, Lowerer::IlType(Lowerer::IlType::Kind::Ptr)};
                        return;
                    } else if (fld->type == ::il::frontends::basic::Type::F64) {
                        // Float array (SINGLE/DOUBLE): use rt_arr_f64_get
                        lowerer_.requireArrayF64Get();
                        Lowerer::IlValue val =
                            lowerer_.emitCallRet(Lowerer::IlType(Lowerer::IlType::Kind::F64),
                                                 "rt_arr_f64_get",
                                                 {arrHandle, indexVal});
                        result_ = Lowerer::RVal{val, Lowerer::IlType(Lowerer::IlType::Kind::F64)};
                        return;
                    } else {
                        lowerer_.requireArrayI64Get();
                        Lowerer::IlValue val =
                            lowerer_.emitCallRet(Lowerer::IlType(Lowerer::IlType::Kind::I64),
                                                 "rt_arr_i64_get",
                                                 {arrHandle, indexVal});
                        result_ = Lowerer::RVal{val, Lowerer::IlType(Lowerer::IlType::Kind::I64)};
                        return;
                    }
                }
            }
        }
        // Default: regular method call lowering
        result_ = lowerer_.lowerMethodCallExpr(expr);
    }

    /// @brief Lower IS expression via RTTI helpers.
    /// @details Resolves the target class or interface ID, obtains the runtime
    ///          type ID of the object value, and converts the runtime predicate
    ///          result to an IL boolean.
    /// @param expr Object identity/type-test expression.
    void visit(const IsExpr &expr) override {
        lowerer_.curLoc = expr.loc;
        // Lower left value to an object pointer
        Lowerer::RVal lhs = lowerer_.lowerExpr(*expr.value);
        // Build type/interface key from dotted name
        std::string dotted;
        for (size_t i = 0; i < expr.typeName.size(); ++i) {
            if (i)
                dotted.push_back('.');
            dotted += expr.typeName[i];
        }
        // Determine if target is an interface
        bool isIface = false;
        int targetId = -1;
        // Interface lookup via OOP index
        for (const auto &p : lowerer_.oopIndex_.interfacesByQname()) {
            if (p.first == dotted) {
                isIface = true;
                targetId = p.second.ifaceId;
                break;
            }
        }
        if (!isIface) {
            // Use last segment as class key for layout map
            std::string cls = expr.typeName.empty() ? std::string{} : expr.typeName.back();
            if (const Lowerer::ClassLayout *layout = lowerer_.findClassLayout(cls))
                targetId = static_cast<int>(layout->classId);
        }

        // Call rt_typeid_of to get type id and then predicate helper
        Lowerer::Value typeIdVal = lowerer_.emitCallRet(
            Lowerer::Type(Lowerer::Type::Kind::I64), "rt_typeid_of", {lhs.value});
        Lowerer::Value pred64 =
            isIface ? lowerer_.emitCallRet(Lowerer::Type(Lowerer::Type::Kind::I64),
                                           "rt_type_implements",
                                           {typeIdVal, Lowerer::Value::constInt(targetId)})
                    : lowerer_.emitCallRet(Lowerer::Type(Lowerer::Type::Kind::I64),
                                           "rt_type_is_a",
                                           {typeIdVal, Lowerer::Value::constInt(targetId)});
        Lowerer::Value cond = lowerer_.emitBinary(
            il::core::Opcode::ICmpNe, lowerer_.ilBoolTy(), pred64, Lowerer::Value::constInt(0));
        result_ = Lowerer::RVal{cond, lowerer_.ilBoolTy()};
    }

    /// @brief Lower ADDRESSOF expression to obtain a function pointer.
    /// @details Emits IL that produces the address of the named SUB or FUNCTION.
    ///          The result is an opaque pointer suitable for passing to threading APIs.
    /// @param expr Procedure-address expression whose target name is
    ///        canonicalized for IL linkage.
    void visit(const AddressOfExpr &expr) override {
        lowerer_.curLoc = expr.loc;
        // Emit a global address reference to the function.
        // The function name will be resolved by the linker/VM.
        std::string funcName = expr.targetName;
        // Use canonical uppercase naming for BASIC procedures
        for (char &c : funcName)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        // Note: Value::global() takes the name without '@' prefix - IL printer adds it
        IlValue ptr = IlValue::global(funcName);
        result_ = Lowerer::RVal{ptr, IlType(IlType::Kind::Ptr)};
    }

    /// @brief Lower AS expression via RTTI helpers.
    /// @details Resolves the target class or interface ID and emits the
    ///          corresponding runtime checked-cast call.
    /// @param expr Checked object-cast expression.
    void visit(const AsExpr &expr) override {
        lowerer_.curLoc = expr.loc;
        Lowerer::RVal lhs = lowerer_.lowerExpr(*expr.value);
        // Dotted type name
        std::string dotted;
        for (size_t i = 0; i < expr.typeName.size(); ++i) {
            if (i)
                dotted.push_back('.');
            dotted += expr.typeName[i];
        }
        bool isIface = false;
        int targetId = -1;
        for (const auto &p : lowerer_.oopIndex_.interfacesByQname()) {
            if (p.first == dotted) {
                isIface = true;
                targetId = p.second.ifaceId;
                break;
            }
        }
        if (!isIface) {
            std::string cls = expr.typeName.empty() ? std::string{} : expr.typeName.back();
            if (const Lowerer::ClassLayout *layout = lowerer_.findClassLayout(cls))
                targetId = static_cast<int>(layout->classId);
        }
        Lowerer::Value casted =
            isIface ? lowerer_.emitCallRet(Lowerer::Type(Lowerer::Type::Kind::Ptr),
                                           "rt_cast_as_iface",
                                           {lhs.value, Lowerer::Value::constInt(targetId)})
                    : lowerer_.emitCallRet(Lowerer::Type(Lowerer::Type::Kind::Ptr),
                                           "rt_cast_as",
                                           {lhs.value, Lowerer::Value::constInt(targetId)});
        result_ = Lowerer::RVal{casted, Lowerer::Type(Lowerer::Type::Kind::Ptr)};
    }

    /// @brief Retrieve the IL value produced by the most recent visit.
    /// @details The visitor stores the result internally to avoid allocating on
    ///          every visit call; this accessor exposes the cached value to the
    ///          caller.
    /// @return Pair containing the IL value and its static type.
    [[nodiscard]] Lowerer::RVal result() const noexcept {
        return result_;
    }

  private:
    /// Borrowed lowering context used for all emitted IL and lookups.
    Lowerer &lowerer_;
    /// Most recent expression result, initialized to an I64 zero fallback.
    Lowerer::RVal result_{IlValue::constInt(0), IlType(IlType::Kind::I64)};
};

/// @brief Lower an arbitrary BASIC expression to IL form.
/// @details Creates a temporary @ref LowererExprVisitor to traverse the AST and
///          capture the resulting value. The current source location is updated
///          so diagnostics emitted during lowering point back to the originating
///          node.
/// @param expr Expression node to lower.
/// @return Resulting IL value and type.
Lowerer::RVal Lowerer::lowerExpr(const Expr &expr) {
    if (++exprLowerDepth_ > kMaxLowerDepth) {
        --exprLowerDepth_;
        // Return a dummy value to avoid cascading errors
        return RVal{Value::constInt(0), Type(Type::Kind::I64)};
    }

    struct DepthGuard {
        unsigned &d;

        ~DepthGuard() {
            --d;
        }
    } exprGuard_{exprLowerDepth_};

    curLoc = expr.loc;
    LowererExprVisitor visitor(*this);
    visitor.visitExpr(expr);
    return visitor.result();
}

/// @brief Lower an expression and coerce it to a scalar IL type.
/// @details Invokes @ref lowerExpr and then normalises the result to an integer
///          or floating type acceptable for scalar contexts (for example loop
///          bounds). The original source location is reused for any diagnostics
///          emitted during coercion.
/// @param expr Expression node expected to resolve to a scalar.
/// @return Coerced IL value and its scalar type.
Lowerer::RVal Lowerer::lowerScalarExpr(const Expr &expr) {
    return lowerScalarExpr(lowerExpr(expr), expr.loc);
}

/// @brief Coerce an already-lowered expression result into a scalar type.
/// @details Examines the value's static type and converts booleans and floating
///          values into 64-bit integers when required by the caller. Other types
///          are forwarded unchanged so complex lowering logic can handle them
///          separately.
/// @param value Result previously returned by @ref lowerExpr.
/// @param loc Source location used for diagnostics during coercion.
/// @return IL value adjusted for scalar contexts.
Lowerer::RVal Lowerer::lowerScalarExpr(RVal value, il::support::SourceLoc loc) {
    switch (value.type.kind) {
        case Type::Kind::I1:
        case Type::Kind::I16:
        case Type::Kind::I32:
        case Type::Kind::I64:
        case Type::Kind::F64:
            value = coerceToI64(std::move(value), loc);
            break;
        default:
            break;
    }
    return value;
}

// classifyNumericType is implemented in Lower_Expr_NumericClassifier.cpp

} // namespace il::frontends::basic
