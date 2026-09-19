//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
/// @file Lowerer_Stmt_EH.cpp
/// @brief Exception handling statement lowering for the Zia IL lowerer.
///
/// @details Implements lowering of try/catch/finally and throw statements to
/// IL exception handling instructions (EhPush, EhPop, EhEntry, ResumeLabel).
///
/// ## IL Pattern for try/catch/finally:
///
/// ```
///   eh.push ^handler
///   [try body]
///   eh.pop
///   br ^finally_normal  (or ^after if no finally)
///
/// ^handler(%err: error, %tok: resumetok):
///   eh.entry
///   [capture error metadata]
///   br ^catch_cont(%err, %tok)
///
/// ^catch_cont(%caught_err: error, %caught_tok: resumetok):
///   [catch body and finally body — duplicated]
///   br ^catch_resume(%caught_err, %caught_tok)
///
/// ^catch_resume(%err: error, %tok: resumetok):
///   eh.entry
///   resume.label %tok, ^after
///
/// ^finally_normal:
///   [finally body]
///   br ^after
///
/// ^after:
///   [continuation]
/// ```
///
/// ## IL Pattern for typed catch — catch(e: ErrorType):
///
/// ```
/// ^handler(%err: error, %tok: resumetok):
///   eh.entry
///   %kind_i64 = trap.kind                // I64
///   %expected = const.i64 <kind_value>
///   %match = icmp.eq %kind_i64, %expected
///   cbr %match, ^catch_body(%err, %tok), ^rethrow(%err, %tok)
///
/// ^rethrow(%err: error, %tok: resumetok):
///   eh.entry
///   %kind = err.get_kind %err
///   %code = err.get_code %err
///   %line = err.get_line %err
///   call @Zanna.Error.RaiseKind(%kind, %code, %line)
///   trap.from_err i32 9                 // unreachable fallback
///
/// ^catch_body(%err: error, %tok: resumetok):
///   eh.entry                             // required: makes block a handler
///   [capture error metadata]
///   br ^catch_cont(%err, %tok)
/// ^catch_cont(%caught_err: error, %caught_tok: resumetok):
///   [catch body with captured error bound]
///   br ^catch_resume(%caught_err, %caught_tok)
/// ^catch_resume(%err: error, %tok: resumetok):
///   eh.entry
///   resume.label %tok, ^after
/// ```
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"
#include "frontends/zia/ZiaLocationScope.hpp"

#include <algorithm>
#include <vector>

