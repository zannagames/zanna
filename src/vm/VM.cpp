//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/VM.cpp
// Purpose: Virtual machine execution engine implementation.
//
// This file implements the core VM execution engine organized into sections:
//
//   1. DISPATCH DRIVER INTERFACE AND IMPLEMENTATIONS
//      - DispatchDriver abstract interface for pluggable dispatch strategies
//      - FnTableDispatchDriver: function-table-based dispatch
//      - SwitchDispatchDriver: switch-statement-based dispatch
//      - ThreadedDispatchDriver: computed-goto threading (when supported)
//
//   2. TRAP DISPATCH SIGNAL
//      - TrapDispatchSignal exception for transferring control to handlers
//
//   3. CORE INTERPRETER LOOP
//      - run(): Entry point that locates and executes main()
//      - executeOpcode(): Single instruction dispatch
//      - runFunctionLoop(): Main interpreter loop
//      - beginDispatch/selectInstruction/finalizeDispatch: Loop helpers
//
//   4. EXECUTION CONTEXT MANAGEMENT
//      - setCurrentContext/clearCurrentContext: Track current instruction
//      - Opcode counting infrastructure (when ZANNA_VM_OPCOUNTS enabled)
//
//   5. TRAP HANDLING
//      - prepareTrap(): Walk stack to find handlers, wire up resume state
//      - throwForTrap(): Transfer control via exception
//
// Key invariants:
//   - Dispatch strategies are interchangeable without changing loop logic
//   - Thread-local VM binding (via ActiveVMGuard) enables trap reporting
//   - Cross-function execution uses the exec stack for handler lookup
//
// Ownership/Lifetime:
//   - VM owns dispatch drivers, string caches, and runtime context
//   - Module data is borrowed from callers
//
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the VM execution engine, dispatch drivers, interrupt
///        delivery, lifecycle, profiling, context tracking, and trap routing.
/// @details The implementation coordinates interchangeable dispatch strategies
///          with shared frame state and process-level interrupt epochs while
///          preserving per-VM diagnostics and runtime resource ownership.

#include "vm/VM.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Value.hpp"
#include "vm/DispatchStrategy.hpp"
#include "vm/OpHandlerAccess.hpp"
#include "vm/OpHandlers.hpp"
#include "vm/RuntimeBridge.hpp"
#include "vm/VMConstants.hpp"
#include "vm/VMContext.hpp"

#include "rt_context.h"
#include "rt_parallel.h"
#include "rt_shutdown.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace il::vm {

// =============================================================================
// Platform interrupt support
// =============================================================================
// A VM-level interrupt is requested when the user presses Ctrl-C (or when the
// host explicitly calls VM::requestInterrupt()). The request is published as a
// monotonically increasing epoch so every running VM instance observes it
// independently instead of the first VM clearing a process-global flag for the
// others.

/// @brief Monotonic epoch incremented for every process-wide interrupt request.
static std::atomic<uint64_t> s_interruptEpoch{0};
/// @brief Most recent epoch explicitly cleared by the host.
static std::atomic<uint64_t> s_interruptClearedEpoch{0};

/// @brief Publish a process-wide interrupt request from normal execution code.
/// @details This helper uses C++ atomics and is therefore called only outside
///          POSIX signal-handler context. POSIX signals set a sig_atomic_t flag
///          that is later folded into this epoch by @ref publishPendingSignalInterrupt.
/// @param reason Runtime shutdown reason published with the request.
static void publishInterruptRequest(int64_t reason = RT_SHUTDOWN_REASON_INTERRUPT) noexcept {
    rt_shutdown_request(reason);
    s_interruptEpoch.fetch_add(1, std::memory_order_relaxed);
}

#if defined(_WIN32)
/// @brief Translate a Windows console control event into a VM interrupt epoch.
/// @param ctrlType Windows control-event identifier.
/// @return Always @c TRUE to indicate that the event was handled.
static BOOL WINAPI windowsCtrlHandler(DWORD ctrlType) {
    int64_t reason = RT_SHUTDOWN_REASON_INTERRUPT;
    if (ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_LOGOFF_EVENT ||
        ctrlType == CTRL_SHUTDOWN_EVENT) {
        reason = RT_SHUTDOWN_REASON_TERMINATE;
    }
    publishInterruptRequest(reason);
    return TRUE; // We handled it; do not call the next handler.
}
#else
static volatile std::sig_atomic_t s_posixInterruptPending = 0;
static volatile std::sig_atomic_t s_posixTerminatePending = 0;

/// @brief Signal-safe SIGINT handler that marks an interrupt pending.
static void posixSigintHandler(int /*signum*/) {
    s_posixInterruptPending = 1;
}

/// @brief Signal-safe SIGTERM handler that marks termination pending.
static void posixSigtermHandler(int /*signum*/) {
    s_posixTerminatePending = 1;
}

/// @brief Fold a POSIX signal-handler flag into the atomic interrupt epoch.
/// @details The handler itself only writes sig_atomic_t state. Interpreter
///          threads call this helper from ordinary execution context where
///          using C++ atomics is well-defined.
static void publishPendingSignalInterrupt() noexcept {
    if (s_posixInterruptPending) {
        s_posixInterruptPending = 0;
        publishInterruptRequest(RT_SHUTDOWN_REASON_INTERRUPT);
    }
    if (s_posixTerminatePending) {
        s_posixTerminatePending = 0;
        publishInterruptRequest(RT_SHUTDOWN_REASON_TERMINATE);
    }
}
#endif

/// @brief Runtime shutdown callback that clears the current interrupt epoch.
static void clearInterruptForShutdownPoll() noexcept {
    VM::clearInterrupt();
}

/// @brief Register the process-level interrupt handler (called once per process).
static void registerInterruptHandler() {
    // Guard against redundant registration when multiple VMs are created.
    static std::atomic<bool> registered{false};
    bool expected = false;
    if (!registered.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

#if defined(_WIN32)
    SetConsoleCtrlHandler(windowsCtrlHandler, TRUE);
#else
    struct sigaction sa{};
    sa.sa_handler = posixSigintHandler;
    sigemptyset(&sa.sa_mask);
    // SA_RESTART: resume interrupted system calls where possible.
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);

    struct sigaction termSa{};
    termSa.sa_handler = posixSigtermHandler;
    sigemptyset(&termSa.sa_mask);
    termSa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &termSa, nullptr);
