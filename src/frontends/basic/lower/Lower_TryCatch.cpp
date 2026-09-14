//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/Lower_TryCatch.cpp
// Purpose: Lower BASIC ON ERROR, RESUME, TRY/CATCH, and USING constructs on the IL
//          exception-handling model.
// Key invariants:
//   - A procedure with ON ERROR GOTO <label> has one dispatcher handler, pushed only
//     by its arm block. The entry, the handler entry, and every RESUME reach the arm
//     with the handler removed, so the handler stack has the same depth on every
//     path through the body and the arm dominates all protected code.
//   - ON ERROR GOTO only stores which label is selected; the handler entry records
//     the error, consumes the resume token, and returns through the arm to that label.
//   - An error raised while the handler runs, or with no label selected, is raised
//     again with the dispatcher removed, so it reaches the caller unchanged.
//   - Resume sites are recorded only where the arm may enter: never inside TRY or
//     USING statements, whose bodies run with another handler or hold a token.
//   - A USING variable owns one reference to its resource, released by the
//     statement's cleanup on both the normal and the exception path.
// Ownership/Lifetime:
//   - Routines borrow the @ref Lowerer state and update handler state owned by
//     @ref ProcedureContext.
// Links: docs/specs/errors.md, docs/adr/0005-resume-token-provenance.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements ON ERROR/RESUME, TRY/CATCH, and USING lowering for BASIC.
/// @details The ON ERROR dispatcher uses these blocks:
/// @code
/// entry:           slots zeroed; br ^onerr_arm
/// onerr_arm:       eh.push ^onerr_handler; switch.i32 target -> body | chosen block
/// onerr_handler:   running ? ^onerr_rethrow : ^onerr_select
/// onerr_select:    selected == 0 ? ^onerr_rethrow : ^onerr_enter
/// onerr_enter:     store kind/code/line, running = 1, failed site = site,
///                  target = selected; resume.label %tok, ^onerr_arm
/// onerr_rethrow:   Zanna.Runtime.Unsafe.RaiseKind(kind, code, line)
/// RESUME:          running = 0; target = resume dispatch | label; eh.pop; br ^onerr_arm
/// resume_dispatch: switch.i32 failed site -> statement start (or the block after it)
/// @endcode

#include "frontends/basic/AstWalker.hpp"
#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/OopIndex.hpp"
#include "frontends/basic/OopLoweringContext.hpp"
#include "frontends/basic/lower/Emitter.hpp"

using namespace il::core;

namespace il::frontends::basic {

namespace {

/// @brief Finds the ON ERROR and RESUME forms a procedure body uses.
/// @details Nested procedure, class, interface, and namespace declarations are
///          lowered as procedures of their own, so their bodies are not scanned.
struct ErrorHandlingScan final : BasicAstWalker<ErrorHandlingScan> {
    /// True when ON ERROR GOTO <label> occurs.
    bool onErrorLabel = false;
    /// True when RESUME or RESUME NEXT occurs.
    bool resumeStatement = false;

    /// @brief Note an ON ERROR directive.
    /// @param stmt Directive being visited.
    void before(const OnErrorGoto &stmt) {
        if (!stmt.toZero)
            onErrorLabel = true;
    }

    /// @brief Note a RESUME statement.
    /// @param stmt Statement being visited.
    void before(const Resume &stmt) {
        if (stmt.mode != Resume::Mode::Label)
            resumeStatement = true;
    }

    /// @brief Skip nested FUNCTION bodies.
    bool shouldVisitChildren(const FunctionDecl &) {
        return false;
    }

    /// @brief Skip nested SUB bodies.
    bool shouldVisitChildren(const SubDecl &) {
        return false;
    }

    /// @brief Skip class members.
    bool shouldVisitChildren(const ClassDecl &) {
        return false;
    }

    /// @brief Skip interface members.
    bool shouldVisitChildren(const InterfaceDecl &) {
        return false;
    }

