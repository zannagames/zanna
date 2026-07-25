//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/oop/Lower_OOP_Emit.cpp
// Purpose: Emit constructor, destructor, and method bodies for BASIC CLASS nodes.
//
// What BASIC syntax it handles:
//   - CLASS ... END CLASS declarations with members
//   - Constructor (SUB NEW) bodies with parameter initialization
//   - Destructor (SUB DESTROY) bodies with field cleanup
//   - Method (FUNCTION/SUB) bodies with ME binding
//   - Property (GET/SET) accessor synthesis
//   - Static constructor ($static) initialization thunks
//   - Interface registration and binding thunks
//
// Invariants expected from Lowerer/LoweringContext:
//   - OopIndex must be fully populated with class/interface metadata
//   - ClassLayout cache must have computed field offsets and sizes
//   - IRBuilder must be available for function/block creation
//
// IL Builder interaction:
//   - Creates IL functions for ctor/dtor/method bodies
//   - Emits alloca for ME slot and parameter storage
//   - Generates vtable/itable population and registration calls
//   - Uses OopEmitHelper for consolidated emission patterns
//
// Key invariants: Functions bind the implicit ME parameter and share lowering
//                 scaffolding with procedure emission.
// Ownership/Lifetime: Operates on Lowerer state borrowed from the lowering
//                     pipeline; owns no persistent resources.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Emits BASIC class constructors, destructors, methods, properties,
///        static initialization, and interface bindings.

#include "frontends/basic/ILTypeUtils.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/NameMangler_OOP.hpp"
#include "frontends/basic/lower/oop/Lower_OOP_RuntimeHelpers.hpp"
#include "il/runtime/RuntimeSignatures.hpp"

#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace il::frontends::basic {
namespace {
using AstType = ::il::frontends::basic::Type;

/// @brief Extract raw statement pointers from an owning body list.
///
/// @details Constructor, destructor, and method declarations all store their
///          bodies as vectors of owning @ref StmtPtr values.  Lowering only needs
///          borrowed pointers because the @ref Lowerer never assumes ownership.
///          The helper strips the indirection while skipping null entries so
///          downstream passes receive a dense sequence of statements.
///
/// @param body Owning pointers sourced from the AST node.
/// @return Borrowed pointer list suitable for lowering routines.
[[nodiscard]] std::vector<const Stmt *> gatherBody(const std::vector<StmtPtr> &body) {
    std::vector<const Stmt *> out;
    out.reserve(body.size());
    for (const auto &stmt : body) {
        if (stmt)
            out.push_back(stmt.get());
    }
    return out;
}

class FieldScopeGuard {
  public:
    FieldScopeGuard(Lowerer &lowerer, const std::string &className) noexcept : lowerer_(lowerer) {
        lowerer_.pushFieldScope(className);
    }

    FieldScopeGuard(const FieldScopeGuard &) = delete;
    FieldScopeGuard &operator=(const FieldScopeGuard &) = delete;

    ~FieldScopeGuard() {
        lowerer_.popFieldScope();
    }

  private:
    Lowerer &lowerer_;
};

} // namespace

namespace {
class ClassContextGuard {
  public:
    explicit ClassContextGuard(Lowerer &lowerer, const std::string &qualifiedName)
        : lowerer_(lowerer) {
        lowerer_.pushClass(qualifiedName);
    }

    ClassContextGuard(const ClassContextGuard &) = delete;
    ClassContextGuard &operator=(const ClassContextGuard &) = delete;

    ~ClassContextGuard() {
        lowerer_.popClass();
    }

  private:
    Lowerer &lowerer_;
};
} // namespace

/// @brief Allocate and initialise the implicit @c ME slot for a class member.
///
/// @details BASIC object procedures implicitly capture @c ME, a pointer to the
///          current instance.  The routine reserves a stack slot, records the
///          slot identifier in the symbol table, and stores the incoming @c self
///          parameter so later field accesses can load from a stable location.
///          The lowering location is cleared because the slot materialisation is
///          synthetic and should not inherit the caller's source location.
///
/// @param className Name of the class being lowered (used for symbol metadata).
/// @param fn        Function currently under construction.
/// @return Identifier of the stack slot holding the @c ME pointer.
unsigned Lowerer::materializeSelfSlot(const std::string &className, Function &fn) {
    curLoc = {};
    setSymbolObjectType("ME", className);
    auto &info = ensureSymbol("ME");
    info.referenced = true;
    Value slot = emitAlloca(8);
    info.slotId = slot.id;
    emitStore(Type(Type::Kind::Ptr), slot, Value::temp(fn.params.front().id));
    return slot.id;
}

/// @brief Load the implicit @c ME pointer from the cached stack slot.
///
/// @details Resets the current source location because the operation is
///          compiler-generated, then emits a load from the previously
///          materialised slot.  Keeping the logic in a helper avoids duplicating
///          the slot bookkeeping across constructor, destructor, and method
///          bodies.
///
/// @param slotId Identifier of the @c ME stack slot materialised earlier.
/// @return Runtime value referencing the @c ME pointer.
Lowerer::Value Lowerer::loadSelfPointer(unsigned slotId) {
    curLoc = {};
    return emitLoad(Type(Type::Kind::Ptr), Value::temp(slotId));
}

