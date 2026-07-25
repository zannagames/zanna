//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/bytecode/BytecodeVM.hpp
// Purpose: Stack-based bytecode interpreter for compiled Zanna programs.
// Key invariants:
//   - Loaded BytecodeModule storage must outlive the VM.
//   - Call depth and operand stack depth remain within fixed VM limits.
//   - Thread-local active VM/module pointers are scoped by guard objects.
// Ownership/Lifetime:
//   - BytecodeVM borrows BytecodeModule storage and owns execution stacks/globals.
//   - Re-entrant callback invocation borrows active frames only for synchronous calls.
// Links: Bytecode.hpp, BytecodeModule.hpp, BytecodeCompiler.hpp
//
//===----------------------------------------------------------------------===//
//
// This file defines the BytecodeVM which executes compiled bytecode.
// The VM uses a stack-based evaluation model with local variable slots
// and supports basic control flow, arithmetic, and function calls.

/**
 * @file
 * @brief Declares the validated stack-based bytecode interpreter and its
 *        callback/runtime integration contracts.
 *
 * `BytecodeVM` owns execution stacks, globals, cached runtime strings, trap
 * snapshots, and debugger state while borrowing the loaded `BytecodeModule`.
 * The module therefore must remain alive until the VM is reloaded or destroyed.
 * Re-entrant APIs execute synchronous runtime callbacks on the suspended VM;
 * asynchronous bridges copy only `ExecutionEnvironment` and module snapshots.
 */

#pragma once

#include "bytecode/Bytecode.hpp"
#include "bytecode/BytecodeModule.hpp"
#include "bytecode/ValueStackStringOwnership.hpp"
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Forward declare runtime string type
struct rt_string_impl;
using rt_string = rt_string_impl *;

// Forward declarations for runtime integration
namespace il::vm {
union Slot;
struct RuntimeCallContext;
} // namespace il::vm

namespace zanna {
namespace bytecode {

/// @brief Type alias for native function handlers invokable directly from bytecode.
/// @details A NativeHandler receives a pointer to the argument slots, the argument
///          count, and a pointer to a result slot. It reads arguments from the
///          args array and writes the return value (if any) to the result slot.
/// @param args   Pointer to the first argument BCSlot.
/// @param argc   Number of arguments provided.
/// @param result Pointer to the result BCSlot (written only if the function returns a value).
using NativeHandler = std::function<void(BCSlot *, uint32_t, BCSlot *)>;

/// @brief Trap kinds for runtime error classification.
/// @details When the VM encounters an exceptional condition, it raises a trap
///          with one of these kinds. Exception handlers can inspect the kind
///          to determine the appropriate recovery strategy.
///
///          Values 0-11 are aligned with il::vm::TrapKind (vm/Trap.hpp) so that
///          the TRAP_KIND bytecode opcode can use a direct cast instead of a
///          translation table. BytecodeVM-specific kinds live at 100+.
enum class TrapKind : uint8_t {
    // --- Aligned with il::vm::TrapKind (vm/Trap.hpp) ---
    DivideByZero = 0,     ///< Division or remainder by zero.
    Overflow = 1,         ///< Integer arithmetic overflow.
    InvalidCast = 2,      ///< Invalid type conversion (e.g., out-of-range float-to-int).
    DomainError = 3,      ///< Semantic domain violation or user trap.
    Bounds = 4,           ///< Array or bounds-check index violation.
    FileNotFound = 5,     ///< File system open on a path that does not exist.
    EndOfFile = 6,        ///< End-of-file reached while input still expected.
    IOError = 7,          ///< Generic I/O failure.
    InvalidOperation = 8, ///< Operation outside the allowed state machine.
    RuntimeError = 9,     ///< Generic runtime error (e.g., from native code).
    Interrupt = 10,       ///< Program interrupted by Ctrl-C or requestInterrupt().
    NetworkError = 11,    ///< Network I/O failure (connection, DNS, TLS, etc.).

    // --- BytecodeVM-specific kinds ---
    NullPointer = 100,   ///< Null pointer dereference.
    StackOverflow = 101, ///< Call stack depth exceeded kMaxCallDepth.
    InvalidOpcode = 102, ///< Unrecognized or unsupported opcode.

    None = 255 ///< No trap (sentinel for normal execution).
};

/// @brief VM execution state.
/// @details Tracks the current phase of the virtual machine lifecycle.
enum class VMState {
    Ready,   ///< Module loaded, ready to execute.
    Running, ///< Currently executing bytecode.
    Halted,  ///< Execution completed normally.
    Trapped  ///< Execution halted due to an unhandled trap.
};

/// @brief Call frame for a single function invocation on the call stack.
/// @details Each call creates a new BCFrame that tracks the function being
///          executed, the program counter, pointers into the value stack for
///          locals and operand stack, and exception handler state.
struct BCFrame {
    const BytecodeFunction *func; ///< Function being executed in this frame.
    uint32_t pc;                  ///< Program counter (index into func->code).
    BCSlot *locals;               ///< Pointer to the first local variable slot.
    BCSlot *stackBase;            ///< Operand stack base for this frame.
    uint32_t ehStackDepth;        ///< Exception handler stack depth at frame entry.
    uint32_t callSitePc;          ///< PC at the call site (for debugging/stack traces).
    size_t allocaBase;            ///< Alloca stack position at frame entry (for cleanup).
};

/// @brief Exception handler entry on the handler stack.
/// @details Pushed by EH_PUSH and popped by EH_POP. When a trap occurs, the
///          VM walks the handler stack to find a matching handler for the
///          current frame.
struct BCExceptionHandler {
    uint32_t handlerPc;   ///< PC of the handler entry point.
    uint32_t frameIndex;  ///< Call stack frame index when this handler was registered.
    BCSlot *stackPointer; ///< Operand stack pointer when this handler was registered.
};

/// @brief Debug callback type for breakpoints and single-stepping.
/// @details Called by the VM when a breakpoint is hit or during single-step
///          execution. The callback can inspect VM state and decide whether
///          to continue execution.
/// @param vm            Reference to the executing BytecodeVM.
/// @param func          The function currently being executed.
/// @param pc            The current program counter.
/// @param is_breakpoint True if this is a breakpoint hit; false for single-step.
/// @return True to continue execution; false to pause.
using DebugCallback =
    std::function<bool(class BytecodeVM &, const BytecodeFunction *, uint32_t, bool)>;

/// @brief Bytecode virtual machine for executing compiled Zanna programs.
/// @details The BytecodeVM loads a BytecodeModule and executes its functions
///          using a stack-based evaluation model. Features include:
///          - Operand stack and local variable slots per frame
///          - Nested function calls with configurable max depth
///          - Exception handling with try/catch-style handler registration
///          - Native function integration (via RuntimeBridge or registered handlers)
///          - Debug support (breakpoints, single-stepping, variable inspection)
///          - Optional threaded dispatch for higher throughput on GCC/Clang
///
/// @invariant `sp_` and every frame pointer refer into `valueStack_`.
/// @invariant String ownership flags mirror live value/global slots.
/// @invariant Trusted dispatch is active only after module validation succeeds.
/// @note Instances are mutable execution engines and are not thread-safe.
class BytecodeVM {
  public:
    /// @brief Construct a new BytecodeVM in the Ready state.
    /// @post Execution buffers are preallocated and no module is loaded.
    BytecodeVM();

