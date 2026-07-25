//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/RuntimeStatementLowerer_Decl.cpp
// Purpose: Implementation of variable declaration runtime statement lowering.
//          Handles DIM, REDIM, CONST, STATIC, RANDOMIZE, and SWAP statements.
// Key invariants: Maintains Lowerer's runtime lowering semantics exactly.
// Ownership/Lifetime: Borrows Lowerer reference; coordinates with parent.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file RuntimeStatementLowerer_Decl.cpp
/// @brief Implements declaration, allocation, resize, randomization, and SWAP lowering.
/// @details Inclusive BASIC array bounds are converted to checked lengths;
///          allocation helpers are selected from symbol element metadata, while
///          CONST and SWAP reuse the shared lifetime-aware assignment paths.

#include "Lowerer.hpp"
#include "RuntimeStatementLowerer.hpp"
#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/LocationScope.hpp"

#include <cassert>
#include <limits>
#include <optional>

using namespace il::core;
using AstType = ::il::frontends::basic::Type;

namespace il::frontends::basic {
namespace {

/// @brief Map slot metadata back to the BASIC array element type expected by store helpers.
/// @param slotInfo Lowerer slot metadata for the array variable.
/// @return BASIC scalar type stored in each element.
AstType arrayElementTypeFromSlot(const SlotType &slotInfo) {
    if (slotInfo.type.kind == il::core::Type::Kind::Str)
        return AstType::Str;
    if (slotInfo.type.kind == il::core::Type::Kind::F64)
        return AstType::F64;
    if (slotInfo.isBoolean || slotInfo.type.kind == il::core::Type::Kind::I1)
        return AstType::Bool;
    return AstType::I64;
}

/// @brief Multiply inclusive array extents while detecting host integer overflow.
/// @param extents Inclusive upper bounds for each dimension.
/// @return Total element count, or std::nullopt if any extent is invalid/overflowing.
std::optional<long long> checkedInclusiveExtentProduct(const std::vector<long long> &extents) {
    long long total = 1;
    for (long long extent : extents) {
        if (extent < 0 || extent == std::numeric_limits<long long>::max())
            return std::nullopt;
        const long long length = extent + 1;
        if (length != 0 && total > std::numeric_limits<long long>::max() / length)
            return std::nullopt;
        total *= length;
    }
    return total;
}

} // namespace

/// @brief Lower a BASIC @c CONST statement.
///
/// @details Evaluates the initializer expression and stores it into the constant's
///          storage location. The lowering is similar to LET - constants are treated
///          as read-only variables at compile-time (semantic analysis prevents reassignment).
///
/// @param stmt Parsed @c CONST statement.
/// @pre @p stmt has a non-null initializer after successful semantic analysis.
void RuntimeStatementLowerer::lowerConst(const ConstStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);

    // Evaluate the initializer expression
    Lowerer::RVal value = lowerer_.lowerExpr(*stmt.initializer);

    // Resolve storage for the constant (same as variable)
    auto storage = lowerer_.resolveVariableStorage(stmt.name, stmt.loc);
    assert(storage && "CONST target should have storage");
    if (!storage)
        return; // Safety: skip if storage lookup fails in Release builds.

    // Store the value
    Lowerer::SlotType slotInfo = storage->slotInfo;
    if (slotInfo.isArray) {
        lowerer_.storeArray(storage->pointer,
                            value.value,
                            arrayElementTypeFromSlot(storage->slotInfo),
                            /*isObjectArray*/ storage->slotInfo.isObject);
    } else {
        assignScalarSlot(slotInfo, storage->pointer, std::move(value), stmt.loc);
    }
}

/// @brief Lower BASIC @c STATIC statements declaring procedure-local persistent variables.
///
/// @details Variable collection marks STATIC metadata, and each later use
///          resolves a procedure-qualified address through the runtime modvar
///          helpers. The declaration itself therefore emits no instruction.
///
/// @param stmt Parsed @c STATIC statement identifying the variable name and type.
void RuntimeStatementLowerer::lowerStatic(const StaticStmt &stmt) {
    // No code emission needed - storage is allocated as module-level global
    // during variable collection phase, and variable references will resolve
    // to that global storage automatically.
    (void)stmt;
}