/// @brief Release reference-counted fields during destructor emission.
///
/// @details Iterates over the cached @ref ClassLayout to determine which fields
///          require runtime release calls.  String fields trigger retain/release
///          helpers, and future field kinds can extend the switch without
///          altering destructor logic.  The helper uses @c curLoc resets so the
///          emitted instructions are treated as compiler-synthesised clean-up
///          rather than user code.
///
/// @param selfPtr Pointer to the object instance being destroyed.
/// @param layout  Metadata describing field offsets and types for the class.
void Lowerer::emitFieldReleaseSequence(Value selfPtr, const ClassLayout &layout) {
    for (const auto &field : layout.fields) {
        curLoc = {};
        Value fieldPtr = emitBinary(Opcode::GEP,
                                    Type(Type::Kind::Ptr),
                                    selfPtr,
                                    Value::constInt(static_cast<long long>(field.offset)));

        // BUG-099 fix: Handle object field release
        // BUG-105 fix: Distinguish object arrays from single objects
        if (!field.objectClassName.empty()) {
            Value fieldValue = emitLoad(Type(Type::Kind::Ptr), fieldPtr);
            if (field.isArray) {
                // Object array field: use rt_arr_obj_release
                requireArrayObjRelease();
                emitCall("rt_arr_obj_release", {fieldValue});
            } else {
                // Single object field: use rt_obj_release_check0
                requestRuntimeFeature(il::runtime::RuntimeFeature::ObjReleaseChk0);
                Value needsFree =
                    emitCallRet(Type(Type::Kind::I1), "rt_obj_release_check0", {fieldValue});
                (void)needsFree; // Destructor ignores the result
            }
            continue;
        }

        switch (field.type) {
            case AstType::Str: {
                Value fieldValue = emitLoad(Type(Type::Kind::Str), fieldPtr);
                requireStrReleaseMaybe();
                emitCall("rt_str_release_maybe", {fieldValue});
                break;
            }
            case AstType::I64:
            case AstType::F64:
            case AstType::Bool:
            default:
                break;
        }
    }
}

/// @brief Emit the IL body for a BASIC class constructor.
///
/// @details Resets lowering state, binds the implicit @c ME parameter, materialises
///          user parameters, and drives the lowering pipeline for the constructor
///          body.  Runtime helpers required for array parameters are requested,
///          and deterministic exits are enforced by branching to the synthetic
///          exit block when user code falls through.  The routine also handles
///          releasing transient allocations before returning.
///
/// @param klass Class definition providing layout and member metadata.
/// @param ctor  AST node describing the constructor body and parameters.
void Lowerer::emitClassConstructor(const ClassDecl &klass, const ConstructorDecl &ctor) {
    resetLoweringState();
    // BUG-BAS-002 fix: Register param names and types early so collectVars doesn't pollute them
    // with module-level object types of the same name and so type inference uses correct types.
    for (const auto &param : ctor.params) {
        registerProcParam(param.name);
        if (!param.objectClass.empty())
            setSymbolObjectType(param.name, qualify(param.objectClass));
        else
            setSymbolType(param.name, param.type);
    }
    ClassContextGuard classGuard(*this, qualify(klass.name));
    FieldScopeGuard fieldScope(*this, klass.name);
    auto body = gatherBody(ctor.body);
    collectVars(body);

    ProcedureMetadata metadata;
    metadata.paramCount = 1 + ctor.params.size();
    metadata.bodyStmts = body;
    metadata.irParams.push_back({"ME", Type(Type::Kind::Ptr)});
    for (const auto &param : ctor.params) {
        Type ilParamTy =
            param.is_array ? Type(Type::Kind::Ptr) : type_conv::astToIlType(param.type);
        metadata.irParams.push_back({param.name, ilParamTy});
        if (param.is_array) {
            requireArrayI64Retain();
            requireArrayI64Release();
        }
    }

    std::string name = mangleClassCtor(qualify(klass.name));
    Function &fn = builder->startFunction(name, Type(Type::Kind::Void), metadata.irParams);

    auto &ctx = context();
    ctx.setFunction(&fn);
    ctx.setNextTemp(fn.valueNames.size());

    buildProcedureSkeleton(fn, name, metadata);

    ctx.setCurrent(&fn.blocks.front());
    unsigned selfSlotId = materializeSelfSlot(klass.name, fn);

    // Initialize the object's vptr with the class's registered vtable.
    // The vtable is populated during __mod_init$oop, so we just retrieve it here.
    // This ensures rt_typeid_of can identify the object's type via vptr lookup.
    if (const ClassInfo *ciInit = oopIndex_.findClass(qualify(klass.name))) {
        // Get class type ID from layout
        auto itLayout = classLayouts_.find(ciInit->name);
        if (itLayout == classLayouts_.end())
            itLayout = classLayouts_.find(klass.name);
        if (itLayout != classLayouts_.end()) {
            const long long typeId = (long long)itLayout->second.classId;
            // Retrieve the registered vtable for this class
            Value vtblPtr = emitCallRet(
                Type(Type::Kind::Ptr), "rt_get_class_vtable", {Value::constInt(typeId)});
            // Store vptr into the object's header (offset 0)
            Value selfPtr = loadSelfPointer(selfSlotId);
            emitStore(Type(Type::Kind::Ptr), selfPtr, vtblPtr);
        }
    }
    // Initialize parameters using consolidated helper (BUG-073 fix)
    OopEmitHelper helper(*this);
    helper.emitAllParamInits(ctor.params, fn, /*selfOffset=*/1, metadata.paramNames);
    allocateLocalSlots(metadata.paramNames, /*includeParams=*/false);

    // BUG-056, BUG-089: Initialize array fields declared with extents.
    helper.emitArrayFieldInits(klass, selfSlotId);

    // Do not cache pointers into blocks vector; later addBlock() may reallocate.
    const size_t exitIdx = ctx.exitIndex();

    // Lower body and branch to exit using consolidated helper
    helper.emitBodyAndBranchToExit(metadata.bodyStmts, exitIdx);

    Function *func = ctx.function();
    ctx.setCurrent(&func->blocks[exitIdx]);

    // Release resources using consolidated epilogue helper (BUG-105 fix)
    helper.emitMethodEpilogue(metadata.paramNames, metadata.paramNames);
    curLoc = {};
    emitRetVoid();
    ctx.blockNames().resetNamer();
}