    /// @brief Destroy the VM and release all internal resources.
    /// @details Releases owned stack/global strings and cached literal handles.
    ~BytecodeVM();

    /// @brief Load a bytecode module for execution.
    /// @details Initializes the VM's global variable storage, string literal
    ///          cache, and internal state from the module. The module pointer
    ///          must remain valid for the lifetime of the VM.
    /// @param module Pointer to the BytecodeModule to execute (must not be null).
    void load(const BytecodeModule *module);

    /// @brief Execute a function by name and return its result.
    /// @details Looks up the function in the loaded module, pushes a new frame,
    ///          executes until the function returns or a trap occurs, and returns
    ///          the result slot. For void functions, the returned BCSlot is zero.
    /// @param funcName The fully qualified name of the function to execute.
    /// @param args     Arguments to pass to the function (default: empty).
    /// @return The function's return value as a BCSlot.
    BCSlot exec(const std::string &funcName, const std::vector<BCSlot> &args = {});

    /// @brief Execute a function by pointer and return its result.
    /// @details Directly executes the given function without a name lookup.
    /// @param func Pointer to the BytecodeFunction to execute (must not be null).
    /// @param args Arguments to pass to the function (default: empty).
    /// @return The function's return value as a BCSlot.
    BCSlot exec(const BytecodeFunction *func, const std::vector<BCSlot> &args = {});

    /// @brief Invoke a void function while another bytecode frame is active.
    /// @details Used by synchronous native callback bridges. The callback runs on
    ///          the same VM instance so module globals and runtime state match
    ///          direct bytecode execution, then dispatch stops when the callback
    ///          frame returns to its original caller depth.
    /// @param func Void function to invoke.
    /// @param args Arguments to pass to the callback.
    /// @return True when the callback returned normally; false after a trap.
    bool invokeVoidReentrant(const BytecodeFunction *func, const std::vector<BCSlot> &args = {});

    /// @brief Invoke a value-returning function while another bytecode frame is active.
    /// @details Counterpart of @ref invokeVoidReentrant for callback bridges that
    ///          need the callback's result (e.g. Lazy suppliers and Map callbacks).
    ///          The return value is read from the reentrant frame boundary slot
    ///          after dispatch stops at the original caller depth.
    /// @param func Value-returning function to invoke.
    /// @param args Arguments to pass to the callback.
    /// @param out Receives the callback's return slot on success.
    /// @return True when the callback returned normally; false after a trap.
    bool invokeValueReentrant(const BytecodeFunction *func,
                              const std::vector<BCSlot> &args,
                              BCSlot *out);

    /// @brief Get the current VM execution state.
    /// @return The current VMState (Ready, Running, Halted, or Trapped).
    VMState state() const {
        return state_;
    }

    /// @brief Get the kind of the last trap that occurred.
    /// @return The TrapKind of the most recent trap, or TrapKind::None.
    TrapKind trapKind() const {
        return trapKind_;
    }

    /// @brief Get the human-readable message of the last trap.
    /// @return A reference to the trap message string (empty if no trap).
    const std::string &trapMessage() const {
        return trapMessage_;
    }

    /// @brief Get the total number of instructions executed (for profiling).
    /// @return Cumulative instruction count since the last reset.
    uint64_t instrCount() const {
        return instrCount_;
    }

    /// @brief Reset the instruction counter to zero.
    /// @post @ref instrCount returns zero until another instruction is dispatched.
    void resetInstrCount() {
        instrCount_ = 0;
    }

    /// @brief Set the maximum number of instructions this VM may dispatch.
    /// @details A value of 0 disables the limit.
    /// @param maxInstructions Dispatch budget, or zero for unlimited execution.
    void setMaxInstructions(uint64_t maxInstructions) {
        maxInstrCount_ = maxInstructions;
    }

    /// @brief Get the configured instruction dispatch limit.
    /// @return Dispatch budget, or zero when unlimited.
    uint64_t maxInstructions() const {
        return maxInstrCount_;
    }

    /// @brief Enable or disable the RuntimeBridge for native function calls.
    /// @details When enabled, CALL_NATIVE instructions route through the Zanna
    ///          RuntimeBridge. When disabled, only directly registered handlers
    ///          are used.
    /// @param enabled True to enable the RuntimeBridge; false to disable.
    void setRuntimeBridgeEnabled(bool enabled) {
        runtimeBridgeEnabled_ = enabled;
    }

    /// @brief Check whether the RuntimeBridge is enabled.
    /// @return True if native calls use the RuntimeBridge; false otherwise.
    bool runtimeBridgeEnabled() const {
        return runtimeBridgeEnabled_;
    }

    /// @brief Enable or disable trusted dispatch for verified bytecode.
    /// @details Trusted dispatch skips per-instruction PC and stack-shape
    ///          validation in the interpreter loop. Use only for modules
    ///          produced by BytecodeCompiler::compileChecked after IL verification.
    /// @param enabled Requested trusted-dispatch state. It takes effect only
    ///        after the loaded module passes VM structural validation.
    void setTrustedDispatch(bool enabled) {
        trustedDispatchRequested_ = enabled;
        trustedDispatch_ = enabled && moduleDispatchValidated_;
    }

    /// @brief Check whether trusted dispatch is enabled.
    /// @return `true` only when requested and safe for the loaded module.
    bool trustedDispatch() const {
        return trustedDispatch_;
    }