#endif
    rt_shutdown_set_interrupt_clear_callback(clearInterruptForShutdownPoll);
}

//===----------------------------------------------------------------------===//
// Section 1: DISPATCH DRIVER INTERFACE AND IMPLEMENTATIONS
//===----------------------------------------------------------------------===//

/// @brief Retrieve the textual mnemonic associated with an opcode.
/// @details Defined in the opcode metadata translation unit; declared here so
///          diagnostic helpers can render human-readable opcode names when the
///          VM encounters unimplemented instructions.
/// @param op Opcode to describe.
/// @return Null-terminated mnemonic string.
std::string opcodeMnemonic(il::core::Opcode op);

using il::core::BasicBlock;
using il::core::Function;
using il::core::Instr;
using il::core::Opcode;
using il::core::Value;

/// @brief Interface for pluggable interpreter dispatch strategies.
struct VM::DispatchDriver {
    /// @brief Ensure derived classes clean up correctly.
    virtual ~DispatchDriver() = default;

    /// @brief Execute the dispatch loop until the VM requests an exit.
    /// @details Implementations fetch instructions from @p state, invoke the
    ///          appropriate opcode handler, and update interpreter bookkeeping.
    /// @param vm Owning virtual machine instance.
    /// @param context Shared VM context (debugging/traps/etc.).
    /// @param state Execution state being driven.
    /// @return True when the dispatch loop terminated normally; false when the
    ///         VM asked to pause.
    virtual bool run(VM &vm, VMContext &context, ExecState &state) = 0;
};

/// @brief Custom deleter for dispatch-driver unique pointers.
/// @details Allows @c std::unique_ptr to own forward-declared dispatch
///          implementations without exposing their concrete types to headers.
/// @param driver Heap-allocated driver to destroy.
void VM::DispatchDriverDeleter::operator()(DispatchDriver *driver) const {
    delete driver;
}

/// @brief Create the dispatch strategy selected by @p kind.
/// @param kind Requested interpreter dispatch mechanism.
/// @return Owning strategy instance, with unsupported modes falling back to
///         the portable switch implementation.
std::unique_ptr<DispatchStrategy> createDispatchStrategy(VM::DispatchKind kind);

namespace detail {

/// @brief Dispatch driver that uses an opcode-to-function table.
class FnTableDispatchDriver final : public VM::DispatchDriver {
  private:
    std::unique_ptr<DispatchStrategy> strategy;

  public:
    /// @brief Construct a driver using the function-table dispatch strategy.
    /// @details The driver exclusively owns the strategy used by subsequent
    ///          calls to @ref run.
    FnTableDispatchDriver() : strategy(createDispatchStrategy(VM::DispatchKind::FnTable)) {}

    /// @brief Execute the interpreter using the function-table strategy.
    /// @details Delegates to the shared dispatch loop with the function table strategy.
    /// @param vm Virtual machine instance driving execution.
    /// @param ctx VM context for trap and debug handling.
    /// @param state Execution state being advanced.
    /// @return True when the VM exited cleanly, false when a pause was requested.
    bool run(VM &vm, VMContext &ctx, VM::ExecState &state) override {
        return runSharedDispatchLoop(vm, ctx, state, *strategy);
    }
};

/// @brief Dispatch driver that expands handlers as a large switch statement.
class SwitchDispatchDriver final : public VM::DispatchDriver {
  private:
    std::unique_ptr<DispatchStrategy> strategy;

  public:
    /// @brief Construct a driver using the portable switch dispatch strategy.
    /// @details The driver exclusively owns the strategy used by subsequent
    ///          calls to @ref run.
    SwitchDispatchDriver() : strategy(createDispatchStrategy(VM::DispatchKind::Switch)) {}