    /// @brief Skip namespace members.
    bool shouldVisitChildren(const NamespaceDecl &) {
        return false;
    }
};

/// @brief Error and resume-token parameters of a handler-shaped block as branch arguments.
/// @param block Block whose first two parameters are the error and the token.
/// @return Branch arguments forwarding both parameters.
std::vector<Value> handlerArgs(const BasicBlock &block) {
    return {Value::temp(block.params[0].id), Value::temp(block.params[1].id)};
}

/// @brief Terminate @p block with `switch.i32`.
/// @param block Block receiving the terminator.
/// @param scrutinee i32 value switched on.
/// @param defaultLabel Label taken when no case matches.
/// @param cases Case values and their target labels.
void appendSwitch(BasicBlock &block,
                  Value scrutinee,
                  const std::string &defaultLabel,
                  const std::vector<std::pair<unsigned, std::string>> &cases) {
    Instr sw;
    sw.op = Opcode::SwitchI32;
    sw.type = il::core::Type(il::core::Type::Kind::Void);
    sw.operands.push_back(scrutinee);
    sw.addBranchTarget(defaultLabel);
    for (const auto &[value, label] : cases) {
        sw.operands.push_back(Value::constInt(static_cast<long long>(value)));
        sw.addBranchTarget(label);
    }
    block.instructions.push_back(std::move(sw));
    block.terminated = true;
}

} // namespace

/// @brief Append a labelled block to the active function, keeping the current block.
/// @param hint Label stem.
/// @return Index of the new block.
size_t Lowerer::addErrorBlock(const char *hint) {
    auto &ctx = context();
    Function *func = ctx.function();
    const bool hasCurrent = ctx.current() != nullptr;
    const size_t curIdx = hasCurrent ? ctx.currentIndex() : 0;
    std::string label;
    if (auto *blockNamer = ctx.blockNames().namer())
        label = blockNamer->generic(hint);
    else
        label = mangler.block(hint);
    const size_t idx = func->blocks.size();
    builder->addBlock(*func, label);
    if (hasCurrent)
        ctx.setCurrentByIndex(curIdx);
    return idx;
}

/// @brief Append a handler-shaped block, keeping the current block.
/// @details The block takes `(%err:Error, %tok:ResumeTok)` and starts with
///          `eh.entry`, so it may receive a forwarded resume token and use it.
/// @param hint Label stem.
/// @return Index of the new block.
size_t Lowerer::addErrorHandlerBlock(const char *hint) {
    auto &ctx = context();
    Function *func = ctx.function();
    const bool hasCurrent = ctx.current() != nullptr;
    const size_t curIdx = hasCurrent ? ctx.currentIndex() : 0;
    std::string label;
    if (auto *blockNamer = ctx.blockNames().namer())
        label = blockNamer->generic(hint);
    else
        label = mangler.block(hint);
    const std::vector<il::core::Param> params = {{"err", Type(Type::Kind::Error)},
                                                 {"tok", Type(Type::Kind::ResumeTok)}};
    const size_t idx = func->blocks.size();
    BasicBlock &block = builder->createBlock(*func, label, params);
    Instr entry;
    entry.op = Opcode::EhEntry;
    entry.type = Type(Type::Kind::Void);
    block.instructions.push_back(std::move(entry));
    if (hasCurrent)
        ctx.setCurrentByIndex(curIdx);
    return idx;
}

/// @brief Emit a trap that reports @p message in the current block.
/// @param message Trap text.
void Lowerer::emitTrapWithMessage(const char *message) {
    requireTrap();
    Value text = emitConstStr(getStringLabel(message));
    emitCall("rt_trap_string", {text});
    emitTrap();
}

/// @brief Raise an error with the given kind, code, and line, ending the current block.
/// @details The runtime supplies the kind's message, as `Zanna.Error.Message` reports it.
/// @param kind i32 trap kind.
/// @param code i32 error code.
/// @param line i32 source line.
void Lowerer::emitRaiseError(Value kind, Value code, Value line) {
    emitCall("Zanna.Runtime.Unsafe.RaiseKind", {kind, code, line});
    emitTrap();
}

/// @brief Continue at arm target @p targetId through the arm, which re-installs the handler.
/// @param targetId Arm target id from ErrorHandlerState::armTarget.
void Lowerer::emitReenterArm(unsigned targetId) {
    ProcedureContext &ctx = context();
    auto &state = ctx.errorHandlers();
    emitStore(Type(Type::Kind::I32), state.slots().target, Value::constInt(targetId));
    emitEhPop();
    emitBr(&ctx.function()->blocks[state.armBlock()]);
}

/// @brief Set up ON ERROR handling before a procedure body is lowered.
/// @details A body that contains ON ERROR GOTO <label> gets the dispatcher: its
///          slots, its handler block, and the arm block that installs the handler.
///          The current block branches to the arm, whose switch (emitted by
///          @ref finalizeErrorHandling) continues at @p bodyBlock. A body that also
///          contains RESUME or RESUME NEXT records a resume site for each statement.
/// @param stmts Body statements.
/// @param bodyBlock Index of the block the body starts in.
/// @return True when the dispatcher was created.
bool Lowerer::prepareErrorHandling(const std::vector<const Stmt *> &stmts, size_t bodyBlock) {
    ProcedureContext &ctx = context();
    auto &state = ctx.errorHandlers();
    if (state.hasDispatcher() || !ctx.function() || !ctx.current() || ctx.current()->terminated)
        return false;

    ErrorHandlingScan scan;
    for (const Stmt *stmt : stmts)
        if (stmt)
            scan.walkStmt(*stmt);
    if (!scan.onErrorLabel)
        return false;

    const auto savedLoc = curLoc;
    curLoc = {};
    const auto slots = allocateDispatchSlots();
    const size_t handlerIdx = addErrorHandlerBlock("onerr_handler");
    const size_t armIdx = addErrorBlock("onerr_arm");
    state.setDispatcher(slots, armIdx, handlerIdx, bodyBlock);
    state.setSiteTracking(scan.resumeStatement);

    emitBr(&ctx.function()->blocks[armIdx]);
    ctx.setCurrentByIndex(armIdx);
    emitEhPush(&ctx.function()->blocks[handlerIdx]);
    curLoc = savedLoc;
    return true;
}

/// @brief Complete the dispatcher once the procedure body has been lowered.
/// @details Builds the handler entry chain, terminates the arm with its switch over
///          the arm targets, and fills the RESUME and RESUME NEXT dispatch blocks,
///          which switch on the failed statement's site.
void Lowerer::finalizeErrorHandling() {
    ProcedureContext &ctx = context();
    auto &state = ctx.errorHandlers();
    Function *func = ctx.function();
    if (!func || !state.hasDispatcher())
        return;
    const size_t armIdx = state.armBlock();
    const size_t handlerIdx = state.handlerBlock();
    if (armIdx >= func->blocks.size() || func->blocks[armIdx].terminated)
        return;

    const bool hadCurrent = ctx.current() != nullptr;
    const size_t savedIdx = hadCurrent ? ctx.currentIndex() : 0;
    const auto savedLoc = curLoc;
    curLoc = {};
    const auto slots = state.slots();
    const Type i32(Type::Kind::I32);
    const Type i64(Type::Kind::I64);

    const size_t selectIdx = addErrorHandlerBlock("onerr_select");
    const size_t enterIdx = addErrorHandlerBlock("onerr_enter");
    const size_t rethrowIdx = addErrorHandlerBlock("onerr_rethrow");

    // An error raised while the handler runs is not handled again.
    ctx.setCurrentByIndex(handlerIdx);
    Value running = emitLoad(i64, slots.running);
    Value busy = emitBinary(Opcode::ICmpNe, ilBoolTy(), running, Value::constInt(0));
    func = ctx.function();
    builder->setInsertPoint(func->blocks[handlerIdx]);
    builder->cbr(busy,
                 func->blocks[rethrowIdx],
                 handlerArgs(func->blocks[handlerIdx]),
                 func->blocks[selectIdx],
                 handlerArgs(func->blocks[handlerIdx]));

    // Without a selected label the error is not handled here.
    ctx.setCurrentByIndex(selectIdx);
    Value selected = emitLoad(i32, slots.selected);
    func = ctx.function();
    {
        BasicBlock &select = func->blocks[selectIdx];
        Instr sw;
        sw.op = Opcode::SwitchI32;
        sw.type = Type(Type::Kind::Void);
        sw.operands.push_back(selected);
        sw.addBranchTarget(func->blocks[enterIdx].label, handlerArgs(select));
        sw.operands.push_back(Value::constInt(0));
        sw.addBranchTarget(func->blocks[rethrowIdx].label, handlerArgs(select));
        select.instructions.push_back(std::move(sw));
        select.terminated = true;
    }

    // Record the error and the failed statement, then enter the selected label.
    ctx.setCurrentByIndex(enterIdx);
    func = ctx.function();
    Value enterErr = Value::temp(func->blocks[enterIdx].params[0].id);
    Value kind = emitUnary(Opcode::ErrGetKind, i32, enterErr);
    emitStore(i32, slots.kind, kind);
    Value code = emitUnary(Opcode::ErrGetCode, i32, enterErr);
    emitStore(i32, slots.code, code);
    Value line = emitUnary(Opcode::ErrGetLine, i32, enterErr);
    emitStore(i32, slots.line, line);
    emitStore(i64, slots.running, Value::constInt(1));
    Value site = emitLoad(i32, slots.site);
    emitStore(i32, slots.failedSite, site);
    Value chosen = emitLoad(i32, slots.selected);
    emitStore(i32, slots.target, chosen);
    func = ctx.function();
    builder->setInsertPoint(func->blocks[enterIdx]);
    builder->emitResumeLabel(
        Value::temp(func->blocks[enterIdx].params[1].id), func->blocks[armIdx], {});

    // Raise the error again; the dispatcher is no longer installed.
    ctx.setCurrentByIndex(rethrowIdx);
    func = ctx.function();
    Value rethrowErr = Value::temp(func->blocks[rethrowIdx].params[0].id);
    Value rethrowKind = emitUnary(Opcode::ErrGetKind, i32, rethrowErr);
    Value rethrowCode = emitUnary(Opcode::ErrGetCode, i32, rethrowErr);
    Value rethrowLine = emitUnary(Opcode::ErrGetLine, i32, rethrowErr);
    emitRaiseError(rethrowKind, rethrowCode, rethrowLine);

    // RESUME and RESUME NEXT continue at the failed statement or the block after it.
    for (const bool next : {false, true}) {
        const auto dispatchIdx = state.dispatchBlock(next);
        func = ctx.function();
        if (!dispatchIdx || *dispatchIdx >= func->blocks.size() ||
            func->blocks[*dispatchIdx].terminated)
            continue;

        const size_t invalidIdx = addErrorBlock(next ? "resume_next_invalid" : "resume_invalid");
        ctx.setCurrentByIndex(invalidIdx);
        emitTrapWithMessage(next ? "RESUME NEXT: no failed statement to continue after"
                                 : "RESUME: no failed statement to retry");

        ctx.setCurrentByIndex(*dispatchIdx);
        Value failed = emitLoad(i32, slots.failedSite);
        func = ctx.function();
        std::vector<std::pair<unsigned, std::string>> cases;
        const auto &sites = state.sites();
        for (size_t i = 0; i < sites.size(); ++i) {
            const std::optional<size_t> target =
                next ? sites[i].nextBlock : std::optional<size_t>(sites[i].startBlock);
            if (target && *target < func->blocks.size())
                cases.emplace_back(static_cast<unsigned>(i + 1), func->blocks[*target].label);
        }
        appendSwitch(func->blocks[*dispatchIdx], failed, func->blocks[invalidIdx].label, cases);
    }

    // The arm continues at the chosen target, or starts the body.
    ctx.setCurrentByIndex(armIdx);
    Value target = emitLoad(i32, slots.target);
    func = ctx.function();
    std::vector<std::pair<unsigned, std::string>> armCases;
    const auto &targets = state.armTargets();
    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i] < func->blocks.size())
            armCases.emplace_back(static_cast<unsigned>(i + 1), func->blocks[targets[i]].label);
    }
    appendSwitch(func->blocks[armIdx], target, func->blocks[state.bodyBlock()].label, armCases);

    curLoc = savedLoc;
    if (hadCurrent)
        ctx.setCurrentByIndex(savedIdx);
    else
        ctx.setCurrent(nullptr);
}

