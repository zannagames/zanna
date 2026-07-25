//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer_Expr_Binary.cpp
/// @brief Lowers Zia binary, unary, assignment, and short-circuit
///        expressions.
///
/// @details Assignment lowering selects identifier, field, index, global, and
///          property destinations while applying semantic coercions and
///          managed-value transfer rules. Binary operator selection is
///          delegated to BinaryOperatorLowerer. Short-circuit expressions
///          release edge-local managed temporaries before branching so no SSA
///          value escapes its defining control-flow path.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/LowererBinaryOperatorLowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"

namespace il::frontends::zia {

using namespace runtime;
using il::core::Opcode;
using il::core::Type;
using il::core::Value;

namespace {

/// @brief True if @p type is stored inline by value (struct, fixed array, or
///        tuple) rather than behind a heap pointer — affects copy/load lowering.
/// @param type Semantic type to classify.
/// @return True for struct, fixed-array, and tuple values.
bool isInlineAggregateType(TypeRef type) {
    return type && (type->kind == TypeKindSem::Struct || type->kind == TypeKindSem::FixedArray ||
                    type->kind == TypeKindSem::Tuple);
}

} // namespace

//=============================================================================
// Helper Functions
//=============================================================================

/// @brief Coerce a value into Optional storage when assigned to an Optional-typed target.
/// @param val The value being stored.
/// @param fieldType Declared type of the target field/variable.
/// @param valueType Static type of @p val.
/// @return @p val unchanged when the target is non-optional or already optional; a null
///         pointer for a `unit` source; otherwise the value wrapped via emitOptionalWrap().
Value Lowerer::wrapValueForOptionalField(Value val, TypeRef fieldType, TypeRef valueType) {
    if (!fieldType || fieldType->kind != TypeKindSem::Optional)
        return val;

    TypeRef innerType = fieldType->innerType();
    if (valueType && valueType->kind == TypeKindSem::Optional) {
        return val; // Already optional
    } else if (valueType && valueType->kind == TypeKindSem::Unit) {
        return Value::null();
    } else if (innerType) {
        return emitOptionalWrap(val, innerType);
    }
    return val;
}

/// @brief Widen an operand to I64 so heterogeneous values can be integer-compared.
/// @param val The operand value.
/// @param type The operand's IL type.
/// @return An I64 (or original) value: null becomes 0, `i1` is zero-extended, and pointer/
///         string values are reinterpreted to I64 via an alloca/store/load round-trip.
Value Lowerer::extendOperandForComparison(Value val, Type type) {
    if (val.kind == Value::Kind::NullPtr) {
        return Value::constInt(0);
    } else if (type.kind == Type::Kind::I1) {
        return emitUnary(Opcode::Zext1, Type(Type::Kind::I64), val);
    } else if (type.kind == Type::Kind::Ptr || type.kind == Type::Kind::Str) {
        // Convert pointer/string to i64 via alloca/store/load
        unsigned slotId = nextTempId();
        il::core::Instr slotInstr;
        slotInstr.result = slotId;
        slotInstr.op = Opcode::Alloca;
        slotInstr.type = Type(Type::Kind::Ptr);
        slotInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
        slotInstr.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(slotInstr);
        Value slot = Value::temp(slotId);
        emitStore(slot, val, type);
        return emitLoad(slot, Type(Type::Kind::I64));
    }
    return val;
}

//=============================================================================
// Binary Expression Lowering
//=============================================================================

/// @brief Lower an assignment expression by dispatching on the target's form.
/// @param expr Assignment binary expression.
/// @return The assigned value and its IL type.
/// @details Routes to lowerIdentAssignment(), lowerIndexAssignment(), or
///          lowerFieldAssignment() based on whether the left-hand side is an identifier,
///          an index, or a field access; unsupported targets are reported (V3000).
LowerResult Lowerer::lowerAssignment(BinaryExpr *expr) {
    auto right = lowerExpr(expr->right.get());
    TypeRef rightType = sema_.typeOf(expr->right.get());

    if (auto *ident = dynamic_cast<IdentExpr *>(expr->left.get()))
        return lowerIdentAssignment(expr, ident, right, rightType);
    if (auto *indexExpr = dynamic_cast<IndexExpr *>(expr->left.get()))
        return lowerIndexAssignment(expr, indexExpr, right, rightType);
    if (auto *fieldExpr = dynamic_cast<FieldExpr *>(expr->left.get()))
        return lowerFieldAssignment(expr, fieldExpr, right, rightType);

    diag_.report({il::support::Severity::Error,
                  "Unsupported assignment target reached lowering",
                  expr->loc,
                  "V3000"});
    return {Value::constInt(0), Type(Type::Kind::I64)};
}

/// @brief Lower assignment to a bare identifier target.
/// @param expr The assignment expression.
/// @param ident The identifier being assigned.
/// @param right The already-lowered right-hand value.
/// @param rightType Static type of the right-hand side.
/// @return The stored value and its IL type.
/// @details Applies optional-wrapping, Ptr→primitive unboxing, Number/Integer numeric
///          coercion, and struct-copy semantics, then stores into the first matching target:
///          a slot variable, an implicit `self.field` (struct/class method), a module global,
///          or a freshly defined local. Reassigning an SSA-only final is skipped defensively.
LowerResult Lowerer::lowerIdentAssignment(BinaryExpr *expr,
                                          IdentExpr *ident,
                                          LowerResult right,
                                          TypeRef rightType) {
    {
        TypeRef targetType = nullptr;
        auto typeIt = localTypes_.find(ident->name);
        if (typeIt != localTypes_.end())
            targetType = typeIt->second;
        else
            targetType = sema_.typeOf(expr->left.get());

        Value assignValue = wrapValueForOptionalField(right.value, targetType, rightType);
        Type assignType = (targetType && targetType->kind == TypeKindSem::Optional)
                              ? Type(Type::Kind::Ptr)
                              : right.type;

        // Unbox obj (Ptr) to primitive type when assigning a boxed value to a typed slot.
        // This handles e.g. `intField = list.Get(i)` where List.Get() returns Ptr.
        if (right.type.kind == Type::Kind::Ptr && targetType) {
            Type targetILType = mapType(targetType);
            if (targetILType.kind != Type::Kind::Ptr) {
                assignValue = emitUnbox(assignValue, targetILType).value;
                assignType = targetILType;
            }
        }

        // Numeric coercion: emit conversion instructions to avoid raw bit
        // reinterpretation when assigning between Number and Integer types.
        if (targetType && rightType) {
            if (targetType->kind == TypeKindSem::Integer &&
                rightType->kind == TypeKindSem::Number) {
                unsigned convId = nextTempId();
                il::core::Instr conv;
                conv.result = convId;
                conv.op = Opcode::CastFpToSiRteChk;
                conv.type = Type(Type::Kind::I64);
                conv.operands = {assignValue};
                conv.loc = curLoc_;
                blockMgr_.currentBlock()->instructions.push_back(conv);
                assignValue = Value::temp(convId);
                assignType = conv.type;
            } else if (targetType->kind == TypeKindSem::Number &&
                       rightType->kind == TypeKindSem::Integer) {
                unsigned convId = nextTempId();
                il::core::Instr conv;
                conv.result = convId;
                conv.op = Opcode::Sitofp;
                conv.type = Type(Type::Kind::F64);
                conv.operands = {assignValue};
                conv.loc = curLoc_;
                blockMgr_.currentBlock()->instructions.push_back(conv);
                assignValue = Value::temp(convId);
                assignType = conv.type;
            }
        }

        // Handle struct type copy semantics for ordinary value storage. Optional
        // struct storage is already heap-boxed by wrapValueForOptionalField();
        // copying after that would turn the boxed payload back into a stack
        // pointer and make globals/fields dangle.
        const bool targetIsOptional = targetType && targetType->kind == TypeKindSem::Optional;
        if (!targetIsOptional && rightType && isInlineAggregateType(rightType)) {
            Value copy = emitInlineValueAlloc(rightType);
            emitInlineValueCopy(rightType, copy, assignValue, true);
            assignValue = copy;
            assignType = Type(Type::Kind::Ptr);
        }

        // Check if this is a slot-based variable
        auto slotIt = slots_.find(ident->name);
        if (slotIt != slots_.end()) {
            if (isInlineAggregateType(targetType)) {
                Value destPtr = loadFromSlot(ident->name, Type(Type::Kind::Ptr));
                emitInlineValueCopy(targetType, destPtr, assignValue, true);
                consumeDeferred(assignValue);
                return {destPtr, Type(Type::Kind::Ptr)};
            }
            if (assignType.kind == Type::Kind::Str && isOwnedStringSlot(ident->name)) {
                // Slot-ownership discipline: owned temps move in, borrowed
                // values are retained, and the displaced occupant is released.
                storeOwnedStringToSlot(ident->name, assignValue, /*releaseDisplaced=*/true);
                return {assignValue, assignType};
            }
            if (assignType.kind == Type::Kind::Ptr && isOwnedObjectSlot(ident->name)) {
                storeOwnedObjectToSlot(ident->name, assignValue, /*releaseDisplaced=*/true);
                return {assignValue, assignType};
            }
            storeToSlot(ident->name, assignValue, assignType);
            // The assigned value is consumed by the slot — don't release
            consumeDeferred(assignValue);
            return {assignValue, assignType};
        }

        // Check for implicit field assignment inside a struct type method
        if (currentStructType_) {
            const FieldLayout *field = currentStructType_->findField(ident->name);
            if (field) {
                Value selfPtr;
                if (getSelfPtr(selfPtr)) {
                    Value fieldValue =
                        wrapValueForOptionalField(right.value, field->type, rightType);
                    // Unbox obj (Ptr) to the field's primitive IL type.
                    if (right.type.kind == Type::Kind::Ptr && field->type) {
                        Type fieldILType = mapType(field->type);
                        if (fieldILType.kind != Type::Kind::Ptr)
                            fieldValue = emitUnbox(fieldValue, fieldILType).value;
                    }
                    // Numeric coercion for field assignment
                    if (field->type && rightType) {
                        if (field->type->kind == TypeKindSem::Integer &&
                            rightType->kind == TypeKindSem::Number) {
                            unsigned convId = nextTempId();
                            il::core::Instr conv;
                            conv.result = convId;
                            conv.op = Opcode::CastFpToSiRteChk;
                            conv.type = Type(Type::Kind::I64);
                            conv.operands = {fieldValue};
                            conv.loc = curLoc_;
                            blockMgr_.currentBlock()->instructions.push_back(conv);
                            fieldValue = Value::temp(convId);
                        } else if (field->type->kind == TypeKindSem::Number &&
                                   rightType->kind == TypeKindSem::Integer) {
                            unsigned convId = nextTempId();
                            il::core::Instr conv;
                            conv.result = convId;
                            conv.op = Opcode::Sitofp;
                            conv.type = Type(Type::Kind::F64);
                            conv.operands = {fieldValue};
                            conv.loc = curLoc_;
                            blockMgr_.currentBlock()->instructions.push_back(conv);
                            fieldValue = Value::temp(convId);
                        }
                    }
                    if (field->type && field->type->kind == TypeKindSem::Struct)
                        fieldValue = emitBoxValue(fieldValue, mapType(field->type), field->type);
                    emitFieldStore(field, selfPtr, fieldValue);
                    consumeDeferred(fieldValue);
                    return {fieldValue, mapType(field->type)};
                }
            }
        }

        // Check for implicit field assignment inside an class method
        if (currentClassType_) {
            const FieldLayout *field = currentClassType_->findField(ident->name);
            if (field) {
                Value selfPtr;
                if (getSelfPtr(selfPtr)) {
                    Value fieldValue =
                        wrapValueForOptionalField(right.value, field->type, rightType);
                    // Unbox obj (Ptr) to the field's primitive IL type.
                    if (right.type.kind == Type::Kind::Ptr && field->type) {
                        Type fieldILType = mapType(field->type);
                        if (fieldILType.kind != Type::Kind::Ptr)
                            fieldValue = emitUnbox(fieldValue, fieldILType).value;
                    }
                    // Numeric coercion for field assignment
                    if (field->type && rightType) {
                        if (field->type->kind == TypeKindSem::Integer &&
                            rightType->kind == TypeKindSem::Number) {
                            unsigned convId = nextTempId();
                            il::core::Instr conv;
                            conv.result = convId;
                            conv.op = Opcode::CastFpToSiRteChk;
                            conv.type = Type(Type::Kind::I64);
                            conv.operands = {fieldValue};
                            conv.loc = curLoc_;
                            blockMgr_.currentBlock()->instructions.push_back(conv);
                            fieldValue = Value::temp(convId);
                        } else if (field->type->kind == TypeKindSem::Number &&
                                   rightType->kind == TypeKindSem::Integer) {
                            unsigned convId = nextTempId();
                            il::core::Instr conv;
                            conv.result = convId;
                            conv.op = Opcode::Sitofp;
                            conv.type = Type(Type::Kind::F64);
                            conv.operands = {fieldValue};
                            conv.loc = curLoc_;
                            blockMgr_.currentBlock()->instructions.push_back(conv);
                            fieldValue = Value::temp(convId);
                        }
                    }
                    if (field->type && field->type->kind == TypeKindSem::Struct)
                        fieldValue = emitBoxValue(fieldValue, mapType(field->type), field->type);
                    emitFieldStore(field, selfPtr, fieldValue);
                    consumeDeferred(fieldValue);
                    return {fieldValue, mapType(field->type)};
                }
            }
        }

        // Check for global variable assignment
        std::string resolvedName = sema_.resolvedIdentifierName(ident);
        if (resolvedName.empty())
            resolvedName = ident->name;
        auto globalIt = globalVariables_.find(resolvedName);
        if (globalIt != globalVariables_.end()) {
            TypeRef globalType = globalIt->second;
            Type ilType = mapType(globalType);
            Value addr = getGlobalVarAddr(resolvedName, globalType);
            Value storeValue = assignValue;
            if (globalType && globalType->kind == TypeKindSem::Struct) {
                storeValue = emitBoxValue(right.value, right.type, globalType);
            }
            emitStore(addr, storeValue, ilType);
            consumeDeferred(storeValue);
            return {storeValue, ilType};
        }

        // Regular variable assignment.
        // Safety net: if a local already exists here (SSA-only, no slot),
        // it's a final variable being reassigned — Sema should have caught
        // this. Skip the overwrite to avoid silently corrupting the value.
        if (lookupLocal(ident->name) != nullptr) {
            return right;
        }
        defineLocal(ident->name, assignValue);
        if (targetType)
            localTypes_[ident->name] = targetType;
        return {assignValue, assignType};
    }
}

/// @brief Lower assignment to an indexed target (`base[index] = value`).
/// @param expr The assignment expression.
/// @param indexExpr The index target.
/// @param right The already-lowered right-hand value.
/// @param rightType Static type of the right-hand side (unused; type taken from sema).
/// @return The assigned value.
/// @details Fixed-size arrays store inline via a bounds-checked GEP + element store (no
///          boxing). List/Map targets box the value and call the appropriate runtime set
///          helper; `Map[Integer, T]` uses IntMap with widened i64 keys.
LowerResult Lowerer::lowerIndexAssignment(BinaryExpr *expr,
                                          IndexExpr *indexExpr,
                                          LowerResult right,
                                          TypeRef rightType) {
    (void)rightType;
    {
        auto base = lowerExpr(indexExpr->base.get());
        auto index = lowerExpr(indexExpr->index.get());
        TypeRef baseType = sema_.typeOf(indexExpr->base.get());
        TypeRef indexRightType = sema_.typeOf(expr->right.get());

        // Fixed-size array: direct GEP + Store (no boxing, no runtime call)
        if (baseType && baseType->kind == TypeKindSem::FixedArray) {
            TypeRef elemType = baseType->elementType();
            size_t elemSize = getSemanticTypeSize(elemType);
            Value checkedIndex =
                emitIndexCheck(widenIntegralToI64(index.value, index.type),
                               Value::constInt(0),
                               Value::constInt(static_cast<int64_t>(baseType->elementCount)));

            // Compute byte offset: index * elemSize
            unsigned mulId = nextTempId();
            il::core::Instr mulInstr;
            mulInstr.result = mulId;
            mulInstr.op = Opcode::IMulOvf;
            mulInstr.type = Type(Type::Kind::I64);
            mulInstr.operands = {checkedIndex, Value::constInt(static_cast<int64_t>(elemSize))};
            mulInstr.loc = curLoc_;
            blockMgr_.currentBlock()->instructions.push_back(mulInstr);
            Value byteOffset = Value::temp(mulId);

            // GEP to element address
            unsigned gepId = nextTempId();
            il::core::Instr gepInstr;
            gepInstr.result = gepId;
            gepInstr.op = Opcode::GEP;
            gepInstr.type = Type(Type::Kind::Ptr);
            gepInstr.operands = {base.value, byteOffset};
            gepInstr.loc = curLoc_;
            blockMgr_.currentBlock()->instructions.push_back(gepInstr);
            Value elemAddr = Value::temp(gepId);

            auto coerced = coerceValueToType(right.value, right.type, indexRightType, elemType);

            // Store the element value
            emitInlineValueStore(elemType, elemAddr, coerced.value, true);
            consumeDeferred(coerced.value);
            return {coerced.value, coerced.type};
        }

        Value boxedValue = emitBoxValue(right.value, right.type, indexRightType);
        Value indexValue = widenIntegralToI64(index.value, index.type);
        if (baseType && baseType->kind == TypeKindSem::Map) {
            const bool integerKeyed = usesIntegerMapRuntime(baseType);
            Value runtimeKey = coerceMapKeyForRuntime(index.value, index.type, baseType);
            emitCall(integerKeyed ? kIntMapSet : kMapSet, {base.value, runtimeKey, boxedValue});
        } else if (baseType && baseType->kind == TypeKindSem::List)
            emitCall(kListSet, {base.value, indexValue, boxedValue});
        return right;
    }
}

/// @brief Lower assignment to a field target (`base.field = value`).
/// @param expr The assignment expression.
/// @param fieldExpr The field target.
/// @param right The already-lowered right-hand value.
/// @param rightType Static type of the right-hand side.
/// @return The assigned value and its IL type.
/// @details Resolves the target in order: a module-qualified global, a synthesized property
///          setter (runtime or user-defined), or a struct/class instance field. Applies
///          optional-wrapping, Ptr→primitive unboxing, struct boxing, and Number/Integer
///          coercion as needed. Unsupported targets are reported (V3000).
LowerResult Lowerer::lowerFieldAssignment(BinaryExpr *expr,
                                          FieldExpr *fieldExpr,
                                          LowerResult right,
                                          TypeRef rightType) {
    (void)rightType;
    {
        TypeRef baseType = sema_.typeOf(fieldExpr->base.get());
        TypeRef targetType = sema_.typeOf(fieldExpr);

        if (baseType && baseType->kind == TypeKindSem::Module) {
            std::string resolvedName = sema_.resolvedFieldSymbolName(fieldExpr);
            std::string globalName =
                resolvedName.empty() ? baseType->name + "." + fieldExpr->field : resolvedName;
            auto globalIt = globalVariables_.find(globalName);
            if (globalIt != globalVariables_.end()) {
                TypeRef globalType = globalIt->second;
                Type ilType = mapType(globalType);
                Value addr = getGlobalVarAddr(globalName, globalType);
                Value storeValue = wrapValueForOptionalField(right.value, globalType, rightType);
                if (globalType && globalType->kind == TypeKindSem::Struct) {
                    storeValue = emitBoxValue(right.value, right.type, globalType);
                }
                if (right.type.kind == Type::Kind::Ptr && globalType) {
                    Type globalILType = mapType(globalType);
                    if (globalILType.kind != Type::Kind::Ptr)
                        storeValue = emitUnbox(storeValue, globalILType).value;
                }
                if (globalType && rightType && globalType->kind == TypeKindSem::Number &&
                    rightType->kind == TypeKindSem::Integer) {
                    unsigned convId = nextTempId();
                    il::core::Instr conv;
                    conv.result = convId;
                    conv.op = Opcode::Sitofp;
                    conv.type = Type(Type::Kind::F64);
                    conv.operands = {storeValue};
                    conv.loc = curLoc_;
                    blockMgr_.currentBlock()->instructions.push_back(conv);
                    storeValue = Value::temp(convId);
                }
                emitStore(addr, storeValue, ilType);
                consumeDeferred(storeValue);
                return {storeValue, ilType};
            }
        }

        auto base = lowerExpr(fieldExpr->base.get());

        std::string setterName = sema_.resolvedFieldSetter(fieldExpr);
        if (!setterName.empty()) {
            Value setterValue = wrapValueForOptionalField(right.value, targetType, rightType);

            if (right.type.kind == Type::Kind::Ptr && targetType) {
                Type targetILType = mapType(targetType);
                if (targetILType.kind != Type::Kind::Ptr)
                    setterValue = emitUnbox(setterValue, targetILType).value;
            }

            if (targetType && rightType) {
                if (targetType->kind == TypeKindSem::Integer &&
                    rightType->kind == TypeKindSem::Number) {
                    unsigned convId = nextTempId();
                    il::core::Instr conv;
                    conv.result = convId;
                    conv.op = Opcode::CastFpToSiRteChk;
                    conv.type = Type(Type::Kind::I64);
                    conv.operands = {setterValue};
                    conv.loc = curLoc_;
                    blockMgr_.currentBlock()->instructions.push_back(conv);
                    setterValue = Value::temp(convId);
                } else if (targetType->kind == TypeKindSem::Number &&
                           rightType->kind == TypeKindSem::Integer) {
                    unsigned convId = nextTempId();
                    il::core::Instr conv;
                    conv.result = convId;
                    conv.op = Opcode::Sitofp;
                    conv.type = Type(Type::Kind::F64);
                    conv.operands = {setterValue};
                    conv.loc = curLoc_;
                    blockMgr_.currentBlock()->instructions.push_back(conv);
                    setterValue = Value::temp(convId);
                }
            }

            TypeRef resolvedBaseType = sema_.typeOf(fieldExpr->base.get());
            if (resolvedBaseType && resolvedBaseType->kind == TypeKindSem::Module)
                emitCall(setterName, {setterValue});
            else
                emitCall(setterName, {base.value, setterValue});
            consumeDeferred(setterValue);
            return {setterValue, mapType(targetType)};
        }

        // Unwrap Optional types for field assignment
        // This handles variables assigned from optionals after null checks
        // (e.g., `var row = maybeRow;` where maybeRow is Row?)
        if (baseType && baseType->kind == TypeKindSem::Optional && baseType->innerType()) {
            baseType = baseType->innerType();
        }

        if (baseType) {
            std::string typeName = baseType->name;

            // Check struct types
            const StructTypeInfo *valueInfo = getOrCreateStructTypeInfo(typeName);
            if (valueInfo) {
                const FieldLayout *field = valueInfo->findField(fieldExpr->field);
                if (field) {
                    Value fieldValue =
                        wrapValueForOptionalField(right.value, field->type, rightType);
                    // Unbox obj (Ptr) to the field's primitive IL type.
                    if (right.type.kind == Type::Kind::Ptr && field->type) {
                        Type fieldILType = mapType(field->type);
                        if (fieldILType.kind != Type::Kind::Ptr)
                            fieldValue = emitUnbox(fieldValue, fieldILType).value;
                    }
                    if (field->type && field->type->kind == TypeKindSem::Struct)
                        fieldValue = emitBoxValue(fieldValue, mapType(field->type), field->type);
                    emitFieldStore(field, base.value, fieldValue);
                    consumeDeferred(fieldValue);
                    return {fieldValue, mapType(field->type)};
                }
            }

            // Check class types
            const ClassTypeInfo *entityInfoPtr = getOrCreateClassTypeInfo(typeName);
            if (entityInfoPtr) {
                const FieldLayout *field = entityInfoPtr->findField(fieldExpr->field);
                if (field) {
                    Value fieldValue =
                        wrapValueForOptionalField(right.value, field->type, rightType);
                    // Unbox obj (Ptr) to the field's primitive IL type.
                    if (right.type.kind == Type::Kind::Ptr && field->type) {
                        Type fieldILType = mapType(field->type);
                        if (fieldILType.kind != Type::Kind::Ptr)
                            fieldValue = emitUnbox(fieldValue, fieldILType).value;
                    }
                    if (field->type && field->type->kind == TypeKindSem::Struct)
                        fieldValue = emitBoxValue(fieldValue, mapType(field->type), field->type);
                    emitFieldStore(field, base.value, fieldValue);
                    consumeDeferred(fieldValue);
                    return {fieldValue, mapType(field->type)};
                }
            }
        }
    }

    diag_.report({il::support::Severity::Error,
                  "Unsupported assignment target reached lowering",
                  expr->loc,
                  "V3000"});
    return {Value::constInt(0), Type(Type::Kind::I64)};
}

/// @brief Lower a binary expression.
/// @param expr Binary expression.
/// @return The result value and its IL type.
/// @details Thin entry point that delegates to BinaryOperatorLowerer, which handles operator
///          selection, numeric promotion, string/comparison helpers, and short-circuiting.
LowerResult Lowerer::lowerBinary(BinaryExpr *expr) {
    return BinaryOperatorLowerer(*this).lowerBinary(expr);
}

//=============================================================================
// Unary Expression Lowering
//=============================================================================

/// @brief Lower a unary expression.
/// @param expr Unary expression.
/// @return The result value and its IL type.
/// @details `&`/address-of yields a function's global address (without lowering the operand,
///          so forward-declared callbacks work). Otherwise the operand is lowered and the
///          op applied: `Neg` (FP `0 - x` or checked integer `0 - x`), `Not` (compare-equal
///          zero), and `BitNot` (`x ^ -1`).
LowerResult Lowerer::lowerUnary(UnaryExpr *expr) {
    if (expr->op == UnaryOp::AddressOf) {
        // Address-of is a symbol reference, not a value load.  Do not lower the
        // operand first: callbacks may legally refer to functions declared later.
        auto *ident = dynamic_cast<IdentExpr *>(expr->operand.get());
        if (!ident) {
            diag_.report({il::support::Severity::Error,
                          "Unsupported function reference operand reached lowering",
                          expr->loc,
                          "V3000"});
            return {Value::constInt(0), Type(Type::Kind::Ptr)};
        }

        std::string resolvedName = sema_.resolvedIdentifierName(ident);
        if (resolvedName.empty())
            resolvedName = ident->name;

        std::string mangledName;
        if (FunctionDecl *decl = sema_.getFunctionDecl(resolvedName))
            mangledName = sema_.loweredFunctionName(decl);
        if (mangledName.empty())
            mangledName = mangleFunctionName(resolvedName);
        return {Value::global(mangledName), Type(Type::Kind::Ptr)};
    }

    auto operand = lowerExpr(expr->operand.get());
    TypeRef operandType = sema_.typeOf(expr->operand.get());
    bool isFloat = operandType && operandType->kind == TypeKindSem::Number;

    switch (expr->op) {
        case UnaryOp::Neg: {
            if (isFloat) {
                Value result =
                    emitBinary(Opcode::FSub, operand.type, Value::constFloat(0.0), operand.value);
                return {result, operand.type};
            } else {
                Value result =
                    emitBinary(Opcode::ISubOvf, operand.type, Value::constInt(0), operand.value);
                return {result, operand.type};
            }
        }

        case UnaryOp::Not: {
            Value opVal = operand.value;
            if (operand.type.kind == Type::Kind::I1)
                opVal = emitUnary(Opcode::Zext1, Type(Type::Kind::I64), operand.value);
            Value result =
                emitBinary(Opcode::ICmpEq, Type(Type::Kind::I1), opVal, Value::constInt(0));
            return {result, Type(Type::Kind::I1)};
        }

        case UnaryOp::BitNot: {
            Value result =
                emitBinary(Opcode::Xor, operand.type, operand.value, Value::constInt(-1));
            return {result, operand.type};
        }

        case UnaryOp::AddressOf:
            break;
    }

    return operand;
}

//=============================================================================
// Short-Circuit Evaluation for And/Or
//=============================================================================

/// @brief Lower short-circuiting `and`/`or` to control flow.
/// @param expr Logical binary expression (`And` or `Or`).
/// @return An `i1` result value.
/// @details Stores the left operand's truthiness into a result slot, then conditionally
///          branches: `and` evaluates the right side only when the left is true, `or` only
///          when the left is false. The right side overwrites the slot, and the merge block
///          loads the final boolean. This guarantees the right operand is not evaluated when
///          the result is already determined.
LowerResult Lowerer::lowerShortCircuit(BinaryExpr *expr) {
    // Short-circuit evaluation for 'and' and 'or' operators.
    //
    // For 'A and B':
    //   - If A is false, result is false (don't evaluate B)
    //   - If A is true, result is B
    //
    // For 'A or B':
    //   - If A is true, result is true (don't evaluate B)
    //   - If A is false, result is B

    bool isAnd = (expr->op == BinaryOp::And);

    // Create basic blocks for control flow
    size_t evalRightIdx = createBlock(isAnd ? "and_rhs" : "or_rhs");
    size_t mergeIdx = createBlock(isAnd ? "and_merge" : "or_merge");

    // Allocate result slot
    unsigned slotId = nextTempId();
    il::core::Instr allocInstr;
    allocInstr.result = slotId;
    allocInstr.op = Opcode::Alloca;
    allocInstr.type = Type(Type::Kind::Ptr);
    allocInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
    allocInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(allocInstr);
    Value resultSlot = Value::temp(slotId);

    // Evaluate the left operand. Any owned values used only to compute its
    // truthiness must be released before this block branches; they cannot be
    // named legally from the merge block at the statement boundary.
    const size_t leftReleaseMark = deferredTemps_.size();
    auto left = lowerExpr(expr->left.get());

    // Extend to i64 for comparison if needed
    Value leftExt = (left.type.kind == Type::Kind::I1)
                        ? emitUnary(Opcode::Zext1, Type(Type::Kind::I64), left.value)
                        : left.value;

    // Convert to bool (non-zero = true)
    Value leftBool = emitBinary(Opcode::ICmpNe, Type(Type::Kind::I1), leftExt, Value::constInt(0));

    // Store left result as i1 in slot
    emitStore(resultSlot, leftBool, Type(Type::Kind::I1));

    releaseDeferredTempsFrom(leftReleaseMark);

    // Branch based on left value
    // For 'and': if left is true, evaluate right; else short-circuit to merge
    // For 'or': if left is false, evaluate right; else short-circuit to merge
    if (isAnd) {
        emitCBr(leftBool, evalRightIdx, mergeIdx);
    } else {
        emitCBr(leftBool, mergeIdx, evalRightIdx);
    }

    // Evaluate right operand block
    setBlock(evalRightIdx);
    const size_t rightReleaseMark = deferredTemps_.size();
    auto right = lowerExpr(expr->right.get());

    // Extend to i64 for comparison if needed
    Value rightExt = (right.type.kind == Type::Kind::I1)
                         ? emitUnary(Opcode::Zext1, Type(Type::Kind::I64), right.value)
                         : right.value;

    // Convert to bool
    Value rightBool =
        emitBinary(Opcode::ICmpNe, Type(Type::Kind::I1), rightExt, Value::constInt(0));

    // Store right result in slot
    emitStore(resultSlot, rightBool, Type(Type::Kind::I1));

    releaseDeferredTempsFrom(rightReleaseMark);

    // Branch to merge
    emitBr(mergeIdx);

    // Merge block - load result from slot
    setBlock(mergeIdx);
    Value result = emitLoad(resultSlot, Type(Type::Kind::I1));

    return {result, Type(Type::Kind::I1)};
}

} // namespace il::frontends::zia