    /// @brief Execute the interpreter using a switch-based dispatch loop.
    /// @details Delegates to the shared dispatch loop with the switch strategy.
    /// @param vm Virtual machine instance.
    /// @param ctx VM context for trap and debug handling.
    /// @param state Execution state being advanced.
    /// @return True when the VM exited normally, false when paused.
    bool run(VM &vm, VMContext &ctx, VM::ExecState &state) override {
        return runSharedDispatchLoop(vm, ctx, state, *strategy);
    }
};

#if ZANNA_THREADING_SUPPORTED
/// @brief Dispatch driver that uses computed goto threading.
/// @note This implementation uses an optimized fast-path dispatch that inlines
///       the common case (next instruction in same block) to minimize overhead.
///       The slow path handles block transitions, debug hooks, and exit checks.
class ThreadedDispatchDriver final : public VM::DispatchDriver {
  public:
    /// @brief Execute the interpreter using computed gotos for dispatch.
    /// @details Uses an optimized dispatch loop that inlines the fast path:
    ///          1. Increment ip
    ///          2. Check bounds (unlikely branch)
    ///          3. Fetch next instruction directly
    ///          4. Dispatch via computed goto
    ///          The slow path handles block exhaustion, debug hooks, and exit.
    /// @param vm Virtual machine instance.
    /// @param context Context used to handle traps and debugging requests.
    /// @param state Execution state under control of the dispatcher.
    /// @return True when execution terminated normally; false otherwise.
    bool run(VM &vm, VMContext &context, VM::ExecState &state) override {
// Label addresses (`&&LBL_x`) and computed gotos (`goto *p`) are GNU
// extensions that ISO C++ does not define. They are the point of this
// dispatcher, so the pedantic diagnostics are suppressed for the body only.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
        // Cache frequently accessed state in locals for faster access
        const il::core::Instr *currentInstr = nullptr;
        il::core::Opcode opcode = il::core::Opcode::Trap;

        // =====================================================================
        // Threaded Dispatch: Label Address Table (Generated)
        // =====================================================================
        static void *kOpLabels[] = {
#include "vm/ops/generated/ThreadedLabels.inc"
        };

        static constexpr size_t kOpLabelCount = sizeof(kOpLabels) / sizeof(kOpLabels[0]);
        static_assert(kOpLabelCount == il::core::kNumOpcodes + 1,
                      "Threaded label table size mismatch: expected kNumOpcodes + 1 labels");

        // =====================================================================
        // Fast-path dispatch macro: inlines the common case
        // =====================================================================
        // The common case after executing an instruction is:
        //   1. ip++ stays within current block
        //   2. No exit requested
        //   3. No debug pause needed
        // This macro inlines that path to avoid function call overhead.
        // =====================================================================
#define DISPATCH_TO(OPCODE_VALUE)                                                                  \
    do {                                                                                           \
        size_t index = static_cast<size_t>(OPCODE_VALUE);                                          \
        if (index >= kOpLabelCount - 1) [[unlikely]]                                               \
            index = kOpLabelCount - 1;                                                             \
        goto *kOpLabels[index];                                                                    \
    } while (false)

        // Fast-path: dispatch next instruction inline
        // Note: ip is already incremented by handleInlineResult->finalizeDispatch
        // Only falls through to slow path on block exhaustion, exit, or active tracing/debug.
        //
        // Optimizations vs slow path:
        //   - No setCurrentContext (reconstructed lazily from ExecState on traps)
        //   - No traceInstruction (only counts/traces on slow path or block transitions)
        //   - Minimal work: fetch, classify opcode, goto
        //
        // When tracingActive_ is set, we fall through to the slow path which
        // handles setCurrentContext, traceInstruction, and shouldPause.
#define DISPATCH_NEXT_FAST()                                                                       \
    do {                                                                                           \
        if (state.ip < state.bb->instructions.size()) [[likely]] {                                 \
            if (vm.tracingActive_ || vm.stepBudget > 0 ||                                          \
                vm.debugStepMode_ != VM::DebugStepMode::None || vm.debugBreakActive_) [[unlikely]] \
                goto LBL_SLOW_PATH;                                                                \
            ++vm.instrCount;                                                                       \
            currentInstr = &state.bb->instructions[state.ip];                                      \
            state.currentInstr = currentInstr;                                                     \
            opcode = currentInstr->op;                                                             \
            ZANNA_VM_DISPATCH_BEFORE(state, opcode);                                               \
            DISPATCH_TO(opcode);                                                                   \
        }                                                                                          \
        goto LBL_SLOW_PATH;                                                                        \
    } while (false)

        for (;;) {
            vm.clearCurrentContext();
            try {
                // =============================================================
                // Slow path entry: handles first instruction and block transitions
                // =============================================================
                // Reset per-dispatch state
                state.exitRequested = false;
                state.hasPendingResult = false;

                // Check for block exhaustion
                if (!state.bb || state.ip >= state.bb->instructions.size()) [[unlikely]] {
                    vm.clearCurrentContext();
                    Slot zero{};
                    zero.i64 = 0;
                    state.pendingResult = zero;
                    state.hasPendingResult = true;
                    state.exitRequested = true;
                    return true;
                }

                // Fetch current instruction
                currentInstr = &state.bb->instructions[state.ip];
                state.currentInstr = currentInstr;

                // Update context for diagnostics (only on slow path)
                vm.setCurrentContext(state.fr, state.bb, state.ip, *currentInstr);

                // Check for debug pause (only on slow path - rare)
                if (auto pause = vm.shouldPause(state, currentInstr, false)) [[unlikely]]
                {
                    state.pendingResult = *pause;
                    state.hasPendingResult = true;
                    state.exitRequested = true;
                    return true;
                }

                opcode = currentInstr->op;
                vm.traceInstruction(*currentInstr, state.fr);
                ZANNA_VM_DISPATCH_BEFORE(state, opcode);
                DISPATCH_TO(opcode);

                // =============================================================
                // Slow path: reached after DISPATCH_NEXT_FAST detects block end
                // =============================================================
            LBL_SLOW_PATH:
                // Check for exit after handler (e.g., Ret instruction)
                if (state.exitRequested) [[unlikely]]
                    return true;

                // Reset per-dispatch state for next iteration
                state.exitRequested = false;
                state.hasPendingResult = false;

                // Check for block exhaustion
                if (!state.bb || state.ip >= state.bb->instructions.size()) [[unlikely]] {
                    vm.clearCurrentContext();
                    Slot zero{};
                    zero.i64 = 0;
                    state.pendingResult = zero;
                    state.hasPendingResult = true;
                    state.exitRequested = true;
                    return true;
                }

                // Fetch current instruction
                currentInstr = &state.bb->instructions[state.ip];
                state.currentInstr = currentInstr;

                // Update context for diagnostics
                vm.setCurrentContext(state.fr, state.bb, state.ip, *currentInstr);

                // Check for debug pause
                if (auto pause = vm.shouldPause(state, currentInstr, false)) [[unlikely]]
                {
                    state.pendingResult = *pause;
                    state.hasPendingResult = true;
                    state.exitRequested = true;
                    return true;
                }

                opcode = currentInstr->op;
                vm.traceInstruction(*currentInstr, state.fr);
                ZANNA_VM_DISPATCH_BEFORE(state, opcode);
                DISPATCH_TO(opcode);

                // =============================================================
                // Threaded Dispatch: Case Labels (Generated)
                // =============================================================
                // Each label handles one opcode then uses DISPATCH_NEXT_FAST()
                // to inline the fast path for fetching the next instruction.
                // =============================================================
#include "vm/ops/generated/ThreadedCases.inc"

            LBL_UNIMPL: { vm.trapUnimplemented(opcode); }
            } catch (const VM::TrapDispatchSignal &signal) {
                if (!context.handleTrapDispatch(signal, state))
                    throw;
            }
        }

#undef DISPATCH_NEXT_FAST
#undef DISPATCH_TO
        return false; // Unreachable but placates control-flow analysis.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    }
};
#endif // ZANNA_THREADING_SUPPORTED

} // namespace detail

//===----------------------------------------------------------------------===//
// Section 2: TRAP DISPATCH SIGNAL
//===----------------------------------------------------------------------===//

/// @brief Construct a trap dispatch signal targeting a specific execution state.
/// @param targetState Execution state that should resume after trap handling.
VM::TrapDispatchSignal::TrapDispatchSignal(ExecState *targetState) : target(targetState) {}

/// @brief Retrieve the diagnostic message associated with trap dispatch signals.
const char *VM::TrapDispatchSignal::what() const noexcept {
    return "VM trap dispatch";
}

//===----------------------------------------------------------------------===//
// Section 3: CORE INTERPRETER LOOP
//===----------------------------------------------------------------------===//

/// @brief Locate and execute the module's @c main function.
///
/// The entry point is looked up by name in the cached function map and then
/// executed via @c execFunction. Any tracing or debugging configured on the VM
/// applies to the entire run.
///
/// Workflow: look up @c main in @c fnMap, report a diagnostic when absent, and
/// otherwise call @c execFunction with an empty argument list before forwarding
/// its return value.
///
/// @returns Signed 64-bit exit code produced by the program's @c main function.
/// @retval 1 When the module lacks an entry point, after printing "missing main".
int64_t VM::run() {
    // Ensure Ctrl-C is caught and converted to a graceful trap rather than an
    // abrupt process kill.  Registration is idempotent — safe to call for every
    // top-level run() invocation including from threaded VMs.
    registerInterruptHandler();

    auto it = fnMap.find("main");
    if (it == fnMap.end()) {
        std::cerr << "missing main" << std::endl;
        return 1;
    }
    const Function &mainFn = *it->second;
    Slot result = execFunction(mainFn, {});
    switch (mainFn.retType.kind) {
        case il::core::Type::Kind::Void:
            return 0;
        case il::core::Type::Kind::I1:
        case il::core::Type::Kind::I16:
        case il::core::Type::Kind::I32:
        case il::core::Type::Kind::I64:
            return result.i64;
        case il::core::Type::Kind::Str:
            rt_str_release_maybe(result.str);
            RuntimeBridge::trap(TrapKind::InvalidOperation,
                                "main must return an integer or void",
                                {},
                                mainFn.name,
                                "");
            return 1;
        case il::core::Type::Kind::F64:
        case il::core::Type::Kind::Ptr:
        case il::core::Type::Kind::Error:
        case il::core::Type::Kind::ResumeTok:
            RuntimeBridge::trap(TrapKind::InvalidOperation,
                                "main must return an integer or void",
                                {},
                                mainFn.name,
                                "");
            return 1;
    }
    return 1;
}

/// @brief Request a graceful interrupt of any currently-running VM on this process.
/// Equivalent to the user pressing Ctrl-C.  Thread-safe.
void VM::requestInterrupt() noexcept {
    publishInterruptRequest();
}

/// @brief Clear any pending process-wide interrupt request.
void VM::clearInterrupt() noexcept {
    const uint64_t epoch = s_interruptEpoch.load(std::memory_order_acquire);
    s_interruptClearedEpoch.store(epoch, std::memory_order_release);
    rt_shutdown_clear_pending_only();
}

/// @brief Consume a process interrupt epoch not yet observed by this VM.
/// @return @c true exactly once per new, uncleared epoch for this VM instance.
bool VM::consumePendingInterrupt() noexcept {
#if !defined(_WIN32)
    publishPendingSignalInterrupt();
#endif
    const uint64_t cleared = s_interruptClearedEpoch.load(std::memory_order_acquire);
    if (lastObservedInterruptEpoch_ < cleared)
        lastObservedInterruptEpoch_ = cleared;

    const uint64_t epoch = s_interruptEpoch.load(std::memory_order_acquire);
    if (epoch == 0 || epoch <= cleared || epoch == lastObservedInterruptEpoch_)
        return false;

    lastObservedInterruptEpoch_ = epoch;
    return true;
}

/// @brief Raise an interrupt trap if this VM has not consumed the current interrupt epoch.
/// @details Called from the per-instruction dispatch hook and from coarser
///          function-loop boundaries. Returning true means a trap observer
///          chose to continue after the interrupt, so the dispatch state should
///          pause instead of executing more instructions.
/// @return True when an interrupt was delivered but control returned.
bool VM::pollPendingInterrupt() {
    if (!consumePendingInterrupt()) [[likely]]
        return false;

    if (rt_shutdown_polling_enabled() && rt_shutdown_has_pending())
        return false;

    RuntimeBridge::trap(TrapKind::Interrupt, "interrupted", {}, "", "");
    return true;
}

/// @brief Dispatch and execute a single IL instruction.
///
/// A handler is selected from a static table based on the opcode and invoked to
/// perform the operation. Handlers such as @c handleCall and @c handleTrap
/// communicate with the runtime bridge for foreign function calls or traps.
///
/// Workflow: index into the opcode handler table, validate the presence of a
/// handler, and invoke it to mutate @p fr, adjust control-flow bookkeeping, and
/// produce an execution result summarising the effects.
///
/// @param fr     Current frame.
/// @param in     Instruction to execute.
/// @param blocks Mapping of block labels used for branch resolution.
/// @param bb     [in,out] Updated to the current basic block after any branch.
/// @param ip     [in,out] Instruction index within @p bb.
/// @return Execution result capturing control-flow effects and return value.
VM::ExecResult VM::executeOpcode(
    Frame &fr, const Instr &in, const BlockMap &blocks, const BasicBlock *&bb, size_t &ip) {
    const size_t index = static_cast<size_t>(in.op);
    const auto &table = (handlerTable_ != nullptr) ? *handlerTable_ : getOpcodeHandlers();
    OpcodeHandler handler = index < table.size() ? table[index] : nullptr;
    if (!handler) {
        const std::string blockLabel = bb ? bb->label : std::string();
        std::string detail = "unimplemented opcode: " + opcodeMnemonic(in.op);
        if (!blockLabel.empty()) {
            detail += " (block " + blockLabel + ')';
        }
        RuntimeBridge::trap(TrapKind::InvalidOperation, detail, in.loc, fr.func->name, blockLabel);
    }
    return handler(*this, fr, in, blocks, bb, ip);
}

/// @brief Determine whether execution should pause before or after an instruction.
///
/// This forwards to @c processDebugControl so the centralised debug logic
/// remains in @c VMDebug.cpp while callers have a clear, intention-revealing
/// helper. A non-empty result signals a pause or termination condition that the
/// interpreter loop must honour immediately.
///
/// @param st      Current execution state.
/// @param in      Instruction involved in the decision or @c nullptr when none.
/// @param postExec True when invoked after executing @p in.
/// @return Optional slot containing the pause sentinel.
std::optional<Slot> VM::shouldPause(ExecState &st, const Instr *in, bool postExec) {
    return processDebugControl(st, in, postExec);
}

/// @brief Reset per-iteration state before dispatching an instruction.
/// @param state Execution state to prepare.
void VM::beginDispatch(ExecState &state) {
    state.exitRequested = false;
    state.hasPendingResult = false;
    state.currentInstr = nullptr;
}

/// @brief Select the next instruction to execute for the active state.
/// @details Handles basic-block exhaustion, updates the current context used for
///          diagnostics, and honours debugger pause requests triggered prior to
///          execution.
/// @param state Execution state being advanced.
/// @param instr Output pointer receiving the selected instruction when present.
/// @return False when execution should stop (due to exit or pause), true otherwise.
bool VM::selectInstruction(ExecState &state, const Instr *&instr) {
    if (!state.bb || state.ip >= state.bb->instructions.size()) [[unlikely]] {
        clearCurrentContext();
        Slot zero{};
        zero.i64 = 0;
        state.pendingResult = zero;
        state.hasPendingResult = true;
        state.exitRequested = true;
        state.currentInstr = nullptr;
        instr = nullptr;
        return false;
    }

    state.currentInstr = &state.bb->instructions[state.ip];
    instr = state.currentInstr;
    /// @brief Sets currentcontext value.
    setCurrentContext(state.fr, state.bb, state.ip, *state.currentInstr);

    if (auto pause = shouldPause(state, state.currentInstr, false)) [[unlikely]]
    {
        state.pendingResult = *pause;
        state.hasPendingResult = true;
        state.exitRequested = true;
        state.currentInstr = nullptr;
        instr = nullptr;
        return false;
    }

    return true;
}

/// @brief Update tracing counters and emit optional trace callbacks.
/// @param instr Instruction that was just executed.
/// @param frame Frame providing operand storage for tracing.
/// @details Uses the cached tracingActive_ flag for fast-path bypass when tracing
///          is disabled. This reduces per-instruction overhead from a function call
///          plus internal enabled() check to a single boolean test.
void VM::traceInstruction(const Instr &instr, Frame &frame) {
    ++instrCount;
#if !defined(ZANNA_VM_TRACE) || ZANNA_VM_TRACE
    // Fast-path: skip tracer call entirely when tracing is disabled.
    // The tracingActive_ flag is cached from tracer.isEnabled() to avoid
    // repeated function calls on every instruction.
    if (tracingActive_) [[unlikely]]
        tracer.onStep(instr, frame);
#else
    (void)frame;
#endif
}

/// @brief Finalise dispatch after executing an instruction.
/// @details Processes return values, control-flow changes, and debugger pauses to
///          determine whether the dispatch loop should continue.
/// @param state Execution state being advanced.
/// @param exec Result returned by the opcode handler.
/// @return True when execution of the enclosing function has completed.
bool VM::finalizeDispatch(ExecState &state, const ExecResult &exec) {
    ZANNA_VM_DISPATCH_AFTER(state,
                            state.currentInstr ? state.currentInstr->op : il::core::Opcode::Trap);
    if (state.exitRequested) [[unlikely]] {
        if (!state.hasPendingResult) {
            Slot s{};
            s.i64 = kDebugPauseSentinel;
            state.pendingResult = s;
            state.hasPendingResult = true;
        }
        return true;
    }
    if (exec.returned) [[unlikely]] {
        state.pendingResult = exec.value;
        state.hasPendingResult = true;
        state.exitRequested = true;
        return true;
    }

    if (exec.jumped)
        debug.resetLastHit();
    else [[likely]]
        ++state.ip;

    // Only check debug pause when step-mode is active.
    // This avoids the function-call overhead of shouldPause on every instruction
    // during normal (non-debug) execution.
    if (stepBudget > 0 || debugStepMode_ != DebugStepMode::None) [[unlikely]] {
        if (auto pause = shouldPause(state, nullptr, true)) [[unlikely]]
        {
            state.pendingResult = *pause;
            state.hasPendingResult = true;
            state.exitRequested = true;
            return true;
        }
    }

    // Note: exitRequested and hasPendingResult will be reset by beginDispatch
    // at the start of the next iteration, so we skip redundant writes here.
    return false;
}

/// @brief Instantiate a dispatch driver for the requested strategy.
/// @param kind Dispatch strategy to instantiate.
/// @return Unique pointer owning the created driver.
std::unique_ptr<VM::DispatchDriver, VM::DispatchDriverDeleter> VM::makeDispatchDriver(
    DispatchKind kind) {
    switch (kind) {
        case DispatchKind::FnTable:
            return std::unique_ptr<DispatchDriver, DispatchDriverDeleter>(
                new detail::FnTableDispatchDriver());
        case DispatchKind::Switch:
            return std::unique_ptr<DispatchDriver, DispatchDriverDeleter>(
                new detail::SwitchDispatchDriver());
        case DispatchKind::Threaded:
#if ZANNA_THREADING_SUPPORTED
            return std::unique_ptr<DispatchDriver, DispatchDriverDeleter>(
                new detail::ThreadedDispatchDriver());
#else
            return std::unique_ptr<DispatchDriver, DispatchDriverDeleter>(
                new detail::SwitchDispatchDriver());
#endif
    }
    return std::unique_ptr<DispatchDriver, DispatchDriverDeleter>(
        new detail::SwitchDispatchDriver());
}

/// @brief Check if a trap dispatch signal targets the given state and clear context if so.
/// @details This is an inline version of the trap dispatch logic that avoids constructing
///          VMContext on the hot path. Used by runFunctionLoop for efficiency.
/// @param vm VM whose current diagnostic context is cleared on a match.
/// @param signal Trap dispatch signal to check.
/// @param state Execution state to compare against.
/// @return True if the signal targeted this state and was handled.
static inline bool handleTrapDispatchInternal(VM &vm,
                                              const VM::TrapDispatchSignal &signal,
                                              VM::ExecState &state) {
    if (signal.target != &state)
        return false;
    vm.clearCurrentContext();
    return true;
}

/// @brief Execute the interpreter loop until the current function returns.
/// @param st Execution state for the active function call.
/// @return Slot containing the function's return value (or zero when absent).
///
/// @note VMContext is still created for compatibility with the dispatch driver interface,
/// but the hot-path macros (ZANNA_VM_DISPATCH_BEFORE/AFTER) now use ExecState directly,
/// avoiding the need to access VMContext on every instruction. This eliminates the
/// overhead described in CRITICAL-1 of vm_issues.txt.
Slot VM::runFunctionLoop(ExecState &st) {
    VMContext context(*this);
    for (;;) {
        // Check for a pending Ctrl-C / programmatic interrupt.  We test at the
        // top of every iteration (i.e. every function-call boundary) rather than
        // every instruction to keep overhead negligible.
        if (consumePendingInterrupt()) {
            RuntimeBridge::trap(TrapKind::Interrupt, "interrupted", {}, "", "");
        }

        clearCurrentContext();
        try {
            if (!dispatchDriver)
                dispatchDriver = makeDispatchDriver(dispatchKind);

#if defined(_WIN32)
            // On Windows, hardware exceptions (access violations, divide-by-zero,
            // stack overflow) manifest as Structured Exceptions rather than POSIX
            // signals.  Wrap the dispatch step so that hardware faults produce a
            // clean Zanna trap message instead of an unhandled-exception dialog.
            //
            // Note: __try/__except cannot appear in the same function as C++ objects
            // with destructors in MSVC — move the dispatch call to a separate helper.
            bool finished = runDispatchStep(context, st);
#else
            const bool finished = dispatchDriver ? dispatchDriver->run(*this, context, st) : false;
#endif

            // Re-check the interrupt flag after dispatch returns.  The flag may
            // have been set by a poll callback (which also returns false to stop
            // the driver via requestPause).  Without this second check the
            // interrupt would never be observed for programs whose dispatch loop
            // never yields on its own (e.g. tight branch loops).
            if (consumePendingInterrupt()) {
                RuntimeBridge::trap(TrapKind::Interrupt, "interrupted", {}, "", "");
            }

            if (finished) {
                if (st.hasPendingResult)
                    return st.pendingResult;
                Slot zero{};
                zero.i64 = 0;
                return zero;
            }
        } catch (const TrapDispatchSignal &signal) {
            // Use inline handler instead of VMContext for efficiency
            if (!handleTrapDispatchInternal(*this, signal, st))
                throw;
        }
    }
}

#if defined(_WIN32)
/// @brief SEH exception filter — only handle true hardware faults.
/// @details C++ exceptions on MSVC are implemented via SEH with exception code
///          0xE06D7363 ("msc" + prefix).  We must let those propagate so that
///          TrapDispatchSignal reaches its C++ catch handler.
/// @param code Windows structured-exception code.
/// @return @c EXCEPTION_EXECUTE_HANDLER for supported hardware faults;
///         otherwise @c EXCEPTION_CONTINUE_SEARCH.
static int sehFilter(unsigned int code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            return EXCEPTION_EXECUTE_HANDLER;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }
}

