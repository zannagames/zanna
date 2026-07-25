//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Expr_Match.cpp
/// @brief Pattern matching expression lowering for the Zia IL lowerer.
///
/// @details Pattern tests and bindings recursively cover literals, ranges,
///          tuples, constructors, alternatives, optionals, Results, structs,
///          and classes. Match expressions evaluate the scrutinee once,
///          isolate arm-local bindings, merge coerced results through shared
///          storage, and select an integer switch fast path when every tested
///          arm is compatible with direct dispatch.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"

namespace il::frontends::zia {

using namespace runtime;

//=============================================================================
// Pattern Matching Helpers
//=============================================================================

/// @brief Extract one element of a tuple scrutinee as a pattern value.
/// @param tuple The lowered tuple value (a pointer to inline storage).
/// @param index Zero-based element index.
/// @param elemType Semantic type of the element.
/// @return The element as a PatternValue. Aggregate elements (struct/fixed-array/tuple) are
///         returned by address; scalar elements are loaded, and string loads are retained.
Lowerer::PatternValue Lowerer::emitTupleElement(const PatternValue &tuple,
                                                size_t index,
                                                TypeRef elemType) {
    Type ilType = mapType(elemType);
    size_t offset = getTupleElementOffset(tuple.type, index);
    Value elemPtr = tuple.value;
    if (offset > 0) {
        elemPtr = emitGEP(tuple.value, static_cast<int64_t>(offset));
    }
    if (elemType &&
        (elemType->kind == TypeKindSem::Struct || elemType->kind == TypeKindSem::FixedArray ||
         elemType->kind == TypeKindSem::Tuple))
        return {elemPtr, elemType};
    Value elemVal = emitLoad(elemPtr, ilType);
    if (ilType.kind == Type::Kind::Str)
        emitCall(runtime::kStrRetainMaybe, {elemVal});
    return {elemVal, elemType};
}

/// @brief Emit the branch logic deciding whether @p scrutinee matches @p pattern.
/// @param pattern The arm pattern to test.
/// @param scrutinee The value (and its semantic type) being matched.
/// @param successBlock Block to branch to when the pattern matches.
/// @param failureBlock Block to branch to when it does not.
/// @details Recursive over the pattern tree. Wildcard/binding always succeed; literal and
///          expression patterns emit type-appropriate comparisons (string equals, FP/int
///          compare, bool extend-and-compare); tuple/constructor patterns chain per-element
///          tests; constructors special-case `Optional` (Some/None via null check), `Result`
///          (Ok/Err via runtime helpers) and struct/class field destructuring; OR patterns
///          chain alternatives so any match jumps to @p successBlock. This routine only tests
///          — it binds nothing (see emitPatternBindings()).
void Lowerer::emitPatternTest(const MatchArm::Pattern &pattern,
                              const PatternValue &scrutinee,
                              size_t successBlock,
                              size_t failureBlock) {
    switch (pattern.kind) {
        case MatchArm::Pattern::Kind::Wildcard:
        case MatchArm::Pattern::Kind::Binding:
            emitBr(successBlock);
            return;

        case MatchArm::Pattern::Kind::Literal: {
            if (!pattern.literal) {
                emitBr(failureBlock);
                return;
            }
            if (scrutinee.type && scrutinee.type->kind == TypeKindSem::Optional) {
                auto emitPtrCompare = [&](Opcode op) -> Value {
                    unsigned ptrSlotId = nextTempId();
                    il::core::Instr ptrSlotInstr;
                    ptrSlotInstr.result = ptrSlotId;
                    ptrSlotInstr.op = Opcode::Alloca;
                    ptrSlotInstr.type = Type(Type::Kind::Ptr);
                    ptrSlotInstr.operands = {
                        Value::constInt(static_cast<long long>(kMachineWordSize))};
                    ptrSlotInstr.loc = curLoc_;
                    blockMgr_.currentBlock()->instructions.push_back(ptrSlotInstr);
                    Value ptrSlot = Value::temp(ptrSlotId);

                    il::core::Instr storePtrInstr;
                    storePtrInstr.op = Opcode::Store;
                    storePtrInstr.type = Type(Type::Kind::Ptr);
                    storePtrInstr.operands = {ptrSlot, scrutinee.value};
                    storePtrInstr.loc = curLoc_;
                    blockMgr_.currentBlock()->instructions.push_back(storePtrInstr);

                    unsigned ptrAsI64Id = nextTempId();
                    il::core::Instr loadAsI64Instr;
                    loadAsI64Instr.result = ptrAsI64Id;
                    loadAsI64Instr.op = Opcode::Load;
                    loadAsI64Instr.type = Type(Type::Kind::I64);
                    loadAsI64Instr.operands = {ptrSlot};
                    loadAsI64Instr.loc = curLoc_;
                    blockMgr_.currentBlock()->instructions.push_back(loadAsI64Instr);
                    Value ptrAsI64 = Value::temp(ptrAsI64Id);

                    return emitBinary(op, Type(Type::Kind::I1), ptrAsI64, Value::constInt(0));
                };

                if (pattern.literal->kind == ExprKind::NullLiteral) {
                    Value isNull = emitPtrCompare(Opcode::ICmpEq);
                    emitCBr(isNull, successBlock, failureBlock);
                    return;
                }

                Value isNotNull = emitPtrCompare(Opcode::ICmpNe);
                size_t someBlock = createBlock("match_opt_lit");
                emitCBr(isNotNull, someBlock, failureBlock);
                setBlock(someBlock);

                TypeRef innerType = scrutinee.type->innerType();
                auto innerValue = emitOptionalUnwrap(scrutinee.value, innerType);
                PatternValue inner{innerValue.value, innerType};
                emitPatternTest(pattern, inner, successBlock, failureBlock);
                return;
            }
            auto litResult = lowerExpr(pattern.literal.get());
            Value cond;
            if (scrutinee.type && scrutinee.type->kind == TypeKindSem::String) {
                cond = emitCallRet(
                    Type(Type::Kind::I1), kStringEquals, {scrutinee.value, litResult.value});
            } else if (scrutinee.type && scrutinee.type->kind == TypeKindSem::Number) {
                cond = emitBinary(
                    Opcode::FCmpEQ, Type(Type::Kind::I1), scrutinee.value, litResult.value);
            } else if (scrutinee.type && scrutinee.type->kind == TypeKindSem::Boolean) {
                // Booleans are i1 — extend to i64 for ICmpEq comparison
                Value lhsExt = emitUnary(Opcode::Zext1, Type(Type::Kind::I64), scrutinee.value);
                Value rhsExt = emitUnary(Opcode::Zext1, Type(Type::Kind::I64), litResult.value);
                cond = emitBinary(Opcode::ICmpEq, Type(Type::Kind::I1), lhsExt, rhsExt);
            } else {
                cond = emitBinary(
                    Opcode::ICmpEq, Type(Type::Kind::I1), scrutinee.value, litResult.value);
            }
            emitCBr(cond, successBlock, failureBlock);
            return;
        }

        case MatchArm::Pattern::Kind::Expression: {
            if (!pattern.literal) {
                emitBr(failureBlock);
                return;
            }
            // Range pattern: test `scrutinee >= lo && scrutinee <(=) hi`, with
            // short-circuit so the upper-bound test only runs when the lower
            // bound holds.
            if (pattern.literal->kind == ExprKind::Range) {
                auto *range = static_cast<RangeExpr *>(pattern.literal.get());
                Value x = widenIntegralToI64(scrutinee.value, mapType(scrutinee.type));
                auto lo = lowerExpr(range->start.get());
                Value loW = widenIntegralToI64(lo.value, lo.type);
                Value geLo = emitBinary(Opcode::SCmpGE, Type(Type::Kind::I1), x, loW);
                size_t hiBlock = createBlock("match_range_hi");
                emitCBr(geLo, hiBlock, failureBlock);
                setBlock(hiBlock);
                auto hi = lowerExpr(range->end.get());
                Value hiW = widenIntegralToI64(hi.value, hi.type);
                Opcode hiOp = range->inclusive ? Opcode::SCmpLE : Opcode::SCmpLT;
                Value cmpHi = emitBinary(hiOp, Type(Type::Kind::I1), x, hiW);
                emitCBr(cmpHi, successBlock, failureBlock);
                return;
            }
            auto exprResult = lowerExpr(pattern.literal.get());
            Value cond = exprResult.value;
            if (exprResult.type.kind != Type::Kind::I1) {
                cond = emitBinary(
                    Opcode::ICmpNe, Type(Type::Kind::I1), exprResult.value, Value::constInt(0));
            }
            emitCBr(cond, successBlock, failureBlock);
            return;
        }

        case MatchArm::Pattern::Kind::Tuple: {
            if (!scrutinee.type || scrutinee.type->kind != TypeKindSem::Tuple) {
                emitBr(failureBlock);
                return;
            }

            const auto &elements = scrutinee.type->tupleElementTypes();
            if (elements.size() != pattern.subpatterns.size()) {
                emitBr(failureBlock);
                return;
            }

            for (size_t i = 0; i < elements.size(); ++i) {
                size_t nextBlock = (i + 1 < elements.size())
                                       ? createBlock("match_tuple_" + std::to_string(i))
                                       : successBlock;
                PatternValue elemValue = emitTupleElement(scrutinee, i, elements[i]);
                emitPatternTest(pattern.subpatterns[i], elemValue, nextBlock, failureBlock);
                if (i + 1 < elements.size()) {
                    setBlock(nextBlock);
                }
            }
            return;
        }

        case MatchArm::Pattern::Kind::Constructor: {
            if (scrutinee.type && scrutinee.type->kind == TypeKindSem::Optional) {
                auto emitPtrCompare = [&](Opcode op) -> Value {
                    unsigned ptrSlotId = nextTempId();
                    il::core::Instr ptrSlotInstr;
                    ptrSlotInstr.result = ptrSlotId;
                    ptrSlotInstr.op = Opcode::Alloca;
                    ptrSlotInstr.type = Type(Type::Kind::Ptr);
                    ptrSlotInstr.operands = {
                        Value::constInt(static_cast<long long>(kMachineWordSize))};
                    ptrSlotInstr.loc = curLoc_;
                    blockMgr_.currentBlock()->instructions.push_back(ptrSlotInstr);
                    Value ptrSlot = Value::temp(ptrSlotId);

                    il::core::Instr storePtrInstr;
                    storePtrInstr.op = Opcode::Store;
                    storePtrInstr.type = Type(Type::Kind::Ptr);
                    storePtrInstr.operands = {ptrSlot, scrutinee.value};
                    storePtrInstr.loc = curLoc_;
                    blockMgr_.currentBlock()->instructions.push_back(storePtrInstr);

                    unsigned ptrAsI64Id = nextTempId();
                    il::core::Instr loadAsI64Instr;
                    loadAsI64Instr.result = ptrAsI64Id;
                    loadAsI64Instr.op = Opcode::Load;
                    loadAsI64Instr.type = Type(Type::Kind::I64);
                    loadAsI64Instr.operands = {ptrSlot};
                    loadAsI64Instr.loc = curLoc_;
                    blockMgr_.currentBlock()->instructions.push_back(loadAsI64Instr);
                    Value ptrAsI64 = Value::temp(ptrAsI64Id);

                    return emitBinary(op, Type(Type::Kind::I1), ptrAsI64, Value::constInt(0));
                };

                if (pattern.binding == "None") {
                    Value isNull = emitPtrCompare(Opcode::ICmpEq);
                    emitCBr(isNull, successBlock, failureBlock);
                    return;
                }

                if (pattern.binding == "Some") {
                    if (pattern.subpatterns.empty()) {
                        emitBr(failureBlock);
                        return;
                    }
                    Value isNotNull = emitPtrCompare(Opcode::ICmpNe);
                    size_t someBlock = createBlock("match_some");
                    emitCBr(isNotNull, someBlock, failureBlock);
                    setBlock(someBlock);

                    TypeRef innerType = scrutinee.type->innerType();
                    auto innerValue = emitOptionalUnwrap(scrutinee.value, innerType);
                    PatternValue inner{innerValue.value, innerType};
                    emitPatternTest(pattern.subpatterns[0], inner, successBlock, failureBlock);
                    return;
                }

                emitBr(failureBlock);
                return;
            }

            if (!scrutinee.type) {
                emitBr(failureBlock);
                return;
            }

            if (scrutinee.type->kind == TypeKindSem::Result) {
                if (pattern.binding == "Ok") {
                    Value isOk =
                        emitCallRet(Type(Type::Kind::I1), kResultGetIsOk, {scrutinee.value});
                    if (pattern.subpatterns.empty()) {
                        emitCBr(isOk, successBlock, failureBlock);
                        return;
                    }
                    size_t okBlock = createBlock("match_result_ok");
                    emitCBr(isOk, okBlock, failureBlock);
                    setBlock(okBlock);
                    TypeRef successType = !scrutinee.type->typeArgs.empty()
                                              ? scrutinee.type->typeArgs[0]
                                              : types::unknown();
                    Type ilSuccessType = mapType(successType);
                    const char *callee = kResultUnwrap;
                    Type runtimeReturn = Type(Type::Kind::Ptr);
                    if (successType && successType->kind == TypeKindSem::String) {
                        callee = kResultUnwrapStr;
                        runtimeReturn = Type(Type::Kind::Str);
                    } else if (successType && (successType->kind == TypeKindSem::Integer ||
                                               successType->kind == TypeKindSem::Enum)) {
                        callee = kResultUnwrapI64;
                        runtimeReturn = Type(Type::Kind::I64);
                    } else if (successType && successType->kind == TypeKindSem::Number) {
                        callee = kResultUnwrapF64;
                        runtimeReturn = Type(Type::Kind::F64);
                    }
                    Value raw = emitCallRet(runtimeReturn, callee, {scrutinee.value});
                    PatternValue okValue{raw, successType};
                    if (runtimeReturn.kind != ilSuccessType.kind) {
                        auto unboxed = emitUnboxValue(raw, ilSuccessType, successType);
                        okValue.value = unboxed.value;
                    }
                    emitPatternTest(pattern.subpatterns[0], okValue, successBlock, failureBlock);
                    return;
                }
                if (pattern.binding == "Err") {
                    Value isErr =
                        emitCallRet(Type(Type::Kind::I1), kResultGetIsErr, {scrutinee.value});
                    if (pattern.subpatterns.empty()) {
                        emitCBr(isErr, successBlock, failureBlock);
                        return;
                    }
                    size_t errBlock = createBlock("match_result_err");
                    emitCBr(isErr, errBlock, failureBlock);
                    setBlock(errBlock);
                    Value err =
                        emitCallRet(Type(Type::Kind::Str), kResultUnwrapErrStr, {scrutinee.value});
                    PatternValue errValue{err, types::string()};
                    emitPatternTest(pattern.subpatterns[0], errValue, successBlock, failureBlock);
                    return;
                }
                emitBr(failureBlock);
                return;
            }

            const std::vector<FieldLayout> *fields = nullptr;
            if (scrutinee.type->kind == TypeKindSem::Struct) {
                const StructTypeInfo *valueInfo = getOrCreateStructTypeInfo(scrutinee.type->name);
                if (valueInfo)
                    fields = &valueInfo->fields;
            } else if (scrutinee.type->kind == TypeKindSem::Class) {
                const ClassTypeInfo *entityInfo = getOrCreateClassTypeInfo(scrutinee.type->name);
                if (entityInfo)
                    fields = &entityInfo->fields;
            }

            if (!fields || fields->size() != pattern.subpatterns.size()) {
                emitBr(failureBlock);
                return;
            }

            for (size_t i = 0; i < fields->size(); ++i) {
                const FieldLayout &field = (*fields)[i];
                PatternValue fieldValue{emitFieldLoad(&field, scrutinee.value), field.type};
                size_t nextBlock = (i + 1 < fields->size())
                                       ? createBlock("match_ctor_" + std::to_string(i))
                                       : successBlock;
                emitPatternTest(pattern.subpatterns[i], fieldValue, nextBlock, failureBlock);
                if (i + 1 < fields->size()) {
                    setBlock(nextBlock);
                }
            }
            return;
        }

        case MatchArm::Pattern::Kind::Or: {
            // OR pattern: chain test blocks — each subpattern's failure jumps to
            // the next subpattern's test. Any subpattern's success jumps to the
            // shared success block. Last subpattern's failure jumps to failureBlock.
            for (size_t i = 0; i < pattern.subpatterns.size(); ++i) {
                bool isLast = (i + 1 == pattern.subpatterns.size());
                size_t nextAltBlock =
                    isLast ? failureBlock : createBlock("match_or_" + std::to_string(i + 1));
                emitPatternTest(pattern.subpatterns[i], scrutinee, successBlock, nextAltBlock);
                if (!isLast) {
                    setBlock(nextAltBlock);
                }
            }
            return;
        }
    }
}

/// @brief Bind a pattern's variable names to the matched sub-values of @p scrutinee.
/// @param pattern The arm pattern (already known to match).
/// @param scrutinee The matched value and its semantic type.
/// @details Run after emitPatternTest() succeeds. Mirrors that routine's structure: a binding
///          defines a local; tuple/struct/class/constructor patterns recurse into element or
///          field sub-values (unwrapping `Optional`/`Result` payloads via the same runtime
///          helpers used during testing). OR patterns introduce no bindings (sema enforces).
void Lowerer::emitPatternBindings(const MatchArm::Pattern &pattern, const PatternValue &scrutinee) {
    switch (pattern.kind) {
        case MatchArm::Pattern::Kind::Binding:
            defineLocal(pattern.binding, scrutinee.value);
            if (scrutinee.type)
                localTypes_[pattern.binding] = scrutinee.type;
            return;

        case MatchArm::Pattern::Kind::Tuple: {
            if (!scrutinee.type || scrutinee.type->kind != TypeKindSem::Tuple)
                return;
            const auto &elements = scrutinee.type->tupleElementTypes();
            if (elements.size() != pattern.subpatterns.size())
                return;
            for (size_t i = 0; i < elements.size(); ++i) {
                PatternValue elemValue = emitTupleElement(scrutinee, i, elements[i]);
                emitPatternBindings(pattern.subpatterns[i], elemValue);
            }
            return;
        }

        case MatchArm::Pattern::Kind::Constructor: {
            if (scrutinee.type && scrutinee.type->kind == TypeKindSem::Optional) {
                if (pattern.binding != "Some" || pattern.subpatterns.empty())
                    return;
                TypeRef innerType = scrutinee.type->innerType();
                auto innerValue = emitOptionalUnwrap(scrutinee.value, innerType);
                PatternValue inner{innerValue.value, innerType};
                emitPatternBindings(pattern.subpatterns[0], inner);
                return;
            }

            if (!scrutinee.type)
                return;

            if (scrutinee.type->kind == TypeKindSem::Result) {
                if (pattern.binding == "Ok" && !pattern.subpatterns.empty()) {
                    TypeRef successType = !scrutinee.type->typeArgs.empty()
                                              ? scrutinee.type->typeArgs[0]
                                              : types::unknown();
                    Type ilSuccessType = mapType(successType);
                    const char *callee = kResultUnwrap;
                    Type runtimeReturn = Type(Type::Kind::Ptr);
                    if (successType && successType->kind == TypeKindSem::String) {
                        callee = kResultUnwrapStr;
                        runtimeReturn = Type(Type::Kind::Str);
                    } else if (successType && (successType->kind == TypeKindSem::Integer ||
                                               successType->kind == TypeKindSem::Enum)) {
                        callee = kResultUnwrapI64;
                        runtimeReturn = Type(Type::Kind::I64);
                    } else if (successType && successType->kind == TypeKindSem::Number) {
                        callee = kResultUnwrapF64;
                        runtimeReturn = Type(Type::Kind::F64);
                    }
                    Value raw = emitCallRet(runtimeReturn, callee, {scrutinee.value});
                    PatternValue okValue{raw, successType};
                    if (runtimeReturn.kind != ilSuccessType.kind) {
                        auto unboxed = emitUnboxValue(raw, ilSuccessType, successType);
                        okValue.value = unboxed.value;
                    }
                    emitPatternBindings(pattern.subpatterns[0], okValue);
                    return;
                }
                if (pattern.binding == "Err" && !pattern.subpatterns.empty()) {
                    Value err =
                        emitCallRet(Type(Type::Kind::Str), kResultUnwrapErrStr, {scrutinee.value});
                    PatternValue errValue{err, types::string()};
                    emitPatternBindings(pattern.subpatterns[0], errValue);
                    return;
                }
                return;
            }

            const std::vector<FieldLayout> *fields = nullptr;
            if (scrutinee.type->kind == TypeKindSem::Struct) {
                const StructTypeInfo *valueInfo = getOrCreateStructTypeInfo(scrutinee.type->name);
                if (valueInfo)
                    fields = &valueInfo->fields;
            } else if (scrutinee.type->kind == TypeKindSem::Class) {
                const ClassTypeInfo *entityInfo = getOrCreateClassTypeInfo(scrutinee.type->name);
                if (entityInfo)
                    fields = &entityInfo->fields;
            }

            if (!fields || fields->size() != pattern.subpatterns.size())
                return;

            for (size_t i = 0; i < fields->size(); ++i) {
                const FieldLayout &field = (*fields)[i];
                PatternValue fieldValue{emitFieldLoad(&field, scrutinee.value), field.type};
                emitPatternBindings(pattern.subpatterns[i], fieldValue);
            }
            return;
        }

        case MatchArm::Pattern::Kind::Or:
            // OR patterns don't introduce bindings (sema enforces this).
            return;

        default:
            return;
    }
}

//=============================================================================
// Match Expression Lowering
//=============================================================================

/// @brief Lower a `match` expression to IL.
/// @param expr The match expression with its scrutinee and arms.
/// @return The match result value and IL type (void placeholder for statement-style matches).
/// @details Lowers the scrutinee once into a reusable slot. For an integer scrutinee whose
///          arms are all integer literals (plus an optional trailing wildcard) it emits a
///          single O(1) SwitchI32; otherwise it emits a chain of pattern-test blocks, each
///          with an optional guard, that branch to per-arm body blocks. Each arm binds its
///          pattern, lowers its body, and stores the (coerced) result into a shared slot;
///          local/slot state is saved and restored around every arm. A trailing trap block
///          handles non-exhaustive fallthrough and is DCE'd away when an arm is irrefutable.
LowerResult Lowerer::lowerMatchExpr(MatchExpr *expr) {
    if (expr->arms.empty()) {
        return {Value::constInt(0), Type(Type::Kind::Void)};
    }

    // Lower the scrutinee and store it for reuse in pattern tests
    auto scrutinee = lowerExpr(expr->scrutinee.get());
    std::string scrutineeSlot = "__match_scrutinee";
    createSlot(scrutineeSlot, scrutinee.type);
    storeToSlot(scrutineeSlot, scrutinee.value, scrutinee.type);

    TypeRef scrutineeType = sema_.typeOf(expr->scrutinee.get());

    // Determine the result type from the first arm body
    TypeRef resultType = sema_.typeOf(expr);
    Type ilResultType = mapType(resultType);

    // Create a result slot to store the match result
    std::string resultSlot = "__match_result";
    bool hasResult = ilResultType.kind != Type::Kind::Void;
    if (hasResult)
        createSlot(resultSlot, ilResultType);

    auto coerceArmResult = [&](LowerResult bodyResult, Expr *bodyExpr) -> Value {
        if (!resultType || resultType->kind == TypeKindSem::Unknown ||
            resultType->kind == TypeKindSem::Error) {
            return bodyResult.value;
        }
        TypeRef bodyType = bodyExpr ? sema_.typeOf(bodyExpr) : types::unknown();
        auto coerced = coerceValueToType(bodyResult.value, bodyResult.type, bodyType, resultType);
        return coerced.value;
    };

    // ---- SwitchI32 fast path for integer-only match ----
    // When the scrutinee is Integer and every arm is either an integer literal
    // (without guard) or a wildcard/binding (the default), emit a single
    // SwitchI32 instruction instead of an O(N) if-else chain.
    if (scrutineeType && scrutineeType->kind == TypeKindSem::Integer) {
        bool canSwitch = true;
        size_t defaultArmIdx = SIZE_MAX;
        std::vector<std::pair<int64_t, size_t>> caseValues; // (value, arm index)

        for (size_t i = 0; i < expr->arms.size(); ++i) {
            const auto &arm = expr->arms[i];
            if (arm.pattern.guard) {
                canSwitch = false;
                break;
            }
            if (arm.pattern.kind == MatchArm::Pattern::Kind::Wildcard ||
                arm.pattern.kind == MatchArm::Pattern::Kind::Binding) {
                defaultArmIdx = i;
                // Only the last arm can be wildcard/binding for switch.
                if (i + 1 != expr->arms.size()) {
                    canSwitch = false;
                    break;
                }
            } else if (arm.pattern.kind == MatchArm::Pattern::Kind::Literal &&
                       arm.pattern.literal && arm.pattern.literal->kind == ExprKind::IntLiteral) {
                auto *lit = static_cast<IntLiteralExpr *>(arm.pattern.literal.get());
                caseValues.emplace_back(lit->value, i);
            } else {
                canSwitch = false;
                break;
            }
        }

        // Need at least 2 cases to justify a switch (1 case → just use the generic path).
        if (canSwitch && caseValues.size() >= 2) {
            size_t endIdx = createBlock("match_end");
            size_t nocaseIdx = createBlock("match_nocase");

            // Narrow I64 scrutinee to I32 for SwitchI32.
            Value scrutineeI32 =
                emitUnary(Opcode::CastUiNarrowChk, Type(Type::Kind::I32), scrutinee.value);

            // Create arm body blocks.
            std::vector<size_t> armBlocks;
            armBlocks.reserve(expr->arms.size());
            for (size_t i = 0; i < expr->arms.size(); ++i)
                armBlocks.push_back(createBlock("match_arm_" + std::to_string(i)));

            // Determine default target: either the wildcard arm or the trap block.
            size_t defaultTarget =
                (defaultArmIdx != SIZE_MAX) ? armBlocks[defaultArmIdx] : nocaseIdx;

            // Emit SwitchI32.
            {
                il::core::Instr sw;
                sw.op = Opcode::SwitchI32;
                sw.type = Type(Type::Kind::Void);
                sw.operands.push_back(scrutineeI32);

                // Default target.
                sw.addBranchTarget(currentFunc_->blocks[defaultTarget].label);

                // Cases.
                for (const auto &[val, armIdx] : caseValues) {
                    sw.operands.push_back(Value::constInt(static_cast<int64_t>(val)));
                    sw.addBranchTarget(currentFunc_->blocks[armBlocks[armIdx]].label);
                }

                sw.loc = curLoc_;
                blockMgr_.currentBlock()->instructions.push_back(std::move(sw));
                blockMgr_.currentBlock()->terminated = true;
            }

            // Emit each arm body block.
            for (size_t i = 0; i < expr->arms.size(); ++i) {
                const auto &arm = expr->arms[i];
                auto localsBackup = locals_;
                auto slotsBackup = slots_;
                auto localTypesBackup = localTypes_;

                setBlock(armBlocks[i]);

                // Emit bindings (wildcard/binding arms bind the scrutinee).
                Value scrutineeInArm = loadFromSlot(scrutineeSlot, scrutinee.type);
                PatternValue scrutineeValueInArm{scrutineeInArm, scrutineeType};
                emitPatternBindings(arm.pattern, scrutineeValueInArm);

                if (arm.body) {
                    auto bodyResult = lowerExpr(arm.body.get());
                    if (hasResult) {
                        Value bodyValue = coerceArmResult(bodyResult, arm.body.get());
                        storeToSlot(resultSlot, bodyValue, ilResultType);
                        consumeDeferred(bodyValue);
                    }
                }

                if (!isTerminated())
                    emitBr(endIdx);

                locals_ = std::move(localsBackup);
                slots_ = std::move(slotsBackup);
                localTypes_ = std::move(localTypesBackup);
            }

            // Trap block for non-exhaustive match.
            setBlock(nocaseIdx);
            {
                il::core::Instr trapInstr;
                trapInstr.op = Opcode::Trap;
                trapInstr.type = Type(Type::Kind::Void);
                trapInstr.loc = curLoc_;
                blockMgr_.currentBlock()->instructions.push_back(trapInstr);
            }

            // Continue from end block.
            setBlock(endIdx);
            removeSlot(scrutineeSlot);
            if (hasResult) {
                Value result = loadFromSlot(resultSlot, ilResultType);
                removeSlot(resultSlot);
                if (needsRelease(resultType))
                    deferRelease(result, isStringType(resultType));
                return {result, ilResultType};
            }
            return {Value::constInt(0), Type(Type::Kind::Void)};
        }
    }

    // Create end block for the match
    size_t endIdx = createBlock("match_end");

    // Create a trap block for non-exhaustive match fallthrough
    size_t nocaseIdx = createBlock("match_nocase");

    // Create blocks for each arm body and the next arm's test
    std::vector<size_t> armBlocks;
    std::vector<size_t> nextTestBlocks;
    for (size_t i = 0; i < expr->arms.size(); ++i) {
        armBlocks.push_back(createBlock("match_arm_" + std::to_string(i)));
        if (i + 1 < expr->arms.size()) {
            nextTestBlocks.push_back(createBlock("match_test_" + std::to_string(i + 1)));
        } else {
            nextTestBlocks.push_back(nocaseIdx); // Last arm falls through to trap
        }
    }

    // Lower each arm
    for (size_t i = 0; i < expr->arms.size(); ++i) {
        const auto &arm = expr->arms[i];
        auto localsBackup = locals_;
        auto slotsBackup = slots_;
        auto localTypesBackup = localTypes_;

        size_t matchBlock = armBlocks[i];
        size_t guardBlock = 0;
        if (arm.pattern.guard) {
            guardBlock = createBlock("match_guard_" + std::to_string(i));
            matchBlock = guardBlock;
        }

        // In the current block, test the pattern
        Value scrutineeVal = loadFromSlot(scrutineeSlot, scrutinee.type);
        PatternValue scrutineeValue{scrutineeVal, scrutineeType};
        emitPatternTest(arm.pattern, scrutineeValue, matchBlock, nextTestBlocks[i]);

        if (guardBlock) {
            setBlock(guardBlock);
            // Reload scrutinee from slot in this block for SSA correctness
            Value scrutineeInGuard = loadFromSlot(scrutineeSlot, scrutinee.type);
            PatternValue scrutineeValueInGuard{scrutineeInGuard, scrutineeType};
            emitPatternBindings(arm.pattern, scrutineeValueInGuard);
            auto guardResult = lowerExpr(arm.pattern.guard.get());
            emitCBr(guardResult.value, armBlocks[i], nextTestBlocks[i]);
        }

        // Lower the arm body and store result
        setBlock(armBlocks[i]);
        if (!guardBlock) {
            // Reload scrutinee from slot in this block for SSA correctness
            Value scrutineeInArm = loadFromSlot(scrutineeSlot, scrutinee.type);
            PatternValue scrutineeValueInArm{scrutineeInArm, scrutineeType};
            emitPatternBindings(arm.pattern, scrutineeValueInArm);
        }
        if (arm.body) {
            auto bodyResult = lowerExpr(arm.body.get());
            if (hasResult) {
                Value bodyValue = coerceArmResult(bodyResult, arm.body.get());
                storeToSlot(resultSlot, bodyValue, ilResultType);
                consumeDeferred(bodyValue);
            }
        }

        // Jump to end after arm body (if not already terminated)
        if (!isTerminated()) {
            emitBr(endIdx);
        }

        locals_ = std::move(localsBackup);
        slots_ = std::move(slotsBackup);
        localTypes_ = std::move(localTypesBackup);

        // Set up next test block for pattern matching
        if (i + 1 < expr->arms.size()) {
            setBlock(nextTestBlocks[i]);
        }
    }

    // Remove the scrutinee slot
    removeSlot(scrutineeSlot);

    // Emit a trap for non-exhaustive match fallthrough.
    // If all arms had a wildcard/irrefutable pattern, the last arm's failure
    // branch is unreachable and the trap will be eliminated by DCE.
    setBlock(nocaseIdx);
    {
        il::core::Instr trapInstr;
        trapInstr.op = Opcode::Trap;
        trapInstr.type = Type(Type::Kind::Void);
        trapInstr.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(trapInstr);
    }

    // Continue from end block
    setBlock(endIdx);

    // Load and return the result
    if (hasResult) {
        Value result = loadFromSlot(resultSlot, ilResultType);
        removeSlot(resultSlot);
        if (needsRelease(resultType))
            deferRelease(result, isStringType(resultType));
        return {result, ilResultType};
    }

    return {Value::constInt(0), Type(Type::Kind::Void)};
}

} // namespace il::frontends::zia
