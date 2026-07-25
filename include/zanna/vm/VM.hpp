//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/vm/VM.hpp
// Purpose: Declare a lightweight facade for running IL modules through the VM
//          without exposing interpreter internals.
// Key invariants: Public API owns its backing VM implementation and forwards all
//                 operations while preserving semantics of the existing VM class.
// Ownership/Lifetime: Runner manages the interpreter lifetime; callers retain
//                     ownership of modules and optional debug scripts passed in
//                     via configuration.
// Links: docs/internals/codemap/vm-runtime.md, src/vm/Runner.cpp
//
//===----------------------------------------------------------------------===//
//
// UNKNOWN/UNIMPLEMENTED OPCODE HANDLING
// ======================================
// Under normal circumstances, all opcodes defined in the IL specification have
// handlers in the VM dispatch table. This is enforced by dispatch coverage tests
// (test_vm_dispatch_coverage) which verify at compile-time and run-time that
// every opcode has a corresponding handler.
//
// If an unknown or unimplemented opcode is somehow executed (e.g., due to a
// mismatched code generator and VM version, or a corrupted IL module), the VM
// treats this as a FATAL ERROR:
//   1. A trap of kind RuntimeError is raised with a message including the
//      opcode mnemonic and execution context.
//   2. The trap propagates to the RuntimeBridge, which terminates the process
//      via rt_abort() since this condition is not recoverable.
//
// This behavior is intentional: an unknown opcode indicates a programmer error
// or version mismatch, not a runtime condition that can be caught or recovered.
// Embedders should NOT rely on trapping or continuing after an unknown opcode;
// instead, ensure that IL modules are generated with a code generator compatible
// with the VM version in use.
//
//===----------------------------------------------------------------------===//

/**
 * @file include/zanna/vm/VM.hpp
 * @brief Declares the stable embedding façade for executing IL in the VM.
 *
 * @details
 * `Runner` hides the concrete interpreter behind a private implementation while
 * exposing whole-program execution, debugger-oriented stepping, breakpoints,
 * trap snapshots, profiling counters, extern registration, memory watches, and
 * backtraces. `RunConfig` supplies the initial runtime and debug environment.
 *
 * A runner owns its interpreter but borrows the IL module and optional debug
 * script/frontend objects. Those borrowed objects must remain valid for the
 * runner lifetime. Individual `Runner` instances are not synchronized for
 * concurrent method calls.
 */

#pragma once

#include "zanna/vm/debug/Debug.hpp"

#include "il/core/Opcode.hpp"
#include "support/source_location.hpp"
#include "zanna/vm/RuntimeBridge.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace il::core
{
struct Module;
} // namespace il::core

namespace il::vm
{
class VM; // forward declaration for callbacks in RunConfig
class DebugFrontend; // forward declaration for the interactive debug driver

/**
 * @brief Request a resumable debug pause at the next instruction boundary.
 *
 * @param vm VM supplied to a `RunConfig::pollCallback`.
 *
 * The request is safe from within the polling callback. It sets pending VM
 * state and returns immediately; execution reports a pause when control reaches
 * the next applicable instruction boundary.
 */
void requestDebugPause(VM &vm);

/**
 * @brief Collects initial execution, debugging, and host-integration settings.
 *
 * The configuration is passed by value and consumed while constructing a
 * runner. Pointer fields remain borrowed. Extern descriptors are copied into
 * the process-global extern registry, and `programArgs` replaces the runtime's
 * current argument store.
 */
struct RunConfig
{
    TraceConfig trace;     ///< Trace mode and borrowed source manager.
    uint64_t maxSteps = 0; ///< Cumulative instruction limit; zero is unlimited.
    DebugCtrl debug;       ///< Controller state moved into the new interpreter.
    DebugScript *debugScript = nullptr; ///< Borrowed automation script, or null.
    DebugFrontend *frontend = nullptr;  ///< Borrowed interactive driver, or null.
    std::vector<ExternDesc> externs;    ///< Helpers registered process-globally at construction.
    /**
     * @brief Per-frame operand-stack capacity in bytes.
     *
     * This storage backs IL `alloca` operations within each call frame. The
     * default is 64 KiB; larger values accommodate larger local allocations,
     * while smaller values reduce per-frame memory at the cost of earlier stack
     * exhaustion traps.
     */
    std::size_t stackBytes = 65536;