/// @brief Emit the IL body for a BASIC class destructor.
///
/// @details Lowers the optional user-defined destructor body, falls back to an
///          empty body when absent, and always invokes
///          @ref emitFieldReleaseSequence to clean up reference-counted fields.
///          Locals and parameters are released before returning to honour BASIC's
///          deterministic destruction semantics.
///
/// @param klass    Class definition whose instance is being destroyed.
/// @param userDtor Optional AST node for the user-authored destructor body.
void Lowerer::emitClassDestructor(const ClassDecl &klass, const DestructorDecl *userDtor) {
    resetLoweringState();
    ClassContextGuard classGuard(*this, qualify(klass.name));
    FieldScopeGuard fieldScope(*this, klass.name);
    std::vector<const Stmt *> body;
    if (userDtor) {
        body = gatherBody(userDtor->body);
        collectVars(body);
    }

    ProcedureMetadata metadata;
    metadata.paramCount = 1;
    metadata.bodyStmts = body;
    metadata.irParams.push_back({"ME", Type(Type::Kind::Ptr)});

    std::string name = mangleClassDtor(qualify(klass.name));
    Function &fn = builder->startFunction(name, Type(Type::Kind::Void), metadata.irParams);

    auto &ctx = context();
    ctx.setFunction(&fn);
    ctx.setNextTemp(fn.valueNames.size());

    buildProcedureSkeleton(fn, name, metadata);

    ctx.setCurrent(&fn.blocks.front());
    unsigned selfSlotId = materializeSelfSlot(klass.name, fn);
    allocateLocalSlots(metadata.paramNames, /*includeParams=*/false);

    // Do not cache pointers into blocks vector; later addBlock() may reallocate.
    const size_t exitIdx = ctx.exitIndex();

    // Lower body and branch to exit using consolidated helper
    OopEmitHelper helper(*this);
    helper.emitBodyAndBranchToExit(metadata.bodyStmts, exitIdx);

    Function *func = ctx.function();
    ctx.setCurrent(&func->blocks[exitIdx]);
    curLoc = {};

    // Release fields (destructor-specific)
    Value selfPtr = loadSelfPointer(selfSlotId);
    if (const ClassLayout *layout = findClassLayout(klass.name))
        emitFieldReleaseSequence(selfPtr, *layout);

    // Chain to base class destructor if the class inherits from another class.
    // This ensures derived destructor body runs first, then base destructor body,
    // matching C++/Java/C# semantics (derived cleanup before base teardown).
    if (klass.baseName.has_value() && !klass.baseName->empty()) {
        // Look up the base class in the OOP index to verify it has a destructor
        const ClassInfo *baseInfo = oopIndex_.findClass(*klass.baseName);
        if (baseInfo) {
            // Use the qualified name from the index for mangling
            const std::string &baseQualified =
                baseInfo->qualifiedName.empty() ? *klass.baseName : baseInfo->qualifiedName;
            // Always chain to base destructor — even if the base has no user-defined
            // destructor body, its synthesized __dtor still handles field cleanup.
            curLoc = {};
            std::string baseDtorName = mangleClassDtor(baseQualified);
            emitCall(baseDtorName, {selfPtr});
        }
    }

    // Release resources using consolidated epilogue helper (BUG-105 fix)
    helper.emitMethodEpilogue(metadata.paramNames, metadata.paramNames);
    curLoc = {};
    emitRetVoid();
    ctx.blockNames().resetNamer();
}