/// @brief Lower an @c ON @c ERROR directive.
///
/// @details `ON ERROR GOTO <label>` selects the label the dispatcher enters on the
///          next error. `ON ERROR GOTO 0` clears the selection; inside a running
///          handler it instead raises the error being handled, as BASIC reports it.
/// @param stmt AST node describing the ON ERROR directive.
void Lowerer::lowerOnErrorGoto(const OnErrorGoto &stmt) {
    ProcedureContext &ctx = context();
    auto &state = ctx.errorHandlers();
    BasicBlock *current = ctx.current();
    if (!ctx.function() || !current || current->terminated || !state.hasDispatcher())
        return;

    curLoc = stmt.loc;
    const auto slots = state.slots();
    const Type i32(Type::Kind::I32);

    if (stmt.toZero) {
        const size_t reportIdx = addErrorBlock("onerr_report");
        const size_t clearIdx = addErrorBlock("onerr_clear");
        Value running = emitLoad(Type(Type::Kind::I64), slots.running);
        Value busy = emitBinary(Opcode::ICmpNe, ilBoolTy(), running, Value::constInt(0));
        emitCBr(busy, &ctx.function()->blocks[reportIdx], &ctx.function()->blocks[clearIdx]);

        ctx.setCurrentByIndex(reportIdx);
        Value kind = emitLoad(i32, slots.kind);
        Value code = emitLoad(i32, slots.code);
        Value line = emitLoad(i32, slots.line);
        emitRaiseError(kind, code, line);

        ctx.setCurrentByIndex(clearIdx);
        emitStore(i32, slots.selected, Value::constInt(0));
        return;
    }

    auto &lineBlocks = ctx.blockNames().lineBlocks();
    const auto targetIt = lineBlocks.find(stmt.target);
    if (targetIt == lineBlocks.end()) {
        emitTrapWithMessage("ON ERROR GOTO: unknown label");
        return;
    }
    emitStore(i32, slots.selected, Value::constInt(state.armTarget(targetIt->second)));
}