namespace il::frontends::zia {

using namespace runtime;

/// @brief Map a typed-catch error type name to its TrapKind integer value.
/// @param name Zia catch type name.
/// @return Runtime trap-kind integer, or -1 for `Error` and unrecognized
///         names.
/// @details Returns -1 for "Error" (catch-all) or unrecognised names.
static int trapKindFromName(const std::string &name) {
    if (name == "DivideByZero")
        return 0;
    if (name == "Overflow")
        return 1;
    if (name == "InvalidCast")
        return 2;
    if (name == "DomainError")
        return 3;
    if (name == "Bounds")
        return 4;
    if (name == "FileNotFound")
        return 5;
    if (name == "EOF")
        return 6;
    if (name == "IOError")
        return 7;
    if (name == "InvalidOperation")
        return 8;
    if (name == "RuntimeError")
        return 9;
    if (name == "Interrupt")
        return 10;
    if (name == "NetworkError")
        return 11;
    return -1; // "Error" catch-all or unknown
}

/// @brief Runtime error code (`Err_RuntimeError`) a language-level `throw`
///        raises through `trap.from_err`, which maps it to the RuntimeError
///        trap kind (ADR 0375).
static constexpr int kErrRuntimeError = 9;

/// @brief Emit `eh.pop` in the current block.
void Lowerer::emitEhPop() {
    il::core::Instr ehPopInstr;
    ehPopInstr.op = Opcode::EhPop;
    ehPopInstr.type = Type(Type::Kind::Void);
    ehPopInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(std::move(ehPopInstr));
}

/// @brief Emit every currently active deferred cleanup.
void Lowerer::emitActiveCleanups() {
    emitCleanupsFrom(0);
}

/// @brief Emit cleanup frames from one lexical boundary in reverse order.
/// @param startIndex First cleanup-stack entry belonging to the exiting scope.
/// @details Temporarily truncates the live stack while lowering each finally
///          body so nested abrupt exits do not re-run the same cleanup, then
///          restores the caller's cleanup metadata.
void Lowerer::emitCleanupsFrom(size_t startIndex) {
    if (cleanupStack_.size() <= startIndex || isTerminated())
        return;

    auto savedFrames = cleanupStack_;

    for (size_t i = savedFrames.size(); i-- > startIndex;) {
        // Truncate the live stack to [0, i) by popping (O(1) per step) instead of
        // re-copying the prefix each iteration, which made deep try/finally O(n^2).
        while (cleanupStack_.size() > i)
            cleanupStack_.pop_back();

        const CleanupFrame &frame = savedFrames[i];
        if (frame.popEhBeforeFinally && !isTerminated())
            emitEhPop();
        if (frame.finallyBody && !isTerminated())
            lowerStmt(frame.finallyBody);
        if (isTerminated())
            break;
    }

    cleanupStack_ = std::move(savedFrames);
}

/// @brief Emit catch-local cleanup frames before a throw or rethrow.
/// @details Walks outward only to the nearest exception-handler boundary,
///          leaving outer handler cleanup for normal EH propagation.
void Lowerer::emitCatchBodyCleanupsBeforeThrow() {
    if (cleanupStack_.empty() || isTerminated())
        return;

    auto savedFrames = cleanupStack_;

    for (size_t i = savedFrames.size(); i-- > 0;) {
        const CleanupFrame &frame = savedFrames[i];
        if (frame.popEhBeforeFinally)
            break;

        // Truncate the live stack to [0, i) by popping (O(1) per step) instead of
        // re-copying the prefix each iteration, which made deep try/finally O(n^2).
        while (cleanupStack_.size() > i)
            cleanupStack_.pop_back();
        if (frame.finallyBody && !isTerminated())
            lowerStmt(frame.finallyBody);
        if (isTerminated())
            break;
    }

    cleanupStack_ = std::move(savedFrames);
}

/// @brief Lower a `try` statement with typed catches and optional `finally`.
/// @param stmt Try statement to lower.
/// @details Emits canonical error/resume-token handler entries, typed
///          trap-kind tests, uniquely parameterized catch continuations,
///          captured error metadata, normal and exceptional finally paths,
///          rethrow fallback, and resume-label transfer to the shared
///          continuation. Cleanup metadata is coordinated with abrupt exits
///          from nested statements.
void Lowerer::materializeLocalsForHandlers() {
    if (!currentFunc_ || currentFunc_->blocks.empty())
        return;
    auto &entry = currentFunc_->blocks.front();
    /// @brief Index just past the entry block's leading allocas (keeps allocas grouped first).
    auto allocaInsertPos = [&entry]() {
        size_t pos = 0;
        while (pos < entry.instructions.size() && entry.instructions[pos].op == Opcode::Alloca)
            ++pos;
        return pos;
    };

    // 1. Relocate the alloca of every visible slot declared outside the entry block.
    for (const auto &[name, slot] : slots_) {
        (void)name;
        if (slot.kind != Value::Kind::Temp)
            continue;
        for (size_t b = 1; b < currentFunc_->blocks.size(); ++b) {
            auto &instrs = currentFunc_->blocks[b].instructions;
            auto it = std::find_if(instrs.begin(), instrs.end(), [&](const il::core::Instr &in) {
                return in.op == Opcode::Alloca && in.result && *in.result == slot.id;
            });
            if (it == instrs.end())
                continue;
            il::core::Instr moved = std::move(*it);
            instrs.erase(it);
            entry.instructions.insert(entry.instructions.begin() + allocaInsertPos(),
                                      std::move(moved));
            break;
        }
    }

    // 2. Spill every visible SSA local into a fresh entry-block slot. The store runs here, at
    //    the try; later reads of the name (in the protected region and the handlers) go
    //    through the slot. Block-scoped maps restore the SSA binding when the scope ends, which
    //    is sound because an SSA local is immutable.
    std::vector<std::string> spilled;
    for (const auto &[name, value] : locals_) {
        if (slots_.count(name))
            continue;
        Type ilType(Type::Kind::Ptr);
        if (name != "self") {
            auto typeIt = localTypes_.find(name);
            if (typeIt == localTypes_.end() || !typeIt->second)
                continue; // untyped internal binding; never referenced by source
            ilType = mapType(typeIt->second);
        }
        il::core::Instr allocaInstr;
        const unsigned allocaId = nextTempId();
        allocaInstr.result = allocaId;
        allocaInstr.op = Opcode::Alloca;
        allocaInstr.type = Type(Type::Kind::Ptr);
        allocaInstr.operands = {Value::constInt(static_cast<long long>(kMachineWordSize))};
        allocaInstr.loc = curLoc_;
        entry.instructions.insert(entry.instructions.begin() + allocaInsertPos(), allocaInstr);
        nameTemp(allocaId, name);
        slots_[name] = Value::temp(allocaId);
        // Block and function exits release owned string/object slots by type, so a spilled
        // managed value is retained: the slot owns one reference, balanced by that release.
        if (isOwnedStringSlot(name))
            emitCall(kStrRetainMaybe, {value});
        else if (isOwnedObjectSlot(name))
            emitCall("rt_obj_retain_maybe", {value});
        storeToSlot(name, value, ilType);
        spilled.push_back(name);
    }
    for (const auto &name : spilled)
        locals_.erase(name);
}

void Lowerer::lowerTryStmt(TryStmt *stmt) {
    ZiaLocationScope locScope(*this, stmt->loc);

    // Handlers may read any visible local: route them all through entry-block slots first.
    materializeLocalsForHandlers();

    bool hasFinally = stmt->finallyBody != nullptr;
    bool hasCatch = !stmt->catches.empty();

    // Create the post-try continuation first so helper lambdas can target it.
    size_t afterIdx = createBlock("after_try");

    /// @brief Creates a canonical error/resume-token handler block.
    /// @param base Base label used for unique naming.
    /// @return New block index.
    auto createHandlerBlock = [&](const std::string &base) -> size_t {
        std::vector<il::core::Param> params;
        params.push_back({"err", Type(Type::Kind::Error)});
        params.push_back({"tok", Type(Type::Kind::ResumeTok)});
        unsigned blockId = blockMgr_.nextBlockId();
        blockMgr_.setNextBlockId(blockId + 1);
        builder_->createBlock(*currentFunc_, makeSuffixedName(base, blockId), params);
        return currentFunc_->blocks.size() - 1;
    };

    /// @brief Creates a uniquely parameterized catch continuation block.
    /// @param base Base label used for unique naming.
    /// @return New block index.
    auto createCatchContinuationBlock = [&](const std::string &base) -> size_t {
        const unsigned blockId = blockMgr_.nextBlockId();
        blockMgr_.setNextBlockId(blockId + 1);
        const std::string suffix = std::to_string(blockId);
        std::vector<il::core::Param> params;
        params.push_back({"catch_err_" + suffix, Type(Type::Kind::Error)});
        params.push_back({"catch_tok_" + suffix, Type(Type::Kind::ResumeTok)});
        builder_->createBlock(*currentFunc_, makeSuffixedName(base, blockId), params);
        return currentFunc_->blocks.size() - 1;
    };

    /// @brief Appends the canonical exception-handler entry marker.
    auto emitEhEntry = [&]() {
        il::core::Instr ehEntryInstr;
        ehEntryInstr.op = Opcode::EhEntry;
        ehEntryInstr.type = Type(Type::Kind::Void);
        ehEntryInstr.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(std::move(ehEntryInstr));
    };

    /// @brief Emits a branch with block arguments.
    /// @param targetIdx Destination block index.
    /// @param args Values transferred to destination parameters.
    auto emitBrWithArgs = [&](size_t targetIdx, const std::vector<Value> &args) {
        il::core::Instr brInstr;
        brInstr.op = Opcode::Br;
        brInstr.type = Type(Type::Kind::Void);
        brInstr.addBranchTarget(currentFunc_->blocks[targetIdx].label, args);
        brInstr.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(std::move(brInstr));
        blockMgr_.currentBlock()->terminated = true;
    };

    /// @brief Emits a conditional branch with arguments on both edges.
    /// @param cond Boolean branch condition.
    /// @param trueIdx True-edge destination index.
    /// @param trueArgs True-edge arguments.
    /// @param falseIdx False-edge destination index.
    /// @param falseArgs False-edge arguments.
    auto emitCBrWithArgs = [&](Value cond,
                               size_t trueIdx,
                               const std::vector<Value> &trueArgs,
                               size_t falseIdx,
                               const std::vector<Value> &falseArgs) {
        il::core::Instr cbrInstr;
        cbrInstr.op = Opcode::CBr;
        cbrInstr.type = Type(Type::Kind::Void);
        cbrInstr.operands.push_back(cond);
        cbrInstr.addBranchTarget(currentFunc_->blocks[trueIdx].label, trueArgs);
        cbrInstr.addBranchTarget(currentFunc_->blocks[falseIdx].label, falseArgs);
        cbrInstr.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(std::move(cbrInstr));
        blockMgr_.currentBlock()->terminated = true;
    };

    /// @brief Emits a structural trap fallback after a runtime raise call.
    auto emitTrapFallback = [&]() {
        il::core::Instr trapInstr;
        trapInstr.op = Opcode::TrapFromErr;
        trapInstr.type = Type(Type::Kind::I32);
        trapInstr.operands.push_back(Value::constInt(kErrRuntimeError));
        trapInstr.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(std::move(trapInstr));
        blockMgr_.currentBlock()->terminated = true;
    };

    /// @brief Captures error metadata into uniquely named local slots.
    /// @param errVal Error value to decompose.
    /// @param prefix Prefix for generated slot names.
    /// @return Binding that identifies the captured slots.
    auto captureErrorFields = [&](Value errVal, const std::string &prefix) -> CatchErrorBinding {
        CatchErrorBinding slots;
        const std::string base = "__zia_" + prefix + "_" + std::to_string(nextTempId());
        slots.kindSlot = base + "_kind";
        slots.codeSlot = base + "_code";
        slots.lineSlot = base + "_line";

        Value errKind = emitUnary(Opcode::ErrGetKind, Type(Type::Kind::I32), errVal);
        Value errCode = emitUnary(Opcode::ErrGetCode, Type(Type::Kind::I32), errVal);
        Value errLine = emitUnary(Opcode::ErrGetLine, Type(Type::Kind::I32), errVal);

        createSlot(slots.kindSlot, Type(Type::Kind::I32));
        storeToSlot(slots.kindSlot, errKind, Type(Type::Kind::I32));
        createSlot(slots.codeSlot, Type(Type::Kind::I32));
        storeToSlot(slots.codeSlot, errCode, Type(Type::Kind::I32));
        createSlot(slots.lineSlot, Type(Type::Kind::I32));
        storeToSlot(slots.lineSlot, errLine, Type(Type::Kind::I32));

        return slots;
    };

    /// @brief Reconstructs and raises an error from captured metadata.
    /// @param slots Captured error-field slots.
    auto emitRethrowFromCaptured = [&](const CatchErrorBinding &slots) {
        Value errKind = loadFromSlot(slots.kindSlot, Type(Type::Kind::I32));
        Value errCode = loadFromSlot(slots.codeSlot, Type(Type::Kind::I32));
        Value errLine = loadFromSlot(slots.lineSlot, Type(Type::Kind::I32));

        emitCall(kRuntimeUnsafeRaiseKind, {errKind, errCode, errLine});

        // The runtime call raises through the active VM/native trap bridge. Keep
        // a terminator fallback so the IL remains structurally valid if a host
        // installs a non-terminating trap observer.
        emitTrapFallback();
    };

    /// @brief Routes an error/token pair through a resume-label block.
    /// @param errVal Error argument.
    /// @param tokVal Resume-token argument.
    /// @param base Base label for the generated resume block.
    auto emitResumeToAfter = [&](Value errVal, Value tokVal, const std::string &base) {
        size_t resumeIdx = createHandlerBlock(base);
        emitBrWithArgs(resumeIdx, {errVal, tokVal});

        setBlock(resumeIdx);
        emitEhEntry();
        const auto &resumeBp = currentFunc_->blocks[resumeIdx].params;
        Value resumeTok = Value::temp(resumeBp[1].id);

        il::core::Instr resumeInstr;
        resumeInstr.op = Opcode::ResumeLabel;
        resumeInstr.type = Type(Type::Kind::Void);
        resumeInstr.operands.push_back(resumeTok);
        resumeInstr.addBranchTarget(currentFunc_->blocks[afterIdx].label);
        resumeInstr.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(std::move(resumeInstr));
        blockMgr_.currentBlock()->terminated = true;
    };

    /// @brief Binds a catch variable to its block error parameter.
    /// @param catchClause Catch declaration supplying the optional variable name.
    /// @param paramBlockIdx Catch continuation block index.
    /// @param captured Captured error metadata associated with the value.
    /// @return Effective captured binding.
    auto bindCatchPayload = [&](const TryStmt::CatchClause &catchClause,
                                size_t paramBlockIdx,
                                const CatchErrorBinding &captured) -> CatchErrorBinding {
        const auto &bp = currentFunc_->blocks[paramBlockIdx].params;
        if (bp.empty())
            return {};

        Value errVal = Value::temp(bp[0].id);
        if (catchClause.var.empty())
            return captured;

        defineLocal(catchClause.var, errVal);
        localTypes_[catchClause.var] = types::error();
        catchErrorBindings_[catchClause.var] = captured;
        return captured;
    };

    // Handler entries keep the canonical ABI names. Catch implementations use
    // unique ordinary block parameters so serialized IL cannot confuse values
    // from different `%err`/`%tok` scopes after cleanup CFG expansion.
    size_t handlerIdx = createHandlerBlock("handler");

    // Optional: finally_normal block (only if we have a finally clause)
    size_t finallyNormalIdx = 0;
    if (hasFinally) {
        finallyNormalIdx = createBlock("finally_normal");
    }

    std::vector<size_t> catchCheckBlocks;
    std::vector<size_t> catchBodyBlocks;
    std::vector<size_t> catchContinuationBlocks;
    catchCheckBlocks.reserve(stmt->catches.size());
    catchBodyBlocks.reserve(stmt->catches.size());
    catchContinuationBlocks.reserve(stmt->catches.size());
    if (hasCatch) {
        catchCheckBlocks.push_back(handlerIdx);
        for (size_t i = 1; i < stmt->catches.size(); ++i)
            catchCheckBlocks.push_back(createHandlerBlock("catch_check"));
        for (size_t i = 0; i < stmt->catches.size(); ++i) {
            catchBodyBlocks.push_back(createHandlerBlock("catch_body"));
            catchContinuationBlocks.push_back(createCatchContinuationBlock("catch_cont"));
        }
    }
    size_t rethrowIdx = 0;
    if (hasCatch)
        rethrowIdx = createHandlerBlock("rethrow");

    // --- Emit eh.push in current block ---
    {
        il::core::Instr ehPushInstr;
        ehPushInstr.op = Opcode::EhPush;
        ehPushInstr.type = Type(Type::Kind::Void);
        ehPushInstr.addBranchTarget(currentFunc_->blocks[handlerIdx].label);
        ehPushInstr.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(std::move(ehPushInstr));
    }

    // --- Lower try body ---
    cleanupStack_.push_back({stmt->finallyBody.get(), true});
    if (stmt->tryBody)
        lowerStmt(stmt->tryBody.get());
    cleanupStack_.pop_back();

    // --- On normal exit from try: eh.pop + branch ---
    if (!isTerminated()) {
        emitEhPop();

        if (hasFinally)
            emitBr(finallyNormalIdx);
        else
            emitBr(afterIdx);
    }

    if (hasCatch) {
        for (size_t i = 0; i < stmt->catches.size(); ++i) {
            setBlock(catchCheckBlocks[i]);
            emitEhEntry();
            const auto &bp = currentFunc_->blocks[catchCheckBlocks[i]].params;
            Value errVal = Value::temp(bp[0].id);
            Value tokVal = Value::temp(bp[1].id);

            const auto &catchClause = stmt->catches[i];
            const bool catchAll = catchClause.typeName.empty() || catchClause.typeName == "Error";
            if (catchAll) {
                emitBrWithArgs(catchBodyBlocks[i], {errVal, tokVal});
                continue;
            }

            int expectedKind = trapKindFromName(catchClause.typeName);
            Value errKindI32 = emitUnary(Opcode::ErrGetKind, Type(Type::Kind::I32), errVal);
            Value errKind = widenIntegralToI64(errKindI32, Type(Type::Kind::I32));
            Value expectedVal = Value::constInt(static_cast<int64_t>(expectedKind));
            Value match = emitBinary(Opcode::ICmpEq, Type(Type::Kind::I1), errKind, expectedVal);
            size_t missBlock =
                (i + 1 < stmt->catches.size()) ? catchCheckBlocks[i + 1] : rethrowIdx;
            emitCBrWithArgs(
                match, catchBodyBlocks[i], {errVal, tokVal}, missBlock, {errVal, tokVal});
        }

        // --- Rethrow block: run finally for mismatches, then re-raise the original error. ---
        setBlock(rethrowIdx);
        emitEhEntry();
        const auto &rethrowBp = currentFunc_->blocks[rethrowIdx].params;
        CatchErrorBinding captured = captureErrorFields(Value::temp(rethrowBp[0].id), "rethrow");

        if (hasFinally && stmt->finallyBody && !isTerminated())
            lowerStmt(stmt->finallyBody.get());

        if (!isTerminated())
            emitRethrowFromCaptured(captured);

        for (size_t i = 0; i < stmt->catches.size(); ++i) {
            const auto &catchClause = stmt->catches[i];
            auto localsBackup = locals_;
            auto slotsBackup = slots_;
            auto localTypesBackup = localTypes_;
            auto catchErrorBindingsBackup = catchErrorBindings_;

            setBlock(catchBodyBlocks[i]);
            emitEhEntry();
            const auto &handlerParams = currentFunc_->blocks[catchBodyBlocks[i]].params;
            CatchErrorBinding capturedError =
                captureErrorFields(Value::temp(handlerParams[0].id), "catch");
            emitBrWithArgs(catchContinuationBlocks[i],
                           {Value::temp(handlerParams[0].id), Value::temp(handlerParams[1].id)});

            setBlock(catchContinuationBlocks[i]);
            CatchErrorBinding activeError =
                bindCatchPayload(catchClause, catchContinuationBlocks[i], capturedError);

            if (hasFinally)
                cleanupStack_.push_back({stmt->finallyBody.get(), false});
            activeCatchErrors_.push_back(activeError);
            if (catchClause.body)
                lowerStmt(catchClause.body.get());
            activeCatchErrors_.pop_back();
            if (hasFinally)
                cleanupStack_.pop_back();

            locals_ = std::move(localsBackup);
            slots_ = std::move(slotsBackup);
            localTypes_ = std::move(localTypesBackup);
            catchErrorBindings_ = std::move(catchErrorBindingsBackup);

            if (!isTerminated())
                emitCall(kRuntimeUnsafeClearThrowMsg, {});

            if (hasFinally && stmt->finallyBody && !isTerminated())
                lowerStmt(stmt->finallyBody.get());

            if (!isTerminated()) {
                const auto &bp = currentFunc_->blocks[catchContinuationBlocks[i]].params;
                emitResumeToAfter(Value::temp(bp[0].id), Value::temp(bp[1].id), "catch_resume");
            }
        }
    } else {
        // Finally-only handlers must rethrow after cleanup so outer handlers
        // still observe the original error.
        setBlock(handlerIdx);
        emitEhEntry();
        const auto &bp = currentFunc_->blocks[handlerIdx].params;
        CatchErrorBinding captured = captureErrorFields(Value::temp(bp[0].id), "finally_rethrow");
        if (hasFinally && stmt->finallyBody && !isTerminated())
            lowerStmt(stmt->finallyBody.get());
        if (!isTerminated())
            emitRethrowFromCaptured(captured);
    }

    // --- Finally normal block (normal path) ---
    if (hasFinally) {
        setBlock(finallyNormalIdx);
        if (stmt->finallyBody)
            lowerStmt(stmt->finallyBody.get());
        if (!isTerminated())
            emitBr(afterIdx);
    }

    // --- Continue at after_try ---
    setBlock(afterIdx);
}

/// @brief Lower a language-level `throw` or catch-local rethrow.
/// @param stmt Throw statement with an optional message expression.
/// @details Runs catch-local cleanups, preserves captured kind/code/line for a
///          value-less rethrow, converts explicit values to stored runtime
///          messages, and terminates with the RuntimeError trap kind.
void Lowerer::lowerThrowStmt(ThrowStmt *stmt) {
    ZiaLocationScope locScope(*this, stmt->loc);

    emitCatchBodyCleanupsBeforeThrow();
    if (isTerminated())
        return;

    if (!stmt->value && !activeCatchErrors_.empty()) {
        const CatchErrorBinding &active = activeCatchErrors_.back();
        if (!active.kindSlot.empty()) {
            Value errKind = loadFromSlot(active.kindSlot, Type(Type::Kind::I32));
            Value errCode = loadFromSlot(active.codeSlot, Type(Type::Kind::I32));
            Value errLine = loadFromSlot(active.lineSlot, Type(Type::Kind::I32));
            emitCall(kRuntimeUnsafeRaiseKind, {errKind, errCode, errLine});
        }

        il::core::Instr trapInstr;
        trapInstr.op = Opcode::TrapFromErr;
        trapInstr.type = Type(Type::Kind::I32);
        trapInstr.operands.push_back(Value::constInt(kErrRuntimeError));
        trapInstr.loc = curLoc_;
        blockMgr_.currentBlock()->instructions.push_back(std::move(trapInstr));
        blockMgr_.currentBlock()->terminated = true;
        return;
    }

    // Lower the thrown expression and store the message via the runtime
    // so catch handlers can retrieve it.
    if (stmt->value) {
        auto result = lowerExpr(stmt->value.get());
        TypeRef throwType = sema_.typeOf(stmt->value.get());
        Value msgStr = emitToString(result.value, throwType);

        // Store the message via rt_throw_msg_set for catch(e) retrieval.
        emitCall(kRuntimeUnsafeSetThrowMsg, {msgStr});
    }

    // Emit a RuntimeError trap for user-visible throw statements. Plain IL
    // `trap` remains a DomainError for lower-level users; Zia `throw` is the
    // language-level runtime error promised by typed catch documentation.
    il::core::Instr trapInstr;
    trapInstr.op = Opcode::TrapFromErr;
    trapInstr.type = Type(Type::Kind::I32);
    trapInstr.operands.push_back(Value::constInt(kErrRuntimeError));
    trapInstr.loc = curLoc_;
    blockMgr_.currentBlock()->instructions.push_back(std::move(trapInstr));
    blockMgr_.currentBlock()->terminated = true;
}

} // namespace il::frontends::zia