/// @brief Emit runtime validation logic for array length expressions.
///
/// @details Adjusts the requested bound to account for BASIC's inclusive array
///          lengths, generates overflow-aware addition, and emits a conditional
///          branch to a trap block when the computed length is negative. The
///          @p labelBase parameter keeps generated block names deterministic for
///          debugging. An SSA continuation parameter carries the valid length
///          when an active function and block exist.
///
/// @param bound     Value representing the user-supplied length expression.
/// @param loc       Source location used for diagnostics and helper emission.
/// @param labelBase Prefix used when naming generated failure blocks.
/// @return Continuation block parameter when validation control flow is emitted;
///         otherwise the checked `bound + 1` result.
Value RuntimeStatementLowerer::emitArrayLengthCheck(Value bound,
                                                    il::support::SourceLoc loc,
                                                    std::string_view labelBase) {
    LocationScope location(lowerer_, loc);
    Value length =
        lowerer_.emitCommon(loc).add_checked(bound, Value::constInt(1), OverflowPolicy::Checked);

    ProcedureContext &ctx = lowerer_.context();
    Function *func = ctx.function();
    BasicBlock *original = ctx.current();
    if (func && original) {
        size_t curIdx = ctx.blockIndex(original);
        BlockNamer *blockNamer = ctx.blockNames().namer();

        std::string base(labelBase);
        std::string failName = base.empty() ? "arr_len_fail" : base + "_fail";
        std::string contName = base.empty() ? "arr_len_cont" : base + "_cont";

        std::string failLbl =
            blockNamer ? blockNamer->generic(failName) : lowerer_.mangler.block(failName);
        std::string contLbl =
            blockNamer ? blockNamer->generic(contName) : lowerer_.mangler.block(contName);

        size_t failIdx = func->blocks.size();
        lowerer_.builder->addBlock(*func, failLbl);
        // Create contBlk with a block parameter for length to ensure proper SSA form
        // across the conditional branch. This is required for native codegen which
        // cannot reference values defined in predecessor blocks.
        std::vector<il::core::Param> contParams = {
            {"len", il::core::Type(il::core::Type::Kind::I64)}};
        size_t contIdx = func->blocks.size();
        lowerer_.builder->createBlock(*func, contLbl, contParams);

        BasicBlock *failBlk = &func->blocks[failIdx];
        BasicBlock *contBlk = &func->blocks[contIdx];

        ctx.setCurrent(&func->blocks[curIdx]);
        Value isNeg =
            lowerer_.emitBinary(Opcode::SCmpLT, lowerer_.ilBoolTy(), length, Value::constInt(0));

        // Emit CBr with branch arguments: pass length to contBlk via block parameter
        Instr cbr;
        cbr.op = Opcode::CBr;
        cbr.type = il::core::Type(il::core::Type::Kind::Void);
        cbr.operands.push_back(isNeg);
        cbr.addBranchTarget(failLbl);           // failBlk has no parameters
        cbr.addBranchTarget(contLbl, {length}); // contBlk receives length
        cbr.loc = lowerer_.curLoc;
        BasicBlock *curBlock = ctx.current();
        curBlock->instructions.push_back(cbr);
        curBlock->terminated = true;

        ctx.setCurrent(failBlk);
        lowerer_.emitTrap();

        ctx.setCurrent(contBlk);
        // Return the block parameter value instead of the original length.
        // This ensures the value is properly defined in contBlk for SSA.
        return lowerer_.builder->blockParam(*contBlk, 0);
    }

    return length;
}

