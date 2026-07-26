//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/Runner.cpp
// Purpose: Implement the lightweight public VM runner façade backed by the
//          full interpreter implementation.
// Key invariants: Runner forwards configuration to the underlying VM and
//                 preserves observable behaviour exposed by existing tooling.
// Ownership/Lifetime: Runner owns its VM instance while borrowing the module
//                     and optional debug script supplied by callers.
// Links: docs/internals/codemap/vm-runtime.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the public VM runner façade and its debugger-oriented
///        stepping, watch, profiling, and diagnostic operations.
/// @details The façade owns a hidden interpreter instance, forwards run
///          configuration, lazily prepares step-mode state, and translates
///          internal debug records into stable public value types.

#include "zanna/vm/VM.hpp"

#include "support/source_manager.hpp"
#include "vm/OpHandlerAccess.hpp"
#include "vm/VM.hpp"
#include "vm/VMConstants.hpp"
#include "vm/VMContext.hpp"

#include <functional>
#include <utility>

namespace il::vm {

/// @brief Private implementation that owns the actual VM instance.
/// @details The façade pattern keeps the public @ref Runner interface header
///          light by hiding the heavy VM headers behind a unique_ptr.  The Impl
///          aggregates the concrete @ref VM object and optional debug script,
///          exposing minimal forwarding methods consumed by Runner.
class Runner::Impl {
  public:
    /// @brief Construct the backing VM with the supplied configuration.
    /// @details Stores the debug script pointer from @p config so the VM can
    ///          drive breakpoints, then instantiates the interpreter with the
    ///          caller-provided trace and step-limit parameters.  Ownership of
    ///          the Debug object transfers into the VM as part of the move.
    /// @param module Module to execute.
    /// @param config Run configuration describing trace/debug behaviour.
    Impl(const il::core::Module &module, RunConfig config)
        : script(config.debugScript), vm(module,
                                         config.trace,
                                         config.maxSteps,
                                         std::move(config.debug),
                                         script,
                                         config.stackBytes) {
        // Forward polling configuration to the underlying VM; allow env override.
        uint32_t everyN = config.interruptEveryN;
        if (everyN == 0) {
            if (const char *envEvery = std::getenv("ZANNA_INTERRUPT_EVERY_N")) {
                char *end = nullptr;
                unsigned long n = std::strtoul(envEvery, &end, 10);
                if (end && *end == '\0')
                    everyN = static_cast<uint32_t>(n);
            }
        }
        detail::VMAccess::setPollConfig(vm, everyN, std::move(config.pollCallback));
        vm.setDebugFrontend(config.frontend);
        for (const auto &ext : config.externs)
            il::vm::RuntimeBridge::registerExtern(ext);

        // Seed runtime ARGC/ARG$/COMMAND$ only after VM construction so the
        // runtime is ready for string/heap operations. Empty is explicit and
        // must suppress the native host argv fallback.
        rt_args_clear();
        for (const auto &s : config.programArgs) {
            rt_string tmp = rt_string_from_bytes(s.data(), s.size());
            rt_args_push(tmp);
            rt_string_unref(tmp);
        }
    }

    /// @brief Execute the loaded module until completion or trap.
    /// @details Simply forwards to @ref VM::run, keeping the façade thin.  The
    ///          result reflects the process exit code or trap-specific return
    ///          value returned by the interpreter.
    /// @return Interpreter exit code as produced by @ref VM::run.
    int64_t run() {
        return vm.run();
    }

    /// @brief Retrieve the number of IL instructions executed so far.
    /// @details Forwards to @ref VM::getInstrCount so tooling can gather
    ///          profiling or debugging information without exposing the full VM
    ///          type in headers.
    /// @return Total number of instructions the VM has executed.
    uint64_t instructionCount() const {
        return vm.getInstrCount();
    }