    /**
     * @brief Command-line arguments installed after VM construction.
     *
     * The runtime argument store is cleared even when this vector is empty, so
     * an explicitly configured runner never falls back to a native host argv.
     * BASIC's `ARGC`, `ARG$`, and `COMMAND$` read this copied data.
     */
    std::vector<std::string> programArgs;

    // Periodic host polling --------------------------------------------------
    /**
     * @brief Instruction interval between host polls.
     *
     * Zero disables explicit polling unless the
     * `ZANNA_INTERRUPT_EVERY_N` environment variable provides a valid override.
     */
    uint32_t interruptEveryN = 0;

    /**
     * @brief Host callback invoked at the configured instruction interval.
     *
     * Returning `false` requests a resumable VM pause. The callback is owned by
     * the runner and receives its concrete VM by reference for the duration of
     * the call.
     */
    std::function<bool(VM &)> pollCallback;
};

/**
 * @brief Owns one interpreter instance while borrowing its immutable IL module.
 *
 * Execution state persists across `step()`, `stepOver()`, `stepOut()`, and
 * `continueRun()` calls, enabling a host to inspect and resume a paused program.
 * A runner is movable but not copyable.
 *
 * @invariant A non-moved-from runner has exactly one private interpreter.
 * @ownership The runner owns VM state and copied configuration. The source
 *            module, debug script, and debug frontend remain caller-owned.
 */
class Runner
{
  public:
    /**
     * @brief Construct an interpreter for a module and execution configuration.
     *
     * The constructor applies tracing and debug state, registers configured
     * externs in the process-global registry, configures polling, and replaces
     * the runtime argument store.
     *
     * @param module Borrowed IL module that must outlive the runner.
     * @param config Configuration consumed by value. Any `debugScript` and
     *               `frontend` targets remain borrowed and must also outlive the
     *               runner.
     */
    Runner(const il::core::Module &module, RunConfig config = {});

    /**
     * @brief Destroy the private interpreter and its execution state.
     *
     * Borrowed module, script, and frontend objects are not destroyed.
     */
    ~Runner();

    Runner(const Runner &) = delete;
    Runner &operator=(const Runner &) = delete;

    /**
     * @brief Move-construct by transferring the private interpreter.
     * @param other Runner whose implementation is transferred.
     * @post `other` is valid only for destruction or move assignment.
     */
    Runner(Runner &&) noexcept;

    /**
     * @brief Replace this interpreter with one transferred from `other`.
     * @param other Runner whose implementation is transferred.
     * @return `*this`.
     * @post `other` is valid only for destruction or move assignment.
     */
    Runner &operator=(Runner &&) noexcept;

    /**
     * @brief Execute the module's entry function through the whole-run API.
     * @return The signed 64-bit result produced by the interpreter.
     */
    [[nodiscard]] int64_t run();

    /**
     * @brief Retrieve the cumulative number of executed IL instructions.
     * @return Count accumulated by this interpreter since construction.
     */
    [[nodiscard]] uint64_t instructionCount() const;

    /**
     * @brief Copy the most recent formatted trap message.
     * @return The message when the VM has recorded a trap; otherwise
     *         `std::nullopt`.
     */
    [[nodiscard]] std::optional<std::string> lastTrapMessage() const;

    // Opcode counting facade

    /**
     * @brief Read the execution count indexed by `il::core::Opcode`.
     * @return Reference to runner-owned counters. When opcode counting was
     *         disabled at build time, returns a process-lifetime all-zero array.
     */
    [[nodiscard]] const std::array<uint64_t, il::core::kNumOpcodes> &opcodeCounts() const;

    /**
     * @brief Reset every per-opcode counter to zero.
     * @note This is a no-op when opcode counting was disabled at build time.
     */
    void resetOpcodeCounts();

