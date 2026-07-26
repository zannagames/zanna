//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/Lower_Loops.cpp
// Purpose: Implement BASIC loop lowering helpers that materialise control-flow
//          skeletons and bridge statement bodies into IL basic blocks.
// Key invariants: Generated blocks always form a well-structured loop with
//                 explicit back-edges, and loopState bookkeeping mirrors the
//                 active nesting depth.
// Ownership/Lifetime: Operates on Lowerer-owned ProcedureContext and does not
//                     allocate persistent resources beyond IL instructions.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements loop lowering helpers for BASIC WHILE, DO, and FOR forms.
/// @details Shared routines allocate deterministic head/body/done blocks,
///          establish loop-state bookkeeping, and ensure terminators are
///          emitted with the correct diagnostics context. Each helper preserves
///          the active Lowerer state so nested statements observe consistent
///          control-flow graphs.

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/LocationScope.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "support/diagnostics.hpp"

#include <cassert>

using namespace il::core;

namespace il::frontends::basic {

/// @brief Lower a sequence of statements that forms the body of the enclosing loop.
/// @details Iterates the provided @p body statements, invoking @ref lowerStmt for
///          each element while aborting early once the active basic block has
///          been terminated. The helper acts as the common body driver for all
///          loop forms so they honour break/exit semantics consistently.
/// @param body Ordered collection of statements representing the loop body.
void Lowerer::lowerLoopBody(const std::vector<StmtPtr> &body) {
    for (const auto &stmt : body) {
        if (!stmt)
            continue;
        lowerStmt(*stmt);
        BasicBlock *current = context().current();
        if (!current || current->terminated)
            break;
    }
}

/// @brief Emit the control-flow scaffolding for a BASIC WHILE loop.
/// @details Allocates head, body, and done blocks using the block namer, wires
///          the conditional branch in the head, and lowers the body statements.
///          The loop exit state is recorded through @ref loopState so EXIT
///          statements resolve correctly. The resulting @ref CtrlState captures
///          the block that follows the loop and whether it remains fallthrough.
/// @param stmt Parsed WHILE statement supplying the condition and body.
/// @return Control-flow snapshot pointing at the loop's done block.
Lowerer::CtrlState Lowerer::emitWhile(const WhileStmt &stmt) {
    LocationScope loc(*this, stmt.loc);
    CtrlState state{};
    auto &ctx = context();
    auto *func = ctx.function();
    auto *current = ctx.current();
    if (!func || !current)
        return state;

    // Save a stable index to the current block. Adding blocks may reallocate
    // the vector and invalidate raw pointers.
    const size_t curIdx = ctx.currentIndex();

    BlockNamer *blockNamer = ctx.blockNames().namer();
    size_t start = func->blocks.size();
    unsigned id = blockNamer ? blockNamer->nextWhile() : 0;
    std::string headLbl = blockNamer ? blockNamer->whileHead(id) : mangler.block("loop_head");
    std::string bodyLbl = blockNamer ? blockNamer->whileBody(id) : mangler.block("loop_body");
    std::string doneLbl = blockNamer ? blockNamer->whileEnd(id) : mangler.block("done");
    builder->addBlock(*func, headLbl);
    builder->addBlock(*func, bodyLbl);
    builder->addBlock(*func, doneLbl);

    func = ctx.function();
    size_t headIdx = start;
    size_t bodyIdx = start + 1;
    size_t doneIdx = start + 2;
    auto *head = &func->blocks[headIdx];
    auto *body = &func->blocks[bodyIdx];
    auto *done = &func->blocks[doneIdx];
    state.after = done;
    ctx.loopState().push(done);
    // Rebind current after potential reallocation, then branch to head.
    ctx.setCurrentByIndex(curIdx);
#ifndef NDEBUG
    assert(ctx.current() == &func->blocks[curIdx] &&
           "lost active block after while block allocation");
#endif
    emitBr(head);
    func = ctx.function();
    head = &func->blocks[headIdx];
    body = &func->blocks[bodyIdx];
    done = &func->blocks[doneIdx];
    ctx.setCurrent(head);
    lowerCondBranch(*stmt.cond, body, done, stmt.loc);

    func = ctx.function();
    body = &func->blocks[bodyIdx];
    done = &func->blocks[doneIdx];
    ctx.setCurrent(body);
    lowerLoopBody(stmt.body);
    auto *bodyCur = ctx.current();
    bool exitTaken = ctx.loopState().taken();
    bool term = bodyCur && bodyCur->terminated;
    if (!term) {
        func = ctx.function();
        head = &func->blocks[headIdx];
        emitBr(head);
    }

    func = ctx.function();
    done = &func->blocks[doneIdx];
    ctx.loopState().refresh(done);
    ctx.setCurrent(done);
    // Do not mark the done block as terminated here. The statement sequencer
    // will emit the fallthrough branch to the next line block. Marking this
    // block terminated without emitting a terminator causes IL verifier
    // "empty block" errors when the loop body consisted solely of control
    // transfers (e.g., GOSUB). Keep the block open; callers append next.
    (void)exitTaken;
    (void)term;
    ctx.loopState().pop();

    state.cur = ctx.current();
    state.after = state.cur;
    state.fallthrough = !done->terminated;
    return state;
}

/// @brief Lower a WHILE statement by delegating to @ref emitWhile.
/// @details Calls @ref emitWhile to build the loop skeleton then restores the
///          procedure context's current block to the returned block so that
///          subsequent statements append to the correct successor.
/// @param stmt WHILE statement to lower into IL blocks.
void Lowerer::lowerWhile(const WhileStmt &stmt) {
    CtrlState state = emitWhile(stmt);
    if (state.cur)
        context().setCurrent(state.cur);
}

/// @brief Construct the control-flow for BASIC DO loops (pre- and post-test).
/// @details Emits the shared head/body/done structure, handles the varying test
///          placement, and respects optional DO...LOOP UNTIL/WHILE semantics by
///          branching appropriately based on @p stmt.condKind. Loop state is
///          tracked to honour EXIT statements and to refresh the done block once
///          the body finishes executing.
/// @param stmt DO statement containing loop body, test, and metadata.
/// @return Control state capturing the block following the DO loop.
Lowerer::CtrlState Lowerer::emitDo(const DoStmt &stmt) {
    LocationScope loc(*this, stmt.loc);
    CtrlState state{};
    auto &ctx = context();
    auto *func = ctx.function();
    auto *current = ctx.current();
    if (!func || !current)
        return state;

    const size_t currentIdx = ctx.currentIndex();

    BlockNamer *blockNamer = ctx.blockNames().namer();
    size_t start = func->blocks.size();
    unsigned id = blockNamer ? blockNamer->nextDo() : 0;
    std::string headLbl = blockNamer ? blockNamer->doHead(id) : mangler.block("do_head");
    std::string bodyLbl = blockNamer ? blockNamer->doBody(id) : mangler.block("do_body");
    std::string doneLbl = blockNamer ? blockNamer->doEnd(id) : mangler.block("do_done");
    builder->addBlock(*func, headLbl);
    builder->addBlock(*func, bodyLbl);
    builder->addBlock(*func, doneLbl);

    func = ctx.function();
    size_t headIdx = start;
    size_t bodyIdx = start + 1;
    size_t doneIdx = start + 2;
    auto *done = &func->blocks[doneIdx];
    state.after = done;
    ctx.setCurrentByIndex(currentIdx);
    ctx.loopState().push(done);
    /// @brief Emits the DO-loop header and its condition-controlled branch.
    auto emitHead = [&]() {
        func = ctx.function();
        if (func->blocks[headIdx].label.empty())
            func->blocks[headIdx].label = headLbl;
        if (func->blocks[bodyIdx].label.empty())
            func->blocks[bodyIdx].label = bodyLbl;
        auto *head = &func->blocks[headIdx];
        ctx.setCurrent(head);
        if (stmt.condKind == DoStmt::CondKind::None) {
            emitBr(&func->blocks[bodyIdx]);
            return;
        }
        assert(stmt.cond && "DO loop missing condition for conditional form");
        auto *body = &func->blocks[bodyIdx];
        auto *doneBlk = &func->blocks[doneIdx];
        if (stmt.condKind == DoStmt::CondKind::While) {
            lowerCondBranch(*stmt.cond, body, doneBlk, stmt.loc);
        } else {
            lowerCondBranch(*stmt.cond, doneBlk, body, stmt.loc);
        }
    };

    func = ctx.function();
    switch (stmt.testPos) {
        case DoStmt::TestPos::Pre:
            if (func->blocks[headIdx].label.empty())
                func->blocks[headIdx].label = headLbl;
            emitBr(&func->blocks[headIdx]);
            emitHead();
            ctx.setCurrent(&func->blocks[bodyIdx]);
            break;
        case DoStmt::TestPos::Post:
            if (func->blocks[bodyIdx].label.empty())
                func->blocks[bodyIdx].label = bodyLbl;
            emitBr(&func->blocks[bodyIdx]);
            ctx.setCurrent(&func->blocks[bodyIdx]);
            break;
    }

    lowerLoopBody(stmt.body);
    auto *bodyCur = ctx.current();
    bool exitTaken = ctx.loopState().taken();
    bool term = bodyCur && bodyCur->terminated;

    if (!term) {
        func = ctx.function();
        if (func->blocks[headIdx].label.empty())
            func->blocks[headIdx].label = headLbl;
        emitBr(&func->blocks[headIdx]);
    }

    if (stmt.testPos == DoStmt::TestPos::Post)
        emitHead();
    func = ctx.function();
    if (func->blocks[doneIdx].label.empty())
        func->blocks[doneIdx].label = doneLbl;
    done = &func->blocks[doneIdx];
    ctx.loopState().refresh(done);
    ctx.setCurrent(done);
    const bool postTest = stmt.testPos == DoStmt::TestPos::Post;
    // Leave the done block unterminated here so the statement sequencer
    // can wire a fallthrough edge to the subsequent line. Setting
    // done->terminated without emitting an instruction leads to verifier
    // failures (empty block) for loops whose bodies generate only branches.
    (void)exitTaken;
    (void)term;
    ctx.loopState().pop();

    state.cur = ctx.current();
    state.after = state.cur;
    state.fallthrough = postTest ? true : !done->terminated;
    return state;
}

/// @brief Lower a DO loop and update the current block to its continuation.
/// @details Invokes @ref emitDo to generate the loop and, when a continuation
///          block is returned, rebinds the context so subsequent statements emit
///          after the loop terminates.
/// @param stmt DO statement to lower.
void Lowerer::lowerDo(const DoStmt &stmt) {
    CtrlState state = emitDo(stmt);
    if (state.cur)
        context().setCurrent(state.cur);
}

/// @brief Allocate the basic blocks required by FOR loops.
/// @details Appends the necessary head, body, increment, and done blocks to the
///          active function. When @p varStep is true additional head blocks are
///          created to handle positive vs negative step comparisons. The
///          procedure context's current block is restored before returning so
///          callers can immediately start emitting control flow.
/// @param varStep Indicates whether the loop step is a runtime value.
/// @return Indices of the newly created blocks.
Lowerer::ForBlocks Lowerer::setupForBlocks(bool varStep) {
    ProcedureContext &ctx = context();
    Function *func = ctx.function();
    assert(func && ctx.current());
    BlockNamer *blockNamer = ctx.blockNames().namer();
    size_t curIdx = ctx.currentIndex();
    size_t base = func->blocks.size();
    unsigned id = blockNamer ? blockNamer->nextFor() : 0;
    ForBlocks fb;
    if (varStep) {
        std::string headPosLbl =
            blockNamer ? blockNamer->generic("for_head_pos") : mangler.block("for_head_pos");
        std::string headNegLbl =
            blockNamer ? blockNamer->generic("for_head_neg") : mangler.block("for_head_neg");
        builder->addBlock(*func, headPosLbl);
        builder->addBlock(*func, headNegLbl);
        fb.headPosIdx = base;
        fb.headNegIdx = base + 1;
        base += 2;
    } else {
        std::string headLbl = blockNamer ? blockNamer->forHead(id) : mangler.block("for_head");
        builder->addBlock(*func, headLbl);
        fb.headIdx = base;
        base += 1;
    }
    std::string bodyLbl = blockNamer ? blockNamer->forBody(id) : mangler.block("for_body");
    std::string incLbl = blockNamer ? blockNamer->forInc(id) : mangler.block("for_inc");
    std::string doneLbl = blockNamer ? blockNamer->forEnd(id) : mangler.block("for_done");
    builder->addBlock(*func, bodyLbl);
    builder->addBlock(*func, incLbl);
    builder->addBlock(*func, doneLbl);
    fb.bodyIdx = base;
    fb.incIdx = base + 1;
    fb.doneIdx = base + 2;
    ctx.setCurrentByIndex(curIdx);
    return fb;
}

/// @brief Lower a FOR loop whose step value is a compile-time constant.
/// @details Builds the canonical FOR skeleton via @ref setupForBlocks, emits
///          comparisons against the @p end value using the sign of @p stepConst
///          to pick the appropriate comparison opcode, and lowers the loop body.
///          When the body leaves without a terminator the helper emits both the
///          increment and the back-edge branch before refreshing the loop state.
/// @param stmt Parsed FOR statement driving the control-flow structure.
/// @param slot Slot storing the loop induction variable.
/// @param end Result of lowering the loop's end expression.
/// @param step Lowered representation of the step expression.
/// @param stepConst Constant value of the step used to choose comparison sense.
void Lowerer::lowerForConstStep(
    const ForStmt &stmt, Value slot, RVal end, RVal step, int64_t stepConst) {
    LocationScope loc(*this, stmt.loc);
    ForBlocks fb = setupForBlocks(false);
    ProcedureContext &ctx = context();
    Function *func = ctx.function();
    assert(func && "lowerForConstStep requires an active function");
    size_t doneIdx = fb.doneIdx;
    BasicBlock *done = &func->blocks[doneIdx];
    ctx.loopState().push(done);
    emitBr(&func->blocks[fb.headIdx]);
    ctx.setCurrent(&func->blocks[fb.headIdx]);
    Value curVal = emitLoad(Type(Type::Kind::I64), slot);
    Opcode cmp = stepConst >= 0 ? Opcode::SCmpLE : Opcode::SCmpGE;
    Value cond = emitBinary(cmp, Type(Type::Kind::I1), curVal, end.value);
    emitCBr(cond, &func->blocks[fb.bodyIdx], &func->blocks[fb.doneIdx]);
    ctx.setCurrent(&func->blocks[fb.bodyIdx]);
    lowerLoopBody(stmt.body);
    BasicBlock *current = ctx.current();
    bool exitTaken = ctx.loopState().taken();
    bool term = current && current->terminated;
    if (!term) {
        emitBr(&func->blocks[fb.incIdx]);
        ctx.setCurrent(&func->blocks[fb.incIdx]);
        emitForStep(slot, step.value);
        emitBr(&func->blocks[fb.headIdx]);
    }
    done = &func->blocks[doneIdx];
    ctx.loopState().refresh(done);
    ctx.setCurrent(done);
    // Keep done block open; the sequencer will branch to the next line.
    (void)exitTaken;
    (void)term;
    ctx.loopState().pop();
}

/// @brief Lower a FOR loop whose step is evaluated at runtime.
/// @details Splits the loop head into positive and negative variants so the
///          comparison direction matches the sign of the step. The helper emits
///          a runtime check that branches to the correct head, performs the body
///          lowering, and emits increment/back-edge logic mirroring the control
///          path used on entry. Loop state is refreshed to maintain EXIT
///          semantics.
/// @param stmt Source FOR statement definition.
/// @param slot Slot representing the induction variable storage.
/// @param end Lowered end bound expression.
/// @param step Lowered step expression.
void Lowerer::lowerForVarStep(const ForStmt &stmt, Value slot, RVal end, RVal step) {
    LocationScope loc(*this, stmt.loc);
    Value stepNonNeg =
        emitBinary(Opcode::SCmpGE, Type(Type::Kind::I1), step.value, Value::constInt(0));
    ForBlocks fb = setupForBlocks(true);
    ProcedureContext &ctx = context();
    Function *func = ctx.function();
    assert(func && "lowerForVarStep requires an active function");
    size_t doneIdx = fb.doneIdx;
    BasicBlock *done = &func->blocks[doneIdx];
    ctx.loopState().push(done);
    emitCBr(stepNonNeg, &func->blocks[fb.headPosIdx], &func->blocks[fb.headNegIdx]);
    ctx.setCurrent(&func->blocks[fb.headPosIdx]);
    Value curVal = emitLoad(Type(Type::Kind::I64), slot);
    Value cmpPos = emitBinary(Opcode::SCmpLE, Type(Type::Kind::I1), curVal, end.value);
    emitCBr(cmpPos, &func->blocks[fb.bodyIdx], &func->blocks[fb.doneIdx]);
    ctx.setCurrent(&func->blocks[fb.headNegIdx]);
    curVal = emitLoad(Type(Type::Kind::I64), slot);
    Value cmpNeg = emitBinary(Opcode::SCmpGE, Type(Type::Kind::I1), curVal, end.value);
    emitCBr(cmpNeg, &func->blocks[fb.bodyIdx], &func->blocks[fb.doneIdx]);
    ctx.setCurrent(&func->blocks[fb.bodyIdx]);
    lowerLoopBody(stmt.body);
    BasicBlock *current = ctx.current();
    bool exitTaken = ctx.loopState().taken();
    bool term = current && current->terminated;
    if (!term) {
        emitBr(&func->blocks[fb.incIdx]);
        ctx.setCurrent(&func->blocks[fb.incIdx]);
        emitForStep(slot, step.value);
        emitCBr(stepNonNeg, &func->blocks[fb.headPosIdx], &func->blocks[fb.headNegIdx]);
    }
    done = &func->blocks[doneIdx];
    ctx.loopState().refresh(done);
    ctx.setCurrent(done);
    // Keep done block open; the sequencer will branch to the next line.
    (void)exitTaken;
    (void)term;
    ctx.loopState().pop();
}

/// @brief Dispatch FOR loop lowering based on step characteristics.
/// @details Currently routes all loops through @ref lowerForVarStep because the
///          lowering logic inspects the step dynamically to decide which
///          comparison path to follow. The returned control state reflects the
///          continuation block after the loop completes.
/// @param stmt Parsed FOR loop metadata.
/// @param slot Slot storing the induction variable.
/// @param end Lowered end bound result.
/// @param step Lowered step expression.
/// @return Control state representing the loop's continuation.
Lowerer::CtrlState Lowerer::emitFor(const ForStmt &stmt, Value slot, RVal end, RVal step) {
    CtrlState state{};
    lowerForVarStep(stmt, slot, end, step);
    state.cur = context().current();
    state.after = state.cur;
    state.fallthrough = state.cur && !state.cur->terminated;
    return state;
}

/// @brief Lower a BASIC FOR loop from its AST representation.
/// @details Lowers the start, end, and optional step expressions, initialises
///          the induction variable slot with the start value, and forwards to
///          @ref emitFor to build the IL control flow. After lowering the loop
///          the current block in the procedure context is updated to the loop's
///          continuation.
/// @param stmt Source FOR statement to lower.
void Lowerer::lowerFor(const ForStmt &stmt) {
    LocationScope loc(*this, stmt.loc);
    RVal start = lowerScalarExpr(*stmt.start);
    RVal end = lowerScalarExpr(*stmt.end);
    RVal step =
        stmt.step ? lowerScalarExpr(*stmt.step) : RVal{Value::constInt(1), Type(Type::Kind::I64)};

    // BUG-081 fix: Handle expression-based loop variables
    // Get pointer to the loop variable storage (lvalue)
    Value ctrlSlot;
    if (!stmt.varExpr) {
        // Missing loop variable expression - this is a parser bug.
        // Emit error and skip lowering to avoid generating invalid IL.
        if (auto *em = diagnosticEmitter()) {
            em->emit(il::support::Severity::Error,
                     "E_FOR_NO_VAR",
                     stmt.loc,
                     1,
                     "FOR loop missing control variable");
        }
        return;
    } else if (auto *varExpr = as<VarExpr>(*stmt.varExpr)) {
        // Simple variable: FOR i = 1 TO 10
        // Use unified variable storage resolution so global loop variables update
        // their module-level storage instead of a loop-local slot (BUG-078).
        if (auto storage = resolveVariableStorage(varExpr->name, stmt.loc)) {
            ctrlSlot = storage->pointer;
        } else {
            const auto *info = findSymbol(varExpr->name);
            assert(info && info->slotId);
            ctrlSlot = Value::temp(*info->slotId);
        }
    } else if (auto *memberAccess = as<MemberAccessExpr>(*stmt.varExpr)) {
        // Member access: FOR obj.field = 1 TO 10
        if (auto access = resolveMemberField(*memberAccess)) {
            ctrlSlot = access->ptr;
        } else {
            // Member field resolution failed - likely undefined object/field.
            // Emit error and skip lowering.
            if (auto *em = diagnosticEmitter()) {
                std::string msg = "cannot resolve member field '" + memberAccess->member +
                                  "' for FOR loop control variable";
                em->emit(il::support::Severity::Error,
                         "E_FOR_MEMBER_RESOLVE",
                         stmt.loc,
                         static_cast<uint32_t>(memberAccess->member.size()),
                         std::move(msg));
            }
            return;
        }
    } else if (auto *arrayExpr = as<ArrayExpr>(*stmt.varExpr)) {
        // Array element loop variables (e.g., FOR arr(i) = 1 TO 10) are not yet supported.
        // Emit a compile-time error and skip lowering to avoid generating bogus IL.
        if (auto *em = diagnosticEmitter()) {
            std::string msg = "FOR control variable cannot be an array element (" +
                              arrayExpr->name + "(...)); not yet supported";
            em->emit(il::support::Severity::Error,
                     "E_FOR_ARRAY_CTRL",
                     stmt.loc,
                     static_cast<uint32_t>(arrayExpr->name.size()),
                     std::move(msg));
        }
        return; // Skip lowering - do not generate bogus IL
    } else {
        // Unsupported expression type for FOR control variable.
        // Emit a compile-time error and skip lowering.
        if (auto *em = diagnosticEmitter()) {
            std::string msg = "FOR control variable must be a simple variable or object field";
            em->emit(il::support::Severity::Error,
                     "E_FOR_UNSUPPORTED_CTRL",
                     stmt.loc,
                     1,
                     std::move(msg));
        }
        return; // Skip lowering - do not generate bogus IL
    }

    // Store initial value to loop variable
    emitStore(Type(Type::Kind::I64), ctrlSlot, start.value);

    // Emit the loop using the resolved pointer for the control variable
    CtrlState state = emitFor(stmt, ctrlSlot, end, step);
    if (state.cur)
        context().setCurrent(state.cur);
}

/// @brief Lower a FOR EACH array iteration loop.
/// @details Generates the equivalent of:
///          index = 0
///          length = array_length(arr)
///          while index < length:
///              element = arr(index)
///              ... body ...
///              index = index + 1
/// @param stmt FOR EACH statement to lower.
void Lowerer::lowerForEach(const ForEachStmt &stmt) {
    LocationScope loc(*this, stmt.loc);
    auto &ctx = context();
    auto *func = ctx.function();
    if (!func || !ctx.current())
        return;

    // Resolve the array symbol
    const auto *arrSym = findSymbol(stmt.arrayName);
    if (!arrSym || !arrSym->slotId) {
        emitTrap();
        return;
    }

    // Resolve the element variable (should have been created by semantic analysis)
    const auto *elemSym = findSymbol(stmt.elementVar);
    if (!elemSym || !elemSym->slotId) {
        emitTrap();
        return;
    }

    // Load the array base pointer
    Value arrSlot = Value::temp(*arrSym->slotId);
    Value arrBase = emitLoad(Type(Type::Kind::Ptr), arrSlot);

    // Get array length using appropriate runtime function
    Value length;
    if (arrSym->type == AstType::Str)
        length = emitCallRet(Type(Type::Kind::I64), "rt_arr_str_len", {arrBase});
    else if (arrSym->isObject)
        length = emitCallRet(Type(Type::Kind::I64), "rt_arr_obj_len", {arrBase});
    else
        length = emitCallRet(Type(Type::Kind::I64), "rt_arr_i64_len", {arrBase});

    // Create a temporary index slot using alloca for 8-byte i64
    Value indexSlot = emitAlloca(8);
    emitStore(Type(Type::Kind::I64), indexSlot, Value::constInt(0));

    // Get element slot
    Value elemSlot = Value::temp(*elemSym->slotId);

    // Setup loop blocks
    BlockNamer *blockNamer = ctx.blockNames().namer();
    size_t curIdx = ctx.currentIndex();
    size_t start = func->blocks.size();
    unsigned id = blockNamer ? blockNamer->nextFor() : 0;

    std::string headLbl = blockNamer ? blockNamer->forHead(id) : mangler.block("foreach_head");
    std::string bodyLbl = blockNamer ? blockNamer->forBody(id) : mangler.block("foreach_body");
    std::string incLbl = blockNamer ? blockNamer->forInc(id) : mangler.block("foreach_inc");
    std::string doneLbl = blockNamer ? blockNamer->forEnd(id) : mangler.block("foreach_done");

    builder->addBlock(*func, headLbl);
    builder->addBlock(*func, bodyLbl);
    builder->addBlock(*func, incLbl);
    builder->addBlock(*func, doneLbl);

    func = ctx.function();
    size_t headIdx = start;
    size_t bodyIdx = start + 1;
    size_t incIdx = start + 2;
    size_t doneIdx = start + 3;

    BasicBlock *done = &func->blocks[doneIdx];
    ctx.setCurrentByIndex(curIdx);
    ctx.loopState().push(done);

    // Branch to loop head
    emitBr(&func->blocks[headIdx]);

    // Head block: check if index < length
    ctx.setCurrent(&func->blocks[headIdx]);
    Value curIndex = emitLoad(Type(Type::Kind::I64), indexSlot);
    Value cond = emitBinary(Opcode::SCmpLT, Type(Type::Kind::I1), curIndex, length);
    emitCBr(cond, &func->blocks[bodyIdx], &func->blocks[doneIdx]);

    // Body block: load element from array into element variable
    ctx.setCurrent(&func->blocks[bodyIdx]);
    curIndex = emitLoad(Type(Type::Kind::I64), indexSlot);

    // Access array element and store to element variable
    // Use appropriate runtime function based on element type
    if (arrSym->type == AstType::Str) {
        Value elem = emitCallRet(Type(Type::Kind::Str), "rt_arr_str_get", {arrBase, curIndex});
        emitStore(Type(Type::Kind::Str), elemSlot, elem);
    } else if (arrSym->isObject) {
        Value elem = emitCallRet(Type(Type::Kind::Ptr), "rt_arr_obj_get", {arrBase, curIndex});
        emitStore(Type(Type::Kind::Ptr), elemSlot, elem);
    } else if (arrSym->type == AstType::F64) {
        // Float arrays - use rt_arr_f64_get runtime function
        requireArrayF64Get();
        Value elem = emitCallRet(Type(Type::Kind::F64), "rt_arr_f64_get", {arrBase, curIndex});
        emitStore(Type(Type::Kind::F64), elemSlot, elem);
    } else {
        // Integer arrays - use rt_arr_i64_get runtime function
        Value elem = emitCallRet(Type(Type::Kind::I64), "rt_arr_i64_get", {arrBase, curIndex});
        emitStore(Type(Type::Kind::I64), elemSlot, elem);
    }

    // Lower the loop body
    lowerLoopBody(stmt.body);

    BasicBlock *current = ctx.current();
    bool term = current && current->terminated;

    // Increment block
    if (!term) {
        emitBr(&func->blocks[incIdx]);
        ctx.setCurrent(&func->blocks[incIdx]);
        Value idx = emitLoad(Type(Type::Kind::I64), indexSlot);
        Value nextIdx = emitBinary(Opcode::IAddOvf, Type(Type::Kind::I64), idx, Value::constInt(1));
        emitStore(Type(Type::Kind::I64), indexSlot, nextIdx);
        emitBr(&func->blocks[headIdx]);
    }

    // Done block
    func = ctx.function();
    done = &func->blocks[doneIdx];
    ctx.loopState().refresh(done);
    ctx.setCurrent(done);
    ctx.loopState().pop();
}

/// @brief Lower the BASIC NEXT statement.
/// @details NEXT is a parsing artefact in the current lowering pipeline and is
///          therefore ignored. The stub remains so future loop finalisation
///          logic has a dedicated extension point.
/// @param next NEXT statement node (unused).
void Lowerer::lowerNext(const NextStmt &next) {
    (void)next;
}

/// @brief Lower an EXIT statement within a loop.
/// @details Resolves the loop's exit block from the @ref loopState stack. When
///          no loop context is active the helper emits a trap, otherwise it
///          branches to the exit block and records that the exit path has been
///          taken so the loop continuation remains reachable.
/// @param stmt EXIT statement to lower.
void Lowerer::lowerExit(const ExitStmt &stmt) {
    LocationScope loc(*this, stmt.loc);
    ProcedureContext &ctx = context();

    // EXIT FUNCTION/SUB should branch directly to the procedure's exit block,
    // not to the current loop's exit block.
    if (stmt.kind == ExitStmt::LoopKind::Function || stmt.kind == ExitStmt::LoopKind::Sub) {
        Function *func = ctx.function();
        if (!func || ctx.exitIndex() >= func->blocks.size()) {
            emitTrap();
            return;
        }
        BasicBlock *target = &func->blocks[ctx.exitIndex()];
        emitBr(target);
        return;
    }

    // For regular loops (FOR/WHILE/DO), use the loop exit target
    BasicBlock *target = ctx.loopState().current();
    if (!target) {
        emitTrap();
        return;
    }
    emitBr(target);
    ctx.loopState().markTaken();
}
} // namespace il::frontends::basic
