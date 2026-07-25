//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ControlStatementLowerer.cpp
// Purpose: Implement control-flow statement lowering extracted from Lowerer.
//          Handles lowering of BASIC jump-oriented control constructs (GOSUB, GOTO,
//          RETURN, END) to IL branches and continuation stack operations.
// Key invariants: Maintains Lowerer's control flow lowering semantics exactly
// Ownership/Lifetime: Borrows Lowerer reference; coordinates with parent
// Links: src/frontends/basic/ControlStatementLowerer.hpp,
//        src/frontends/basic/Lowerer.hpp,
//        src/frontends/basic/LocationScope.hpp
//
//===----------------------------------------------------------------------===//

#include "ControlStatementLowerer.hpp"
#include "Lowerer.hpp"
#include "frontends/basic/LocationScope.hpp"

#include <cassert>

using namespace il::core;
using il::core::Instr;
using il::frontends::basic::OverflowPolicy;

/// @file
/// @brief Control-flow lowering helpers for BASIC statements.
/// @details Contains the lowering routines for legacy control constructs such
///          as GOTO, GOSUB, and END.  The helpers coordinate with the active
///          @ref Lowerer context to produce deterministic block graphs while
///          respecting runtime invariants around the continuation stack.

namespace il::frontends::basic {

/// @copydoc ControlStatementLowerer::ControlStatementLowerer()
ControlStatementLowerer::ControlStatementLowerer(Lowerer &lowerer) : lowerer_(lowerer) {}

/// @brief Lower a BASIC GOSUB statement using the runtime-managed continuation stack.
/// @details Materialises the continuation push sequence: verifies/initialises
///          the stack, guards against overflow with a trap block, stores the
///          current continuation index, bumps the stack pointer, and finally
///          branches to the target line's basic block.  Continuation metadata is
///          looked up through @ref ProcedureContext::gosub so matching RETURN
///          statements can pop back to the correct block.
/// @param stmt GOSUB statement providing the target line and source location.
/// @note If the target line is absent, the routine returns after stack setup and
///       continuation lookup without emitting the final branch.
void ControlStatementLowerer::lowerGosub(const GosubStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);
    Lowerer::ProcedureContext &ctx = lowerer_.context();
    Lowerer::Function *func = ctx.function();
    Lowerer::BasicBlock *current = ctx.current();
    if (!func || !current)
        return;

    lowerer_.ensureGosubStack();

    auto &gosubState = ctx.gosub();
    auto contIndex = gosubState.indexFor(&stmt);
    if (!contIndex)
        contIndex = gosubState.registerContinuation(&stmt, ctx.exitIndex());

    Lowerer::Value sp =
        lowerer_.emitLoad(Lowerer::Type(Lowerer::Type::Kind::I64), gosubState.spSlot());

    auto &lineBlocks = ctx.blockNames().lineBlocks();
    auto destIt = lineBlocks.find(stmt.targetLine);
    if (destIt == lineBlocks.end())
        return;

    Lowerer::BlockNamer *blockNamer = ctx.blockNames().namer();
    std::string overflowLbl = blockNamer ? blockNamer->generic("gosub_overflow")
                                         : lowerer_.mangler.block("gosub_overflow");
    std::string pushLbl =
        blockNamer ? blockNamer->generic("gosub_push") : lowerer_.mangler.block("gosub_push");

    size_t curIdx = ctx.blockIndex(current);
    size_t overflowIdx = func->blocks.size();
    lowerer_.builder->addBlock(*func, overflowLbl);
    size_t pushIdx = func->blocks.size();
    lowerer_.builder->addBlock(*func, pushLbl);

    func = ctx.function();
    Lowerer::BasicBlock *overflowBlk = &func->blocks[overflowIdx];
    Lowerer::BasicBlock *pushBlk = &func->blocks[pushIdx];
    current = &func->blocks[curIdx];
    ctx.setCurrent(current);

    Lowerer::Value limit = Lowerer::Value::constInt(Lowerer::kGosubStackDepth);
    Lowerer::Value overflow =
        lowerer_.emitBinary(Lowerer::Opcode::SCmpGE, lowerer_.ilBoolTy(), sp, limit);
    lowerer_.emitCBr(overflow, overflowBlk, pushBlk);