    /// @brief Register a native handler for direct invocation by name.
    /// @details Handlers registered here bypass the RuntimeBridge and are
    ///          called directly by the VM when a matching CALL_NATIVE is executed.
    /// @param name    The fully qualified native function name to register.
    /// @param handler The callback to invoke when this function is called.
    void registerNativeHandler(const std::string &name, NativeHandler handler);

    /// @brief Snapshot of worker-relevant execution settings.
    /// @details Child bytecode VMs spawned for threads, async work, or HTTP
    ///          callbacks must copy this state instead of borrowing the parent
    ///          VM object, which may be destroyed before the worker finishes.
    struct ExecutionEnvironment {
        /// Route native calls through RuntimeBridge.
        bool runtimeBridgeEnabled = false;
        /// Prefer computed-goto dispatch where available.
        bool useThreadedDispatch = true;
        /// Request validated fast-path dispatch.
        bool trustedDispatch = false;
        /// Worker dispatch budget; zero is unlimited.
        uint64_t maxInstructions = 0;
        /// Worker-owned copy of direct native handlers.
        std::unordered_map<std::string, NativeHandler> nativeHandlers;
    };

    /// @brief Capture the current worker-relevant execution settings.
    /// @return Independent settings snapshot that does not retain this VM.
    [[nodiscard]] ExecutionEnvironment captureExecutionEnvironment() const;

    /// @brief Apply a previously captured worker execution environment.
    /// @param env Snapshot whose settings and handlers are copied.
    void applyExecutionEnvironment(const ExecutionEnvironment &env);

    /// @brief Copy worker-relevant execution settings from another BytecodeVM.
    /// @details Used by worker VMs spawned from Thread.Start/Async.Run so they
    ///          inherit the parent VM's runtime bridge toggle, dispatch mode,
    ///          and direct native handler registrations.
    /// @param other Source VM; module and transient execution state are not copied.
    void copyExecutionEnvironmentFrom(const BytecodeVM &other);

    /// @brief Enable or disable threaded dispatch (computed goto).
    /// @details Threaded dispatch uses compiler-specific computed goto for
    ///          faster opcode dispatch. Only available on GCC and Clang.
    ///          Falls back to switch-based dispatch when not available.
    /// @param enabled True to enable threaded dispatch; false for switch-based.
    void setThreadedDispatch(bool enabled) {
        useThreadedDispatch_ = enabled;
    }

    /// @brief Check whether threaded dispatch is enabled.
    /// @return True if threaded dispatch is active; false otherwise.
    bool useThreadedDispatch() const {
        return useThreadedDispatch_;
    }

    //==========================================================================
    // Debug Support
    //==========================================================================

    /// @brief Set the debug callback for breakpoints and single-stepping.
    /// @param callback The callback function to invoke on debug events.
    void setDebugCallback(DebugCallback callback) {
        debugCallback_ = std::move(callback);
    }

    /// @brief Enable or disable single-step execution mode.
    /// @details When enabled, the debug callback is invoked before each
    ///          instruction is executed.
    /// @param enabled True to enable single-stepping; false to disable.
    void setSingleStep(bool enabled) {
        singleStep_ = enabled;
    }

    /// @brief Check whether single-step mode is enabled.
    /// @return True if single-stepping is active; false otherwise.
    bool singleStep() const {
        return singleStep_;
    }

    /// @brief Set a breakpoint at a specific program counter in a function.
    /// @param funcName The fully qualified name of the function.
    /// @param pc       The program counter (instruction index) to break at.
    void setBreakpoint(const std::string &funcName, uint32_t pc);

    /// @brief Clear a previously set breakpoint.
    /// @param funcName The fully qualified name of the function.
    /// @param pc       The program counter of the breakpoint to clear.
    void clearBreakpoint(const std::string &funcName, uint32_t pc);

    /// @brief Clear all breakpoints in all functions.
    void clearAllBreakpoints();

    /// @brief Get the current program counter.
    /// @details Returns the PC of the current frame, or 0 if no frame is active.
    /// @return The current instruction index.
    uint32_t currentPc() const {
        return fp_ ? fp_->pc : 0;
    }

    /// @brief Get the function currently being executed.
    /// @return Pointer to the current BytecodeFunction, or nullptr if idle.
    const BytecodeFunction *currentFunction() const {
        return fp_ ? fp_->func : nullptr;
    }

    /// @brief Get the current exception handler stack depth.
    /// @return Number of exception handlers currently registered.
    size_t exceptionHandlerDepth() const {
        return ehStack_.size();
    }

    /// @brief Get the source line number corresponding to the current PC.
    /// @return The source line number, or 0 if debug info is not available.
    uint32_t currentSourceLine() const;

    /// @brief Get the source line number for a specific PC in a function.
    /// @param func The function containing the PC.
    /// @param pc   The program counter to look up.
    /// @return The source line number, or 0 if debug info is not available.
    static uint32_t getSourceLine(const BytecodeFunction *func, uint32_t pc);

  private:
    /// @brief The module being executed (borrowed, non-owning pointer).
    const BytecodeModule *module_;

    // Execution state
    VMState state_;                     ///< Current VM execution state.
    TrapKind trapKind_;                 ///< Kind of the most recent trap.
    int32_t currentErrorCode_;          ///< Error code for the current exception handler.
    bool pendingTrapErrorCode_ = false; ///< True after dispatchTrap sets a code for trap().
    std::string trapMessage_;           ///< Human-readable message for the most recent trap.

    /// @brief Snapshot of the most recent resumable trap state.
    struct TrapRecord {
        bool valid = false;
        TrapKind kind = TrapKind::None;
        int32_t errorCode = 0;
        uint32_t faultPc = 0;
        uint32_t nextPc = 0;
        int32_t faultLine = -1;
        size_t valueCount = 0;
        size_t stackPointerIndex = 0;
        size_t resumeStackPointerIndex = 0;
        size_t allocaSize = 0;
        std::vector<BCSlot> valueSlots;
        std::vector<uint8_t> valueOwned;
        std::vector<BCFrame> callStack;
        std::vector<BCExceptionHandler> ehStack;
        std::vector<uint8_t> allocaBytes;
    };

    TrapRecord trapRecord_;

    /// @brief Value stack holding locals and operand stack entries for all frames.
    std::vector<BCSlot> valueStack_;

    /// @brief Per-slot ownership flags for managed string values in @ref valueStack_.
    ValueStackStringOwnership valueStackStringOwned_;

    /// @brief Call stack of active function frames.
    std::vector<BCFrame> callStack_;

