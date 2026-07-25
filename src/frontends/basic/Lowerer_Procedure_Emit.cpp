//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Lowerer_Procedure_Emit.cpp
// Purpose: Procedure body emission, parameter materialization, and state reset.
//
// Phase: Emission (final phase of procedure lowering)
//
// Key Invariants:
// - ProcedureLowering orchestrates the five-phase lowering pipeline:
//   1. makeContext: Build context with all procedure references
//   2. resetContext: Clear per-procedure state
//   3. collectProcedureInfo: Gather metadata (symbols, params, body)
//   4. scheduleBlocks: Create IL function skeleton
//   5. emitProcedureIL: Emit IL instructions for body
// - Empty bodies use fast path via config.emitEmptyBody
// - Exit block receives cleanup (deferred temps, object/array release)
// - FUNCTION returns use VB-style implicit return via function name slot
//
// Ownership/Lifetime: Operates on borrowed Lowerer instance.
//
//===----------------------------------------------------------------------===//

/**
 * @file Lowerer_Procedure_Emit.cpp
 * @brief Implements procedure pipeline orchestration, parameters, and cleanup.
 *
 * Each LoweringContext borrows its AST vectors and configuration. This unit
 * creates function storage in the active module, owns no AST/IL nodes itself,
 * and leaves the procedure context positioned according to the emitted path.
 */

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/EmitCommon.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/LoweringPipeline.hpp"
#include "frontends/basic/lower/Emitter.hpp"

#include "il/core/Linkage.hpp"
#include "zanna/il/IRBuilder.hpp"

#include <cassert>
#include <memory>

using namespace il::core;