    /// @brief Fetch the most recent trap message emitted by the VM.
    /// @details Returns an optional string describing the last trap recorded by
    ///          the interpreter.  When no trap has occurred the optional is
    ///          empty, mirroring @ref VM::lastTrapMessage.
    /// @return Trap description when available; otherwise `std::nullopt`.
    std::optional<std::string> lastTrapMessage() const {
        return vm.lastTrapMessage();
    }

#if ZANNA_VM_OPCOUNTS
    /// @brief Access the VM's per-opcode execution counters.
    /// @return Immutable counter array indexed by opcode value.
    const std::array<uint64_t, il::core::kNumOpcodes> &opcodeCounts() const {
        return vm.opcodeCounts();
    }

    /// @brief Zero all per-opcode execution counters.
    /// @details Useful when starting a profiling interval; counters are
    ///          accumulated by the VM's hot dispatch path.
    void resetOpcodeCounts() {
        vm.resetOpcodeCounts();
    }

    /// @brief Return the most frequently executed opcodes.
    /// @param n Maximum number of ranked entries to return.
    /// @return Opcode/count pairs ordered by the VM profiling implementation.
    std::vector<std::pair<int, uint64_t>> topOpcodes(std::size_t n) const {
        return vm.topOpcodes(n);
    }
#endif

    // Single-step support ----------------------------------------------------

    /// @brief Execute a single IL instruction and return the resulting status.
    /// @details Lazily prepares execution state on the first call, then delegates
    ///          to the VM's internal single-step mechanism.  The returned status
    ///          distinguishes between normal advancement, breakpoint hits, pause
    ///          requests, and program termination.
    /// @return A StepResult indicating whether execution advanced, hit a
    ///         breakpoint, paused, or halted.
    StepResult step() {
        ensurePrepared();
        auto maybe = detail::VMAccess::stepOnce(vm, *state);
        if (!maybe)
            return {StepStatus::Advanced};

        const auto code = maybe->i64;
        if (code == kDebugBreakpointSentinel)
            return {StepStatus::BreakpointHit};
        if (code == kDebugPauseSentinel)
            return {StepStatus::Paused};

        // Any other concrete result means function returned (halted).
        return {StepStatus::Halted};
    }

    /// @brief Get the current call stack depth.
    /// @return Number of execution states on the active VM stack.
    size_t currentDepth() const {
        return detail::VMAccess::execStack(vm).size();
    }

    /// @brief Get the current source line (-1 if unknown).
    /// @return One-based source line for the active instruction, or @c -1 when
    ///         no location is available.
    int32_t currentSourceLine() const {
        const auto &stack = detail::VMAccess::execStack(vm);
        if (stack.empty())
            return -1;
        const auto *es = stack.back();
        if (!es || !es->bb || es->ip >= es->bb->instructions.size())
            return -1;
        const auto &instr = es->bb->instructions[es->ip];
        return instr.loc.hasLine() ? static_cast<int32_t>(instr.loc.line) : -1;
    }

    /// @brief Step over: execute until a new source line at same/shallower depth.
    /// @return Status describing the breakpoint, pause, trap, or halt that
    ///         stopped stepping.
    RunStatus stepOver() {
        ensurePrepared();
        const size_t startDepth = currentDepth();
        const int32_t startLine = currentSourceLine();

        while (true) {
            auto res = step();
            switch (res.status) {
                case StepStatus::BreakpointHit:
                    return RunStatus::BreakpointHit;
                case StepStatus::Halted:
                    return RunStatus::Halted;
                case StepStatus::Trapped:
                    return RunStatus::Trapped;
                case StepStatus::Paused:
                    return RunStatus::Paused;
                case StepStatus::Advanced: {
                    const size_t depth = currentDepth();
                    // Still inside a callee — keep running
                    if (depth > startDepth)
                        continue;
                    // At same or shallower depth, check if source line changed
                    const int32_t line = currentSourceLine();
                    if (line >= 0 && line != startLine)
                        return RunStatus::Paused;
                    // Same line or unknown line — keep stepping
                    continue;
                }
            }
        }
    }

