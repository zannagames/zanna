//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/link/InteropThunks.cpp
// Purpose: Implements boolean thunk generation for cross-language interop.
// Key invariants:
//   - i1→i64: uses Zext1 (zero-extend, so true=1).
//   - i64→i1: uses ICmpNe vs 0 (any non-zero → true).
// Ownership/Lifetime: Generated functions are returned by value.
// Links: docs/adr/0003-il-linkage-and-module-linking.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements link-time boolean representation adapter generation.
/// @details Generated wrappers expose an import-side signature while calling an
///          export-side implementation, inserting explicit I1/I64 conversions
///          around parameters and return values without changing either module's
///          original ABI declaration.

#include "il/link/InteropThunks.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Extern.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Linkage.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Param.hpp"
#include "il/core/Value.hpp"

#include <string>
#include <vector>

namespace il::link {

using il::core::BasicBlock;
using il::core::Function;
using il::core::Instr;
using il::core::Linkage;
using il::core::Module;
using il::core::Opcode;
using il::core::Param;
using il::core::Type;
using il::core::Value;

namespace {

/// @brief Check if two types differ only in boolean representation (i1 vs i64).
/// @param a First type kind.
/// @param b Second type kind.
/// @return True exactly when one kind is I1 and the other is I64.
bool isBooleanMismatch(Type::Kind a, Type::Kind b) {
    return (a == Type::Kind::I1 && b == Type::Kind::I64) ||
           (a == Type::Kind::I64 && b == Type::Kind::I1);
}

/// @brief Find an Export function by name in a module.
/// @param mod Module whose functions are searched.
/// @param name Exact exported symbol name to locate.
/// @return Borrowed pointer to the matching export, or null when none exists.
const Function *findExport(const Module &mod, const std::string &name) {
    for (const auto &fn : mod.functions) {
        if (fn.name == name && fn.linkage == Linkage::Export)
            return &fn;
    }
    return nullptr;
}

/// @brief Generate a thunk that converts between boolean representations.
///
/// @details The thunk has the import's signature (what the caller expects) and
///          calls the export with converted arguments, converting the return value.
///
/// @param importDecl The Import declaration (caller's expected signature).
/// @param exportDef The Export definition (actual function signature).
/// @param thunkName Name for the generated thunk function.
/// @return Internal-linkage wrapper with fresh SSA temporaries and one entry
///         block that performs the necessary conversions.
Function generateThunk(const Function &importDecl,
                       const Function &exportDef,
                       const std::string &thunkName) {
    Function thunk;
    thunk.name = thunkName;
    thunk.retType = importDecl.retType; // Match what the caller expects.
    thunk.linkage = Linkage::Internal;
    thunk.callingConv = importDecl.callingConv;

    // Build parameter list matching the import declaration.
    unsigned nextTemp = 0;
    for (size_t i = 0; i < importDecl.params.size(); ++i) {
        Param p;
        p.name = "p" + std::to_string(i);
        p.type = importDecl.params[i].type;
        p.id = nextTemp++;
        thunk.params.push_back(std::move(p));
    }

    BasicBlock entry;
    entry.label = "entry";

    // Build call arguments, converting booleans as needed.
    std::vector<Value> callArgs;
    for (size_t i = 0; i < importDecl.params.size(); ++i) {
        Type::Kind importKind = importDecl.params[i].type.kind;
        Type::Kind exportKind = exportDef.params[i].type.kind;

        if (importKind == Type::Kind::I64 && exportKind == Type::Kind::I1) {
            // Caller passes i64, callee expects i1: insert ICmpNe vs 0.
            // %conv = icmp_ne i64 %param, 0
            Instr cmp;
            cmp.op = Opcode::ICmpNe;
            cmp.type = Type(Type::Kind::I1);
            cmp.result = nextTemp;
            cmp.operands.push_back(Value::temp(thunk.params[i].id));
            cmp.operands.push_back(Value::constInt(0));
            entry.instructions.push_back(std::move(cmp));
            callArgs.push_back(Value::temp(nextTemp));
            nextTemp++;
        } else if (importKind == Type::Kind::I1 && exportKind == Type::Kind::I64) {
            // Caller passes i1, callee expects i64: insert Zext1.
            // %conv = zext1 i1 %param
            Instr zext;
            zext.op = Opcode::Zext1;
            zext.type = Type(Type::Kind::I64);
            zext.result = nextTemp;
            zext.operands.push_back(Value::temp(thunk.params[i].id));
            entry.instructions.push_back(std::move(zext));
            callArgs.push_back(Value::temp(nextTemp));
            nextTemp++;
        } else {
            // No conversion needed — pass through.
            callArgs.push_back(Value::temp(thunk.params[i].id));
        }
    }

    // Emit the call to the real function.
    bool needsRetConv = isBooleanMismatch(importDecl.retType.kind, exportDef.retType.kind);

    Instr call;
    call.op = Opcode::Call;
    call.setDirectCallee(exportDef.name);
    call.type = exportDef.retType; // Call with the export's return type.
    call.operands = std::move(callArgs);

    if (exportDef.retType.kind != Type::Kind::Void) {
        call.result = nextTemp;
        entry.instructions.push_back(std::move(call));
        unsigned callResult = nextTemp;
        nextTemp++;

        if (needsRetConv) {
            if (exportDef.retType.kind == Type::Kind::I1 &&
                importDecl.retType.kind == Type::Kind::I64) {
                // Export returns i1, caller expects i64: zext.
                Instr zext;
                zext.op = Opcode::Zext1;
                zext.type = Type(Type::Kind::I64);
                zext.result = nextTemp;
                zext.operands.push_back(Value::temp(callResult));
                entry.instructions.push_back(std::move(zext));

                Instr ret;
                ret.op = Opcode::Ret;
                ret.type = Type(Type::Kind::I64);
                ret.operands.push_back(Value::temp(nextTemp));
                entry.instructions.push_back(std::move(ret));
                nextTemp++;
            } else if (exportDef.retType.kind == Type::Kind::I64 &&
                       importDecl.retType.kind == Type::Kind::I1) {
                // Export returns i64, caller expects i1: icmp ne 0.
                Instr cmp;
                cmp.op = Opcode::ICmpNe;
                cmp.type = Type(Type::Kind::I1);
                cmp.result = nextTemp;
                cmp.operands.push_back(Value::temp(callResult));
                cmp.operands.push_back(Value::constInt(0));
                entry.instructions.push_back(std::move(cmp));

                Instr ret;
                ret.op = Opcode::Ret;
                ret.type = Type(Type::Kind::I1);
                ret.operands.push_back(Value::temp(nextTemp));
                entry.instructions.push_back(std::move(ret));
                nextTemp++;
            }
        } else {
            // No return conversion — just return the call result.
            Instr ret;
            ret.op = Opcode::Ret;
            ret.type = importDecl.retType;
            ret.operands.push_back(Value::temp(callResult));
            entry.instructions.push_back(std::move(ret));
        }
    } else {
        entry.instructions.push_back(std::move(call));
        Instr ret;
        ret.op = Opcode::Ret;
        entry.instructions.push_back(std::move(ret));
    }

    // Set up valueNames for SSA.
    thunk.valueNames.resize(nextTemp);
    for (const auto &p : thunk.params)
        thunk.valueNames[p.id] = p.name;
    for (unsigned i = 0; i < nextTemp; ++i) {
        if (thunk.valueNames[i].empty())
            thunk.valueNames[i] = "t" + std::to_string(i);
    }

    thunk.blocks.push_back(std::move(entry));
    return thunk;
}

} // namespace

/// @brief Generate adapters for compatible boolean-only ABI mismatches.
/// @details Each fixed-arity import is matched to an export of the same name.
///          Pairs with identical signatures, unsupported differences, variadic
///          signatures, or calling-convention mismatches are ignored.
/// @param importModule Module containing caller-facing import declarations.
/// @param exportModule Module containing candidate exported definitions.
/// @return Generated thunk descriptors in import declaration order.
std::vector<ThunkInfo> generateBooleanThunks(const Module &importModule,
                                             const Module &exportModule) {
    std::vector<ThunkInfo> thunks;

    for (const auto &fn : importModule.functions) {
        if (fn.linkage != Linkage::Import)
            continue;

        const Function *exportFn = findExport(exportModule, fn.name);
        if (!exportFn)
            continue;
        if (fn.params.size() != exportFn->params.size())
            continue;
        if (fn.callingConv != exportFn->callingConv)
            continue;
        // IL call operands do not provide a way for this synthesized wrapper to
        // forward an arbitrary variadic tail.
        if (fn.isVarArg || exportFn->isVarArg)
            continue;

        // Check for boolean mismatches in return type or parameters. Any
        // non-boolean signature difference is not thunkable here.
        bool incompatible = false;
        bool hasMismatch = isBooleanMismatch(fn.retType.kind, exportFn->retType.kind);
        if (fn.retType.kind != exportFn->retType.kind && !hasMismatch)
            incompatible = true;
        for (size_t i = 0; i < fn.params.size(); ++i) {
            const auto importKind = fn.params[i].type.kind;
            const auto exportKind = exportFn->params[i].type.kind;
            if (importKind != exportKind && !isBooleanMismatch(importKind, exportKind)) {
                incompatible = true;
                break;
            }
            if (isBooleanMismatch(importKind, exportKind)) {
                hasMismatch = true;
            }
        }

        if (incompatible || !hasMismatch)
            continue;

        std::string thunkName = fn.name + "$bool_thunk";
        ThunkInfo info;
        info.thunkName = thunkName;
        info.targetName = fn.name;
        info.thunk = generateThunk(fn, *exportFn, thunkName);
        thunks.push_back(std::move(info));
    }

    return thunks;
}

} // namespace il::link