    /// @brief Current stack pointer (points to the next free operand slot).
    BCSlot *sp_;

    /// @brief Current frame pointer (top of the call stack).
    BCFrame *fp_;

    // Profiling
    uint64_t instrCount_;    ///< Cumulative instruction count for profiling.
    uint64_t maxInstrCount_; ///< Maximum dispatched instructions before trapping (0 = unlimited).

    // Runtime integration
    bool runtimeBridgeEnabled_;             ///< Whether CALL_NATIVE routes through RuntimeBridge.
    bool useThreadedDispatch_;              ///< Whether to use computed-goto dispatch.
    bool trustedDispatch_;                  ///< Whether verified bytecode skips hot-path guards.
    bool trustedDispatchRequested_ = false; ///< User-requested trusted dispatch preference.
    bool moduleDispatchValidated_ = false;  ///< True once load() validates module headers/tables.
    bool loadFailed_ = false; ///< True when the most recent load() rejected the module.
    std::unordered_map<std::string, NativeHandler> nativeHandlers_; ///< Registered native handlers.

    /// @brief Exception handler stack (pushed by EH_PUSH, popped by EH_POP).
    std::vector<BCExceptionHandler> ehStack_;

    /// @brief Call-stack depth where a reentrant callback invocation should stop.
    /// @details SIZE_MAX means normal top-level execution with no reentrant stop boundary.
    size_t reentrantStopDepth_;

    /// @brief Operand stack pointer restored when a reentrant callback returns.
    /// @details Preserves the suspended native call's argument slots across callback dispatch.
    BCSlot *reentrantReturnSp_;

    /// @brief Alloca buffer for stack allocations (separate from the operand stack).
    std::vector<uint8_t> allocaBuffer_;

    /// @brief Current allocation position in the alloca buffer.
    size_t allocaTop_;

    // Debug support
    bool singleStep_;             ///< Whether single-step mode is active.
    DebugCallback debugCallback_; ///< Callback for debug events.
    std::unordered_map<std::string, std::set<uint32_t>>
        breakpoints_; ///< Per-function breakpoint PCs.

    /// @brief Global variable storage (one BCSlot per global, indexed by global index).
    std::vector<BCSlot> globals_;

    /// @brief Per-global ownership flags for runtime string handles stored in globals_.
    std::vector<uint8_t> globalsStringOwned_;

    /// @brief String literal cache storing proper rt_string objects for string constants.
    /// @details Indexed by string pool index and materialized lazily. Ensures the
    ///          runtime receives rt_string pointers rather than raw C strings.
    std::vector<rt_string> stringCache_;

    /// @brief Reset the string cache slots for the loaded module.
    void initStringCache();

    /// @brief Return a cached runtime string for a string-pool entry.
    /// @param idx Zero-based string-pool index.
    /// @return Borrowed cached handle, or null for an invalid index.
    [[nodiscard]] rt_string getStringLiteral(uint16_t idx);

    /// @brief Compute the backing-array index for a stack/local slot.
    /// @param slot Address within the fixed value stack.
    /// @return Zero-based index used by the parallel ownership bitmap.
    [[nodiscard]] size_t slotIndex(const BCSlot *slot) const;

    /// @brief Check whether a stack/local slot currently owns a string reference.
    /// @param slot Address within the fixed value stack.
    /// @return Ownership bit associated with @p slot.
    [[nodiscard]] bool slotOwnsString(const BCSlot *slot) const;

    /// @brief Update the ownership flag for a stack/local slot.
    /// @param slot Address within the fixed value stack.
    /// @param owns New ownership state.
    void setSlotOwnsString(const BCSlot *slot, bool owns);

    /// @brief Return whether a bytecode local stores runtime string handles.
    /// @param frame Frame whose local-type metadata is consulted.
    /// @param idx Zero-based local index.
    /// @return `true` when the local is string-typed.
    [[nodiscard]] bool localIsString(const BCFrame &frame, uint32_t idx) const;

    /// @brief Validate that a pointer is a live runtime string handle.
    /// @param ptr Candidate handle; null is accepted.
    /// @param site Operation name included in a trap.
    /// @return `true` for null or a registered live handle.
    [[nodiscard]] bool validateStringHandle(const void *ptr, const char *site);

    /// @brief Release an owned string slot and clear its ownership flag.
    /// @param slot Mutable value-stack slot.
    void releaseOwnedString(BCSlot *slot);

    /// @brief Retain a string slot and mark it as owning the retained handle.
    /// @details Used by STR_RETAIN and string-producing helpers to apply the
    ///          same validation, reference-count increment, and ownership-flag
    ///          update in every bytecode dispatch engine.
    /// @param slot Stack/local slot containing the string handle to retain.
    /// @param site Diagnostic site passed to string-handle validation.
    /// @return True when the slot was null or retained successfully; false when
    ///         validation trapped.
    bool retainStringSlot(BCSlot *slot, const char *site);

    /// @brief Copy one stack slot and retain its string handle when ownership is duplicated.
    /// @param dst Destination value-stack slot that receives the copied value.
    /// @param src Source value-stack slot to duplicate.
    /// @param site Diagnostic site passed to string-handle validation.
    /// @return True when the copy is complete; false after raising a runtime trap.
    bool copyStackSlotRetainingString(BCSlot *dst, const BCSlot *src, const char *site);

    /// @brief Implement the ownership-aware `DUP` stack opcode.
    /// @param site Diagnostic site passed to string-handle validation.
    /// @return True when the top slot was duplicated; false after raising a runtime trap.
    bool duplicateTopSlot(const char *site);

    /// @brief Implement the ownership-aware `DUP2` stack opcode.
    /// @param site Diagnostic site passed to string-handle validation.
    /// @return True when the top two slots were duplicated; false after raising a runtime trap.
    bool duplicateTopTwoSlots(const char *site);

    /// @brief Release and pop one or more slots from the operand stack.
    /// @param count Number of topmost operand slots to release and remove.
    void popOwnedSlots(size_t count);

    /// @brief Implement the ownership-aware `SWAP` stack opcode.
    void swapTopTwoSlots();

    /// @brief Implement the ownership-aware `ROT3` stack opcode.
    void rotateTopThreeSlots();

    /// @brief Push a local value onto the operand stack, retaining strings.
    /// @param idx Local slot index to push.
    /// @param site Diagnostic site passed to string-handle validation.
    /// @return True when the value was pushed; false after raising a runtime trap.
    bool pushLocal(uint32_t idx, const char *site);