    /// @brief Step out: execute until the current function returns.
    /// @return Status describing the breakpoint, pause, trap, or halt that
    ///         stopped stepping.
    RunStatus stepOut() {
        ensurePrepared();
        const size_t startDepth = currentDepth();

        while (true) {
            auto res = step();
            switch (res.status) {
                case StepStatus::BreakpointHit:
                    return RunStatus::BreakpointHit;
                case StepStatus::Halted:
                    return RunStatus::Halted;
                case StepStatus::Trapped:
                    return RunStatus::Trapped;
                case StepStatus::Paused:
                    return RunStatus::Paused;
                case StepStatus::Advanced: {
                    if (currentDepth() < startDepth)
                        return RunStatus::Paused;
                    continue;
                }
            }
        }
    }

    /// @brief Resume execution until a breakpoint, pause, trap, or halt occurs.
    /// @details Repeatedly calls step() in a tight loop, returning only when a
    ///          non-advancing status is encountered.  This is the primary entry
    ///          point for debugger "continue" semantics.
    /// @return A RunStatus reflecting the reason execution stopped.
    RunStatus continueRun() {
        ensurePrepared();
        while (true) {
            auto res = step();
            switch (res.status) {
                case StepStatus::Advanced:
                    continue;
                case StepStatus::BreakpointHit:
                    return RunStatus::BreakpointHit;
                case StepStatus::Halted:
                    return RunStatus::Halted;
                case StepStatus::Trapped:
                    return RunStatus::Trapped;
                case StepStatus::Paused:
                    return RunStatus::Paused;
            }
        }
    }

    /// @brief Register a source-level breakpoint at the given location.
    /// @details Looks up the file path via the debug controller's source manager.
    ///          No-ops when the location is incomplete or no source manager is attached.
    /// @param loc Source location (file + line) at which to break.
    void setBreakpoint(const il::support::SourceLoc &loc) {
        auto &dbg = detail::VMAccess::debug(vm);
        const auto *sm = dbg.getSourceManager();
        if (!sm || !loc.hasFile() || !loc.hasLine())
            return;
        dbg.addBreakSrcLine(std::string(sm->getPath(loc.file_id)), loc.line);
    }

    /// @brief Remove all registered breakpoints.
    /// @details Replaces the debug controller with a fresh instance, preserving
    ///          the source manager reference so file-path lookups still work.
    void clearBreakpoints() {
        auto &dbg = detail::VMAccess::debug(vm);
        const auto *sm = dbg.getSourceManager();
        // Reconstruct a fresh controller but preserve the source manager.
        DebugCtrl fresh{};
        fresh.setSourceManager(sm);
        dbg = std::move(fresh);
    }

    /// @brief Cap the total number of IL instructions the VM will execute.
    /// @details When the step counter reaches @p max the VM halts with a
    ///          @c Paused status so the caller can inspect or resume.
    /// @param max Maximum instruction count (0 = unlimited).
    void setMaxSteps(uint64_t max) {
        detail::VMAccess::setMaxSteps(vm, max);
    }

    /// @brief Install a memory-watch guard over [@p addr, @p addr+@p size).
    /// @details Alerts are collected and returned by @ref drainMemWatchHits.
    ///          Refreshes the VM's debug fast-path flag so the hot loop checks
    ///          for watches on every memory access.
    /// @param addr  Start of the memory region to watch.
    /// @param size  Number of bytes covered by the watch.
    /// @param tag   Human-readable label reported in watch hit events.
    void addMemWatch(const void *addr, std::size_t size, std::string tag) {
        detail::VMAccess::debug(vm).addMemWatch(addr, size, std::move(tag));
        detail::VMAccess::refreshDebugFlags(vm); // Update fast-path flag
    }