/// @brief Pure SEH wrapper free of C++ objects that require unwinding.
/// @param fn Function pointer that performs the actual dispatch.
/// @param ctx Opaque context pointer forwarded to @p fn.
/// @return Zero when unfinished, one when finished, or negative one after a
///         handled hardware exception.
static int sehRunStep(int (*fn)(void *), void *ctx) {
    int r = 0;
    __try {
        r = fn(ctx);
    } __except (sehFilter(GetExceptionCode())) {
        r = -1;
    }
    return r;
}

/// @brief Execute one dispatch step, translating Windows hardware exceptions into
/// Zanna traps.
/// @param context Shared VM context passed to the dispatch driver.
/// @param st Execution state advanced by the driver.
/// @return @c true when dispatch completed the current function.
bool VM::runDispatchStep(VMContext &context, ExecState &st) {
    struct Args {
        VM *vm;
        VMContext *ctx;
        ExecState *st;
    } args{this, &context, &st};

    /// @brief Run one dispatch step through the Windows SEH adapter.
    /// @param p Erased pointer to the local dispatch arguments.
    /// @return One when the driver completes the function, otherwise zero.
    int result = sehRunStep(
        [](void *p) -> int {
            auto *a = static_cast<Args *>(p);
            if (a->vm->dispatchDriver)
                return a->vm->dispatchDriver->run(*a->vm, *a->ctx, *a->st) ? 1 : 0;
            return 0;
        },
        &args);
    if (result < 0) {
        RuntimeBridge::trap(
            TrapKind::RuntimeError,
            "hardware exception (access violation, divide by zero, or stack overflow)",
            {},
            "",
            "");
    }
    return result > 0;
}
#endif