    /**
     * @brief Rank the most frequently executed opcodes.
     * @param n Maximum number of entries to return.
     * @return Owning `(opcode index, count)` pairs sorted by descending count,
     *         or an empty vector when opcode counting is unavailable.
     */
    [[nodiscard]] std::vector<std::pair<int, uint64_t>> topOpcodes(std::size_t n) const;

    // Extern registration facade

    /**
     * @brief Register a native helper in the process-global extern registry.
     * @param ext Descriptor copied into global registry storage.
     *
     * @note Despite being a runner method, this operation affects other VMs
     *       that resolve through the global registry.
     */
    void registerExtern(const ExternDesc &);

    /**
     * @brief Remove a helper from the process-global extern registry.
     * @param name Borrowed name matched case-insensitively for ASCII letters.
     * @return `true` when an entry was removed; otherwise `false`.
     */
    bool unregisterExtern(std::string_view name);

    //===------------------------------------------------------------------===//
    // Single-step and continue APIs
    //===------------------------------------------------------------------===//

    /// @brief Reason a single-step request returned control to the host.
    enum class StepStatus
    {
        Advanced,      ///< Successfully executed one instruction; can continue.
        Halted,        ///< Program finished (returned from main).
        BreakpointHit, ///< Reached a breakpoint or step budget pause.
        Trapped,       ///< Unhandled trap occurred.
        Paused         ///< Paused for non-breakpoint reason (e.g., external pause).
    };

    /// @brief Result payload returned by `step()`.
    struct StepResult
    {
        StepStatus status; ///< Final status for this step.
    };

    /// @brief Reason a multi-step execution request returned to the host.
    enum class RunStatus
    {
        Halted,            ///< Program finished (returned from main).
        BreakpointHit,     ///< Hit a breakpoint while running.
        Trapped,           ///< Unhandled trap occurred.
        Paused,            ///< Paused for non-breakpoint reason.
        StepBudgetExceeded ///< Global step limit reached.
    };

    /**
     * @brief Execute at most one instruction, preparing the entry state lazily.
     * @return Status distinguishing advancement, halt, breakpoint, trap, or
     *         resumable pause.
     *
     * @note When the module has no `main` function, the lazily prepared empty
     *       state reports a halt rather than executing an instruction.
     */
    StepResult step();

    /**
     * @brief Run to a different source line at the same or shallower call depth.
     *
     * If the current instruction calls another function, that callee executes
     * until it returns unless another stopping condition intervenes.
     *
     * @return `Paused` when the step-over target is reached; otherwise the halt,
     *         trap, breakpoint, or externally requested pause that stopped
     *         execution first.
     *
     * @note Instructions without source-line metadata are skipped until a known,
     *       changed line or another stop condition is reached.
     */
    RunStatus stepOver();

    /**
     * @brief Run until execution leaves the current call depth.
     * @return `Paused` after the current function returns to its caller;
     *         otherwise the halt, trap, breakpoint, or external pause reached
     *         first.
     */
    RunStatus stepOut();

    /**
     * @brief Resume repeated stepping until execution stops advancing.
     * @return The halt, trap, breakpoint, or resumable pause that returned
     *         control to the host.
     */
    RunStatus continueRun();

    /**
     * @brief Add a source-line breakpoint resolved through the source manager.
     * @param loc File and line to register.
     *
     * @note The request is ignored when `loc` lacks a file or line or the debug
     *       controller has no source manager.
     */
    void setBreakpoint(const il::support::SourceLoc &);

    /**
     * @brief Reset breakpoint and watch controller state.
     *
     * The controller's source-manager pointer is preserved, while label/source
     * breakpoints, variable watches, memory watches, pending watch events, and
     * installed class layouts are discarded.
     */
    void clearBreakpoints();

    /**
     * @brief Replace the interpreter's cumulative instruction budget.
     * @param maxSteps Maximum permitted steps, or zero for no limit.
     */
    void setMaxSteps(uint64_t);