/// @brief Start the resume site of a statement in a procedure that uses RESUME.
/// @details Declarations, labels, statement lists (their children record their own
///          sites), ON ERROR, and control transfers that cannot fail record no site.
///          Otherwise the statement begins in a block of its own, splitting the
///          current block when it already holds code or parameters, and that block
///          first stores the site id so the handler knows which statement failed.
///          RESUME records one too: "RESUME without error" is an error of its own.
/// @param stmt Statement about to be lowered.
/// @return Site id, or nothing when no site is recorded.
std::optional<unsigned> Lowerer::beginResumeSite(const Stmt &stmt) {
    ProcedureContext &ctx = context();
    auto &state = ctx.errorHandlers();
    if (!state.hasDispatcher() || !state.siteTracking())
        return std::nullopt;

    switch (stmt.stmtKind()) {
        case Stmt::Kind::Label:
        case Stmt::Kind::Const:
        case Stmt::Kind::Shared:
        case Stmt::Kind::StmtList:
        case Stmt::Kind::OnErrorGoto:
        case Stmt::Kind::Exit:
        case Stmt::Kind::Goto:
        case Stmt::Kind::End:
        case Stmt::Kind::Next:
        case Stmt::Kind::FunctionDecl:
        case Stmt::Kind::SubDecl:
        case Stmt::Kind::ConstructorDecl:
        case Stmt::Kind::DestructorDecl:
        case Stmt::Kind::MethodDecl:
        case Stmt::Kind::PropertyDecl:
        case Stmt::Kind::ClassDecl:
        case Stmt::Kind::TypeDecl:
        case Stmt::Kind::EnumDecl:
        case Stmt::Kind::InterfaceDecl:
        case Stmt::Kind::NamespaceDecl:
        case Stmt::Kind::UsingDecl:
            return std::nullopt;
        default:
            break;
    }

    Function *func = ctx.function();
    BasicBlock *current = ctx.current();
    if (!func || !current || current->terminated)
        return std::nullopt;

    size_t startIdx = ctx.currentIndex();
    if (!current->instructions.empty() || !current->params.empty()) {
        const size_t idx = addErrorBlock("resume_site");
        curLoc = {};
        emitBr(&ctx.function()->blocks[idx]);
        ctx.setCurrentByIndex(idx);
        startIdx = idx;
    }

    const unsigned site = state.addSite(startIdx);
    curLoc = stmt.loc;
    emitStore(Type(Type::Kind::I32), state.slots().site, Value::constInt(site));
    return site;
}

