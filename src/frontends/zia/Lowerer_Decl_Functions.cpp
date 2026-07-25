//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Decl_Functions.cpp
/// @brief Lowers Zia globals, functions, methods, properties, destructors,
///        asynchronous functions, and generic function instantiations.
///
/// @details Each concrete callable is emitted through the active IL builder
///          with slot-backed parameters for cross-block access. The routines
///          maintain the current function and owning-type contexts, synthesize
///          type-correct fall-through returns, and arrange managed-value
///          ownership cleanup. Module globals are either folded into constants
///          or initialized through runtime-backed storage at program startup.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"
#include "frontends/zia/ZiaLocationScope.hpp"

#include "il/core/Linkage.hpp"

namespace il::frontends::zia {

using namespace runtime;

//=============================================================================
// Global Variable Declaration Lowering
//=============================================================================

/// @brief Select the runtime helper that returns a module-level variable's address.
/// @param kind IL type kind of the global being accessed.
/// @return Name of the matching `rt_modvar_addr_*` helper (the pointer helper covers
///         aggregates and any unmapped kind).
std::string Lowerer::getModvarAddrHelper(Type::Kind kind) {
    switch (kind) {
        case Type::Kind::I64:
            return "rt_modvar_addr_i64";
        case Type::Kind::F64:
            return "rt_modvar_addr_f64";
        case Type::Kind::I1:
            return "rt_modvar_addr_i1";
        case Type::Kind::Str:
            return "rt_modvar_addr_str";
        case Type::Kind::Ptr:
        default:
            return "rt_modvar_addr_ptr";
    }
}

/// @brief Emit the runtime call that yields the storage address of a module-level global.
/// @param name Source-level (already namespace-qualified) global variable name.
/// @param type Semantic type of the global, used to pick the address helper.
/// @return An IL pointer value holding the global's runtime storage address.
/// @details Interns the global's string name, then calls the type-appropriate
///          `rt_modvar_addr_*` helper, registering that helper as a used extern.
Lowerer::Value Lowerer::getGlobalVarAddr(const std::string &name, TypeRef type) {
    std::string globalName = getStringGlobal(name);
    Value nameStr = emitConstStr(globalName);

    Type ilType = mapType(type);
    std::string helper = getModvarAddrHelper(ilType.kind);
    usedExterns_.insert(helper);

    Value addr = emitCallRet(Type(Type::Kind::Ptr), helper, {nameStr});
    // The address helper only borrows its lookup key.  Release the owned
    // const_str handle in this block, before control-flow lowering can move
    // statement cleanup to a short-circuit merge block where the SSA value is
    // no longer available.
    consumeDeferred(nameStr);
    emitManagedRelease(nameStr, /*isString=*/true);
    return addr;
}

/// @brief Lower a module-level variable declaration.
/// @param decl The global variable declaration AST node.
/// @details `final` declarations whose initializer is a literal or constant-foldable
///          expression are recorded in @c globalConstants_ and emit no storage. Mutable
///          globals and runtime-initialized finals are registered in @c globalVariables_,
///          with any initializer queued in @c globalInitializers_ for module startup.
void Lowerer::lowerGlobalVarDecl(GlobalVarDecl &decl) {
    ZiaLocationScope locScope(*this, decl.loc);

    // Use qualified name for globals inside namespaces
    std::string qualifiedName = declarationName(decl, decl.name);

    // Resolve the type
    TypeRef type = decl.type ? sema_.resolveType(decl.type.get()) : nullptr;
    if (!type && decl.initializer) {
        type = sema_.typeOf(decl.initializer.get());
    }

    // Try to inline literal initializers as constants (only for final declarations)
    // Mutable variables must use runtime storage even with literal initializers
    if (decl.isFinal && decl.initializer) {
        Expr *init = decl.initializer.get();
        bool inlinedAsConstant = false;

        // Handle integer literals
        if (auto *intLit = dynamic_cast<IntLiteralExpr *>(init)) {
            globalConstants_[qualifiedName] = Value::constInt(intLit->value);
            inlinedAsConstant = true;
        }
        // Handle number (float) literals
        else if (auto *numLit = dynamic_cast<NumberLiteralExpr *>(init)) {
            globalConstants_[qualifiedName] = Value::constFloat(numLit->value);
            inlinedAsConstant = true;
        }
        // Handle boolean literals
        else if (auto *boolLit = dynamic_cast<BoolLiteralExpr *>(init)) {
            globalConstants_[qualifiedName] = Value::constBool(boolLit->value);
            inlinedAsConstant = true;
        }
        // Handle string literals
        else if (auto *strLit = dynamic_cast<StringLiteralExpr *>(init)) {
            std::string label = stringTable_.intern(strLit->value);
            globalConstants_[qualifiedName] = Value::constStr(label);
            inlinedAsConstant = true;
        }
        // Fold constant-expression initializers (e.g. `0 - 2147483647`, `-1`,
        // `2 * 1024`) that are not direct literals (BUG-FE-011).
        else if (auto folded = tryFoldNumericConstant(init)) {
            globalConstants_[qualifiedName] = *folded;
            inlinedAsConstant = true;
        }

        if (inlinedAsConstant) {
            return;
        }

        // Non-constant finals are immutable at the language level, but need
        // runtime storage so aggregate/object initializers are evaluated once
        // during module startup.
    }

    // Register mutable globals and runtime-initialized finals for storage.
    if (type) {
        globalVariables_[qualifiedName] = type;
        if (decl.initializer) {
            GlobalInitializer init;
            init.name = qualifiedName;
            init.type = type;
            init.initializer = decl.initializer.get();
            globalInitializers_.push_back(std::move(init));
        }
    }
}

/// @brief Emit stores for every queued global initializer at module startup.
/// @details Invoked from the program entry point. Each initializer expression is lowered,
///          struct values are boxed or deep-copied to pointer form, the result is coerced to
///          the global's declared type, and the value is stored at the address returned by
///          getGlobalVarAddr(). Deferred temporaries produced during lowering are consumed.
void Lowerer::emitGlobalInitializers() {
    for (const auto &entry : globalInitializers_) {
        if (!entry.initializer || !entry.type)
            continue;

        ZiaLocationScope locScope(*this, entry.initializer->loc);

        auto lowered = lowerExpr(entry.initializer);
        TypeRef sourceType = sema_.typeOf(entry.initializer);

        if (entry.type && entry.type->kind == TypeKindSem::Struct) {
            lowered.value = emitBoxValue(lowered.value, lowered.type, entry.type);
            lowered.type = Type(Type::Kind::Ptr);
        } else if (sourceType && sourceType->kind == TypeKindSem::Struct) {
            if (const StructTypeInfo *info = getOrCreateStructTypeInfo(sourceType->name)) {
                lowered.value = emitStructTypeCopy(*info, lowered.value);
                lowered.type = Type(Type::Kind::Ptr);
            }
        }

        auto coerced = coerceValueToType(lowered.value, lowered.type, sourceType, entry.type);
        Value addr = getGlobalVarAddr(entry.name, entry.type);
        emitStore(addr, coerced.value, mapType(entry.type));
        consumeDeferred(coerced.value);
    }
}

//=============================================================================
// Function Declaration Lowering
//=============================================================================

/// @brief Lower a top-level function declaration to an IL function.
/// @param decl The function declaration AST node.
/// @details Generic templates are skipped (they are lowered on demand at instantiation) and
///          `async` functions are delegated to lowerAsyncFunctionDecl(). Foreign functions
///          become import declarations with no body. Parameters are stored in slots for
///          cross-block SSA correctness; the entry point (`start`/`main`) seeds itable and
///          global initialization. A type-correct implicit return is appended when the body
///          falls through.
void Lowerer::lowerFunctionDecl(FunctionDecl &decl) {
    ZiaLocationScope locScope(*this, decl.loc);

    // Skip generic functions - they will be instantiated when called
    if (!decl.genericParams.empty())
        return;

    // Determine return type from sema's resolved declaration signature.
    TypeRef funcType = sema_.getFunctionType(&decl);
    if (!funcType) {
        std::vector<TypeRef> fallbackParamTypes;
        fallbackParamTypes.reserve(decl.params.size());
        for (const auto &param : decl.params)
            fallbackParamTypes.push_back(param.type ? sema_.resolveType(param.type.get())
                                                    : types::unknown());
        TypeRef fallbackReturnType =
            decl.returnType ? sema_.resolveType(decl.returnType.get()) : types::voidType();
        funcType = types::function(fallbackParamTypes, fallbackReturnType);
    }
    TypeRef returnType = funcType && funcType->kind == TypeKindSem::Function
                             ? funcType->returnType()
                             : types::voidType();
    Type ilReturnType = mapType(returnType);

    // Build parameter list
    std::vector<il::core::Param> params;
    params.reserve(decl.params.size());
    const auto cachedParamTypes = funcType && funcType->kind == TypeKindSem::Function
                                      ? funcType->paramTypes()
                                      : std::vector<TypeRef>{};
    for (size_t i = 0; i < decl.params.size(); ++i) {
        TypeRef paramType =
            i < cachedParamTypes.size()
                ? cachedParamTypes[i]
                : (decl.params[i].type ? sema_.resolveType(decl.params[i].type.get())
                                       : types::unknown());
        params.push_back({decl.params[i].name, mapType(paramType)});
    }

    // Use qualified name for functions inside namespaces
    std::string qualifiedName = declarationName(decl, decl.name);
    std::string mangledName = sema_.loweredFunctionName(&decl);
    if (mangledName.empty())
        mangledName = mangleFunctionName(qualifiedName);

    TypeRef declaredReturnType =
        decl.returnType ? sema_.resolveType(decl.returnType.get()) : types::voidType();

    if (decl.isAsync) {
        lowerAsyncFunctionDecl(decl,
                               mangledName,
                               params,
                               returnType,
                               ilReturnType,
                               declaredReturnType,
                               cachedParamTypes);
        return;
    }

    if (!decl.isForeign && !decl.body)
        return;

    // Track this function as defined in this module
    definedFunctions_.insert(mangledName);

    // Create function
    currentFunc_ = &builder_->startFunction(mangledName, ilReturnType, params);
    currentReturnType_ = returnType;

    // Set IL linkage from AST visibility/foreign flag.
    if (decl.isForeign)
        currentFunc_->linkage = il::core::Linkage::Import;
    else if (decl.visibility == Visibility::Public)
        currentFunc_->linkage = il::core::Linkage::Export;

    // Foreign (import) functions have no body -- just a declaration.
    if (decl.isForeign) {
        currentFunc_ = nullptr;
        currentReturnType_ = nullptr;
        return;
    }

    blockMgr_.bind(builder_.get(), currentFunc_);
    locals_.clear();
    slots_.clear();
    localTypes_.clear();
    deferredTemps_.clear();

    // Create entry block with the function's params as block params
    // (required for proper VM argument passing)
    builder_->createBlock(*currentFunc_, "entry_0", currentFunc_->params);
    size_t entryIdx = currentFunc_->blocks.size() - 1;
    setBlock(entryIdx);

    // Define parameters using slot-based storage for cross-block SSA correctness
    // This ensures parameters are accessible in all basic blocks (if, while, guard, etc.)
    const auto &blockParams = currentFunc_->blocks[entryIdx].params;
    for (size_t i = 0; i < decl.params.size() && i < blockParams.size(); ++i) {
        TypeRef paramType =
            i < cachedParamTypes.size()
                ? cachedParamTypes[i]
                : (decl.params[i].type ? sema_.resolveType(decl.params[i].type.get())
                                       : types::unknown());
        Type ilParamType = mapType(paramType);

        // Create slot and store the parameter value
        createSlot(decl.params[i].name, ilParamType);
        storeToSlot(decl.params[i].name, Value::temp(blockParams[i].id), ilParamType);
        if (ilParamType.kind == Type::Kind::Str) // params are borrowed; the slot owns +1
            emitCall(runtime::kStrRetainMaybe, {Value::temp(blockParams[i].id)});
        else if (ilParamType.kind == Type::Kind::Ptr && needsRelease(paramType))
            emitCall("rt_obj_retain_maybe", {Value::temp(blockParams[i].id)});
        localTypes_[decl.params[i].name] = paramType;
    }

    const bool isEntryPoint = decl.name == "start" || decl.name == "main";

    // Emit interface itable init call at start of the entry point (before any user code).
    // The __zia_iface_init function is emitted later by emitItableInit(); if no
    // interfaces have implementors, it emits a trivial ret-void stub.
    if (isEntryPoint && !interfaceTypes_.empty()) {
        emitCall("__zia_iface_init", {});
    }

    // Emit global variable initializations at the entry point so both
    // `start()` and `main()` programs observe the same module startup state.
    if (isEntryPoint && !globalInitializers_.empty())
        emitGlobalInitializers();

    // Lower function body
    if (decl.body) {
        lowerStmt(decl.body.get());
    }

    // Add implicit return if needed (use the correct default value for each type).
    if (!isTerminated()) {
        releaseLocalStringSlots(); // param slots own +1; body block released its own
        releaseLocalObjectSlots();
        if (ilReturnType.kind == Type::Kind::Void) {
            emitRetVoid();
        } else {
            Value defaultValue;
            switch (ilReturnType.kind) {
                case Type::Kind::I1:
                    defaultValue = Value::constBool(false);
                    break;
                case Type::Kind::I64:
                case Type::Kind::I16:
                case Type::Kind::I32:
                    defaultValue = Value::constInt(0);
                    break;
                case Type::Kind::F64:
                    defaultValue = Value::constFloat(0.0);
                    break;
                case Type::Kind::Str:
                    defaultValue = Value::constStr("");
                    break;
                case Type::Kind::Ptr:
                    defaultValue = Value::null();
                    break;
                default:
                    defaultValue = Value::constInt(0);
                    break;
            }
            emitRet(defaultValue);
        }
    }

    currentFunc_ = nullptr;
    currentReturnType_ = nullptr;
}

/// @brief Lower an `async` function into a worker trampoline plus a public wrapper.
/// @param decl Async function declaration.
/// @param mangledName Lowered name of the public entry point.
/// @param params IL parameter list of the public entry.
/// @param returnType Semantic return type of the public entry.
/// @param ilReturnType IL return type of the public entry.
/// @param declaredReturnType Declared payload type that the worker resolves into a future.
/// @param cachedParamTypes Pre-resolved parameter types (generic substitutions applied).
/// @details Emits `<name>__async_worker(ptr env)`, which unpacks and takes ownership of the
///          captured arguments, lowers the body, and returns the boxed payload (or null for
///          void). It then emits the public wrapper that boxes arguments into an env block
///          and starts the task through the async runtime, returning the future handle.
void Lowerer::lowerAsyncFunctionDecl(FunctionDecl &decl,
                                     const std::string &mangledName,
                                     const std::vector<il::core::Param> &params,
                                     TypeRef returnType,
                                     il::core::Type ilReturnType,
                                     TypeRef declaredReturnType,
                                     const std::vector<TypeRef> &cachedParamTypes) {
    {
        auto resetLoweringState = [&]() {
            blockMgr_.reset(nullptr);
            locals_.clear();
            slots_.clear();
            localTypes_.clear();
            deferredTemps_.clear();
            asyncOwnedValues_.clear();
            currentAsyncWorker_ = false;
            currentFunc_ = nullptr;
            currentReturnType_ = nullptr;
        };

        auto emitAsyncImplicitReturn = [&](TypeRef payloadType) {
            if (isTerminated())
                return;

            if (!payloadType || payloadType->kind == TypeKindSem::Void) {
                for (const auto &owned : asyncOwnedValues_)
                    emitManagedRelease(owned, /*isString=*/false);
                asyncOwnedValues_.clear();
                releaseDeferredTemps();
                emitRet(Value::null());
                return;
            }

            Type payloadIlType = mapType(payloadType);
            Value defaultValue;
            switch (payloadIlType.kind) {
                case Type::Kind::I1:
                    defaultValue = Value::constBool(false);
                    break;
                case Type::Kind::I64:
                case Type::Kind::I16:
                case Type::Kind::I32:
                    defaultValue = Value::constInt(0);
                    break;
                case Type::Kind::F64:
                    defaultValue = Value::constFloat(0.0);
                    break;
                case Type::Kind::Str:
                    defaultValue = emitEmptyString();
                    break;
                case Type::Kind::Ptr:
                    defaultValue = Value::null();
                    break;
                default:
                    defaultValue = Value::constInt(0);
                    break;
            }

            Value futureValue = defaultValue;
            if (payloadType->kind == TypeKindSem::Struct || payloadIlType.kind != Type::Kind::Ptr)
                futureValue = emitBoxValue(defaultValue, payloadIlType, payloadType);
            else
                emitCall("rt_obj_retain_maybe", {futureValue});

            for (const auto &owned : asyncOwnedValues_)
                emitManagedRelease(owned, /*isString=*/false);
            asyncOwnedValues_.clear();
            releaseDeferredTemps();
            emitRet(futureValue);
        };

        const std::string workerName = mangledName + "__async_worker";
        definedFunctions_.insert(workerName);

        // Emit the async worker trampoline: ptr(ptr env)
        currentFunc_ = &builder_->startFunction(
            workerName, Type(Type::Kind::Ptr), {{"__env", Type(Type::Kind::Ptr)}});
        currentReturnType_ = declaredReturnType;
        currentAsyncWorker_ = true;
        blockMgr_.bind(builder_.get(), currentFunc_);
        locals_.clear();
        slots_.clear();
        localTypes_.clear();
        deferredTemps_.clear();
        asyncOwnedValues_.clear();

        builder_->createBlock(*currentFunc_, "entry_0", currentFunc_->params);
        size_t workerEntryIdx = currentFunc_->blocks.size() - 1;
        setBlock(workerEntryIdx);

        const auto &workerParams = currentFunc_->blocks[workerEntryIdx].params;
        Value envPtr = Value::temp(workerParams[0].id);

        for (size_t i = 0; i < decl.params.size(); ++i) {
            TypeRef paramType =
                i < cachedParamTypes.size()
                    ? cachedParamTypes[i]
                    : (decl.params[i].type ? sema_.resolveType(decl.params[i].type.get())
                                           : types::unknown());
            Type ilParamType = mapType(paramType);

            Value argAddr = emitGEP(envPtr, static_cast<int64_t>(i * sizeof(void *)));
            Value ownedArg = emitLoad(argAddr, Type(Type::Kind::Ptr));
            asyncOwnedValues_.push_back(ownedArg);

            Value unpacked = ownedArg;
            if (paramType &&
                (paramType->kind == TypeKindSem::Struct || ilParamType.kind != Type::Kind::Ptr))
                unpacked = emitUnboxValue(ownedArg, ilParamType, paramType).value;

            createSlot(decl.params[i].name, ilParamType);
            storeToSlot(decl.params[i].name, unpacked, ilParamType);
            if (ilParamType.kind == Type::Kind::Str) // env values are borrowed; the slot owns +1
                emitCall(runtime::kStrRetainMaybe, {unpacked});
            localTypes_[decl.params[i].name] = paramType;
        }

        emitManagedRelease(envPtr, /*isString=*/false);

        if (decl.body)
            lowerStmt(decl.body.get());
        emitAsyncImplicitReturn(declaredReturnType);
        resetLoweringState();

        // Emit the public wrapper that packages arguments and starts the async task.
        definedFunctions_.insert(mangledName);
        currentFunc_ = &builder_->startFunction(mangledName, ilReturnType, params);
        currentReturnType_ = returnType;
        if (decl.visibility == Visibility::Public)
            currentFunc_->linkage = il::core::Linkage::Export;

        blockMgr_.bind(builder_.get(), currentFunc_);
        locals_.clear();
        slots_.clear();
        localTypes_.clear();
        deferredTemps_.clear();

        builder_->createBlock(*currentFunc_, "entry_0", currentFunc_->params);
        size_t wrapperEntryIdx = currentFunc_->blocks.size() - 1;
        setBlock(wrapperEntryIdx);
        const auto &wrapperParams = currentFunc_->blocks[wrapperEntryIdx].params;

        Value envSize = Value::constInt(static_cast<int64_t>(decl.params.size() * sizeof(void *)));
        Value wrapperEnv =
            emitCallRet(Type(Type::Kind::Ptr), "rt_obj_new_i64", {Value::constInt(0), envSize});

        for (size_t i = 0; i < decl.params.size() && i < wrapperParams.size(); ++i) {
            TypeRef paramType =
                i < cachedParamTypes.size()
                    ? cachedParamTypes[i]
                    : (decl.params[i].type ? sema_.resolveType(decl.params[i].type.get())
                                           : types::unknown());
            Type ilParamType = mapType(paramType);
            Value paramValue = Value::temp(wrapperParams[i].id);

            Value storedValue = paramValue;
            if (paramType &&
                (paramType->kind == TypeKindSem::Struct || ilParamType.kind != Type::Kind::Ptr)) {
                storedValue = emitBoxValue(paramValue, ilParamType, paramType);
            } else {
                emitCall("rt_obj_retain_maybe", {storedValue});
            }

            Value argAddr = emitGEP(wrapperEnv, static_cast<int64_t>(i * sizeof(void *)));
            emitStore(argAddr, storedValue, Type(Type::Kind::Ptr));
        }

        Value future =
            emitCallRet(Type(Type::Kind::Ptr), kAsyncRun, {Value::global(workerName), wrapperEnv});
        emitRet(future);
        resetLoweringState();
    }
}

/// @brief Lower one concrete instantiation of a generic function.
/// @param mangledName Substitution-keyed lowered name of this instantiation.
/// @param decl The generic function template declaration.
/// @details Pushes the substitution context so type parameters resolve to concrete types,
///          re-analyzes the declaration under those substitutions, lowers the body like an
///          ordinary function (slot-based params, typed implicit return), then pops the
///          substitution context.
void Lowerer::lowerGenericFunctionInstantiation(const std::string &mangledName,
                                                FunctionDecl *decl) {
    // Push substitution context so type parameters resolve correctly
    bool pushedContext = sema_.pushSubstitutionContext(mangledName);
    if (pushedContext)
        sema_.analyzeFunctionDecl(*decl);

    // Get the instantiated function type from Sema
    // The types should resolve with substitutions active
    TypeRef returnType =
        decl->returnType ? sema_.resolveType(decl->returnType.get()) : types::voidType();
    Type ilReturnType = mapType(returnType);

    // Build parameter list
    std::vector<il::core::Param> params;
    params.reserve(decl->params.size());
    for (const auto &param : decl->params) {
        TypeRef paramType = param.type ? sema_.resolveType(param.type.get()) : types::unknown();
        params.push_back({param.name, mapType(paramType)});
    }

    // Track this function as defined in this module
    definedFunctions_.insert(mangledName);

    // Create function
    currentFunc_ = &builder_->startFunction(mangledName, ilReturnType, params);
    currentReturnType_ = returnType;
    blockMgr_.bind(builder_.get(), currentFunc_);
    locals_.clear();
    slots_.clear();
    localTypes_.clear();
    deferredTemps_.clear();

    // Create entry block with the function's params as block params
    builder_->createBlock(*currentFunc_, "entry_0", currentFunc_->params);
    size_t entryIdx = currentFunc_->blocks.size() - 1;
    setBlock(entryIdx);

    // Define parameters using slot-based storage
    const auto &blockParams = currentFunc_->blocks[entryIdx].params;
    for (size_t i = 0; i < decl->params.size() && i < blockParams.size(); ++i) {
        TypeRef paramType =
            decl->params[i].type ? sema_.resolveType(decl->params[i].type.get()) : types::unknown();
        Type ilParamType = mapType(paramType);

        // Create slot and store the parameter value
        createSlot(decl->params[i].name, ilParamType);
        storeToSlot(decl->params[i].name, Value::temp(blockParams[i].id), ilParamType);
        if (ilParamType.kind == Type::Kind::Str) // params are borrowed; the slot owns +1
            emitCall(runtime::kStrRetainMaybe, {Value::temp(blockParams[i].id)});
        else if (ilParamType.kind == Type::Kind::Ptr && needsRelease(paramType))
            emitCall("rt_obj_retain_maybe", {Value::temp(blockParams[i].id)});
        localTypes_[decl->params[i].name] = paramType;
    }

    // Lower function body
    if (decl->body) {
        lowerStmt(decl->body.get());
    }

    // Add implicit return if needed
    if (!isTerminated()) {
        releaseLocalStringSlots(); // param slots own +1; body block released its own
        releaseLocalObjectSlots();
        if (ilReturnType.kind == Type::Kind::Void) {
            emitRetVoid();
        } else {
            Value defaultValue;
            switch (ilReturnType.kind) {
                case Type::Kind::I1:
                    defaultValue = Value::constBool(false);
                    break;
                case Type::Kind::I64:
                case Type::Kind::I16:
                case Type::Kind::I32:
                    defaultValue = Value::constInt(0);
                    break;
                case Type::Kind::F64:
                    defaultValue = Value::constFloat(0.0);
                    break;
                case Type::Kind::Str:
                    defaultValue = emitEmptyString();
                    break;
                case Type::Kind::Ptr:
                    defaultValue = Value::null();
                    break;
                default:
                    defaultValue = Value::constInt(0);
                    break;
            }
            emitRet(defaultValue);
        }
    }

    // Pop substitution context
    if (pushedContext) {
        sema_.popTypeParams();
    }

    currentFunc_ = nullptr;
    currentReturnType_ = nullptr;
}

//=============================================================================
// Value and Entity Declaration Lowering
//=============================================================================

/// @brief Lower a value-type (`struct`) declaration: register its layout and lower methods.
/// @param decl Struct declaration AST node.
/// @details Uninstantiated generics are skipped (lowered at instantiation). The layout may
///          already be registered by the pre-pass (BUG-FE-006); if not, it is registered
///          here before each method is lowered against the qualified type name.
void Lowerer::lowerStructDecl(StructDecl &decl) {
    ZiaLocationScope locScope(*this, decl.loc);

    // Skip uninstantiated generic types - they're lowered during instantiation
    if (!decl.genericParams.empty())
        return;

    std::string qualifiedName = declarationName(decl, decl.name);

    // BUG-FE-006 fix: Layout may already be registered by the pre-pass.
    if (structTypes_.find(qualifiedName) == structTypes_.end()) {
        registerStructLayout(decl);
    }

    const StructTypeInfo &storedInfo = structTypes_[qualifiedName];

    // Lower all methods using qualified type name
    for (auto *method : storedInfo.methods) {
        lowerMethodDecl(*method, qualifiedName, false);
    }
}

/// @brief Lower a reference-type (`class`) declaration.
/// @param decl Class declaration AST node.
/// @details Uninstantiated generics are skipped. Registers the layout if the pre-pass did
///          not, promotes static fields to module-level globals (queuing their initializers),
///          lowers all methods before the vtable can reference them, synthesizes property
///          getters/setters and the optional destructor, and finally emits the vtable global.
void Lowerer::lowerClassDecl(ClassDecl &decl) {
    ZiaLocationScope locScope(*this, decl.loc);

    // Skip uninstantiated generic types - they're lowered during instantiation
    if (!decl.genericParams.empty())
        return;

    std::string qualifiedName = declarationName(decl, decl.name);

    // BUG-FE-006 fix: Layout may already be registered by the pre-pass.
    // If not registered yet (e.g., in pending generic instantiation), do it now.
    auto it = classTypes_.find(qualifiedName);
    if (it == classTypes_.end()) {
        registerClassLayout(decl);
    }

    ClassTypeInfo &storedInfo = classTypes_[qualifiedName];

    // Register module-level globals for static fields
    for (auto &member : decl.members) {
        if (member->kind == DeclKind::Field) {
            auto *field = static_cast<FieldDecl *>(member.get());
            if (field->isStatic) {
                TypeRef fieldType =
                    field->type ? sema_.resolveType(field->type.get()) : types::unknown();
                std::string globalName = qualifiedName + "." + field->name;
                globalVariables_[globalName] = fieldType;

                if (field->initializer) {
                    GlobalInitializer init;
                    init.name = globalName;
                    init.type = fieldType;
                    init.initializer = field->initializer.get();
                    globalInitializers_.push_back(std::move(init));
                }
            }
        }
    }

    // Lower all methods (so they are defined before vtable references them)
    for (auto *method : storedInfo.methods) {
        lowerMethodDecl(*method, qualifiedName, true);
    }

    // Lower property declarations as synthesized get_/set_ methods
    for (auto &member : decl.members) {
        if (member->kind == DeclKind::Property) {
            auto *prop = static_cast<PropertyDecl *>(member.get());
            lowerPropertyDecl(*prop, qualifiedName, true);
        }
    }

    // Lower destructor declaration (at most one per class)
    for (auto &member : decl.members) {
        if (member->kind == DeclKind::Destructor) {
            auto *dtor = static_cast<DestructorDecl *>(member.get());
            lowerDestructorDecl(*dtor, qualifiedName);
            break; // at most one destructor
        }
    }

    // Emit vtable global (array of function pointers)
    if (!storedInfo.vtable.empty()) {
        emitVtable(storedInfo);
    }
}

/// @brief Lower an interface declaration.
/// @param decl Interface declaration AST node.
/// @details Registers the interface layout. Methods with bodies become default
///          implementations (lowered via lowerInterfaceDefaultMethodDecl); abstract
///          signatures are satisfied by implementing types through the itable.
void Lowerer::lowerInterfaceDecl(InterfaceDecl &decl) {
    ZiaLocationScope locScope(*this, decl.loc);

    // Use qualified name for interfaces inside namespaces
    std::string qualifiedName = declarationName(decl, decl.name);
    registerInterfaceLayout(decl);

    // Methods with bodies are default implementations. Abstract signatures are
    // satisfied by implementing classes/structs through the itable.
    for (auto &member : decl.members) {
        if (member->kind != DeclKind::Method)
            continue;
        auto *method = static_cast<MethodDecl *>(member.get());
        if (method->body)
            lowerInterfaceDefaultMethodDecl(*method, qualifiedName);
    }
}

/// @brief Lower an interface default-method body to a standalone IL function.
/// @param decl Interface method declaration carrying a default body.
/// @param interfaceName Qualified interface name used to mangle the lowered function name.
/// @details The lowered function takes `self` (ptr) followed by the declared parameters,
///          stored in slots, and is dispatched through the itable when an implementor does
///          not override the method. A typed implicit return is appended on fall-through.
void Lowerer::lowerInterfaceDefaultMethodDecl(MethodDecl &decl, const std::string &interfaceName) {
    ZiaLocationScope locScope(*this, decl.loc);

    TypeRef methodType = sema_.getMethodType(interfaceName, &decl);
    if (!methodType)
        methodType = sema_.getMethodType(interfaceName, decl.name);

    std::vector<TypeRef> cachedParamTypes;
    TypeRef returnType = types::voidType();
    if (methodType && methodType->kind == TypeKindSem::Function) {
        cachedParamTypes = methodType->paramTypes();
        returnType = methodType->returnType();
    } else {
        returnType = decl.returnType ? sema_.resolveType(decl.returnType.get()) : types::voidType();
        for (const auto &param : decl.params)
            cachedParamTypes.push_back(param.type ? sema_.resolveType(param.type.get())
                                                  : types::unknown());
    }

    Type ilReturnType = mapType(returnType);

    std::vector<il::core::Param> params;
    params.reserve(decl.params.size() + 1);
    params.push_back({"self", Type(Type::Kind::Ptr)});
    for (size_t i = 0; i < decl.params.size(); ++i) {
        TypeRef paramType = i < cachedParamTypes.size() ? cachedParamTypes[i] : types::unknown();
        params.push_back({decl.params[i].name, mapType(paramType)});
    }

    std::string loweredName = sema_.loweredMethodName(interfaceName, &decl);
    if (loweredName.empty())
        loweredName = interfaceName + "." + decl.name;

    definedFunctions_.insert(loweredName);
    currentFunc_ = &builder_->startFunction(loweredName, ilReturnType, params);
    currentReturnType_ = returnType;
    blockMgr_.bind(builder_.get(), currentFunc_);
    locals_.clear();
    slots_.clear();
    localTypes_.clear();
    deferredTemps_.clear();
    currentClassType_ = nullptr;
    currentStructType_ = nullptr;

    builder_->createBlock(*currentFunc_, "entry_0", currentFunc_->params);
    size_t entryIdx = currentFunc_->blocks.size() - 1;
    setBlock(entryIdx);

    const auto &blockParams = currentFunc_->blocks[entryIdx].params;
    if (!blockParams.empty()) {
        createSlot("self", Type(Type::Kind::Ptr));
        storeToSlot("self", Value::temp(blockParams[0].id), Type(Type::Kind::Ptr));
        localTypes_["self"] = types::interface(interfaceName);
    }

    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (i + 1 >= blockParams.size())
            continue;
        TypeRef paramType = i < cachedParamTypes.size() ? cachedParamTypes[i] : types::unknown();
        Type ilParamType = mapType(paramType);
        createSlot(decl.params[i].name, ilParamType);
        storeToSlot(decl.params[i].name, Value::temp(blockParams[i + 1].id), ilParamType);
        if (ilParamType.kind == Type::Kind::Str) // params are borrowed; the slot owns +1
            emitCall(runtime::kStrRetainMaybe, {Value::temp(blockParams[i + 1].id)});
        else if (ilParamType.kind == Type::Kind::Ptr && needsRelease(paramType))
            emitCall("rt_obj_retain_maybe", {Value::temp(blockParams[i + 1].id)});
        localTypes_[decl.params[i].name] = paramType;
    }