    ctx.setCurrent(overflowBlk);
    lowerer_.requireTrap();
    std::string overflowMsg = lowerer_.getStringLabel("gosub: stack overflow");
    Lowerer::Value overflowStr = lowerer_.emitConstStr(overflowMsg);
    lowerer_.emitCall("rt_trap_string", {overflowStr});
    lowerer_.emitTrap();

    ctx.setCurrent(pushBlk);

    Lowerer::Value offset = lowerer_.emitBinary(Lowerer::Opcode::IMulOvf,
                                                Lowerer::Type(Lowerer::Type::Kind::I64),
                                                sp,
                                                Lowerer::Value::constInt(4));
    Lowerer::Value slotPtr = lowerer_.emitBinary(Lowerer::Opcode::GEP,
                                                 Lowerer::Type(Lowerer::Type::Kind::Ptr),
                                                 gosubState.stackSlot(),
                                                 offset);
    lowerer_.emitStore(Lowerer::Type(Lowerer::Type::Kind::I32),
                       slotPtr,
                       Lowerer::Value::constInt(static_cast<long long>(*contIndex)));

    Lowerer::Value nextSp = lowerer_.emitCommon(stmt.loc).add_checked(
        sp, Lowerer::Value::constInt(1), OverflowPolicy::Checked);
    lowerer_.emitStore(Lowerer::Type(Lowerer::Type::Kind::I64), gosubState.spSlot(), nextSp);

    Lowerer::BasicBlock *target = &func->blocks[destIt->second];
    lowerer_.emitBr(target);
}

/// @brief Lower an unconditional GOTO statement.
/// @details Resolves the destination block via the shared line-label mapping
///          and emits a direct branch when the label has been materialised.
///          Missing targets are ignored.
/// @param stmt GOTO statement pointing at a target line label.
/// @pre A resolved target requires an active function in the lowering context.
void ControlStatementLowerer::lowerGoto(const GotoStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);
    auto &lineBlocks = lowerer_.context().blockNames().lineBlocks();
    auto it = lineBlocks.find(stmt.target);
    if (it != lineBlocks.end()) {
        Lowerer::Function *func = lowerer_.context().function();
        assert(func && "lowerGoto requires an active function");
        lowerer_.emitBr(&func->blocks[it->second]);
    }
}

