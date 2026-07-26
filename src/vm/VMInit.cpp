//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/VMInit.cpp
// Purpose: Construct VM instances and prepare execution state.
// Key invariants:
//   - VM function/global lookup tables are populated before execution starts.
//   - Runtime extern bridges are registered before interpreted programs call them.
// Ownership/Lifetime:
//   - VM instances borrow Module storage and own per-execution frame/global state.
//   - Runtime external registrations are process-wide and live for the process.
// Links: src/vm/VM.hpp, src/vm/RuntimeBridge.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements VM construction, shared program-state initialization,
///        frame setup, and reusable execution-buffer management.
/// @details Initialization validates runtime descriptors, selects configurable
///          dispatch behavior, materializes module globals and literal caches,
///          then prepares per-call frames without executing program code.

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Global.hpp"
#include "il/core/Module.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "vm/DiagFormat.hpp"
#include "vm/Marshal.hpp"
#include "vm/OpHandlerAccess.hpp"
#include "vm/RuntimeBridge.hpp"
#include "vm/VM.hpp"
#include "vm/VMConstants.hpp"
#include "vm/control_flow.hpp"

#include "rt_context.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace il::core;

namespace il::vm {

/// @brief Register thread and synchronization runtime externals.
void registerThreadsRuntimeExternals();
/// @brief Register network runtime externals.
void registerNetworkRuntimeExternals();
/// @brief Register 3D game runtime externals.
void registerGame3DRuntimeExternals();
/// @brief Register functional-programming runtime externals.
void registerFunctionalRuntimeExternals();

namespace {

/// @brief RAII helper that forces the process-wide numeric locale to "C".
/// @details The VM relies on deterministic decimal formatting for diagnostics
///          and trace output.  Constructing this static initializer once per
///          process sets the locale early so subsequent numeric prints remain
///          stable regardless of the host environment.
struct NumericLocaleInitializer {
    /// @brief Select the C numeric locale during process initialization.
    NumericLocaleInitializer() {
        std::setlocale(LC_NUMERIC, "C");
    }
};

/// @brief Process-lifetime instance that installs deterministic numeric formatting.
[[maybe_unused]] const NumericLocaleInitializer kNumericLocaleInitializer{};

/// @brief Process initializer that installs VM-owned runtime external groups.
struct ThreadsRuntimeInitializer {
    /// @brief Register every auxiliary runtime external group.
    ThreadsRuntimeInitializer() {
        registerThreadsRuntimeExternals();
        registerNetworkRuntimeExternals();
        registerGame3DRuntimeExternals();
        registerFunctionalRuntimeExternals();
    }
};

/// @brief Process-lifetime instance that performs auxiliary external registration.
[[maybe_unused]] const ThreadsRuntimeInitializer kThreadsRuntimeInitializer{};

/// @brief Validate runtime descriptors before VM execution starts.
/// @details The self-check is cached after the first run. Failures are surfaced
///          as constructor exceptions instead of aborting during static init,
///          so callers and tests can report the compiler/runtime configuration
///          problem as a normal diagnostic.
void ensureRuntimeDescriptorsValid() {
    static const bool descriptorsValid = il::runtime::selfCheckRuntimeDescriptors();
    if (!descriptorsValid)
        throw std::runtime_error("VM initialization failed: runtime descriptor self-check failed");
}

/// @brief Static initializer that registers the VM trap handler for invariant violations.
/// @details Enables the RuntimeSignatures layer to route invariant violations through
///          the VM's trap mechanism when a VM is active. When no VM is active, the
///          handler returns false and the violation falls back to abort behavior.
struct InvariantTrapHandlerRegistrar {
    /// @brief Install a callback that routes descriptor invariant failures through an active VM.
    InvariantTrapHandlerRegistrar() {
        /// @brief Route one runtime-descriptor invariant failure through the active VM.
        /// @param message Invariant diagnostic text.
        /// @return `false` when no VM can handle the trap or control unexpectedly returns.
        il::runtime::setInvariantTrapHandler([](const char *message) -> bool {
            // Check if there's an active VM that can handle the trap.
            if (!RuntimeBridge::hasActiveVm())
                return false;

            // Route through the RuntimeBridge trap mechanism.
            // This will invoke vm_raise() if a VM is active, which may throw
            // TrapDispatchSignal for exception handler dispatch.
            RuntimeBridge::trap(TrapKind::RuntimeError, message, {}, {}, {});

            // If we reach here, the trap was not caught (shouldn't happen with RuntimeError).
            // Return false to fall back to abort.
            return false;
        });
    }
};

/// @brief Process-lifetime instance that installs the invariant trap bridge.
[[maybe_unused]] const InvariantTrapHandlerRegistrar kInvariantTrapHandlerRegistrar{};

/// @brief Check the environment to determine whether verbose VM logging is enabled.
///
/// @details Reads the ZANNA_DEBUG_VM flag once and caches the result so subsequent
///          calls remain cheap. Uses a lazy-initialized static for thread-safe
///          initialization without runtime overhead.
///
/// @return @c true when debugging output should be emitted.
/// @note Inline candidate for better performance in hot paths.
inline bool isVmDebugLoggingEnabled() noexcept {
    /// @brief Read the process environment once to initialize VM debug logging.
    /// @return `true` when `ZANNA_DEBUG_VM` is present and nonempty.
    static const bool enabled = [] {
        if (const char *flag = std::getenv("ZANNA_DEBUG_VM"))
            return flag[0] != '\0';
        return false;
    }();
    return enabled;
}

/// @brief Translate a dispatch strategy into a printable label.
///
/// @details Provides stable string representations for each dispatch kind,
///          defaulting to "Unknown" for unrecognized values to maintain
///          graceful degradation in diagnostics.
///
/// @param kind Dispatch strategy currently active.
/// @return Human-readable dispatch name used in debug logs.
/// @note Constexpr for compile-time evaluation when possible.
constexpr const char *dispatchKindName(VM::DispatchKind kind) noexcept {
    switch (kind) {
        case VM::DispatchKind::FnTable:
            return "FnTable";
        case VM::DispatchKind::Switch:
            return "Switch";
        case VM::DispatchKind::Threaded:
            return "Threaded";
    }
    return "Unknown";
}

/// @brief Determine the storage required for a mutable global of an IL type.
/// @param kind IL type category to size.
/// @return Required byte count, or zero for unsupported/non-storable types.
size_t globalStorageSize(Type::Kind kind) {
    switch (kind) {
        case Type::Kind::I1:
            return sizeof(uint8_t);
        case Type::Kind::I16:
            return sizeof(int16_t);
        case Type::Kind::I32:
            return sizeof(int32_t);
        case Type::Kind::I64:
            return sizeof(int64_t);
        case Type::Kind::F64:
            return sizeof(double);
        case Type::Kind::Ptr:
        case Type::Kind::Error:
        case Type::Kind::ResumeTok:
            return sizeof(void *);
        case Type::Kind::Str:
            return sizeof(rt_string);
        case Type::Kind::Void:
            return 0;
    }
    return 0;
}

/// @brief Advance @p cursor past trailing ASCII whitespace.
/// @param cursor Pointer into a null-terminated initializer string.
/// @return Pointer to the first non-whitespace byte after @p cursor.
const char *skipTrailingSpace(const char *cursor) noexcept {
    while (cursor && *cursor && std::isspace(static_cast<unsigned char>(*cursor)))
        ++cursor;
    return cursor;
}

/// @brief Parse a signed integer global initializer with full validation.
/// @details Rejects empty input, partial parses, errno overflow, and values
///          outside the requested inclusive range. The VM stores initializers
///          into fixed-width global slots, so accepting truncation here would
///          make malformed IL observe implementation-defined state.
/// @param global Global whose initializer is being parsed, used for diagnostics.
/// @param minValue Smallest accepted value.
/// @param maxValue Largest accepted value.
/// @return Parsed signed value within the requested range.
int64_t parseCheckedIntegerInitializer(const Global &global, int64_t minValue, int64_t maxValue) {
    errno = 0;
    char *end = nullptr;
    const char *text = global.init.c_str();
    const long long parsed = std::strtoll(text, &end, 10);
    const char *rawEnd = end;
    end = const_cast<char *>(skipTrailingSpace(end));
    if (text == rawEnd || !end || *end != '\0' || errno == ERANGE || parsed < minValue ||
        parsed > maxValue) {
        throw std::runtime_error("VM initialization failed: invalid initializer for global '" +
                                 global.name + "'");
    }
    return static_cast<int64_t>(parsed);
}

/// @brief Parse a floating-point global initializer with full validation.
/// @details Requires the entire string to be consumed and rejects errno range
///          errors so invalid literals do not silently become zero, infinity,
///          or a host-specific approximation.
/// @param global Global whose initializer is being parsed.
/// @return Parsed double-precision initializer.
double parseCheckedFloatInitializer(const Global &global) {
    errno = 0;
    char *end = nullptr;
    const char *text = global.init.c_str();
    const double parsed = std::strtod(text, &end);
    const char *rawEnd = end;
    end = const_cast<char *>(skipTrailingSpace(end));
    if (text == rawEnd || !end || *end != '\0' || errno == ERANGE) {
        throw std::runtime_error("VM initialization failed: invalid initializer for global '" +
                                 global.name + "'");
    }
    return parsed;
}

/// @brief Decode a textual initializer into zeroed mutable-global storage.
/// @details Integer and floating-point values are range-checked before being
///          copied. Pointer globals initialize to null; an empty initializer
///          leaves the caller-provided zeroed storage unchanged.
/// @param global Global declaration supplying type and initializer text.
/// @param storage Writable allocation large enough for @p global's type.
/// @throws std::runtime_error If a numeric initializer is malformed or out of range.
void initializeGlobalStorage(const Global &global, void *storage) {
    if (global.init.empty())
        return;

    switch (global.type.kind) {
        case Type::Kind::I1: {
            const uint8_t value =
                static_cast<uint8_t>(parseCheckedIntegerInitializer(global, 0, 1));
            std::memcpy(storage, &value, sizeof(value));
            break;
        }
        case Type::Kind::I16: {
            const int16_t value = static_cast<int16_t>(parseCheckedIntegerInitializer(
                global,
                static_cast<int64_t>(std::numeric_limits<int16_t>::min()),
                static_cast<int64_t>(std::numeric_limits<int16_t>::max())));
            std::memcpy(storage, &value, sizeof(value));
            break;
        }
        case Type::Kind::I32: {
            const int32_t value = static_cast<int32_t>(parseCheckedIntegerInitializer(
                global,
                static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
                static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
            std::memcpy(storage, &value, sizeof(value));
            break;
        }
        case Type::Kind::I64: {
            const int64_t value = parseCheckedIntegerInitializer(
                global, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
            std::memcpy(storage, &value, sizeof(value));
            break;
        }
        case Type::Kind::F64: {
            const double value = parseCheckedFloatInitializer(global);
            std::memcpy(storage, &value, sizeof(value));
            break;
        }
        case Type::Kind::Ptr: {
            void *value = nullptr;
            std::memcpy(storage, &value, sizeof(value));
            break;
        }
        case Type::Kind::Void:
        case Type::Kind::Str:
        case Type::Kind::Error:
        case Type::Kind::ResumeTok:
            break;
    }
}

} // namespace

/// @brief Construct a VM instance bound to a specific IL module.
///
/// The constructor wires tracing/debug facilities, honours environment variables
/// controlling dispatch strategy selection, caches function/global lookups, and
/// records debugger configuration so future executions can evaluate breakpoints.
///
/// @param m   Module containing code and globals to execute. It must outlive the VM.
/// @param tc  Trace configuration used to initialise the @c TraceSink.
/// @param ms  Optional step limit; @c 0 disables the limit.
/// @param dbg Initial debugger control block describing active breakpoints.
/// @param script Optional scripted debugger interaction controller.
/// @param stackBytes Per-frame operand-stack capacity; zero selects the default.
VM::VM(const Module &m,
       TraceConfig tc,
       uint64_t ms,
       DebugCtrl dbg,
       DebugScript *script,
       std::size_t stackBytes)
    : mod(m), tracer(tc), debug(std::move(dbg)), script(script), maxSteps(ms),
      stackBytes_(stackBytes ? stackBytes : Frame::kDefaultStackSize) {
    debug.setSourceManager(tc.sm);
    init(nullptr);
}

/// @brief Construct a VM that shares an initialized program state.
/// @details Creates independent interpreter/debug state while reusing the
///          supplied runtime context and mutable module globals. The shared
///          state must have completed initialization and belong to @p m.
/// @param m Module containing code executed by this VM; must outlive the VM.
/// @param program Shared runtime context and global storage.
/// @param tc Trace configuration used to initialize the trace sink.
/// @param ms Instruction limit, or zero for unlimited execution.
/// @param dbg Initial breakpoint and watch configuration.
/// @param script Optional non-owning scripted debugger.
/// @param stackBytes Per-frame operand-stack capacity; zero selects the default.
VM::VM(const Module &m,
       std::shared_ptr<ProgramState> program,
       TraceConfig tc,
       uint64_t ms,
       DebugCtrl dbg,
       DebugScript *script,
       std::size_t stackBytes)
    : mod(m), tracer(tc), debug(std::move(dbg)), script(script), maxSteps(ms),
      stackBytes_(stackBytes ? stackBytes : Frame::kDefaultStackSize) {
    debug.setSourceManager(tc.sm);
    init(std::move(program));
}

/// @brief Populate VM configuration, runtime state, and module lookup caches.
/// @details Applies supported environment overrides, validates runtime
///          descriptors, attaches or creates ProgramState, initializes globals
///          and inline string handles, selects a dispatch kind, and reserves
///          hot-path containers before execution begins.
/// @param program Existing initialized state to share, or empty to create a new state.
/// @throws std::runtime_error If descriptors, shared-state ownership, global
///         types, initializers, or allocations are invalid.
void VM::init(std::shared_ptr<ProgramState> program) {
    ensureRuntimeDescriptorsValid();

    // Runtime overrides via environment -------------------------------------
    if (const char *envCounts = std::getenv("ZANNA_ENABLE_OPCOUNTS")) {
        std::string v{envCounts};
        /// @brief Fold one opcode-count option byte to lowercase.
        /// @param c Byte to normalize.
        /// @return Lowercase representation converted back to `char`.
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        // Accept 1/true/on to enable; 0/false/off to disable.
        if (v == "0" || v == "false" || v == "off")
            enableOpcodeCounts = false;
        else if (v == "1" || v == "true" || v == "on")
            enableOpcodeCounts = true;
        // Other values are ignored, preserving the build-time default.
    }
    if (const char *envEvery = std::getenv("ZANNA_INTERRUPT_EVERY_N")) {
        char *end = nullptr;
        unsigned long n = std::strtoul(envEvery, &end, 10);
        if (end && *end == '\0')
            pollEveryN_ = static_cast<uint32_t>(n);
    }

    const char *switchModeEnv = std::getenv("ZANNA_SWITCH_MODE");
    zanna::vm::SwitchMode mode = zanna::vm::SwitchMode::Auto;
    if (switchModeEnv != nullptr) {
        std::string rawMode{switchModeEnv};
        /// @brief Fold one switch-mode option byte to lowercase.
        /// @param ch Byte to normalize.
        /// @return Lowercase representation converted back to `char`.
        std::transform(rawMode.begin(), rawMode.end(), rawMode.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        if (rawMode == "dense")
            mode = zanna::vm::SwitchMode::Dense;
        else if (rawMode == "sorted")
            mode = zanna::vm::SwitchMode::Sorted;
        else if (rawMode == "hashed")
            mode = zanna::vm::SwitchMode::Hashed;
        else if (rawMode == "linear")
            mode = zanna::vm::SwitchMode::Linear;
        else
            mode = zanna::vm::SwitchMode::Auto;
    }
    zanna::vm::setSwitchMode(mode);

    DispatchKind defaultDispatch = DispatchKind::Switch;
#if ZANNA_THREADING_SUPPORTED
    defaultDispatch = DispatchKind::Threaded;
#endif
    DispatchKind selectedDispatch = defaultDispatch;

    if (const char *dispatchEnv = std::getenv("ZANNA_DISPATCH")) {
        std::string rawDispatch{dispatchEnv};
        /// @brief Fold one dispatch-mode option byte to lowercase.
        /// @param ch Byte to normalize.
        /// @return Lowercase representation converted back to `char`.
        std::transform(rawDispatch.begin(),
                       rawDispatch.end(),
                       rawDispatch.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        if (rawDispatch == "table")
            selectedDispatch = DispatchKind::FnTable;
        else if (rawDispatch == "switch")
            selectedDispatch = DispatchKind::Switch;
        else if (rawDispatch == "threaded") {
#if ZANNA_THREADING_SUPPORTED
            selectedDispatch = DispatchKind::Threaded;
#else
            selectedDispatch = DispatchKind::Switch;
#endif
        }
    }

    dispatchKind = selectedDispatch;

    if (isVmDebugLoggingEnabled()) {
        std::fprintf(stderr, "[DEBUG][VM] dispatch kind: %s\n", dispatchKindName(dispatchKind));
    }

    // Cache pointer to opcode handler table once.
    handlerTable_ = &VM::getOpcodeHandlers();

    if (program) {
        programState_ = std::move(program);
        // CONC-009: verify maps were fully populated before sharing across threads.
        if (!programState_->initComplete.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "VM initialization failed: shared ProgramState was not fully initialized");
        }
        if (!programState_->module) {
            programState_->module = &mod;
        } else if (programState_->module != &mod) {
            throw std::runtime_error(
                "VM initialization failed: shared ProgramState belongs to a different module");
        }
    } else {
        programState_ = std::make_shared<ProgramState>();
        programState_->module = &mod;
        programState_->rtContext = std::shared_ptr<RtContext>(new RtContext(), RtContextDeleter{});
        rt_context_init(programState_->rtContext.get());

        programState_->strMap.reserve(mod.globals.size());
        programState_->mutableGlobalMap.reserve(mod.globals.size());

        for (const auto &g : mod.globals) {
            if (g.type.kind == il::core::Type::Kind::Str) {
                programState_->strMap[g.name] =
                    ZannaStringHandle(toZannaString(g.init, AssumeNullTerminated::Yes));
                continue;
            }

            const size_t size = globalStorageSize(g.type.kind);
            if (size == 0)
                throw std::runtime_error("VM initialization failed: unsupported global type");
            std::unique_ptr<void, decltype(&std::free)> storage(std::calloc(1, size), &std::free);
            if (!storage) {
                throw std::runtime_error(
                    "VM initialization failed: failed to allocate mutable global storage");
            }
            initializeGlobalStorage(g, storage.get());
            auto [globalIt, inserted] =
                programState_->mutableGlobalMap.emplace(g.name, storage.get());
            (void)inserted;
            try {
                programState_->mutableGlobalSizes.emplace(storage.get(), size);
            } catch (...) {
                programState_->mutableGlobalMap.erase(globalIt);
                throw;
            }
            storage.release();
        }
        programState_->initComplete.store(true, std::memory_order_release);
    }

    // Reserve map capacities based on module sizes to reduce rehashing.
    fnMap.reserve(mod.functions.size());
    // Cache function pointers and constant strings for fast lookup during
    // execution and for resolving runtime bridge requests such as ConstStr.
    for (const auto &f : mod.functions)
        fnMap[f.name] = &f;

    // Pre-populate string literal cache to eliminate map lookups on hot path.
    // This scans all const_str operands in the module and creates rt_string
    // objects upfront, providing a 15-25% speedup for string-heavy programs.
    // First, estimate the number of string occurrences to reserve capacity.
    {
        size_t estimate = 0;
        for (const auto &f : mod.functions)
            for (const auto &block : f.blocks)
                for (const auto &instr : block.instructions) {
                    for (const auto &operand : instr.operands)
                        if (operand.kind == Value::Kind::ConstStr)
                            ++estimate;
                    for (const auto &args : instr.brArgs)
                        for (const auto &arg : args)
                            if (arg.kind == Value::Kind::ConstStr)
                                ++estimate;
                }
        if (estimate)
            inlineLiteralCache.reserve(estimate);
    }
    for (const auto &f : mod.functions) {
        for (const auto &block : f.blocks) {
            for (const auto &instr : block.instructions) {
                // Check all instruction operands for string constants
                for (const auto &operand : instr.operands) {
                    if (operand.kind == Value::Kind::ConstStr) {
                        if (inlineLiteralCache.find(operand.str) == inlineLiteralCache.end()) {
                            if (operand.str.find('\0') == std::string::npos)
                                inlineLiteralCache[operand.str] =
                                    ZannaStringHandle(rt_const_cstr(operand.str.c_str()));
                            else
                                inlineLiteralCache[operand.str] = ZannaStringHandle(
                                    rt_string_from_bytes(operand.str.data(), operand.str.size()));
                        }
                    }
                }

                // Check branch arguments for string constants
                for (const auto &brArgList : instr.brArgs) {
                    for (const auto &brArg : brArgList) {
                        if (brArg.kind == Value::Kind::ConstStr) {
                            if (inlineLiteralCache.find(brArg.str) == inlineLiteralCache.end()) {
                                if (brArg.str.find('\0') == std::string::npos)
                                    inlineLiteralCache[brArg.str] =
                                        ZannaStringHandle(rt_const_cstr(brArg.str.c_str()));
                                else
                                    inlineLiteralCache[brArg.str] = ZannaStringHandle(
                                        rt_string_from_bytes(brArg.str.data(), brArg.str.size()));
                            }
                        }
                    }
                }
            }
        }
    }

    // Build reverse map from BasicBlock -> Function for fast exception handler lookup.
    {
        size_t totalBlocks = 0;
        for (const auto &f : mod.functions)
            totalBlocks += f.blocks.size();
        blockToFunction.reserve(totalBlocks);
        for (const auto &f : mod.functions)
            for (const auto &block : f.blocks)
                blockToFunction[&block] = &f;
    }

    refreshDebugFlags();
    execStack.reserve(kExecStackInitialCapacity);
}

/// @brief Refresh all debug fast-path flags from current state.
/// @details Updates tracingActive_, memWatchActive_, and varWatchActive_
///          based on the current tracer and debug controller state.
///          Call this after changing trace config or adding/removing watches.
void VM::refreshDebugFlags() {
    tracingActive_ = tracer.isEnabled();
    memWatchActive_ = debug.hasMemWatches();
    varWatchActive_ = debug.hasVarWatches();
    debugBreakActive_ = debug.hasSrcLineBPs() || frontend_ != nullptr;
}

//===----------------------------------------------------------------------===//
// Buffer Pool Management
//===----------------------------------------------------------------------===//

/// @brief Acquire a stack buffer from the VM-local reuse pool.
/// @param size Required logical buffer size in bytes.
/// @return Resized buffer containing @p size bytes.
std::vector<uint8_t> VM::acquireStackBuffer(size_t size) {
    if (!stackBufferPool_.empty()) {
        auto buf = std::move(stackBufferPool_.back());
        stackBufferPool_.pop_back();
        buf.resize(size);
        return buf;
    }
    return std::vector<uint8_t>(size);
}

/// @brief Return a completed frame's stack buffer to the bounded reuse pool.
/// @param buf Buffer whose allocation may be retained for a later frame.
void VM::releaseStackBuffer(std::vector<uint8_t> &&buf) {
    if (stackBufferPool_.size() < kStackBufferPoolSize) {
        // Clear but keep capacity for reuse
        buf.clear();
        stackBufferPool_.push_back(std::move(buf));
    }
    // Otherwise let buffer be deallocated
}

/// @brief Acquire a register vector from the VM-local reuse pool.
/// @param size Required number of register slots.
/// @return Resized register file containing @p size slots.
std::vector<Slot> VM::acquireRegFile(size_t size) {
    if (!regFilePool_.empty()) {
        auto regs = std::move(regFilePool_.back());
        regFilePool_.pop_back();
        regs.resize(size);
        return regs;
    }
    return std::vector<Slot>(size);
}

/// @brief Return a completed frame's register vector to the bounded reuse pool.
/// @param regs Register file whose allocation may be retained for reuse.
void VM::releaseRegFile(std::vector<Slot> &&regs) {
    if (regFilePool_.size() < kRegisterFilePoolSize) {
        regs.clear();
        regFilePool_.push_back(std::move(regs));
    }
}

/// @brief Release retained strings and recycle a completed frame's large buffers.
/// @param fr Frame whose register and stack allocations are relinquished.
void VM::releaseFrameBuffers(Frame &fr) {
    // Release any owned string values before pooling the register file.
    // Each regIsStr[i] == 1 indicates regs[i].str was retained by storeResult.
    for (size_t i = 0; i < fr.regIsStr.size(); ++i) {
        if (fr.regIsStr[i])
            rt_str_release_maybe(fr.regs[i].str);
    }
    fr.regIsStr.clear();
    releaseStackBuffer(std::move(fr.stack));
    releaseRegFile(std::move(fr.regs));
}

//===----------------------------------------------------------------------===//
// Frame Setup
//===----------------------------------------------------------------------===//

/// @brief Obtain (or lazily build) the block label→BasicBlock* map for @p fn.
/// @details The map is built once per function and cached for the VM's lifetime.
///          IL is immutable after parsing so the cached map remains valid.
/// @param fn Function whose blocks should be mapped.
/// @return Const reference to the cached BlockMap.
const VM::BlockMap &VM::getOrBuildBlockMap(const Function &fn) {
    auto it = fnBlockMapCache_.find(&fn);
    if (it != fnBlockMapCache_.end())
        return it->second;
    auto &map = fnBlockMapCache_[&fn];
    map.reserve(fn.blocks.size());
    for (const auto &b : fn.blocks)
        map[b.label] = &b;
    return map;
}

/// @brief Initialise a fresh frame for executing @p fn.
///
/// Seeds parameter slots, verifies the argument count, and selects the entry
/// block so the interpreter loop can begin without additional bookkeeping.
/// The block label map is obtained from the VM-level cache (see
/// getOrBuildBlockMap) to avoid repeated hash-map construction.
///
/// @param fn     Function to execute.
/// @param args   Argument slots for the function's entry block.
/// @param bb     Set to the entry basic block of @p fn.
/// @return Fully initialised frame ready to run.
Frame VM::setupFrame(const Function &fn, std::span<const Slot> args, const BasicBlock *&bb) {
    Frame fr;
    fr.func = &fn;
    // Pre-size register file to the function's SSA value count. This mirrors
    // the number of temporaries and parameters required by @p fn and avoids
    // incremental growth during execution.
    if (isVmDebugLoggingEnabled()) {
        std::fprintf(stderr,
                     "[SETUP] fn=%s valueNames=%zu params=%zu blocks=%zu\n",
                     fn.name.c_str(),
                     fn.valueNames.size(),
                     fn.params.size(),
                     fn.blocks.size());
    }
    // Use shared helper to compute/cache register file size
    const size_t maxSsaId = detail::VMAccess::computeMaxSsaId(*this, fn);

    // Acquire register file from pool or allocate new.
    // Pool reuse avoids repeated allocations for recursive/repeated calls.
    const size_t regCount = maxSsaId + 1;
    fr.regs = acquireRegFile(regCount);

    if (isVmDebugLoggingEnabled()) {
        std::fprintf(stderr,
                     "[SETUP] maxSsaId=%zu regCount=%zu valueNames=%zu\n",
                     maxSsaId,
                     regCount,
                     fn.valueNames.size());
        std::fflush(stderr);
    }
    fr.regIsStr.assign(regCount, 0);
    fr.params.assign(fr.regs.size(), Slot{});
    fr.paramsSet.assign(fr.regs.size(), 0);
    // Acquire stack buffer from pool or allocate new.
    // Pool reuse avoids repeated 64KB allocations.
    fr.stack = acquireStackBuffer(stackBytes_);
    fr.sp = 0;
    fr.ehStack.clear();
    fr.activeError = {};
    fr.resumeState = {};
    // Block map is cached at VM level (getOrBuildBlockMap) — no per-call rebuild.
    bb = fn.blocks.empty() ? nullptr : &fn.blocks.front();
    if (bb) {
        const auto &params = bb->params;
        if (args.size() != params.size()) {
            RuntimeBridge::trap(
                TrapKind::InvalidOperation,
                diag::formatArgumentCountMismatch(fn.name, params.size(), args.size()),
                {},
                fn.name,
                bb->label);
            bb = nullptr;
            return fr;
        }
        for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
            const auto id = params[i].id;
            if (id >= fr.params.size()) {
                RuntimeBridge::trap(TrapKind::InvalidOperation,
                                    "entry parameter ID out of range",
                                    {},
                                    fn.name,
                                    bb->label);
                continue;
            }
            const bool isStringParam = params[i].type.kind == Type::Kind::Str;
            if (isStringParam) {
                if (fr.paramsSet[id])
                    rt_str_release_maybe(fr.params[id].str);

                Slot retained = args[i];
                rt_str_retain_maybe(retained.str);
                fr.params[id] = retained;
                fr.paramsSet[id] = 1;
                continue;
            }

            fr.params[id] = args[i];
            fr.paramsSet[id] = 1;
        }
    }
    return fr;
}

/// @brief Create an initial execution state for running @p fn.
///
/// Builds the frame, primes the tracer, clears debug bookkeeping, and resets the
/// interpreter's instruction pointer.
///
/// @param fn   Function to execute.
/// @param args Arguments passed to the function's entry block.
/// @return Fully initialised execution state ready for the interpreter loop.
VM::ExecState VM::prepareExecution(const Function &fn, std::span<const Slot> args) {
    ExecState st{};
    st.owner = this;
    st.blocks = &getOrBuildBlockMap(fn);
    st.fr = setupFrame(fn, args, st.bb);
    // Transfer block parameters immediately after frame setup.
    // This ensures parameters are copied to registers before the first instruction
    // executes, regardless of which dispatch path is taken. The transfer is
    // idempotent (pending values are consumed), so if processDebugControl also
    // runs at ip=0, it will see already-consumed params and skip redundant work.
    if (st.bb)
        transferBlockParams(st.fr, *st.bb);
    // Reserve branch-target cache buckets roughly based on number of
    // terminators to reduce rehashing during branching-heavy execution.
    size_t estTerms = 0;
    for (const auto &b : fn.blocks)
        for (const auto &in : b.instructions)
            if (!in.labels.empty())
                ++estTerms;
    if (estTerms)
        st.branchTargetCache.reserve(estTerms);
    // Inherit polling configuration from VM.
    st.config.interruptEveryN = pollEveryN_;
    // Install raw fn pointer trampoline; the actual std::function is stored in
    // pollCallback_ and invoked through pollCallbackTrampoline_ to avoid
    // std::function overhead (SBO + type-erasure cost) on every dispatch boundary.
    st.config.pollCallback = pollCallback_ ? &VM::pollCallbackTrampoline_ : nullptr;
#if ZANNA_VM_OPCOUNTS
    st.config.enableOpcodeCounts = enableOpcodeCounts;
#else
    st.config.enableOpcodeCounts = false;
#endif
    tracer.onFramePrepared(st.fr);
    debug.resetLastHit();
    st.ip = 0;
    st.skipBreakOnce = false;
    // Prime the pre-resolved operand cache for the entry block so the first
    // instruction dispatch can use the fast evalFast() path immediately.
    st.blockCache = getOrBuildBlockCache(&fn, st.bb);
    return st;
}

} // namespace il::vm
