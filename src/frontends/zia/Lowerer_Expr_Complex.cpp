//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Expr_Complex.cpp
/// @brief Lowers Zia field access and object/value construction expressions.
///
/// @details Field resolution covers properties, enum variants, module symbols,
///          error metadata, stored struct/class fields, collection properties,
///          and runtime-class getters. Construction dispatches among built-in
///          collections, cataloged runtime classes, stack-allocated value
///          types, and heap-allocated user classes while honoring resolved
///          constructor bindings, field defaults, coercions, and ownership.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

namespace il::frontends::zia {

using namespace runtime;

namespace {

/// @brief Append @p typeName's field declarations (base-class fields first,
///        then this class's) to @p out — gives the in-memory field order used
///        when lowering class construction/initialization.
/// @param sema Semantic analyzer used to resolve class declarations.
/// @param typeName Qualified class name whose fields are collected.
/// @param out Destination vector extended with inherited and local non-static
///        fields.
void appendClassFieldDecls(Sema &sema,
                           const std::string &typeName,
                           std::vector<const FieldDecl *> &out) {
    ClassDecl *decl = sema.findClassDecl(typeName);
    if (!decl)
        return;
    if (!decl->baseClass.empty())
        appendClassFieldDecls(sema, decl->baseClass, out);
    for (const auto &member : decl->members) {
        if (member->kind != DeclKind::Field)
            continue;
        auto *field = static_cast<FieldDecl *>(member.get());
        if (!field->isStatic)
            out.push_back(field);
    }
}

/// @brief Collect a struct type's non-static field declarations in declaration order.
/// @param sema Semantic analyzer used to find the struct declaration.
/// @param typeName Qualified struct type name.
/// @return The field declarations, or an empty vector if the type is unknown.
std::vector<const FieldDecl *> collectStructFieldDecls(Sema &sema, const std::string &typeName) {
    std::vector<const FieldDecl *> out;
    StructDecl *decl = sema.findStructDecl(typeName);
    if (!decl)
        return out;
    for (const auto &member : decl->members) {
        if (member->kind != DeclKind::Field)
            continue;
        auto *field = static_cast<FieldDecl *>(member.get());
        if (!field->isStatic)
            out.push_back(field);
    }
    return out;
}

} // namespace

//=============================================================================
// Field Expression Lowering
//=============================================================================

/// @brief Lower a field-access (`base.field`) expression.
/// @param expr Field expression.
/// @return The accessed value and its IL type.
/// @details Resolves the access in priority order: synthesized property getter (runtime or
///          user-defined), dotted enum variant, then by base type — enum variant constant,
///          module-qualified constant/global/function, `Error` fields (kind/message/code/...),
///          struct/class instance fields, built-in `Length`/`Count` properties for
///          string/list/map/set, and runtime-class `get_<Prop>` getters. Unknown fields fall
///          back to a zero value typed from sema (BUG-FE-006 safety net).
LowerResult Lowerer::lowerField(FieldExpr *expr) {
    /// @brief Reconstructs a dotted identifier/field expression name.
    /// @param node Expression node to inspect.
    /// @param[out] out Receives the reconstructed name.
    /// @param self Recursive callback reference.
    /// @return `true` when the expression is a pure name chain.
    auto dottedName = [](Expr *node, std::string &out, auto &self) -> bool {
        if (auto *ident = dynamic_cast<IdentExpr *>(node)) {
            out = ident->name;
            return true;
        }
        if (auto *field = dynamic_cast<FieldExpr *>(node)) {
            std::string base;
            if (!self(field->base.get(), base, self))
                return false;
            out = base + "." + field->field;
            return true;
        }
        return false;
    };

    // Property access lowers to a synthesized getter call for both runtime and
    // user-defined properties.
    std::string getterName = sema_.resolvedFieldGetter(expr);
    if (!getterName.empty()) {
        TypeRef resultType = sema_.typeOf(expr);
        Type ilType = mapType(resultType);
        TypeRef baseType = sema_.typeOf(expr->base.get());
        std::vector<Value> args;
        const auto *getterDesc = il::runtime::findRuntimeDescriptor(getterName);
        bool staticRuntimeGetter = getterDesc && getterDesc->signature.paramTypes.empty();
        if (!staticRuntimeGetter && (!baseType || baseType->kind != TypeKindSem::Module)) {
            auto base = lowerExpr(expr->base.get());
            args.push_back(base.value);
        }
        Value result = emitCallRet(ilType, getterName, args);
        return {result, ilType};
    }

    // Handle dotted enum variant access even when the base expression itself was
    // not cached as an Enum type during semantic analysis (e.g. Color.Red).
    std::string dottedBase;
    if (dottedName(expr->base.get(), dottedBase, dottedName)) {
        std::string key = dottedBase + "." + expr->field;
        auto it = enumVariantValues_.find(key);
        if (it != enumVariantValues_.end())
            return {Value::constInt(it->second), Type(Type::Kind::I64)};
    }

    // Get the type of the base expression first (before lowering)
    TypeRef baseType = sema_.typeOf(expr->base.get());
    if (!baseType) {
        return {Value::constInt(0), Type(Type::Kind::I64)};
    }

    // Unwrap Optional types for field access
    // This handles variables assigned from optionals after null checks
    // (e.g., `var col = maybeCol;` where maybeCol is Column?)
    if (baseType->kind == TypeKindSem::Optional && baseType->innerType()) {
        baseType = baseType->innerType();
    }

    // Handle enum variant access (e.g., Color.Red) -- emit I64 constant
    if (baseType->kind == TypeKindSem::Enum) {
        std::string key = baseType->name + "." + expr->field;
        auto it = enumVariantValues_.find(key);
        if (it != enumVariantValues_.end()) {
            return {Value::constInt(it->second), Type(Type::Kind::I64)};
        }
        // Fallthrough should not happen (Sema catches unknown variants)
        return {Value::constInt(0), Type(Type::Kind::I64)};
    }

    // Handle module-qualified identifier access (e.g., colors.Black)
    // The module is just a namespace - we load the symbol directly
    if (baseType->kind == TypeKindSem::Module) {
        // Look up the symbol as a global variable or function. Static fields
        // are stored under their fully qualified Type.field names.
        std::string resolvedSymbolName = sema_.resolvedFieldSymbolName(expr);
        std::string qualifiedSymbolName =
            resolvedSymbolName.empty() ? baseType->name + "." + expr->field : resolvedSymbolName;
        std::string symbolName = expr->field;

        // Check for global constants first (compile-time constants)
        auto constIt = globalConstants_.find(qualifiedSymbolName);
        if (constIt == globalConstants_.end())
            constIt = globalConstants_.find(symbolName);
        if (constIt != globalConstants_.end()) {
            const Value &val = constIt->second;
            Type ilType;
            switch (val.kind) {
                case Value::Kind::ConstFloat:
                    ilType = Type(Type::Kind::F64);
                    break;
                case Value::Kind::ConstStr: {
                    Value loaded = emitConstStr(val.str);
                    return {loaded, Type(Type::Kind::Str)};
                }
                case Value::Kind::ConstInt:
                    ilType = val.isBool ? Type(Type::Kind::I1) : Type(Type::Kind::I64);
                    break;
                default:
                    ilType = Type(Type::Kind::I64);
                    break;
            }
            return {val, ilType};
        }

        // Check for global mutable variables
        auto globalIt = globalVariables_.find(qualifiedSymbolName);
        if (globalIt == globalVariables_.end())
            globalIt = globalVariables_.find(symbolName);
        if (globalIt != globalVariables_.end()) {
            const std::string &globalStorageName =
                globalIt->first == qualifiedSymbolName ? qualifiedSymbolName : symbolName;
            TypeRef varType = globalIt->second;
            Type ilType = mapType(varType);
            Value addr = getGlobalVarAddr(globalStorageName, varType);
            Value loaded = emitLoad(addr, ilType);
            return {loaded, ilType};
        }

        // For function references, return a placeholder (call handling is separate)
        return {Value::constInt(0), Type(Type::Kind::Ptr)};
    }

    // Lower the base expression
    auto base = lowerExpr(expr->base.get());

    if (baseType->kind == TypeKindSem::Error) {
        Value errKind;
        Value errCode;
        Value errLine;
        bool hasSnapshot = false;
        if (auto *ident = dynamic_cast<IdentExpr *>(expr->base.get())) {
            auto bindingIt = catchErrorBindings_.find(ident->name);
            if (bindingIt != catchErrorBindings_.end()) {
                errKind = loadFromSlot(bindingIt->second.kindSlot, Type(Type::Kind::I32));
                errCode = loadFromSlot(bindingIt->second.codeSlot, Type(Type::Kind::I32));
                errLine = loadFromSlot(bindingIt->second.lineSlot, Type(Type::Kind::I32));
                hasSnapshot = true;
            }
        }
        if (!hasSnapshot) {
            errKind = emitUnary(Opcode::ErrGetKind, Type(Type::Kind::I32), base.value);
            errCode = emitUnary(Opcode::ErrGetCode, Type(Type::Kind::I32), base.value);
            errLine = emitUnary(Opcode::ErrGetLine, Type(Type::Kind::I32), base.value);
        }

        if (expr->field == "kind" || expr->field == "type") {
            Value result = emitCallRet(Type(Type::Kind::Str), kErrorKindName, {errKind});
            return {result, Type(Type::Kind::Str)};
        }
        if (expr->field == "message") {
            Value result =
                emitCallRet(Type(Type::Kind::Str), kErrorMessage, {errKind, errCode, errLine});
            return {result, Type(Type::Kind::Str)};
        }
        if (expr->field == "location") {
            Value result =
                emitCallRet(Type(Type::Kind::Str), kErrorLocation, {errKind, errCode, errLine});
            return {result, Type(Type::Kind::Str)};
        }
        if (expr->field == "code") {
            Value result = widenIntegralToI64(errCode, Type(Type::Kind::I32));
            return {result, Type(Type::Kind::I64)};
        }
        if (expr->field == "line") {
            Value result = widenIntegralToI64(errLine, Type(Type::Kind::I32));
            return {result, Type(Type::Kind::I64)};
        }
    }

    // Check if base is a struct type
    std::string typeName = baseType->name;
    const StructTypeInfo *info = getOrCreateStructTypeInfo(typeName);
    if (info) {
        const FieldLayout *field = info->findField(expr->field);

        if (field) {
            Type fieldType = mapType(field->type);
            return {emitFieldLoad(field, base.value), fieldType};
        }
    }

    // Check if base is an class type
    const ClassTypeInfo *entityInfoPtr = getOrCreateClassTypeInfo(typeName);
    if (entityInfoPtr) {
        const ClassTypeInfo &entityInfo = *entityInfoPtr;
        const FieldLayout *field = entityInfo.findField(expr->field);

        if (field) {
            Type fieldType = mapType(field->type);
            return {emitFieldLoad(field, base.value), fieldType};
        }
    }

    // Handle String.Length and String.length property (Bug #3 fix)
    if (baseType->kind == TypeKindSem::String) {
        if (expr->field == "Length" || expr->field == "length") {
            // Synthesize a call to Zanna.String.get_Length(str)
            Value result = emitCallRet(Type(Type::Kind::I64), kStringLength, {base.value});
            return {result, Type(Type::Kind::I64)};
        }
    }

    // Handle List.count, List.size, List.length, and List.Len property
    if (baseType->kind == TypeKindSem::List) {
        if (expr->field == "Count" || expr->field == "count" || expr->field == "size" ||
            expr->field == "length" || expr->field == "Len" || expr->field == "Length") {
            // Synthesize a call to Zanna.Collections.List.get_Count(list)
            Value result = emitCallRet(Type(Type::Kind::I64), kListCount, {base.value});
            return {result, Type(Type::Kind::I64)};
        }
    }

    // Handle Map.Length, Map.Len, Map.Count, etc. property
    if (baseType->kind == TypeKindSem::Map) {
        if (expr->field == "Length" || expr->field == "length" || expr->field == "Len" ||
            expr->field == "Count" || expr->field == "count" || expr->field == "size") {
            Value result = emitCallRet(Type(Type::Kind::I64),
                                       usesIntegerMapRuntime(baseType) ? kIntMapCount : kMapCount,
                                       {base.value});
            return {result, Type(Type::Kind::I64)};
        }
    }

    // Handle Set.Length, Set.Len, Set.Count, etc. property
    if (baseType->kind == TypeKindSem::Set) {
        if (expr->field == "Length" || expr->field == "length" || expr->field == "Len" ||
            expr->field == "Count" || expr->field == "count" || expr->field == "size") {
            Value result = emitCallRet(Type(Type::Kind::I64), kSetCount, {base.value});
            return {result, Type(Type::Kind::I64)};
        }
    }

    // Handle runtime class property access (e.g., app.ShouldClose, editor.LineCount)
    // Runtime classes are Ptr types with a non-empty generated runtime class name.
    if (baseType->kind == TypeKindSem::Ptr && !baseType->name.empty()) {
        // Construct getter function name: {ClassName}.get_{PropertyName}
        std::string rtGetterName = baseType->name + ".get_" + expr->field;

        // Look up the getter function
        Symbol *getterSym = sema_.findExternFunction(rtGetterName);
        if (getterSym && getterSym->type) {
            // Determine the return type - extract from function type if needed
            TypeRef symType = getterSym->type;
            if (symType->kind == TypeKindSem::Function && symType->returnType()) {
                symType = symType->returnType();
            }
            Type retType = mapType(symType);

            // Emit call to the getter function
            Value result = emitCallRet(retType, rtGetterName, {base.value});
            return {result, retType};
        }
    }

    // Unreachable for well-formed input: Sema::resolveRuntimeClassFieldAccess now
    // resolves or diagnoses every runtime-class member access, and the other
    // analyzeField branches diagnose their own unknown members. Reaching here means
    // a sema gap let an unresolved field through to lowering — fail loud with an
    // internal-compiler-error rather than silently emitting a mistyped constant
    // (the BUG-FE-006 intent: never silently mistype a field, now enforced loudly).
    diag_.report({il::support::Severity::Error,
                  "internal: unresolved field '" + expr->field +
                      "' reached lowering; please report this as a compiler bug",
                  expr->loc,
                  "V-ZIA-INTERNAL"});
    return {Value::constInt(0), Type(Type::Kind::I64)};
}

//=============================================================================
// New Expression Lowering
//=============================================================================

/// @brief Lower a `new T(...)` expression by dispatching on the constructed type.
/// @param expr New expression.
/// @return The constructed value (always a pointer) and its IL type.
/// @details Built-in `List`/`Set`/`Map` route to their runtime constructors; named runtime
///          (Ptr) classes go to lowerNewRuntimeClass(); value types to lowerNewStruct();
///          everything else to lowerNewClass().
LowerResult Lowerer::lowerNew(NewExpr *expr) {
    // Get the type from the new expression
    TypeRef type = sema_.resolveType(expr->type.get());
    if (!type) {
        return {Value::null(), Type(Type::Kind::Ptr)};
    }

    // Handle built-in collection types
    if (type->kind == TypeKindSem::List) {
        // Create a new list via runtime
        Value list = emitCallRet(Type(Type::Kind::Ptr), kListNew, {});
        return {list, Type(Type::Kind::Ptr)};
    }
    if (type->kind == TypeKindSem::Set) {
        Value set = emitCallRet(Type(Type::Kind::Ptr), kSetNew, {});
        return {set, Type(Type::Kind::Ptr)};
    }
    if (type->kind == TypeKindSem::Map) {
        Value map = emitCallRet(
            Type(Type::Kind::Ptr), usesIntegerMapRuntime(type) ? kIntMapNew : kMapNew, {});
        return {map, Type(Type::Kind::Ptr)};
    }

    // Runtime class types (Ptr): resolve ctor from the RuntimeRegistry catalog.
    // Class and Struct types have their own lowering below.
    if (!type->name.empty() && type->kind != TypeKindSem::Class &&
        type->kind != TypeKindSem::Struct) {
        return lowerNewRuntimeClass(expr, type);
    }

    if (auto structResult = lowerNewStruct(expr, type))
        return *structResult;

    return lowerNewClass(expr, type);
}

/// @brief Lower `new` of a runtime (Ptr) class by calling its catalog constructor.
/// @param expr New expression.
/// @param type Resolved runtime class type.
/// @return The constructed object pointer.
/// @details Resolves the constructor from runtime class metadata, preferring a zero-arg
///          `NewDefault` for empty argument lists when present. Arguments are lowered in
///          the semantically resolved order and coerced to the descriptor's parameter types
///          (boxing scalars into Ptr params, widening I64→F64).
LowerResult Lowerer::lowerNewRuntimeClass(NewExpr *expr, TypeRef type) {
    if (!type)
        return {Value::null(), Type(Type::Kind::Ptr)};
    {
        std::string ctorName;
        const auto *rtClass = il::runtime::findRuntimeClassByQName(type->name);
        if (!rtClass || !rtClass->ctor) {
            return poisonValue(expr->loc,
                               "V-ZIA-LOWER-MISSING-RUNTIME-CTOR",
                               "runtime type '" + type->name +
                                   "' reached lowering without constructor metadata");
        }

        if (expr->args.empty()) {
            std::string defaultCtorName = type->name + ".NewDefault";
            if (il::runtime::findRuntimeDescriptor(defaultCtorName))
                ctorName = defaultCtorName;
        }
        if (ctorName.empty()) {
            ctorName = rtClass->ctor;
        }

        const auto *binding = sema_.newArgBinding(expr);
        std::vector<int> orderedSources = orderedArgSources(expr->args, binding);
        const auto *rtDesc = il::runtime::findRuntimeDescriptor(ctorName);
        const std::vector<il::core::Type> *expectedParamTypes =
            rtDesc ? &rtDesc->signature.paramTypes : nullptr;

        // Lower arguments in the semantically resolved order.
        std::vector<Value> argValues;
        argValues.reserve(orderedSources.size());
        for (size_t i = 0; i < orderedSources.size(); ++i) {
            size_t sourceIndex = toIndex(orderedSources[i]);
            auto result = lowerExpr(expr->args[sourceIndex].value.get());
            Value argValue = result.value;

            if (result.type.kind == Type::Kind::I32)
                argValue = widenByteToInteger(argValue);

            if (expectedParamTypes && i < expectedParamTypes->size()) {
                Type expectedType = (*expectedParamTypes)[i];
                if (expectedType.kind == Type::Kind::Ptr && result.type.kind != Type::Kind::Ptr &&
                    result.type.kind != Type::Kind::Void) {
                    argValue = emitBox(argValue, result.type);
                } else if (expectedType.kind == Type::Kind::F64 &&
                           result.type.kind == Type::Kind::I64) {
                    unsigned convId = nextTempId();
                    il::core::Instr convInstr;
                    convInstr.result = convId;
                    convInstr.op = Opcode::Sitofp;
                    convInstr.type = Type(Type::Kind::F64);
                    convInstr.operands = {argValue};
                    convInstr.loc = curLoc_;
                    blockMgr_.currentBlock()->instructions.push_back(convInstr);
                    argValue = Value::temp(convId);
                }
            }

            argValues.push_back(argValue);
        }

        // Call the runtime constructor
        Value result = emitCallRet(Type(Type::Kind::Ptr), ctorName, argValues);
        return {result, Type(Type::Kind::Ptr)};
    }
}

/// @brief Lower `new` of a value type (`struct`) into stack-allocated, initialized storage.
/// @param expr New expression.
/// @param type Resolved struct type.
/// @return The initialized struct (by pointer), or nullopt if @p type is not a known struct.
/// @details Allocas the struct's storage, then either calls its explicit `init` method (self
///          first) or stores each constructor argument / field default into its field slot,
///          coercing to the declared field type.
std::optional<LowerResult> Lowerer::lowerNewStruct(NewExpr *expr, TypeRef type) {
    if (!type)
        return std::nullopt;
    // Struct types can be instantiated with 'new' just like class types.
    std::string typeName = type->name;
    const StructTypeInfo *valueInfo = getOrCreateStructTypeInfo(typeName);
    if (!valueInfo)
        return std::nullopt;
    {
        const StructTypeInfo &valInfo = *valueInfo;

        // Allocate zero-initialized stack space for the value. Zeroing is
        // load-bearing: field assignments in `init` (and the direct-store path
        // below) release the slot's previous value for managed fields. The VM
        // zero-inits allocas so that first release sees null, but native
        // codegen leaves stack garbage — releasing it traps or corrupts the
        // heap (same hazard class as the for-in string-slot init in
        // Lowerer_Stmt.cpp).
        Value ptr = emitStructTypeAlloc(valInfo);

        // Check if the struct type has an explicit init method
        MethodDecl *resolvedInit = sema_.resolvedInitDecl(expr);
        std::string initOwner = sema_.resolvedInitOwnerType(expr);
        auto initIt = valInfo.methodMap.find("init");
        if (resolvedInit || initIt != valInfo.methodMap.end()) {
            // Call the explicit init method
            MethodDecl *initDecl = resolvedInit ? resolvedInit : initIt->second;
            if (initOwner.empty())
                initOwner = typeName;
            std::string initName = sema_.loweredMethodName(initOwner, initDecl);
            if (initName.empty())
                initName = initOwner + ".init";
            std::vector<Value> initArgs;
            initArgs.push_back(ptr); // self is first argument
            TypeRef initType = sema_.getMethodType(initOwner, initDecl);
            std::vector<TypeRef> initParamTypes =
                initType ? initType->paramTypes() : std::vector<TypeRef>{};
            std::vector<Value> argValues =
                lowerResolvedNewArgs(expr, initParamTypes, initDecl ? &initDecl->params : nullptr);
            for (const auto &argVal : argValues) {
                initArgs.push_back(argVal);
            }
            emitCall(initName, initArgs);
        } else {
            auto loweredSources = lowerSourceArgs(expr->args);
            const auto *binding = sema_.newArgBinding(expr);
            std::vector<const FieldDecl *> fieldDecls = collectStructFieldDecls(sema_, typeName);

            // No init method - store arguments directly into fields
            for (size_t i = 0; i < valInfo.fields.size(); ++i) {
                const FieldLayout &field = valInfo.fields[i];
                Value fieldValue;
                int sourceIndex = binding && i < binding->fixedParamSources.size()
                                      ? binding->fixedParamSources[i]
                                      : (i < loweredSources.size() ? static_cast<int>(i) : -1);

                if (sourceIndex >= 0) {
                    size_t idx = toIndex(sourceIndex);
                    TypeRef argType = sema_.typeOf(expr->args[idx].value.get());
                    auto coerced = coerceValueToType(
                        loweredSources[idx].value, loweredSources[idx].type, argType, field.type);
                    fieldValue = coerced.value;
                } else if (i < fieldDecls.size() && fieldDecls[i]->initializer) {
                    auto initValue = lowerExpr(fieldDecls[i]->initializer.get());
                    TypeRef initType = sema_.typeOf(fieldDecls[i]->initializer.get());
                    auto coerced =
                        coerceValueToType(initValue.value, initValue.type, initType, field.type);
                    fieldValue = coerced.value;
                } else {
                    Type ilFieldType = mapType(field.type);
                    switch (ilFieldType.kind) {
                        case Type::Kind::I1:
                            fieldValue = Value::constBool(false);
                            break;
                        case Type::Kind::I64:
                        case Type::Kind::I16:
                        case Type::Kind::I32:
                            fieldValue = Value::constInt(0);
                            break;
                        case Type::Kind::F64:
                            fieldValue = Value::constFloat(0.0);
                            break;
                        case Type::Kind::Str:
                            fieldValue = emitEmptyString();
                            break;
                        case Type::Kind::Ptr:
                            fieldValue = Value::null();
                            break;
                        default:
                            fieldValue = Value::constInt(0);
                            break;
                    }
                }

                // GEP to get field address
                unsigned gepId = nextTempId();
                il::core::Instr gepInstr;
                gepInstr.result = gepId;
                gepInstr.op = Opcode::GEP;
                gepInstr.type = Type(Type::Kind::Ptr);
                gepInstr.operands = {ptr, Value::constInt(static_cast<int64_t>(field.offset))};
                gepInstr.loc = curLoc_;
                blockMgr_.currentBlock()->instructions.push_back(gepInstr);
                Value fieldAddr = Value::temp(gepId);

                // Store the value
                il::core::Instr storeInstr;
                storeInstr.op = Opcode::Store;
                storeInstr.type = mapType(field.type);
                storeInstr.operands = {fieldAddr, fieldValue};
                storeInstr.loc = curLoc_;
                blockMgr_.currentBlock()->instructions.push_back(storeInstr);
            }
        }

        return LowerResult{ptr, Type(Type::Kind::Ptr)};
    }
}

/// @brief Lower `new` of a reference type (`class`) into a heap-allocated, initialized object.
/// @param expr New expression.
/// @param type Resolved class type.
/// @return The constructed object pointer (null pointer if @p type is not a known class).
/// @details Allocates via `rt_obj_new_i64` (seeding the heap header with the class id and
///          size so the object is GC/refcount-ready), then either calls the explicit `init`
///          method or performs inline field initialization from constructor args / field
///          defaults, boxing struct-typed fields and routing weak fields through
///          emitFieldStore().
LowerResult Lowerer::lowerNewClass(NewExpr *expr, TypeRef type) {
    if (!type)
        return {Value::null(), Type(Type::Kind::Ptr)};
    std::string typeName = type->name;
    const ClassTypeInfo *infoPtr = getOrCreateClassTypeInfo(typeName);
    if (!infoPtr) {
        // Not a class type.
        return {Value::null(), Type(Type::Kind::Ptr)};
    }

    const ClassTypeInfo &entityInfo = *infoPtr;

    // Allocate heap memory for the class using rt_obj_new_i64
    // This properly initializes the heap header with magic, refcount, etc.
    // so that entities can be added to lists and other reference-counted collections
    Value ptr = emitCallRet(Type(Type::Kind::Ptr),
                            "rt_obj_new_i64",
                            {Value::constInt(static_cast<int64_t>(entityInfo.classId)),
                             Value::constInt(static_cast<int64_t>(entityInfo.totalSize))});

    // Check if the class has an explicit init method
    MethodDecl *resolvedInit = sema_.resolvedInitDecl(expr);
    std::string initOwner = sema_.resolvedInitOwnerType(expr);
    auto initIt = entityInfo.methodMap.find("init");
    if (resolvedInit || initIt != entityInfo.methodMap.end()) {
        MethodDecl *initDecl = resolvedInit ? resolvedInit : initIt->second;
        if (initOwner.empty())
            initOwner = typeName;
        std::string initName = sema_.loweredMethodName(initOwner, initDecl);
        if (initName.empty())
            initName = initOwner + ".init";
        std::vector<Value> initArgs;
        initArgs.push_back(ptr); // self is first argument
        TypeRef initType = sema_.getMethodType(initOwner, initDecl);
        std::vector<TypeRef> initParamTypes =
            initType ? initType->paramTypes() : std::vector<TypeRef>{};
        std::vector<Value> argValues =
            lowerResolvedNewArgs(expr, initParamTypes, initDecl ? &initDecl->params : nullptr);
        for (const auto &argVal : argValues) {
            initArgs.push_back(argVal);
        }
        emitCall(initName, initArgs);
    } else {
        auto loweredSources = lowerSourceArgs(expr->args);
        const auto *binding = sema_.newArgBinding(expr);
        std::vector<const FieldDecl *> fieldDecls;
        appendClassFieldDecls(sema_, typeName, fieldDecls);

        // No explicit init - do inline field initialization
        // Constructor args map directly to fields in declaration order
        for (size_t i = 0; i < entityInfo.fields.size(); ++i) {
            const auto &field = entityInfo.fields[i];
            Type ilFieldType = mapType(field.type);
            Value fieldValue;

            int sourceIndex = binding && i < binding->fixedParamSources.size()
                                  ? binding->fixedParamSources[i]
                                  : (i < loweredSources.size() ? static_cast<int>(i) : -1);
            if (sourceIndex >= 0) {
                size_t idx = toIndex(sourceIndex);
                TypeRef argType = sema_.typeOf(expr->args[idx].value.get());
                auto coerced = coerceValueToType(
                    loweredSources[idx].value, loweredSources[idx].type, argType, field.type);
                fieldValue = coerced.value;
                if (field.type && field.type->kind == TypeKindSem::Struct)
                    fieldValue = emitBoxValue(fieldValue, coerced.type, field.type);
            } else if (i < fieldDecls.size() && fieldDecls[i]->initializer) {
                auto initValue = lowerExpr(fieldDecls[i]->initializer.get());
                TypeRef initType = sema_.typeOf(fieldDecls[i]->initializer.get());
                auto coerced =
                    coerceValueToType(initValue.value, initValue.type, initType, field.type);
                fieldValue = coerced.value;
                if (field.type && field.type->kind == TypeKindSem::Struct)
                    fieldValue = emitBoxValue(fieldValue, coerced.type, field.type);
            } else {
                // Use default value
                switch (ilFieldType.kind) {
                    case Type::Kind::I1:
                        fieldValue = Value::constBool(false);
                        break;
                    case Type::Kind::I64:
                    case Type::Kind::I16:
                    case Type::Kind::I32:
                        fieldValue = Value::constInt(0);
                        break;
                    case Type::Kind::F64:
                        fieldValue = Value::constFloat(0.0);
                        break;
                    case Type::Kind::Str:
                        fieldValue = emitEmptyString();
                        break;
                    case Type::Kind::Ptr:
                        fieldValue = Value::null();
                        break;
                    default:
                        fieldValue = Value::constInt(0);
                        break;
                }
            }

            emitFieldStore(&field, ptr, fieldValue);
        }
    }

    // Return pointer to the allocated class
    return {ptr, Type(Type::Kind::Ptr)};
}

} // namespace il::frontends::zia