    /// @brief Remove a previously registered memory watch.
    /// @param addr Start address used when the watch was registered.
    /// @param size Watched byte count.
    /// @param tag Label used to disambiguate matching watch ranges.
    /// @return @c true when a matching watch was removed.
    bool removeMemWatch(const void *addr, std::size_t size, std::string_view tag) {
        bool removed = detail::VMAccess::debug(vm).removeMemWatch(addr, size, tag);
        detail::VMAccess::refreshDebugFlags(vm); // Update fast-path flag
        return removed;
    }

    /// @brief Consume all queued memory-watch events.
    /// @return Collected hit records, leaving the internal event queue empty.
    std::vector<MemWatchHit> drainMemWatchHits() {
        return detail::VMAccess::debug(vm).drainMemWatchEvents();
    }

    /// @brief Build a newest-to-oldest snapshot of the active call stack.
    /// @details Captures function, block, instruction index, and source line,
    ///          falling back to the lazily prepared step state when it is not
    ///          represented on the VM execution stack.
    /// @return Value-owned backtrace frames safe to retain after the call.
    std::vector<BacktraceFrame> backtrace() const {
        std::vector<BacktraceFrame> frames;
        const auto &stack = detail::VMAccess::execStack(vm);

        // Walk from top (most recent) to bottom (oldest)
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            const auto *es = *it;
            if (!es)
                continue;

            BacktraceFrame frame;

            // Function name
            if (es->fr.func)
                frame.function = es->fr.func->name;

            // Block label and IP
            if (es->bb) {
                frame.block = es->bb->label;
                frame.ip = es->ip;

                // Source line from current instruction
                if (es->ip < es->bb->instructions.size()) {
                    const auto &instr = es->bb->instructions[es->ip];
                    if (instr.loc.hasLine())
                        frame.line = static_cast<int32_t>(instr.loc.line);
                }
            }

            frames.push_back(std::move(frame));
        }

        // Also include the state from the step-mode execution if not already on stack
        if (state && frames.empty()) {
            BacktraceFrame frame;
            if (state->fr.func)
                frame.function = state->fr.func->name;
            if (state->bb) {
                frame.block = state->bb->label;
                frame.ip = state->ip;
                if (state->ip < state->bb->instructions.size()) {
                    const auto &instr = state->bb->instructions[state->ip];
                    if (instr.loc.hasLine())
                        frame.line = static_cast<int32_t>(instr.loc.line);
                }
            }
            frames.push_back(std::move(frame));
        }

        return frames;
    }

    /// @brief Materialize comprehensive information for the most recent trap.
    /// @details Copies internal trap state into @ref cachedTrap so the returned
    ///          pointer remains valid until the next query or runner mutation.
    /// @return Pointer to façade-owned trap information, or @c nullptr when no
    ///         trap message is available.
    const TrapInfo *lastTrap() const {
        // Populate on demand from VM's trap state.
        auto msg = vm.lastTrapMessage();
        if (!msg)
            return nullptr;

        // Copy all fields from the internal trap state for comprehensive diagnostics.
        const auto &trap = detail::VMAccess::lastTrapState(vm);
        cachedTrap.kind = static_cast<int32_t>(trap.error.kind);
        cachedTrap.code = trap.error.code;
        cachedTrap.ip = trap.error.ip != 0 ? trap.error.ip : trap.frame.ip;
        cachedTrap.line = trap.error.line >= 0 ? trap.error.line : trap.frame.line;
        cachedTrap.function = trap.frame.function;
        cachedTrap.block = trap.frame.block;
        cachedTrap.message = *msg;
        return &cachedTrap;
    }

  private:
    /// @brief Lazily prepare the module's @c main function for step-mode use.
    /// @details When no entry function exists, installs an empty state so
    ///          repeated calls remain idempotent.
    void ensurePrepared() {
        if (state)
            return;
        // Locate the entry function and prepare initial execution state.
        const auto &fnMap = detail::VMAccess::functionMap(vm);
        auto it = fnMap.find("main");
        if (it == fnMap.end()) {
            // No main; mark as halted by creating an empty state.
            state = std::make_unique<detail::VMAccess::ExecState>();
            return;
        }
        state = std::make_unique<detail::VMAccess::ExecState>(
            detail::VMAccess::prepare(vm, *it->second, {}));
    }