/// @brief Custom deleter implementation for RtContext.
/// @details Shuts down the shared default pool before cleaning and deleting the
///          runtime context so worker unbinds remain within its lifetime.
/// @param ctx Runtime context to destroy; @c nullptr is ignored.
void VM::RtContextDeleter::operator()(RtContext *ctx) const noexcept {
    if (ctx) {
        // Drain the shared default thread pool before this per-run context is freed. Its worker
        // threads bind this context at creation (rt_thread_trampoline) and would otherwise unbind
        // it during the process-exit finalizer sweep — after it has been freed — underflowing
        // bind_count. Joining them here makes each worker's unbind nest inside the context's
        // lifetime. No-op when no pool was ever created.
        rt_parallel_shutdown_default_pool();
        rt_context_cleanup(ctx);
        delete ctx;
    }
}

//===----------------------------------------------------------------------===//
// Section 4: VM LIFECYCLE AND RESOURCE MANAGEMENT
//===----------------------------------------------------------------------===//

/// @brief Release heap allocations backing mutable program globals.
VM::ProgramState::~ProgramState() {
    for (auto &entry : mutableGlobalMap)
        std::free(entry.second);
}

/// @brief Release resources owned by the VM, including cached strings, mutable globals, and runtime
/// context.
VM::~VM() {
    inlineLiteralCache.clear();
    releaseExternRegistry(externRegistry_);
    externRegistry_ = nullptr;
}