    lowerStmt(decl.body.get());

    if (!isTerminated()) {
        releaseLocalStringSlots(); // param slots own +1; body block released its own
        releaseLocalObjectSlots();
        if (ilReturnType.kind == Type::Kind::Void) {
            emitRetVoid();
        } else {
            Value defaultValue = ilReturnType.kind == Type::Kind::F64
                                     ? Value::constFloat(0.0)
                                     : (ilReturnType.kind == Type::Kind::I1
                                            ? Value::constBool(false)
                                            : (ilReturnType.kind == Type::Kind::Ptr ||
                                                       ilReturnType.kind == Type::Kind::Str
                                                   ? Value::null()
                                                   : Value::constInt(0)));
            emitRet(defaultValue);
        }
    }

    currentFunc_ = nullptr;
    currentReturnType_ = nullptr;
    currentStructType_ = nullptr;
    currentClassType_ = nullptr;
}

//=============================================================================
// Method Declaration Lowering
//=============================================================================

/// @brief Lower a class or struct method to an IL function.
/// @param decl Method declaration AST node.
/// @param typeName Qualified owning-type name (used for type lookup and name mangling).
/// @param isClass True for class (reference) methods, false for struct (value) methods.
/// @details Sets currentClassType_/currentStructType_ for the duration of lowering, prepends
///          a `self` pointer parameter for instance methods, stores all parameters in slots
///          for cross-block SSA, lowers the body, and appends a type-correct implicit return
///          on fall-through. The class/struct context is cleared on exit.
void Lowerer::lowerMethodDecl(MethodDecl &decl, const std::string &typeName, bool isClass) {
    ZiaLocationScope locScope(*this, decl.loc);

    if (!decl.body)
        return;

    // Find the type info
    if (isClass) {
        auto it = classTypes_.find(typeName);
        if (it == classTypes_.end())
            return;
        currentClassType_ = &it->second;
        currentStructType_ = nullptr;
    } else {
        const StructTypeInfo *valueInfo = getOrCreateStructTypeInfo(typeName);
        if (!valueInfo)
            return;
        currentStructType_ = valueInfo;
        currentClassType_ = nullptr;
    }

    // Look up cached method type - this has already-substituted types for generics
    TypeRef methodType = sema_.getMethodType(typeName, &decl);
    if (!methodType)
        methodType = sema_.getMethodType(typeName, decl.name);
    std::vector<TypeRef> cachedParamTypes;
    TypeRef returnType = types::voidType();
    if (methodType && methodType->kind == TypeKindSem::Function) {
        cachedParamTypes = methodType->paramTypes();
        returnType = methodType->returnType();
    } else {
        // Fallback to direct resolution for non-generic types
        returnType = decl.returnType ? sema_.resolveType(decl.returnType.get()) : types::voidType();
        for (const auto &param : decl.params) {
            TypeRef paramType = param.type ? sema_.resolveType(param.type.get()) : types::unknown();
            cachedParamTypes.push_back(paramType);
        }
    }
    Type ilReturnType = mapType(returnType);

    // Build parameter list: self (ptr) + declared params (unless static)
    std::vector<il::core::Param> params;
    params.reserve(decl.params.size() + (decl.isStatic ? 0 : 1));
    if (!decl.isStatic) {
        params.push_back({"self", Type(Type::Kind::Ptr)});
    }

    for (size_t i = 0; i < decl.params.size(); ++i) {
        // Use cached param type if available, otherwise resolve from AST
        TypeRef paramType = (i < cachedParamTypes.size()) ? cachedParamTypes[i] : types::unknown();
        params.push_back({decl.params[i].name, mapType(paramType)});
    }

    // Mangle method name: TypeName.methodName
    std::string mangledName = sema_.loweredMethodName(typeName, &decl);
    if (mangledName.empty())
        mangledName = typeName + "." + decl.name;

    definedFunctions_.insert(mangledName);

    // Create function
    currentFunc_ = &builder_->startFunction(mangledName, ilReturnType, params);
    currentReturnType_ = returnType;
    blockMgr_.bind(builder_.get(), currentFunc_);
    locals_.clear();
    slots_.clear();
    localTypes_.clear();
    deferredTemps_.clear();

    // Create entry block with the function's params as block params
    // (required for proper VM argument passing)
    builder_->createBlock(*currentFunc_, "entry_0", currentFunc_->params);
    size_t entryIdx = currentFunc_->blocks.size() - 1;
    setBlock(entryIdx);

    // Define parameters using slot-based storage for cross-block SSA correctness
    const auto &blockParams = currentFunc_->blocks[entryIdx].params;
    if (!decl.isStatic && !blockParams.empty()) {
        // 'self' is first block param - store in slot
        createSlot("self", Type(Type::Kind::Ptr));
        storeToSlot("self", Value::temp(blockParams[0].id), Type(Type::Kind::Ptr));
    }

    // Define other parameters using slot-based storage
    size_t paramOffset = decl.isStatic ? 0 : 1;
    for (size_t i = 0; i < decl.params.size(); ++i) {
        // Block param i+offset corresponds to method param i (after self if not static)
        if (i + paramOffset < blockParams.size()) {
            // Use cached param type if available
            TypeRef paramType =
                (i < cachedParamTypes.size()) ? cachedParamTypes[i] : types::unknown();
            Type ilParamType = mapType(paramType);

            createSlot(decl.params[i].name, ilParamType);
            storeToSlot(
                decl.params[i].name, Value::temp(blockParams[i + paramOffset].id), ilParamType);
            if (ilParamType.kind == Type::Kind::Str) // params are borrowed; the slot owns +1
                emitCall(runtime::kStrRetainMaybe, {Value::temp(blockParams[i + paramOffset].id)});
            else if (ilParamType.kind == Type::Kind::Ptr && needsRelease(paramType))
                emitCall("rt_obj_retain_maybe", {Value::temp(blockParams[i + paramOffset].id)});
            // Store parameter type for expression lowering
            localTypes_[decl.params[i].name] = paramType;
        }
    }

    // Lower method body
    if (decl.body) {
        lowerStmt(decl.body.get());
    }

    // Add implicit return if needed (Bug #5 fix: use correct default value for each type)
    if (!isTerminated()) {
        releaseLocalStringSlots(); // param slots own +1; body block released its own
        releaseLocalObjectSlots();
        if (ilReturnType.kind == Type::Kind::Void) {
            emitRetVoid();
        } else {
            // Emit correct default value based on return type
            Value defaultValue;
            switch (ilReturnType.kind) {
                case Type::Kind::I1:
                    defaultValue = Value::constBool(false);
                    break;
                case Type::Kind::I64:
                case Type::Kind::I16:
                case Type::Kind::I32:
                    defaultValue = Value::constInt(0);
                    break;
                case Type::Kind::F64:
                    defaultValue = Value::constFloat(0.0);
                    break;
                case Type::Kind::Str:
                    defaultValue = Value::constStr("");
                    break;
                case Type::Kind::Ptr:
                    defaultValue = Value::null();
                    break;
                default:
                    defaultValue = Value::constInt(0);
                    break;
            }
            emitRet(defaultValue);
        }
    }

    currentFunc_ = nullptr;
    currentReturnType_ = nullptr;
    currentStructType_ = nullptr;
    currentClassType_ = nullptr;
}