    DebugScript *script = nullptr; ///< Borrowed debug script used for breakpoints.
    VM vm;                         ///< Owning interpreter instance.
    std::unique_ptr<detail::VMAccess::ExecState> state; ///< Lazily prepared step state.
    mutable TrapInfo cachedTrap{}; ///< Stable storage returned by @ref lastTrap.
};

/// @brief Create a runner façade for the supplied module and configuration.
/// @details Allocates the hidden @ref Impl instance, transferring ownership of
///          any debug handles stored in the configuration.  The façade keeps the
///          header minimal while allowing callers to construct runners on the
///          stack.
/// @param module Borrowed IL module that must outlive the runner.
/// @param config Runtime, debug, polling, argument, and resource configuration.
Runner::Runner(const il::core::Module &module, RunConfig config)
    : impl(std::make_unique<Impl>(module, std::move(config))) {}

/// @brief Destroy the runner, releasing its private implementation.
/// @details Defaulted because unique_ptr cleanly tears down the underlying VM.
Runner::~Runner() = default;

/// @brief Move-construct a runner by transferring ownership of the implementation.
/// @param other Runner whose implementation is transferred.
Runner::Runner(Runner &&) noexcept = default;

/// @brief Move-assign a runner by swapping private implementation pointers.
/// @param other Runner whose implementation is transferred.
/// @return Reference to this runner after assignment.
Runner &Runner::operator=(Runner &&) noexcept = default;

/// @brief Run the module associated with this runner.
/// @details Forwards directly to @ref Impl::run so call sites interact solely
///          with the façade.
/// @return Exit code produced by the VM execution.
int64_t Runner::run() {
    return impl->run();
}

/// @brief Report how many IL instructions have executed.
/// @details Simply forwards to the private implementation to avoid exposing VM
///          internals.
/// @return Number of instructions executed by the VM.
uint64_t Runner::instructionCount() const {
    return impl->instructionCount();
}

/// @brief Retrieve the diagnostic message for the most recent trap.
/// @details Allows tooling to present user-facing diagnostics without touching
///          the VM internals.
/// @return Optional trap message; empty when no trap has fired.
std::optional<std::string> Runner::lastTrapMessage() const {
    return impl->lastTrapMessage();
}

/// @brief Access per-opcode execution counters.
/// @details Returns live VM counters when opcode profiling is compiled in and a
///          process-wide zero-filled array otherwise.
/// @return Immutable counter array indexed by opcode value.
const std::array<uint64_t, il::core::kNumOpcodes> &Runner::opcodeCounts() const {
#if ZANNA_VM_OPCOUNTS
    return impl->opcodeCounts();
#else
    static const std::array<uint64_t, il::core::kNumOpcodes> kEmpty{};
    return kEmpty;
#endif
}

/// @brief Reset opcode counters when profiling support is enabled.
/// @details This operation is a no-op in builds without opcode counting.
void Runner::resetOpcodeCounts() {
#if ZANNA_VM_OPCOUNTS
    impl->resetOpcodeCounts();
#endif
}

/// @brief Return the most frequently executed opcodes.
/// @param n Maximum number of ranked entries requested.
/// @return Opcode/count pairs when profiling is enabled, or an empty vector
///         otherwise.
std::vector<std::pair<int, uint64_t>> Runner::topOpcodes(std::size_t n) const {
#if ZANNA_VM_OPCOUNTS
    return impl->topOpcodes(n);
#else
    return {};
#endif
}

/// @brief Register a runtime external for subsequent VM calls.
/// @param ext External name, signature, and function descriptor to publish.
void Runner::registerExtern(const ExternDesc &ext) {
    il::vm::RuntimeBridge::registerExtern(ext);
}