/// @brief Close a resume site, recording where RESUME NEXT continues.
/// @details Lowering continues in a fresh block, which is the RESUME NEXT target. A
///          statement that falls through branches to it; after one that ends its
///          block (a RETURN, or RESUME itself) it is reached only by RESUME NEXT,
///          which then goes on with the following statement.
/// @param site Site id returned by @ref beginResumeSite.
void Lowerer::endResumeSite(unsigned site) {
    ProcedureContext &ctx = context();
    BasicBlock *current = ctx.current();
    if (!ctx.function() || !current)
        return;

    const size_t idx = addErrorBlock("resume_next");
    if (!ctx.current()->terminated) {
        curLoc = {};
        emitBr(&ctx.function()->blocks[idx]);
    }
    ctx.setCurrentByIndex(idx);
    ctx.errorHandlers().setSiteNext(site, idx);
}

/// @brief Get or create the block that RESUME or RESUME NEXT continues at.
/// @param next True for RESUME NEXT.
/// @return Index of the dispatch block, filled in by @ref finalizeErrorHandling.
size_t Lowerer::resumeDispatchBlock(bool next) {
    auto &state = context().errorHandlers();
    if (auto existing = state.dispatchBlock(next))
        return *existing;
    const size_t idx = addErrorBlock(next ? "resume_next_dispatch" : "resume_dispatch");
    state.setDispatchBlock(next, idx);
    return idx;
}

