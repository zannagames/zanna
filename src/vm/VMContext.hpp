//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/VMContext.hpp
// Purpose: Declares helper utilities for accessing VM execution context shared by
// Key invariants: Maintains a thread-local pointer to the active VM for trap
// Ownership/Lifetime: VMContext references a VM instance owned externally.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares thread-local VM binding and the controlled context facade
///        shared by interpreter dispatch strategies.
/// @details ActiveVMGuard manages nested runtime bindings, while VMContext
///          forwards evaluation, tracing, dispatch, and trap operations to a
///          non-owned VM instance.

#pragma once

#include "vm/VM.hpp"

namespace il::vm {

/**
 * @brief RAII guard that installs a VM as the thread-local active instance.
 *
 * ActiveVMGuard manages the thread-local `tlsActiveVM` pointer that enables
 * `VM::activeInstance()` to retrieve the currently executing VM. It also
 * binds the VM's runtime context via `rt_set_current_context()` so that
 * C runtime functions access the correct per-VM state.
 *
 * ## Thread-Local Semantics
 *
 * - On construction: saves the current `tlsActiveVM` and sets it to the new VM
 * - On destruction: restores the saved value (supports nesting)
 * - Each thread has its own `tlsActiveVM`, so concurrent threads can each
 *   have their own active VM without interference
 *
 * ## Debug Assertions
 *
 * In debug builds (`NDEBUG` not defined), the guard asserts that:
 * - **No re-entry**: If `tlsActiveVM` is already non-null and points to a
 *   *different* VM, an assertion fires. This catches accidental concurrent
 *   use of a VM from the same thread.
 * - **Consistent restore**: The destructor asserts that `tlsActiveVM` still
 *   points to the expected VM before restoring.
 *
 * ## Usage
 *
 * @code
 *   {
 *       ActiveVMGuard guard(&myVM);
 *       // VM::activeInstance() now returns &myVM
 *       // rt_get_current_context() returns myVM.rtContext
 *       myVM.run();
 *   }  // guard destructor restores previous active VM
 * @endcode
 *
 * @invariant Restores the previous active VM value on destruction.
 * @invariant In debug builds, asserts on invalid re-entry patterns.
 * @ownership Does not own the VM pointer; lifetime is managed by the caller.
 */
struct ActiveVMGuard {
    /**
     * @brief Install @p vm as the active VM for this thread.
     * @param vm VM to activate; may be nullptr to clear active state.
     * @pre In debug builds, asserts if a different VM is already active.
     */
    explicit ActiveVMGuard(VM *vm);

    /**
     * @brief Restore the previously active VM.
     * @pre In debug builds, asserts if tlsActiveVM was unexpectedly modified.
     */
    ~ActiveVMGuard();

    /// @brief Active-VM guards cannot be copied.
    ActiveVMGuard(const ActiveVMGuard &) = delete;
    /// @brief Active-VM guards cannot be copy-assigned.
    ActiveVMGuard &operator=(const ActiveVMGuard &) = delete;

  private:
    VM *previous = nullptr;                 ///< Previously active VM instance.
    VM *current = nullptr;                  ///< VM installed by this guard (for debug checks).
    RtContext *previousRtContext = nullptr; ///< Previously bound runtime context.
};

/**
 * @brief Execution context providing controlled access to VM internals.
 *
 * VMContext encapsulates per-execution state and provides a stable API
 * for dispatch strategies and runtime functions to interact with the VM.
 * Each execution creates its own context that tracks the active VM instance.
 *
 * @invariant Wraps a valid VM reference throughout its lifetime.
 * @ownership Non-owning reference to VM; caller manages VM lifetime.
 */
class VMContext {
  public:
    /**
     * @brief Construct a context bound to the given VM instance.
     * @param vm VM to provide context for. Must outlive this context.
     */
    explicit VMContext(VM &vm) noexcept;

    /// @brief Evaluates a value operand within the given stack frame.
    ///
    /// Resolves IL value operands (temporaries, immediates, block arguments)
    /// to their concrete runtime values. For temporaries, looks up the value
    /// in the frame's local slots. For immediates, returns the constant directly.
    ///
    /// @param fr The current execution frame containing local variable values.
    /// @param value The IL value to evaluate.
    /// @return The evaluated slot containing the runtime value.
    Slot eval(Frame &fr, const il::core::Value &value) const;