// =============================================================================
// Dispatch Handler Implementations (Generated)
// =============================================================================
// These includes provide the actual implementations for VM dispatch:
//
// InlineHandlersImpl.inc:
//   Defines VM::inline_handle_<OpName>(ExecState&) for each opcode. Each handler
//   delegates to detail::handle<OpName>() and processes the ExecResult.
//
// SwitchDispatchImpl.inc:
//   Defines VM::dispatchOpcodeSwitch() - a portable fallback dispatcher using a
//   switch statement when computed-goto (threaded dispatch) is unavailable.
//
// Both files are synchronized with il/core/Opcode.def and must be updated when
// opcodes are added or removed. See docs/internals/generated-files.md for details.
// =============================================================================
#include "vm/ops/generated/InlineHandlersImpl.inc"
#include "vm/ops/generated/SwitchDispatchImpl.inc"

/// @brief Execute function @p fn with optional arguments.
///
/// Prepares an execution state, then runs the interpreter loop. The callee's
/// execution participates fully in tracing, debugging, and runtime bridge
/// interactions triggered through individual instructions.
///
/// Workflow: create an execution state via @c prepareExecution, pass it to
/// @c runFunctionLoop, and propagate the returned slot to the caller.
///
/// @param fn   Function to execute.
/// @param args Argument slots passed to the entry block parameters.
/// @return Slot containing the function's return value.
Slot VM::execFunction(const Function &fn, std::span<const Slot> args) {
    ActiveVMGuard guard(this);
    const bool topLevelCall = execStack.empty();
    if (topLevelCall)
        clearTrapState();

    // Check for stack overflow before pushing a new frame
    if (execStack.size() >= kMaxRecursionDepth) {
        RuntimeBridge::trap(TrapKind::RuntimeError,
                            "stack overflow: maximum recursion depth exceeded",
                            {},
                            fn.name,
                            "");
    }

    auto st = prepareExecution(fn, args);
    // Reconstruct call-site info from the active execution state on the stack
    // rather than currentContext, which may be stale when the fast-path dispatch
    // skips setCurrentContext() for performance.
    if (!execStack.empty()) {
        const ExecState *caller = execStack.back();
        st.callSiteBlock = caller->bb;
        st.callSiteIp = caller->ip;
        if (caller->bb && caller->ip < caller->bb->instructions.size())
            st.callSiteLoc = caller->bb->instructions[caller->ip].loc;
        else
            st.callSiteLoc = {};
    } else {
        st.callSiteBlock = currentContext.block;
        st.callSiteIp = currentContext.hasInstruction ? currentContext.instructionIndex : 0;
        st.callSiteLoc = currentContext.loc;
    }

    // Use the shared ExecStackGuard from VM.hpp (pre-allocated stack avoids heap allocs)
    ExecStackGuard guardStack(*this, st);

    try {
        Slot result = runFunctionLoop(st);

        // If the return value is a string, retain it before releasing frame buffers.
        // releaseFrameBuffers will release all regIsStr registers, which would drop
        // the refcount on the return value before the caller can retain or discard it.
        if (st.fr.func && st.fr.func->retType.kind == il::core::Type::Kind::Str &&
            st.hasPendingResult) {
            rt_str_retain_maybe(result.str);
        }

        // Return frame buffers to pool for reuse by subsequent calls.
        releaseFrameBuffers(st.fr);

        return result;
    } catch (...) {
        releaseFrameBuffers(st.fr);
        throw;
    }
}