/// @brief Emit the IL body for a BASIC class method.
///
/// @details Mirrors constructor emission by setting up the @c ME slot, mapping
///          user parameters to stack slots, and invoking the standard statement
///          lowering sequence.  The helper also ensures array parameters request
///          their runtime retain/release helpers and that fallthrough paths branch
///          to the synthesised exit block.
///
/// @param klass  Class definition that owns the method.
/// @param method AST node describing the method signature and body.
void Lowerer::emitClassMethod(const ClassDecl &klass, const MethodDecl &method) {
    resetLoweringState();
    // BUG-BAS-002 fix: Register param names and types early so collectVars doesn't pollute them
    // with module-level object types of the same name and so type inference uses correct types.
    for (const auto &param : method.params) {
        registerProcParam(param.name);
        if (!param.objectClass.empty())
            setSymbolObjectType(param.name, qualify(param.objectClass));
        else
            setSymbolType(param.name, param.type);
    }
    ClassContextGuard classGuard(*this, qualify(klass.name));
    FieldScopeGuard fieldScope(*this, klass.name);
    auto body = gatherBody(method.body);
    collectVars(body);

    ProcedureMetadata metadata;
    metadata.paramCount = (method.isStatic ? 0 : 1) + method.params.size();
    metadata.bodyStmts = body;
    if (!method.isStatic)
        metadata.irParams.push_back({"ME", Type(Type::Kind::Ptr)});
    for (const auto &param : method.params) {
        // Object-typed parameters should use pointer IL type regardless of AST primitive default
        const bool isObjectParam = !param.objectClass.empty();
        Type ilParamTy = (param.is_array || isObjectParam) ? Type(Type::Kind::Ptr)
                                                           : type_conv::astToIlType(param.type);
        metadata.irParams.push_back({param.name, ilParamTy});
        if (param.is_array) {
            requireArrayI64Retain();
            requireArrayI64Release();
        }
    }

    const bool returnsValue = method.ret.has_value();
    const bool returnsObject = !method.explicitClassRetQname.empty();
    Type methodRetType = Type(Type::Kind::Void);
    std::optional<::il::frontends::basic::Type> methodRetAst;
    if (returnsValue || returnsObject) {
        // BUG-099 fix: Methods returning objects should use ptr type, not i64
        if (returnsObject) {
            methodRetType = Type(Type::Kind::Ptr);
            // Mark the return value symbol as an object type
            if (findSymbol(method.name)) {
                std::string qualifiedClassName;
                for (size_t i = 0; i < method.explicitClassRetQname.size(); ++i) {
                    if (i > 0)
                        qualifiedClassName += ".";
                    qualifiedClassName += method.explicitClassRetQname[i];
                }
                setSymbolObjectType(method.name, qualifiedClassName);
            }
        } else if (returnsValue) {
            methodRetType = type_conv::astToIlType(*method.ret);
            methodRetAst = method.ret;
            // BUG-084 fix: Set the return type for the method name symbol (VB-style implicit
            // return). This ensures the function return value slot is allocated with the correct
            // type. Must be done after collectVars() but before allocateLocalSlots().
            if (findSymbol(method.name)) {
                setSymbolType(method.name, *method.ret);
            }
        }
    }

    std::string name = mangleMethod(qualify(klass.name), method.name);
    Function &fn = builder->startFunction(name, methodRetType, metadata.irParams);

    auto &ctx = context();
    ctx.setFunction(&fn);
    ctx.setNextTemp(fn.valueNames.size());

    buildProcedureSkeleton(fn, name, metadata);

    ctx.setCurrent(&fn.blocks.front());
    if (!method.isStatic)
        materializeSelfSlot(klass.name, fn);

    // Initialize parameters using consolidated helper (BUG-073 fix)
    OopEmitHelper helper(*this);
    const std::size_t selfOffset = method.isStatic ? 0 : 1;
    helper.emitAllParamInits(method.params, fn, selfOffset, metadata.paramNames);
    allocateLocalSlots(metadata.paramNames, /*includeParams=*/false);

    // Do not cache pointers into blocks vector; later addBlock() may reallocate.
    const size_t exitIdx = ctx.exitIndex();

    // Lower body and branch to exit using consolidated helper
    helper.emitBodyAndBranchToExit(metadata.bodyStmts, exitIdx);

    Function *func = ctx.function();
    ctx.setCurrent(&func->blocks[exitIdx]);

    // BUG-099 fix: Exclude method name from release if it returns an object
    std::unordered_set<std::string> excludeNames = metadata.paramNames;
    if (returnsObject)
        excludeNames.insert(method.name);

    // Release resources using consolidated epilogue helper (BUG-105 fix)
    helper.emitMethodEpilogue(metadata.paramNames, excludeNames);
    curLoc = {};
    if (returnsValue) {
        Value retValue = Value::constInt(0);
        // BUG-068 fix: Check for VB-style implicit return via function name assignment
        auto methodNameSym = findSymbol(method.name);
        if (methodNameSym && methodNameSym->slotId.has_value()) {
            // Function name was assigned - load from that variable
            // BUG-099 fix: Use methodRetType which handles object returns correctly
            Value slot = Value::temp(*methodNameSym->slotId);
            retValue = emitLoad(methodRetType, slot);
        } else {
            // No assignment - use default value
            switch (*methodRetAst) {
                case ::il::frontends::basic::Type::I64:
                    retValue = Value::constInt(0);
                    break;
                case ::il::frontends::basic::Type::F64:
                    retValue = Value::constFloat(0.0);
                    break;
                case ::il::frontends::basic::Type::Str:
                    retValue = emitConstStr(getStringLabel(""));
                    break;
                case ::il::frontends::basic::Type::Bool:
                    retValue = emitBoolConst(false);
                    break;
            }
        }
        emitRet(retValue);
    } else
        emitRetVoid();
    ctx.blockNames().resetNamer();
}