//=============================================================================
// Property Declaration Lowering
//=============================================================================

/// @brief Synthesize getter/setter IL functions for a property declaration.
/// @param decl Property declaration AST node.
/// @param typeName Qualified owning-type name (prefixes the synthesized `get_`/`set_` names).
/// @param isClass True when the owning type is a class.
/// @details When present, emits `<Type>.get_<Name>(self) -> PropType` from the getter body
///          (with a typed implicit return) and `<Type>.set_<Name>(self, value) -> Void` from
///          the setter body. Static properties omit the `self` parameter.
void Lowerer::lowerPropertyDecl(PropertyDecl &decl, const std::string &typeName, bool isClass) {
    ZiaLocationScope locScope(*this, decl.loc);

    TypeRef propType = decl.type ? sema_.resolveType(decl.type.get()) : types::unknown();
    Type ilPropType = mapType(propType);

    // Set current class/struct type context
    if (isClass) {
        auto it = classTypes_.find(typeName);
        if (it == classTypes_.end())
            return;
        currentClassType_ = &it->second;
        currentStructType_ = nullptr;
    }

    // --- Synthesize getter: get_PropertyName(self: Ptr) -> Type ---
    if (decl.getterBody) {
        std::string getterName = typeName + ".get_" + decl.name;

        std::vector<il::core::Param> params;
        if (!decl.isStatic)
            params.push_back({"self", Type(Type::Kind::Ptr)});

        currentFunc_ = &builder_->startFunction(getterName, ilPropType, params);
        currentReturnType_ = propType;
        blockMgr_.bind(builder_.get(), currentFunc_);
        locals_.clear();
        slots_.clear();
        localTypes_.clear();
        deferredTemps_.clear();

        builder_->createBlock(*currentFunc_, "entry_0", currentFunc_->params);
        size_t entryIdx = currentFunc_->blocks.size() - 1;
        setBlock(entryIdx);

        const auto &blockParams = currentFunc_->blocks[entryIdx].params;
        if (!decl.isStatic && !blockParams.empty()) {
            createSlot("self", Type(Type::Kind::Ptr));
            storeToSlot("self", Value::temp(blockParams[0].id), Type(Type::Kind::Ptr));
        }

        // Lower getter body
        if (decl.getterBody)
            lowerStmt(decl.getterBody.get());

        // Add implicit return if needed
        if (!isTerminated()) {
            Value defaultValue;
            switch (ilPropType.kind) {
                case Type::Kind::I1:
                    defaultValue = Value::constBool(false);
                    break;
                case Type::Kind::I64:
                case Type::Kind::I16:
                case Type::Kind::I32:
                    defaultValue = Value::constInt(0);
                    break;
                case Type::Kind::F64:
                    defaultValue = Value::constFloat(0.0);
                    break;
                case Type::Kind::Str:
                    defaultValue = Value::constStr("");
                    break;
                case Type::Kind::Ptr:
                    defaultValue = Value::null();
                    break;
                default:
                    defaultValue = Value::constInt(0);
                    break;
            }
            emitRet(defaultValue);
        }

        definedFunctions_.insert(getterName);
        currentFunc_ = nullptr;
        currentReturnType_ = nullptr;
    }

    // --- Synthesize setter: set_PropertyName(self: Ptr, value: Type) -> Void ---
    if (decl.setterBody) {
        std::string setterName = typeName + ".set_" + decl.name;

        std::vector<il::core::Param> params;
        if (!decl.isStatic)
            params.push_back({"self", Type(Type::Kind::Ptr)});
        params.push_back({decl.setterParam, ilPropType});

        currentFunc_ = &builder_->startFunction(setterName, Type(Type::Kind::Void), params);
        currentReturnType_ = types::voidType();
        blockMgr_.bind(builder_.get(), currentFunc_);
        locals_.clear();
        slots_.clear();
        localTypes_.clear();
        deferredTemps_.clear();

        builder_->createBlock(*currentFunc_, "entry_0", currentFunc_->params);
        size_t entryIdx = currentFunc_->blocks.size() - 1;
        setBlock(entryIdx);

        const auto &blockParams = currentFunc_->blocks[entryIdx].params;
        size_t paramIdx = 0;
        if (!decl.isStatic && paramIdx < blockParams.size()) {
            createSlot("self", Type(Type::Kind::Ptr));
            storeToSlot("self", Value::temp(blockParams[paramIdx].id), Type(Type::Kind::Ptr));
            ++paramIdx;
        }
        if (paramIdx < blockParams.size()) {
            createSlot(decl.setterParam, ilPropType);
            storeToSlot(decl.setterParam, Value::temp(blockParams[paramIdx].id), ilPropType);
            if (ilPropType.kind == Type::Kind::Str) // setter param is borrowed; the slot owns +1
                emitCall(runtime::kStrRetainMaybe, {Value::temp(blockParams[paramIdx].id)});
            else if (ilPropType.kind == Type::Kind::Ptr && needsRelease(propType))
                emitCall("rt_obj_retain_maybe", {Value::temp(blockParams[paramIdx].id)});
            localTypes_[decl.setterParam] = propType;
        }

        // Lower setter body
        lowerStmt(decl.setterBody.get());

        // Add implicit return void
        if (!isTerminated()) {
            releaseLocalStringSlots(); // setter param slot owns +1
            releaseLocalObjectSlots();
            emitRetVoid();
        }

        definedFunctions_.insert(setterName);
        currentFunc_ = nullptr;
        currentReturnType_ = nullptr;
    }

    currentClassType_ = nullptr;
    currentStructType_ = nullptr;
}