/// @brief Lower BASIC @c DIM declarations into runtime allocations.
///
/// @details Iterates the declared arrays, evaluates bounds with
///          @ref emitArrayLengthCheck, and emits runtime helper calls to allocate
///          the storage.  Newly allocated arrays are stored into their target
///          slots with retain bookkeeping configured so later scope exits release
///          the memory.
///
/// @param stmt Parsed @c DIM statement to lower.
void RuntimeStatementLowerer::lowerDim(const DimStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);

    // Collect dimension expressions (backward compat: check 'size' first, then 'dimensions')
    std::vector<const ExprPtr *> dimExprs;
    if (stmt.size) {
        dimExprs.push_back(&stmt.size);
    } else {
        for (const auto &dimExpr : stmt.dimensions) {
            if (dimExpr)
                dimExprs.push_back(&dimExpr);
        }
    }
    if (dimExprs.empty()) {
        if (auto *em = lowerer_.diagnosticEmitter()) {
            em->emit(il::support::Severity::Error,
                     "B2000",
                     stmt.loc,
                     static_cast<uint32_t>(stmt.name.size()),
                     "DIM array must have at least one dimension");
        }
        lowerer_.emitTrap();
        return;
    }

    // BUG-010 fix: If resolved extents are available from semantic analysis, use them
    // to compute array size as a compile-time constant. This handles CONST dimensions
    // correctly without needing runtime module variable lookups.
    Value length;
    if (!stmt.resolvedExtents.empty() && stmt.resolvedExtents.size() == dimExprs.size()) {
        // Compute total size from resolved extents (add 1 to each for 0-based indexing)
        auto totalSize = checkedInclusiveExtentProduct(stmt.resolvedExtents);
        if (!totalSize) {
            if (auto *em = lowerer_.diagnosticEmitter()) {
                em->emit(il::support::Severity::Error,
                         "B2000",
                         stmt.loc,
                         static_cast<uint32_t>(stmt.name.size()),
                         "array size computation overflowed");
            }
            lowerer_.emitTrap();
            return;
        }
        length = lowerer_.emitConstI64(*totalSize);
    } else if (dimExprs.size() == 1) {
        // For single-dimensional arrays, use the dimension directly
        Lowerer::RVal bound = lowerer_.lowerExpr(**dimExprs[0]);
        bound = lowerer_.ensureI64(std::move(bound), stmt.loc);
        length = emitArrayLengthCheck(bound.value, stmt.loc, "dim_len");
    } else {
        // For multi-dimensional arrays, compute total size = product of all extents.
        // Use an alloca to store running product because emitArrayLengthCheck creates
        // new basic blocks, and values from predecessor blocks aren't accessible
        // without explicit block parameters (BUG-001 fix).
        Value sizeSlot = lowerer_.emitAlloca(8);

        // Start with first dimension
        Lowerer::RVal bound = lowerer_.lowerExpr(**dimExprs[0]);
        bound = lowerer_.ensureI64(std::move(bound), stmt.loc);
        Value firstLen = emitArrayLengthCheck(bound.value, stmt.loc, "dim_len");
        lowerer_.emitStore(il::core::Type(il::core::Type::Kind::I64), sizeSlot, firstLen);

        // Multiply by remaining dimensions
        for (size_t i = 1; i < dimExprs.size(); ++i) {
            Lowerer::RVal dimBound = lowerer_.lowerExpr(**dimExprs[i]);
            dimBound = lowerer_.ensureI64(std::move(dimBound), stmt.loc);
            Value dimLen = emitArrayLengthCheck(dimBound.value, stmt.loc, "dim_len");
            // Reload running product from alloca (accessible from current block)
            Value currentSize =
                lowerer_.emitLoad(il::core::Type(il::core::Type::Kind::I64), sizeSlot);
            Value newSize = lowerer_.emitBinary(
                Opcode::IMulOvf, il::core::Type(il::core::Type::Kind::I64), currentSize, dimLen);
            lowerer_.emitStore(il::core::Type(il::core::Type::Kind::I64), sizeSlot, newSize);
        }
        // Load final product
        length = lowerer_.emitLoad(il::core::Type(il::core::Type::Kind::I64), sizeSlot);
    }

    const auto *info = lowerer_.findSymbol(stmt.name);
    if (!info) {
        if (auto *em = lowerer_.diagnosticEmitter()) {
            em->emit(il::support::Severity::Error,
                     "B2000",
                     stmt.loc,
                     static_cast<uint32_t>(stmt.name.size()),
                     "DIM target has no symbol metadata");
        }
        lowerer_.emitTrap();
        return;
    }
    // Determine array element type and call appropriate runtime allocator
    Value handle;
    if (info->type == AstType::Str) {
        // String array: use rt_arr_str_alloc
        lowerer_.requireArrayStrAlloc();
        handle = lowerer_.emitCallRet(
            il::core::Type(il::core::Type::Kind::Ptr), "rt_arr_str_alloc", {length});
    } else if (info->isObject) {
        // Object array
        lowerer_.requireArrayObjNew();
        handle = lowerer_.emitCallRet(
            il::core::Type(il::core::Type::Kind::Ptr), "rt_arr_obj_new", {length});
    } else if (info->type == AstType::F64) {
        // Float array (SINGLE/DOUBLE): use rt_arr_f64_new
        lowerer_.requireArrayF64New();
        handle = lowerer_.emitCallRet(
            il::core::Type(il::core::Type::Kind::Ptr), "rt_arr_f64_new", {length});
    } else {
        // Integer/numeric array: use rt_arr_i64_new (all Zanna integers are 64-bit)
        lowerer_.requireArrayI64New();
        handle = lowerer_.emitCallRet(
            il::core::Type(il::core::Type::Kind::Ptr), "rt_arr_i64_new", {length});
    }

    // Store into the resolved storage (supports module-level globals across procedures)
    if (auto storage = lowerer_.resolveVariableStorage(stmt.name, stmt.loc)) {
        lowerer_.storeArray(
            storage->pointer, handle, info ? info->type : AstType::I64, info && info->isObject);
    } else {
        // Avoid hard assertions in production builds; emit a trap so the
        // failure is observable without terminating the entire test suite.
        lowerer_.emitTrap();
    }
    if (lowerer_.boundsChecks) {
        if (info && info->arrayLengthSlot)
            lowerer_.emitStore(il::core::Type(il::core::Type::Kind::I64),
                               Value::temp(*info->arrayLengthSlot),
                               length);
    }
}