void Lowerer::emitClassMethodWithBody(const ClassDecl &klass,
                                      const MethodDecl &method,
                                      const std::vector<const Stmt *> &bodyStmts) {
    resetLoweringState();
    // BUG-BAS-002 fix: Register param names and types early so collectVars doesn't pollute them
    // with module-level object types of the same name and so type inference uses correct types.
    for (const auto &param : method.params) {
        registerProcParam(param.name);
        if (!param.objectClass.empty())
            setSymbolObjectType(param.name, qualify(param.objectClass));
        else
            setSymbolType(param.name, param.type);
    }
    ClassContextGuard classGuard(*this, qualify(klass.name));
    FieldScopeGuard fieldScope(*this, klass.name);
    collectVars(bodyStmts);

    ProcedureMetadata metadata;
    metadata.paramCount = (method.isStatic ? 0 : 1) + method.params.size();
    metadata.bodyStmts = bodyStmts;
    if (!method.isStatic)
        metadata.irParams.push_back({"ME", Type(Type::Kind::Ptr)});
    for (const auto &param : method.params) {
        const bool isObjectParam = !param.objectClass.empty();
        Type ilParamTy = (param.is_array || isObjectParam) ? Type(Type::Kind::Ptr)
                                                           : type_conv::astToIlType(param.type);
        metadata.irParams.push_back({param.name, ilParamTy});
        if (param.is_array) {
            requireArrayI64Retain();
            requireArrayI64Release();
        }
    }

    const bool returnsValue = method.ret.has_value();
    const bool returnsObject = !method.explicitClassRetQname.empty();
    Type methodRetType = Type(Type::Kind::Void);
    std::optional<::il::frontends::basic::Type> methodRetAst;
    if (returnsValue || returnsObject) {
        if (returnsObject) {
            methodRetType = Type(Type::Kind::Ptr);
            if (findSymbol(method.name)) {
                std::string qualifiedClassName;
                for (size_t i = 0; i < method.explicitClassRetQname.size(); ++i) {
                    if (i > 0)
                        qualifiedClassName += ".";
                    qualifiedClassName += method.explicitClassRetQname[i];
                }
                setSymbolObjectType(method.name, qualifiedClassName);
            }
        } else if (returnsValue) {
            methodRetType = type_conv::astToIlType(*method.ret);
            methodRetAst = method.ret;
            if (findSymbol(method.name)) {
                setSymbolType(method.name, *method.ret);
            }
        }
    }

    std::string name = mangleMethod(qualify(klass.name), method.name);
    Function &fn = builder->startFunction(name, methodRetType, metadata.irParams);

    auto &ctx = context();
    ctx.setFunction(&fn);
    ctx.setNextTemp(fn.valueNames.size());

    buildProcedureSkeleton(fn, name, metadata);

    ctx.setCurrent(&fn.blocks.front());
    if (!method.isStatic)
        materializeSelfSlot(klass.name, fn);

    // Initialize parameters using consolidated helper (BUG-073 fix)
    OopEmitHelper helper(*this);
    const std::size_t selfOffset = method.isStatic ? 0 : 1;
    helper.emitAllParamInits(method.params, fn, selfOffset, metadata.paramNames);
    allocateLocalSlots(metadata.paramNames, /*includeParams=*/false);

    const size_t exitIdx = ctx.exitIndex();

    // Lower body and branch to exit using consolidated helper
    helper.emitBodyAndBranchToExit(metadata.bodyStmts, exitIdx);

    Function *func = ctx.function();
    ctx.setCurrent(&func->blocks[exitIdx]);

    // BUG-099 fix: Exclude method name from release if it returns an object
    std::unordered_set<std::string> excludeNames = metadata.paramNames;
    if (returnsObject)
        excludeNames.insert(method.name);

    // Release resources using consolidated epilogue helper (BUG-105 fix)
    helper.emitMethodEpilogue(metadata.paramNames, excludeNames);
    curLoc = {};
    if (returnsValue) {
        Value retValue = Value::constInt(0);
        auto methodNameSym = findSymbol(method.name);
        if (methodNameSym && methodNameSym->slotId.has_value()) {
            Value slot = Value::temp(*methodNameSym->slotId);
            retValue = emitLoad(methodRetType, slot);
        } else if (methodRetAst) {
            switch (*methodRetAst) {
                case ::il::frontends::basic::Type::I64:
                    retValue = Value::constInt(0);
                    break;
                case ::il::frontends::basic::Type::F64:
                    retValue = Value::constFloat(0.0);
                    break;
                case ::il::frontends::basic::Type::Str:
                    retValue = emitConstStr(getStringLabel(""));
                    break;
                case ::il::frontends::basic::Type::Bool:
                    retValue = emitBoolConst(false);
                    break;
            }
        }
        emitRet(retValue);
    } else
        emitRetVoid();
    ctx.blockNames().resetNamer();
}