namespace il::frontends::basic {

using pipeline_detail::coreTypeForAstType;

// =============================================================================
// ProcedureLowering Pipeline Methods
// =============================================================================

/// @brief Construct procedure-lowering helpers bound to a parent Lowerer.
/// @param lowerer Borrowed parent that must outlive this pipeline helper.
ProcedureLowering::ProcedureLowering(Lowerer &lowerer) : lowerer(lowerer) {}

/// @brief Build a lowering context for a specific procedure body.
/// @details Validates that the parent @ref Lowerer owns an active IR builder and
///          bundles together the core references required to emit IL for the
///          procedure.
/// @param name Procedure identifier.
/// @param params Parameter declarations in declaration order.
/// @param body Sequence of statements forming the body.
/// @param config Behavioural hooks controlling lowering.
/// @return Fully initialised lowering context ready for emission.
ProcedureLowering::LoweringContext ProcedureLowering::makeContext(
    const std::string &name,
    const std::vector<Param> &params,
    const std::vector<StmtPtr> &body,
    const Lowerer::ProcedureConfig &config) {
    assert(lowerer.builder && "makeContext requires an active IRBuilder");
    return LoweringContext(
        lowerer, lowerer.symbols, *lowerer.builder, lowerer.emitter(), name, params, body, config);
}

/// @brief Reset shared lowering state prior to emitting a new procedure.
/// @details Defers to @ref Lowerer::resetLoweringState; the @p ctx parameter
///          exists for symmetry with other hooks and future expansion.
/// @param ctx Active lowering context (unused).
void ProcedureLowering::resetContext(LoweringContext &ctx) {
    lowerer.resetLoweringState();
    (void)ctx;
}

/// @brief Compute metadata describing the procedure prior to emission.
/// @details Invokes @ref Lowerer::collectProcedureMetadata to gather parameter
///          names, IR parameter descriptions, and the flattened statement list.
/// @param ctx Lowering context receiving the computed metadata.
void ProcedureLowering::collectProcedureInfo(LoweringContext &ctx) {
    auto metadata = std::make_shared<Lowerer::ProcedureMetadata>(
        lowerer.collectProcedureMetadata(ctx.params, ctx.body, ctx.config));
    ctx.metadata = metadata;
    ctx.paramCount = metadata->paramCount;
    ctx.bodyStmts = metadata->bodyStmts;
    ctx.paramNames = metadata->paramNames;
    ctx.irParams = metadata->irParams;
}

/// @brief Create the basic block skeleton for a procedure.
/// @details Validates required callbacks, allocates entry/exit blocks, assigns
///          synthetic labels for each unique source line, and materialises
///          parameter slots.
/// @param ctx Lowering context describing the procedure.
void ProcedureLowering::scheduleBlocks(LoweringContext &ctx) {
    const auto &config = ctx.config;
    assert(config.emitEmptyBody && "Missing empty body return handler");
    assert(config.emitFinalReturn && "Missing final return handler");
    if (!config.emitEmptyBody || !config.emitFinalReturn)
        return;

    // Clear any deferred temps from module-level init or prior procedures
    lowerer.clearDeferredTemps();

    auto metadata = ctx.metadata;
    auto &procCtx = lowerer.context();
    il::core::Function &f = ctx.builder.startFunction(ctx.name, config.retType, ctx.irParams);
    ctx.function = &f;
    procCtx.setFunction(&f);
    procCtx.setNextTemp(f.valueNames.size());

    lowerer.buildProcedureSkeleton(f, ctx.name, *metadata);

    if (!f.blocks.empty())
        procCtx.setCurrent(&f.blocks.front());

    lowerer.materializeParams(ctx.params);
    lowerer.allocateLocalSlots(ctx.paramNames, /*includeParams=*/false);
}

/// @brief Emit IL instructions for the procedure body.
/// @details Handles both the empty-body fast path (delegating entirely to the
///          configuration callback) and the general case where statements are
///          lowered sequentially.  After lowering, performs cleanup including
///          deferred temp release and object/array local release.
/// @param ctx Lowering context that owns the partially constructed function.
void ProcedureLowering::emitProcedureIL(LoweringContext &ctx) {
    const auto &config = ctx.config;
    if (!config.emitEmptyBody || !config.emitFinalReturn || !ctx.function)
        return;

    auto &procCtx = lowerer.context();

    // Fast path for empty bodies
    if (ctx.bodyStmts.empty()) {
        lowerer.curLoc = {};
        config.emitEmptyBody();
        // Remove any empty blocks (e.g., the exit block created by skeleton that's now unreachable)
        if (ctx.function) {
            auto &blocks = ctx.function->blocks;
            /// Identify skeleton blocks containing no emitted instructions.
            blocks.erase(std::remove_if(blocks.begin(),
                                        blocks.end(),
                                        [](const auto &bb) { return bb.instructions.empty(); }),
                         blocks.end());
        }
        procCtx.blockNames().resetNamer();
        return;
    }

    // Lower the procedure body
    lowerer.lowerStatementSequence(ctx.bodyStmts, /*stopOnTerminated=*/true);

    // Patch any empty preallocated line blocks with branch to exit
    patchEmptyLineBlocks(ctx);

    // Emit cleanup in exit block
    emitProcedureCleanup(ctx);

    procCtx.blockNames().resetNamer();
}

/// @brief Patch empty line blocks with explicit branch to exit.
/// @details BUG-052 guard: ensures no preallocated line blocks remain completely
///          empty, which would fail verification.
/// @param ctx Lowering context with the function being patched.
void ProcedureLowering::patchEmptyLineBlocks(LoweringContext &ctx) {
    if (!ctx.function)
        return;

    auto &f = *ctx.function;
    auto &procCtx = lowerer.context();
    int exitIdx = procCtx.exitIndex();

    for (std::size_t i = 0; i < f.blocks.size(); ++i) {
        if (i == 0 || i == static_cast<std::size_t>(exitIdx))
            continue; // skip entry and exit
        auto &bb = f.blocks[i];
        if (bb.instructions.empty()) {
            procCtx.setCurrent(&bb);
            lowerer.emitBr(&f.blocks[exitIdx]);
        }
    }
}

/// @brief Emit cleanup code in the procedure's exit block.
/// @details Switches to exit block, releases deferred temps, objects, and arrays,
///          then invokes the configured final return callback.
/// @param ctx Lowering context for cleanup emission.
void ProcedureLowering::emitProcedureCleanup(LoweringContext &ctx) {
    auto &procCtx = lowerer.context();
    const auto &config = ctx.config;

    procCtx.setCurrent(&ctx.function->blocks[procCtx.exitIndex()]);
    lowerer.curLoc = {};
    lowerer.releaseDeferredTemps();

    // BUG-OOP-035 fix: Exclude function name from release for object-returning functions
    std::unordered_set<std::string> excludeFromRelease = ctx.paramNames;
    if (config.retType.kind == il::core::Type::Kind::Ptr) {
        excludeFromRelease.insert(ctx.name);
    }

    lowerer.releaseObjectLocals(excludeFromRelease);
    // BUG-105 fix: Don't release object/array parameters - they are borrowed references from caller
    // lowerer.releaseObjectParams(ctx.paramNames);  // REMOVED - params not owned by callee
    lowerer.releaseArrayLocals(ctx.paramNames);
    // lowerer.releaseArrayParams(ctx.paramNames);  // REMOVED - params not owned by callee

    lowerer.curLoc = {};
    config.emitFinalReturn();
}

// =============================================================================
// Procedure Metadata Collection
// =============================================================================

/// @brief Gather metadata required to lower a single procedure body.
/// @details Records the number of parameters, flattens the body statements,
///          discovers symbol usage, and executes optional callbacks.
/// @param params Declaration-time parameters for the procedure.
/// @param body Statements composing the procedure body.
/// @param config Configuration hooks supplied by the caller.
/// @return Populated metadata structure consumed by later lowering stages.
Lowerer::ProcedureMetadata Lowerer::collectProcedureMetadata(const std::vector<Param> &params,
                                                             const std::vector<StmtPtr> &body,
                                                             const ProcedureConfig &config) {
    // BUG-BAS-002 fix: Register param names and types early so collectVars doesn't pollute them
    // with module-level object types of the same name and so type inference uses correct types.
    for (const auto &p : params) {
        registerProcParam(p.name);
        if (!p.objectClass.empty())
            setSymbolObjectType(p.name, qualify(p.objectClass));
        else
            setSymbolType(p.name, p.type);
    }

    ProcedureMetadata metadata;
    metadata.paramCount = params.size();
    metadata.bodyStmts.reserve(body.size());
    for (const auto &stmt : body)
        metadata.bodyStmts.push_back(stmt.get());

    collectVars(metadata.bodyStmts);

    if (config.postCollect)
        config.postCollect();

    metadata.irParams.reserve(params.size());
    for (const auto &p : params) {
        metadata.paramNames.insert(p.name);
        Type ty = computeParamILType(p);
        metadata.irParams.push_back({p.name, ty});
        if (p.is_array) {
            requireArrayI64Retain();
            requireArrayI64Release();
        }
    }

    return metadata;
}

/// @brief Compute the IL type for a procedure parameter.
/// @details Arrays, objects, and BYREF parameters all use pointer types.
/// @param p Parameter declaration.
/// @return IL type for the parameter slot.
il::core::Type Lowerer::computeParamILType(const Param &p) {
    if (p.is_array)
        return il::core::Type(il::core::Type::Kind::Ptr);
    if (!p.objectClass.empty())
        return il::core::Type(il::core::Type::Kind::Ptr);
    if (p.isByRef)
        return il::core::Type(il::core::Type::Kind::Ptr);
    return coreTypeForAstType(p.type);
}

// =============================================================================
// Parameter Materialization
// =============================================================================

/// @brief Allocate stack slots and store incoming arguments for parameters.
/// @details For each parameter: allocates a stack slot, stores default values
///          for arrays, records the slot identifier, and writes the incoming
///          argument value into the slot.
/// @param params Procedure parameters in declaration order.
void Lowerer::materializeParams(const std::vector<Param> &params) {
    ProcedureContext &ctx = context();
    Function *func = ctx.function();
    assert(func && "materializeParams requires an active function");

    size_t ilParamOffset = 0;
    if (func->params.size() >= params.size())
        ilParamOffset = func->params.size() - params.size();

    for (size_t i = 0; i < params.size(); ++i) {
        materializeSingleParam(params[i], i, ilParamOffset);
    }
}

/// @brief Materialize a single parameter into a stack slot.
/// @param p Parameter to materialize.
/// @param index Parameter index in declaration order.
/// @param ilParamOffset Offset into IL function params.
void Lowerer::materializeSingleParam(const Param &p, size_t index, size_t ilParamOffset) {
    ProcedureContext &ctx = context();
    Function *func = ctx.function();

    // BUG-BAS-002 fix: Register parameter name to prevent module-level shadowing
    registerProcParam(p.name);

    bool isBoolParam = !p.is_array && p.type == AstType::Bool;
    bool isObjectParam = !p.objectClass.empty();
    const size_t ilIndex = ilParamOffset + index;

    Value incoming =
        (ilIndex < func->params.size()) ? Value::temp(func->params[ilIndex].id) : Value::null();
    bool byRef = p.isByRef;
    Value slot = byRef ? incoming : emitAlloca(isBoolParam ? 1 : 8);

    if (p.is_array) {
        markArray(p.name);
        emitStore(Type(Type::Kind::Ptr), slot, Value::null());
    }

    if (isObjectParam) {
        setSymbolObjectType(p.name, p.objectClass);
    } else {
        setSymbolType(p.name, p.type);
    }
    markSymbolReferenced(p.name);

    auto &info = ensureSymbol(p.name);
    info.slotId = slot.id;
    info.isByRefParam = byRef;

    if (ilIndex >= func->params.size())
        return;

    il::core::Type ty = func->params[ilIndex].type;
    if (p.is_array) {
        bool isObjectArray = !p.objectClass.empty();
        storeArray(slot, incoming, p.type, isObjectArray);
    } else if (!byRef) {
        emitStore(ty, slot, incoming);
    }
}

// =============================================================================
// FUNCTION/SUB Declaration Lowering
// =============================================================================

/// @brief Lower a BASIC FUNCTION declaration into IL.
/// @details Prepares a @ref ProcedureConfig that seeds the return value with the
///          correct default, ensures the function name's symbol adopts the
///          declared return type, and delegates to @ref lowerProcedure.
/// @param decl AST node describing the function declaration.
void Lowerer::lowerFunctionDecl(const FunctionDecl &decl) {
    /// Produce the default return value dictated by the declared return kind.
    auto defaultRet = [&]() {
        if (!decl.explicitClassRetQname.empty())
            return Value::null();
        switch (decl.ret) {
            case ::il::frontends::basic::Type::I64:
                return Value::constInt(0);
            case ::il::frontends::basic::Type::F64:
                return Value::constFloat(0.0);
            case ::il::frontends::basic::Type::Str:
                return emitConstStr(getStringLabel(""));
            case ::il::frontends::basic::Type::Bool:
                return emitBoolConst(false);
        }
        return Value::constInt(0);
    };

    ProcedureConfig config;
    if (!decl.explicitClassRetQname.empty()) {
        config.retType = Type(Type::Kind::Ptr);
        /// Retag the function-name result slot with its resolved class type.
        config.postCollect = [&]() {
            if (findSymbol(decl.name)) {
                std::string q = resolveQualifiedClassCasing(JoinDots(decl.explicitClassRetQname));
                setSymbolObjectType(decl.name, q);
            }
        };
    } else {
        config.retType = functionRetTypeFromHint(decl.name, decl.explicitRetType);
        /// Retag the function-name result slot with its declared scalar type.
        config.postCollect = [&]() {
            if (findSymbol(decl.name))
                setSymbolType(decl.name, decl.ret);
        };
    }
    /// Return the declared default when the function has no statements.
    config.emitEmptyBody = [&]() { emitRet(defaultRet()); };
    /// Load the VB-style function-name result slot or fall back to the default.
    config.emitFinalReturn = [&]() {
        // VB-style implicit return: check if function name was assigned
        if (auto storage = resolveVariableStorage(decl.name, {})) {
            const bool isClassReturn = !decl.explicitClassRetQname.empty();
            Type loadTy = isClassReturn ? Type(Type::Kind::Ptr) : storage->slotInfo.type;
            Value val = emitLoad(loadTy, storage->pointer);
            emitRet(val);
        } else {
            emitRet(defaultRet());
        }
    };

    const std::string ilName = decl.qualifiedName.empty() ? decl.name : decl.qualifiedName;

    // Foreign (import) functions: emit declaration only, no body.
    if (decl.isForeign) {
        std::vector<il::core::Param> irParams;
        for (size_t i = 0; i < decl.params.size(); ++i) {
            il::core::Param p;
            p.name = decl.params[i].name;
            p.type = coreTypeForAstType(decl.params[i].type);
            p.id = static_cast<unsigned>(i);
            irParams.push_back(p);
        }
        auto &f = builder->startFunction(ilName, config.retType, irParams);
        f.linkage = il::core::Linkage::Import;
        return;
    }

    lowerProcedure(ilName, decl.params, decl.body, config);

    // Set export linkage after lowering (function is now in the module).
    if (decl.isExport && !mod->functions.empty() && mod->functions.back().name == ilName)
        mod->functions.back().linkage = il::core::Linkage::Export;
}

/// @brief Lower a BASIC SUB declaration into IL.
/// @details Configures a void-returning @ref ProcedureConfig and delegates to
///          @ref lowerProcedure.
/// @param decl AST node describing the subroutine declaration.
void Lowerer::lowerSubDecl(const SubDecl &decl) {
    ProcedureConfig config;
    config.retType = Type(Type::Kind::Void);
    /// Terminate an empty SUB.
    config.emitEmptyBody = [&]() { emitRetVoid(); };
    /// Terminate the synthetic exit block of a non-empty SUB.
    config.emitFinalReturn = [&]() { emitRetVoid(); };

    const std::string ilName = decl.qualifiedName.empty() ? decl.name : decl.qualifiedName;

    // Foreign (import) subs: emit declaration only, no body.
    if (decl.isForeign) {
        std::vector<il::core::Param> irParams;
        for (size_t i = 0; i < decl.params.size(); ++i) {
            il::core::Param p;
            p.name = decl.params[i].name;
            p.type = coreTypeForAstType(decl.params[i].type);
            p.id = static_cast<unsigned>(i);
            irParams.push_back(p);
        }
        auto &f = builder->startFunction(ilName, config.retType, irParams);
        f.linkage = il::core::Linkage::Import;
        return;
    }

    lowerProcedure(ilName, decl.params, decl.body, config);

    // Set export linkage after lowering (function is now in the module).
    if (decl.isExport && !mod->functions.empty() && mod->functions.back().name == ilName)
        mod->functions.back().linkage = il::core::Linkage::Export;
}

// =============================================================================
// State Reset
// =============================================================================

/// @brief Clear all procedure-specific lowering state.
/// @details Resets the symbol table, clears the procedure context, and drops the
///          cache of synthetic line numbers so the next procedure starts fresh.
void Lowerer::resetLoweringState() {
    resetSymbolState();
    context().reset();
    stmtVirtualLines_.clear();
    synthSeq_ = 0;
    clearDeferredTemps();
    clearProcParams(); // BUG-BAS-002 fix
}

// =============================================================================
// Context and Emitter Accessors
// =============================================================================

/// @brief Access the mutable procedure context for the current lowering run.
/// @return Mutable context owned by this Lowerer.
Lowerer::ProcedureContext &Lowerer::context() noexcept {
    return context_;
}

/// @brief Access the immutable procedure context for the current lowering run.
/// @return Const context owned by this Lowerer.
const Lowerer::ProcedureContext &Lowerer::context() const noexcept {
    return context_;
}

/// @brief Construct an @ref Emit helper bound to the current lowering state.
/// @return Lightweight facade borrowing this Lowerer.
Emit Lowerer::emitCommon() noexcept {
    return Emit(*this);
}

/// @brief Construct an emit helper and pre-set its source location.
/// @param loc Source location installed in the facade.
/// @return Lightweight facade borrowing this Lowerer.
Emit Lowerer::emitCommon(il::support::SourceLoc loc) noexcept {
    Emit helper(*this);
    helper.at(loc);
    return helper;
}

/// @brief Retrieve the shared lowering emitter.
/// @return Mutable owned emitter reference.
lower::Emitter &Lowerer::emitter() noexcept {
    assert(emitter_ && "emitter must be initialized");
    return *emitter_;
}

/// @brief Retrieve the shared lowering emitter (const overload).
/// @return Const owned emitter reference.
const lower::Emitter &Lowerer::emitter() const noexcept {
    assert(emitter_ && "emitter must be initialized");
    return *emitter_;
}

// =============================================================================
// Temporary ID and Block Label Generation
// =============================================================================

/// @brief Reserve a fresh temporary identifier for IL value creation.
/// @details Prefers the active builder's allocator, otherwise advances the
///          procedure context counter. If a function exists, its value-name
///          table is extended and an empty entry receives a `%tN` name.
/// @return Reserved identifier.
unsigned Lowerer::nextTempId() {
    ProcedureContext &ctx = context();
    unsigned id = 0;
    if (builder) {
        id = builder->reserveTempId();
    } else {
        id = ctx.nextTemp();
        ctx.setNextTemp(id + 1);
    }
    if (Function *func = ctx.function()) {
        if (func->valueNames.size() <= id)
            func->valueNames.resize(id + 1);
        if (func->valueNames[id].empty())
            func->valueNames[id] = "%t" + std::to_string(id);
    }
    if (ctx.nextTemp() <= id)
        ctx.setNextTemp(id + 1);
    return id;
}

/// @brief Generate a unique fallback block label for ad-hoc control flow.
/// @return Mangled `bb_N` label using the next fallback counter.
std::string Lowerer::nextFallbackBlockLabel() {
    return mangler.block("bb_" + std::to_string(nextFallbackBlockId++));
}

} // namespace il::frontends::basic