    /// @brief Pop the operand stack into a local slot with string ownership transfer.
    /// @param idx Local slot index to receive the popped value.
    /// @param site Diagnostic site passed to string-handle validation.
    /// @return True when the value was stored; false after raising a runtime trap.
    bool storeLocal(uint32_t idx, const char *site);

    /// @brief Return the top operand value from the current frame.
    /// @return True when execution should continue in the caller; false when the entry frame halted
    ///         or a trap was raised while unwinding.
    bool returnValueFromFrame();

    /// @brief Return void from the current frame.
    /// @return True when execution should continue in the caller; false when the entry frame
    /// halted.
    bool returnVoidFromFrame();

    /// @brief Push a global value onto the operand stack, retaining strings.
    /// @param idx Global slot index to load.
    /// @param site Diagnostic site passed to string-handle validation.
    /// @return True when the global was loaded; false after raising a runtime trap.
    bool loadGlobal(uint16_t idx, const char *site);

    /// @brief Pop the operand stack into a global slot with string ownership transfer.
    /// @param idx Global slot index to store.
    /// @param site Diagnostic site passed to string-handle validation.
    /// @return True when the global was stored; false after raising a runtime trap.
    bool storeGlobal(uint16_t idx, const char *site);

    /// @brief Release owned string arguments about to be popped after a native call.
    /// @param args First call-argument slot.
    /// @param argCount Number of consecutive slots to release.
    void releaseCallArgs(BCSlot *args, uint8_t argCount);

    /// @brief Clear ownership for string arguments consumed directly by a callee.
    /// @details Some runtime helpers, such as `rt_str_release_maybe`, take
    ///          ownership of their incoming string handles. Those slots must be
    ///          detached before @ref releaseCallArgs runs to avoid a second
    ///          release on the same handle.
    /// @param ref Native-call descriptor identifying consumed string parameters.
    /// @param args First call-argument slot.
    /// @param argCount Number of consecutive argument slots.
    void dismissConsumedStringArgs(const NativeFuncRef &ref, BCSlot *args, uint8_t argCount);

    /// @brief Release string-owning locals in a frame before unwinding it.
    /// @param frame Frame whose local slots are being destroyed.
    void releaseFrameLocals(const BCFrame &frame);

    /// @brief Release any string-owning values left on the stack before reuse.
    void releaseOwnedValueStack();

    /// @brief Release any string-owning globals before reload or destruction.
    void releaseOwnedGlobals();

    /// @brief Return the global index for an exact pointer to a global slot, or SIZE_MAX.
    /// @param ptr Candidate exact slot address.
    /// @return Global index or `SIZE_MAX`.
    [[nodiscard]] size_t globalIndexForAddress(const void *ptr) const;

    /// @brief Return the global index overlapped by an address range, or SIZE_MAX.
    /// @param ptr First byte of the candidate range.
    /// @param bytes Range width.
    /// @return First overlapped global index or `SIZE_MAX`.
    [[nodiscard]] size_t globalIndexForAddressRange(const void *ptr, size_t bytes) const;

    /// @brief Release a string owned by a single global slot.
    /// @param idx Global-table index.
    void releaseOwnedGlobalString(size_t idx);

    /// @brief Clear global string ownership before writing raw non-string data through a pointer.
    /// @param ptr First byte of the raw store.
    /// @param bytes Store width.
    void clearGlobalStringOwnershipForRawStore(void *ptr, size_t bytes);

    /// @brief Record string ownership when STORE_STR_MEM writes directly to a global slot.
    /// @param ptr Exact global slot address.
    /// @param owns New ownership state.
    void setGlobalStringOwnershipForAddress(void *ptr, bool owns);

    /// @brief Release any retained string handles held by the active trap record.
    void clearTrapRecord();

    /// @brief Reset execution state and release owned values from the prior run.
    void resetExecutionState();

    /// @brief Return the current operand depth above @p frame.stackBase.
    /// @param frame Frame defining the operand-stack base.
    /// @param sp Current stack pointer.
    /// @return Number of live operand slots.
    [[nodiscard]] uint32_t operandDepth(const BCFrame &frame, const BCSlot *sp) const;

    /// @brief Validate that @p pc is a valid instruction fetch location.
    /// @param func Function whose code bounds apply.
    /// @param pc Candidate program counter.
    /// @param site Dispatch path included in a trap.
    /// @return `true` when @p pc is in range.
    bool ensurePcInRange(const BytecodeFunction &func, uint32_t pc, const char *site);

    /// @brief Detailed reason for rejecting a bytecode module during load().
    struct ModuleValidationFailure {
        TrapKind kind = TrapKind::InvalidOpcode; ///< Trap kind reported to the caller.
        std::string message;                     ///< Human-readable validation failure.
    };

    /// @brief Validate bytecode module header, indexes, functions, and code tables.
    /// @details Trusted dispatch is enabled only after this check succeeds.
    /// @param module Candidate module passed to @ref load.
    /// @param failure Populated with the first validation failure.
    /// @return True when the module can be safely bound to this VM.
    bool validateModuleForLoad(const BytecodeModule *module,
                               ModuleValidationFailure &failure) const;

    /// @brief Validate one function's metadata and bytecode instruction stream.
    /// @param module Candidate module that owns @p func.
    /// @param func Function to validate.
    /// @param functionIndex Index of @p func within @p module.
    /// @param failure Populated with the first validation failure.
    /// @return True when the function metadata and instruction stream are well-formed.
    bool validateFunctionForLoad(const BytecodeModule &module,
                                 const BytecodeFunction &func,
                                 size_t functionIndex,
                                 ModuleValidationFailure &failure) const;

    /// @brief Return whether @p func is one of the loaded module's functions.
    /// @details Protects the pointer-based exec overload from being handed a
    ///          stale or foreign function pointer.
    /// @param func Candidate function address.
    /// @return `true` when @p func belongs to the loaded function table.
    [[nodiscard]] bool functionBelongsToModule(const BytecodeFunction *func) const;

    /// @brief Validate that @p words extra code words can be read from @p pc.
    /// @param func Function whose code bounds apply.
    /// @param pc First word to read.
    /// @param words Required contiguous word count.
    /// @param site Dispatch path included in a trap.
    /// @return `true` when the full range is available.
    bool ensureWordsAvailable(const BytecodeFunction &func,
                              uint32_t pc,
                              uint32_t words,
                              const char *site);