/// @brief Lower BASIC @c REDIM statements that resize dynamic arrays.
///
/// @details Reuses @ref emitArrayLengthCheck for each bound, multiplies
///          multidimensional lengths with checked IL arithmetic, selects the
///          object/F64/I64 resize helper from symbol metadata, and replaces the
///          stored handle. The current parser treats REDIM as preserving by default.
///
/// @param stmt Parsed @c REDIM statement describing the new bounds.
void RuntimeStatementLowerer::lowerReDim(const ReDimStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);
    std::vector<const ExprPtr *> dimExprs;
    if (stmt.size) {
        dimExprs.push_back(&stmt.size);
    } else {
        for (const auto &dimExpr : stmt.dimensions) {
            if (dimExpr)
                dimExprs.push_back(&dimExpr);
        }
    }
    if (dimExprs.empty()) {
        lowerer_.emitTrap();
        return;
    }

    Value length;
    if (dimExprs.size() == 1) {
        Lowerer::RVal bound = lowerer_.lowerExpr(**dimExprs[0]);
        bound = lowerer_.ensureI64(std::move(bound), stmt.loc);
        length = emitArrayLengthCheck(bound.value, stmt.loc, "redim_len");
    } else {
        Value sizeSlot = lowerer_.emitAlloca(8);
        Lowerer::RVal bound = lowerer_.lowerExpr(**dimExprs[0]);
        bound = lowerer_.ensureI64(std::move(bound), stmt.loc);
        Value firstLen = emitArrayLengthCheck(bound.value, stmt.loc, "redim_len");
        lowerer_.emitStore(il::core::Type(il::core::Type::Kind::I64), sizeSlot, firstLen);
        for (size_t i = 1; i < dimExprs.size(); ++i) {
            Lowerer::RVal dimBound = lowerer_.lowerExpr(**dimExprs[i]);
            dimBound = lowerer_.ensureI64(std::move(dimBound), stmt.loc);
            Value dimLen = emitArrayLengthCheck(dimBound.value, stmt.loc, "redim_len");
            Value currentSize =
                lowerer_.emitLoad(il::core::Type(il::core::Type::Kind::I64), sizeSlot);
            Value newSize = lowerer_.emitBinary(
                Opcode::IMulOvf, il::core::Type(il::core::Type::Kind::I64), currentSize, dimLen);
            lowerer_.emitStore(il::core::Type(il::core::Type::Kind::I64), sizeSlot, newSize);
        }
        length = lowerer_.emitLoad(il::core::Type(il::core::Type::Kind::I64), sizeSlot);
    }
    const auto *info = lowerer_.findSymbol(stmt.name);
    auto storage = lowerer_.resolveVariableStorage(stmt.name, stmt.loc);
    assert(storage && "REDIM target should have resolvable storage");
    if (!storage)
        return; // Safety: skip if storage lookup fails in Release builds.
    Value current = lowerer_.emitLoad(il::core::Type(il::core::Type::Kind::Ptr), storage->pointer);
    const char *resizeFn = "rt_arr_i64_resize";
    if (info && info->isObject) {
        lowerer_.requireArrayObjResize();
        resizeFn = "rt_arr_obj_resize";
    } else if (info && info->type == AstType::F64) {
        lowerer_.requireArrayF64Resize();
        resizeFn = "rt_arr_f64_resize";
    } else {
        lowerer_.requireArrayI64Resize();
    }
    Value resized = lowerer_.emitCallRet(
        il::core::Type(il::core::Type::Kind::Ptr), resizeFn, {current, length});
    lowerer_.storeArray(storage->pointer,
                        resized,
                        info ? info->type : AstType::I64,
                        /*isObjectArray*/ info && info->isObject);
    if (lowerer_.boundsChecks && info && info->arrayLengthSlot)
        lowerer_.emitStore(
            il::core::Type(il::core::Type::Kind::I64), Value::temp(*info->arrayLengthSlot), length);
}