/// @brief Lower a RESUME statement.
///
/// @details RESUME is valid only while the ON ERROR handler runs; otherwise it
///          raises "RESUME without error". It clears the running flag and ERR, then
///          returns through the arm: `RESUME` to the start of the failed statement,
///          `RESUME NEXT` to the block after it, and `RESUME <label>` to the label.
/// @param stmt AST node describing the RESUME statement.
void Lowerer::lowerResume(const Resume &stmt) {
    ProcedureContext &ctx = context();
    BasicBlock *current = ctx.current();
    if (!ctx.function() || !current || current->terminated)
        return;

    curLoc = stmt.loc;
    auto &state = ctx.errorHandlers();
    if (!state.hasDispatcher()) {
        emitTrapWithMessage("RESUME without error");
        return;
    }
    const auto slots = state.slots();

    const size_t withoutIdx = addErrorBlock("resume_without_error");
    const size_t okIdx = addErrorBlock("resume_ok");
    Value running = emitLoad(Type(Type::Kind::I64), slots.running);
    Value idle = emitBinary(Opcode::ICmpEq, ilBoolTy(), running, Value::constInt(0));
    emitCBr(idle, &ctx.function()->blocks[withoutIdx], &ctx.function()->blocks[okIdx]);

    ctx.setCurrentByIndex(withoutIdx);
    emitTrapWithMessage("RESUME without error");

    ctx.setCurrentByIndex(okIdx);
    emitStore(Type(Type::Kind::I64), slots.running, Value::constInt(0));
    emitStore(Type(Type::Kind::I32), slots.code, Value::constInt(0));

    unsigned target = 0;
    switch (stmt.mode) {
        case Resume::Mode::Label: {
            auto &lineBlocks = ctx.blockNames().lineBlocks();
            const auto targetIt = lineBlocks.find(stmt.target);
            if (targetIt == lineBlocks.end()) {
                emitTrapWithMessage("RESUME: unknown label");
                return;
            }
            target = state.armTarget(targetIt->second);
            break;
        }
        case Resume::Mode::Same:
        case Resume::Mode::Next:
            target = state.armTarget(resumeDispatchBlock(stmt.mode == Resume::Mode::Next));
            break;
    }
    emitReenterArm(target);
}

/// @brief Lower a TRY/CATCH/FINALLY statement using the runtime EH model.
///
/// Interaction with ON ERROR/RESUME:
/// - TRY installs its own handler with `eh.push`/`eh.pop` on top of the procedure's
///   ON ERROR dispatcher, which is restored when TRY exits (single `eh.pop`).
/// - The TRY statement records one resume site; statements inside it record none,
///   because the dispatcher's arm cannot enter a body that runs with the TRY handler
///   installed or holds its resume token. A RESUME inside CATCH resumes the error the
///   ON ERROR handler is running, like any other RESUME.
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

    // The TRY statement is the resume site; RESUME cannot enter its bodies, which run
    // with the TRY handler installed or hold its resume token.
    ctx.errorHandlers().suppressSites();

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

    // The optional CATCH variable holds the handled error's code, as ERR() reports it.
    if (stmt.catchVar && !stmt.catchVar->empty()) {
        if (auto storage = resolveVariableStorage(*stmt.catchVar, stmt.loc)) {
            BuiltinCallExpr errCall;
            errCall.builtin = BuiltinCallExpr::Builtin::Err;
            errCall.loc = stmt.loc;
            RVal code = coerceToI64(lowerBuiltinCall(errCall), stmt.loc);
            emitStore(Type(Type::Kind::I64), storage->pointer, code.value);
        }
    }

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

    ctx.errorHandlers().restoreSites();

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
        // The variable owns one reference, which the cleanup below releases: an owned
        // temporary such as a NEW result moves in (statements in the body must not
        // release it), and a borrowed value is retained.
        if (!emitter().takeDeferredTemp(objPtr)) {
            requestHelper(RuntimeFeature::ObjRetainMaybe);
            emitCall("rt_obj_retain_maybe", {objPtr});
        }
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

    // Step 3: Lower body statements. They run with the USING handler installed, so
    // RESUME cannot enter them; the USING statement is their resume site.
    ctx.errorHandlers().suppressSites();
    for (const auto &st : stmt.body) {
        if (!st)
            continue;
        lowerStmt(*st);
        BasicBlock *cur = ctx.current();
        if (!cur || cur->terminated)
            break;
    }
    ctx.errorHandlers().restoreSites();

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