    /// @brief Validate that a computed branch target lands inside @p func.
    /// @param func Function whose code bounds apply.
    /// @param target Absolute target program counter.
    /// @param site Operation name included in a trap.
    /// @return `true` when @p target is within the code stream.
    bool ensureBranchTarget(const BytecodeFunction &func, uint32_t target, const char *site);

    /// @brief Validate stack depth and capacity requirements for @p instr.
    /// @param frame Current call frame.
    /// @param sp Current operand stack pointer.
    /// @param instr Encoded instruction whose stack effect is checked.
    /// @param site Dispatch path included in a trap.
    /// @return `true` when execution cannot underflow or overflow the frame stack.
    bool ensureStackForInstruction(const BCFrame &frame,
                                   const BCSlot *sp,
                                   uint32_t instr,
                                   const char *site);

    /// @brief Validate that the active operand stack matches the callee arity exactly.
    /// @param func Candidate callee.
    /// @param caller Current frame, or null for an entry call.
    /// @param sp Stack pointer after arguments are pushed.
    /// @param site Call path included in a trap.
    /// @return `true` when operand depth equals the declared parameter count.
    bool ensureCallArity(const BytecodeFunction *func,
                         const BCFrame *caller,
                         const BCSlot *sp,
                         const char *site);

    /// @brief Validate frame-local and operand-stack footprint for a callee.
    /// @param func Candidate callee.
    /// @param sp Stack pointer after arguments are pushed.
    /// @param site Call path included in a trap.
    /// @return `true` when the frame fits in `valueStack_`.
    bool ensureFrameFootprint(const BytecodeFunction *func, const BCSlot *sp, const char *site);

    /// @brief Clone runtime-call arguments for helpers that consume strings.
    /// @details The bytecode VM stores raw slot aliases in locals and on the
    ///          operand stack. Helpers such as `Zanna.String.Concat` release
    ///          their input strings, so the bridge must receive retained copies
    ///          rather than the VM's original aliases.
    /// @param ref Runtime helper reference.
    /// @param args Argument slots currently on the operand stack.
    /// @param argCount Number of arguments in @p args.
    /// @return Retained argument copy for consuming helpers; otherwise an empty
    ///         vector.
    std::vector<BCSlot> cloneRuntimeStringArgs(const NativeFuncRef &ref,
                                               const BCSlot *args,
                                               size_t argCount) const;

    /// @brief Release any retained string arguments created for a runtime call.
    /// @param ref Runtime helper reference.
    /// @param args Retained argument copy returned by @ref cloneRuntimeStringArgs.
    void releaseRuntimeStringArgs(const NativeFuncRef &ref, std::vector<BCSlot> &args) const;

    /// @brief Invoke a runtime-bridge native helper with bytecode trap integration.
    /// @details Returns false when execution was diverted into bytecode trap
    ///          handling, either because a handler caught the trap or because
    ///          the trap became terminal.
    /// @param ref Native helper descriptor and signature metadata.
    /// @param args Borrowed argument slots.
    /// @param argCount Number of argument slots.
    /// @param result Receives return bits on success.
    /// @return `true` on normal return; `false` when the call traps.
    bool invokeRuntimeBridgeNative(const NativeFuncRef &ref,
                                   BCSlot *args,
                                   uint8_t argCount,
                                   BCSlot &result);

    /// @brief Return whether a runtime helper consumes retained clones of string args.
    /// @param name Runtime registry name.
    /// @return `true` when string arguments must be retained before dispatch.
    [[nodiscard]] static bool runtimeCallConsumesClonedStringArgs(std::string_view name);

    /// @brief Return whether a runtime helper consumes the original string arguments.
    /// @param name Runtime registry name.
    /// @return `true` when ownership transfers from the VM's argument slots.
    [[nodiscard]] static bool runtimeCallConsumesOwnedStringArgs(std::string_view name);

    /// @brief Main interpreter loop using switch-based dispatch.
    void run();

    /// @brief Threaded interpreter loop using computed-goto dispatch (faster).
    /// @details Only available when compiled with GCC or Clang (ZANNA_BC_THREADED).
#if defined(__GNUC__) || defined(__clang__)
    void runThreaded();
#endif

    /// @brief Enter a new call frame with arguments already pushed on the stack.
    /// @details Shared frame setup used by direct and reentrant bytecode calls.
    /// @param func The function to enter.
    /// @param site Diagnostic site name for frame validation traps.
    void enterCallFrame(const BytecodeFunction *func, const char *site);

    /// @brief Push a new call frame for the given function.
    /// @details Validates the active operand stack depth against the callee arity.
    /// @param func The function to call.
    void call(const BytecodeFunction *func);

    /// @brief Push a callback frame while a runtime call is suspended.
    /// @details Uses only the arguments at the top of the stack; the suspended
    ///          caller may still have runtime-call operands below them.
    /// @param func The callback function to call.
    void callReentrant(const BytecodeFunction *func);

    /// @brief Pop the current call frame and restore the caller's state.
    /// @return True if there are more frames on the call stack; false if the
    ///         top-level function has returned.
    bool popFrame();

    /// @brief Raise a trap with the specified kind and message.
    /// @details Sets the VM state to Trapped and attempts to dispatch to an
    ///          exception handler. If no handler is found, execution halts.
    /// @param kind    The trap classification.
    /// @param message A human-readable description of the error.
    void trap(TrapKind kind, const char *message);

    /// @brief Format the current execution location and trap payload for users.
    /// @param kind Trap category.
    /// @param errorCode Runtime-compatible numeric code.
    /// @param message Optional diagnostic detail.
    /// @return Source-aware human-readable trap report.
    [[nodiscard]] std::string formatTrapMessage(TrapKind kind,
                                                int32_t errorCode,
                                                const char *message) const;

    /// @brief Return the instruction PC responsible for the current trap.
    /// @return Faulting PC, or zero when no frame is active.
    [[nodiscard]] uint32_t currentFaultPc() const;

    /// @brief Resolve the basic block label associated with a function PC.
    /// @param func Function containing @p pc.
    /// @param pc Program counter to resolve.
    /// @return Block label, or an empty string.
    [[nodiscard]] std::string currentBlockLabelForPc(const BytecodeFunction *func,
                                                     uint32_t pc) const;