/// @brief Remove a runtime external by name.
/// @param name Fully qualified external name.
/// @return @c true when a matching registration was removed.
bool Runner::unregisterExtern(std::string_view name) {
    return il::vm::RuntimeBridge::unregisterExtern(name);
}

/// @brief Execute a single IL instruction through the facade.
/// @details Forwards to the private implementation's single-step logic,
///          preparing execution state on first invocation if needed.
/// @return A StepResult indicating the outcome of the single step.
Runner::StepResult Runner::step() {
    return impl->step();
}

/// @brief Step over the current source line without stopping inside callees.
/// @details Continues until execution reaches a different source line at the
///          same or shallower call depth, or another debugger stop condition
///          occurs first.
/// @return Status describing the line pause, breakpoint, trap, or halt.
Runner::RunStatus Runner::stepOver() {
    return impl->stepOver();
}

/// @brief Continue until the current function returns or another stop occurs.
/// @return Status describing the breakpoint, pause, trap, or halt.
Runner::RunStatus Runner::stepOut() {
    return impl->stepOut();
}

/// @brief Resume execution until a debugger-visible stopping condition.
/// @return Status describing the breakpoint, pause, trap, or halt.
Runner::RunStatus Runner::continueRun() {
    return impl->continueRun();
}

/// @brief Register a source breakpoint.
/// @param loc File and line location at which execution should pause.
void Runner::setBreakpoint(const il::support::SourceLoc &loc) {
    impl->setBreakpoint(loc);
}

/// @brief Remove every source breakpoint while preserving source lookup state.
void Runner::clearBreakpoints() {
    impl->clearBreakpoints();
}

/// @brief Set the interpreter instruction budget.
/// @param max Maximum instructions before pausing; zero disables the limit.
void Runner::setMaxSteps(uint64_t max) {
    impl->setMaxSteps(max);
}

/// @brief Retrieve comprehensive information about the most recent trap.
/// @return Pointer to runner-owned trap information, or @c nullptr when no trap
///         has occurred.
const Runner::TrapInfo *Runner::lastTrap() const {
    return impl->lastTrap();
}

/// @brief Watch a half-open memory range for writes.
/// @param addr Start of the watched range.
/// @param size Number of bytes covered.
/// @param tag Value-owned label included in watch events.
void Runner::addMemWatch(const void *addr, std::size_t size, std::string tag) {
    impl->addMemWatch(addr, size, std::move(tag));
}

/// @brief Remove a matching memory watch.
/// @param addr Start address used when the watch was registered.
/// @param size Watched byte count.
/// @param tag Watch label used for matching.
/// @return @c true when a watch was removed.
bool Runner::removeMemWatch(const void *addr, std::size_t size, std::string_view tag) {
    return impl->removeMemWatch(addr, size, tag);
}

/// @brief Consume queued memory-watch events.
/// @return Value-owned hit records in debugger queue order.
std::vector<MemWatchHit> Runner::drainMemWatchHits() {
    return impl->drainMemWatchHits();
}

/// @brief Capture the current VM backtrace.
/// @return Newest-to-oldest value-owned frame descriptions.
std::vector<Runner::BacktraceFrame> Runner::backtrace() const {
    return impl->backtrace();
}

/// @brief Convenience helper that constructs a runner, executes it, and returns the result.
/// @details Used by CLI tooling and tests that only need to run a module once.
///          The helper ensures resources are released immediately after
///          execution by keeping the runner scoped to the call.
/// @param module Module to execute.
/// @param config Runtime configuration and optional debug handles.
/// @return Exit code reported by the VM execution.
int64_t runModule(const il::core::Module &module, RunConfig config) {
    Runner runner(module, std::move(config));
    return runner.run();
}

} // namespace il::vm

// On Windows with static libraries, the linker may not include object files that
// don't have unresolved symbols from previously seen objects. Including VMInit.cpp
// here ensures the VM constructor and static initializers are always compiled into
// the same translation unit as Runner, avoiding the need for WHOLEARCHIVE linking.
#include "VMInit.cpp"
