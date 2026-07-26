//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/Lower_TryCatch.cpp
// Purpose: Lower BASIC ON ERROR and RESUME constructs by manipulating the
//          lowerer's exception-handling stack.
// Key invariants: Handler metadata in the procedure context reflects the most
//                 recent ON ERROR directive; resume tokens are only materialised
//                 when the target handler remains live.
// Ownership/Lifetime: Routines borrow the @ref Lowerer state and update handler
//                     caches owned by @ref ProcedureContext.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements ON ERROR/RESUME lowering for BASIC error-handling constructs.
/// @details Helpers manipulate the Lowerer's exception-handling stack metadata,
/// ensuring runtime handlers are installed and resumed according to the source
/// semantics without leaking state between statements.

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/OopIndex.hpp"
#include "frontends/basic/OopLoweringContext.hpp"

using namespace il::core;

namespace il::frontends::basic {

/// @brief Lower an @c ON @c ERROR directive to push or clear runtime handlers.
///
/// @details Establishes the correct handler block when @p stmt targets a line
///          number and clears state when @c ON @c ERROR @c GOTO @c 0 is
///          encountered.  The helper ensures the procedure context records the
///          active handler index and line for use by subsequent statements.
/// @param stmt AST node describing the ON ERROR directive.
void Lowerer::lowerOnErrorGoto(const OnErrorGoto &stmt) {
    ProcedureContext &ctx = context();
    Function *func = ctx.function();
    BasicBlock *current = ctx.current();
    if (!func || !current)
        return;

    curLoc = stmt.loc;

    // NOTE: No-op here; curIdx tracking belongs in lowerTryCatch.

    if (stmt.toZero) {
        clearActiveErrorHandler();
        return;
    }

    clearActiveErrorHandler();

    BasicBlock *handler = ensureErrorHandlerBlock(stmt.target);
    emitEhPush(handler);

    size_t idx = ctx.blockIndex(handler);
    ctx.errorHandlers().setActive(true);
    ctx.errorHandlers().setActiveIndex(idx);
    ctx.errorHandlers().setActiveLine(stmt.target);
}

/// @brief Lower a RESUME statement that unwinds to a stored handler.
///
/// @details Finds the appropriate handler block (either by explicit line or the
///          currently active handler), materialises a resume token, and appends
///          the matching opcode to the handler block.  The helper bails out if no
///          live handler exists or if the block already terminated.
/// @param stmt AST node describing the RESUME statement.
void Lowerer::lowerResume(const Resume &stmt) {
    ProcedureContext &ctx = context();
    Function *func = ctx.function();
    if (!func)
        return;

    std::optional<size_t> handlerIndex;

    auto &handlersByLine = ctx.errorHandlers().blocks();
    if (auto it = handlersByLine.find(stmt.target); it != handlersByLine.end()) {
        handlerIndex = it->second;
    } else if (auto active = ctx.errorHandlers().activeIndex()) {
        handlerIndex = *active;
    }

    if (!handlerIndex || *handlerIndex >= func->blocks.size())
        return;

    BasicBlock &handlerBlock = func->blocks[*handlerIndex];
    if (handlerBlock.terminated)
        return;

    if (handlerBlock.params.size() < 2)
        return;

    // Retrieve the resume token via the builder to avoid manipulating
    // function valueNames directly.
    Value resumeTok = builder->blockParam(handlerBlock, 1);

    Instr instr;
    instr.type = Type(Type::Kind::Void);
    instr.loc = curLoc;
    instr.operands.push_back(resumeTok);

    switch (stmt.mode) {
        case Resume::Mode::Same:
            instr.op = Opcode::ResumeSame;
            break;
        case Resume::Mode::Next:
            instr.op = Opcode::ResumeNext;
            break;
        case Resume::Mode::Label: {
            instr.op = Opcode::ResumeLabel;
            auto &lineBlocks = ctx.blockNames().lineBlocks();
            auto targetIt = lineBlocks.find(stmt.target);
            if (targetIt == lineBlocks.end())
                return;
            size_t targetIdx = targetIt->second;
            if (targetIdx >= func->blocks.size())
                return;
            instr.addBranchTarget(func->blocks[targetIdx].label);
            break;
        }
    }

    handlerBlock.instructions.push_back(std::move(instr));
    handlerBlock.terminated = true;
}

/// @brief Lower a TRY/CATCH/FINALLY statement using the runtime EH model.
///
/// Interaction model with legacy ON ERROR/RESUME:
/// - TRY installs a fresh handler using only `eh.push`/`eh.pop`, without mutating
///   the Lowerer's ErrorHandlerState (active/line/index). This ensures a preexisting
///   ON ERROR GOTO handler remains beneath the TRY handler on the runtime stack and
///   is automatically restored when TRY exits (single `eh.pop`).
/// - CATCH may include a RESUME statement. It is permitted but typically unnecessary,
///   because the canonical endpoint of the handler uses `resume.label %tok, ^after_try`.
///
/// Emission sequence (without FINALLY):
/// - Emit `eh.push ^handler` before the try-body.
/// - Lower try-body; on normal fallthrough emit `eh.pop` and branch to `^after_try`.
/// - In the handler block (with `eh.entry` and params `%err`, `%tok`):
///     * Optionally initialise the catch variable with ERR() (i64) if resolvable.
///     * Lower the catch-body.
///     * Terminate with `resume.label %tok, ^after_try`.
///
/// Emission sequence (with FINALLY):
/// - Emit `eh.push ^handler` before the try-body.
/// - Lower try-body; on normal fallthrough emit `eh.pop` and branch to `^finally_normal`.
/// - In `^finally_normal`: lower finally-body, then branch to `^after_try`.
/// - In the handler block:
///     * Lower catch-body (if present).
///     * Lower finally-body (duplicated for handler path).
///     * Terminate with `resume.label %tok, ^after_try`.
///
/// Note: The finally code is duplicated between the normal path and exception path
/// because `resume.label` must be the terminator of the handler block, and we cannot
/// branch to a shared finally block and then return to emit the resume.
/// @param stmt TRY statement containing ordered try, catch, and finally bodies,
///        plus the optional catch variable.
void Lowerer::lowerTryCatch(const TryCatchStmt &stmt) {
    ProcedureContext &ctx = context();
    Function *func = ctx.function();
    BasicBlock *current = ctx.current();
    if (!func || !current)
        return;

    curLoc = stmt.loc;

    const bool hasFinally = !stmt.finallyBody.empty();
    const bool hasCatch = !stmt.catchBody.empty() || stmt.catchVar.has_value();
    if (!hasCatch && !hasFinally) {
        if (auto *diag = diagnosticEmitter()) {
            diag->emit(il::support::Severity::Error,
                       "B0801",
                       stmt.loc,
                       1,
                       "TRY requires CATCH, FINALLY, or both");
        }
        for (const auto &st : stmt.tryBody) {
            if (!st)
                continue;
            lowerStmt(*st);
            BasicBlock *cur = ctx.current();
            if (!cur || cur->terminated)
                break;
        }
        return;
    }

    // Capture the index of the current block before creating any new blocks,
    // since appending to the function's block list may reallocate and
    // invalidate raw pointers stored in the context.
    const std::size_t curIdx = ctx.currentIndex();

    // Create the post-try continuation block with a deterministic label.
    BlockNamer *blockNamer = ctx.blockNames().namer();
    const size_t afterIdx = func->blocks.size();
    std::string afterLbl =
        blockNamer ? blockNamer->generic("after_try") : mangler.block("after_try");
    builder->addBlock(*func, afterLbl);

    // Restore pointers that might be invalidated by block creation.
    func = ctx.function();
    BasicBlock *afterTry = &func->blocks[afterIdx];

    // Create the finally_normal block if we have finally code.
    // This is where the normal (non-exception) path runs the finally code.
    size_t finallyNormalIdx = 0;
    if (hasFinally) {
        func = ctx.function();
        finallyNormalIdx = func->blocks.size();
        std::string finallyLbl =
            blockNamer ? blockNamer->generic("finally") : mangler.block("finally");
        builder->addBlock(*func, finallyLbl);
        func = ctx.function();
    }

    // Determine a stable handler key. Prefer the first statement inside TRY so
    // the handler is associated with that line; fall back to the TRY node.
    int handlerKey = virtualLine(stmt);
    if (!stmt.tryBody.empty()) {
        for (const auto &sp : stmt.tryBody) {
            if (sp) {
                handlerKey = virtualLine(*sp);
                break;
            }
        }
    }

    // Pre-create handler block keyed by handlerKey so we can capture its label
    // before creating additional blocks that may reallocate the block vector.
    BasicBlock *preHandler = ensureErrorHandlerBlock(handlerKey);
    std::string preHandlerLabel = preHandler ? preHandler->label : std::string{};

    // Emit eh.push in a dedicated try-entry block to avoid attributing inner TRY
    // coverage to the parent line block. This also creates a clean structural
    // region for post-dominator checks.
    func = ctx.function();
    std::string tryEntryLbl =
        blockNamer ? blockNamer->generic("try_entry") : mangler.block("try_entry");
    builder->addBlock(*func, tryEntryLbl);
    BasicBlock *tryEntry = &func->blocks.back();
    // Branch from the original current block to the try-entry block.
    ctx.setCurrentByIndex(curIdx);
    emitBr(tryEntry);
    // Start TRY region in the new block.
    ctx.setCurrent(tryEntry);
    // Compute handler label deterministically and emit eh.push by label to avoid
    // dangling block pointers across vector reallocations.
    std::string handlerLabel = preHandlerLabel;
    {
        Instr in;
        in.op = Opcode::EhPush;
        in.type = Type(Type::Kind::Void);
        in.addBranchTarget(handlerLabel);
        in.loc = curLoc;
        BasicBlock *block = ctx.current();
        if (block)
            block->instructions.push_back(std::move(in));
    }
    for (const auto &st : stmt.tryBody) {
        if (!st)
            continue;
        lowerStmt(*st);
        BasicBlock *cur = ctx.current();
        if (!cur || cur->terminated)
            break;
    }

    // On the normal path, pop the handler and branch to continuation.
    // If we have finally, branch to finally_normal; otherwise branch to after_try.
    if (ctx.current() && !ctx.current()->terminated) {
        func = ctx.function();
        afterTry = &func->blocks[afterIdx];
        emitEhPop();

        if (hasFinally) {
            BasicBlock *finallyNormal = &func->blocks[finallyNormalIdx];
            emitBr(finallyNormal);
        } else {
            emitBr(afterTry);
        }
    }

    // Lower finally_normal block: finally statements then branch to after_try.
    if (hasFinally) {
        func = ctx.function();
        BasicBlock *finallyNormal = &func->blocks[finallyNormalIdx];
        ctx.setCurrent(finallyNormal);

        for (const auto &st : stmt.finallyBody) {
            if (!st)
                continue;
            lowerStmt(*st);
            BasicBlock *cur = ctx.current();
            if (!cur || cur->terminated)
                break;
        }

        // Branch to after_try if not already terminated.
        if (ctx.current() && !ctx.current()->terminated) {
            func = ctx.function();
            afterTry = &func->blocks[afterIdx];
            emitBr(afterTry);
        }
    }

    // Switch insertion to the handler to lower the catch body.
    func = ctx.function();
    BasicBlock *handlerBlock = ensureErrorHandlerBlock(handlerKey);
    ctx.setCurrent(handlerBlock);

    // Lower the catch body statements (if any).
    for (const auto &st : stmt.catchBody) {
        if (!st)
            continue;
        lowerStmt(*st);
        BasicBlock *cur = ctx.current();
        if (!cur || cur->terminated)
            break;
    }

    // Lower the finally body in the handler path (duplicated from normal path).
    // This ensures finally runs even when an exception was caught.
    if (hasFinally && ctx.current() && !ctx.current()->terminated) {
        for (const auto &st : stmt.finallyBody) {
            if (!st)
                continue;
            lowerStmt(*st);
            BasicBlock *cur = ctx.current();
            if (!cur || cur->terminated)
                break;
        }
    }

    // Terminate handler with resume.label to after_try if not already terminated.
    handlerBlock = ctx.current();
    if (handlerBlock && !handlerBlock->terminated) {
        // Find the original handler block to get the %tok parameter.
        func = ctx.function();
        BasicBlock *origHandler = ensureErrorHandlerBlock(handlerKey);
        if (origHandler && origHandler->params.size() >= 2) {
            // Refresh after_try pointer before emitting the terminator.
            afterTry = &func->blocks[afterIdx];
            builder->setInsertPoint(*handlerBlock);
            Value resumeTok2 = Value::temp(origHandler->params[1].id);
            builder->emitResumeLabel(resumeTok2, *afterTry, stmt.loc);
            handlerBlock->terminated = true;
        }
    }

    // Continue lowering at the after_try block.
    func = ctx.function();
    afterTry = &func->blocks[afterIdx];
    ctx.setCurrent(afterTry);
}

/// @brief Lower a USING resource statement into cleanup with destruction.
///
/// The generated IL installs a scoped exception handler around the body. Normal
/// fallthrough pops the handler and runs cleanup. Exception flow enters the
/// synthetic handler, runs the same cleanup, then resumes the original exception
/// token so outer handlers still observe the failure.
/// @param stmt USING statement whose resource, body, and cleanup are lowered.
void Lowerer::lowerUsingStmt(const UsingStmt &stmt) {
    ProcedureContext &ctx = context();
    Function *func = ctx.function();
    BasicBlock *current = ctx.current();
    if (!func || !current)
        return;

    curLoc = stmt.loc;

    // Step 1: Build the class name from qualified type
    std::string className;
    for (size_t i = 0; i < stmt.typeQualified.size(); ++i) {
        if (i > 0)
            className += ".";
        className += stmt.typeQualified[i];
    }

    // Step 2: Lower the initialization expression and store in the variable
    // The variable storage should already be allocated by semantic analysis
    auto storage = resolveVariableStorage(stmt.varName, stmt.loc);
    if (!storage) {
        // Variable not found - bail out
        return;
    }

    Value objPtr;
    if (stmt.initExpr) {
        RVal initVal = lowerExpr(*stmt.initExpr);
        objPtr = initVal.value;
    } else {
        // No initializer - use null pointer
        objPtr = Value::null();
    }

    // Store the object pointer in the variable's slot
    emitStore(Type(Type::Kind::Ptr), storage->pointer, objPtr);

    const std::size_t usingEntryIdx = ctx.currentIndex();
    BlockNamer *blockNamer = ctx.blockNames().namer();

    func = ctx.function();
    const size_t handlerIdx = func->blocks.size();
    std::string handlerLbl =
        blockNamer ? blockNamer->generic("using_handler") : mangler.block("using_handler");
    std::vector<il::core::Param> handlerParams = {{"err", Type(Type::Kind::Error)},
                                                  {"tok", Type(Type::Kind::ResumeTok)}};
    BasicBlock &handler = builder->createBlock(*func, handlerLbl, handlerParams);
    Instr entry;
    entry.op = Opcode::EhEntry;
    entry.type = Type(Type::Kind::Void);
    entry.loc = stmt.loc;
    handler.instructions.push_back(entry);
    const std::string handlerLabel = handler.label;

    /// @brief Invokes user cleanup and releases a USING resource.
    /// @param loadedObj Loaded object pointer to destroy and free.
    auto emitResourceDestroy = [&](Value loadedObj) {
        if (!className.empty()) {
            OopLoweringContext oopCtx(*this, oopIndex_);
            // Qualify the class name for lookup
            std::string qualifiedName = oopCtx.qualify(className);
            // Check if class has a SUB DESTROY() method (not same as DESTRUCTOR keyword)
            // The DESTRUCTOR keyword's code is folded into __dtor, but SUB DESTROY() is separate
            if (oopIndex_.findMethod(qualifiedName, "DESTROY") != nullptr) {
                // Call the user's DESTROY method
                std::string destroyName = oopCtx.getMethodName(qualifiedName, "DESTROY");
                emitCall(destroyName, {loadedObj});
            }
            // Always call __dtor for field cleanup and DESTRUCTOR keyword code
            std::string dtorName = oopCtx.getDestructorName(qualifiedName);
            if (!dtorName.empty()) {
                emitCall(dtorName, {loadedObj});
            }
        }
        emitCall("rt_obj_free", {loadedObj});
    };

    /// @brief Emits normal-path reference release and conditional resource destruction.
    auto emitUsingCleanup = [&]() {
        if (!ctx.current() || ctx.current()->terminated)
            return;
        const std::size_t originIdx = ctx.currentIndex();

        // Load the object pointer
        Value loadedObj = emitLoad(Type(Type::Kind::Ptr), storage->pointer);

        requestHelper(RuntimeFeature::ObjReleaseChk0);
        requestHelper(RuntimeFeature::ObjFree);

        Value shouldDestroy = emitCallRet(ilBoolTy(), "rt_obj_release_check0", {loadedObj});

        // Create destroy and continue blocks
        func = ctx.function();
        BlockNamer *cleanupNamer = ctx.blockNames().namer();

        const size_t destroyIdx = func->blocks.size();
        std::string destroyLbl =
            cleanupNamer ? cleanupNamer->generic("using_dtor") : mangler.block("using_dtor");
        builder->addBlock(*func, destroyLbl);

        func = ctx.function();
        const size_t contIdx = func->blocks.size();
        std::string contLbl =
            cleanupNamer ? cleanupNamer->generic("using_cont") : mangler.block("using_cont");
        builder->addBlock(*func, contLbl);

        func = ctx.function();
        BasicBlock *destroyBlk = &func->blocks[destroyIdx];
        BasicBlock *contBlk = &func->blocks[contIdx];

        ctx.setCurrentByIndex(originIdx);
        emitCBr(shouldDestroy, destroyBlk, contBlk);

        // Destroy block: call destructor if available, then free
        ctx.setCurrent(destroyBlk);
        curLoc = stmt.loc;

        emitResourceDestroy(loadedObj);
        emitBr(contBlk);

        // Continue at contBlk
        ctx.setCurrent(contBlk);

        // Set variable to null to prevent double-free in function epilogue
        emitStore(Type(Type::Kind::Ptr), storage->pointer, Value::null());
    };

    /// @brief Builds the standard error and resume-token handler parameters.
    /// @return Fresh parameter vector for an exception handler block.
    auto makeHandlerParams = []() {
        return std::vector<il::core::Param>{{"err", Type(Type::Kind::Error)},
                                            {"tok", Type(Type::Kind::ResumeTok)}};
    };

    /// @brief Appends an exception-handler entry marker to a block.
    /// @param block Handler block to update.
    auto appendEhEntry = [&](BasicBlock &block) {
        Instr entryInstr;
        entryInstr.op = Opcode::EhEntry;
        entryInstr.type = Type(Type::Kind::Void);
        entryInstr.loc = stmt.loc;
        block.instructions.push_back(std::move(entryInstr));
    };

    /// @brief Emits a branch carrying handler values.
    /// @param target Destination block.
    /// @param args Branch arguments transferred to the destination parameters.
    auto emitBrWithArgs = [&](BasicBlock *target, std::vector<Value> args) {
        BasicBlock *block = ctx.current();
        if (!block || !target)
            return;
        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.addBranchTarget(target->label, std::move(args));
        br.loc = stmt.loc;
        block->instructions.push_back(std::move(br));
        block->terminated = true;
    };

    /// @brief Emits a conditional branch carrying values on both edges.
    /// @param cond Boolean branch condition.
    /// @param trueTarget Destination for the true edge.
    /// @param trueArgs Arguments carried on the true edge.
    /// @param falseTarget Destination for the false edge.
    /// @param falseArgs Arguments carried on the false edge.
    auto emitCBrWithArgs = [&](Value cond,
                               BasicBlock *trueTarget,
                               std::vector<Value> trueArgs,
                               BasicBlock *falseTarget,
                               std::vector<Value> falseArgs) {
        BasicBlock *block = ctx.current();
        if (!block || !trueTarget || !falseTarget)
            return;
        Instr cbr;
        cbr.op = Opcode::CBr;
        cbr.type = Type(Type::Kind::Void);
        cbr.operands.push_back(cond);
        cbr.addBranchTarget(trueTarget->label, std::move(trueArgs));
        cbr.addBranchTarget(falseTarget->label, std::move(falseArgs));
        cbr.loc = stmt.loc;
        block->instructions.push_back(std::move(cbr));
        block->terminated = true;
    };

    /// @brief Emits exception-path USING cleanup while preserving handler values.
    auto emitUsingHandlerCleanup = [&]() {
        BasicBlock *handlerBlock = ctx.current();
        if (!handlerBlock || handlerBlock->terminated || handlerBlock->params.size() < 2)
            return;

        const std::size_t originIdx = ctx.currentIndex();
        Value errArg = Value::temp(handlerBlock->params[0].id);
        Value tokArg = Value::temp(handlerBlock->params[1].id);

        Value loadedObj = emitLoad(Type(Type::Kind::Ptr), storage->pointer);

        requestHelper(RuntimeFeature::ObjReleaseChk0);
        requestHelper(RuntimeFeature::ObjFree);

        Value shouldDestroy = emitCallRet(ilBoolTy(), "rt_obj_release_check0", {loadedObj});

        func = ctx.function();
        BlockNamer *cleanupNamer = ctx.blockNames().namer();

        const size_t destroyIdx = func->blocks.size();
        std::string destroyLbl =
            cleanupNamer ? cleanupNamer->generic("using_dtor") : mangler.block("using_dtor");
        builder->createBlock(*func, destroyLbl, makeHandlerParams());
        appendEhEntry(func->blocks[destroyIdx]);

        func = ctx.function();
        const size_t contIdx = func->blocks.size();
        std::string contLbl =
            cleanupNamer ? cleanupNamer->generic("using_cont") : mangler.block("using_cont");
        builder->createBlock(*func, contLbl, makeHandlerParams());
        appendEhEntry(func->blocks[contIdx]);

        func = ctx.function();
        BasicBlock *destroyBlk = &func->blocks[destroyIdx];
        BasicBlock *contBlk = &func->blocks[contIdx];

        ctx.setCurrentByIndex(originIdx);
        emitCBrWithArgs(shouldDestroy, destroyBlk, {errArg, tokArg}, contBlk, {errArg, tokArg});

        ctx.setCurrent(destroyBlk);
        curLoc = stmt.loc;
        Value destroyErr = Value::temp(destroyBlk->params[0].id);
        Value destroyTok = Value::temp(destroyBlk->params[1].id);
        emitResourceDestroy(loadedObj);
        emitBrWithArgs(contBlk, {destroyErr, destroyTok});

        ctx.setCurrent(contBlk);
        emitStore(Type(Type::Kind::Ptr), storage->pointer, Value::null());

        Instr resume;
        resume.op = Opcode::ResumeSame;
        resume.type = Type(Type::Kind::Void);
        resume.operands.push_back(Value::temp(contBlk->params[1].id));
        resume.loc = stmt.loc;
        contBlk->instructions.push_back(std::move(resume));
        contBlk->terminated = true;
    };

    ctx.setCurrentByIndex(usingEntryIdx);
    {
        Instr in;
        in.op = Opcode::EhPush;
        in.type = Type(Type::Kind::Void);
        in.addBranchTarget(handlerLabel);
        in.loc = stmt.loc;
        BasicBlock *block = ctx.current();
        if (block)
            block->instructions.push_back(std::move(in));
    }

    // Step 3: Lower body statements
    for (const auto &st : stmt.body) {
        if (!st)
            continue;
        lowerStmt(*st);
        BasicBlock *cur = ctx.current();
        if (!cur || cur->terminated)
            break;
    }

    std::size_t normalContIdx = 0;
    bool hasNormalCont = false;

    // Step 4: Normal cleanup - pop the scoped handler, then destroy the resource.
    if (ctx.current() && !ctx.current()->terminated) {
        emitEhPop();
        emitUsingCleanup();
        if (ctx.current() && !ctx.current()->terminated) {
            normalContIdx = ctx.currentIndex();
            hasNormalCont = true;
        }
    }

    // Step 5: Exceptional cleanup - destroy the resource, then resume the same exception.
    func = ctx.function();
    if (handlerIdx < func->blocks.size()) {
        ctx.setCurrent(&func->blocks[handlerIdx]);
        emitUsingHandlerCleanup();
    }

    if (hasNormalCont && normalContIdx < ctx.function()->blocks.size())
        ctx.setCurrentByIndex(normalContIdx);
}

} // namespace il::frontends::basic
