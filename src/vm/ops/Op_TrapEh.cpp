//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements trap and exception-handling opcode handlers for the virtual
// machine interpreter.  The helpers decode VmError payloads, manage resume
// tokens, and bridge legacy error codes into structured trap reporting.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Opcode handlers for trap production and exception resumption.
/// @details The helper functions in this translation unit operate on
///          @ref VM frames to expose IL instructions that inspect, modify, and
///          raise trap state.  They coordinate with the runtime bridge to keep
///          legacy error codes interoperable with the structured trap model.

#include "vm/OpHandlers_Control.hpp"

#include "vm/Marshal.hpp"
#include "vm/OpHandlerUtils.hpp"
#include "vm/RuntimeBridge.hpp"
#include "vm/Trap.hpp"
#include "vm/err_bridge.hpp"

#include <cassert>
#include <string>

extern "C" {
#include "runtime/core/rt_string.h"
}

namespace il::vm::detail::control {

/// @brief Extract fields from a VmError record and store them into registers.
///
/// @details The handler accepts an optional operand referencing a resume token
///          or error value.  When omitted it falls back to the frame's active
///          error.  Depending on the opcode variant it copies the requested
///          field (kind, code, instruction pointer, or line) into the result
///          register.  The helper never modifies control flow and simply returns
///          to the dispatcher.
///
/// @param vm Virtual machine used to evaluate an optional error operand.
/// @param fr Frame providing the active error state.
/// @param in Instruction indicating which field to retrieve.
/// @param blocks Map of block labels to block pointers (unused).
/// @param bb Reference to the current block pointer (unused).
/// @param ip Instruction pointer (unused).
/// @return Execution result signalling normal continuation.
VM::ExecResult handleErrGet(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;

    Slot operandSlot{};
    if (!in.operands.empty())
        operandSlot = VMAccess::eval(vm, fr, in.operands[0]);

    const VmError *error = resolveErrorToken(fr, operandSlot);

    Slot out{};
    switch (in.op) {
        case il::core::Opcode::ErrGetKind:
            out.i64 = static_cast<int64_t>(static_cast<int32_t>(error->kind));
            break;
        case il::core::Opcode::ErrGetCode:
            out.i64 = static_cast<int64_t>(error->code);
            break;
        case il::core::Opcode::ErrGetIp:
            out.i64 = static_cast<int64_t>(error->ip);
            break;
        case il::core::Opcode::ErrGetLine:
            out.i64 = static_cast<int64_t>(static_cast<int32_t>(error->line));
            break;
        case il::core::Opcode::ErrGetMsg: {
            // Retrieve the trap message stored by trap.err / vm_store_trap_token_message.
            // The message persists in vm->trapToken.message even after the token
            // valid flag is cleared during handler dispatch.
            std::string msg = vm_current_trap_message();
            out.str = msg.empty() ? rt_str_empty() : rt_string_from_bytes(msg.c_str(), msg.size());
            break;
        }
        default:
            out.i64 = 0;
            break;
    }

    ops::storeResult(fr, in, out);
    return {};
}

/// @brief No-op handler that marks the beginning of an exception region.
///
/// @details The opcode exists to keep the instruction stream aligned with the
///          source program; the runtime only needs to know about push/pop and
///          resume operations.  Consequently the handler intentionally performs
///          no work and returns immediately.
/// @param vm VM argument required by the common handler signature; unused.
/// @param fr Frame argument required by the common handler signature; unused.
/// @param in Marker instruction; no operands are consumed.
/// @param blocks Block map required by the common handler signature; unused.
/// @param bb Current block argument; unchanged.
/// @param ip Instruction pointer argument; unchanged.
/// @return Empty result indicating normal continuation.
VM::ExecResult handleEhEntry(VM &vm,
                             Frame &fr,
                             const il::core::Instr &in,
                             const VM::BlockMap &blocks,
                             const il::core::BasicBlock *&bb,
                             size_t &ip) {
    (void)vm;
    (void)fr;
    (void)in;
    (void)blocks;
    (void)bb;
    (void)ip;
    return {};
}

/// @brief Register an exception handler block on the frame's handler stack.
///
/// @details Validates that a destination label accompanies the opcode, resolves
///          it to a basic block, and pushes a handler record capturing the
///          target block together with the installation instruction snapshot.
/// @param vm VM argument required by the common handler signature; unused.
/// @param fr Active frame whose exception-handler stack is updated.
/// @param in Handler installation instruction containing the target label.
/// @param blocks Label-to-block lookup used to resolve the handler.
/// @param bb Current block pointer used for diagnostics.
/// @param ip Current instruction index captured in the handler record.
/// @return Empty result indicating normal continuation after installation.
VM::ExecResult handleEhPush(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip) {
    (void)vm;
    (void)bb;
    (void)ip;
    if (in.labels.empty()) {
        RuntimeBridge::trap(TrapKind::InvalidOperation,
                            "eh.push requires handler label",
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            bb ? bb->label : std::string());
    }
    auto it = blocks.find(in.labels[0]);
    if (it == blocks.end()) {
        RuntimeBridge::trap(TrapKind::InvalidOperation,
                            "eh.push handler block not found",
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            bb ? bb->label : std::string());
    }
    Frame::HandlerRecord record{};
    record.handler = it->second;
    record.ipSnapshot = ip;
    fr.ehStack.push_back(record);
    return {};
}

/// @brief Remove the most recently registered exception handler.
///
/// @details Pops the frame's handler stack when non-empty.  The opcode is a
///          safety net in case control flow leaves a protected region without a
///          corresponding resume, ensuring stale handlers do not survive across
///          scopes.
/// @param vm VM argument required by the common handler signature; unused.
/// @param fr Active frame whose newest handler is removed.
/// @param in Pop instruction; no operands are consumed.
/// @param blocks Block map required by the common handler signature; unused.
/// @param bb Current block argument; unchanged.
/// @param ip Instruction pointer argument; unchanged.
/// @return Empty result indicating normal continuation.
VM::ExecResult handleEhPop(VM &vm,
                           Frame &fr,
                           const il::core::Instr &in,
                           const VM::BlockMap &blocks,
                           const il::core::BasicBlock *&bb,
                           size_t &ip) {
    (void)vm;
    (void)in;
    (void)blocks;
    (void)bb;
    (void)ip;
    if (!fr.ehStack.empty())
        fr.ehStack.pop_back();
    return {};
}

/// @brief Resume execution at the trapping instruction itself.
/// @details Validates the supplied resume token, ensuring it matches the
///          current frame and that the recorded target block still exists.
///          Successful resumes clear the frame's resume state, redirect the
///          current block pointer to the captured block, and reset the
///          instruction pointer to the trapping site.  Failures emit a
///          diagnostic via @ref trapInvalidResume.  Resume tokens are
///          single-use; consuming one invalidates it to prevent stale
///          resumptions after handler unwinding.
/// @param vm Virtual machine used to evaluate the token operand.
/// @param fr Active frame owning the resume state.
/// @param in Resume instruction containing the token.
/// @param blocks Block map required by the common handler signature; unused.
/// @param bb Current block pointer redirected to the faulting block.
/// @param ip Instruction pointer redirected to the faulting instruction.
/// @return Jump result on success, or normal result after an invalid-token trap.
VM::ExecResult handleResumeSame(VM &vm,
                                Frame &fr,
                                const il::core::Instr &in,
                                const VM::BlockMap &blocks,
                                const il::core::BasicBlock *&bb,
                                size_t &ip) {
    (void)blocks;
    if (in.operands.empty()) {
        trapInvalidResume(fr, in, bb, "resume.same: missing resume token operand");
        return {};
    }

    Slot tokSlot = VMAccess::eval(vm, fr, in.operands[0]);
    Frame::ResumeState *token = expectResumeToken(fr, tokSlot);
    if (!token) {
        trapInvalidResume(fr, in, bb, "resume.same: requires an active resume token");
        return {};
    }
    if (!token->block) {
        trapInvalidResume(fr, in, bb, "resume.same: resume target is no longer available");
        return {};
    }
    // Save token fields before invalidating to prevent use-after-invalidate
    // when token aliases fr.resumeState.
    const auto *savedBlock = token->block;
    const auto savedFaultIp = token->faultIp;
    fr.resumeState.valid = false;
    bb = savedBlock;
    ip = savedFaultIp;
    VM::ExecResult result{};
    result.jumped = true;
    return result;
}

/// @brief Resume execution at the instruction immediately following the trap.
///
/// @details Mirrors @ref handleResumeSame but jumps to the saved "next" program
///          counter recorded when the resume token was created.  This is used
///          for trap handlers that want to skip the trapping instruction rather
///          than re-executing it.
/// @param vm Virtual machine used to evaluate the token operand.
/// @param fr Active frame owning the resume state.
/// @param in Resume instruction containing the token.
/// @param blocks Block map required by the common handler signature; unused.
/// @param bb Current block pointer redirected to the faulting block.
/// @param ip Instruction pointer redirected to the saved successor.
/// @return Jump result on success, or normal result after an invalid-token trap.
VM::ExecResult handleResumeNext(VM &vm,
                                Frame &fr,
                                const il::core::Instr &in,
                                const VM::BlockMap &blocks,
                                const il::core::BasicBlock *&bb,
                                size_t &ip) {
    (void)blocks;
    if (in.operands.empty()) {
        trapInvalidResume(fr, in, bb, "resume.next: missing resume token operand");
        return {};
    }

    Slot tokSlot = VMAccess::eval(vm, fr, in.operands[0]);
    Frame::ResumeState *token = expectResumeToken(fr, tokSlot);
    if (!token) {
        trapInvalidResume(fr, in, bb, "resume.next: requires an active resume token");
        return {};
    }
    if (!token->block) {
        trapInvalidResume(fr, in, bb, "resume.next: resume target is no longer available");
        return {};
    }
    // Save token fields before invalidating to prevent use-after-invalidate
    // when token aliases fr.resumeState.
    const auto *savedBlock = token->block;
    const auto savedNextIp = token->nextIp;
    fr.resumeState.valid = false;
    bb = savedBlock;
    ip = savedNextIp;
    VM::ExecResult result{};
    result.jumped = true;
    return result;
}

/// @brief Resume execution by branching to an explicitly provided label.
///
/// @details Validates both the resume token and the destination label, emitting
///          detailed diagnostics when either is invalid.  On success the frame's
///          resume state is cleared and control transfers through
///          @ref branchToTarget to reuse branch argument propagation logic.
/// @param vm Virtual machine used to evaluate the token and branch arguments.
/// @param fr Active frame owning resume state and target parameters.
/// @param in Resume instruction containing a token and destination label.
/// @param blocks Label-to-block lookup used for target validation and transfer.
/// @param bb Current block pointer updated by the branch.
/// @param ip Instruction pointer reset by the branch.
/// @return Branch result on success, or normal result after validation traps.
VM::ExecResult handleResumeLabel(VM &vm,
                                 Frame &fr,
                                 const il::core::Instr &in,
                                 const VM::BlockMap &blocks,
                                 const il::core::BasicBlock *&bb,
                                 size_t &ip) {
    if (in.operands.empty()) {
        trapInvalidResume(fr, in, bb, "resume.label: missing resume token operand");
        return {};
    }

    Slot tokSlot = VMAccess::eval(vm, fr, in.operands[0]);
    Frame::ResumeState *token = expectResumeToken(fr, tokSlot);
    if (!token) {
        trapInvalidResume(fr, in, bb, "resume.label: requires an active resume token");
        return {};
    }

    if (in.labels.empty()) {
        trapInvalidResume(fr, in, bb, "resume.label: missing destination label");
        return {};
    }

    const auto &label = in.labels[0];
    if (blocks.find(label) == blocks.end()) {
        std::string msg;
        msg.reserve(48 + label.size());
        msg.append("resume.label: unknown destination label '");
        msg.append(label);
        msg.push_back('\'');
        trapInvalidResume(fr, in, bb, msg);
        return {};
    }
    fr.resumeState.valid = false;
    return branchToTarget(vm, fr, in, 0, blocks, bb, ip);
}

/// @brief Return the trap kind associated with the active error or provided token.
///
/// @details Evaluates an optional operand referring to a VmError and otherwise
///          falls back to the current trap token or the frame's active error.
///          The resulting kind is stored as an integer in the destination
///          register, enabling IL code to branch on trap categories.
/// @param vm Virtual machine used to evaluate an optional error operand.
/// @param fr Active frame supplying the fallback error and result storage.
/// @param in Trap-kind query instruction.
/// @param blocks Block map required by the common handler signature; unused.
/// @param bb Current block argument; unchanged.
/// @param ip Instruction pointer argument; unchanged.
/// @return Empty result indicating normal continuation after storing the kind.
VM::ExecResult handleTrapKind(VM &vm,
                              Frame &fr,
                              const il::core::Instr &in,
                              const VM::BlockMap &blocks,
                              const il::core::BasicBlock *&bb,
                              size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;

    const VmError *error = nullptr;
    if (!in.operands.empty()) {
        Slot errorSlot = VMAccess::eval(vm, fr, in.operands[0]);
        error = reinterpret_cast<const VmError *>(errorSlot.ptr);
    }

    if (!error)
        error = vm_current_trap_token();
    if (!error)
        error = &fr.activeError;

    Slot out{};
    const auto kindValue =
        error ? static_cast<int32_t>(error->kind) : static_cast<int32_t>(TrapKind::RuntimeError);
    out.i64 = static_cast<int64_t>(kindValue);
    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Convert a legacy BASIC error code into a VmError trap token.
/// @details Evaluates the numeric error code operand, optionally captures an
///          additional string message, and initialises a freshly acquired trap
///          token.  The token inherits the mapped trap kind and retains the
///          original error code for diagnostic purposes before being written to
///          the result register.  This bridges the legacy `err` semantics into
///          the structured trap path so diagnostics remain consistent.
/// @param vm Virtual machine used to evaluate code and message operands.
/// @param fr Active frame supplying operands, context, and result storage.
/// @param in Trap-token construction instruction.
/// @param blocks Block map required by the common handler signature; unused.
/// @param bb Current block pointer used for malformed-instruction diagnostics.
/// @param ip Instruction pointer argument; unchanged.
/// @return Empty result indicating normal continuation after token construction.
VM::ExecResult handleTrapErr(VM &vm,
                             Frame &fr,
                             const il::core::Instr &in,
                             const VM::BlockMap &blocks,
                             const il::core::BasicBlock *&bb,
                             size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;
    (void)fr;

    if (in.operands.empty()) {
        RuntimeBridge::trap(TrapKind::InvalidOperation,
                            "trap.err: missing error code operand",
                            in.loc,
                            fr.func ? fr.func->name : std::string(),
                            bb ? bb->label : std::string());
    }
    Slot codeSlot = VMAccess::eval(vm, fr, in.operands[0]);
    const int32_t code = static_cast<int32_t>(codeSlot.i64);

    std::string message;
    if (in.operands.size() > 1) {
        Slot textSlot = VMAccess::eval(vm, fr, in.operands[1]);
        if (textSlot.str != nullptr) {
            auto view = fromZannaString(textSlot.str);
            message.assign(view.begin(), view.end());
        }
    }

    VmError *token = vm_acquire_trap_token();
    token->kind = map_err_to_trap(code);
    token->code = code;
    token->ip = 0;
    token->line = -1;
    vm_store_trap_token_message(message);

    Slot out{};
    out.ptr = token;
    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Raise a trap immediately using the opcode-specific semantics.
///
/// @details Handles `trap`, `trap.err`, and `trap.from.err` forms by delegating
///          to the runtime trap helpers.  The function always marks the
///          execution result as returned so the interpreter unwinds to the
///          caller after the trap is raised.
/// @param vm Virtual machine used to evaluate a legacy error-code operand.
/// @param fr Active frame supplying operand and diagnostic context.
/// @param in Trap instruction selecting the raising semantics.
/// @param blocks Block map required by the common handler signature; unused.
/// @param bb Current block pointer required by the common signature; unchanged.
/// @param ip Instruction pointer required by the common signature; unchanged.
/// @return Result marked returned after the trap has been raised.
VM::ExecResult handleTrap(VM &vm,
                          Frame &fr,
                          const il::core::Instr &in,
                          const VM::BlockMap &blocks,
                          const il::core::BasicBlock *&bb,
                          size_t &ip) {
    (void)blocks;
    (void)bb;
    (void)ip;

    switch (in.op) {
        case il::core::Opcode::Trap:
            vm_raise(TrapKind::DomainError);
            break;
        case il::core::Opcode::TrapFromErr: {
            if (in.operands.empty()) {
                vm_raise(TrapKind::RuntimeError);
                break;
            }
            Slot codeSlot = VMAccess::eval(vm, fr, in.operands[0]);
            const auto trapKind = map_err_to_trap(static_cast<int32_t>(codeSlot.i64));
            vm_raise(trapKind, static_cast<int32_t>(codeSlot.i64));
            break;
        }
        default:
            vm_raise(TrapKind::RuntimeError);
            break;
    }
    VM::ExecResult result{};
    result.returned = true;
    return result;
}

} // namespace il::vm::detail::control