/// @brief Lower the BASIC @c RANDOMIZE statement configuring the RNG seed.
///
/// @details Evaluates and coerces the optional seed to I64, defaults a missing
///          seed to zero, and emits `rt_randomize_i64`.
///
/// @param stmt Parsed @c RANDOMIZE statement.
void RuntimeStatementLowerer::lowerRandomize(const RandomizeStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);
    Value seed = Value::constInt(0);
    if (stmt.seed) {
        Lowerer::RVal s = lowerer_.lowerExpr(*stmt.seed);
        seed = lowerer_.coerceToI64(std::move(s), stmt.loc).value;
    }
    lowerer_.emitCall("rt_randomize_i64", {seed});
}

/// @brief Lower a @c SWAP statement to exchange two lvalue contents.
///
/// @details Emits IL instructions to: (1) load both lvalues, (2) store first
///          value into second location, (3) store second value into first location.
///          Uses a temporary slot to hold the first value during the exchange.
///          Scalar variables and array elements are supported; other lvalue
///          shapes are left unchanged after evaluation.
///
/// @param stmt Parsed @c SWAP statement.
void RuntimeStatementLowerer::lowerSwap(const SwapStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);

    // Lower both lvalues to get their RVals
    Lowerer::RVal lhsVal = lowerer_.lowerExpr(*stmt.lhs);
    Lowerer::RVal rhsVal = lowerer_.lowerExpr(*stmt.rhs);

    // Store lhs value to temp
    Value tempSlot = lowerer_.emitAlloca(8);
    lowerer_.emitStore(lhsVal.type, tempSlot, lhsVal.value);

    // Assign rhs to lhs
    if (auto *var = as<const VarExpr>(*stmt.lhs)) {
        auto storage = lowerer_.resolveVariableStorage(var->name, stmt.loc);
        if (storage) {
            assignScalarSlot(storage->slotInfo, storage->pointer, rhsVal, stmt.loc);
        }
    } else if (auto *arr = as<const ArrayExpr>(*stmt.lhs)) {
        assignArrayElement(*arr, rhsVal, stmt.loc);
    }

    // Assign temp to rhs
    Value tempVal = lowerer_.emitLoad(lhsVal.type, tempSlot);
    Lowerer::RVal tempRVal{tempVal, lhsVal.type};
    if (auto *var = as<const VarExpr>(*stmt.rhs)) {
        auto storage = lowerer_.resolveVariableStorage(var->name, stmt.loc);
        if (storage) {
            assignScalarSlot(storage->slotInfo, storage->pointer, tempRVal, stmt.loc);
        }
    } else if (auto *arr = as<const ArrayExpr>(*stmt.rhs)) {
        assignArrayElement(*arr, tempRVal, stmt.loc);
    }
}

} // namespace il::frontends::basic