/// @brief Lower RETURN statements that exit from a GOSUB invocation.
/// @details Pops the continuation stack with full error checking: emits an
///          empty-stack trap, decrements the stack pointer, loads the stored
///          continuation index, and dispatches via a `switch` to the recorded
///          basic block.  Invalid indices funnel into a trap block so mismatched
///          RETURN statements manifest as runtime errors rather than silent
///          corruption.
/// @param stmt RETURN statement appearing in GOSUB contexts.
/// @note Returns without emission if either the active function or current
///       block is absent.
void ControlStatementLowerer::lowerGosubReturn(const ReturnStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);
    Lowerer::ProcedureContext &ctx = lowerer_.context();
    Lowerer::Function *func = ctx.function();
    Lowerer::BasicBlock *current = ctx.current();
    if (!func || !current)
        return;

    lowerer_.ensureGosubStack();

    auto &gosubState = ctx.gosub();

    Lowerer::Value sp =
        lowerer_.emitLoad(Lowerer::Type(Lowerer::Type::Kind::I64), gosubState.spSlot());

    Lowerer::BlockNamer *blockNamer = ctx.blockNames().namer();
    std::string emptyLbl = blockNamer ? blockNamer->generic("gosub_ret_empty")
                                      : lowerer_.mangler.block("gosub_ret_empty");
    std::string contLbl = blockNamer ? blockNamer->generic("gosub_ret_cont")
                                     : lowerer_.mangler.block("gosub_ret_cont");

    size_t curIdx = ctx.blockIndex(current);
    size_t emptyIdx = func->blocks.size();
    lowerer_.builder->addBlock(*func, emptyLbl);
    size_t contIdx = func->blocks.size();
    lowerer_.builder->addBlock(*func, contLbl);

    func = ctx.function();
    Lowerer::BasicBlock *emptyBlk = &func->blocks[emptyIdx];
    Lowerer::BasicBlock *contBlk = &func->blocks[contIdx];
    current = &func->blocks[curIdx];
    ctx.setCurrent(current);

    Lowerer::Value isEmpty = lowerer_.emitBinary(
        Lowerer::Opcode::ICmpEq, lowerer_.ilBoolTy(), sp, Lowerer::Value::constInt(0));
    lowerer_.emitCBr(isEmpty, emptyBlk, contBlk);

    ctx.setCurrent(emptyBlk);
    lowerer_.requireTrap();
    std::string emptyMsg = lowerer_.getStringLabel("gosub: empty return stack");
    Lowerer::Value emptyStr = lowerer_.emitConstStr(emptyMsg);
    lowerer_.emitCall("rt_trap_string", {emptyStr});
    lowerer_.emitTrap();

    ctx.setCurrent(contBlk);

    Lowerer::Value nextSp = lowerer_.emitBinary(Lowerer::Opcode::ISubOvf,
                                                Lowerer::Type(Lowerer::Type::Kind::I64),
                                                sp,
                                                Lowerer::Value::constInt(1));
    lowerer_.emitStore(Lowerer::Type(Lowerer::Type::Kind::I64), gosubState.spSlot(), nextSp);

    Lowerer::Value offset = lowerer_.emitBinary(Lowerer::Opcode::IMulOvf,
                                                Lowerer::Type(Lowerer::Type::Kind::I64),
                                                nextSp,
                                                Lowerer::Value::constInt(4));
    Lowerer::Value slotPtr = lowerer_.emitBinary(Lowerer::Opcode::GEP,
                                                 Lowerer::Type(Lowerer::Type::Kind::Ptr),
                                                 gosubState.stackSlot(),
                                                 offset);
    Lowerer::Value idxVal = lowerer_.emitLoad(Lowerer::Type(Lowerer::Type::Kind::I32), slotPtr);

    std::string invalidLbl = blockNamer ? blockNamer->generic("gosub_ret_invalid")
                                        : lowerer_.mangler.block("gosub_ret_invalid");
    size_t invalidIdx = func->blocks.size();
    lowerer_.builder->addBlock(*func, invalidLbl);
    func = ctx.function();
    Lowerer::BasicBlock *invalidBlk = &func->blocks[invalidIdx];

    Instr sw;
    sw.op = Lowerer::Opcode::SwitchI32;
    sw.type = Lowerer::Type(Lowerer::Type::Kind::Void);
    sw.operands.push_back(idxVal);
    if (invalidBlk->label.empty())
        invalidBlk->label = lowerer_.nextFallbackBlockLabel();
    sw.addBranchTarget(invalidBlk->label);

    const auto &continuations = gosubState.continuations();
    for (unsigned i = 0; i < continuations.size(); ++i) {
        sw.operands.push_back(Lowerer::Value::constInt(static_cast<long long>(i)));
        Lowerer::BasicBlock *target = &func->blocks[continuations[i]];
        if (target->label.empty())
            target->label = lowerer_.nextFallbackBlockLabel();
        sw.addBranchTarget(target->label);
    }
    sw.loc = stmt.loc;
    contBlk->instructions.push_back(std::move(sw));
    contBlk->terminated = true;

    ctx.setCurrent(invalidBlk);
    lowerer_.emitTrap();
}

/// @brief Lower the END statement, terminating program execution.
/// @details Emits `ret 0` for any active function whose declared IL return type
///          is i64. A void, floating, object, missing, or otherwise non-i64
///          function context emits a trap instead.
/// @param stmt END statement providing the source location.
void ControlStatementLowerer::lowerEnd(const EndStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);
    // BUG-OOP-014 fix: Check if current function returns void (SUB) or i64 (main)
    // END in main should return 0; END in SUB/FUNCTION should trap to terminate.
    auto *func = lowerer_.context().function();
    if (func && func->retType.kind == il::core::Type::Kind::I64) {
        // In main() or FUNCTION returning INTEGER - return 0 for normal termination
        lowerer_.emitRet(Lowerer::Value::constInt(0));
    } else {
        // In SUB (void) or other context - trap to terminate program
        lowerer_.emitTrap();
    }
}

} // namespace il::frontends::basic