    /// @brief Resolve the source file path associated with a function PC.
    /// @param func Function containing @p pc.
    /// @param pc Program counter to resolve.
    /// @return Source path, or an empty string.
    [[nodiscard]] std::string currentSourcePathForPc(const BytecodeFunction *func,
                                                     uint32_t pc) const;

    /// @brief Check for signed addition overflow.
    /// @param a      Left operand.
    /// @param b      Right operand.
    /// @param result [out] The result of a + b (valid only when no overflow).
    /// @return True if the addition overflows; false otherwise.
    bool addOverflow(int64_t a, int64_t b, int64_t &result);

    /// @brief Check for signed subtraction overflow.
    /// @param a      Left operand.
    /// @param b      Right operand.
    /// @param result [out] The result of a - b (valid only when no overflow).
    /// @return True if the subtraction overflows; false otherwise.
    bool subOverflow(int64_t a, int64_t b, int64_t &result);

    /// @brief Check for signed multiplication overflow.
    /// @param a      Left operand.
    /// @param b      Right operand.
    /// @param result [out] The result of a * b (valid only when no overflow).
    /// @return True if the multiplication overflows; false otherwise.
    bool mulOverflow(int64_t a, int64_t b, int64_t &result);

    /// @brief Execute signed division without invoking host-language UB.
    /// @param a Dividend.
    /// @param b Divisor.
    /// @param result Receives the quotient on success.
    /// @param fault Receives the failure category or `None`.
    /// @return `true` when division succeeds.
    bool safeSignedDiv(int64_t a, int64_t b, int64_t &result, TrapKind &fault) const;

    /// @brief Execute unsigned division without invoking host-language UB.
    /// @param a Dividend bit pattern.
    /// @param b Divisor bit pattern.
    /// @param result Receives the quotient bit pattern on success.
    /// @param fault Receives `DivideByZero` or `None`.
    /// @return `true` when division succeeds.
    bool safeUnsignedDiv(int64_t a, int64_t b, int64_t &result, TrapKind &fault) const;

    /// @brief Execute signed remainder without invoking host-language UB.
    /// @param a Dividend.
    /// @param b Divisor.
    /// @param result Receives the remainder on success.
    /// @param fault Receives `DivideByZero` or `None`.
    /// @return `true` when remainder succeeds.
    bool safeSignedRem(int64_t a, int64_t b, int64_t &result, TrapKind &fault) const;

    /// @brief Execute unsigned remainder without invoking host-language UB.
    /// @param a Dividend bit pattern.
    /// @param b Divisor bit pattern.
    /// @param result Receives the remainder bit pattern on success.
    /// @param fault Receives `DivideByZero` or `None`.
    /// @return `true` when remainder succeeds.
    bool safeUnsignedRem(int64_t a, int64_t b, int64_t &result, TrapKind &fault) const;

    /// @brief Execute signed negation without invoking host-language UB.
    /// @param value Operand to negate.
    /// @param result Receives the negated value on success.
    /// @param fault Receives `Overflow` or `None`.
    /// @return `true` when negation succeeds.
    bool safeNegate(int64_t value, int64_t &result, TrapKind &fault) const;

    /// @brief Add with defined two's-complement wrapping.
    /// @param a Left operand.
    /// @param b Right operand.
    /// @return Wrapped sum.
    static int64_t wrappingAdd(int64_t a, int64_t b) noexcept;

    /// @brief Subtract with defined two's-complement wrapping.
    /// @param a Left operand.
    /// @param b Right operand.
    /// @return Wrapped difference.
    static int64_t wrappingSub(int64_t a, int64_t b) noexcept;

    /// @brief Multiply with defined two's-complement wrapping.
    /// @param a Left operand.
    /// @param b Right operand.
    /// @return Wrapped product.
    static int64_t wrappingMul(int64_t a, int64_t b) noexcept;

    /// @brief Left-shift with a masked shift count and defined bit semantics.
    /// @param value Bit pattern to shift.
    /// @param shift Count whose low six bits are used.
    /// @return Shifted bit pattern.
    static int64_t wrappingShl(int64_t value, int64_t shift) noexcept;

    /// @brief Arithmetic right-shift with portable sign extension.
    /// @param value Signed bit pattern to shift.
    /// @param shift Count whose low six bits are used.
    /// @return Sign-extended shifted value.
    static int64_t arithmeticShr(int64_t value, int64_t shift) noexcept;

    /// @brief Convert f64 to i64 with truncation and defined range checks.
    /// @param value Floating-point input.
    /// @param result Receives the converted value on success.
    /// @param fault Receives `InvalidCast`, `Overflow`, or `None`.
    /// @return `true` for an in-range finite input.
    static bool truncF64ToI64(double value, int64_t &result, TrapKind &fault) noexcept;

    /// @brief Convert f64 to i64 using round-to-even and defined range checks.
    /// @param value Floating-point input.
    /// @param result Receives the rounded value on success.
    /// @param fault Receives `InvalidCast`, `Overflow`, or `None`.
    /// @return `true` for an in-range finite input.
    static bool roundF64ToI64(double value, int64_t &result, TrapKind &fault) noexcept;

    /// @brief Convert f64 to a u64 bit pattern using round-to-even.
    /// @param value Floating-point input.
    /// @param result Receives the unsigned result bits on success.
    /// @param fault Receives `InvalidCast`, `Overflow`, or `None`.
    /// @return `true` for a nonnegative in-range finite input.
    static bool roundF64ToU64Bits(double value, int64_t &result, TrapKind &fault) noexcept;

    /// @brief Dispatch a resumable trap, or make it terminal when no handler exists.
    /// @param kind Trap category.
    /// @param message Diagnostic detail.
    /// @param errorCode Explicit code, or -1 for the kind's default.
    /// @return `true` when an exception handler receives control.
    bool trapOrDispatch(TrapKind kind, const char *message, int32_t errorCode = -1);

    /// @brief Validate a current-frame local index.
    /// @param idx Candidate local index.
    /// @param site Operation name included in a trap.
    /// @return `true` when @p idx exists.
    bool ensureLocalIndex(uint32_t idx, const char *site);

    /// @brief Validate a host-memory access before dereferencing it.
    /// @param ptr First byte to access.
    /// @param bytes Access width.
    /// @param site Operation name included in a trap.
    /// @return `true` when the range is permitted.
    bool ensureMemoryAccess(const void *ptr, size_t bytes, const char *site);