/// @brief Return the number of instructions executed by the VM instance.
/// @return Cumulative instruction count since construction or last reset.
uint64_t VM::getInstrCount() const {
    return instrCount;
}

/// @brief Emit a tail-call event to the trace sink when enabled.
/// @param from Caller function, which may be @c nullptr.
/// @param to Tail-called function, which may be @c nullptr.
void VM::onTailCall(const Function *from, const Function *to) {
#if !defined(ZANNA_VM_TRACE) || ZANNA_VM_TRACE
    tracer.onTailCall(from, to);
#else
    (void)from;
    (void)to;
#endif
}

#if ZANNA_VM_OPCOUNTS
/// @brief Access per-opcode execution counters.
/// @return Immutable counter array indexed by opcode value.
const std::array<uint64_t, il::core::kNumOpcodes> &VM::opcodeCounts() const {
    return opCounts_;
}

/// @brief Reset every per-opcode counter to zero.
void VM::resetOpcodeCounts() {
    opCounts_.fill(0);
}

/// @brief Rank the most frequently executed opcodes.
/// @param n Maximum number of opcode/count pairs to return.
/// @return Nonzero counters sorted in descending execution-count order.
std::vector<std::pair<int, uint64_t>> VM::topOpcodes(std::size_t n) const {
    std::vector<std::pair<int, uint64_t>> items;
    items.reserve(opCounts_.size());
    for (std::size_t i = 0; i < opCounts_.size(); ++i)
        if (opCounts_[i] != 0)
            items.emplace_back(static_cast<int>(i), opCounts_[i]);
    /// @brief Order opcode counters from most to least frequently executed.
    /// @param a Left-hand opcode/count pair.
    /// @param b Right-hand opcode/count pair.
    /// @return `true` when `a` has the larger count.
    std::partial_sort(items.begin(),
                      items.begin() + std::min(n, items.size()),
                      items.end(),
                      [](const auto &a, const auto &b) { return a.second > b.second; });
    if (items.size() > n)
        items.resize(n);
    return items;
}
#endif

//===----------------------------------------------------------------------===//
// Section 5: EXECUTION CONTEXT MANAGEMENT
//===----------------------------------------------------------------------===//

/// @brief Record the currently executing instruction for diagnostics and traps.
/// @param fr Active frame containing the instruction.
/// @param bb Basic block housing the instruction.
/// @param ip Index of the instruction within the block.
/// @param in Instruction being executed.
void VM::setCurrentContext(Frame &fr, const BasicBlock *bb, size_t ip, const Instr &in) {
    // Optimize: only update fields that changed (common case: same function/block)
    if (currentContext.function != fr.func) [[unlikely]]
        currentContext.function = fr.func;
    if (currentContext.block != bb) [[unlikely]]
        currentContext.block = bb;
    currentContext.instructionIndex = ip;
    currentContext.hasInstruction = true;
    currentContext.loc = in.loc;
}

/// @brief Reset the recorded execution context.
void VM::clearCurrentContext() {
    currentContext.function = nullptr;
    currentContext.block = nullptr;
    currentContext.instructionIndex = 0;
    currentContext.hasInstruction = false;
    currentContext.loc = {};
}