    /// @brief Light-weight snapshot of the last trap for diagnostics.
    ///
    /// @details Populated when a trap occurs during execution. Use lastTrap()
    ///          to retrieve this information after RunStatus::Trapped is returned.
    ///
    /// Trap kinds (matching TrapKind enum values):
    /// - 0: DivideByZero - Integer division or remainder by zero
    /// - 1: Overflow - Arithmetic or conversion overflow
    /// - 2: InvalidCast - Invalid cast or conversion semantics
    /// - 3: DomainError - Semantic domain violation or user trap
    /// - 4: Bounds - Array bounds check failure
    /// - 5: FileNotFound - File system open on non-existent path
    /// - 6: EOF - End-of-file reached while input still expected
    /// - 7: IOError - Generic I/O failure
    /// - 8: InvalidOperation - Operation outside allowed state machine
    /// - 9: RuntimeError - Catch-all for unexpected runtime failures
    ///
    /// @note A trap of kind RuntimeError with a message containing "unimplemented
    ///       opcode" indicates a fatal programmer error (unknown/missing opcode
    ///       handler). This is not recoverable; see the header documentation for
    ///       details on unknown opcode handling.
    struct TrapInfo
    {
        int32_t kind = 0;     ///< Trap kind (see enum values above).
        int32_t code = 0;     ///< Secondary error code (0 = none).
        uint64_t ip = 0;      ///< Instruction index within block at trap.
        int32_t line = -1;    ///< Source line (-1 = unknown).
        std::string function; ///< Function name (empty if unknown).
        std::string block;    ///< Block label (empty if unknown).
        std::string message;  ///< Formatted human-readable trap message.
    };

    /**
     * @brief Retrieve a structured snapshot of the most recent VM trap.
     * @return Pointer to runner-owned cached storage, or null when no trap has
     *         been recorded.
     *
     * @warning The pointer remains valid only until the next call to
     *          `lastTrap()`, a non-const runner operation that changes the cache,
     *          or destruction of the runner. Copy the value for longer use.
     */
    const TrapInfo *lastTrap() const;

    // Memory watch façade ----------------------------------------------------
    /**
     * @brief Register a tagged half-open memory watch range.
     * @param addr Borrowed start address reported in matching events.
     * @param size Number of bytes in `[addr, addr + size)`.
     * @param tag Owned label copied into the watch record and hit events.
     *
     * @pre Address arithmetic for the supplied range must not overflow.
     * @pre The watched storage remains addressable while VM writes may target it.
     */
    void addMemWatch(const void *addr, std::size_t size, std::string tag);

    /**
     * @brief Remove a watch matching its complete range and tag.
     * @param addr Original range start.
     * @param size Original range size.
     * @param tag Original tag text.
     * @return `true` when a matching watch was removed.
     */
    bool removeMemWatch(const void *addr, std::size_t size, std::string_view tag);

    /**
     * @brief Consume all queued memory-watch hit payloads.
     * @return Events in queue order; the internal pending queue is empty after
     *         the call.
     */
    std::vector<MemWatchHit> drainMemWatchHits();

    /// @brief Host-owned snapshot of one active VM call frame.
    struct BacktraceFrame
    {
        std::string function; ///< Function name.
        std::string block;    ///< Block label.
        uint64_t ip = 0;      ///< Instruction index within block.
        int32_t line = -1;    ///< Source line (-1 = unknown).
    };

    /**
     * @brief Snapshot the active VM call stack.
     *
     * @return Owning frames ordered from the most recent call to the oldest.
     *         Returns an empty vector before execution state exists; step-mode
     *         state contributes one frame when the main execution stack is
     *         otherwise empty.
     *
     * When paused after a breakpoint or trap, the snapshot reflects the call
     * chain retained at that suspension point.
     */
    [[nodiscard]] std::vector<BacktraceFrame> backtrace() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

/**
 * @brief Construct a temporary runner and execute a module once.
 * @param module Borrowed IL module kept alive for the duration of the call.
 * @param config Configuration consumed by the temporary runner.
 * @return The signed 64-bit result returned by `Runner::run()`.
 */
[[nodiscard]] int64_t runModule(const il::core::Module &module, RunConfig config = {});

} // namespace il::vm