    /// @brief Executes a single instruction step in the VM.
    ///
    /// Fetches and executes one IL instruction, advancing the instruction
    /// pointer. A slot is returned only when dispatch reaches a function result
    /// or pause boundary; ordinary instructions return @c std::nullopt so the
    /// enclosing driver continues.
    ///
    /// @param state The current execution state (frame stack, IP, etc.).
    /// @return Completed function result or pause sentinel, otherwise @c std::nullopt.
    std::optional<Slot> stepOnce(VM::ExecState &state) const;

    /// @brief Handles trap dispatch when an exception or error is raised.
    ///
    /// Called when a trap signal is generated during execution. Determines
    /// whether there's an active exception handler to invoke, or if the trap
    /// should propagate up the call stack.
    ///
    /// @param signal The trap dispatch signal containing trap details.
    /// @param state The current execution state to potentially modify.
    /// @return True if the trap was handled, false if it should propagate.
    bool handleTrapDispatch(const VM::TrapDispatchSignal &signal, VM::ExecState &state) const;

    /// @brief Fetches the opcode of the next instruction to execute.
    ///
    /// Reads the opcode from the instruction at the current instruction pointer
    /// in the execution state. Used by dispatch strategies to determine which
    /// handler to invoke.
    ///
    /// @param state The current execution state with valid IP.
    /// @return The opcode of the current instruction.
    il::core::Opcode fetchOpcode(VM::ExecState &state) const;

    /// @brief Processes the result of inline instruction execution.
    ///
    /// Applies the same jump, return, and loop-exit bookkeeping used by the
    /// regular dispatch path.
    ///
    /// @param state The execution state to update.
    /// @param exec The result from instruction execution.
    void handleInlineResult(VM::ExecState &state, const VM::ExecResult &exec) const;

    /// @brief Raises a fatal trap for unimplemented opcodes.
    ///
    /// Called when the VM encounters an opcode without a handler implementation.
    /// This is a development/debugging aid that terminates execution with a
    /// clear diagnostic message indicating which opcode is missing.
    ///
    /// @param opcode The unimplemented opcode that was encountered.
    void trapUnimplemented(il::core::Opcode opcode) const;

    /// @brief Emits trace output for debugging/profiling instruction execution.
    ///
    /// When tracing is enabled, records the instruction being executed along
    /// with relevant frame state (local values, etc.) to the configured
    /// trace sink.
    ///
    /// @param instr The instruction about to be executed.
    /// @param frame The current stack frame.
    void traceStep(const il::core::Instr &instr, Frame &frame) const;

    /// @brief Executes a single opcode and returns the execution result.
    ///
    /// Core dispatch function that invokes the appropriate handler for the
    /// given instruction. Updates the basic block and instruction pointer
    /// for control flow instructions.
    ///
    /// @param frame The current stack frame for operand access.
    /// @param instr The instruction to execute.
    /// @param blocks Block map for control flow target resolution.
    /// @param bb Current basic block (updated for branches).
    /// @param ip Instruction pointer (updated after execution).
    /// @return The execution result (value, continue, return, trap, etc.).
    VM::ExecResult executeOpcode(Frame &frame,
                                 const il::core::Instr &instr,
                                 const VM::BlockMap &blocks,
                                 const il::core::BasicBlock *&bb,
                                 size_t &ip) const;

    /// @brief Clear the VM's recorded diagnostic instruction context.
    ///
    /// Prevents a later trap from reporting stale function, block, or source
    /// information after control has been redirected.
    void clearCurrentContext() const;

    /// @brief Returns the trace sink for execution tracing output.
    /// @return Reference to the VM's configured trace sink.
    TraceSink &traceSink() const noexcept;

    /// @brief Returns the debug controller for breakpoint/stepping control.
    /// @return Reference to the VM's debug controller.
    DebugCtrl &debugController() const noexcept;

    /// @brief Returns the underlying VM instance.
    /// @return Pointer to the bound VM.
    VM *vm() const noexcept;

  private:
    VM *vmInstance = nullptr; ///< Bound VM instance.
  public:
    /// @brief Lightweight feature configuration consumed by dispatch macros.
    struct Config {
        bool enableOpcodeCounts = true; ///< Whether pre-dispatch hooks increment counters.
    } config; ///< Lightweight runtime config snapshot used by macros.
};

/// @brief Return the active VM associated with the current thread.
/// @return Non-owning active VM pointer, or @c nullptr outside an active guard.
VM *activeVMInstance();

} // namespace il::vm