/// @brief Lower all class declarations and their members within a program.
///
/// @details Iterates the top-level statements looking for CLASS declarations,
///          gathers their constructor, destructor, and method members, and then
///          emits each body using the dedicated helpers.  This ensures object
///          members are materialised before ordinary procedures so runtime
///          helpers and mangled names are available to subsequent lowering steps.
///
/// @param prog BASIC program containing potential CLASS declarations.
void Lowerer::emitOopDeclsAndBodies(const Program &prog) {
    if (!builder)
        return;

    // Emit module-scope globals for static fields in all classes (once per module)
    for (const auto &entry : oopIndex_.classes()) {
        const ClassInfo &ci = entry.second;
        for (const auto &sf : ci.staticFields) {
            il::core::Global g;
            // Use qualified class name to keep names unique and readable
            g.name = ci.qualifiedName + "::" + sf.name;
            // Object/static string fields are pointers/strings in IL
            if (!sf.objectClassName.empty())
                g.type = Type(Type::Kind::Ptr);
            else
                g.type = type_conv::astToIlType(sf.type);
            g.init = std::string(); // zero-initialized by default
            mod->globals.push_back(std::move(g));
        }
    }

    // Walk the program and nested namespaces to emit class/interface members.
    std::function<void(const std::vector<StmtPtr> &)> scan;
    scan = [&](const std::vector<StmtPtr> &stmts) {
        for (const auto &stmt : stmts) {
            if (!stmt)
                continue;
            if (stmt->stmtKind() == Stmt::Kind::NamespaceDecl) {
                const auto &ns = static_cast<const NamespaceDecl &>(*stmt);
                // Enter namespace for qualification
                pushNamespace(ns.path);
                scan(ns.body);
                // Leave namespace
                popNamespace(ns.path.size());
                continue;
            }
            if (stmt->stmtKind() != Stmt::Kind::ClassDecl)
                continue;

            const auto &klass = static_cast<const ClassDecl &>(*stmt);
            const ConstructorDecl *ctor = nullptr;
            const ConstructorDecl *staticCtor = nullptr;
            const DestructorDecl *dtor = nullptr;
            std::vector<const MethodDecl *> methods;
            methods.reserve(klass.members.size());

            for (const auto &member : klass.members) {
                if (!member)
                    continue;
                switch (member->stmtKind()) {
                    case Stmt::Kind::ConstructorDecl: {
                        auto *c = static_cast<const ConstructorDecl *>(member.get());
                        if (c->isStatic)
                            staticCtor = c;
                        else
                            ctor = c;
                        break;
                    }
                    case Stmt::Kind::DestructorDecl:
                        dtor = static_cast<const DestructorDecl *>(member.get());
                        break;
                    case Stmt::Kind::MethodDecl:
                        methods.push_back(static_cast<const MethodDecl *>(member.get()));
                        break;
                    case Stmt::Kind::PropertyDecl: {
                        const auto *prop = static_cast<const PropertyDecl *>(member.get());
                        // Synthesize and emit getter
                        if (prop->get.present) {
                            MethodDecl getter;
                            getter.loc = prop->loc;
                            getter.name = std::string("get_") + prop->name;
                            getter.access = prop->get.access;
                            getter.params = {}; // no extra params
                            getter.ret = prop->type;
                            getter.isStatic = prop->isStatic;
                            auto bodyStmts = gatherBody(prop->get.body);
                            emitClassMethodWithBody(klass, getter, bodyStmts);
                        }
                        // Synthesize and emit setter
                        if (prop->set.present) {
                            MethodDecl setter;
                            setter.loc = prop->loc;
                            setter.name = std::string("set_") + prop->name;
                            setter.access = prop->set.access;
                            Param p;
                            p.name = prop->set.paramName;
                            p.type = prop->type;
                            setter.params = {p};
                            setter.isStatic = prop->isStatic;
                            auto bodyStmts = gatherBody(prop->set.body);
                            emitClassMethodWithBody(klass, setter, bodyStmts);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }

            if (ctor) {
                emitClassConstructor(klass, *ctor);
            } else {
                const ClassInfo *info = oopIndex_.findClass(klass.name);
                if (info && info->hasSynthCtor) {
                    ConstructorDecl synthCtor;
                    synthCtor.loc = klass.loc;
                    synthCtor.line = klass.line;
                    emitClassConstructor(klass, synthCtor);
                }
            }
            emitClassDestructor(klass, dtor);
            for (const auto *method : methods)
                emitClassMethod(klass, *method);

            // Emit static constructor thunk and register in module-init
            if (staticCtor) {
                resetLoweringState();
                ClassContextGuard classGuard(*this, qualify(klass.name));
                auto body = gatherBody(staticCtor->body);
                collectVars(body);
                ProcedureMetadata metadata;
                metadata.paramCount = 0;
                metadata.bodyStmts = body;
                const std::string cctorName = mangleClassCtor(qualify(klass.name)) + "$static";
                Function &fn = builder->startFunction(cctorName, Type(Type::Kind::Void), {});
                context().setFunction(&fn);
                context().setNextTemp(fn.valueNames.size());
                buildProcedureSkeleton(fn, cctorName, metadata);
                context().setCurrent(&fn.blocks.front());
                // Lower static ctor body
                lowerStatementSequence(metadata.bodyStmts, /*stopOnTerminated=*/true);
                if (context().current() && !context().current()->terminated) {
                    Function *func = context().function();
                    BasicBlock *exitBlock = &func->blocks[context().exitIndex()];
                    emitBr(exitBlock);
                }
                Function *func = context().function();
                context().setCurrent(&func->blocks[context().exitIndex()]);
                emitRetVoid();
                // Mark for module init call
                procNameAliases[cctorName] = "__static_ctor";
            }
        }
    };

    // Start recursive scan from the program's main statements.
    scan(prog.main);

    // Synthesize interface registration, binding thunks, and a module init.
    std::vector<std::string> regThunks = emitInterfaceRegThunks();
    std::vector<std::string> bindThunks = emitInterfaceBindThunks();

    // 3) Module init that calls iface reg thunks, then bind thunks.
    const std::string initName = mangleOopModuleInit();
    Function &initF = builder->startFunction(initName, Type(Type::Kind::Void), {});
    context().setFunction(&initF);
    context().setNextTemp(initF.valueNames.size());
    // Create entry block for module init body
    builder->addBlock(initF, "entry");
    context().setCurrent(&initF.blocks.front());
    initF.blocks.front().terminated = false;

    // Register each class with its qualified name so Object.ToString works
    // and is-a relationships are preserved for IS operator.
    // Also populate vtables here so rt_get_class_vtable can retrieve them.
    //
    // IMPORTANT: Classes must be registered in topological order (base before derived)
    // so that rt_register_class_with_base can look up the base class at registration time.
    std::vector<std::string> classOrder;
    {
        std::unordered_set<std::string> registered;
        std::function<void(const std::string &)> registerInOrder = [&](const std::string &qname) {
            if (registered.count(qname))
                return;
            const ClassInfo *ci = oopIndex_.findClass(qname);
            if (!ci)
                return;
            // Register base class first if it exists
            if (!ci->baseQualified.empty())
                registerInOrder(ci->baseQualified);
            classOrder.push_back(qname);
            registered.insert(qname);
        };
        for (const auto &entry : oopIndex_.classes())
            registerInOrder(entry.second.qualifiedName);
    }

    for (const std::string &qname : classOrder) {
        const ClassInfo *ciPtr = oopIndex_.findClass(qname);
        if (!ciPtr)
            continue;
        const ClassInfo &ci = *ciPtr;
        auto itLayout = classLayouts_.find(ci.name);
        if (itLayout == classLayouts_.end())
            continue;
        const long long typeId = (long long)itLayout->second.classId;

        // Compute vtable size from virtual slot metadata
        std::size_t maxSlot = 0;
        bool hasAnyVirtual = false;
        {
            const ClassInfo *cur = &ci;
            while (cur) {
                for (const auto &mp : cur->methods) {
                    const auto &mi = mp.second;
                    if (!mi.isVirtual || mi.slot < 0)
                        continue;
                    hasAnyVirtual = true;
                    maxSlot = std::max<std::size_t>(maxSlot, static_cast<std::size_t>(mi.slot));
                }
                if (cur->baseQualified.empty())
                    break;
                cur = oopIndex_.findClass(cur->baseQualified);
            }
        }

        const std::size_t slotCount = hasAnyVirtual ? (maxSlot + 1) : 0;
        const long long bytes =
            slotCount > 0 ? static_cast<long long>(slotCount * 8ULL) : 8LL; // at least 8 bytes
        Value vtablePtr = emitCallRet(Type(Type::Kind::Ptr), "rt_alloc", {Value::constInt(bytes)});

        // Populate vtable slots with method pointers
        if (slotCount > 0) {
            std::size_t unusedMaxSlot = 0;
            std::vector<std::string> slotToName =
                OopEmitHelper::buildVtableSlotMap(oopIndex_, ci.qualifiedName, unusedMaxSlot);

            for (std::size_t s = 0; s < slotCount; ++s) {
                const long long offset = static_cast<long long>(s * 8ULL);
                Value slotPtr = emitBinary(
                    Opcode::GEP, Type(Type::Kind::Ptr), vtablePtr, Value::constInt(offset));
                const std::string &mname = (s < slotToName.size()) ? slotToName[s] : "";
                if (mname.empty()) {
                    emitStore(Type(Type::Kind::Ptr), slotPtr, Value::null());
                } else {
                    const std::string implQ =
                        OopEmitHelper::findImplementorClass(oopIndex_, ci.qualifiedName, mname);
                    const std::string target = mangleMethod(implQ, mname);
                    emitStore(Type(Type::Kind::Ptr), slotPtr, Value::global(target));
                }
            }
        }

        // Determine base class type ID (-1 if no base)
        long long baseTypeId = -1;
        if (!ci.baseQualified.empty()) {
            // Look up base class in layout table
            const ClassInfo *baseCi = oopIndex_.findClass(ci.baseQualified);
            if (baseCi) {
                auto itBase = classLayouts_.find(baseCi->name);
                if (itBase != classLayouts_.end())
                    baseTypeId = (long long)itBase->second.classId;
            }
        }

        // Register the class with rt_register_class_with_base_rs(typeId, vtable, qname, slotCount,
        // baseTypeId) Note: Use _rs variant which accepts rt_string, not const char*
        std::string qnameLabel = getStringLabel(ci.qualifiedName);
        emitCall("rt_register_class_with_base_rs",
                 {Value::constInt(typeId),
                  vtablePtr,
                  emitConstStr(qnameLabel),
                  Value::constInt(static_cast<long long>(slotCount)),
                  Value::constInt(baseTypeId)});
    }

    for (const auto &fn : regThunks)
        emitCall(fn, {});
    for (const auto &fn : bindThunks)
        emitCall(fn, {});
    // Call per-class static constructors in class declaration order
    for (const auto &entry : oopIndex_.classes()) {
        const ClassInfo &ci = entry.second;
        if (ci.hasStaticCtor) {
            const std::string cctorName = mangleClassCtor(ci.qualifiedName) + "$static";
            emitCall(cctorName, {});
        }
    }
    emitRetVoid();

    // Call module init at the start of main by emitting a call in program emission.
    // Note: Program emission will run after this; ensure ProgramLowering invokes this init.
}

std::vector<std::string> Lowerer::emitInterfaceRegThunks() {
    std::vector<std::string> regThunks;
    for (const auto &p : oopIndex_.interfacesByQname()) {
        const std::string &qname = p.first;
        const InterfaceInfo &iface = p.second;
        const std::string fn = mangleIfaceRegThunk(qname);
        regThunks.push_back(fn);
        // fn(): void
        Function &f = builder->startFunction(fn, Type(Type::Kind::Void), {});
        context().setFunction(&f);
        context().setNextTemp(f.valueNames.size());
        // Create entry block for thunk body
        builder->addBlock(f, "entry");
        context().setCurrent(&f.blocks.front());
        f.blocks.front().terminated = false;
        // Call the runtime-string bridge so IL `str` is not passed to a raw pointer ABI.
        std::string qnameLabel = getStringLabel(qname);
        emitCall("rt_register_interface_direct_rs",
                 {Value::constInt(iface.ifaceId),
                  emitConstStr(qnameLabel),
                  Value::constInt((long long)iface.slots.size())});
        emitRetVoid();
    }
    return regThunks;
}

std::vector<std::string> Lowerer::emitInterfaceBindThunks() {
    std::vector<std::string> bindThunks;
    for (const auto &entry : oopIndex_.classes()) {
        const ClassInfo &ci = entry.second;
        // Resolve type id from class layout cache (by unqualified name)
        auto itLayout = classLayouts_.find(ci.name);
        if (itLayout == classLayouts_.end())
            continue;
        const long long typeId = (long long)itLayout->second.classId;
        for (int ifaceId : ci.implementedInterfaces) {
            // Find iface qname
            const InterfaceInfo *iface = nullptr;
            for (const auto &ip : oopIndex_.interfacesByQname()) {
                if (ip.second.ifaceId == ifaceId) {
                    iface = &ip.second;
                    break;
                }
            }
            if (!iface)
                continue;
            const std::string thunk = mangleIfaceBindThunk(ci.qualifiedName, iface->qualifiedName);
            bindThunks.push_back(thunk);
            Function &fb = builder->startFunction(thunk, Type(Type::Kind::Void), {});
            context().setFunction(&fb);
            context().setNextTemp(fb.valueNames.size());
            // Create entry block for thunk body
            builder->addBlock(fb, "entry");
            context().setCurrent(&fb.blocks.front());
            fb.blocks.front().terminated = false;
            // Allocate a persistent itable: slot_count * sizeof(void*)
            const std::size_t slotCount = iface->slots.size();
            const long long bytes = static_cast<long long>(slotCount * 8ULL);
            Value itablePtr =
                emitCallRet(Type(Type::Kind::Ptr), "rt_alloc", {Value::constInt(bytes)});

            // Populate itable slots in interface slot order using consolidated helper
            auto mapIt = ci.ifaceSlotImpl.find(ifaceId);
            for (std::size_t s = 0; s < slotCount; ++s) {
                const long long offset = static_cast<long long>(s * 8ULL);
                Value slotPtr = emitBinary(
                    Opcode::GEP, Type(Type::Kind::Ptr), itablePtr, Value::constInt(offset));
                // Resolve method name for this slot; may be empty for abstract/missing
                std::string mname;
                if (mapIt != ci.ifaceSlotImpl.end() && s < mapIt->second.size())
                    mname = mapIt->second[s];
                if (mname.empty()) {
                    // Store null for missing implementations (keeps layout deterministic)
                    emitStore(Type(Type::Kind::Ptr), slotPtr, Value::null());
                } else {
                    const std::string implQ =
                        OopEmitHelper::findImplementorClass(oopIndex_, ci.qualifiedName, mname);
                    const std::string targetLabel = mangleMethod(implQ, mname);
                    emitStore(Type(Type::Kind::Ptr), slotPtr, Value::global(targetLabel));
                }
            }

            // Bind the populated itable to (typeId, ifaceId)
            emitCall("rt_bind_interface",
                     {Value::constInt(typeId), Value::constInt((long long)ifaceId), itablePtr});
            emitRetVoid();
        }
    }
    return bindThunks;
}

} // namespace il::frontends::basic