/// @brief Capture the best available context for a newly raised trap.
/// @details Prefers the active execution stack because fast dispatch may not
///          refresh @ref currentContext for every instruction.
/// @return Function, block, instruction index, and source location snapshot.
VM::TrapContext VM::currentTrapContext() const {
    // Prefer execStack (always current) over currentContext (may be stale
    // when fast-path dispatch skips setCurrentContext).
    if (!execStack.empty()) {
        const ExecState *st = execStack.back();
        TrapContext ctx{};
        ctx.function = st->fr.func;
        ctx.block = st->bb;
        ctx.instructionIndex = st->ip;
        if (st->bb && st->ip < st->bb->instructions.size()) {
            ctx.hasInstruction = true;
            ctx.loc = st->bb->instructions[st->ip].loc;
        }
        return ctx;
    }
    return currentContext;
}

//===----------------------------------------------------------------------===//
// Section 6: TRAP HANDLING
//===----------------------------------------------------------------------===//

/// @brief Populate trap metadata and redirect control to an active handler.
/// @details Walks the execution stack to locate the nearest handler, wiring up
///          resume state and error slots before transferring control.
/// @param error Error structure describing the trap condition; updated in place.
/// @return True when a handler was found and control transferred.
bool VM::prepareTrap(VmError &error) {
    // Derive trap context from execution stack for better performance.
    // This avoids updating currentContext on every instruction dispatch.
    const BasicBlock *faultBlock = nullptr;
    size_t faultIp = 0;
    il::support::SourceLoc faultLoc{};

    if (!execStack.empty()) {
        // Get context from the active execution state
        const ExecState *activeState = execStack.back();
        faultBlock = activeState->bb;
        faultIp = activeState->ip;
        // Get source location from the current instruction
        if (faultBlock && faultIp < faultBlock->instructions.size())
            faultLoc = faultBlock->instructions[faultIp].loc;
    } else if (currentContext.hasInstruction) {
        // Fallback to currentContext if no active execution state
        faultBlock = currentContext.block;
        faultIp = currentContext.instructionIndex;
        faultLoc = currentContext.loc;
    }

    // Remember the original fault site: the search loop below rewrites faultLoc to
    // each caller's call site while looking for a handler.
    const il::support::SourceLoc trapLoc = faultLoc;

    // Use index-based iteration to avoid potential iterator invalidation
    // if exception handling modifies the execution stack
    const size_t stackSize = execStack.size();
    for (size_t i = 0; i < stackSize; ++i) {
        // Iterate in reverse order (from top of stack)
        ExecState *st = execStack[stackSize - 1 - i];
        Frame &fr = st->fr;
        if (!fr.ehStack.empty()) {
            const auto &record = fr.ehStack.back();

            fr.activeError = error;
            const uint64_t ipValue = static_cast<uint64_t>(faultIp);
            const int32_t lineValue = faultLoc.hasLine() ? static_cast<int32_t>(faultLoc.line) : -1;
            fr.activeError.ip = ipValue;
            fr.activeError.line = lineValue;
            error.ip = ipValue;
            error.line = lineValue;

            fr.resumeState.block = faultBlock;
            fr.resumeState.faultIp = faultIp;
            if (faultBlock) {
                const size_t instructionCount = faultBlock->instructions.size();
                fr.resumeState.nextIp = faultIp < instructionCount ? faultIp + 1 : instructionCount;
            } else {
                fr.resumeState.nextIp = faultIp;
            }
            fr.resumeState.valid = true;

            Slot errSlot{};
            errSlot.ptr = &fr.activeError;
            Slot tokSlot{};
            tokSlot.ptr = &fr.resumeState;

            if (record.handler) {
                // Use reverse map for O(1) lookup instead of O(N*M) linear scan
                const Function *ownerFn = nullptr;
                auto it = blockToFunction.find(record.handler);
                if (it != blockToFunction.end()) {
                    ownerFn = it->second;
                }

                if (ownerFn && fr.func != ownerFn) {
                    for (size_t idx = 0; idx < fr.regIsStr.size() && idx < fr.regs.size(); ++idx) {
                        if (fr.regIsStr[idx])
                            rt_str_release_maybe(fr.regs[idx].str);
                    }
                    fr.func = ownerFn;
                    fr.regs.clear();

                    // Use shared helper to compute/cache register file size
                    const size_t maxSsaId = detail::VMAccess::computeMaxSsaId(*this, *ownerFn);
                    fr.regs.resize(maxSsaId + 1);
                    fr.regIsStr.assign(fr.regs.size(), 0);
                    fr.params.assign(fr.regs.size(), Slot{});
                    fr.paramsSet.assign(fr.regs.size(), 0);
                    st->blocks = &getOrBuildBlockMap(*ownerFn);
                }
            }

            if (record.handler && !record.handler->params.empty()) {
                const auto &params = record.handler->params;
                if (params[0].id < fr.params.size()) {
                    fr.params[params[0].id] = errSlot;
                    fr.paramsSet[params[0].id] = 1;
                }
                if (params.size() > 1) {
                    if (params[1].id < fr.params.size()) {
                        fr.params[params[1].id] = tokSlot;
                        fr.paramsSet[params[1].id] = 1;
                    }
                }
            }

            st->bb = record.handler;
            st->ip = 0;
            st->skipBreakOnce = false;

            // Auto-pop the handler from the EH stack before transferring control.
            // Handler blocks always start with a clean stack — if catch body
            // throws, the trap propagates to the next outer handler rather than
            // re-entering the same one.
            fr.ehStack.pop_back();

            vm_clear_trap_token();
            throwForTrap(st);
        }

        faultBlock = st->callSiteBlock;
        faultIp = st->callSiteIp;
        faultLoc = st->callSiteLoc;
    }

    // Unhandled trap: let an interactive debugger inspect the crash site (the call
    // stack and locals are still intact) before the program unwinds and exits. The
    // returned action is ignored — execution cannot resume past an unhandled trap.
    if (frontend_)
        (void)frontend_->onStop(buildStopInfo("exception", trapLoc));

    return false;
}

/// @brief Throw a trap-dispatch signal targeting the supplied execution state.
/// @param target Execution state that should resume after handling the trap.
[[noreturn]] void VM::throwForTrap(ExecState *target) {
    throw TrapDispatchSignal(target);
}

} // namespace il::vm