    /// @brief Allocate zeroed, function-lifetime storage in the VM alloca arena.
    /// @param requestedSize Unaligned byte count.
    /// @param ptr Receives the allocation on success.
    /// @param site Operation name included in a trap.
    /// @return `true` when allocation succeeds.
    bool allocateAlloca(int64_t requestedSize, void *&ptr, const char *site);

    /// @brief Compute and validate a relative branch target without unsigned wraparound.
    /// @param func Function containing the branch.
    /// @param basePc PC used as the origin for @p offset.
    /// @param offset Signed relative offset.
    /// @param target [out] Absolute target PC on success.
    /// @param site Diagnostic site name.
    /// @return True on success; false after raising an InvalidOpcode trap.
    bool computeRelativeTarget(const BytecodeFunction &func,
                               uint32_t basePc,
                               int32_t offset,
                               uint32_t &target,
                               const char *site);

    /// @brief Add a signed byte offset to a pointer with overflow/null checks.
    /// @param base Base pointer operand.
    /// @param offset Signed byte offset operand.
    /// @param result [out] Adjusted pointer when the operation succeeds.
    /// @param site Diagnostic site name.
    /// @return True on success; false after raising or dispatching a trap.
    bool addPointerOffset(void *base, int64_t offset, void *&result, const char *site);

    /// @brief Resolve an unchecked numeric-array fast-path element address safely.
    /// @details The `ARR_*_FAST` opcodes intentionally skip logical length checks because the
    ///          optimizer only emits them after a dominating bounds proof. They still must not
    ///          overflow host address arithmetic or access memory outside VM-owned ranges. This
    ///          helper centralizes the common `base + index * sizeof(T)` computation for both
    ///          switch and threaded dispatch engines.
    /// @tparam Element Numeric element type stored by the runtime array payload.
    /// @param arrayPayload Non-null runtime array payload pointer. Callers perform the null trap
    ///        before stack mutation so trap snapshots preserve existing opcode semantics.
    /// @param idx Zero-based element index already popped or read from the bytecode stack.
    /// @param element [out] Pointer to the resolved element on success.
    /// @param overflowMessage Trap message used when pointer arithmetic would overflow.
    /// @param site Diagnostic site name passed to memory-access validation.
    /// @return True when @p element is safe to read/write; false after raising or dispatching a
    ///         bounds/null-style trap.
    template <typename Element>
    bool resolveArrayFastElement(void *arrayPayload,
                                 size_t idx,
                                 Element *&element,
                                 const char *overflowMessage,
                                 const char *site) {
        static_assert(std::is_same_v<Element, int32_t> || std::is_same_v<Element, int64_t> ||
                          std::is_same_v<Element, double>,
                      "ARR_*_FAST supports only i32, i64, and f64 payloads");

        const uintptr_t base = reinterpret_cast<uintptr_t>(arrayPayload);
        constexpr size_t elementSize = sizeof(Element);
        if (idx > std::numeric_limits<uintptr_t>::max() / elementSize ||
            idx * elementSize > std::numeric_limits<uintptr_t>::max() - base) {
            trapOrDispatch(TrapKind::Bounds, overflowMessage);
            return false;
        }

        element = reinterpret_cast<Element *>(base + idx * elementSize);
        return ensureMemoryAccess(element, elementSize, site);
    }

    /// @brief Push an exception handler onto the handler stack.
    /// @param handlerPc The PC of the handler entry point.
    void pushExceptionHandler(uint32_t handlerPc);

    /// @brief Pop the most recently pushed exception handler.
    void popExceptionHandler();

    /// @brief Attempt to dispatch a trap to a registered exception handler.
    /// @details Walks the handler stack looking for a handler in the current
    ///          or an enclosing frame. If found, unwinds the call and operand
    ///          stacks and transfers control to the handler.
    /// @param kind The trap kind to dispatch.
    /// @param errorCode Explicit runtime code, or -1 for the kind's default.
    /// @param message Optional diagnostic text retained in the trap record.
    /// @return True if a handler was found and control was transferred; false
    ///         if the trap is unhandled.
    bool dispatchTrap(TrapKind kind, int32_t errorCode = -1, const char *message = nullptr);

    /// @brief Restore execution state for resume.same or resume.next.
    /// @param useNextPc Resume after the fault when true; retry it when false.
    /// @return `true` when the operand token matches a live trap snapshot.
    bool resumeTrap(bool useNextPc);

    /// @brief Check whether the current PC matches a breakpoint.
    /// @return True if the debugger should pause; false otherwise.
    bool checkBreakpoint();

    /// @brief Notify the debugger and report whether execution should pause.
    /// @param isBreakpoint True for breakpoint events; false for single-step.
    /// @param pc Program counter to report to the debugger callback.
    /// @return True when execution should pause.
    bool requestDebugPause(bool isBreakpoint, uint32_t pc);
};

/// @brief Get the currently active BytecodeVM on this thread.
/// @details The active VM is set by ActiveBytecodeVMGuard and is used by
///          native functions that need to access VM state.
/// @return Pointer to the active BytecodeVM, or nullptr if none.
BytecodeVM *activeBytecodeVMInstance();

/// @brief Get the BytecodeModule of the active BytecodeVM on this thread.
/// @return Pointer to the module, or nullptr if no VM is active.
const BytecodeModule *activeBytecodeModule();

/// @brief RAII guard that sets the active BytecodeVM for the current thread.
/// @details On construction, saves the previous active VM and sets a new one.
///          On destruction, restores the previous active VM. This ensures
///          that re-entrant native calls see the correct VM context.
struct ActiveBytecodeVMGuard {
    /// @brief Set @p vm as the active VM for this thread.
    /// @param vm The BytecodeVM to make active (must not be null).
    explicit ActiveBytecodeVMGuard(BytecodeVM *vm);

    /// @brief Restore the previous active VM.
    ~ActiveBytecodeVMGuard();

    /// @brief Guards are unique scope owners and cannot be copied.
    ActiveBytecodeVMGuard(const ActiveBytecodeVMGuard &) = delete;

    /// @brief Guards are unique scope owners and cannot be copy-assigned.
    /// @return This guard; declaration is deleted and cannot be called.
    ActiveBytecodeVMGuard &operator=(const ActiveBytecodeVMGuard &) = delete;

  private:
    BytecodeVM *previous_; ///< The VM that was active before this guard.
    BytecodeVM *current_;  ///< The VM made active by this guard.
};

} // namespace bytecode
} // namespace zanna
