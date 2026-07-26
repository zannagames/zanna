//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/VMContext.cpp
// Purpose: Thread-local VM binding and execution context helpers.
//
// This file implements the thread-local VM binding infrastructure and helper
// utilities that expose the execution context of the virtual machine. The
// routines centralize trap handling, operand evaluation, and debug forwarding
// so that individual dispatch strategies can share behavior without duplicating
// state management.
//
// Sections:
//
//   1. THREAD-LOCAL VM BINDING
//      - tlsActiveVM: Thread-local pointer to the active VM
//      - ActiveVMGuard: RAII guard for installing/restoring thread-local VM
//      - activeVMInstance(): Query the currently active VM
//
//   2. VMCONTEXT HELPERS
//      - eval(): Evaluate IL values to Slots
//      - stepOnce(): Execute single interpreter step
//      - handleTrapDispatch(): Forward trap signals to handlers
//      - trapUnimplemented(): Report missing opcode implementations
//
//   3. VM CONVENIENCE WRAPPERS
//      - VM::eval(), VM::stepOnce(), etc. that delegate to VMContext
//
// Key invariants:
//   - Only one VM may be active per thread at a time
//   - ActiveVMGuard must be used for all VM execution entry points
//   - Thread-local binding enables trap bridges to find the active interpreter
//
// Ownership/Lifetime:
//   - VMContext holds a non-owning pointer to its VM
//   - ActiveVMGuard saves and restores previous VM on scope exit
//
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements thread-local VM activation and the dispatch-context facade.
/// @details The helpers in this file centralize runtime-context binding,
///          operand evaluation, tracing, opcode dispatch, and trap forwarding
///          for every interpreter strategy.

#include "vm/VMContext.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Value.hpp"
#include "vm/Marshal.hpp"
#include "vm/OpcodeHandlerHelpers.hpp"
#include "vm/RuntimeBridge.hpp"
#include "vm/Trap.hpp"

#include "rt_context.h"

#include <cassert>
#include <exception>
#include <string>

