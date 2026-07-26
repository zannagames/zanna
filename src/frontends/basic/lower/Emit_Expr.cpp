//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/Emit_Expr.cpp
// Purpose: Provide expression emission helpers for the BASIC lowerer so common
//          IL patterns remain centralised.
// Key invariants: Helpers assume the caller manages current block state and
//                 avoid emitting terminators, leaving control transfer to
//                 statement lowering routines.
// Ownership/Lifetime: Operates on ProcedureContext owned by the active Lowerer
//                     and returns IL values tracked by the lowerer's emitter.
// Links: docs/tutorials/basic-tutorial.md, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements expression emission helpers for the BASIC lowerer.
/// @details Functions in this unit create temporaries, allocate stack slots,
///          and build data-flow operations while respecting the lowerer's block
///          invariants. Each helper assumes the caller has positioned the
///          active block and will only append non-terminating instructions,
///          leaving terminator emission to control helpers. Temporary values
///          remain owned by the lowerer and are tracked through the shared
///          ProcedureContext.

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"
#include "frontends/basic/lower/Emitter.hpp"

#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/RuntimeSignaturesData.hpp"
#include "zanna/il/Module.hpp"

#include <cassert>
#include <limits>
#include <optional>
#include <utility>

using namespace il::core;

namespace il::frontends::basic {

/// @brief Fetch the canonical IL boolean type used by BASIC lowering.
/// @details Delegates to the shared @ref Emitter instance because it owns the
///          interned IL type objects. Using the emitter ensures downstream call
///          sites receive the exact handle used when materialising boolean
///          constants.
/// @return IL type representing a one-bit logical value.
Lowerer::IlType Lowerer::ilBoolTy() {
    return emitter().ilBoolTy();
}

/// @brief Emit a boolean constant value.
/// @details Wraps the emitter helper so boolean literals produced during
///          lowering stay consistent with other constant-generation paths.
/// @param v Boolean value that should be materialised.
/// @return IL value referencing the emitted constant.
Lowerer::IlValue Lowerer::emitBoolConst(bool v) {
    return emitter().emitBoolConst(v);
}

/// @brief Materialise a boolean result from two control-flow branches.
/// @details Builds the mini CFG required by short-circuit expressions by
///          delegating to the emitter. Callers provide lambdas that emit the
///          true and false branches, while this helper ensures the resulting
///          value is stored in a shared slot and merged at @p joinLabelBase.
/// @param emitThen Callback invoked when the true branch is active.
/// @param emitElse Callback invoked when the false branch is active.
/// @param thenLabelBase Base label used for the true branch blocks.
/// @param elseLabelBase Base label used for the false branch blocks.
/// @param joinLabelBase Base label for the join block where values converge.
/// @return Boolean IL value synthesised by the emitter helper.
Lowerer::IlValue Lowerer::emitBoolFromBranches(const std::function<void(Value)> &emitThen,
                                               const std::function<void(Value)> &emitElse,
                                               std::string_view thenLabelBase,
                                               std::string_view elseLabelBase,
                                               std::string_view joinLabelBase) {
    return emitter().emitBoolFromBranches(
        emitThen, emitElse, thenLabelBase, elseLabelBase, joinLabelBase);
}

/// @brief Lower a BASIC array access expression.
/// @details Requests the runtime helpers needed for bounds checks, loads the
///          backing pointer for the array variable, coerces the index to 64-bit,
///          and emits the bounds check that panics on out-of-range accesses. The
///          resulting @ref ArrayAccess struct captures the address and length so
///          callers can emit load or store operations as needed.
/// @param expr Array expression AST node being lowered.
/// @param kind Indicates whether the caller intends to load from or store to the array.
/// @return Metadata describing the computed address and optional result slot.
Lowerer::ArrayAccess Lowerer::lowerArrayAccess(const ArrayExpr &expr, ArrayAccessKind kind) {
    // Resolve storage for the target symbol instead of assuming a local slot.
    // This supports module-level globals referenced inside procedures (BUG-053),
    // where globals are routed through runtime-backed storage and do not have
    // a materialised local stack slot.
    // Member array field resolution consolidated via resolveMemberArrayField().
    // BUG-056: Detect object field array access via dotted name (e.g., B.CELLS(i)).
    // BUG-059/058: Also detect field arrays accessed without dotted syntax in methods.
    // BUG-089: Object arrays detected via non-empty objectClassName.
    // BUG-108: Local variables shadow implicit field arrays.
    const MemberArrayInfo fieldInfo = resolveMemberArrayField(expr.name);
    const bool isMemberArray = fieldInfo.isField;

    const auto *info = isMemberArray ? nullptr : findSymbol(expr.name);

    // BUG-097 fix: Check module cache for object array type info
    std::string moduleObjectClass;
    if (!isMemberArray && (!info || (info && !info->isObject))) {
        moduleObjectClass = lookupModuleArrayElemClass(expr.name);
    }

    // BUG-OOP-011 fix: Check module-level string array cache
    bool isModuleStrArrayVar = !isMemberArray && isModuleStrArray(expr.name);

    // When accessing array fields, we'll compute 'base' by loading the pointer from
    // the object's field; otherwise we load from variable storage as usual.
    ::il::frontends::basic::Type memberElemAstType = fieldInfo.elementAstType;
    bool isMemberObjectArray = fieldInfo.isObjectArray;
    Value base;

    // Only resolve storage for non-member arrays
    std::optional<VariableStorage> storage;
    if (!isMemberArray) {
        storage = resolveVariableStorage(expr.name, expr.loc);
        if (!storage) {
            if (auto *em = diagnosticEmitter()) {
                em->emit(il::support::Severity::Error,
                         "B2000",
                         expr.loc,
                         static_cast<uint32_t>(expr.name.size()),
                         "array access requires resolvable storage");
            }
            emitTrap();
            return {Value::null(), Value::null()}; // Safety: null in Release.
        }
    }

    // Require appropriate runtime functions based on array element type
    if (isMemberArray) {
        // Use memberElemAstType for field arrays (dotted or implicit)
        if (memberElemAstType == ::il::frontends::basic::Type::Str) {
            requireArrayStrLen();
            if (kind == ArrayAccessKind::Load)
                requireArrayStrGet();
            else {
                requireArrayStrPut();
                requireStrRetainMaybe();
            }
        } else if (isMemberObjectArray) {
            // BUG-089 fix: Use object array runtime functions for object fields
            requireArrayObjLen();
            if (kind == ArrayAccessKind::Load)
                requireArrayObjGet();
            else
                requireArrayObjPut();
        } else {
            requireArrayI64Len();
            if (kind == ArrayAccessKind::Load)
                requireArrayI64Get();
            else
                requireArrayI64Set();
        }
    }
    // BUG-OOP-011 fix: Also check module-level string array cache
    else if ((info && info->type == AstType::Str) || isModuleStrArrayVar) {
        requireArrayStrLen();
        if (kind == ArrayAccessKind::Load)
            requireArrayStrGet();
        else {
            requireArrayStrPut();
            requireStrRetainMaybe();
        }
    } else if ((info && info->isObject) || !moduleObjectClass.empty()) {
        // BUG-097 fix: Use object array functions for module-level object arrays too
        requireArrayObjLen();
        if (kind == ArrayAccessKind::Load)
            requireArrayObjGet();
        else
            requireArrayObjPut();
    } else {
        requireArrayI64Len();
        if (kind == ArrayAccessKind::Load)
            requireArrayI64Get();
        else
            requireArrayI64Set();
    }
    requireArrayOobPanic();

    // Capture member field extents when available so we can compute
    // correct row-major flattened indices for multi-dimensional arrays.
    std::vector<long long> memberFieldExtents;
    ProcedureContext &ctx = context();
    if (isMemberArray) {
        // Split into base variable and field name
        const std::string &full = expr.name;
        std::size_t dot = full.find('.');
        std::string baseName = fieldInfo.isDottedAccess ? full.substr(0, dot) : "ME";
        std::string fieldName = fieldInfo.isDottedAccess ? full.substr(dot + 1) : full;

        // Load the object pointer for the base
        const auto *baseSym = findSymbol(baseName);
        if (baseSym && baseSym->slotId) {
            curLoc = expr.loc;
            Value selfPtr = emitLoad(Type(Type::Kind::Ptr), Value::temp(*baseSym->slotId));
            // Find field in class layout (already looked up above)
            std::string klass = getSlotType(baseName).objectClass;
            auto it = classLayouts_.find(klass);
            if (it != classLayouts_.end()) {
                if (const ClassLayout::Field *fld = it->second.findField(fieldName)) {
                    // Type info already resolved via resolveMemberArrayField() above.
                    curLoc = expr.loc;
                    Value fieldPtr =
                        emitBinary(Opcode::GEP,
                                   Type(Type::Kind::Ptr),
                                   selfPtr,
                                   Value::constInt(static_cast<long long>(fld->offset)));
                    curLoc = expr.loc;
                    base = emitLoad(Type(Type::Kind::Ptr), fieldPtr);
                    // Use declared extents from class layout if available
                    if (fld->isArray) {
                        memberFieldExtents = fld->arrayExtents;
                    }
                }
            }
        }
    } else {
        // storage->pointer is the address of the variable's storage (local slot or
        // runtime-backed module variable). Load the array handle pointer from it.
        base = emitLoad(Type(Type::Kind::Ptr), storage->pointer);
    }

    // Collect all index expressions (backward compat: check 'index' first, then 'indices')
    std::vector<const ExprPtr *> indexExprs;
    if (expr.index) {
        indexExprs.push_back(&expr.index);
    } else {
        for (const auto &idxExpr : expr.indices) {
            if (idxExpr)
                indexExprs.push_back(&idxExpr);
        }
    }
    if (indexExprs.empty()) {
        if (auto *em = diagnosticEmitter()) {
            em->emit(il::support::Severity::Error,
                     "B2000",
                     expr.loc,
                     static_cast<uint32_t>(expr.name.size()),
                     "array access requires at least one index");
        }
        emitTrap();
        return {Value::null(), Value::null()};
    }

    // Lower all index expressions to i64 for bounds checking in the current block
    std::vector<Value> indices;
    for (const ExprPtr *idxPtr : indexExprs) {
        RVal idx = lowerExpr(**idxPtr);
        idx = coerceToI64(std::move(idx), expr.loc);
        indices.push_back(idx.value);
    }
    curLoc = expr.loc;

    // Compute flattened index for multi-dimensional arrays using row-major order
    // For N dimensions with extents [E0, E1, ..., E_{N-1}] and indices [i0, i1, ..., i_{N-1}]:
    // flat_index = i0*L1*L2*...*L_{N-1} + i1*L2*...*L_{N-1} + ... + i_{N-2}*L_{N-1} + i_{N-1}
    // where Lk = (Ek + 1) are inclusive lengths per dimension.
    Value index;
    // For implicit field arrays (e.g., inventory(i) in methods), retrieve extents from active
    // layout
    if (memberFieldExtents.empty()) {
        // Try field scope layout for implicit field arrays
        if (const FieldScope *scope = activeFieldScope(); scope && scope->layout) {
            if (const ClassLayout::Field *fld2 = scope->layout->findField(expr.name)) {
                if (fld2->isArray)
                    memberFieldExtents = fld2->arrayExtents;
            }
        }
    }
    /// @brief Computes a row-major index using the best available extent metadata.
    /// @param idxVals Lowered per-dimension indices.
    /// @return Flattened array index, or zero after emitting a diagnostic trap.
    auto computeFlatIndex = [&](const std::vector<Value> &idxVals) -> Value {
        if (idxVals.size() == 1)
            return idxVals[0];
        // Prefer member field extents when available.
        if (!memberFieldExtents.empty() && memberFieldExtents.size() == idxVals.size())
            return emitRowMajorFlatIndex(idxVals, memberFieldExtents);
        // BUG-020 fix: Use resolvedExtents from the AST node — the semantic analyzer
        // stores extents in the ArrayExpr so they survive procedure-scope cleanup.
        if (!expr.resolvedExtents.empty() && expr.resolvedExtents.size() == idxVals.size())
            return emitRowMajorFlatIndex(idxVals, expr.resolvedExtents);
        // Fallback to semantic analyzer metadata lookup for backward compatibility.
        const SemanticAnalyzer *sema = semanticAnalyzer();
        const ArrayMetadata *metadata = sema ? sema->lookupArrayMetadata(expr.name) : nullptr;
        if (metadata && metadata->extents.size() == idxVals.size())
            return emitRowMajorFlatIndex(idxVals, metadata->extents);
        if (auto *em = diagnosticEmitter()) {
            std::string msg = "cannot lower multidimensional array access for '" + expr.name +
                              "' without matching extent metadata";
            em->emit(il::support::Severity::Error,
                     "B2000",
                     expr.loc,
                     static_cast<uint32_t>(expr.name.size()),
                     std::move(msg));
        }
        emitTrap();
        return Value::constInt(0);
    };
    index = computeFlatIndex(indices);

    // Use appropriate length function based on array element type
    Value len;
    if (isMemberArray) {
        if (memberElemAstType == ::il::frontends::basic::Type::Str)
            len = emitCallRet(Type(Type::Kind::I64), "rt_arr_str_len", {base});
        else if (isMemberObjectArray)
            len = emitCallRet(Type(Type::Kind::I64), "rt_arr_obj_len", {base}); // BUG-089 fix
        else
            len = emitCallRet(Type(Type::Kind::I64), "rt_arr_i64_len", {base});
    } else {
        // BUG-OOP-011 fix: info may be null for module-level arrays accessed from procedures
        // In that case we rely on isModuleStrArrayVar and moduleObjectClass caches
        if ((info && info->type == AstType::Str) || isModuleStrArrayVar)
            len = emitCallRet(Type(Type::Kind::I64), "rt_arr_str_len", {base});
        else if ((info && info->isObject) || !moduleObjectClass.empty())
            len = emitCallRet(
                Type(Type::Kind::I64), "rt_arr_obj_len", {base}); // BUG-097: Check module cache too
        else
            len = emitCallRet(Type(Type::Kind::I64), "rt_arr_i64_len", {base});
    }
    Value isNeg = emitBinary(Opcode::SCmpLT, ilBoolTy(), index, Value::constInt(0));
    Value tooHigh = emitBinary(Opcode::SCmpGE, ilBoolTy(), index, len);
    auto emit = emitCommon(expr.loc);
    Value isNeg64 = emit.widen_to(isNeg, 1, 64, Signedness::Unsigned);
    Value tooHigh64 = emit.widen_to(tooHigh, 1, 64, Signedness::Unsigned);
    Value oobInt = emit.logical_or(isNeg64, tooHigh64);
    Value oobCond = emitBinary(Opcode::ICmpNe, ilBoolTy(), oobInt, Value::constInt(0));

    Function *func = ctx.function();
    assert(func && ctx.current());
    size_t curIdx = ctx.currentIndex();
    unsigned bcId = ctx.consumeBoundsCheckId();
    BlockNamer *blockNamer = ctx.blockNames().namer();
    size_t okIdx = func->blocks.size();
    std::string okLbl = blockNamer ? blockNamer->tag("bc_ok" + std::to_string(bcId))
                                   : mangler.block("bc_ok" + std::to_string(bcId));
    builder->addBlock(*func, okLbl);
    size_t oobIdx = func->blocks.size();
    std::string oobLbl = blockNamer ? blockNamer->tag("bc_oob" + std::to_string(bcId))
                                    : mangler.block("bc_oob" + std::to_string(bcId));
    builder->addBlock(*func, oobLbl);
    BasicBlock *ok = &func->blocks[okIdx];
    BasicBlock *oob = &func->blocks[oobIdx];
    ctx.setCurrent(&func->blocks[curIdx]);
    emitCBr(oobCond, oob, ok);

    ctx.setCurrent(oob);
    emitCall("rt_arr_oob_panic", {index, len});
    emitTrap();

    ctx.setCurrent(ok);
    // Only for string/object arrays (value or member), re-lower base/index in the ok block
    // to avoid cross-block temp reuse issues seen with reference-counted element handling.
    bool isRefCountedArray = false;
    if (isMemberArray)
        isRefCountedArray =
            (memberElemAstType == ::il::frontends::basic::Type::Str) || isMemberObjectArray;
    else if (info)
        isRefCountedArray = (info->type == AstType::Str) || info->isObject;
    // BUG-OOP-011 fix: module-level string arrays are also reference-counted
    else if (!moduleObjectClass.empty() || isModuleStrArrayVar)
        isRefCountedArray = true;

    if (isRefCountedArray) {
        Value baseOk;
        if (isMemberArray) {
            const std::string &full = expr.name;
            std::size_t dot = full.find('.');
            std::string baseName = fieldInfo.isDottedAccess ? full.substr(0, dot) : "ME";
            std::string fieldName = fieldInfo.isDottedAccess ? full.substr(dot + 1) : full;
            const auto *baseSym = findSymbol(baseName);
            if (baseSym && baseSym->slotId) {
                curLoc = expr.loc;
                Value selfPtr = emitLoad(Type(Type::Kind::Ptr), Value::temp(*baseSym->slotId));
                std::string klass = getSlotType(baseName).objectClass;
                auto it = classLayouts_.find(klass);
                if (it != classLayouts_.end()) {
                    if (const ClassLayout::Field *fld = it->second.findField(fieldName)) {
                        Value fieldPtr =
                            emitBinary(Opcode::GEP,
                                       Type(Type::Kind::Ptr),
                                       selfPtr,
                                       Value::constInt(static_cast<long long>(fld->offset)));
                        baseOk = emitLoad(Type(Type::Kind::Ptr), fieldPtr);
                    }
                }
            }
        } else {
            baseOk = emitLoad(Type(Type::Kind::Ptr), storage->pointer);
        }
        std::vector<Value> indicesOk;
        indicesOk.reserve(indexExprs.size());
        for (const ExprPtr *idxPtr : indexExprs) {
            RVal idx = lowerExpr(**idxPtr);
            idx = coerceToI64(std::move(idx), expr.loc);
            indicesOk.push_back(idx.value);
        }
        Value indexOk = computeFlatIndex(indicesOk);
        return ArrayAccess{baseOk, indexOk};
    }
    // Non-reference-counted arrays (i32/i64/f64): keep original SSA values to preserve IL golden
    // tests
    return ArrayAccess{base, index};
}

/// @copydoc Lowerer::emitRowMajorFlatIndex(const std::vector<Value> &,
///                                         const std::vector<long long> &)
Value Lowerer::emitRowMajorFlatIndex(const std::vector<Value> &idxVals,
                                     const std::vector<long long> &extents) {
    /// @brief Multiplies non-negative constants without signed overflow.
    /// @param lhs Left factor.
    /// @param rhs Right factor.
    /// @return Product, or `std::nullopt` for invalid or overflowing factors.
    auto checkedMulConst = [&](long long lhs, long long rhs) -> std::optional<long long> {
        if (lhs < 0 || rhs < 0)
            return std::nullopt;
        if (rhs != 0 && lhs > std::numeric_limits<long long>::max() / rhs)
            return std::nullopt;
        return lhs * rhs;
    };

    if (idxVals.empty() || idxVals.size() != extents.size()) {
        if (auto *em = diagnosticEmitter()) {
            em->emit(il::support::Severity::Error,
                     "B2000",
                     curLoc,
                     1,
                     "array index rank does not match array extent metadata");
        }
        emitTrap();
        return Value::constInt(0);
    }

    // Convert declared bounds to inclusive lengths: Lk = Ek + 1.
    std::vector<long long> lengths(extents.size(), 0);
    for (size_t i = 0; i < extents.size(); ++i) {
        if (extents[i] == std::numeric_limits<long long>::max()) {
            if (auto *em = diagnosticEmitter())
                em->emit(il::support::Severity::Error,
                         "B2000",
                         curLoc,
                         1,
                         "array extent is too large to flatten");
            emitTrap();
            return Value::constInt(0);
        }
        lengths[i] = extents[i] + 1;
    }
    long long stride = 1;
    for (size_t i = 1; i < lengths.size(); ++i) {
        auto product = checkedMulConst(stride, lengths[i]);
        if (!product) {
            if (auto *em = diagnosticEmitter())
                em->emit(il::support::Severity::Error,
                         "B2000",
                         curLoc,
                         1,
                         "array stride computation overflowed");
            emitTrap();
            return Value::constInt(0);
        }
        stride = *product;
    }
    Value sum =
        emitBinary(Opcode::IMulOvf, Type(Type::Kind::I64), idxVals[0], Value::constInt(stride));
    for (size_t k = 1; k < idxVals.size(); ++k) {
        stride = 1;
        for (size_t i = k + 1; i < lengths.size(); ++i) {
            auto product = checkedMulConst(stride, lengths[i]);
            if (!product) {
                if (auto *em = diagnosticEmitter())
                    em->emit(il::support::Severity::Error,
                             "B2000",
                             curLoc,
                             1,
                             "array stride computation overflowed");
                emitTrap();
                return Value::constInt(0);
            }
            stride = *product;
        }
        Value term =
            emitBinary(Opcode::IMulOvf, Type(Type::Kind::I64), idxVals[k], Value::constInt(stride));
        sum = emitBinary(Opcode::IAddOvf, Type(Type::Kind::I64), sum, term);
    }
    return sum;
}

/// @brief Emits a stack allocation in the active procedure.
/// @param bytes Number of bytes reserved in the stack frame.
/// @return Pointer-valued IL handle for the allocated slot.
Value Lowerer::emitAlloca(int bytes) {
    return emitter().emitAlloca(bytes);
}

/// @brief Emits a typed load from an address.
/// @param ty IL type loaded from memory.
/// @param addr Pointer-valued IL address.
/// @return SSA value produced by the load.
Value Lowerer::emitLoad(Type ty, Value addr) {
    return emitter().emitLoad(ty, addr);
}

/// @brief Emits a typed store to an address.
/// @param ty IL type written to memory.
/// @param addr Pointer-valued IL destination.
/// @param val Value written to @p addr.
void Lowerer::emitStore(Type ty, Value addr, Value val) {
    emitter().emitStore(ty, addr, val);
}

/// @brief Emits a binary IL instruction through the shared emitter.
/// @param op Binary opcode to append.
/// @param ty Result type recorded on the instruction.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return SSA result of the binary instruction.
Value Lowerer::emitBinary(Opcode op, Type ty, Value lhs, Value rhs) {
    return emitter().emitBinary(op, ty, lhs, rhs);
}

/// @brief Emits a unary IL instruction through the shared emitter.
/// @param op Unary opcode to append.
/// @param ty Result type recorded on the instruction.
/// @param val Operand consumed by the instruction.
/// @return SSA result of the unary instruction.
Value Lowerer::emitUnary(Opcode op, Type ty, Value val) {
    return emitter().emitUnary(op, ty, val);
}

/// @brief Emits integer negation with the lowerer's checked-overflow policy.
/// @param ty Integer type of @p val and the result.
/// @param val Integer operand to negate.
/// @return Negated SSA value; overflow follows the emitter's trap path.
Value Lowerer::emitCheckedNeg(Type ty, Value val) {
    return emitter().emitCheckedNeg(ty, val);
}

/// @brief Narrow a 64-bit value to 32 bits.
/// @details Convenience helper that wraps emitCommon().to_iN(value, 32) to reduce
///          boilerplate when preparing arguments for 32-bit runtime function calls.
///          This pattern appears frequently in LowerStmt_Runtime.cpp when calling
///          terminal control functions that expect i32 arguments.
/// @param value The value to narrow (typically i64 from BASIC expressions).
/// @param loc Source location for the narrowing instruction.
/// @return Narrowed 32-bit value suitable for passing to runtime helpers.
Value Lowerer::narrow32(Value value, il::support::SourceLoc loc) {
    return emitCommon(loc).to_iN(value, 32);
}

namespace {
/// @brief Selects the preferred canonical runtime spelling for a callee.
/// @details Runtime aliases sharing a generated signature ID are ranked with
///          Zanna.String names first, Zanna.Terminal names second, the caller's
///          already-canonical spelling next, and any remaining canonical name
///          last. Unknown names pass through unchanged.
/// @param name Runtime name or compatibility alias requested by the lowerer.
/// @return Owned canonical spelling, or a copy of @p name when unregistered.
static std::string mapToCanonicalRuntime(std::string_view name) {
    using namespace il::runtime;

    const RuntimeDescriptor *desc = findRuntimeDescriptor(name);
    if (!desc)
        return std::string(name);

    // Identify a canonical descriptor in the alias group sharing the same
    // generated signature id. Prefer entries with a namespace ('.' in name).
    // Priority: Zanna.String.* > Zanna.Terminal.* > other Zanna.* namespaces.
    if (auto sigId = findRuntimeSignatureId(desc->name)) {
        const bool callerCanonical = name.find('.') != std::string_view::npos;
        const RuntimeDescriptor *callerDesc =
            (callerCanonical && desc->name == name) ? desc : nullptr;
        const RuntimeDescriptor *stringPreferred = nullptr;
        const RuntimeDescriptor *terminalPreferred = nullptr;
        const RuntimeDescriptor *firstCanonical = nullptr;
        const auto &reg = runtimeRegistry();
        for (const auto &entry : reg) {
            auto otherId = findRuntimeSignatureId(entry.name);
            if (!otherId || *otherId != *sigId)
                continue;
            const bool isCanonical = entry.name.find('.') != std::string_view::npos;
            if (!isCanonical)
                continue;
            if (!firstCanonical)
                firstCanonical = &entry;
            if (entry.name.rfind("Zanna.String.", 0) == 0) {
                stringPreferred = &entry;
                break; // strongest preference satisfied
            }
            if (entry.name.rfind("Zanna.Terminal.", 0) == 0) {
                terminalPreferred = &entry;
                continue; // keep looking for String.*
            }
        }
        if (stringPreferred)
            return std::string(stringPreferred->name);
        if (terminalPreferred)
            return std::string(terminalPreferred->name);
        if (callerDesc)
            return std::string(callerDesc->name);
        if (firstCanonical)
            return std::string(firstCanonical->name);
        return std::string(desc->name);
    }

    return std::string(desc->name);
}
} // namespace

/// @brief Emits a direct void call and tracks registered runtime callees.
/// @param callee Requested runtime alias or direct function name.
/// @param args Ordered IL argument values.
void Lowerer::emitCall(const std::string &callee, const std::vector<Value> &args) {
    const std::string name = mapToCanonicalRuntime(callee);
    // Track runtime callees so externs match call-site spellings.
    if (il::runtime::findRuntimeDescriptor(name))
        runtimeTracker.trackCalleeName(name);
    emitter().emitCall(name, args);
}

/// @brief Emits a direct value-returning call and tracks runtime callees.
/// @param ty Expected IL result type.
/// @param callee Requested runtime alias or direct function name.
/// @param args Ordered IL argument values.
/// @return SSA result of the call.
Value Lowerer::emitCallRet(Type ty, const std::string &callee, const std::vector<Value> &args) {
    const std::string name = mapToCanonicalRuntime(callee);
    if (il::runtime::findRuntimeDescriptor(name))
        runtimeTracker.trackCalleeName(name);
    return emitter().emitCallRet(ty, name, args);
}

/// @brief Request a runtime helper and emit a call in one operation.
/// @details Combines requestHelper() and emitCallRet() to reduce boilerplate when
///          calling runtime functions. This is especially useful in lowering visitor
///          methods where both operations are always performed together. For void
///          return types, emits a call without a result to maintain IL validity.
/// @param feature Runtime feature to request (ensures the helper is linked).
/// @param callee Name of the runtime function to call.
/// @param returnType Return type of the runtime function.
/// @param args Arguments to pass to the runtime function.
/// @return Value representing the result of the call, or a null value for void calls.
Value Lowerer::emitRuntimeHelper(il::runtime::RuntimeFeature feature,
                                 const std::string &callee,
                                 Type returnType,
                                 const std::vector<Value> &args) {
    requestHelper(feature);
    // Void-returning helpers must use emitCall (no result) to maintain IL validity.
    if (returnType.kind == Type::Kind::Void) {
        emitCall(callee, args);
        return Value::null();
    }
    return emitCallRet(returnType, callee, args);
}

/// @brief Emits a value-returning indirect call through a function pointer.
/// @param ty Expected IL result type.
/// @param callee Function-pointer value.
/// @param args Ordered IL argument values.
/// @return SSA result of the indirect call.
Value Lowerer::emitCallIndirectRet(Type ty, Value callee, const std::vector<Value> &args) {
    return emitter().emitCallIndirectRet(ty, callee, args);
}

/// @brief Emits a void indirect call through a function pointer.
/// @param callee Function-pointer value.
/// @param args Ordered IL argument values.
void Lowerer::emitCallIndirect(Value callee, const std::vector<Value> &args) {
    emitter().emitCallIndirect(callee, args);
}

/// @brief Emits a reference to a global string constant.
/// @param globalName IL global symbol containing the string data.
/// @return Pointer-valued reference to the global string.
Value Lowerer::emitConstStr(const std::string &globalName) {
    return emitter().emitConstStr(globalName);
}

/// @brief Interns string content and returns its stable global label.
/// @details On the first insertion, installs a callback that materializes each
///          newly interned string as a module global. Existing strings reuse
///          their previously assigned labels.
/// @param s String content to look up or intern.
/// @return Stable global label associated with @p s.
std::string Lowerer::getStringLabel(const std::string &s) {
    // Check if already interned in the StringTable
    std::string existing = stringTable_.lookup(s);
    if (!existing.empty())
        return existing;

    // Set up the emitter callback if not already configured
    if (!stringTable_.size()) {
        /// @brief Materializes a newly interned string as a module global.
        /// @param label Stable string-table label.
        /// @param content String bytes to emit.
        stringTable_.setEmitter([this](const std::string &label, const std::string &content) {
            builder->addGlobalStr(label, content);
        });
    }

    // Intern the string (this will call the emitter callback)
    return stringTable_.intern(s);
}

} // namespace il::frontends::basic