//=============================================================================
// Destructor Declaration Lowering
//=============================================================================

/// @brief Lower a class destructor into the `<Type>.__dtor(self)` function.
/// @param decl Destructor declaration AST node.
/// @param typeName Qualified owning-class name.
/// @details Lowers the user-defined body, then on fall-through releases each reference-typed
///          field: weak fields via `Zanna.Memory.WeakRef.Free`, and owned `Str`/`Ptr` fields
///          via emitManagedRelease(). Emits an implicit return-void.
void Lowerer::lowerDestructorDecl(DestructorDecl &decl, const std::string &typeName) {
    ZiaLocationScope locScope(*this, decl.loc);

    auto it = classTypes_.find(typeName);
    if (it == classTypes_.end())
        return;
    currentClassType_ = &it->second;
    currentStructType_ = nullptr;

    // Emit __dtor function: TypeName.__dtor(self: Ptr) -> Void
    std::string dtorName = typeName + ".__dtor";

    std::vector<il::core::Param> params;
    params.push_back({"self", Type(Type::Kind::Ptr)});

    currentFunc_ = &builder_->startFunction(dtorName, Type(Type::Kind::Void), params);
    currentReturnType_ = types::voidType();
    blockMgr_.bind(builder_.get(), currentFunc_);
    locals_.clear();
    slots_.clear();
    localTypes_.clear();
    deferredTemps_.clear();

    builder_->createBlock(*currentFunc_, "entry_0", currentFunc_->params);
    size_t entryIdx = currentFunc_->blocks.size() - 1;
    setBlock(entryIdx);

    const auto &blockParams = currentFunc_->blocks[entryIdx].params;
    if (!blockParams.empty()) {
        createSlot("self", Type(Type::Kind::Ptr));
        storeToSlot("self", Value::temp(blockParams[0].id), Type(Type::Kind::Ptr));
    }

    // Lower user-defined destructor body
    if (decl.body)
        lowerStmt(decl.body.get());

    // Release reference-typed fields (Str and Ptr)
    if (!isTerminated()) {
        Value selfPtr = loadFromSlot("self", Type(Type::Kind::Ptr));
        const ClassTypeInfo &info = *currentClassType_;

        for (const auto &field : info.fields) {
            if (field.isWeak) {
                Value fieldAddr = emitGEP(selfPtr, static_cast<int64_t>(field.offset));
                Value fieldValue = emitLoad(fieldAddr, Type(Type::Kind::Ptr));
                emitCall(kMemoryWeakRefFree, {fieldValue});
                continue;
            }
            Type ilFieldType = mapType(field.type);
            if (ilFieldType.kind == Type::Kind::Str) {
                Value fieldAddr = emitGEP(selfPtr, static_cast<int64_t>(field.offset));
                Value fieldValue = emitLoad(fieldAddr, Type(Type::Kind::Str));
                emitManagedRelease(fieldValue, /*isString=*/true);
            } else if (ilFieldType.kind == Type::Kind::Ptr) {
                Value fieldAddr = emitGEP(selfPtr, static_cast<int64_t>(field.offset));
                Value fieldValue = emitLoad(fieldAddr, Type(Type::Kind::Ptr));
                emitManagedRelease(fieldValue, /*isString=*/false);
            }
        }
    }

    // Emit return void
    if (!isTerminated())
        emitRetVoid();

    definedFunctions_.insert(dtorName);
    currentFunc_ = nullptr;
    currentReturnType_ = nullptr;
    currentClassType_ = nullptr;
    currentStructType_ = nullptr;
}

} // namespace il::frontends::zia