namespace il::vm {

//===----------------------------------------------------------------------===//
// Section 1: THREAD-LOCAL VM BINDING
//===----------------------------------------------------------------------===//

namespace {
thread_local VM *tlsActiveVM = nullptr; ///< Active VM for trap reporting.

/// @brief Resolve a human-readable mnemonic for an opcode.
/// @details Consults the opcode metadata table for a printable mnemonic and
///          falls back to a numbered placeholder when the entry lacks a name.
///          This guarantees that trap diagnostics always include a descriptive
///          token even for experimental opcodes.
/// @param op Opcode whose textual mnemonic is required.
/// @return Canonical mnemonic or a fallback string such as "opcode#17".
std::string opcodeMnemonic(il::core::Opcode op) {
    const size_t index = static_cast<size_t>(op);
    if (index < il::core::kNumOpcodes) {
        const auto &info = getOpcodeInfo(op);
        if (info.name && info.name[0] != '\0')
            return info.name;
    }
    return std::string("opcode#") + std::to_string(static_cast<int>(op));
}

} // namespace

/// @brief Install a VM instance as the thread-local active VM and bind its runtime context.
/// @details Guards set the thread-local pointer on construction and record the
///          previously active VM so trap reporting can access the current
///          interpreter without explicit plumbing at each call site. Also binds
///          the VM's runtime context so C runtime calls access this VM's state.
///
///          In debug builds, asserts if a *different* VM is already active on this
///          thread. Activating the same VM again (nested guard) is permitted since
///          this occurs legitimately in recursive function calls within the VM.
///
/// @param vm VM instance that will be considered active for the guard scope.
ActiveVMGuard::ActiveVMGuard(VM *vm)
    : previous(tlsActiveVM), current(vm), previousRtContext(rt_get_current_context()) {
    // Debug assertion: Catch accidental re-entry with a *different* VM.
    // Re-activating the same VM is allowed (nested calls within the interpreter).
    // Activating nullptr is allowed (clearing active state).
    assert((tlsActiveVM == nullptr || tlsActiveVM == vm || vm == nullptr) &&
           "ActiveVMGuard: attempting to activate a different VM while one is already active "
           "on this thread. Each VM instance must be used by only one thread at a time.");

    tlsActiveVM = vm;
    // Bind the VM's runtime context to this thread
    if (vm && vm->programState_ && vm->programState_->rtContext) {
        rt_set_current_context(vm->programState_->rtContext.get());
    }
}

/// @brief Restore the previously active VM and runtime context when the guard leaves scope.
/// @details Resets the thread-local pointer to the saved predecessor so nested
///          guards correctly restore whichever VM was running before the most
///          recent activation. Also restores the previous runtime context.
///
///          In debug builds, asserts that the thread-local pointer was not unexpectedly
///          modified during the guard's lifetime (e.g., by another guard on a different VM).
ActiveVMGuard::~ActiveVMGuard() {
    // Debug assertion: Verify the thread-local pointer wasn't tampered with.
    // It should still point to the VM we installed, unless nested guards ran.
    assert((tlsActiveVM == current || tlsActiveVM == nullptr) &&
           "ActiveVMGuard: tlsActiveVM was modified unexpectedly during guard lifetime. "
           "This may indicate a concurrency bug or mismatched guard lifetimes.");

    // Restore the previously bound runtime context, which may not correspond
    // to a VM-owned RtContext (e.g., native threads inheriting rt_legacy_context()).
    rt_set_current_context(previousRtContext);
    tlsActiveVM = previous;
}

//===----------------------------------------------------------------------===//
// Section 2: VMCONTEXT HELPERS
//===----------------------------------------------------------------------===//

/// @brief Bind the context helper to a specific VM instance.
/// @details Stores the pointer to the owning VM so future helper calls can
///          delegate directly without incurring repeated lookups.
/// @param vm VM whose helpers should be exposed via this context.
VMContext::VMContext(VM &vm) noexcept : vmInstance(&vm) {
#if ZANNA_VM_OPCOUNTS
    config.enableOpcodeCounts = vm.enableOpcodeCounts;
#else
    config.enableOpcodeCounts = false;
#endif
}

/// @brief Evaluate an IL value within the current frame.
/// @details Delegates to VM::eval() which handles temporaries, integer/float
///          immediates, string literals, and global references. This wrapper
///          ensures VMContext callers have a consistent interface while avoiding
///          code duplication with the core VM evaluation logic.
/// @param fr Frame providing registers and pending literals.
/// @param value IL value to evaluate.
/// @return Slot populated with the evaluated payload.
Slot VMContext::eval(Frame &fr, const il::core::Value &value) const {
    return vmInstance->eval(fr, value);
}

/// @brief Execute a single interpreter step for the bound VM.
/// @details Selects the next instruction, forwards it to the tracer for
///          debugging visibility, executes it via the VM's opcode handlers, and
///          performs dispatch finalisation.  When the VM signals exit, the
///          pending result is surfaced; otherwise @c std::nullopt indicates that
///          execution should continue.
/// @param state Mutable execution state for the active frame.
/// @return Optional slot representing a completed execution result.
std::optional<Slot> VMContext::stepOnce(VM::ExecState &state) const {
    vmInstance->beginDispatch(state);

    const il::core::Instr *instr = nullptr;
    if (!vmInstance->selectInstruction(state, instr))
        return state.hasPendingResult ? std::optional<Slot>(state.pendingResult) : std::nullopt;

    // Dispatch hook before executing the opcode (counts, etc.).
#if ZANNA_VM_OPCOUNTS
    ZANNA_VM_DISPATCH_BEFORE((*this), instr->op);
#endif

    vmInstance->traceInstruction(*instr, state.fr);
    auto result = vmInstance->executeOpcode(state.fr, *instr, *state.blocks, state.bb, state.ip);
    if (vmInstance->finalizeDispatch(state, result))
        return state.hasPendingResult ? std::optional<Slot>(state.pendingResult) : std::nullopt;

    return std::nullopt;
}

/// @brief Handle a trap dispatch request emitted by the runtime bridge.
/// @details Compares the signal target with the supplied execution state and, on
///          a match, clears the VM's notion of the current context so that trap
///          handlers regain control without observing stale metadata.
/// @param signal Trap dispatch payload generated by @ref RuntimeBridge.
/// @param state Execution state potentially referenced by the signal.
/// @return @c true if the signal referred to @p state.
bool VMContext::handleTrapDispatch(const VM::TrapDispatchSignal &signal,
                                   VM::ExecState &state) const {
    if (signal.target != &state)
        return false;
    vmInstance->clearCurrentContext();
    return true;
}

/// @brief Inspect the opcode that would execute for the provided state.
/// @details Initiates dispatch to synchronise the instruction pointer before
///          returning the pending opcode.  When dispatch fails the trap opcode is
///          reported so debugging tools still receive a meaningful value.
/// @param state Execution state being inspected.
/// @return Opcode slated for execution.
il::core::Opcode VMContext::fetchOpcode(VM::ExecState &state) const {
    vmInstance->beginDispatch(state);

    const il::core::Instr *instr = nullptr;
    if (!vmInstance->selectInstruction(state, instr))
        return instr ? instr->op : il::core::Opcode::Trap;

    return instr->op;
}

/// @brief Propagate an inline execution result through the VM finalisation path.
/// @details Delegates to @ref VM::finalizeDispatch so inline handlers can reuse
///          the same clean-up logic as the main interpreter loop.
/// @param state Execution state receiving the result.
/// @param exec Result produced by an inline opcode handler.
void VMContext::handleInlineResult(VM::ExecState &state, const VM::ExecResult &exec) const {
    vmInstance->finalizeDispatch(state, exec);
}

/// @brief Report an unimplemented opcode.
/// @details Builds a trap message containing the opcode mnemonic and current
///          execution context before routing it through the runtime bridge.
///          Production trap handlers terminate or redirect control, while test
///          observers may return after recording the failure.
/// @param opcode Opcode lacking an implementation.
void VMContext::trapUnimplemented(il::core::Opcode opcode) const {
    const auto ctx = vmInstance->currentTrapContext();
    const std::string funcName = ctx.function ? ctx.function->name : std::string("<unknown>");
    const std::string blockLabel = ctx.block ? ctx.block->label : std::string();
    std::string detail = "unimplemented opcode: " + opcodeMnemonic(opcode);
    if (!blockLabel.empty())
        detail += " (block " + blockLabel + ')';
    RuntimeBridge::trap(TrapKind::InvalidOperation, detail, ctx.loc, funcName, blockLabel);
    return;
}

/// @brief Forward trace events to the underlying VM tracer.
/// @details Calls into @ref VM::traceInstruction so that instrumentation lives
///          in one place regardless of whether the interpreter is executing
///          inline or through the main dispatch loop.
/// @param instr Instruction being executed.
/// @param frame Active frame at the time of tracing.
void VMContext::traceStep(const il::core::Instr &instr, Frame &frame) const {
    vmInstance->traceInstruction(instr, frame);
}

/// @brief Delegate opcode execution to the owning VM.
/// @details Thin wrapper that forwards to @ref VM::executeOpcode, keeping the
///          context helper responsible for routing rather than implementing the
///          opcode semantics itself.
/// @param frame Active execution frame.
/// @param instr Instruction currently being executed.
/// @param blocks Cached block lookup for branch resolution.
/// @param bb Reference to the current basic block pointer.
/// @param ip Reference to the instruction index within @p bb.
/// @return Execution result capturing exit/trap status.
VM::ExecResult VMContext::executeOpcode(Frame &frame,
                                        const il::core::Instr &instr,
                                        const VM::BlockMap &blocks,
                                        const il::core::BasicBlock *&bb,
                                        size_t &ip) const {
    return vmInstance->executeOpcode(frame, instr, blocks, bb, ip);
}

/// @brief Clear the VM's notion of the current execution context.
/// @details Calls the owning VM's reset routine so subsequent traps do not refer
///          to stale frame metadata.
void VMContext::clearCurrentContext() const {
    vmInstance->clearCurrentContext();
}

/// @brief Access the trace sink used by the VM.
/// @details Exposes the tracer so callers running through the helper can emit
///          trace events without reaching directly into the VM internals.
/// @return Reference to the trace sink owned by the VM.
TraceSink &VMContext::traceSink() const noexcept {
    return vmInstance->tracer;
}

/// @brief Access the debug controller associated with the VM.
/// @details Provides mutable access so tooling can configure breakpoints while
///          still routing through the shared context helpers.
/// @return Reference to the debug controller.
DebugCtrl &VMContext::debugController() const noexcept {
    return vmInstance->debug;
}

/// @brief Access the underlying VM instance.
/// @details Serves as a convenience for helpers that need to reach escape hatches
///          on the owning VM.
/// @return Reference to the VM bound to this context.
VM *VMContext::vm() const noexcept {
    return vmInstance;
}

/// @brief Retrieve the currently active VM for the calling thread.
/// @details Returns the thread-local pointer established by
///          @ref ActiveVMGuard so trap bridges and other facilities can discover
///          the active interpreter.
///
///          This function may return nullptr if no VM is currently active on the
///          calling thread. Callers that require a valid VM should either:
///          - Check the return value before use, or
///          - Use the debug assertion variant that traps on nullptr (internal use)
///
/// @return Pointer to the VM previously installed via @ref ActiveVMGuard,
///         or nullptr if no VM is active.
VM *activeVMInstance() {
    return tlsActiveVM;
}

//===----------------------------------------------------------------------===//
// Section 3: VM CONVENIENCE WRAPPERS
//===----------------------------------------------------------------------===//

/// @brief Evaluate an IL value using a temporary context helper.
/// @details Optimized for the hot path: Temp values with valid register indices.
///          Other value kinds delegate to helper functions to keep the fast path
///          compact and cache-friendly.
/// @param fr Frame providing registers and runtime state.
/// @param value IL value to evaluate.
/// @return Slot populated with the evaluated payload.
Slot VM::eval(Frame &fr, const il::core::Value &value) {
    // Hot path: Temp values are the most common operand type in typical IL.
    // Inline the fast path (valid register index) directly to avoid any
    // lambda or function call overhead. The error path is cold and can
    // afford the extra work.
    if (value.kind == il::core::Value::Kind::Temp) [[likely]] {
        if (value.id < fr.regs.size()) [[likely]]
            return fr.regs[value.id];

        // Cold path: out-of-range register access - report detailed error
        const auto ctx = currentTrapContext();
        const std::string fnName = fr.func ? fr.func->name : std::string("<unknown>");
        const il::core::BasicBlock *block = ctx.block;
        const std::string blockLabel = block ? block->label : std::string();
        const auto loc = ctx.loc;

        std::string message =
            detail::formatRegisterRangeError(value.id, fr.regs.size(), fnName, blockLabel);
        if (loc.hasLine()) {
            message.append(", at line ");
            message.append(std::to_string(loc.line));
            if (loc.hasColumn()) {
                message.push_back(':');
                message.append(std::to_string(loc.column));
            }
        } else {
            message.append(", at unknown location");
        }
        RuntimeBridge::trap(TrapKind::InvalidOperation, message, loc, fnName, blockLabel);
    }

    // Second hot path: integer constants are very common
    if (value.kind == il::core::Value::Kind::ConstInt) {
        Slot slot;
        slot.i64 = value.i64;
        return slot;
    }

    // Third hot path: floating-point constants
    if (value.kind == il::core::Value::Kind::ConstFloat) {
        Slot slot;
        slot.f64 = value.f64;
        return slot;
    }

    // Cold paths: string constants, global addresses, null pointers
    // These are less frequent and can afford the switch overhead
    switch (value.kind) {
        case il::core::Value::Kind::ConstStr: {
            Slot s{};
            // Fast path: lookup in pre-populated cache (CRITICAL-3 optimization).
            // The cache is populated during VM construction, so this find() should
            // succeed for all string literals in the module. The try_emplace fallback
            // handles edge cases like dynamically generated strings.
            auto it = inlineLiteralCache.find(value.str);
            if (it != inlineLiteralCache.end()) {
                s.str = it->second.get();
                return s;
            }
            // Cold path: string not in cache, insert it
            auto [insertIt, inserted] = inlineLiteralCache.try_emplace(value.str);
            if (inserted) {
                if (value.str.find('\0') == std::string::npos)
                    insertIt->second = ZannaStringHandle(rt_const_cstr(value.str.c_str()));
                else
                    insertIt->second =
                        ZannaStringHandle(rt_string_from_bytes(value.str.data(), value.str.size()));
            }
            s.str = insertIt->second.get();
            return s;
        }
        case il::core::Value::Kind::GlobalAddr: {
            Slot s{};
            // Map to function pointer when name matches a function
            auto fIt = fnMap.find(value.str);
            if (fIt != fnMap.end()) {
                s.ptr = const_cast<il::core::Function *>(fIt->second);
                return s;
            }

            // Check mutable globals
            auto mIt = programState_->mutableGlobalMap.find(value.str);
            if (mIt != programState_->mutableGlobalMap.end()) {
                s.ptr = mIt->second;
                return s;
            }

            // Fall back to const string globals
            auto it = programState_->strMap.find(value.str);
            if (it == programState_->strMap.end()) {
                RuntimeBridge::trap(TrapKind::DomainError,
                                    "unknown global",
                                    {},
                                    fr.func ? fr.func->name : std::string{},
                                    "");
                return {};
            } else {
                s.str = it->second.get();
            }
            return s;
        }
        case il::core::Value::Kind::NullPtr: {
            Slot slot;
            slot.ptr = nullptr;
            return slot;
        }
        default:
            RuntimeBridge::trap(TrapKind::InvalidOperation,
                                "eval: unexpected value kind",
                                {},
                                fr.func ? fr.func->name : std::string{},
                                "");
            return {};
    }
}

/// @brief Execute a single interpreter step on behalf of the VM.
/// @details Delegates to @ref VMContext::stepOnce using a temporary context so
///          callers interact with a stable API while the shared helpers retain
///          the core control flow logic.
/// @param state Mutable execution state for the active frame.
/// @return Optional slot containing the program result when execution finished.
std::optional<Slot> VM::stepOnce(ExecState &state) {
    ActiveVMGuard active(this);
    VMContext ctx(*this);

    // Use the shared ExecStackGuard from VM.hpp (pre-allocated stack avoids heap allocs)
    ExecStackGuard guard(*this, state);

    try {
        return ctx.stepOnce(state);
    } catch (const VM::TrapDispatchSignal &signal) {
        if (!ctx.handleTrapDispatch(signal, state))
            throw;
        // Trap dispatched; no completed result yet.
        return std::nullopt;
    }
}

/// @brief Forward a trap dispatch signal to the shared context helpers.
/// @details Routes the signal through @ref VMContext::handleTrapDispatch so
///          the invalidation logic remains centralised in the helper.
/// @param signal Trap dispatch payload generated by @ref RuntimeBridge.
/// @param state Execution state potentially referenced by the signal.
/// @return @c true when the signal targets @p state.
bool VM::handleTrapDispatch(const TrapDispatchSignal &signal, ExecState &state) {
    VMContext ctx(*this);
    return ctx.handleTrapDispatch(signal, state);
}

/// @brief Inspect the opcode that would execute for the provided state.
/// @details Convenience wrapper that forwards to
///          @ref VMContext::fetchOpcode while hiding the context construction.
/// @param state Execution state being inspected.
/// @return Opcode slated for execution.
il::core::Opcode VM::fetchOpcode(ExecState &state) {
    VMContext ctx(*this);
    return ctx.fetchOpcode(state);
}

/// @brief Propagate an inline execution result through the shared helpers.
/// @details Instantiates a context and forwards to
///          @ref VMContext::handleInlineResult so inline opcode handlers share
///          the same finalisation behaviour as the main interpreter.
/// @param state Execution state receiving the result.
/// @param exec Result produced by an inline opcode handler.
void VM::handleInlineResult(ExecState &state, const ExecResult &exec) {
    VMContext ctx(*this);
    ctx.handleInlineResult(state, exec);
}

/// @brief Report an unimplemented opcode using the shared context helpers.
/// @details Creates a context and reuses
///          @ref VMContext::trapUnimplemented to emit diagnostics before
///          terminating.
/// @param opcode Opcode lacking an implementation.
void VM::trapUnimplemented(il::core::Opcode opcode) {
    VMContext ctx(*this);
    ctx.trapUnimplemented(opcode);
    return;
}

/// @brief Retrieve the currently active VM for the calling thread.
/// @details Static convenience that simply forwards to the free-standing
///          @ref activeVMInstance helper.
/// @return Pointer to the active VM or @c nullptr when none is set.
VM *VM::activeInstance() {
    return activeVMInstance();
}

} // namespace il::vm
